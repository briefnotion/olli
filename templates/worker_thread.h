#ifndef WORKER_THREAD_H
#define WORKER_THREAD_H

#include <atomic>

#include "threading.h"

struct WORKER_THREAD_CLASS_PROPERTIES
{
    double INTERVAL = 500; // ms, background thread tick rate

    // ================================================================
    // WARNING - READ BEFORE CHANGING THE DEFAULT BELOW.
    //
    // BLOCKING = true: exchange() spins until PROCESSING clears (see the
    // PROCESSING/INTERUPTED comment below) before touching EXCHANGE. If
    // thread_main()'s background block ever does something slow (a
    // network call, an LLM request - like SIDETRACK_CHAT_INSTANCE.send()
    // in the class this was modeled on), exchange() STALLS THE OWNER'S
    // MAIN LOOP for the full duration of that pass. Called from a real
    // main loop, that means audio/display/everything freezes, not just
    // this worker.
    //
    // BLOCKING = false: exchange() does NOT wait - but it also does NOT
    // write. It passes straight through and SILENTLY DROPS New_Start on
    // any tick where a pass happens to be in flight. There is no queue,
    // no retry - the value is just gone.
    //
    // Neither setting is free. Pick BLOCKING deliberately once
    // thread_main() actually does real work, don't leave the default
    // sitting here unexamined.
    // ================================================================
    bool BLOCKING = true;
};

/**
 * WORKER_THREAD_CLASS
 * Blank skeleton for a background thread paired with a main-thread
 * check-in, following the shape SIDETRACK_CLASS (source/sidetrack.h/.cpp)
 * uses for its own background work - just without any routine/stage
 * machinery baked in. Copy this pair of files to source/, rename the
 * class (and EXCHANGE_DUMMY, and the file/guard names) to match the new
 * use case, then fill in thread_main() and exchange(). Needs source/ -
 * depends on threading.h living there.
 *
 * - thread_main() runs on its own background thread (started by
 *   thread_start()). Do the actual work here.
 * - exchange() runs on the MAIN/owner thread - call it once per the
 *   owner's own loop tick. Anything that must run on the owner's thread
 *   (because it touches state only that thread may safely touch) goes
 *   here instead of in thread_main() - same single-owner rule
 *   SIDETRACK_CLASS follows for main_instance.history.
 *
 * EXCHANGE variables are touched from both threads, so the two blocks
 * above are guarded by a small two-flag lock instead of a real mutex:
 *   - INTERUPTED - set by exchange() before it writes, cleared after.
 *     Tells thread_main() "don't start a new background pass right now."
 *     Checked once at the top of thread_main()'s loop, so it only ever
 *     blocks the *next* pass, not one already underway - see PROCESSING
 *     below for that.
 *   - PROCESSING - set by thread_main() right before it enters its
 *     EXCHANGE-touching block, cleared right after. exchange() spins on
 *     this (after setting INTERUPTED) before it's safe to write -
 *     otherwise a pass already in flight when exchange() runs could still
 *     race it.
 * Together they mean: once exchange() sees PROCESSING == false with
 * INTERUPTED already set, thread_main() cannot enter that block again
 * until clear_interrupt() runs, so the write is safe. Neither flag alone
 * is enough - INTERUPTED alone leaves a gap for a pass already running,
 * PROCESSING alone leaves a gap for a new pass starting while exchange()
 * is still mid-write.
 */
class WORKER_THREAD_CLASS
{
    protected:
        THREADING_INFO THREAD_CONTROL;
        std::atomic<bool> INTERUPTED{false};
        std::atomic<bool> PROCESSING{false};

        void signal_interrupt();
        void clear_interrupt();
        bool interrupted() const;

    public:
        WORKER_THREAD_CLASS_PROPERTIES PROPS;

        bool RUN = false;

        // --------------------------------------------------------------
        // EXCHANGE - variables that pass data between the background
        // thread (thread_main()) and the owner/main thread (exchange()).
        // Same handoff role temp_chat_history plays in SIDETRACK_CLASS.
        // --------------------------------------------------------------
        int EXCHANGE_DUMMY = 0; // TODO: replace with real exchange variables
        // --------------------------------------------------------------

        virtual ~WORKER_THREAD_CLASS();

        void thread_start();
        void thread_stop();

        // Runs on the background thread. Fill in.
        virtual void thread_main();

        // Runs on the MAIN/owner thread - call once per its own loop tick. Fill in.
        virtual void exchange(int New_Start);
};

#endif
