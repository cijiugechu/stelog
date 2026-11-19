// Compile-time embedded, validated, and minified MCP static JSON payloads.
// Source files are kept pretty-printed for readability and stripped to single-line
// JSON at comptime so stdio JSON-RPC NDJSON framing has zero runtime overhead.

fn minifiedLen(comptime src: []const u8) usize {
    @setEvalBranchQuota(src.len * 16 + 1024);
    var len: usize = 0;
    var in_str = false;
    var escaped = false;
    var depth: usize = 0;

    for (src) |c| {
        if (in_str) {
            if (c < 0x20) @compileError("Unescaped control character in embedded JSON string");
            len += 1;
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_str = false;
            }
        } else {
            switch (c) {
                ' ', '\t', '\r', '\n' => {},
                '"' => {
                    in_str = true;
                    len += 1;
                },
                '{', '[' => {
                    depth += 1;
                    len += 1;
                },
                '}', ']' => {
                    if (depth == 0) @compileError("Unbalanced closing bracket/brace in embedded JSON");
                    depth -= 1;
                    len += 1;
                },
                else => {
                    len += 1;
                },
            }
        }
    }

    if (in_str) @compileError("Unterminated string literal in embedded JSON");
    if (depth != 0) @compileError("Unbalanced opening bracket/brace in embedded JSON");
    if (len == 0) @compileError("Embedded JSON cannot be empty");
    return len;
}

fn minifyJson(comptime src: []const u8) [minifiedLen(src):0]u8 {
    @setEvalBranchQuota(src.len * 16 + 1024);
    const out_len = comptime minifiedLen(src);
    var out: [out_len:0]u8 = undefined;
    var i: usize = 0;
    var in_str = false;
    var escaped = false;

    for (src) |c| {
        if (in_str) {
            out[i] = c;
            i += 1;
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_str = false;
            }
        } else {
            switch (c) {
                ' ', '\t', '\r', '\n' => {},
                '"' => {
                    in_str = true;
                    out[i] = c;
                    i += 1;
                },
                else => {
                    out[i] = c;
                    i += 1;
                },
            }
        }
    }
    out[out_len] = 0;
    return out;
}

// Emits `src` (minified JSON) without any `,"<key>":<value>` members, or only its length when
// `out` is null. Keys are matched outside string literals only.
fn stripMember(comptime src: []const u8, comptime key: []const u8, out: ?[]u8) usize {
    @setEvalBranchQuota(src.len * 64 + 1024);
    const needle = ",\"" ++ key ++ "\":";
    var n: usize = 0;
    var i: usize = 0;
    var in_str = false;
    var escaped = false;
    while (i < src.len) {
        const c = src[i];
        if (!in_str and c == ',' and i + needle.len <= src.len and std.mem.eql(u8, src[i .. i + needle.len], needle)) {
            var j = i + needle.len;
            var depth: usize = 0;
            var s_in = false;
            var s_esc = false;
            while (j < src.len) : (j += 1) {
                const d = src[j];
                if (s_in) {
                    if (s_esc) {
                        s_esc = false;
                    } else if (d == '\\') {
                        s_esc = true;
                    } else if (d == '"') {
                        s_in = false;
                        if (depth == 0) {
                            j += 1;
                            break;
                        }
                    }
                    continue;
                }
                switch (d) {
                    '"' => s_in = true,
                    '{', '[' => depth += 1,
                    '}', ']' => {
                        if (depth == 0) break;
                        depth -= 1;
                        if (depth == 0) {
                            j += 1;
                            break;
                        }
                    },
                    ',' => if (depth == 0) break,
                    else => {},
                }
            }
            i = j;
            continue;
        }
        if (in_str) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_str = false;
            }
        } else if (c == '"') {
            in_str = true;
        }
        if (out) |o| o[n] = c;
        n += 1;
        i += 1;
    }
    return n;
}

fn stripOutputSchemas(comptime src: []const u8) [stripMember(src, "outputSchema", null):0]u8 {
    const len = comptime stripMember(src, "outputSchema", null);
    var out: [len:0]u8 = undefined;
    _ = stripMember(src, "outputSchema", out[0..len]);
    out[len] = 0;
    return out;
}

const std = @import("std");
const kInitializeMinified = minifyJson(@embedFile("initialize.json"));
const kToolsListMinified = minifyJson(@embedFile("tools_list.json"));
// For clients (Codex) that would show structuredContent to the model instead of the text result.
const kToolsListPlain = stripOutputSchemas(&kToolsListMinified);

pub export const stelog_mcp_initialize_json: [kInitializeMinified.len:0]u8 = kInitializeMinified;
pub export const stelog_mcp_initialize_json_len: usize = kInitializeMinified.len;

pub export const stelog_mcp_tools_list_json: [kToolsListMinified.len:0]u8 = kToolsListMinified;
pub export const stelog_mcp_tools_list_json_len: usize = kToolsListMinified.len;

pub export const stelog_mcp_tools_list_plain_json: [kToolsListPlain.len:0]u8 = kToolsListPlain;
pub export const stelog_mcp_tools_list_plain_json_len: usize = kToolsListPlain.len;
