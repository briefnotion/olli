#ifndef SIDETRACK_H
#define SIDETRACK_H

#if 0 // SIDETRACK_CLASS is being rewritten from scratch - kept here for
      // reference, not compiled. See TODO.md's sidetrack rewrite entry.

#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>

//#include <nlohmann/json.hpp>
#include "olla.h"
#include "fled_time.h"
#include "threading.h"

class IO_WORKER_CLASS; // for create()'s audio param below - see its own comment

bool consolidate(std::vector<Message>& chat_history, OLLAMA_SYSTEM_PROPERTIES& config);

// Set by the main thread (main.cpp), read by SIDETRACK_CLASS::check() (also
// main thread) which mirrors them into the atomics below for the sidetrack
// background thread to see. These two bools are plain, non-atomic - a known,
// currently-accepted data race (main.cpp writes them, check() reads/clears
// them, both on the main thread in practice, but nothing enforces that).
struct SIDETRACK_SIGNALS
{
    bool INTERUPT_SIGNAL = false;      // user typed/spoke something - see SIDETRACK_CLASS::check
    bool CHAT_FINISHED_SIGNAL = false; // the main chat's response just finished

    // The other direction: set by check() (main thread) when its context-
    // clear routine (CLEAR_CONTEXT_STAGE) just cleared history, read and
    // cleared by main.cpp right after calling check() - which then closes
    // OUTPUT_CLASS's chat log (see OUTPUT_CLASS::close_chat_log() in
    // user_io.cpp). sidetrack.cpp deliberately doesn't touch OUTPUT_CLASS
    // directly - it only ever reaches main.cpp/pull_output() - so this flag
    // is the hop instead, same shape as CHAT_FINISHED_SIGNAL just reversed.
    bool CONTEXT_CLEARED_SIGNAL = false;
};

/**
 * SIDETRACK_CLASS
 *
 * Runs a background thread (thread_main, on its own std::thread) that does
 * three things while the main chat is otherwise idle:
 *   ROUTINE 1: consolidation   - compress old history (see consolidate() above)
 *   ROUTINE 2: "second guess"  - after a turn finishes, have a separate
 *                                 ollama_system instance (SIDETRACK_CHAT_INSTANCE)
 *                                 review it and speak a follow-up if it has
 *                                 something worth adding.
 *   ROUTINE 3: context clear   - after a long stretch of no user activity,
 *                                 wipe the real conversation history (but
 *                                 not protected/foundational messages - see
 *                                 CLEAR_CONTEXT_STAGE below).
 * Only one ROUTINE runs at a time; ROUTINE 0 means idle.
 *
 * IMPORTANT - two threads are involved, and they split responsibility for
 * each routine's state machine:
 *   - thread_main() runs on the sidetrack background thread. It drives the
 *     ROUTINE and *_PROCESSING_STAGE transitions and does the actual LLM work
 *     (consolidate(), SIDETRACK_CHAT_INSTANCE.send()).
 *   - check() runs on the MAIN thread (called once per main-loop tick in
 *     main.cpp). It's the only place that's safe to read or write
 *     main_instance.history directly, so every stage that needs to touch
 *     the real conversation is handled there instead of in thread_main().
 * temp_chat_history is the handoff buffer between them: check() copies
 * main_instance.history into it (stage 1->2), thread_main() operates on
 * that copy, and check() copies the result back (stage 3->4).
 *
 * GOTCHA worth remembering: SIDETRACK_CHAT_INSTANCE.send() is a direct,
 * blocking call made from thread_main() - not launched on its own thread.
 * While it's in flight (which can be many seconds for a local model), the
 * sidetrack thread is stuck inside that call and CANNOT re-check anything,
 * including SIGNALS.INTERUPT_SIGNAL at the top of the ROUTINE==2 block. That
 * check only ever catches an interrupt that arrives *before* generation
 * starts. To actually abort an in-flight review, check() (main thread) calls
 * SIDETRACK_CHAT_INSTANCE.stop() directly the moment it sees the interrupt -
 * see check()'s definition for why that's the only thread that can do it.
 */
class SIDETRACK_CLASS
{
    private:
        double INTERVAL = 500;  //ms
        THREADING_INFO  THREAD_CONTROL;  // Controls: update_frame_thread()
        std::filesystem::path settings_path;

        TIMED_IS_READY  RESUME_TIMER;

