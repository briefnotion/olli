#ifndef olla_h
#define olla_h

#include <string>
#include <vector>
#include <iostream>
#include <chrono>
#include <ctime>
#include <atomic>
#include <filesystem>
#include <queue>
#include <mutex>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "system.h"
#include "helper_olli.h"
#include "tools_helper.h"
#include "stringthings.h"

using json = nlohmann::json;

// --- FORWARD DECLARATION ---

class ollama_system;
class AUDIO_CONTROL_CLASS; // for the text-to-speech output hook; see g_audio_control below
class CLASS_SYSTEM; // see the CLASS_SYSTEM* parameter's comment on TOOL_BASE::check() (tools.h)
/**
 * @brief Structure representing a single chat message in the history.
 */
struct Message {
    std::string role = "";    
    std::string content = "";
    std::string tool_call_id = ""; 
    int consolidation_level = 0;

    // Helper to convert a Message object to a JSON object
    // This allows nlohmann::json to handle the struct automatically
    friend void to_json(json& j, const Message& m) {
        j = json{
            {"role", m.role},
            {"content", m.content},
            {"tool_call_id", m.tool_call_id},
            {"consolidation_level", m.consolidation_level}
        };
    }

    // Helper to convert a JSON object back into a Message object
    friend void from_json(const json& j, Message& m) {
        j.at("role").get_to(m.role);
        j.at("content").get_to(m.content);
        j.at("tool_call_id").get_to(m.tool_call_id);
        j.at("consolidation_level").get_to(m.consolidation_level);
    }
};

struct ToolCall {
    std::string id;
    std::string name;
    json arguments;
};

struct ChatResult {
    std::string response;
    std::string thinking;
    std::vector<ToolCall> tool_calls;
    bool complete = false;
};

/**
 * Simple data struct to hold metrics.
 */
struct ollama_system_status {
    bool is_active = false;
    // The interrupt flag. Set this to true from another thread to stop 'send'
    std::atomic<bool> interrupt_signal{false};

    int total_messages = 0;
    std::map<int, int> level_counts;
    int max_level = 0;

    // Helper for easy printing
    std::string to_string() const {
        if (total_messages == 0) return "History Empty";
        std::stringstream ss;
        ss << "Total: " << total_messages << " | ";
        for (int i = 0; i <= max_level; ++i) {
            auto it = level_counts.find(i);
            int count = (it != level_counts.end()) ? it->second : 0;
            ss << "L" << i << ": " << count;
            if (i < max_level) ss << ", ";
        }
        return ss.str();
    }
};

// Every TOOL_* class (register_tool/handle_tool/monitor_tool per tool) now
// lives in tools.h/tools.cpp, kept separate from the chat engine itself.
#include "tools.h"

class OLLAMA_SYSTEM_PROPERTIES
{
    public:
        std::filesystem::path OLLI_DIRECTORY = "";
        std::filesystem::path path_output = "";
        std::filesystem::path path_history = "";

        std::string model = "qwen3:8b";
        std::string host = "localhost";
        int port = 11434;
        int num_ctx = 8192; // size
        bool stream_output = true;
        bool use_thinking = true;

        // Sent to Ollama as "keep_alive" on every request - how long it
        // keeps the model loaded in memory after a request. -1 means
        // indefinitely (until explicitly unloaded or the Ollama server
        // restarts), overriding Ollama's own default of unloading after 5
        // minutes of inactivity. A positive value is seconds.
        int keep_alive_seconds = -1;

        string web_search_api_key = "Enter_API_key_for_serpapi.com";
        std::string hue_ip = "127.0.0.1";
        std::string hue_key = "Enter_Hue_Bridge_API_Key";
        std::string hue_path = "scenes.json";

        // parameters
        int consolitation_starts_starts_at = 20;
        int consolitation_sizes = 10;

        // test
        //int consolitation_starts_starts_at = 4;
        //int consolitation_sizes = 2;

        bool LOAD_SAVE_HISTORY_ON_DISK = true;

        // Hard ceiling on how many tool calls handle_instance_tools() will
        // actually execute within one turn (see ollama_system::
        // tool_calls_this_turn's comment for what a "turn" means and why
        // this exists). A real multi-step request ("set a timer AND tell me
        // the time") legitimately needs more than one - this just bounds
        // the worst case (a runaway loop) to a small, fixed number instead
        // of unbounded.
        int max_tool_calls_per_turn = 4;
};

class ollama_system {
    private:

        std::vector<std::unique_ptr<ollama_system>> background_tasks;
        
        json tools = json::array();
        std::chrono::steady_clock::time_point last_consolidation = std::chrono::steady_clock::now();

        // One instance of every TOOL_* class, populated once in the
        // constructor. Driven uniformly (configure/register_tool/check/
        // monitor_tool - see the TOOL_BASE comment in tools.h) instead of as
        // fixed named members, so adding a tool never touches this class.
        std::vector<std::unique_ptr<TOOL_BASE>> tools_list;

        size_t PREVIOUS_HISTORY_SIZE = 0;

