#ifndef SIDETRACK_H
#define SIDETRACK_H

#include <iostream>
#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>

//#include <nlohmann/json.hpp>
#include "olla.h"
#include "fled_time.h"
#include "threading.h"

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
        // 4 = thread_main() clears SIDETRACK_CHAT_INSTANCE's history/
        //     tts_buffer and resets this to 0 and ROUTINE to 0.

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
        // into SIDETRACK_CHAT_INSTANCE.PROPS. Called once from main.cpp
        // after the main chat instance is configured.
        void create(OLLAMA_SYSTEM_PROPERTIES Ollama_Properties);

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

#endif