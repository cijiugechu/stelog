# stelog Usage Guide

Practical cheatsheet for the `stelog` CLI and MCP Server.

## Route by Question

| Goal | Command |
| :--- | :--- |
| Find which session(s) decided X *(best first step)* | `stelog search "X \| Y" . --group-by session` |
| Read a session's condensed decision stream | `stelog digest @<sid8> [--budget 14336]` |
| Trace how a topic evolved over time | `stelog timeline "X \| Y" .` |
| Find who created or edited a file | `stelog files roaring.c .` |
| See surrounding dialogue right inside search | `stelog search "X" . -C 2` |
| Expand surrounding turns around a `doc_id` | `stelog context <doc_id> -w 5` |
| Read full message or raw JSONL line on disk | `stelog show <doc_id> [--raw] [--tail]` |
| List recent sessions for the current repo | `stelog sessions . --since 7d` |
| Count matches or break down by session/agent/role | `stelog search "X" -c` / `--count-by session` |
| Find earliest / latest mention | `stelog search "X" --first 1` / `--last 1` |

## Query Syntax

### Boolean, Phrases & Wildcards

| Syntax | Meaning | Example |
| :--- | :--- | :--- |
| `A B` | Logical AND (all terms required) | `stelog search "gateway timeout"` |
| `A \| B` / `A OR B` | Logical OR (great for bilingual queries) | `stelog search "镜像 \| mirror"` |
| `"exact phrase"` | Exact substring verification | `stelog search "\"zero copy\""` |
| `-term` / `-"phrase"` | Exclude docs matching term or phrase | `stelog search "镜像 \| mirror -docker"` |
| `prefix*` | Word-start prefix wildcard (len ≥ 2) | `stelog search "truncat*"` |
| `path/or::symbol` | Auto exact-substring verify for code tokens | `stelog search "src/store/roaring.c"` |

### Inline Modifiers

Modifiers work inside `<query>` in both CLI and MCP `search_history`:

- `agent:<csv>` — `codex`, `agy`, `pi` (e.g. `agent:agy,codex`)
- `role:<csv>` / `-role:<csv>` — `user`, `assistant`, `inter_agent`, `tool_call`, `tool_result`, `thinking`, `system`
- `project:<substr>` (or `proj:<substr>`) — project/cwd substring
- `actor:<substr>` — actor/model substring
- `session:<prefix>` or `@<4+hex>` — session UUID prefix (e.g. `@30c3cab1`; non-hex `@Override` is treated as normal text)
- `since:<val>` / `until:<val>` — relative (`7d`, `24h`) or ISO-8601 (`2026-11-01T00:00:00Z`)

```bash
stelog search "agent:agy role:user project:unicus since:7d 镜像 -docker"
```

### Case Sensitivity, Fixed Strings & PCRE2 Regex

- **`-S`, `--smart-case`** *(default)*: Case-insensitive when all-lowercase; automatically case-sensitive if query contains uppercase letters (ignores ` OR ` and regex escapes like `\D`, `\S`, `\p{Han}`).
- **`-i`, `--ignore-case`**: Force case-insensitive matching.
- **`-s`, `--case-sensitive`**: Force case-sensitive matching.
- **`-F`, `--fixed-strings`**: Treat the entire query as a literal substring (no OR, negation, modifiers, or wildcards). Use `--` before queries starting with `-`:
  ```bash
  stelog search -F "POST /api/parts/mirror-rules"
  stelog search -F -- "--smart-case"
  ```
- **`-E`, `--regex`, `-P`**: Full UTF-8 PCRE2 JIT regular expression with automatic literal/index prefiltering and ReDoS limits (`^` and `$` are multiline):
  ```bash
  stelog search -E 'qz[0-9]{4,}[a-z]?'
  stelog search -E 'docker(?=\s+compose)'
  stelog search -E 'src/store/[a-z_]+\.c'
  ```

## Positional Targets & Common Flags

Positional target shortcuts can follow `<query>`:

- `.` — scope to current working directory (`project`)
- `@<prefix>` — scope to session UUID prefix (e.g. `@30c3cab1`)
- `/<path>` — scope to project path substring