        // The dedicated ollama_system used ONLY for the second-guess review
        // (ROUTINE 2). NOT used for consolidation - consolidate() creates
        // its own separate, local ollama_system ("consolidate_client") each
        // time it runs. This instance's own .history is unrelated to the
        // real conversation except that it gets seeded with a copy of it
        // (see SECOND_GUESS_PROCESSING_STAGE == 1/2 below) as context for
        // the review prompt.
        ollama_system SIDETRACK_CHAT_INSTANCE;

        // SIDETRACK_CHAT_INSTANCE's own private tools_list - never the real
        // CLASS_SYSTEM's, and not shared with anything else. Populated once
        // in create() (see olla.h's process() comment for why tools_list
        // moved to a reference parameter rather than living on ollama_system
        // itself) - real, always-valid, so send()/process() here never need
        // a nullable/missing tools_list, only a nullable/missing CLASS_SYSTEM*
        // (which this thread genuinely has none of - see thread_main()'s
        // nullptr call sites below).
        std::vector<std::unique_ptr<TOOL_BASE>> tools_list;

        // How long the system must be idle (no interrupts) before
        // consolidation is allowed to run. Armed once at thread start in
        // thread_main() and re-armed on every interrupt (see INTERUPT below)
        // and after every consolidation pass.
        double IDLE_WAIT_TIME_FOR_CONSOLIDATION = 1.0 * 60.0 * 1000.0; // ms
        //double IDLE_WAIT_TIME_FOR_CONSOLIDATION = 10.0 * 1000.0; // ms (handy for fast local testing)
        TIMED_IS_READY IDLE_WAIT_TIMER_FOR_CONSOLIDATION;

        //double IDLE_WAIT_TIME = 1000.0; // ms
        //TIMED_IS_READY IDLE_WAIT_TIMER;

        // How long the system must be idle (no interrupts) before the
        // conversation history is auto-cleared. Armed once at thread start
        // in thread_main() and re-armed on every interrupt (same as
        // IDLE_WAIT_TIMER_FOR_CONSOLIDATION above).
        double IDLE_WAIT_TIME_FOR_CONTEXT_CLEAR = 30.0 * 60.0 * 1000.0; // ms (30 minutes)
        //double IDLE_WAIT_TIME_FOR_CONTEXT_CLEAR = 10.0 * 1000.0; // ms (handy for fast local testing)
        TIMED_IS_READY IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR;

        // Cross-thread handoff buffer for whichever routine is active. See
        // the class-level comment above for the full stage 1<->2, 3<->4
        // choreography between thread_main() (sidetrack thread) and check()
        // (main thread) that reads/writes this.
        std::vector<Message> temp_chat_history;

        // Which routine is currently running; only one at a time.
        int ROUTINE = 0; // 0 = idle, 1 = consolidation, 2 = second-guess review, 3 = context clear

        // Routine 1: Consolidation Routine
        int CHAT_HISTORY_PROCESSING_STAGE = 0;
        // 0 = idle
        // 1 = thread_main() has requested the current history; waiting for
        //     check() (main thread) to copy main_instance.history into
        //     temp_chat_history and advance this to 2.
        // 2 = thread_main() runs consolidate() on temp_chat_history. Moves
        //     on to 3 if it changed anything, straight to 4 (no-op) if not.
        // 3 = consolidation produced a result; waiting for check() to copy
        //     temp_chat_history back into main_instance.history (but only
        //     if no interrupt arrived in the meantime - see check()) and
        //     save it to disk, then advance this to 4.
        // 4 = thread_main() resets this to 0 and ROUTINE to 0 (idle again).

        // Routine 2: Second_Guess Routine
        int SECOND_GUESS_PROCESSING_STAGE = 0;
        // 0 = idle; thread_main() advances this to 1 to kick off a review.
        // 1 = thread_main() has requested the current history; waiting for
        //     check() to copy main_instance.history into temp_chat_history
        //     and advance this to 2.
        // 2 = thread_main() seeds SIDETRACK_CHAT_INSTANCE with that history
        //     and calls .send() with the "Internal Monologue" review prompt
        //     - a direct, BLOCKING call (see the GOTCHA in the class-level
        //     comment). Once it returns, this becomes 4 if the model said
        //     "DONE" (nothing worth adding), otherwise 3.
        // 3 = reached either from stage 2 (review had something to say) or
        //     forced here directly by an interrupt (see thread_main() and
        //     check()). Waiting for check() to decide whether to fold
        //     SIDETRACK_CHAT_INSTANCE's reply into main_instance.history
        //     (only if this cycle actually produced one - see check()) and
        //     advance this to 4.
        // 4 = thread_main() clears SIDETRACK_CHAT_INSTANCE's history and
        //     resets this to 0 and ROUTINE to 0.

