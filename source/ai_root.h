#ifndef ai_root_h
#define ai_root_h

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

class ai_root;
class CLASS_SYSTEM; // see the CLASS_SYSTEM* parameter's comment on TOOL_BASE::check() (tools.h)
class OUTPUT_CLASS; // for pull_background_output() below - see user_io.h (now reached via IO_WORKER_CLASS, not CLASS_SYSTEM)
class TOOL_WORKER_CLASS; // nullable pointer, same reasoning as CLASS_SYSTEM* above - see process()'s own comment below

// ToolCall, Confidence, TokenProb, Message, YesNoMaybe, YES_NO_MAYBE_FORMAT,
// ChatResult and ai_status live in ai_systems.h - shared by every
// AI backend, not specific to this one.
#include "ai_systems.h"


// Every TOOL_* class (register_tool/handle_tool/monitor_tool per tool) now
// lives in tools.h/tools.cpp, kept separate from the chat engine itself.
#include "tools.h"

class AI_PROPERTIES
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
        // allowed to write into comms (send(), ai_root.cpp) - stream_output
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
        // actually execute within one turn (see ai_root::
        // tool_calls_this_turn's comment for what a "turn" means and why
        // this exists). A real multi-step request ("set a timer AND tell me
        // the time") legitimately needs more than one - this just bounds
        // the worst case (a runaway loop) to a small, fixed number instead
        // of unbounded.
        int max_tool_calls_per_turn = 4;
};

// ai_root: everything about a conversation that doesn't depend on WHICH AI
// is answering - history, consolidation hooks, tool dispatch, result
// claiming, threads, saving/loading, and the head and tail of send().
// A concrete backend (ollama_system in ollama_system.h/.cpp; others to
// follow) derives from this and provides only the parts that talk to its own
// API. The rest of olli holds and passes around ai_root, never a backend.
//
// WHAT A BACKEND MUST PROVIDE
//   backend_generate()  - the middle of send(): turn history + tool defs into
//                         its API's request, make the call (streaming or
//                         not), and report the result. See its comment below.
//   make_sibling()      - a fresh, empty instance of the same backend (used
//                         for background task-runner/delegate instances).
//   unload_model()      - optional; the default does nothing.
// A backend must NOT contain history, consolidation, tool-dispatch or
// result-claiming code - if it starts to, that code belongs here instead.
class ai_root {
    protected:

        // Rebuilt by the backend on every call - see ollama_system.cpp.
        // Protected (not private) only so a backend can reach it.
        json tools = json::array();

        // The backend step of send(), called between its shared head (state
        // reset, user message pushed to history) and shared tail (history
        // push, confidence, yes/no/maybe, debug log). It must:
        //  - read history (under history_mutex) and tool defs, build and send
        //    its API's request, honouring PROPS and 'response_format';
        //  - stream text/thinking into comms.INPUT_FROM_LLM/INPUT_FROM_THINKING
        //    while PROPS.stream_output/stream_thinking allow it;
        //  - poll status.interrupt_signal and stop early if set;
        //  - fill last_received.tool_calls / .response_tokens / .thinking_tokens
        //    / .complete (leave .response/.thinking alone - send() sets those);
        //  - return the full reply text in accumulated_content and the full
        //    thinking text in accumulated_thinking.
        // Leave response_tokens empty if the API has no per-token
        // probabilities - confidence then simply stays absent.
        virtual void backend_generate(TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const json& response_format, std::string& accumulated_content, std::string& accumulated_thinking) = 0;

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
        std::vector<std::pair<std::unique_ptr<ai_root>, std::unique_ptr<COMMS>>> background_tasks;

        std::chrono::steady_clock::time_point last_consolidation = std::chrono::steady_clock::now();

        size_t PREVIOUS_HISTORY_SIZE = 0;

        // Shared by both call sources handle_instance_tools() drains (the
        // model's own last_received.tool_calls, and the system-injected
        // pending_tool_calls) - same cap check, same dispatch, same
        // "unrecognized name" fallback either way.
        // 'system' is nullable, forwarded to each tool's check() - see
        // TOOL_BASE::check()'s comment (tools.h). 'tools_list' is the
        // caller's own built-ins, tried first via each tool's own check().
        // 'tool_worker' is the caller's own, also nullable - if nothing in
        // tools_list claims tc.name, it's queued there via
        // put_pending_call() instead when non-null; "not recognized" only if
        // neither claims it.
        void dispatch_tool_call(IO_WORKER_CLASS& io_worker, const ToolCall& tc, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms);

        bool saveHistoryToJson(std::filesystem::path filepath);
        bool loadHistoryFromJson(std::filesystem::path filepath);

        void history_write(std::string Directory);

    public:

        ai_root();
        virtual ~ai_root() = default;

        // A fresh, empty instance of the same backend as this one - used by
        // spawn_background_task() so a background instance talks to the same
        // AI its parent does, without ai_root needing to know any backend.
        virtual std::unique_ptr<ai_root> make_sibling() const = 0;