Common flags for `search` / `timeline`:

| Flag | Description |
| :--- | :--- |
| `--agent <csv>` | Filter by agent (`codex`, `agy`, `pi`) |
| `-t, --role <csv>` | Include specific roles (default: `user,assistant,inter_agent,tool_call`) |
| `-T, --exclude-role <csv>` | Exclude roles |
| `--no-subagents` | Exclude subagent transcripts |
| `--include-tools` / `--include-thinking` | Also search `tool_result` / `thinking` (if indexed) |
| `--since <7d\|24h\|ISO8601>` / `--until <ISO8601>` | Time window filter |
| `-C <N>` / `-B <N>` / `-A <N>` | Show N dialogue turns before/after each hit |
| `--group-by session` | Group hits by session with title, hit count & best snippet |
| `--count-by <axis>` | Histogram by `agent`, `role`, `project`, `actor`, or `session` |
| `-c, --count` / `-l, --sessions-only` | Print total match count only / unique session IDs only |
| `--sort <relevance\|oldest\|newest>` | Sort order (`--first N`, `--last N`, `--max-count N\|-N`) |
| `--no-dedup` | Keep exact duplicate copies across forked/retried transcripts |
| `--limit <N>` / `--offset <N>` | Pagination (default limit `20`) |
| `--json` / `--no-color` / `--no-sync` | JSON output / disable ANSI colors / skip auto Hot Delta sync |

## Index Management & Diagnostics

Every CLI query automatically checks for newly appended transcript bytes and updates the Hot Delta segment in ~1–2 ms (pass `--no-sync` to skip).

```bash
stelog index                   # incremental Hot Delta sync
stelog index --compact         # compact Base + Hot Delta into a single Base segment
stelog index --rebuild         # full rebuild from raw transcripts
stelog stats                   # show Base/Delta doc counts, terms, and disk usage
stelog bench --iters 300       # 6-suite latency & zero-malloc regression benchmark
```

- **Default index location**: `~/.cache/stelog/index.v2`
- **Override location**: set `STELOG_INDEX_DIR=/path/to/dir` or pass `--index-dir <DIR>`
- **Disable colors**: set `NO_COLOR=1` or pass `--no-color`

## MCP Server Tools (`stelog mcp`)

Runs JSON-RPC 2.0 over stdio with automatic Hot Delta refresh (2s cooldown) and a 32 KB / 500-line response budget guard.

| Tool | Purpose | Key Arguments |
| :--- | :--- | :--- |
| `search_history` | Full-text, fixed-string, or regex search | `query`, `agent`, `project`, `role`, `exclude_role`, `actor`, `session`, `since`, `until`, `case`, `fixed`, `regex`, `group_by`, `count_by`, `sort`, `no_subagents`, `limit`, `offset` |
| `get_session_digest` | Condensed chronological user ↔ assistant stream | `session` *(required)*, `max_bytes`, `include_tools` |
| `get_decision_timeline` | Oldest-first evolution of a topic across agents | `query` *(required)*, `agent`, `project`, `role`, `since`, `until`, `no_subagents`, `limit`, `offset` |
| `get_session_context` | Surrounding dialogue turns around a `doc_id` | `doc_id` *(required)*, `window`, `max_bytes_per_item`, `include_tools` |
| `get_document` | Full/paginated doc(s) or raw JSONL via `pread` | `doc_id` or `doc_ids` *(batch up to 16)*, `raw`, `offset`, `limit`, `tail` |
| `list_sessions` | Recent sessions sorted by activity with titles | `agent`, `project`, `since`, `until`, `limit`, `offset` |
| `search_file_history` | Sessions & actors that edited/created a file | `file`, `agent`, `project`, `session`, `since`, `until`, `no_subagents`, `limit`, `offset` |

### Definitive Absence

When a search returns `0 matches`, `stelog` reports how many documents were in scope and probes outside current filters (e.g. `Hint: 4 matches exist outside current filters (assistant=3, tool_call=1)`) so you know whether to widen filters or stop looking.
