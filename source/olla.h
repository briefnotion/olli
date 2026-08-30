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
#include "comms.h"

using json = nlohmann::json;

// --- FORWARD DECLARATION ---

class ollama_system;
class CLASS_SYSTEM; // see the CLASS_SYSTEM* parameter's comment on TOOL_BASE::check() (tools.h)
class OUTPUT_CLASS; // for pull_background_output() below - see user_io.h (now reached via IO_WORKER_CLASS, not CLASS_SYSTEM)

// Declared before Message - Message::tool_calls (below) holds a vector of
// these, and needs the type (and its own JSON (de)serialization) already
// available.
struct ToolCall {
    std::string id;
    std::string name;
    json arguments;

    // .value() throughout, not .at() - unlike Message's own to_json/
    // from_json below, there's no legacy history.json to stay compatible
    // with here (this type is new), but a malformed/unexpected entry
    // should still degrade to empty fields rather than throw and take an
    // entire Message (and everything after it in a from_json array walk)
    // down with it.
    friend void to_json(json& j, const ToolCall& tc) {
        j = json{
            {"id", tc.id},
            {"name", tc.name},
            {"arguments", tc.arguments}
        };
    }

    friend void from_json(const json& j, ToolCall& tc) {
        tc.id = j.value("id", "");
        tc.name = j.value("name", "");
        tc.arguments = j.value("arguments", json::object());
    }
};

/**
 * @brief Structure representing a single chat message in the history.
 */
struct Message {
    std::string role = "";
    std::string content = "";
    std::string tool_call_id = "";
    int consolidation_level = 0;

    // The assistant's own tool_calls request for this message, if any -
    // see ollama_system::send()'s comment on why this is recorded now
    // (it used to not exist anywhere, even transiently: only the tool's
    // *result* and a persona-narrated version of it ever made it into
    // history, never the actual structured call the model itself made).
    // Empty for every message that isn't an assistant turn that called a
    // tool.
    std::vector<ToolCall> tool_calls;

    // Helper to convert a Message object to a JSON object
    // This allows nlohmann::json to handle the struct automatically
    friend void to_json(json& j, const Message& m) {
        j = json{
            {"role", m.role},
            {"content", m.content},
            {"tool_call_id", m.tool_call_id},
            {"consolidation_level", m.consolidation_level},
            {"tool_calls", m.tool_calls}
        };
    }

    // Helper to convert a JSON object back into a Message object
    friend void from_json(const json& j, Message& m) {
        j.at("role").get_to(m.role);
        j.at("content").get_to(m.content);
        j.at("tool_call_id").get_to(m.tool_call_id);
        j.at("consolidation_level").get_to(m.consolidation_level);
        // .value(), not .at() - every history.json saved before this field
        // existed is missing this key entirely; .at() would throw loading
        // any of them. This is the one field here that must tolerate
        // being absent.
        m.tool_calls = j.value("tool_calls", json::array()).get<std::vector<ToolCall>>();
    }
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
        // Independent per-channel gates on what a streamed response is
        // allowed to write into comms (send(), olla.cpp) - stream_output
        // for comms.INPUT_FROM_LLM (content), stream_thinking for
        // comms.INPUT_FROM_THINKING. Either one alone is enough to make
        // send() use Ollama's streaming API (chunks needed for either
        // channel to show up live at all) - the other channel's chunks
        // still get accumulated into last_received.response/.thinking
        // either way, just not forwarded to comms for display while the
        // call is in flight. Both default true, matching stream_output's
        // old sole meaning (both channels visible) for every existing
        // caller.
        bool stream_output = true;
        bool stream_thinking = true;
        bool use_thinking = true;

        // Discourages the model from reproducing a recent sequence of
        // tokens verbatim. Ollama's own default when this option is
        // omitted entirely (as it was before this field existed) is
        // ~1.1. Set explicitly higher here to test whether it helps with
        // a real observed failure mode: the model locking onto a short
        // phrase and repeating it verbatim across unrelated follow-up
        // turns, confirmed via a raw wire-level capture of an actual
        // session - see the wire-log findings, no other fix tried that
        // day changed this behavior. Untested as of adding this - next
        // thing to actually verify.
        double repeat_penalty = 1.3;

        // Sent to Ollama as "keep_alive" on every request - how long it
        // keeps the model loaded in memory after a request. -1 means
        // indefinitely (until explicitly unloaded or the Ollama server
        // restarts), overriding Ollama's own default of unloading after 5
        // minutes of inactivity. A positive value is seconds.
        int keep_alive_seconds = -1;

        string web_search_api_key = "Enter_API_key_for_serpapi.com";

