#ifndef tools_h
#define tools_h

#include <string>
#include <map>
#include <vector>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "tools_helper.h"

using json = nlohmann::json;

// Forward declarations only - TOOL_BASE and every TOOL_* method below just
// takes/returns a reference, never needs the complete type. Both complete
// types live in olla.h, which includes this header (ollama_system holds
// tools_list, a vector of these).
class ollama_system;
struct ToolCall;

// Appends one tool definition (name/description/JSON-schema parameters) to
// the tools array sent to Ollama. Shared by every TOOL_*::register_tool
// below rather than each building the {"type": "function", ...} envelope
// itself.
void add_tool(json& tools, const std::string& name, const std::string& description, json parameters);

// Common interface every TOOL_* class implements, so ollama_system can store
// them uniformly as tools_list (std::vector<std::unique_ptr<TOOL_BASE>>,
// see olla.h) instead of as fixed named members, and drive all of them the
// same way regardless of concrete type:
//   - configure(): once per instance, during open(), before register_tool().
//     Pulls whatever setup a tool needs from chat.PROPS (API keys,
//     credentials, directories).
//   - register_tool(): adds this tool's JSON definition(s) to `tools`, but
//     only if chat.TOOL_PERMISSIONS grants it - each override does its own
//     permission check internally, since the uniform loop calling this has
//     no per-tool knowledge itself.
//   - check(): true if tc.name belongs to this tool. If so, handles the call
//     (permission-gated, same pattern as register_tool) and returns true
//     either way; false, untouched, if the name isn't this tool's - the
//     dispatcher loop moves on to the next tool.
//   - monitor_tool(): called every process() tick, for every tool - a tool
//     that only makes sense when permitted (e.g. TOOL_HUE polling the
//     bridge) checks chat.TOOL_PERMISSIONS itself, same as register_tool().
// A no-op override (not an omitted method) is how a tool opts out of any of
// these - keep new tools matching this shape rather than adding one-off
// signatures.
class TOOL_BASE
{
    public:
        virtual ~TOOL_BASE() = default;

        virtual void configure(ollama_system& chat) = 0;
        virtual void register_tool(ollama_system& chat, json& tools) = 0;
        virtual bool check(ollama_system& chat, const ToolCall& tc) = 0;
        virtual void monitor_tool(ollama_system& chat) = 0;

        // Unlike the four above, not something each tool author has to
        // consciously decide - it's a connection-lifecycle question that's
        // permanently true for anything running in-process. Only
        // TOOL_REMOTE (remote_tools.h) overrides it, once its socket
        // closes; every hardcoded tool just inherits this default rather
        // than needing a meaningless override. Checked once per
        // process() tick - see ollama_system::process() in olla.cpp - to
        // drop a dead tool from tools_list.
        virtual bool is_alive() const { return true; }
};

class TOOL_SET_THINKING_MODE : public TOOL_BASE
{
    private:
        void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);

    public:
        void configure(ollama_system& chat) override;
        void register_tool(ollama_system& chat, json& tools) override;
        bool check(ollama_system& chat, const ToolCall& tc) override;
        void monitor_tool(ollama_system& chat) override;
};

class TOOL_HUE : public TOOL_BASE
{
    private:
        HUE_LIGHT_CLASS hue;

        void set_credentials(const std::string& ip, const std::string& key, const std::string& path);
        void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);

    public:
        // Sends a direct system-level nudge forcing a tool call, for when the
        // model just confirms a lighting request in text instead of acting on it.
        void refresh_system_prompt(ollama_system& chat);

        void configure(ollama_system& chat) override;
        void register_tool(ollama_system& chat, json& tools) override;
        bool check(ollama_system& chat, const ToolCall& tc) override;
        void monitor_tool(ollama_system& chat) override;
};

class TOOL_WEB_SEARCH : public TOOL_BASE
{
    private:

        std::string strip_html_tags(std::string html);
        std::string make_clickable(const std::string& url, const std::string& text);

        std::string perform_actual_search(const std::string& query);
        std::string fetch_url_content(const std::string& url);

        void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);

        static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
            static_cast<std::string*>(userp)->append(static_cast<const char*>(contents), size * nmemb);
            return size * nmemb;
        }

    public:
        std::string apiKey = "Enter_API_key_for_serpapi.com";

        void configure(ollama_system& chat) override;
        void register_tool(ollama_system& chat, json& tools) override;
        bool check(ollama_system& chat, const ToolCall& tc) override;
        void monitor_tool(ollama_system& chat) override;
};

// Disabled: recursive sub-agent delegation (a chat instance spawning a
// secondary ollama_system to handle an isolated sub-task). Kept commented
// out as a design reference, not wired into ollama_system - see the
// matching commented-out block in tools.cpp for the implementation. Left
// out of the TOOL_BASE hierarchy since it isn't part of tools_list.
//
//class TOOL_DELEGATOR {
//    private:
//
//    public:
//        // Testing switch: Turn this off to prevent the AI from spawning sub-agents
//        bool enable_delegation = true;
//
//        void register_tool(json& tools);
//        void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
//};

class TOOL_TASK_RUNNER : public TOOL_BASE
{
    private:
        TASK_SIMPLE_MANAGER task_manager;

        bool iequals(const std::string& a, const std::string& b);

        // Matches args["intent_phrase"] against task_manager's list, then spawns
        // a background ollama_system (chat.spawn_background_task()) and drives
        // it through the matched task's whole command sequence synchronously -
        // not a single instruction handed back to the model.
        void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);

    public:
        std::filesystem::path OLLI_DIRECTORY;

        void configure(ollama_system& chat) override;
        void register_tool(ollama_system& chat, json& tools) override;
        bool check(ollama_system& chat, const ToolCall& tc) override;

        // No periodic work needed - no automation-loop equivalent of
        // TOOL_HUE's monitor_tool exists (yet). A no-op, but still called
        // every process() tick like every other tool's.
        void monitor_tool(ollama_system& chat) override;
};

#endif
