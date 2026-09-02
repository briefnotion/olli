#ifndef SIDETRACK_H
#define SIDETRACK_H

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

        void run_second_guess(IO_WORKER_CLASS& io_worker, ollama_system& main_instance, COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, CLASS_SYSTEM* system);
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
        void check(IO_WORKER_CLASS& io_worker, ollama_system& main_instance, COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, CLASS_SYSTEM* system);

        // Test/debug hook - skips IDLE_WAIT_TIMER_FOR_CONSOLIDATION and runs
        // the consolidation pass immediately. The real check()-driven path
        // (main.cpp) never needs this; it always gets to stage 2 eventually
        // via the timer. See test_consolidation.cpp.
        void force_consolidation(ollama_system& main_instance);
};

#endif