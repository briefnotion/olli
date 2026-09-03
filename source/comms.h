#ifndef COMMS_H
#define COMMS_H

#include <string>
#include <mutex>
#include <atomic>

#include <ncursesw/curses.h>

// A single shared mutex guarding every ollama_system instance's COMMS.
//
// This MUST be an 'inline' variable (C++17), not 'static' - same reasoning
// as history_mutex (olla.h): a 'static' definition in a header gives every
// translation unit that includes this header its OWN private mutex, so
// different threads touching different instances' COMMS would lock
// unrelated mutexes and never actually exclude one another. 'inline'
// yields one shared instance across all translation units with no
// "multiple definition" linker error.
//
// Deliberately ONE mutex covering every instance's COMMS, not one per
// instance - see COMMS's own class comment for why a private-per-instance
// mutex here specifically would be a mistake even though the buffers
// themselves are per-instance.
inline std::mutex output_buffer_mutex;

/**
 * COMMS
 * Bundles what an ollama_system instance uses to hand output to whatever's
 * consuming it (the screen, a log) - moved out of olla.h so it can be
 * included/passed around on its own instead of needing the rest of
 * ollama_system along with it.
 *
 * Each ollama_system instance (the main chat, background tasks,
 * sidetrack's own SIDETRACK_CHAT_INSTANCE) owns its OWN COMMS - the text
 * buffers are per-instance. output_buffer_mutex above is the one
 * exception: it's deliberately shared across every instance's COMMS rather
 * than being a member here, for the same reason history_mutex is shared
 * across every instance's history - one coarse lock is simpler to reason
 * about correctly than a private mutex per instance, which would silently
 * fail to exclude anything.
 */
class COMMS
{
    public:
        // --------------------------------------------------------------
        // Output-direction buffers - streamed into incrementally by
        // whoever's producing them, drained (read + cleared) by whoever's
        // consuming them, under output_buffer_mutex above. See
        // OUTPUT_CLASS::get_response() (user_io.cpp) and SIDETRACK_CLASS::
        // pull_output() (sidetrack.cpp) for the two existing consumers.
        // --------------------------------------------------------------
        std::string INPUT_FROM_LLM = "";
        std::string INPUT_FROM_THINKING = "";
        std::string INPUT_FROM_SYSTEM = "";

        // ncurses attribute value (e.g. COLOR_PAIR(n) | A_DIM) for each
        // buffer above when it's rendered to the chat panel - same type
        // NCURSES_TEXT_PANEL::append()'s own attr parameter takes
        // (user_io.h). Not wired into display_with_ncurses() yet (user_io.cpp
        // still uses its own local PAIR_USER_INPUT_GREY|A_DIM for
        // INPUT_FROM_USER and a bare 0 for INPUT_FROM_LLM) - just the
        // storage for now, defaulted to match what's rendered there today.
        //
        // Pair index 1 below duplicates user_io.cpp's own PAIR_USER_INPUT_
        // GREY (still private to that file) rather than sharing one
        // definition - deliberate for now, scope kept to this class only.
        // Relies on user_io.cpp's existing init_pair(1, COLOR_WHITE, -1)
        // call actually having run (guarded by ncurses_colors_available) -
        // if colors aren't available there, ncurses harmlessly ignores an
        // uninitialized pair index and this just renders unstyled.
        int INPUT_FROM_LLM_COLOR = 0;                    // plain/undecorated - terminal's own default (white on most themes)
        int INPUT_FROM_USER_COLOR = COLOR_PAIR(1) | A_DIM; // dimmed white - reads as grey
        // --------------------------------------------------------------

        // --------------------------------------------------------------
        // Input-direction signals - set by IO_WORKER_CLASS (io_worker.h/
        // .cpp), relayed here via its exchange(), consumed by
        // ollama_system::input() (olla.cpp) and main.cpp's own loop. Not
        // protected by output_buffer_mutex above - IO_WORKER_CLASS's own
        // INTERUPTED/PROCESSING lock covers the handoff into these
        // fields instead (see its class comment). Once a field lands
        // here, it's the CONSUMING side's job to clear it when actually
        // acted on - exchange() only ever sets these, never clears them.
        // Named to match KEYBOARD_INPUT's own ENTER_PRESSED/INTERRUPTED/
        // EXIT_REQUESTED (user_io.h), which these are relayed from.
        // --------------------------------------------------------------
        bool ENTER_PRESSED = false;    // a line is ready to submit
        std::string INPUT_FROM_USER;   // valid when ENTER_PRESSED == true
        bool INTERRUPTED = false;      // abort in-flight generation/speech
        bool IS_TYPING = false;        // a line is being typed/spoken, not yet submitted
        bool EXIT_REQUESTED = false;   // Ctrl+C - shut olli down
        // --------------------------------------------------------------

        // Opposite direction from the block above: set by main.cpp (main
        // thread) when sidetrack's context-clear routine fires mid-loop,
        // consumed by IO_WORKER_CLASS::thread_main() (its own thread),
        // which is the only safe caller of output.close_chat_log() while
        // the worker thread is still running - see IO_WORKER_CLASS's
        // class comment. Not part of exchange()'s relay - both sides
        // touch this field directly, so it's atomic instead (same
        // reasoning as KEYBOARD_INPUT_PROPERTIES::ENABLED, user_io.h).
        std::atomic<bool> close_chat_log_requested{false};
};

#endif
