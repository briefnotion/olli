#include "worker_thread.h"

#include <chrono>
#include <thread>

WORKER_THREAD_CLASS::~WORKER_THREAD_CLASS()
{
}

void WORKER_THREAD_CLASS::signal_interrupt()
{
    INTERUPTED.store(true);
}

void WORKER_THREAD_CLASS::clear_interrupt()
{
    INTERUPTED.store(false);
}

bool WORKER_THREAD_CLASS::interrupted() const
{
    return INTERUPTED.load();
}

void WORKER_THREAD_CLASS::thread_start()
{
    THREAD_CONTROL.create(1000);
    THREAD_CONTROL.start_render_thread([&]() { thread_main(); });
}

void WORKER_THREAD_CLASS::thread_stop()
{
    // Signal exit, then actually wait for the background thread to finish
    // before returning - same reasoning as SIDETRACK_CLASS::thread_stop:
    // returning early lets a caller destroy this object (and everything it
    // references) while thread_main() is still running against it.
    RUN = false;
    THREAD_CONTROL.wait_for_thread_to_finish();
}

void WORKER_THREAD_CLASS::thread_main()
{
    RUN = true;
    while (RUN)
    {
        if (!INTERUPTED.load())
        {
            PROCESSING.store(true);

            // --------------------------------------------------------------
            // BACKGROUND THREAD - your code here. Runs on its own thread,
            // started by thread_start(). Don't touch state the owner thread
            // already owns from here - hand it off via EXCHANGE and let
            // exchange() (the actual main thread) do that part.
            // --------------------------------------------------------------

            EXCHANGE_DUMMY++;

            // --------------------------------------------------------------

            PROCESSING.store(false);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(PROPS.INTERVAL)));
    }
}

void WORKER_THREAD_CLASS::exchange(int New_Start)
{
    if (!PROPS.BLOCKING) return; // pass straight through, don't touch EXCHANGE

    signal_interrupt();

    // Wait out any background pass already in flight - INTERUPTED only
    // stops a NEW pass from starting, it doesn't abort one already
    // running. See the PROCESSING/INTERUPTED comment in worker_thread.h.
    while (PROCESSING.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // --------------------------------------------------------------
    // MAIN THREAD - your code here. Called once per the owner's own
    // loop tick. This is the only place safe to touch state the owner
    // thread already owns.
    // ---------------------------------------------------------
    {
        EXCHANGE_DUMMY = New_Start;
    }
    // --------------------------------------------------------------

    clear_interrupt();
}
