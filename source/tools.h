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
// takes/returns a reference (or, for CLASS_SYSTEM, a pointer), never needs
// the complete type. ollama_system/ToolCall's complete types live in olla.h,
// which includes this header (ollama_system holds tools_list, a vector of
// these); CLASS_SYSTEM's lives in system.h, deliberately not included here -
// see the CLASS_SYSTEM* parameter's own comment on TOOL_BASE::check() below.
class ollama_system;
class CLASS_SYSTEM;
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
//     that only makes sense when permitted checks chat.TOOL_PERMISSIONS
//     itself, same as register_tool().
// A no-op override (not an omitted method) is how a tool opts out of any of
// these - keep new tools matching this shape rather than adding one-off
// signatures.
class TOOL_BASE
{
    public:
        virtual ~TOOL_BASE() = default;

        virtual void configure(ollama_system& chat) = 0;
        virtual void register_tool(ollama_system& chat, json& tools) = 0;

        // 'system' is the one real CLASS_SYSTEM for the whole process
        // (constructed once in main.cpp) - reachable here, unlike chat.PROPS,
        // for things like the current user's identity (CLASS_SYSTEM, once
        // it grows one), the real keyboard/display/audio, etc. Nullable: the
        // entire tool-dispatch spine these two run through (process() ->
        // handle_instance_tools() -> dispatch_tool_call(), olla.h/.cpp) is
        // main-thread-only except for one caller - sidetrack.cpp's
        // background thread drives its own throwaway SIDETRACK_CHAT_INSTANCE
        // through process() directly, with no real CLASS_SYSTEM of its own
        // and no business touching the real one from a second thread - that
        // call site passes nullptr rather than a dangling/cross-thread
        // reference. A tool that needs 'system' must null-check it; every
        // tool below still ignores it entirely for now (leaves the parameter
        // unnamed), since nothing in tools_list has a use for it yet.
        virtual bool check(ollama_system& chat, CLASS_SYSTEM* system, const ToolCall& tc) = 0;
        virtual void monitor_tool(ollama_system& chat, CLASS_SYSTEM* system) = 0;

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
        bool check(ollama_system& chat, CLASS_SYSTEM* system, const ToolCall& tc) override;
        void monitor_tool(ollama_system& chat, CLASS_SYSTEM* system) override;
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
        bool check(ollama_system& chat, CLASS_SYSTEM* system, const ToolCall& tc) override;
        void monitor_tool(ollama_system& chat, CLASS_SYSTEM* system) override;
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
        bool check(ollama_system& chat, CLASS_SYSTEM* system, const ToolCall& tc) override;

        // No periodic work needed - no automation-loop equivalent of a
        // permission-gated background poll (e.g. TOOL_WEB_SEARCH, if it
        // ever grew one) exists here (yet). A no-op, but still called
        // every process() tick like every other tool's.
        void monitor_tool(ollama_system& chat, CLASS_SYSTEM* system) override;
};

#endif