        // Consolidation tuning (SIDETRACK_CLASS::run_consolidation,
        // sidetrack.cpp): consolitation_starts_starts_at is the minimum
        // number of messages kept untouched (raw) at a given consolidation
        // level; consolitation_sizes is the minimum overflow past that
        // before a squash triggers. Both even - run_consolidation() rounds
        // the actual squashed count down to the nearest even number anyway
        // (level 0's raw messages strictly alternate user/assistant, so an
        // odd slice would end on a lone unanswered turn), so odd values
        // here would just mean the real trigger point is one lower than it
        // looks. 30/12 keeps roughly 15 conversational turns at full
        // fidelity before consolidating - comfortably inside num_ctx above
        // even with several summary levels and tool schemas also present -
        // and squashes in six-turn batches once it does, big enough to be
        // worth an LLM call without asking one summarization pass to cover
        // too much at once.
        int consolitation_starts_starts_at = 30;
        int consolitation_sizes = 12;

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

        // Each background task gets its own real, persistent COMMS - paired
        // right alongside it rather than shared/thrown-away, so its output
        // is never silently discarded (see spawn_background_task()'s
        // comment for why this isn't just a plain vector<unique_ptr<...>>
        // anymore). unique_ptr<COMMS>, not a bare COMMS: COMMS holds a
        // std::atomic member (close_chat_log_requested), which has no copy
        // or move constructor - a bare COMMS can't live inside a vector
        // that needs to move elements around (growth, erase()). unique_ptr
        // sidesteps that and, just as importantly, keeps every COMMS's
        // address stable across reallocation - spawn_background_task()
        // hands out a COMMS& into this vector, which a by-value COMMS
        // would leave dangling the next time the vector grows.
        std::vector<std::pair<std::unique_ptr<ollama_system>, std::unique_ptr<COMMS>>> background_tasks;
        
        json tools = json::array();
        std::chrono::steady_clock::time_point last_consolidation = std::chrono::steady_clock::now();

        size_t PREVIOUS_HISTORY_SIZE = 0;

        // Shared by both call sources handle_instance_tools() drains (the
        // model's own last_received.tool_calls, and the system-injected
        // pending_tool_calls) - same cap check, same tools_list dispatch,
        // same "unrecognized name" fallback either way. Keyboard_Input_Enabled
        // is only ever actually toggled for a model-issued run_automation_task
        // call (see its own TODO comment in tools.cpp) - always true for
        // anything from pending_tool_calls, which never contains that name.
        // 'system' is just forwarded to each tool's check() - see that
        // parameter's own comment on TOOL_BASE::check() (tools.h). 'tools_list'
        // is the caller's own - see its comment on process() below for why
        // this is a reference parameter now, not a member.
        void dispatch_tool_call(const ToolCall& tc, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, std::atomic<bool>& Keyboard_Input_Enabled);

        bool saveHistoryToJson(std::filesystem::path filepath);
        bool loadHistoryFromJson(std::filesystem::path filepath);

        void history_write(std::string Directory);

    public:

        ollama_system();

        // Short human-readable tag identifying this instance in
        // debug_full_history.txt (debug_log_message()/debug_log_instance_event(),
        // helper_olli.h) - main chat, sidetrack's review, a task-runner
        // automation instance, etc. all funnel through the same send()/
        // completion code and share one log file, so without this every
        // line would be indistinguishable. Set once by whoever constructs
        // the instance (main.cpp, sidetrack.cpp, tools.cpp) - defaults to
        // "unlabeled" rather than something plausible-sounding like "chat",
        // so a creation site that forgets to set this is obvious in the log
        // instead of silently mislabeled.
        std::string debug_label = "unlabeled";

        // 'system' is the one real CLASS_SYSTEM for the process, or nullptr
        // where there isn't one to give (see TOOL_BASE::check()'s comment in
        // tools.h) - just forwarded down to dispatch_tool_call() for each
        // tool's check()/monitor_tool(). 'tools_list' is the caller's own -
        // see process()'s comment below for why this moved to a reference
        // parameter instead of living on ollama_system.
        void handle_instance_tools(CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, std::atomic<bool>& Keyboard_Input_Enabled);

        // Explicit flush to disk, e.g. right after consolidation commits or on shutdown.
        void save_history();

        // Wipes history entirely, in memory only - caller decides whether/
        // when to save_history() after. No exceptions - even protected
        // (consolidation_level < 0) messages, e.g. the persona/opening
        // prompt, are dropped.
        void clear_history();

        // Wipes history down to only protected (consolidation_level < 0)
        // messages, in memory only - caller decides whether/when to
        // save_history() after. Keeps the persona/opening prompt while
        // dropping everything since. Same wipe sidetrack.cpp's
        // run_clear_context()/persistent_time_checks() used to do inline.
        void clear_history_keep_protected();

