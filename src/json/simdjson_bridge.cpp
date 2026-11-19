#include "stelog_json.h"
#include "base/simd.h"
#include "base/time_iso8601.h"

#include "simdjson.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

using namespace simdjson;

struct StelogJsonParser {
    ondemand::parser parser;
    std::string padded_buf;
    std::string text_accum;
    std::string think_accum;
    std::string tool_accum;
    std::string tmp_str;

    StelogJsonParser() {
        padded_buf.reserve(64 * 1024);
        text_accum.reserve(16 * 1024);
        think_accum.reserve(4 * 1024);
        tool_accum.reserve(2 * 1024);
        tmp_str.reserve(4 * 1024);
    }

    padded_string_view make_view(const char *line, size_t line_len, size_t padded_cap) {
        if (padded_cap >= line_len + SIMDJSON_PADDING) {
            return padded_string_view(line, line_len, padded_cap);
        }
        padded_buf.resize(line_len + SIMDJSON_PADDING);
        if (line_len > 0) {
            std::memcpy(padded_buf.data(), line, line_len);
        }
        std::memset(padded_buf.data() + line_len, ' ', SIMDJSON_PADDING);
        return padded_string_view(padded_buf.data(), line_len, line_len + SIMDJSON_PADDING);
    }
};

extern "C" StelogJsonParser *stelog_json_parser_create(void) {
    return new StelogJsonParser();
}

extern "C" void stelog_json_parser_destroy(StelogJsonParser *parser) {
    delete parser;
}

static inline StelogStr arena_copy_sv(StelogArena *arena, std::string_view sv) {
    if (sv.empty()) {
        StelogStr s = { "", 0 };
        return s;
    }
    char *dst = stelog_arena_strndup(arena, sv.data(), sv.size());
    StelogStr s = { dst ? dst : "", dst ? (uint32_t)sv.size() : 0u };
    return s;
}

static inline void append_with_newline(std::string &dst, std::string_view sv) {
    if (sv.empty()) return;
    if (!dst.empty() && dst.back() != '\n') {
        dst.push_back('\n');
    }
    dst.append(sv.data(), sv.size());
}

static inline std::string_view trim_ws(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
        sv.remove_suffix(1);
    }
    return sv;
}

static inline bool starts_with(std::string_view sv, std::string_view prefix) {
    return sv.size() >= prefix.size() && sv.substr(0, prefix.size()) == prefix;
}

/* Appends at most `cap` bytes of sv without splitting a UTF-8 sequence. */
static void append_utf8_capped(std::string &dst, std::string_view sv, size_t cap) {
    size_t n = sv.size() < cap ? sv.size() : cap;
    while (n > 0 && n < sv.size() && (static_cast<unsigned char>(sv[n]) & 0xC0u) == 0x80u) n--;
    dst.append(sv.data(), n);
}

static bool is_path_arg_key(std::string_view k) {
    return k == "Path" || k == "path" || k == "TargetFile" || k == "file_path" || k == "filePath" ||
           k == "FilePath" || k == "AbsolutePath" || k == "absolute_path" || k == "target_file" ||
           k == "notebook_path";
}