        // Short human-readable tag identifying this instance in
        // debug_full_history.txt (DEBUG_LOG_CLASS::log_message()/log_event(),
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
        // tools.h) - forwarded down to dispatch_tool_call(). 'tools_list'/
        // 'tool_worker' are the caller's own, also nullable/reference the
        // same way - see process()'s own comment below.
        void handle_instance_tools(IO_WORKER_CLASS& io_worker, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms);

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

        // Puts message into the reserved slot for a negative consolidation
        // level, in memory only - caller decides whether/when to
        // save_history() after. The slot is fixed by the level: -1 is entry 0
        // (the persona), -2 is entry 1, -3 is entry 2. If that entry already
        // has this level it is replaced, otherwise the message is inserted
        // there. message.consolidation_level is set to level here.
        void set_protected_slot(int level, Message message);

        // Asks the backend to free PROPS.model from memory immediately, for
        // a backend that holds one locally (Ollama - see ollama_system.cpp;
        // it tracks loaded models by name, not by connection, so one call
        // covers every instance sharing the model). The default does
        // nothing, which is right for a hosted API. Meant for shutdown,
        // alongside save_history().
        virtual void unload_model() {}

        std::string AI_OPENING =
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


        AI_PROPERTIES PROPS;

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

        // Ids of tool calls this instance has dispatched to tool_worker but
        // not yet claimed a result for - pushed in dispatch_tool_call()
        // right before put_pending_call() (tools.cpp), erased once
        // process()'s own PART 5 (ai_root.cpp) successfully claims that id's
        // result. Every ai_root instance shares the same tool_worker
        // (main chat, a task-runner's own instance, a delegate's own
        // instance), so without this, whichever instance's process() tick
        // happens to run next would blindly claim whatever result is
        // sitting oldest in tool_worker's queue - including one that
        // belongs to a completely different instance. This is what lets
        // each instance claim only its own.
        std::vector<std::string> outstanding_tool_call_ids;

        // Every call id this instance has EVER dispatched, for its whole
        // life - unlike outstanding_tool_call_ids above, never pruned once
        // a result comes in. A remote tool's own standing state (e.g.
        // clock's timers, tools/clock/clock.cpp) can outlive the call that
        // created it by a long time, and stamps that call's own id back
        // onto whatever unsolicited TOOL_EVENT it eventually produces
        // (origin_id, remote_tools.h) - checked against this list in
        // process()'s own PART 5 (ai_root.cpp) so an instance can tell "this
        // event traces back to something I asked for" from "this is a
        // leftover from somewhere else, or purely ambient", instead of
        // narrating every event as if it just answered whatever's
        // currently being discussed. Unbounded growth here is the same
        // accepted tradeoff active_timers' own "never pruned" comment
        // (clock.cpp) already makes for the same reason - nothing in
        // normal use accumulates fast enough for it to matter, and a
        // short-lived background task/delegate instance never grows this
        // far regardless.
        std::vector<std::string> owned_tool_call_ids;

        // True for every instance today. It used to be set to false by the
        // in-olli subcon's own subcon_llm (SUBCON_WORKER_CLASS, removed
        // 2026-10-31 - subcon is the standalone tools/subcon now), and is
        // kept as an opt-out for any future instance that shares
        // tool_worker but shouldn't compete for events. Guards process()'s
        // own PART 5 event-draining loop (ai_root.cpp) specifically, not the
        // result-claiming loop right above it in the same PART 5 - results are already safe to share
        // (outstanding_tool_call_ids above, id-based, live-verified 2026-
        // 09-22), but pending_events has no call_id to correlate against at
        // all, so it's drained blindly by whichever instance's process()
        // tick happens to run next (see the PART 5 comment on this in
        // ai_root.cpp). The old subcon sharing tool_worker (2026-09-27, for full
        // tool access) would otherwise have been put in that same blind
        // competition, meaning it could silently absorb a real
        // presence/timer event that was meant to reach the real chat, into
        // its own isolated conversation where it'd never be seen. The flag
        // let it keep full, safe call/result access while structurally
        // opting out of the one part of tool_worker sharing that's still
        // genuinely unsafe for multiple consumers.
        bool drains_events = true;

        ai_status status;

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

        // Creates a new background ai_root instance, owned by this one
        // (added to background_tasks so it's picked up by process()'s own
        // background-task loop and pull_background_output() same as any
        // other), and returns a reference to it plus its own newly-created
        // COMMS (see background_tasks' own comment for why this is a pair
        // now, not just the instance). Lets a tool's handle_tool spawn a
        // sub-conversation without needing direct access to the private
        // background_tasks vector - currently only TOOL_TASK_RUNNER does
        // this, to run an automation sequence without blocking chat.
        std::pair<ai_root&, COMMS&> spawn_background_task();

