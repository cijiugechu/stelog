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

const kInitializeMinified = minifyJson(@embedFile("initialize.json"));
const kToolsListMinified = minifyJson(@embedFile("tools_list.json"));

pub export const stelog_mcp_initialize_json: [kInitializeMinified.len:0]u8 = kInitializeMinified;
pub export const stelog_mcp_initialize_json_len: usize = kInitializeMinified.len;

pub export const stelog_mcp_tools_list_json: [kToolsListMinified.len:0]u8 = kToolsListMinified;
pub export const stelog_mcp_tools_list_json_len: usize = kToolsListMinified.len;