/* AGY double-encodes string args ("\"/abs/path\""); strip one layer of quotes. */
static std::string_view strip_wrapping_quotes(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

/*
 * Collects "*** Update/Add/Delete File: <path>" markers from an apply_patch body, whether newlines are
 * real or JSON/JS-escaped ("\n"), into a compact "*** Begin Patch\n*** Update File: a\n..." header.
 */
static void collect_patch_file_header(std::string_view in, std::string &header) {
    header.clear();
    static constexpr std::string_view kMarkers[] = { "*** Update File: ", "*** Add File: ", "*** Delete File: " };
    size_t files = 0;
    size_t pos = 0;
    while (files < 16) {
        size_t best = std::string_view::npos;
        size_t best_m = 0;
        for (size_t m = 0; m < 3; ++m) {
            size_t at = in.find(kMarkers[m], pos);
            if (at < best) { best = at; best_m = m; }
        }
        if (best == std::string_view::npos) break;
        size_t ps = best + kMarkers[best_m].size();
        size_t pe = ps;
        while (pe < in.size() && in[pe] != '\n' && in[pe] != '\r' && in[pe] != '\\' && in[pe] != '"') pe++;
        if (pe > ps) {
            if (header.empty()) header.assign("*** Begin Patch\n");
            header.append(kMarkers[best_m]);
            header.append(in.data() + ps, pe - ps);
            header.push_back('\n');
            files++;
        }
        pos = pe;
    }
}

/*
 * Appends tool arguments so that file-path keys come first and survive later truncation, the rest is
 * capped at `cap` bytes, and no cut splits a UTF-8 sequence. Non-object args are appended raw (capped).
 */
static void append_tool_args(std::string &dst, ondemand::value &v, size_t cap) {
    ondemand::json_type jt;
    if (v.type().get(jt) != SUCCESS) return;
    if (jt != ondemand::json_type::object) {
        std::string_view raw;
        if (v.raw_json().get(raw) == SUCCESS) append_utf8_capped(dst, trim_ws(raw), cap);
        return;
    }
    ondemand::object obj;
    if (v.get_object().get(obj) != SUCCESS) return;
    std::string paths;
    std::string rest;
    std::string patch_header;
    for (auto f : obj) {
        std::string_view key;
        if (f.unescaped_key().get(key) != SUCCESS) continue;
        std::string key_s(key);
        ondemand::value fv;
        if (f.value().get(fv) != SUCCESS) continue;
        ondemand::json_type ft;
        if (fv.type().get(ft) != SUCCESS) continue;
        if (ft == ondemand::json_type::string && is_path_arg_key(key_s)) {
            std::string_view s;
            if (fv.get_string().get(s) == SUCCESS) {
                s = strip_wrapping_quotes(trim_ws(s));
                if (!s.empty() && paths.size() < 1024) {
                    paths.append("\"").append(key_s).append("\":\"");
                    paths.append(s.data(), s.size());
                    paths.append("\", ");
                }
            }
            continue;
        }
        std::string_view raw;
        if (fv.raw_json().get(raw) != SUCCESS) continue;
        if (patch_header.empty() && ft == ondemand::json_type::string &&
            raw.find("*** Begin Patch") != std::string_view::npos) {
            collect_patch_file_header(raw, patch_header);
        }
        if (rest.size() >= cap) continue;
        rest.append("\"").append(key_s).append("\":");
        append_utf8_capped(rest, trim_ws(raw), cap - (rest.size() < cap ? rest.size() : cap));
        rest.append(", ");
    }
    if (rest.size() >= 2 && rest.compare(rest.size() - 2, 2, ", ") == 0) rest.resize(rest.size() - 2);
    if (paths.size() >= 2 && rest.empty()) paths.resize(paths.size() - 2);
    dst.append(patch_header);
    dst.push_back('{');
    dst.append(paths);
    append_utf8_capped(dst, rest, cap);
    dst.push_back('}');
}


/*
 * Detect Codex harness-injected "user" messages that are NOT actually typed by the human
 * (inspired by csift Law 1): AGENTS.md injections, environment_context, permissions,
 * collaboration_mode, turn_aborted, codex_internal_context / external_codex_apps payloads,
 * and code-mode `[N] tool exec result:` callbacks.
 */
static bool is_codex_injected_user_message(std::string_view sv) {
    sv = trim_ws(sv);
    if (sv.empty()) return true;
    if (starts_with(sv, "# AGENTS.md instructions for ") ||
        starts_with(sv, "<environment_context>") ||
        starts_with(sv, "<INSTRUCTIONS>") ||
        starts_with(sv, "<permissions instructions>") ||
        starts_with(sv, "<collaboration_mode>") ||
        starts_with(sv, "<turn_aborted>") ||
        starts_with(sv, "<subagent_notification>") ||
        starts_with(sv, "<codex_internal_context") ||
        starts_with(sv, "<external_codex_apps") ||
        starts_with(sv, "The following is a <SYSTEM_MESSAGE>")) {
        return true;
    }
    /* Code-mode exec callback injected into user role: `... [384] tool exec result:` */
    size_t head_cap = sv.size() < 256 ? sv.size() : 256;
    if (sv.substr(0, head_cap).find("] tool exec result:") != std::string_view::npos) {
        return true;
    }
    return false;
}

static inline std::string_view strip_agy_user_request_tags(std::string_view sv) {
    sv = trim_ws(sv);
    static constexpr std::string_view kOpen = "<USER_REQUEST>";
    static constexpr std::string_view kClose = "</USER_REQUEST>";
    size_t open_pos = sv.find(kOpen);
    if (open_pos != std::string_view::npos) {
        size_t body_start = open_pos + kOpen.size();
        size_t close_pos = sv.find(kClose, body_start);
        if (close_pos != std::string_view::npos) {
            return trim_ws(sv.substr(body_start, close_pos - body_start));
        }
        return trim_ws(sv.substr(body_start));
    }
    return sv;
}

static void emit_event(StelogLineResult *out,
                       StelogAgentKind agent,
                       StelogRoleKind role,
                       uint8_t flags,
                       int64_t ts_ms,
                       uint32_t file_id,
                       uint64_t raw_offset,
                       uint32_t raw_len,
                       const StelogStreamContext *ctx,
                       StelogStr actor_override,
                       StelogStr msg_id,
                       StelogStr parent_id,
                       StelogStr text) {
    if (out->count >= STELOG_MAX_LINE_EVENTS) return;
    StelogEvent *ev = &out->events[out->count++];
    std::memset(ev, 0, sizeof(*ev));
    ev->ts_ms = ts_ms;
    ev->raw_offset = raw_offset;
    ev->raw_len = raw_len;
    ev->file_id = file_id;
    ev->tag = stelog_tag_pack(agent, role, (uint8_t)(flags | ctx->session_flags));
    ev->session_id = ctx->session_id;
    ev->project = ctx->cwd;
    ev->title = ctx->title;
    ev->actor = actor_override.len > 0 ? actor_override : ctx->actor;
    ev->msg_id = msg_id;
    ev->parent_id = parent_id.len > 0 ? parent_id : ctx->parent_session_id;
    ev->text = text;
}

/* ============================================================================
 * 1. Codex CLI Line Parser
 * ============================================================================ */

static void parse_codex_content_array(ondemand::value &val, std::string &accum, std::string &tmp) {
    ondemand::array arr;
    if (val.get_array().get(arr) != SUCCESS) {
        /* Fallback: plain string content */
        if (val.get_string(tmp) == SUCCESS) {
            append_with_newline(accum, tmp);
        }
        return;
    }

    for (auto elem : arr) {
        ondemand::object item;
        if (elem.get_object().get(item) != SUCCESS) continue;
        std::string item_type;
        std::string item_text;
        for (auto f : item) {
            ondemand::raw_json_string k;
            if (f.key().get(k) != SUCCESS) continue;
            if (k == "type") {
                (void)f.value().get_string(item_type);
            } else if (k == "text") {
                (void)f.value().get_string(item_text);
            }
        }
        /* Skip encrypted_content / input_image / audio blobs; keep textual blocks */
        if (!item_text.empty() &&
            (item_type.empty() || item_type == "input_text" || item_type == "output_text" ||
             item_type == "text" || item_type == "summary_text")) {
            append_with_newline(accum, item_text);
        }
    }
}

extern "C" int stelog_json_parse_codex_line(StelogJsonParser *p,
                                            const char *line,
                                            size_t line_len,
                                            size_t padded_cap,
                                            uint32_t file_id,
                                            uint64_t raw_offset,
                                            const StelogParseOptions *opts,
                                            StelogStreamContext *ctx,
                                            StelogArena *arena,
                                            StelogLineResult *out) {
    std::memset(out, 0, sizeof(*out));
    p->text_accum.clear();
    p->tool_accum.clear();

    ondemand::document doc;
    if (p->parser.iterate(p->make_view(line, line_len, padded_cap)).get(doc) != SUCCESS) {
        return -1;
    }

    ondemand::object root;
    if (doc.get_object().get(root) != SUCCESS) {
        return -1;
    }

    int64_t ts_ms = 0;
    std::string top_type;
    bool handled_payload = false;

    for (auto field : root) {
        ondemand::raw_json_string key;
        if (field.key().get(key) != SUCCESS) continue;

        if (key == "timestamp") {
            std::string_view sv;
            if (field.value().get_string().get(sv) == SUCCESS) {
                ts_ms = stelog_parse_iso8601_ms(sv.data(), sv.size());
            }
        } else if (key == "type") {
            (void)field.value().get_string(top_type);
        } else if (key == "payload") {
            handled_payload = true;
            ondemand::object payload;
            if (field.value().get_object().get(payload) != SUCCESS) continue;

            if (top_type == "session_meta") {
                std::string sid, cwd, parent_tid, role_str, nick;
                for (auto pf : payload) {
                    ondemand::raw_json_string pk;
                    if (pf.key().get(pk) != SUCCESS) continue;
                    if (pk == "id") {
                        (void)pf.value().get_string(sid);
                    } else if (pk == "cwd") {
                        (void)pf.value().get_string(cwd);
                    } else if (pk == "parent_thread_id") {
                        (void)pf.value().get_string(parent_tid);
                    } else if (pk == "agent_nickname") {
                        (void)pf.value().get_string(nick);
                    } else if (pk == "agent_role") {
                        (void)pf.value().get_string(role_str);
                    }
                }
                if (!sid.empty()) ctx->session_id = arena_copy_sv(arena, sid);
                if (!cwd.empty()) ctx->cwd = arena_copy_sv(arena, cwd);
                if (!parent_tid.empty()) {
                    ctx->parent_session_id = arena_copy_sv(arena, parent_tid);
                    ctx->session_flags |= STELOG_FLAG_IS_SUBAGENT;
                }
                if (!nick.empty()) {
                    std::string actor = "codex:" + nick;
                    if (!role_str.empty()) {
                        actor.push_back('(');
                        actor.append(role_str);
                        actor.push_back(')');
                    }
                    ctx->actor = arena_copy_sv(arena, actor);
                }
                out->updated_context = true;
            } else if (top_type == "turn_context") {
                std::string cwd, model;
                for (auto pf : payload) {
                    ondemand::raw_json_string pk;
                    if (pf.key().get(pk) != SUCCESS) continue;
                    if (pk == "cwd") {
                        (void)pf.value().get_string(cwd);
                    } else if (pk == "model") {
                        (void)pf.value().get_string(model);
                    }
                }
                if (!cwd.empty()) {
                    ctx->cwd = arena_copy_sv(arena, cwd);
                    out->updated_context = true;
                }
                if (!model.empty() && ctx->actor.len == 0) {
                    std::string actor = "codex:" + model;
                    ctx->actor = arena_copy_sv(arena, actor);
                    out->updated_context = true;
                }
            } else if (top_type == "response_item") {
                std::string ptype, role, id, name, author, recipient;
                std::string call_input, call_output;
                for (auto pf : payload) {
                    ondemand::raw_json_string pk;
                    if (pf.key().get(pk) != SUCCESS) continue;
                    if (pk == "type") {
                        (void)pf.value().get_string(ptype);
                    } else if (pk == "role") {
                        (void)pf.value().get_string(role);
                    } else if (pk == "id") {
                        (void)pf.value().get_string(id);
                    } else if (pk == "name") {
                        (void)pf.value().get_string(name);
                    } else if (pk == "author") {
                        (void)pf.value().get_string(author);
                    } else if (pk == "recipient") {
                        (void)pf.value().get_string(recipient);
                    } else if (pk == "content") {
                        auto v = pf.value();
                        ondemand::value val;
                        if (v.get(val) == SUCCESS) {
                            parse_codex_content_array(val, p->text_accum, p->tmp_str);
                        }
                    } else if (pk == "summary") {
                        if (opts->include_thinking) {
                            auto v = pf.value();
                            ondemand::value val;
                            if (v.get(val) == SUCCESS) {
                                parse_codex_content_array(val, p->text_accum, p->tmp_str);
                            }
                        }
                    } else if (pk == "input" || pk == "arguments") {
                        (void)pf.value().get_string(call_input);
                    } else if (pk == "output") {
                        if (opts->include_tool_results) {
                            (void)pf.value().get_string(call_output);
                        }
                    }
                }

                StelogStr msg_id = arena_copy_sv(arena, id);
                if (ptype == "message") {
                    if (role == "developer" || role == "system") {
                        if (!opts->include_system || p->text_accum.empty()) continue;
                        StelogStr actor = { "system", 6 };
                        emit_event(out, STELOG_AGENT_CODEX, STELOG_ROLE_SYSTEM, 0, ts_ms,
                                   file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                                   { "", 0 }, arena_copy_sv(arena, p->text_accum));
                    } else if (role == "user") {
                        if (p->text_accum.empty()) continue;
                        if (is_codex_injected_user_message(p->text_accum)) {
                            if (!opts->include_system) continue;
                            StelogStr actor = { "system", 6 };
                            emit_event(out, STELOG_AGENT_CODEX, STELOG_ROLE_SYSTEM, 0, ts_ms,
                                       file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                                       { "", 0 }, arena_copy_sv(arena, p->text_accum));
                        } else {
                            StelogStr actor = { "user", 4 };
                            emit_event(out, STELOG_AGENT_CODEX, STELOG_ROLE_USER, 0, ts_ms,
                                       file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                                       { "", 0 }, arena_copy_sv(arena, p->text_accum));
                        }
                    } else if (role == "assistant") {
                        if (p->text_accum.empty()) continue;
                        StelogStr actor = ctx->actor.len ? ctx->actor : StelogStr{ "codex:main", 10 };
                        emit_event(out, STELOG_AGENT_CODEX, STELOG_ROLE_ASSISTANT, 0, ts_ms,
                                   file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                                   { "", 0 }, arena_copy_sv(arena, p->text_accum));
                    }
                } else if (ptype == "agent_message") {
                    if (p->text_accum.empty()) continue;
                    std::string actor_str = "codex:";
                    actor_str.append(author.empty() ? "agent" : author);
                    if (!recipient.empty()) {
                        actor_str.append("->");
                        actor_str.append(recipient);
                    }
                    emit_event(out, STELOG_AGENT_CODEX, STELOG_ROLE_INTER_AGENT, 0, ts_ms,
                               file_id, raw_offset, (uint32_t)line_len, ctx,
                               arena_copy_sv(arena, actor_str), msg_id,
                               { "", 0 }, arena_copy_sv(arena, p->text_accum));
                } else if (ptype == "custom_tool_call" || ptype == "function_call") {
                    uint32_t max_b = opts->tool_call_max_bytes ? opts->tool_call_max_bytes : 512u;
                    p->tool_accum.assign(name.empty() ? "tool" : name);
                    p->tool_accum.append(": ");
                    size_t head_len = p->tool_accum.size();
                    /* Patches wrapped in exec/shell calls name their files deep inside the input; hoist the
                       file markers so file history still sees them after truncation. */
                    collect_patch_file_header(call_input, p->tmp_str);
                    if (!p->tmp_str.empty() && !(name == "apply_patch" && call_input.size() + head_len <= max_b)) {
                        p->tool_accum.append(p->tmp_str);
                    }
                    size_t prefix_len = p->tool_accum.size();
                    bool trunc = false;
                    StelogStr txt;
                    if (prefix_len + call_input.size() <= max_b) {
                        p->tool_accum.append(call_input);
                        txt = arena_copy_sv(arena, p->tool_accum);
                    } else {
                        size_t body_cap = (max_b > prefix_len + 128u) ? (max_b - prefix_len) : 128u;
                        StelogStr body = stelog_truncate_middle(arena, call_input.data(), call_input.size(), (uint32_t)body_cap, &trunc);
                        p->tool_accum.append(body.ptr, body.len);
                        txt = arena_copy_sv(arena, p->tool_accum);
                    }
                    if (call_input.empty() && txt.len >= 2 && txt.ptr[txt.len - 2] == ':') txt.len -= 2;
                    StelogStr actor = ctx->actor.len ? ctx->actor : StelogStr{ "codex:main", 10 };
                    emit_event(out, STELOG_AGENT_CODEX, STELOG_ROLE_TOOL_CALL,
                               trunc ? STELOG_FLAG_TRUNCATED : 0, ts_ms,
                               file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                               { "", 0 }, txt);
                } else if (ptype == "reasoning" && opts->include_thinking && !p->text_accum.empty()) {
                    StelogStr actor = ctx->actor.len ? ctx->actor : StelogStr{ "codex:main", 10 };
                    emit_event(out, STELOG_AGENT_CODEX, STELOG_ROLE_THINKING, 0, ts_ms,
                               file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                               { "", 0 }, arena_copy_sv(arena, p->text_accum));
                } else if ((ptype == "custom_tool_call_output" || ptype == "function_call_output") &&
                           opts->include_tool_results && !call_output.empty()) {
                    bool trunc = false;
                    StelogStr txt = stelog_truncate_middle(arena, call_output.data(), call_output.size(), 4096u, &trunc);
                    if (!trunc) txt = arena_copy_sv(arena, call_output);
                    StelogStr actor = { "tool", 4 };
                    emit_event(out, STELOG_AGENT_CODEX, STELOG_ROLE_TOOL_RESULT,
                               trunc ? STELOG_FLAG_TRUNCATED : 0, ts_ms,
                               file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                               { "", 0 }, txt);
                }
            } else if (top_type == "inter_agent_communication") {
                std::string author, recipient, content;
                for (auto pf : payload) {
                    ondemand::raw_json_string pk;
                    if (pf.key().get(pk) != SUCCESS) continue;
                    if (pk == "author") (void)pf.value().get_string(author);
                    else if (pk == "recipient") (void)pf.value().get_string(recipient);
                    else if (pk == "content") (void)pf.value().get_string(content);
                }
                if (!content.empty()) {
                    std::string actor_str = "codex:";
                    actor_str.append(author.empty() ? "agent" : author);
                    if (!recipient.empty()) {
                        actor_str.append("->");
                        actor_str.append(recipient);
                    }
                    emit_event(out, STELOG_AGENT_CODEX, STELOG_ROLE_INTER_AGENT, 0, ts_ms,
                               file_id, raw_offset, (uint32_t)line_len, ctx,
                               arena_copy_sv(arena, actor_str), { "", 0 }, { "", 0 },
                               arena_copy_sv(arena, content));
                }
            }
        }
    }

    (void)handled_payload;
    return 0;
}

/* ============================================================================
 * 2. AGY CLI Line Parser
 * ============================================================================ */

extern "C" int stelog_json_parse_agy_line(StelogJsonParser *p,
                                          const char *line,
                                          size_t line_len,
                                          size_t padded_cap,
                                          uint32_t file_id,
                                          uint64_t raw_offset,
                                          const StelogParseOptions *opts,
                                          StelogStreamContext *ctx,
                                          StelogArena *arena,
                                          StelogLineResult *out) {
    std::memset(out, 0, sizeof(*out));
    p->text_accum.clear();
    p->think_accum.clear();
    p->tool_accum.clear();

    ondemand::document doc;
    if (p->parser.iterate(p->make_view(line, line_len, padded_cap)).get(doc) != SUCCESS) {
        return -1;
    }

    ondemand::object root;
    if (doc.get_object().get(root) != SUCCESS) {
        return -1;
    }

    int64_t ts_ms = 0;
    int64_t step_idx = -1;
    std::string source, type;
    bool has_truncated = false;

    for (auto field : root) {
        ondemand::raw_json_string key;
        if (field.key().get(key) != SUCCESS) continue;

        if (key == "step_index") {
            (void)field.value().get_int64().get(step_idx);
        } else if (key == "source") {
            (void)field.value().get_string(source);
        } else if (key == "type") {
            (void)field.value().get_string(type);
        } else if (key == "created_at") {
            std::string_view sv;
            if (field.value().get_string().get(sv) == SUCCESS) {
                ts_ms = stelog_parse_iso8601_ms(sv.data(), sv.size());
            }
        } else if (key == "content") {
            (void)field.value().get_string(p->text_accum);
        } else if (key == "thinking") {
            if (opts->include_thinking) {
                (void)field.value().get_string(p->think_accum);
            }
        } else if (key == "truncated_fields") {
            has_truncated = true;
        } else if (key == "tool_calls") {
            ondemand::array arr;
            if (field.value().get_array().get(arr) == SUCCESS) {
                for (auto elem : arr) {
                    ondemand::object tc;
                    if (elem.get_object().get(tc) != SUCCESS) continue;
                    std::string tname;
                    std::string args_s;
                    for (auto tf : tc) {
                        ondemand::raw_json_string tk;
                        if (tf.key().get(tk) != SUCCESS) continue;
                        if (tk == "name") {
                            (void)tf.value().get_string(tname);
                        } else if (tk == "arguments" || tk == "args") {
                            ondemand::value av;
                            if (tf.value().get(av) == SUCCESS) append_tool_args(args_s, av, 256);
                        }
                    }
                    if (!tname.empty()) {
                        if (!p->tool_accum.empty()) p->tool_accum.append("\n");
                        p->tool_accum.append(tname);
                        if (!args_s.empty()) {
                            p->tool_accum.append(": ");
                            p->tool_accum.append(args_s);
                        }
                    }
                }
            }
        }
    }

    char step_buf[32];
    StelogStr msg_id = { "", 0 };
    if (step_idx >= 0) {
        int n = std::snprintf(step_buf, sizeof(step_buf), "step_%lld", (long long)step_idx);
        if (n > 0) msg_id = arena_copy_sv(arena, std::string_view(step_buf, (size_t)n));
    }

    uint8_t base_flags = has_truncated ? STELOG_FLAG_TRUNCATED : 0;

    if (source == "USER_EXPLICIT" && type == "USER_INPUT") {
        std::string_view clean = strip_agy_user_request_tags(p->text_accum);
        if (!clean.empty()) {
            StelogStr actor = { "user", 4 };
            emit_event(out, STELOG_AGENT_AGY, STELOG_ROLE_USER, base_flags, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                       { "", 0 }, arena_copy_sv(arena, clean));
        }
    } else if (source == "MODEL" && type == "PLANNER_RESPONSE") {
        StelogStr actor = ctx->actor.len ? ctx->actor : StelogStr{ "agy:main", 8 };
        if (!p->text_accum.empty()) {
            emit_event(out, STELOG_AGENT_AGY, STELOG_ROLE_ASSISTANT, base_flags, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                       { "", 0 }, arena_copy_sv(arena, p->text_accum));
        }
        if (!p->tool_accum.empty()) {
            bool trunc = false;
            uint32_t max_b = opts->tool_call_max_bytes ? opts->tool_call_max_bytes : 512u;
            StelogStr txt = stelog_truncate_middle(arena, p->tool_accum.data(), p->tool_accum.size(), max_b, &trunc);
            if (!trunc) txt = arena_copy_sv(arena, p->tool_accum);
            emit_event(out, STELOG_AGENT_AGY, STELOG_ROLE_TOOL_CALL,
                       (uint8_t)(base_flags | (trunc ? STELOG_FLAG_TRUNCATED : 0)), ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                       { "", 0 }, txt);
        }
        if (opts->include_thinking && !p->think_accum.empty()) {
            emit_event(out, STELOG_AGENT_AGY, STELOG_ROLE_THINKING, base_flags, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                       { "", 0 }, arena_copy_sv(arena, p->think_accum));
        }
    } else if (source == "MODEL" && type == "GENERIC") {
        if (opts->include_tool_results && !p->text_accum.empty()) {
            bool trunc = false;
            StelogStr txt = stelog_truncate_middle(arena, p->text_accum.data(), p->text_accum.size(), 4096u, &trunc);
            if (!trunc) txt = arena_copy_sv(arena, p->text_accum);
            StelogStr actor = { "tool", 4 };
            emit_event(out, STELOG_AGENT_AGY, STELOG_ROLE_TOOL_RESULT,
                       (uint8_t)(base_flags | (trunc ? STELOG_FLAG_TRUNCATED : 0)), ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                       { "", 0 }, txt);
        }
    } else if (source == "SYSTEM") {
        if (opts->include_system && !p->text_accum.empty()) {
            StelogStr actor = { "system", 6 };
            emit_event(out, STELOG_AGENT_AGY, STELOG_ROLE_SYSTEM, base_flags, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id,
                       { "", 0 }, arena_copy_sv(arena, p->text_accum));
        }
    }

    return 0;
}

/* ============================================================================
 * 3. Pi Coding Agent Line Parser
 * ============================================================================ */

extern "C" int stelog_json_parse_pi_line(StelogJsonParser *p,
                                         const char *line,
                                         size_t line_len,
                                         size_t padded_cap,
                                         uint32_t file_id,
                                         uint64_t raw_offset,
                                         const StelogParseOptions *opts,
                                         StelogStreamContext *ctx,
                                         StelogArena *arena,
                                         StelogLineResult *out) {
    std::memset(out, 0, sizeof(*out));
    p->text_accum.clear();
    p->think_accum.clear();
    p->tool_accum.clear();

    ondemand::document doc;
    if (p->parser.iterate(p->make_view(line, line_len, padded_cap)).get(doc) != SUCCESS) {
        return -1;
    }

    ondemand::object root;
    if (doc.get_object().get(root) != SUCCESS) {
        return -1;
    }

    int64_t ts_ms = 0;
    std::string top_type, id, parent_id, cwd, provider, model_id;
    std::string msg_role, tool_name;

    for (auto field : root) {
        ondemand::raw_json_string key;
        if (field.key().get(key) != SUCCESS) continue;

        if (key == "type") {
            (void)field.value().get_string(top_type);
        } else if (key == "id") {
            (void)field.value().get_string(id);
        } else if (key == "parentId") {
            (void)field.value().get_string(parent_id);
        } else if (key == "timestamp") {
            std::string_view sv;
            if (field.value().get_string().get(sv) == SUCCESS) {
                ts_ms = stelog_parse_iso8601_ms(sv.data(), sv.size());
            }
        } else if (key == "cwd") {
            (void)field.value().get_string(cwd);
        } else if (key == "provider") {
            (void)field.value().get_string(provider);
        } else if (key == "modelId") {
            (void)field.value().get_string(model_id);
        } else if (key == "message") {
            ondemand::object mobj;
            if (field.value().get_object().get(mobj) != SUCCESS) continue;
            for (auto mf : mobj) {
                ondemand::raw_json_string mk;
                if (mf.key().get(mk) != SUCCESS) continue;
                if (mk == "role") {
                    (void)mf.value().get_string(msg_role);
                } else if (mk == "toolName") {
                    (void)mf.value().get_string(tool_name);
                } else if (mk == "timestamp" && ts_ms == 0) {
                    int64_t m_ts = 0;
                    if (mf.value().get_int64().get(m_ts) == SUCCESS && m_ts > 0) {
                        ts_ms = m_ts;
                    }
                } else if (mk == "content") {
                    ondemand::array carr;
                    if (mf.value().get_array().get(carr) == SUCCESS) {
                        for (auto ce : carr) {
                            ondemand::object co;
                            if (ce.get_object().get(co) != SUCCESS) continue;
                            std::string ctype, ctext, cthink, cname;
                            std::string cargs;
                            for (auto cf : co) {
                                ondemand::raw_json_string ck;
                                if (cf.key().get(ck) != SUCCESS) continue;
                                if (ck == "type") (void)cf.value().get_string(ctype);
                                else if (ck == "text") (void)cf.value().get_string(ctext);
                                else if (ck == "thinking") {
                                    if (opts->include_thinking) (void)cf.value().get_string(cthink);
                                } else if (ck == "name") (void)cf.value().get_string(cname);
                                else if (ck == "arguments") {
                                    ondemand::value av;
                                    if (cf.value().get(av) == SUCCESS) append_tool_args(cargs, av, 256);
                                }
                            }
                            if (ctype == "text" && !ctext.empty()) {
                                append_with_newline(p->text_accum, ctext);
                            } else if (ctype == "thinking" && opts->include_thinking && !cthink.empty()) {
                                append_with_newline(p->think_accum, cthink);
                            } else if (ctype == "toolCall" && !cname.empty()) {
                                if (!p->tool_accum.empty()) p->tool_accum.push_back('\n');
                                p->tool_accum.append(cname);
                                if (!cargs.empty()) {
                                    p->tool_accum.append(": ");
                                    p->tool_accum.append(cargs);
                                }
                            }
                        }
                    } else if (mf.value().get_string(p->tmp_str) == SUCCESS) {
                        append_with_newline(p->text_accum, p->tmp_str);
                    }
                }
            }
        }
    }

    if (top_type == "session") {
        if (!id.empty()) ctx->session_id = arena_copy_sv(arena, id);
        if (!cwd.empty()) ctx->cwd = arena_copy_sv(arena, cwd);
        out->updated_context = true;
        return 0;
    }

    if (top_type == "model_change") {
        if (!model_id.empty()) {
            std::string actor = "pi:";
            if (!provider.empty()) {
                actor.append(provider);
                actor.push_back('/');
            }
            actor.append(model_id);
            ctx->actor = arena_copy_sv(arena, actor);
            out->updated_context = true;
        }
        return 0;
    }

    if (top_type != "message") return 0;

    StelogStr msg_id_s = arena_copy_sv(arena, id);
    StelogStr parent_id_s = arena_copy_sv(arena, parent_id);

    if (msg_role == "user") {
        if (!p->text_accum.empty()) {
            StelogStr actor = { "user", 4 };
            emit_event(out, STELOG_AGENT_PI, STELOG_ROLE_USER, 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor,
                       msg_id_s, parent_id_s, arena_copy_sv(arena, p->text_accum));
        }
    } else if (msg_role == "assistant") {
        StelogStr actor = ctx->actor.len ? ctx->actor : StelogStr{ "pi:main", 7 };
        if (!p->text_accum.empty()) {
            emit_event(out, STELOG_AGENT_PI, STELOG_ROLE_ASSISTANT, 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor,
                       msg_id_s, parent_id_s, arena_copy_sv(arena, p->text_accum));
        }
        if (!p->tool_accum.empty()) {
            bool trunc = false;
            uint32_t max_b = opts->tool_call_max_bytes ? opts->tool_call_max_bytes : 512u;
            StelogStr txt = stelog_truncate_middle(arena, p->tool_accum.data(), p->tool_accum.size(), max_b, &trunc);
            if (!trunc) txt = arena_copy_sv(arena, p->tool_accum);
            emit_event(out, STELOG_AGENT_PI, STELOG_ROLE_TOOL_CALL,
                       trunc ? STELOG_FLAG_TRUNCATED : 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor,
                       msg_id_s, parent_id_s, txt);
        }
        if (opts->include_thinking && !p->think_accum.empty()) {
            emit_event(out, STELOG_AGENT_PI, STELOG_ROLE_THINKING, 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor,
                       msg_id_s, parent_id_s, arena_copy_sv(arena, p->think_accum));
        }
    } else if (msg_role == "toolResult" || msg_role == "bashExecution") {
        if (opts->include_tool_results && !p->text_accum.empty()) {
            bool trunc = false;
            StelogStr txt = stelog_truncate_middle(arena, p->text_accum.data(), p->text_accum.size(), 4096u, &trunc);
            if (!trunc) txt = arena_copy_sv(arena, p->text_accum);
            std::string actor_s = tool_name.empty() ? "tool" : ("tool:" + tool_name);
            emit_event(out, STELOG_AGENT_PI, STELOG_ROLE_TOOL_RESULT,
                       trunc ? STELOG_FLAG_TRUNCATED : 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx,
                       arena_copy_sv(arena, actor_s), msg_id_s, parent_id_s, txt);
        }
    } else if (msg_role == "system" && opts->include_system && !p->text_accum.empty()) {
        StelogStr actor = { "system", 6 };
        emit_event(out, STELOG_AGENT_PI, STELOG_ROLE_SYSTEM, 0, ts_ms,
                   file_id, raw_offset, (uint32_t)line_len, ctx, actor,
                   msg_id_s, parent_id_s, arena_copy_sv(arena, p->text_accum));
    }

    return 0;
}

/* ============================================================================
 * 4. Factory Droid Session Parser (~/.factory/sessions/<cwd-slug>/<session>.jsonl)
 * ============================================================================ */

static inline bool is_droid_reminder(std::string_view sv) {
    return starts_with(trim_ws(sv), "<system-reminder>");
}

static void append_droid_tool_result(std::string &dst, ondemand::value &v) {
    ondemand::json_type jt;
    if (v.type().get(jt) != SUCCESS) return;
    if (jt == ondemand::json_type::string) {
        std::string_view s;
        if (v.get_string().get(s) == SUCCESS) append_with_newline(dst, s);
        return;
    }
    if (jt != ondemand::json_type::array) return;
    ondemand::array arr;
    if (v.get_array().get(arr) != SUCCESS) return;
    for (auto el : arr) {
        ondemand::object o;
        if (el.get_object().get(o) != SUCCESS) continue;
        for (auto of : o) {
            ondemand::raw_json_string ok;
            if (of.key().get(ok) != SUCCESS) continue;
            if (ok == "text") {
                std::string_view s;
                if (of.value().get_string().get(s) == SUCCESS) append_with_newline(dst, s);
            }
        }
    }
}

extern "C" int stelog_json_parse_droid_line(StelogJsonParser *p,
                                            const char *line,
                                            size_t line_len,
                                            size_t padded_cap,
                                            uint32_t file_id,
                                            uint64_t raw_offset,
                                            const StelogParseOptions *opts,
                                            StelogStreamContext *ctx,
                                            StelogArena *arena,
                                            StelogLineResult *out) {
    std::memset(out, 0, sizeof(*out));
    p->text_accum.clear();
    p->think_accum.clear();
    p->tool_accum.clear();

    ondemand::document doc;
    if (p->parser.iterate(p->make_view(line, line_len, padded_cap)).get(doc) != SUCCESS) {
        return -1;
    }
    ondemand::object root;
    if (doc.get_object().get(root) != SUCCESS) {
        return -1;
    }

    int64_t ts_ms = 0;
    std::string top_type, id, parent_id, title, cwd, calling_sid;
    std::string role, visibility, model_id;
    std::string sys_text, tool_res;
    bool system_notice = false;

    for (auto field : root) {
        ondemand::raw_json_string key;
        if (field.key().get(key) != SUCCESS) continue;
        if (key == "type") {
            (void)field.value().get_string(top_type);
        } else if (key == "id") {
            (void)field.value().get_string(id);
        } else if (key == "parentId") {
            (void)field.value().get_string(parent_id);
        } else if (key == "timestamp") {
            std::string_view sv;
            if (field.value().get_string().get(sv) == SUCCESS) {
                ts_ms = stelog_parse_iso8601_ms(sv.data(), sv.size());
            }
        } else if (key == "title") {
            (void)field.value().get_string(title);
        } else if (key == "cwd") {
            (void)field.value().get_string(cwd);
        } else if (key == "callingSessionId") {
            (void)field.value().get_string(calling_sid);
        } else if (key == "message") {
            ondemand::object mobj;
            if (field.value().get_object().get(mobj) != SUCCESS) continue;
            for (auto mf : mobj) {
                ondemand::raw_json_string mk;
                if (mf.key().get(mk) != SUCCESS) continue;
                if (mk == "role") {
                    (void)mf.value().get_string(role);
                } else if (mk == "visibility") {
                    (void)mf.value().get_string(visibility);
                } else if (mk == "systemNotice") {
                    (void)mf.value().get_bool().get(system_notice);
                } else if (mk == "modelId") {
                    (void)mf.value().get_string(model_id);
                } else if (mk == "content") {
                    ondemand::array carr;
                    if (mf.value().get_array().get(carr) != SUCCESS) {
                        if (mf.value().get_string(p->tmp_str) == SUCCESS) {
                            append_with_newline(is_droid_reminder(p->tmp_str) ? sys_text : p->text_accum, p->tmp_str);
                        }
                        continue;
                    }
                    for (auto ce : carr) {
                        ondemand::object co;
                        if (ce.get_object().get(co) != SUCCESS) continue;
                        std::string ctype, cname;
                        std::string ctext, cthink, cargs;
                        bool has_result = false;
                        std::string cres;
                        for (auto cf : co) {
                            ondemand::raw_json_string ck;
                            if (cf.key().get(ck) != SUCCESS) continue;
                            if (ck == "type") {
                                (void)cf.value().get_string(ctype);
                            } else if (ck == "text") {
                                (void)cf.value().get_string(ctext);
                            } else if (ck == "thinking") {
                                if (opts->include_thinking) (void)cf.value().get_string(cthink);
                            } else if (ck == "name") {
                                (void)cf.value().get_string(cname);
                            } else if (ck == "input") {
                                ondemand::value av;
                                if (cf.value().get(av) == SUCCESS) append_tool_args(cargs, av, 256);
                            } else if (ck == "content") {
                                if (opts->include_tool_results) {
                                    ondemand::value rv;
                                    if (cf.value().get(rv) == SUCCESS) {
                                        append_droid_tool_result(cres, rv);
                                        has_result = true;
                                    }
                                }
                            }
                        }
                        if (ctype == "text" && !ctext.empty()) {
                            append_with_newline(is_droid_reminder(ctext) ? sys_text : p->text_accum, ctext);
                        } else if (ctype == "thinking" && !cthink.empty()) {
                            append_with_newline(p->think_accum, cthink);
                        } else if (ctype == "tool_use" && !cname.empty()) {
                            if (!p->tool_accum.empty()) p->tool_accum.push_back('\n');
                            p->tool_accum.append(cname);
                            if (!cargs.empty()) {
                                p->tool_accum.append(": ");
                                p->tool_accum.append(cargs);
                            }
                        } else if (ctype == "tool_result" && has_result && !cres.empty()) {
                            append_with_newline(tool_res, cres);
                        }
                    }
                }
            }
        }
    }

    if (top_type == "session_start") {
        if (!id.empty()) ctx->session_id = arena_copy_sv(arena, id);
        if (!cwd.empty()) ctx->cwd = arena_copy_sv(arena, cwd);
        if (!title.empty() && title != "New Session" && title != "新会话") ctx->title = arena_copy_sv(arena, title);
        if (!calling_sid.empty()) {
            ctx->parent_session_id = arena_copy_sv(arena, calling_sid);
            ctx->session_flags |= STELOG_FLAG_IS_SUBAGENT;
        }
        out->updated_context = true;
        return 0;
    }
    if (top_type != "message") return 0;

    StelogStr msg_id_s = arena_copy_sv(arena, id);
    StelogStr parent_id_s = arena_copy_sv(arena, parent_id);

    if (role == "user") {
        /* llm_only user turns are harness context (system reminders, tool catalogs), not human input */
        bool harness = (visibility == "llm_only") || system_notice || starts_with(id, "__internal__");
        if (harness) {
            append_with_newline(sys_text, p->text_accum);
            p->text_accum.clear();
        }
        if (!p->text_accum.empty()) {
            StelogStr actor = { "user", 4 };
            emit_event(out, STELOG_AGENT_DROID, STELOG_ROLE_USER, 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor,
                       msg_id_s, parent_id_s, arena_copy_sv(arena, p->text_accum));
        }
        if (opts->include_system && !sys_text.empty()) {
            StelogStr actor = { "system", 6 };
            emit_event(out, STELOG_AGENT_DROID, STELOG_ROLE_SYSTEM, 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor,
                       msg_id_s, parent_id_s, arena_copy_sv(arena, sys_text));
        }
        if (opts->include_tool_results && !tool_res.empty()) {
            bool trunc = false;
            StelogStr txt = stelog_truncate_middle(arena, tool_res.data(), tool_res.size(), 4096u, &trunc);
            if (!trunc) txt = arena_copy_sv(arena, tool_res);
            StelogStr actor = { "tool", 4 };
            emit_event(out, STELOG_AGENT_DROID, STELOG_ROLE_TOOL_RESULT,
                       trunc ? STELOG_FLAG_TRUNCATED : 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor, msg_id_s, parent_id_s, txt);
        }
    } else if (role == "assistant") {
        if (!model_id.empty()) {
            std::string actor_s = "droid:" + model_id;
            if (ctx->actor.len != actor_s.size() || std::memcmp(ctx->actor.ptr, actor_s.data(), actor_s.size()) != 0) {
                ctx->actor = arena_copy_sv(arena, actor_s);
                out->updated_context = true;
            }
        }
        StelogStr actor = ctx->actor.len ? ctx->actor : StelogStr{ "droid:main", 10 };
        if (!p->text_accum.empty()) {
            emit_event(out, STELOG_AGENT_DROID, STELOG_ROLE_ASSISTANT, 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor,
                       msg_id_s, parent_id_s, arena_copy_sv(arena, p->text_accum));
        }
        if (!p->tool_accum.empty()) {
            bool trunc = false;
            uint32_t max_b = opts->tool_call_max_bytes ? opts->tool_call_max_bytes : 512u;
            StelogStr txt = stelog_truncate_middle(arena, p->tool_accum.data(), p->tool_accum.size(), max_b, &trunc);
            if (!trunc) txt = arena_copy_sv(arena, p->tool_accum);
            emit_event(out, STELOG_AGENT_DROID, STELOG_ROLE_TOOL_CALL,
                       trunc ? STELOG_FLAG_TRUNCATED : 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor,
                       msg_id_s, parent_id_s, txt);
        }
        if (opts->include_thinking && !p->think_accum.empty()) {
            emit_event(out, STELOG_AGENT_DROID, STELOG_ROLE_THINKING, 0, ts_ms,
                       file_id, raw_offset, (uint32_t)line_len, ctx, actor,
                       msg_id_s, parent_id_s, arena_copy_sv(arena, p->think_accum));
        }
    }
    return 0;
}

/* ============================================================================
 * 5. MCP JSON-RPC 2.0 Request Parser (powered by simdjson On-Demand)
 * ============================================================================ */

static void parse_mcp_arguments(ondemand::object &args, StelogArena *arena, StelogMcpRequest *out) {
    std::string tmp;
    for (auto f : args) {
        ondemand::raw_json_string k;
        if (f.key().get(k) != SUCCESS) continue;

        if (k == "query") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_query = arena_copy_sv(arena, tmp);
        } else if (k == "agent") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_agent = arena_copy_sv(arena, tmp);
        } else if (k == "project") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_project = arena_copy_sv(arena, tmp);
        } else if (k == "role") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_role = arena_copy_sv(arena, tmp);
        } else if (k == "exclude_role") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_exclude_role = arena_copy_sv(arena, tmp);
        } else if (k == "actor") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_actor = arena_copy_sv(arena, tmp);
        } else if (k == "session" || k == "session_id") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_session = arena_copy_sv(arena, tmp);
        } else if (k == "since") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_since = arena_copy_sv(arena, tmp);
        } else if (k == "until") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_until = arena_copy_sv(arena, tmp);
        } else if (k == "sort") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_sort = arena_copy_sv(arena, tmp);
        } else if (k == "count_by") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_count_by = arena_copy_sv(arena, tmp);
        } else if (k == "group_by") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_group_by = arena_copy_sv(arena, tmp);
        } else if (k == "file") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_file = arena_copy_sv(arena, tmp);
        } else if (k == "case" || k == "case_mode") {
            if (f.value().get_string(tmp) == SUCCESS) out->arg_case = arena_copy_sv(arena, tmp);
        } else if (k == "fixed" || k == "fixed_strings") {
            bool b = false;
            if (f.value().get_bool().get(b) == SUCCESS) out->arg_fixed = b;
        } else if (k == "regex") {
            bool b = false;
            if (f.value().get_bool().get(b) == SUCCESS) out->arg_regex = b;
        } else if (k == "doc_id") {
            uint64_t u = 0;
            if (f.value().get_uint64().get(u) == SUCCESS) {
                out->arg_doc_id = u;
                out->has_doc_id = true;
            } else if (f.value().get_string(tmp) == SUCCESS && !tmp.empty()) {
                out->arg_doc_id = std::strtoull(tmp.c_str(), nullptr, 10);
                out->has_doc_id = true;
            }
        } else if (k == "doc_ids") {
            ondemand::array arr;
            if (f.value().get_array().get(arr) == SUCCESS) {
                for (auto el : arr) {
                    if (out->arg_doc_id_count >= STELOG_MCP_MAX_BATCH_DOCS) break;
                    uint64_t u = 0;
                    if (el.get_uint64().get(u) == SUCCESS) {
                        out->arg_doc_ids[out->arg_doc_id_count++] = u;
                    } else if (el.get_string(tmp) == SUCCESS && !tmp.empty()) {
                        out->arg_doc_ids[out->arg_doc_id_count++] = std::strtoull(tmp.c_str(), nullptr, 10);
                    }
                }
            }
        } else if (k == "limit") {
            uint64_t u = 0;
            if (f.value().get_uint64().get(u) == SUCCESS) out->arg_limit = (uint32_t)u;
        } else if (k == "offset") {
            uint64_t u = 0;
            if (f.value().get_uint64().get(u) == SUCCESS) out->arg_offset = (uint32_t)u;
        } else if (k == "window") {
            uint64_t u = 0;
            if (f.value().get_uint64().get(u) == SUCCESS) out->arg_window = (uint32_t)u;
        } else if (k == "max_bytes" || k == "max_bytes_per_item") {
            uint64_t u = 0;
            if (f.value().get_uint64().get(u) == SUCCESS) out->arg_max_bytes = (uint32_t)u;
        } else if (k == "include_tools") {
            bool b = false;
            if (f.value().get_bool().get(b) == SUCCESS) out->arg_include_tools = b;
        } else if (k == "include_thinking") {
            bool b = false;
            if (f.value().get_bool().get(b) == SUCCESS) out->arg_include_thinking = b;
        } else if (k == "no_subagents") {
            bool b = false;
            if (f.value().get_bool().get(b) == SUCCESS) out->arg_no_subagents = b;
        } else if (k == "no_dedup") {
            bool b = false;
            if (f.value().get_bool().get(b) == SUCCESS) out->arg_no_dedup = b;
        } else if (k == "raw") {
            bool b = false;
            if (f.value().get_bool().get(b) == SUCCESS) out->arg_raw = b;
        } else if (k == "tail") {
            bool b = false;
            if (f.value().get_bool().get(b) == SUCCESS) out->arg_tail = b;
        }
    }
}

