#ifndef IO_WORKER_H
#define IO_WORKER_H

#include <atomic>

#include "threading.h"
#include "user_io.h" // KEYBOARD_INPUT, OUTPUT_CLASS, COMMS (via comms.h)

class ollama_system;
class SIDETRACK_CLASS;
class AUDIO_CONTROL_CLASS;

struct IO_WORKER_CLASS_PROPERTIES
{
    double INTERVAL = 20; // ms, matches olli's original main-loop tick rate

    // ================================================================
    // WARNING - READ BEFORE CHANGING THE DEFAULT BELOW.
    //
    // BLOCKING = true: exchange() spins until PROCESSING clears before
    // touching the passed-in COMMS. thread_main()'s per-tick work here
    // (keyboard read, voice poll, draining comms into the screen, an
    // ncurses draw) is all fast/non-blocking, so this stall is brief -
    // unlike SIDETRACK_CLASS's multi-second LLM calls, this is safe to
    // leave on.
    //
    // BLOCKING = false: exchange() does NOT wait - but it also does NOT
    // relay anything. Whatever was staged this tick is simply not handed
    // off, and stays staged for the next exchange() call instead (unlike
    // WORKER_THREAD_CLASS's original EXCHANGE_DUMMY, nothing is lost here
    // - see IO_WORKER_CLASS's class comment on staged/comms).
    // ================================================================
    bool BLOCKING = true;
};

/**
 * IO_WORKER_CLASS
 *
 * Owns everything that talks to the user: keyboard input, voice input
 * (polled from AUDIO_CONTROL_CLASS, which keeps its own separate thread -
 * see create() below), and the ncurses/plain-terminal display. Runs on its
 * own background thread (thread_main(), via WORKER_THREAD_CLASS's shape -
 * see templates/worker_thread.h/.cpp, which this was copied and adapted
 * from) so none of that interleaves with chat/model logic on the main
 * thread.
 *
 * KEYBOARD_INPUT and OUTPUT_CLASS (user_io.h/.cpp) have zero built-in
 * thread-safety of their own - raw termios manipulation on STDIN_FILENO,
 * unlocked plain fields, and ncurses itself is single-thread-only. So they
 * are OWNED here, directly, as members - not borrowed from CLASS_SYSTEM -
 * and this worker is their sole caller for their entire lifetime. Declared
 * in this order (key_input before output) to match CLASS_SYSTEM's old
 * member order: OUTPUT_CLASS's destructor comment documents relying on
 * being destroyed before KEYBOARD_INPUT's termios-restoring destructor
 * runs, which C++'s reverse-declaration-order destruction still gives for
 * free as long as this order is preserved.
 *
 * comms (COMMS, comms.h) is the ONLY thing that crosses the exchange()
 * boundary - not key_input/output themselves. `staged` below is this
 * worker's own local COMMS, touched only by thread_main() (its own
 * thread); only its four new input-direction fields (send/submitted_line/
 * stop_requested/exit_requested) are ever used here - the rest sit unused,
 * since the original four output buffers already flow through the real
 * chat.comms directly (output_buffer_mutex, unrelated to this lock).
 * exchange(COMMS& comms) - called by the main thread once per tick with
 * chat.comms - relays whatever's pending in `staged` into it and clears
 * the staged side; the HOST side is cleared later by whoever actually
 * consumes it (ollama_system::input(), olla.cpp), not by exchange()
 * itself. exchange() only ever SETS comms.send/stop_requested/
 * exit_requested (never false->true->false in one call) so it never stomps
 * a submission the consuming side hasn't gotten to yet.
 *
 * chat/sidetrack/audio below are wired up once via create() - NOT part of
 * the exchange() surface, just what thread_main() itself needs for its own
 * per-tick work (draining chat/sidetrack's output, polling/stopping
 * audio).
 */
class IO_WORKER_CLASS
{
    protected:
        THREADING_INFO THREAD_CONTROL;
        std::atomic<bool> INTERUPTED{false};
        std::atomic<bool> PROCESSING{false};

        void signal_interrupt();
        void clear_interrupt();
        bool interrupted() const;

        COMMS staged;

        ollama_system* chat = nullptr;
        SIDETRACK_CLASS* sidetrack = nullptr;
        AUDIO_CONTROL_CLASS* audio = nullptr;

    public:
        IO_WORKER_CLASS_PROPERTIES PROPS;

        bool RUN = false;

        KEYBOARD_INPUT key_input;
        OUTPUT_CLASS output;

        ~IO_WORKER_CLASS();

        void create(ollama_system& chat_ref, SIDETRACK_CLASS& sidetrack_ref, AUDIO_CONTROL_CLASS& audio_ref);

        void thread_start();
        void thread_stop();

        // Runs on the background thread.
        void thread_main();

        // Runs on the MAIN/owner thread - call once per its own loop
        // tick, passing chat.comms.
        void exchange(COMMS& comms);
};

#endif