        // Shared by both call sources handle_instance_tools() drains (the
        // model's own last_received.tool_calls, and the system-injected
        // pending_tool_calls) - same cap check, same tools_list dispatch,
        // same "unrecognized name" fallback either way. Keyboard_Input_Enabled
        // is only ever actually toggled for a model-issued run_automation_task
        // call (see its own TODO comment in tools.cpp) - always true for
        // anything from pending_tool_calls, which never contains that name.
        // 'system' is just forwarded to each tool's check() - see that
        // parameter's own comment on TOOL_BASE::check() (tools.h).
        void dispatch_tool_call(const ToolCall& tc, CLASS_SYSTEM* system, bool& Keyboard_Input_Enabled);

        // Called from send() right as a new real "user" turn starts (see
        // tool_calls_this_turn's comment just below, and send()'s own
        // comment, for what counts as a turn boundary). Erases every 'tool'
        // message and every non-protected (consolidation_level >= 0)
        // 'system' message still in history - the raw tool results and
        // DIRECTOR_NOTE prompts (integrate_tool_result()) the previous turn
        // generated. Safe to drop unconditionally at this point: whatever
        // they were for is already captured in the assistant's own reply
        // (which this leaves untouched, along with every 'user' message).
        // The protected persona message (consolidation_level -1) is exempt
        // by the level check, same as consolidation's own bucketing.
        void prune_turn_scaffolding();

        bool saveHistoryToJson(std::filesystem::path filepath);
        bool loadHistoryFromJson(std::filesystem::path filepath);

        void history_write(std::string Directory);

    public:

        ollama_system();

        // 'system' is the one real CLASS_SYSTEM for the process, or nullptr
        // where there isn't one to give (see TOOL_BASE::check()'s comment in
        // tools.h) - just forwarded down to dispatch_tool_call() for each
        // tool's check()/monitor_tool().
        void handle_instance_tools(CLASS_SYSTEM* system, bool& Keyboard_Input_Enabled);

        // Explicit flush to disk, e.g. right after consolidation commits or on shutdown.
        void save_history();

        // Tells Ollama to unload PROPS.model from memory immediately
        // (sends keep_alive: 0). Ollama tracks loaded models by name, not
        // by connection, so one call covers every ollama_system instance
        // in the process that shares this model - no need to call it per
        // instance. Meant for shutdown, alongside save_history().
        void unload_model();

        std::string OLLAMA_OPENING =
                //"You are a but helpful assistant with access to tools. "
                //"1. For delayed requests, use set_timer. Always summarize the "
                //"user's intent in the 'reminder' field (e.g., 'Turn off "
                //"the living room fan'). "
                //"2. When the system sends a message starting "
                //"with 'SYSTEM NOTIFICATION: Timer Expired', look at the "
                //"associated reminder and immediately call the relevant tool "
                //"to fulfill that action without asking for further confirmation.";

                "You are a cyberpunk-flavored assistant with access to tools. "
                "Talk like a street-level netrunner - chrome, corpo, the sprawl, "
                "jacking in, flatlining. "
                "Your responses will be short and sweet. "
                "No need to be polite.";


        OLLAMA_SYSTEM_PROPERTIES PROPS;
        TOOL_PERMISSIONS_CLASS TOOL_PERMISSIONS;

        // Use Atomics for thread safety to avoid data races
        std::thread chat_thread;
        std::atomic<bool> is_processing{false};
        std::atomic<bool> running{true};

        // System-injected tool calls - not from the model's own
        // last_received.tool_calls, but constructed elsewhere and queued
        // for real execution regardless (currently: TOOL_REMOTE::
        // monitor_tool() building one from an `action` field on an
        // incoming `event`, see tools/PROTOCOL.md - a remote timer's
        // pre-authored on-expire action). Drained in
        // handle_instance_tools() through the same tools_list dispatch and
        // tool_calls_this_turn cap as every other call, just via a
        // separate queue - last_received gets reset at the top of every
        // send() call, so anything sitting in it could be silently dropped
        // if a new turn started before this got a chance to run; this
        // queue has no such lifecycle tied to it.
        std::queue<ToolCall> pending_tool_calls;

        // Counts tool calls executed since the last real "user"-role
        // send() - reset to 0 there (see send()'s body), incremented in
        // handle_instance_tools() for every call actually dispatched.
        // Guards against a specific real bug: a tool's result gets
        // "explained" back to the model via integrate_tool_result()'s
        // DIRECTOR_NOTE, asking it to acknowledge the result in persona -
        // but the model can respond to that with *another* tool call
        // instead of text, which itself gets dispatched and DIRECTOR_NOTE'd
        // the same way, with nothing to stop the chain (seen firsthand: 6
        // identical set_timer calls in a row for one request, no assistant
        // text anywhere in between). "Turn" here means one real user
        // message (or, for a background task-runner instance, one scripted
        // command - each is fed in via send(..., "user") too, so the same
        // reset applies there without special-casing it).
        int tool_calls_this_turn = 0;

        ollama_system_status status;    

        std::vector<Message> history;
        ChatResult last_received;

        std::string tts_buffer = "";