        // 'tools_list' is the caller's own built-ins - each one's
        // configure() runs here, once, before anything else touches it. A
        // remote tool's own registration (tool_worker.h/.cpp) doesn't route
        // through open() at all, so this never needed tool_worker.
        void open(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list);
        void open(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, AI_PROPERTIES Properties);

        string gather_history();
        void integrate_tool_result(TOOL_WORKER_CLASS* tool_worker, COMMS& comms, std::string Special_Instruction, const std::string& raw_result);

        // response_format: optional Ollama "format" constraint (a JSON
        // Schema object, e.g. {"type":"object","properties":{...}}) -
        // forces the model's reply into that exact shape via constrained
        // decoding server-side, rather than asking for a particular phrase
        // and hoping it lands somewhere findable in free text (the DONE-
        // marker bug, sidetrack.cpp, was exactly that class of problem).
        // Default-empty (json()'s own default, a null value - json::empty()
        // is true for that) means "no constraint," so every existing call
        // site is unaffected. Still just a plain string in last_received.
        // response either way - json::parse() it yourself to read the
        // structured reply back out.
        void send(TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const std::string& role, const json& response_format = json());
        void send_tool_result(const std::string& tool_call_id, const std::string& result);

        // Helper to reset the signal
        void stop();

        // Safe shutdown: stops any in-flight response and joins chat_thread
        // before setting running = false, so main()'s ai_root chat;
        // never gets destroyed with chat_thread still joinable (that's a
        // std::terminate crash). Same body jump_input()'s "bye"/"quit"
        // handling already used inline - factored out here so
        // KEYBOARD_INPUT's Ctrl+C handling (main.cpp) can trigger the exact
        // same safe path instead of duplicating it or setting running =
        // false directly.
        void request_exit();

        // Same crash request_exit() guards against, for background_tasks
        // instead of this instance itself - a task-runner automation or
        // consult_expert delegation still mid-response when the whole
        // program exits used to leave its own chat_thread joinable with
        // nothing left to join it (process()'s own PART 2 only runs while
        // the main loop is still calling process() at all - once running
        // is false, it never gets another tick), so it stayed joinable
        // right up until background_tasks itself got destroyed - a real,
        // live std::terminate/SIGABRT crash, confirmed via a real backtrace.
        // Called from main.cpp's shutdown sequence, before this instance's
        // own destruction.
        void shutdown_background_tasks();

        /**
         * Updates the internal status struct by scanning the history.
         */
        void update_status();

        bool jump_input(COMMS& comms);
        bool input(COMMS& comms, TOOL_WORKER_CLASS* tool_worker);

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
        //
        // 'tool_worker' is nullable the same way 'system' is, for the same
        // reason - there's only ever one real TOOL_WORKER_CLASS (main.cpp's
        // own, owning the actual remote-tool connections). Most sub-agent
        // instances pass nullptr, but not all: sidetrack's second-guess
        // review and a running TOOL_TASK_RUNNER/TOOL_DELEGATOR script's own
        // sub-instance deliberately get the real one too, same reasoning as
        // their own real tools_list - so a remote-tool call made from
        // inside either has somewhere to actually go, not just built-ins.
        void process(IO_WORKER_CLASS& io_worker, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms);

};

// Pushes one instance of every built-in TOOL_* class onto tools_list - the
// same 3 tools every ai_root instance used to get for free from its
// own constructor before tools_list moved off this class (see process()'s
// comment above). Call once per real tools_list that needs them: main.cpp's
// real one, SIDETRACK_CLASS's own, each task-runner automation instance's
// own local one.
void populate_default_tools(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list);

// A single shared mutex protecting every ai_root::history vector.
//
// This MUST be an 'inline' variable (C++17), not 'static'. A 'static'
// definition in a header gives every translation unit that includes ai_root.h
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

// A mutex used to live here (tools_list_mutex) guarding main.cpp's one real
// tools_list against a live crash: send() (ai_root.cpp) rebuilding the tools
// schema by iterating tools_list on one thread, while process() (ai_root.cpp)
// concurrently erased a disconnected TOOL_REMOTE from the same vector on
// the main thread - caught with gdb as a null unique_ptr dereference
// mid-iteration. That whole race is gone now, not just guarded: a
// TOOL_REMOTE never lives in tools_list at all anymore, only inside
// TOOL_WORKER_CLASS's own thread-local tools_list (tool_worker.cpp), reached
// exclusively through its own INTERUPTED/PROCESSING rendezvous - the same
// job this mutex used to do, done by construction instead of a lock.

// The text-to-speech output hook used to live here as a single process-wide
// global (g_audio_control), then later as a per-instance COMMS::audio
// pointer. Neither exists anymore - IO_WORKER_CLASS::thread_main() now
// copies the main chat's own comms.INPUT_FROM_LLM (already relayed in by
// exchange() into its comms_buffer) into its own comms_stt_tts and speaks
// that via display_with_tts() (io_worker.cpp); other instances (background
// tasks, sidetrack) currently have no speech path.

#endif