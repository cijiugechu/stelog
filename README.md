# stelog

Ultra-fast local history search and decision archaeology across coding agents (**Codex CLI**, **AGY CLI**, **Pi**, and **Factory Droid**).

One **2.8 MB** static binary provides both a `ripgrep`-inspired CLI and a stdio **MCP Server** (`stelog mcp`) backed by a Data-Oriented `mmap` index — microsecond queries, automatic Hot Delta refresh, no background daemon.

## Why stelog

- **Purified decision signal**: Strips 90%+ transcript noise (`compacted` snapshots, duplicate events, harness-injected `AGENTS.md` / `<environment_context>` prompts labeled `role:user`, huge tool outputs) while keeping O(1) `pread` access to raw JSONL records.
- **Microsecond queries**: Columnar SoA `mmap`, NEON/SSE2 SwissTable + Roaring bitmaps, SIMD substring search, and PCRE2 JIT regex with automatic literal prefiltering (P50 **~11 µs** exact, **~380 µs** regex on 1.57 GB of transcripts; zero heap allocation on hot queries).
- **Zero-dependency deployment**: Pure C11/C++17 core with vendored `simdjson`, `simdutf`, `pcre2`, `zstd`, and `sqlite3`. Cross-compiles cleanly to macOS and Linux (ARM64 & x86_64).

## Supported Transcript Sources

| Agent | Local Data Path |
| :--- | :--- |
| **Codex CLI** | `~/.codex/sessions/**/*.jsonl(.zst)`, `archived_sessions/`, `state_5.sqlite` |
| **AGY CLI** | `~/.gemini/antigravity-cli/brain/*/.system_generated/logs/transcript.jsonl`, `conversation_summaries.db` |
| **Pi** | `~/.pi/agent/sessions/**/*.jsonl` |
| **Factory Droid** | `~/.factory/sessions/<cwd-slug>/<session-id>.jsonl` |

## Build & Install

Requires **Zig 0.16+**.

```bash
# Build native ReleaseFast binary -> ./zig-out/bin/stelog
zig build -Doptimize=ReleaseFast

# Run all unit & integration tests
zig build test -Doptimize=ReleaseFast

# Optional: install to ~/.local/bin
cp zig-out/bin/stelog ~/.local/bin/stelog
```

Cross-compile static binaries for other targets:

```bash
zig build -Doptimize=ReleaseFast -Dtarget=x86_64-linux-musl
zig build -Doptimize=ReleaseFast -Dtarget=aarch64-linux-musl
zig build -Doptimize=ReleaseFast -Dtarget=x86_64-macos
zig build -Doptimize=ReleaseFast -Dtarget=aarch64-macos
```

## Quickstart

```bash
# 1. Build (or incrementally sync) the local index
stelog index

# 2. Best 2-step decision archaeology workflow:
#    Step A: find which session(s) made the decision (bilingual OR + negation)
stelog search "镜像 | mirror -docker" . --group-by session

#    Step B: read that session's condensed user <-> assistant decision stream
stelog digest @30c3cab1

# 3. Other essentials
stelog timeline "simdutf | roaring" .           # chronological evolution
stelog files roaring.c .                        # who edited a file and when
stelog search -E 'qz[0-9]{4,}[a-z]?' -C 1       # PCRE2 JIT regex + inline context
stelog bench                                    # 6-suite latency & 0-malloc benchmark
```

## MCP Server & Agent Skill

Add `stelog` as a stdio MCP server in Codex, AGY, Pi, or Claude Code:

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

An agent skill with recommended tool-calling patterns is bundled at [skills/stelog/SKILL.md](skills/stelog/SKILL.md).

## Documentation

- **User Guide & Syntax Cheatsheet**: [docs/USAGE.md](docs/USAGE.md)
- **Agent Skill**: [skills/stelog/SKILL.md](skills/stelog/SKILL.md)
