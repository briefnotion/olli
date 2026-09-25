#ifndef SUBCON_WORKER_H
#define SUBCON_WORKER_H

#include "olla.h" // OLLAMA_SYSTEM_PROPERTIES
#include "comms.h" // COMMS
#include "threading.h"

#include <mutex>

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
 *
 * comms_buffer's own synchronization is a real std::mutex, not the
 * INTERUPTED/PROCESSING atomic-bool handshake IO_WORKER_CLASS uses - a
 * deliberate improvement on that pattern, decided after actually comparing
 * the two: a real mutex is correct regardless of how many callers exist
 * (IO_WORKER_CLASS's own scheme only works because there's exactly one -
 * TOOL_WORKER_CLASS's own state_mutex, tool_worker.h, exists precisely
 * because that assumption broke there once, a real live SIGSEGV). What's
 * different from TOOL_WORKER_CLASS's own state_mutex: exchange() takes it
 * via try_lock (see its own comment below) rather than blocking, since
 * copying a snapshot is safe to skip on lock contention (missed once,
 * refreshed again next tick) the same way TOOL_WORKER_CLASS::
 * get_pending_result()/get_pending_event() safely skip on contention -
 * unlike put_pending_call()/abandon_call(), which must never be skipped.
 * thread_main()'s own side still blocks normally - it's fine for subcon's
 * own background thread to wait briefly on its own mutex, since nothing
 * else is waiting on it.
 */
class SUBCON_WORKER_CLASS
{
    private:
        THREADING_INFO THREAD_CONTROL;

        bool RUN = false;

        // Guards comms_buffer below - see this class's own comment above
        // for why this is a real mutex, not IO_WORKER_CLASS's own
        // INTERUPTED/PROCESSING scheme.
        std::mutex comms_mutex;

        // This worker's own snapshot of the real comms (main.cpp's own),
        // refreshed once per main-loop tick by exchange() below (when it
        // manages to get the lock - see its own comment) - never touched
        // directly by the main thread outside of exchange() itself, and
        // never touched by thread_main() except while holding comms_mutex.
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
        // real conversation right now. Non-blocking (try_lock, not a plain
        // lock_guard) - the main thread has a lot else to do every tick, so
        // it never waits on subcon's own background thread; if the attempt
        // fails, comms_buffer just keeps last tick's snapshot and this
        // tries again next time, same as leaving it alone would anyway.
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