extern "C" int stelog_json_parse_mcp_request(StelogJsonParser *p,
                                             const char *line,
                                             size_t line_len,
                                             size_t padded_cap,
                                             StelogArena *arena,
                                             StelogMcpRequest *out) {
    std::memset(out, 0, sizeof(*out));
    ondemand::document doc;
    if (p->parser.iterate(p->make_view(line, line_len, padded_cap)).get(doc) != SUCCESS) {
        return -1;
    }
    ondemand::object root;
    if (doc.get_object().get(root) != SUCCESS) {
        return -1;
    }

    std::string tmp;
    for (auto field : root) {
        ondemand::raw_json_string key;
        if (field.key().get(key) != SUCCESS) continue;
        if (key == "id") {
            ondemand::value v;
            if (field.value().get(v) != SUCCESS) continue;
            ondemand::json_type jt;
            if (v.type().get(jt) != SUCCESS) continue;
            if (jt == ondemand::json_type::number) {
                int64_t iv = 0;
                if (v.get_int64().get(iv) == SUCCESS) {
                    out->id_kind = STELOG_RPC_ID_INT;
                    out->id_int = iv;
                }
            } else if (jt == ondemand::json_type::string) {
                if (v.get_string(tmp) == SUCCESS) {
                    out->id_kind = STELOG_RPC_ID_STR;
                    out->id_str = arena_copy_sv(arena, tmp);
                }
            } else if (jt == ondemand::json_type::null) {
                out->id_kind = STELOG_RPC_ID_NULL;
            }
        } else if (key == "method") {
            if (field.value().get_string(tmp) == SUCCESS) {
                out->method = arena_copy_sv(arena, tmp);
            }
        } else if (key == "params") {
            ondemand::object params;
            if (field.value().get_object().get(params) != SUCCESS) continue;
            for (auto pf : params) {
                ondemand::raw_json_string pk;
                if (pf.key().get(pk) != SUCCESS) continue;
                if (pk == "name") {
                    if (pf.value().get_string(tmp) == SUCCESS) {
                        out->tool_name = arena_copy_sv(arena, tmp);
                    }
                } else if (pk == "protocolVersion") {
                    if (pf.value().get_string(tmp) == SUCCESS) {
                        out->protocol_version = arena_copy_sv(arena, tmp);
                    }
                } else if (pk == "clientInfo") {
                    ondemand::object ci;
                    if (pf.value().get_object().get(ci) != SUCCESS) continue;
                    for (auto cf : ci) {
                        ondemand::raw_json_string ck;
                        if (cf.key().get(ck) != SUCCESS) continue;
                        if (ck == "name" && cf.value().get_string(tmp) == SUCCESS) {
                            out->client_name = arena_copy_sv(arena, tmp);
                        }
                    }
                } else if (pk == "arguments") {
                    ondemand::object args;
                    if (pf.value().get_object().get(args) == SUCCESS) {
                        parse_mcp_arguments(args, arena, out);
                    }
                }
            }
        }
    }
    return 0;
}