        // Installs new_history as history wholesale, in memory only -
        // caller decides whether/when to save_history() after. For
        // replacing history with something built elsewhere (e.g.
        // consolidation's rebuilt vector, sidetrack.cpp) rather than
        // filtering the existing one in place, which clear_history()/
        // clear_history_keep_protected() don't support.
        void replace_history(std::vector<Message> new_history);

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
        // reset applies there without special-casing it) - also reset at
        // the top of TOOL_REMOTE::monitor_tool()'s "event" handling
        // (remote_tools.cpp), since a pushed event (a presence transition,
        // a timer expiring) is likewise a fresh, externally-initiated topic
        // rather than a continuation of an existing chain. Without that
        // second reset point, an idle session accumulates this count across
        // every unrelated background event it ever receives (nothing but a
        // real user message ever zeroed it), eventually tripping the cap
        // and then silently dropping every future event's action for the
        // rest of the session - observed for real with a flapping presence
        // sensor.
        int tool_calls_this_turn = 0;

        ollama_system_status status;    

        std::vector<Message> history;
        ChatResult last_received;

        // Thin forwarder to comms.log() - kept here so the many existing
        // `chat.log(...)` call sites (tool handlers, etc.) didn't all need
        // to become `chat.comms.log(...)`.
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
        // other), and returns a reference to it plus its own newly-created
        // COMMS (see background_tasks' own comment for why this is a pair
        // now, not just the instance). Lets a tool's handle_tool spawn a
        // sub-conversation without needing direct access to the private
        // background_tasks vector - currently only TOOL_TASK_RUNNER does
        // this, to run an automation sequence without blocking chat.
        std::pair<ollama_system&, COMMS&> spawn_background_task();

        // 'tools_list' is now owned by the caller, not this instance (see
        // process()'s comment below for why) - each of these three tools_list-
        // touching entry points takes it as a reference. A remote tool joins
        // simply by tools_list.push_back()-ing directly into the caller's own
        // vector (source/remote_tools.h's registration handshake, main.cpp);
        // that takes effect on the next send() call, which rebuilds the
        // tools array sent to Ollama from tools_list fresh every time (see
        // the comment there) rather than once at open().
        void open(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list);
        void open(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, OLLAMA_SYSTEM_PROPERTIES Properties);

        string gather_history();
        void integrate_tool_result(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, std::string Special_Instruction, const std::string& raw_result);
        void send(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, const std::string& role);
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

        bool jump_input(COMMS& comms);
        bool input(COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list);

        // 'system' is nullable and just threaded down to handle_instance_tools()
        // and each tool's monitor_tool() - see TOOL_BASE::check()'s comment in
        // tools.h for why (main-thread-only call sites pass the real
        // CLASS_SYSTEM&, e.g. main.cpp; sidetrack.cpp's background-thread
        // call passes nullptr instead).
        //
        // 'tools_list' used to live on this class (one instance of every
        // TOOL_* class, populated in the constructor - see git history if
        // curious). Moved to a reference parameter instead: 'system' being
        // nullable for sidetrack.cpp's background thread and the task-runner
        // automation instance (tools.cpp) was fine when it only gated a
        // tool's *optional* access to real audio/keyboard/identity - but
        // once tools_list itself lived on CLASS_SYSTEM, that same nullptr
        // would mean "no tools at all" for those two instances, which
        // actually need real tools to function. Decoupling tools_list from
        // CLASS_SYSTEM keeps 'system' nullable for what it's always been for,
        // while every caller (main.cpp's real one, SIDETRACK_CLASS's own
        // private one, the automation instance's own local one) supplies a
        // real, always-valid tools_list of its own - never null, because a
        // reference can't be.
        void process(CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, std::atomic<bool>& Keyboard_Input_Enabled);

};

// Pushes one instance of every built-in TOOL_* class onto tools_list - the
// same 3 tools every ollama_system instance used to get for free from its
// own constructor before tools_list moved off this class (see process()'s
// comment above). Call once per real tools_list that needs them: main.cpp's
// real one, SIDETRACK_CLASS's own, each task-runner automation instance's
// own local one.
void populate_default_tools(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list);

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

// output_buffer_mutex (same 'inline' reasoning as history_mutex above,
// covering every instance's comms.INPUT_FROM_LLM/INPUT_FROM_THINKING/
// INPUT_FROM_SYSTEM instead of ::history) now lives in comms.h, included
// above - kept separate from history_mutex there too, so locking one doesn't block
// the other.

// The text-to-speech output hook used to live here as a single process-wide
// global (g_audio_control), then later as a per-instance COMMS::audio
// pointer. Neither exists anymore - IO_WORKER_CLASS::exchange() now fans
// the main chat's own comms.INPUT_FROM_LLM out into its private
// comms_buffer_audio (io_worker.cpp) and speaks that directly; other
// instances (background tasks, sidetrack) currently have no speech path.

#endif