        // Routine 3: Clear-Context Routine
        int CLEAR_CONTEXT_STAGE = 0;
        // 0 = idle; thread_main() advances this to 1 to kick off a clear.
        // 1 = thread_main() has requested the clear; waiting for check()
        //     (main thread) to wipe main_instance.history - preserving any
        //     protected (consolidation_level < 0) messages, e.g. the
        //     persona/opening prompt, same as consolidate() already leaves
        //     those untouched - and save the result to disk, then advance
        //     this to 2. Skipped (left as a no-op) if an interrupt arrived
        //     in the meantime - see check().
        // 2 = thread_main() resets ROUTINE to 0 (idle again) and advances
        //     this to 3 - NOT back to 0, so a still-idle system doesn't
        //     clear an already-empty history again every time the idle
        //     timer comes back around.
        // 3 = finished; there's nothing left to clear until the user does
        //     something. Stays here - the ROUTINE==0 dispatch in
        //     thread_main() won't re-arm ROUTINE 3 while this is 3 - until
        //     an interrupt resets this back to 0 alongside
        //     IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR (see thread_main()).

        // Communication variables. These are the atomic (thread-safe)
        // counterparts of SIGNALS above; check() (main thread) mirrors
        // SIGNALS into these, and thread_main() (sidetrack thread) reads
        // them - this is the actual thread-safe hop, SIGNALS itself isn't.
        std::atomic<bool> PROCESSING{false};   // set/cleared around thread_main()'s per-tick work; not currently read anywhere
        std::atomic<bool> INTERUPT{false};     // consumed once at the top of each thread_main() tick: re-arms the consolidation idle timer
        std::atomic<bool> CHAT_FINISHED{false}; // consumed once when ROUTINE==0: triggers ROUTINE 2 (second-guess review)


    public:

        SIDETRACK_SIGNALS SIGNALS;

        bool RUN = false;

        SIDETRACK_CLASS();

        // Copies the given properties (model, host, port, thinking, etc)
        // into SIDETRACK_CHAT_INSTANCE.PROPS. audio is currently unused -
        // COMMS::audio is gone and sidetrack has no speech path of its own
        // right now (see olla.h's note near output_buffer_mutex); kept as a
        // parameter for whenever sidetrack's rework adds one back, probably
        // via IO_WORKER_CLASS::speak() directly. Called once from main.cpp
        // after the main chat instance is configured.
        void create(OLLAMA_SYSTEM_PROPERTIES Ollama_Properties, IO_WORKER_CLASS* audio);

        // Runs on its own background thread (started by thread_start).
        // Drives both routines' state machines; see the class-level comment.
        void thread_main();

        void thread_start();
        void thread_stop();

        // Runs on the MAIN thread - called once per main-loop tick in
        // main.cpp, right after chat.process(). Handles every stage that
        // needs to touch main_instance.history, plus consuming SIGNALS.
        void check(ollama_system& main_instance);

        // Pulls SIDETRACK_CHAT_INSTANCE's response_buffer/thinking_buffer/
        // log_buffer into output - same shape as ollama_system's own
        // pull_background_output(), just for the one instance sidetrack
        // owns privately instead of a container of them. One difference: a
        // lone "DONE" response_buffer (the review found nothing worth
        // adding) is routed into chat_thinking instead of chat_response -
        // see the .cpp for why.
        void pull_output(OUTPUT_CLASS& output);
};

#endif // SIDETRACK_CLASS rewrite - see the #if 0 above

#include "olla.h"
#include "fled_time.h"

/**
 * SIDETRACK_CLASS (rewrite in progress)
 *
 * No background thread this time - check() is meant to be called once per
 * main-loop tick (main.cpp) and stay non-blocking. See sidetrack.cpp for
 * the actual state machines; this header only declares what that skeleton
 * currently references.
 */
class SIDETRACK_CLASS
{
    private:
        // Dedicated ollama_system used for sidetrack's own LLM calls
        // (consolidation, second-guess review, etc.) - fully separate from
        // main_instance so this work never blocks, streams into, or
        // otherwise interferes with the real conversation. Its COMMS
        // (comms is no longer a member of ollama_system - see the
        // COMMS-ownership move in olla.h) and its tools_list are built
        // locally, on the fly, wherever they're actually needed (create(),
        // and later whatever function actually calls .send()/.process()) -
        // same pattern tools.cpp's task-runner automation instance uses -
        // rather than kept as members here.
        ollama_system SIDETRACK_CHAT_INSTANCE;

