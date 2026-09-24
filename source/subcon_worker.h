#ifndef SUBCON_WORKER_H
#define SUBCON_WORKER_H

#include "olla.h" // OLLAMA_SYSTEM_PROPERTIES
#include "threading.h"

/**
 * SUBCON_WORKER_CLASS
 *
 * Skeleton only, for now - thread lifecycle proven out, no actual
 * reasoning/state yet. See IDEAS.md's "The subconscious" section for the
 * full design this is meant to grow into.
 *
 * Modeled on TOOL_WORKER_CLASS (tool_worker.h), not IO_WORKER_CLASS -
 * chosen deliberately: IO_WORKER_CLASS's single combined exchange() exists
 * because keyboard/audio genuinely need a full COMMS snapshot handed over
 * every tick; this worker isn't going to need that shape. It's going to
 * own and drive its own ollama_system, thinking independently on its own
 * schedule - the main thread will just call a handful of small, purpose-
 * specific methods when it actually has something to hand over or ask
 * for, same as TOOL_WORKER_CLASS's put_pending_call()/get_pending_result()
 * - not tied to a fixed per-tick shape. None of those methods exist yet;
 * deliberately holding off until the actual design (what goes in, what
 * comes out) is agreed, rather than guessing at names/signatures now.
 */
class SUBCON_WORKER_CLASS
{
    private:
        THREADING_INFO THREAD_CONTROL;

        bool RUN = false;

        // Runs on the background thread - only ever invoked internally,
        // via thread_start()'s own lambda (same access rights as any
        // other member-function code, since the lambda is defined inside
        // one), never meant to be called directly from outside.
        void thread_main();

    public:
        // Set by main.cpp, once, before thread_start() - a one-way copy of
        // chat's own PROPS (same model/host/port, so the subconscious is
        // always talking to the same LLM the real conversation is). Just a
        // stepping stone, not canon: thread_main() copies this into its own
        // local ollama_system, then applies its own real overrides on top
        // (LOAD_SAVE_HISTORY_ON_DISK, OLLI_DIRECTORY, use_thinking - see
        // that function's own comments for why).
        OLLAMA_SYSTEM_PROPERTIES PROPS;

        void thread_start();
        void thread_stop();
};

#endif
