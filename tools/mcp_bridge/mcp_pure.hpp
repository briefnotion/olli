// Pure helpers for mcp_bridge - no sockets, no child processes, no files, no
// olli state - so they can be unit-tested on their own (see README.md).
//
//   - the config file (mcp_bridge.json) -> MCP_BRIDGE_CONFIG
//   - an MCP tool definition -> an olli `register` entry (name, description,
//     parameters), with the safety limits applied to text a third-party
//     server wrote
//   - an MCP tools/call result -> the plain string olli's `result` carries
//   - UTF-8 hygiene (nlohmann's dump() THROWS on invalid UTF-8, and a throw
//     out of the main loop would end the bridge - see to_valid_utf8())

#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

// ---- config ----

struct MCP_SERVER_CONFIG
{
    std::string name;                        // [a-z0-9_]{1,24}; used in tool names, log files, work dir
    bool enabled = true;
    std::vector<std::string> command;        // argv, run directly (never through a shell)
    std::map<std::string, std::string> env;  // extra environment for the child (on top of a small safe set)
    std::string cwd;                         // empty = a private work directory under the profile folder
    // Which of the server's tools olli may see. EMPTY = none (the safe default:
    // a small model does badly with dozens of tools, and every tool a
    // third-party server offers is something it could be talked into calling).
    // "*" as the only entry = all of them.
    std::vector<std::string> allow_tools;
    std::string prefix;                      // put in front of every exposed tool name; default "mcp_<name>_"
    int startup_timeout_seconds = 120;       // first npx/uvx run may have to download the package
};

struct MCP_BRIDGE_CONFIG
{
    int max_result_chars = 12000;            // one tool answer is cut off here (keeps the model's context sane)
    int call_timeout_seconds = 120;          // olli's own limit is 300 s; answer before that
    std::vector<MCP_SERVER_CONFIG> servers;  // in name order
    std::vector<std::string> warnings;       // things in the file that were skipped or corrected
};

// The file written the first time (an empty server list, with the numbers above).
nlohmann::json default_config_json();

// Never throws: a wrongly typed or invalid piece is skipped (or falls back to
// its default) and described in warnings.
MCP_BRIDGE_CONFIG parse_config(const nlohmann::json& config);

// ---- text hygiene ----

// Invalid UTF-8 bytes -> U+FFFD (so the result can always be put in JSON).
std::string to_valid_utf8(const std::string& text);

// At most max_bytes, never cutting a UTF-8 character in half. 'truncated' says whether anything was cut.
std::string truncate_utf8(const std::string& text, size_t max_bytes, bool& truncated);

// Lower-case, anything outside a-z0-9 becomes '_', runs of '_' collapse, no leading/trailing '_'.
// "get-Current.Time" -> "get_current_time".
std::string sanitize_identifier(const std::string& text);

// Server-written prose headed for the model's tool list: control characters and
// line breaks become spaces, runs of spaces collapse, cut at max_bytes (with "...").
// Invisible characters are removed too (zero-width, bidi overrides, the Unicode "tag"
// block, C1 controls) - they can hide instructions from a human reading the list.
std::string clean_description(const std::string& text, size_t max_bytes);

// True if the JSON text nests arrays/objects deeper than max_depth. nlohmann parses
// iteratively but copies, compares and dumps RECURSIVELY, so one 8 MB line of '[' could
// crash the bridge with a stack overflow. Check before parse().
bool json_too_deep(const std::string& json_text, int max_depth);

// The same check for a value that is already parsed (arguments in a call from olli). Walks it with an explicit stack,
// NOT recursion - copying, comparing or printing a deeply nested json value recurses once per level and overflows the
// stack long before the walk here would.
bool json_value_too_deep(const nlohmann::json& value, int max_depth);

// ---- tools ----

struct EXPOSED_TOOL
{
    std::string exposed_name;   // what olli's model sees, e.g. "mcp_time_get_current_time"
    std::string server;         // which configured server owns it
    std::string real_name;      // the name to send the server in tools/call
    nlohmann::json definition;  // {"name","description","parameters"} - one entry of a `register` message
};

bool tool_is_allowed(const MCP_SERVER_CONFIG& server, const std::string& real_name);

// One tool from a server's tools/list -> an EXPOSED_TOOL. false (and why_not set) when it can't be
// offered safely: no name, name too long once prefixed, schema too big.
bool convert_tool(const MCP_SERVER_CONFIG& server, const nlohmann::json& mcp_tool, EXPOSED_TOOL& out, std::string& why_not);

// ---- results ----

struct FLATTENED_RESULT
{
    std::string text;
    bool is_error = false;
};

// An MCP tools/call "result" object -> text. Text blocks are joined; an image or audio block becomes a
// short "[image ... not shown]" note; a resource link becomes "name - uri"; structuredContent is used if
// there is no text. Always valid UTF-8, at most max_chars bytes (a notice is appended when cut).
FLATTENED_RESULT flatten_call_result(const nlohmann::json& result, size_t max_chars);
