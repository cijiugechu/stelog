#!/bin/sh
set -eu

stelog=$1
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
index=$fixture/index

run() {
    env HOME="$fixture" STELOG_INDEX_DIR="$index" "$stelog" "$@"
}

expect_status() {
    expected=$1
    shift
    status=0
    run "$@" >"$fixture/stdout" 2>"$fixture/stderr" || status=$?
    if [ "$status" -ne "$expected" ]; then
        echo "$*: expected exit $expected, got $status" >&2
        cat "$fixture/stderr" >&2
        exit 1
    fi
}

archived=$fixture/.codex/archived_sessions
mkdir -p "$archived"
cat >"$archived/fixture.jsonl" <<'JSON'
{"type":"session_meta","payload":{"id":"fixture","cwd":"/project"}}
{"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"Searchable needle --quiet"}]}}
JSON

expect_status 1 doctor
grep -F "$archived  [readable, 1 file]" "$fixture/stdout" >/dev/null
grep -F "$index  [missing]" "$fixture/stdout" >/dev/null
test ! -e "$index"
expect_status 2 search -q needle --no-sync
test ! -s "$fixture/stdout"
test -s "$fixture/stderr"

expect_status 0 search needle -q -c --json --offset 100
test ! -s "$fixture/stdout"
test ! -s "$fixture/stderr"
expect_status 0 search -q -F -- --quiet
expect_status 1 search --quiet 'needle -searchable'
test ! -s "$fixture/stdout"
test ! -s "$fixture/stderr"
expect_status 1 search --quiet needle --agent pi
expect_status 2 search -q -E '['
test ! -s "$fixture/stdout"
test -s "$fixture/stderr"
expect_status 2 search --quiet needle --index-dir

expect_status 0 doctor --json
grep -F '"healthy":true' "$fixture/stdout" >/dev/null
expect_status 1 doctor --index-dir "$fixture/other" --json
grep -F '"healthy":false' "$fixture/stdout" >/dev/null
test ! -e "$fixture/other"
printf 'bad metadata' >"$index/current.meta"
expect_status 1 doctor
grep -F "$index  [unreadable or invalid]" "$fixture/stdout" >/dev/null
