---
name: stelog
description: "Ultra-fast local coding agent history search and decision archaeology across Codex CLI (~/.codex), AGY CLI (~/.gemini/antigravity-cli), and Pi (~/.pi/agent). Use whenever asked when or who made a decision, why a design changed, which session edited a file, or before writing ad-hoc grep/jq/python scripts over local session .jsonl files."
---

# stelog

`stelog` indexes local coding agent session transcripts (Codex CLI, AGY CLI, Pi) into a compact Data-Oriented mmap index (microsecond queries, automatic Hot Delta refresh). Use `stelog` (via MCP tools or CLI) instead of manually grepping raw `.jsonl` / `.jsonl.zst` files.

## Laws

1. **Never hand-parse `.jsonl` logs**: Raw logs mix 90%+ noise (`compacted` snapshots, duplicate events, injected `AGENTS.md`/`<environment_context>` prompts labeled `role:user`, encrypted reasoning, giant tool outputs). `stelog` purifies human prompts and indexes clean decision layers.
2. **Trust Definitive Absence**: A `0 matches` result is not an error. Read the scope summary and cross-role hint (`N matches exist outside current filters`) before widening filters (`--include-tools`, `--include-thinking`, or dropping `--role`/`--project`).
3. **Narrow search -> Expand on demand**: Start with `search` / `timeline` / `files` to find `doc_id`s, then call `context <doc_id>` to read the surrounding discussion or `show <doc_id> [--raw]` to inspect full/raw bytes.

## Optimal 2-Step Decision Archaeology Workflow

1. **Locate the right session first (~500 B)**: `stelog search "镜像 | mirror" [.] --group-by session` (or MCP `search_history(query="镜像 | mirror", group_by="session")`). Use `|` / `OR` to cover bilingual Chinese+English terms in one shot.
2. **Read the entire decision stream (~6–12 KB)**: `stelog digest @<sid8>` (or MCP `get_session_digest(session="@<sid8>")`). Collapses intermediate tool calls, deduplicates retries, and preserves ending options/conclusions of long assistant turns.

## Route by Question

| Question | CLI Command | MCP Tool |
| :--- | :--- | :--- |
| Which session(s) decided X? (best first step) | `stelog search "X \| Y" [.] --group-by session` | `search_history(group_by="session")` |
| Read full decision stream of a session | `stelog digest @<sid8> [--budget 14336]` | `get_session_digest` |
| Who decided X and why? | `stelog search "X" [.] [-t user,assistant] --no-subagents` | `search_history` |
| When did X evolve from A to B across agents? | `stelog timeline "X \| Y" [.]` | `get_decision_timeline` |
| Which session edited `rate_limit.rs`? | `stelog files rate_limit.rs [.]` | `search_file_history` |
| What was discussed right before/after `doc_id`? | `stelog context <doc_id> -w 5` | `get_session_context` |
| Read full truncated message or raw JSONL line | `stelog show <doc_id> [--raw] [--offset N]` | `get_document` |
| Which sessions touched this repo recently? | `stelog sessions . --since 7d` | `list_sessions` |
| See distribution of a topic across projects/agents/roles | `stelog search "X" --count-by session` | `search_history(count_by="session")` |
| Earliest / latest mention of X | `stelog search "X" --first 1` / `--last 1` | `search_history(sort="oldest"\|"newest", limit=1)` |

## Query Syntax & Useful Flags

- **OR branches**: `"镜像 | mirror"` or `"镜像 OR mirror"` — cover bilingual Chinese + English terms in one call.
- **Negation**: `镜像 -docker` or `镜像 -"docker compose"` — exclude noise terms/phrases (O(1) inverted-index filter).
- **Prefix wildcard**: `truncat*`, `qz706*` — matches any word starting with prefix (SIMD contiguous `text.bin` scan).
- **Inline modifiers**: `agent:agy role:user project:unicus since:7d @8956a5cb 镜像 -docker` (works in both CLI and MCP `query`).
- **Smart-case & Fixed strings**: Smart-case (`-S`, default: uppercase triggers case-sensitive), `-i` (`case="insensitive"`), `-s` (`case="sensitive"`), `-F` (`fixed=true` for literal URLs/paths like `POST /api/parts/mirror-rules`).
- **PCRE2 JIT Regex (`-E` / `regex=true`)**: Full Perl-compatible UTF-8 regex (`QZ706\d+[A-Z]?`, `docker(?=\s+compose)`, `^Mirror\s+rules`) with automatic 3-tier acceleration (SwissTable interior tokens → NEON/SSE2 literal scan → P-core parallel PCRE2 JIT) and ReDoS limits.
- **Inline context (CLI)**: `-C N` / `-B N` / `-A N` shows N surrounding dialogue turns directly in `stelog search`.
- **Target shortcuts**: `.` (current working directory), `@<prefix>` (session UUID prefix), `/<path>` (project substring).
- `-t, --role <csv>` / `-T, --exclude-role <csv>` — roles: `user`, `assistant`, `inter_agent`, `tool_call`, `tool_result`, `thinking`, `system`.
- `--no-subagents` — exclude subagent transcripts to focus on main-thread human decisions.
- `-c` (count only), `-l` (matching session IDs one per line), `--json` (structured JSON).

## MCP Setup

Add `stelog` as a stdio MCP server in Codex / AGY / Pi / Claude Code:

```json
{
  "mcpServers": {
    "stelog": {
      "command": "stelog",
      "args": ["mcp"]
    }
  }
}
```
