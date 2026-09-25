#ifndef SUBCON_WORKER_H
#define SUBCON_WORKER_H

#include "olla.h" // OLLAMA_SYSTEM_PROPERTIES
#include "comms.h" // COMMS
#include "threading.h"

#include <atomic>

/**
 * SUBCON_WORKER_CLASS
 *
 * Skeleton mostly - thread lifecycle proven out, its own isolated
 * ollama_system built and live-verified. See IDEAS.md's "The subconscious"
 * section for the full design this is meant to grow into.
 *
 * Modeled on TOOL_WORKER_CLASS (tool_worker.h) for its own thread lifecycle,
 * but now also has one piece modeled on IO_WORKER_CLASS instead: exchange()
 * hands over a COMMS snapshot once per main-loop tick (main.cpp), same
 * shape as IO_WORKER_CLASS's own - a deliberate change from this class's
 * earlier "no fixed per-tick shape" plan, once subcon actually had a first
 * real reason to need one: reading the real chat's own COMMS::busy_count()
 * to know whether the system is currently idle. Everything else TOOL_
 * WORKER_CLASS-shaped (put_pending_call()-style purpose-specific methods)
 * is still open for whatever else subcon eventually needs to hand over or
 * ask for - exchange() covers the "snapshot of what's currently going on"
 * need specifically, not every future need.
 */
class SUBCON_WORKER_CLASS
{
    private:
        THREADING_INFO THREAD_CONTROL;

        bool RUN = false;

        // Same INTERUPTED/PROCESSING handshake IO_WORKER_CLASS uses to keep
        // exchange() (main thread) and thread_main() (this class's own
        // thread) from touching comms_buffer at the same time - see
        // IO_WORKER_CLASS's own class comment (io_worker.h) for the full
        // reasoning. One-way here (comms -> comms_buffer only, nothing
        // flows back into the real comms yet), so it's simpler than IO_
        // WORKER_CLASS's own two-direction version, but the same real
        // synchronization requirement applies regardless of direction.
        std::atomic<bool> INTERUPTED{false};
        std::atomic<bool> PROCESSING{false};

        // This worker's own snapshot of the real comms (main.cpp's own),
        // refreshed once per main-loop tick by exchange() below - never
        // touched directly by the main thread outside of exchange() itself,
        // and never touched by thread_main() except while PROCESSING is
        // true (guaranteeing exchange() isn't mid-copy at the same time).
        COMMS comms_buffer;

        // Runs on the background thread - only ever invoked internally,
        // via thread_start()'s own lambda (same access rights as any
        // other member-function code, since the lambda is defined inside
        // one), never meant to be called directly from outside.
        void thread_main();

    public:
        // Runs on the MAIN/owner thread, once per tick, right alongside
        // IO_WORKER_CLASS's own exchange() call (main.cpp). One-way copy
        // of the real comms into this worker's own comms_buffer - nothing
        // flows back out yet, since subcon has nothing to relay into the
        // real conversation right now.
        void exchange(COMMS& comms);

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
