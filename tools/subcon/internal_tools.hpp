// Subcon's own tools - the ones it runs itself rather than through a
// connected remote tool: web_search, fetch_website_content, get_clock_time.
//
// Copied (deliberately not shared) from source/tools.cpp's TOOL_WEB_SEARCH
// and the clock - tools/ never shares headers with source/ (../PROTOCOL.md,
// "Repo / build layout"). Subcon does not get olli's other built-ins
// (run_automation_task and friends). Everything here is blocking and pure
// (no shared state), so it runs on job_worker's thread.

#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <string>

struct INTERNAL_TOOL_RESULT {
    bool ok = false;
    std::string text; // the result, or an "Error: ..." description when !ok
};

// {name, description, parameters} for each internal tool, the same plain
// shape the connection server hands back for registered remote tools.
nlohmann::json internal_tool_defs();
bool is_internal_tool(const std::string& name);

// Runs one internal tool. api_key is the serpapi key for web_search.
// 'abort' cuts a slow request short (shutdown).
INTERNAL_TOOL_RESULT run_internal_tool(const std::string& name, const nlohmann::json& arguments,
                                       const std::string& api_key, const std::atomic<bool>& abort);

// Cuts text to at most max_bytes WITHOUT splitting a multi-byte UTF-8
// character, appending a marker if anything was cut. The old in-olli copy
// (tools.cpp, substr(0, 4000)) could cut mid-character, and the invalid
// UTF-8 then made the next request unserialisable (SUBCON.md, "Known
// limitations"); this is the fix in the copy that lives here.
std::string truncate_utf8(const std::string& text, size_t max_bytes);