        // Live streaming output for the screen - appended to from send()'s
        // streaming loop (chat_thread), drained by OUTPUT_CLASS::get_response()
        // (see user_io.h/.cpp), which runs on whichever thread calls it once
        // per tick. Same cross-thread shape as tts_buffer above, but for the
        // screen instead of speech. Both guarded by output_buffer_mutex below
        // - lock it around any access to either.
        std::string response_buffer = "";
        std::string thinking_buffer = "";

        // Status/debug text ("[System] Tool call received: ...", "[Delegator]
        // ...", etc.) that used to go straight to std::cout from tool
        // handlers and other ollama_system methods. Same cross-thread shape
        // and same output_buffer_mutex as the two buffers above - appended
        // to from wherever a tool handler already has `chat`/`this` in
        // scope (no new parameter needed), drained into
        // OUTPUT_CLASS::system_message by get_response().
        std::string log_buffer = "";

        // Appends to log_buffer under output_buffer_mutex - the one place
        // that lock actually gets taken for it, so call sites (tool
        // handlers, etc.) don't each need their own lock_guard.
        void log(const std::string& text);

        // Pulls every background task's response_buffer/thinking_buffer/
        // log_buffer into output, same as OUTPUT_CLASS::get_response() does
        // for this instance's own - background_tasks stays private, this is
        // the one place that reaches into it for that purpose. One level
        // only: a background task's own background_tasks (if it ever had
        // any) isn't recursed into, matching process()'s own iteration below.
        void pull_background_output(OUTPUT_CLASS& output);

        // Creates a new background ollama_system instance, owned by this one
        // (added to background_tasks so it's picked up by process()'s own
        // background-task loop and pull_background_output() same as any
        // other), and returns a reference to it. Lets a tool's handle_tool
        // spawn a sub-conversation without needing direct access to the
        // private background_tasks vector - currently only TOOL_TASK_RUNNER
        // does this, to run an automation sequence without blocking chat.
        ollama_system& spawn_background_task();

        // Adds a tool to tools_list after construction - lets a remote tool
        // (source/remote_tools.h) join mid-session once its registration
        // handshake completes, without exposing tools_list itself. Takes
        // effect on the next send() call, which rebuilds the tools array
        // sent to Ollama from tools_list fresh every time (see the comment
        // there) rather than once at open().
        void register_remote_tool(std::unique_ptr<TOOL_BASE> tool);

        void open();
        void open(OLLAMA_SYSTEM_PROPERTIES Properties);
        
        string gather_history();
        void integrate_tool_result(std::string Special_Instruction, const std::string& raw_result);
        void send(const std::string& user_input, const std::string& role = "user");
        void send_tool_result(const std::string& tool_call_id, const std::string& result);

        // Helper to reset the signal
        void stop();

        // Safe shutdown: stops any in-flight response and joins chat_thread
        // before setting running = false, so main()'s ollama_system chat;
        // never gets destroyed with chat_thread still joinable (that's a
        // std::terminate crash). Same body jump_input()'s "bye"/"quit"
        // handling already used inline - factored out here so
        // KEYBOARD_INPUT's Ctrl+C handling (main.cpp) can trigger the exact
        // same safe path instead of duplicating it or setting running =
        // false directly.
        void request_exit();

        /**
         * Updates the internal status struct by scanning the history.
         */
        void update_status();

        void write_to_tts();

        bool jump_input(CLASS_SYSTEM& System);
        bool input(CLASS_SYSTEM& System);

        // 'system' is nullable and just threaded down to handle_instance_tools()
        // and each tool's monitor_tool() - see TOOL_BASE::check()'s comment in
        // tools.h for why (main-thread-only call sites pass the real
        // CLASS_SYSTEM&, e.g. main.cpp/jump_input(); sidetrack.cpp's
        // background-thread call passes nullptr instead).
        void process(CLASS_SYSTEM* system, bool& Keyboard_Input_Enabled);

};

// A single shared mutex protecting every ollama_system::history vector.
//
// This MUST be an 'inline' variable (C++17), not 'static'. A 'static'
// definition in a header gives every translation unit that includes olla.h
// its OWN private mutex, so the main/chat thread and the sidetrack thread
// would lock unrelated mutexes and never actually exclude one another —
// a silent data race on the history while consolidation runs. 'inline'
// yields one shared instance across all translation units with no
// "multiple definition" linker error.
inline std::mutex history_mutex;

// Same reasoning as history_mutex above (must be 'inline', not 'static'),
// covering ollama_system::response_buffer and ::thinking_buffer instead of
// ::history. Kept separate from history_mutex so locking one doesn't block
// the other - they're touched by different code for different reasons.
inline std::mutex output_buffer_mutex;

// The one text-to-speech output for the whole process. There's only ever
// one physical speaker, so every ollama_system instance (the main chat,
// sidetrack's second-guess review, task-runner/delegator sub-instances)
// routes spoken output through the same AUDIO_CONTROL_CLASS rather than
// each needing to be wired up individually. Set once in main.cpp; null
// until then, in which case write_to_tts() just no-ops.
inline AUDIO_CONTROL_CLASS* g_audio_control = nullptr;

#endif