        int consolidation_stage = 0;
        int context_clear_stage = 0;
        int second_guess_stage = 0;

        // history.size() as of the last run_second_guess() tick - self-
        // contained rather than sharing check()'s own PREVIOUS_HISTORY_SIZE
        // (per "all code in run_second_guess()"). Its one job: notice a new
        // turn landed while stage sat at 100 (a previous review already
        // finished) so there's a way back to 0 to review the new one too -
        // without this, second_guess_stage had no path off 100 at all once
        // it got there, ever again.
        size_t SECOND_GUESS_PREVIOUS_HISTORY_SIZE = 0;

        // Counts consecutive second-guess cycles chained off its own prior
        // follow-up (each one is itself an "assistant" reply, same as a
        // real one - see the reset check's own comment) rather than off a
        // genuine new user turn. Resets to 0 the moment a real user message
        // shows up. Capped at SECOND_GUESS_MAX_CHAIN so a review that keeps
        // finding more to say about its own last follow-up can't chain
        // forever - deliberately not preventing the chain outright (second-
        // guessing its own second-guess is fine), just bounding it.
        int second_guess_chain_count = 0;
        static constexpr int SECOND_GUESS_MAX_CHAIN = 10;

        // How long to wait after a turn finishes before running the
        // second-guess review - short, since this isn't waiting for real
        // idle like consolidation/context-clear, just a brief grace period.
        double SECOND_GUESS_WAIT_TIME = 2.0 * 1000.0; // ms
        TIMED_IS_READY_SIMPLE SECOND_GUESS_WAIT_TIMER;

        // handle_instance_tools()'s own gate for a model-issued
        // run_automation_task call (see its own comment in olla.h) - kept
        // entirely separate from the real io_worker.key_input.PROPS.ENABLED
        // so that if the review ever did trigger an automation, it couldn't
        // actually disable the real user's keyboard. Nothing else reads
        // this, so isolating it here is a no-op in practice, just a safety
        // margin.
        std::atomic<bool> second_guess_keyboard_enabled{false};

        // history.size() as of the end of the last check() tick - a
        // real submission, tool result, or anything else that grows
        // history counts as activity too, not just an in-flight
        // interrupt (main_instance.status.interrupt_signal only reflects
        // that, not "did anyone do anything").
        size_t PREVIOUS_HISTORY_SIZE = 0;

        double IDLE_WAIT_TIME_FOR_CONSOLIDATION = 1.0 * 60.0 * 1000.0; // ms
        TIMED_IS_READY_SIMPLE IDLE_WAIT_TIMER_FOR_CONSOLIDATION;

        double IDLE_WAIT_TIME_FOR_CONTEXT_CLEAR = 30.0 * 60.0 * 1000.0; // ms
        TIMED_IS_READY_SIMPLE IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR;

        // Runs every PERSISTENT_CHECK_INTERVAL regardless of activity -
        // see persistent_time_checks()'s own comment.
        double PERSISTENT_CHECK_INTERVAL = 10.0 * 60.0 * 1000.0; // ms (10 minutes)
        TIMED_IS_READY_SIMPLE PERSISTENT_CHECK_TIMER;
        size_t MAX_CONTEXT_SIZE = 200; // messages - tune as needed

        void run_second_guess(ollama_system& main_instance, COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, CLASS_SYSTEM* system);
        void persistent_time_checks(ollama_system& main_instance);
        void run_consolidation(ollama_system& main_instance);
        void run_clear_context(ollama_system& main_instance);

    public:
        // Properties: copied from the main chat instance's own PROPS (model,
        // host, port, etc.) so sidetrack's LLM calls hit the same Ollama
        // server/model - see ollama_system::open()'s two-argument overload,
        // which this uses and which forces LOAD_SAVE_HISTORY_ON_DISK off
        // (this instance's history is scratch, never the real history.json).
        void create(OLLAMA_SYSTEM_PROPERTIES Properties);
        void check(ollama_system& main_instance, COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, CLASS_SYSTEM* system);

        // Test/debug hook - skips IDLE_WAIT_TIMER_FOR_CONSOLIDATION and runs
        // the consolidation pass immediately. The real check()-driven path
        // (main.cpp) never needs this; it always gets to stage 2 eventually
        // via the timer. See test_consolidation.cpp.
        void force_consolidation(ollama_system& main_instance);
};

#endif