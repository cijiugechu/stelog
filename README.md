# stelog

Search your local coding-agent conversations to find past decisions, revisit what changed in a file, or read a condensed session history.

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
# Build stelog
zig build -Doptimize=ReleaseFast

# Check the version
./zig-out/bin/stelog --version

# Optional: install to ~/.local/bin
mkdir -p ~/.local/bin
cp zig-out/bin/stelog ~/.local/bin/stelog
```

To build for a different platform, choose the corresponding target below.

```bash
zig build -Doptimize=ReleaseFast -Dtarget=x86_64-linux-musl
zig build -Doptimize=ReleaseFast -Dtarget=aarch64-linux-musl
zig build -Doptimize=ReleaseFast -Dtarget=x86_64-macos
zig build -Doptimize=ReleaseFast -Dtarget=aarch64-macos
```

## Quickstart

```bash
# Prepare your history for search or refresh it after a session
stelog index

# Find sessions mentioning “镜像” or “mirror” and exclude “docker”
stelog search "镜像 | mirror -docker" . --group-by session

# Read a condensed conversation using a session ID from the results
stelog digest @30c3cab1

# Follow mentions of a topic over time
stelog timeline "镜像 | mirror" .

# Find sessions that touched a file
stelog files README.md .

# Search with a regular expression and show surrounding context
stelog search -E 'qz[0-9]{4,}[a-z]?' -C 1
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
