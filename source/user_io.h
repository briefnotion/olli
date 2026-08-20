#ifndef user_io_h
#define user_io_h

#include <string>
#include <termios.h>

#include "fled_time.h"

// ----

// All the ways a person can talk to olli funnel through here. Speech-to-text
// input (Voca) is no longer a separate process talking through files - it's
// in-process now (see audio_control.h/AUDIO_CONTROL_CLASS and voca.hpp).
// main.cpp drains its transcripts each loop tick and feeds them into
// KEYBOARD_INPUT's LINE/INTERRUPTED/ENTER_PRESSED below, the same fields a
// typed line sets.

class KEYBOARD_INPUT_PROPERTIES
{
    public:

    bool ENABLED = false;

    // When OUTPUT_CLASS::display_with_ncurses() is the active display path,
    // this must be false - ncurses owns the screen buffer, and raw cout
    // writes from keyboard_input() (typed-character echo, backspace,
    // newline) would corrupt it. Ncurses renders the typed line itself
    // instead (see display_with_ncurses()'s input window). True by default
    // since plain display() doesn't render typed-but-unsubmitted text at
    // all - the raw echo is the only thing showing it.
    bool RAW_ECHO = true;
};

class KEYBOARD_INPUT
{
    private:

        struct termios oldt, newt;
        EFFICIANTCY_TIMER_EASY enter_ready;

    public:

        KEYBOARD_INPUT_PROPERTIES PROPS;

        std::string LINE = "";
        bool ENTER_PRESSED = false;
        bool INTERRUPTED = false;
        bool IS_TYPING = false;

        // Set when Ctrl+C (raw byte 0x03) is read. Raw mode clears ISIG
        // (see the constructor), so the terminal driver no longer turns
        // Ctrl+C into a real SIGINT on its own - it just arrives as a plain
        // byte here like any other keystroke, so it needs its own explicit
        // handling. main.cpp checks this each tick and calls
        // ollama_system::request_exit(), the same safe shutdown path typed
        // "bye"/"quit" already uses (stop any in-flight response, join
        // chat_thread, then set running = false) - important with ncurses
        // active, since without a working way to quit, breaking the display
        // (e.g. a terminal resize before that's handled) leaves no way back
        // to a normal prompt short of killing the process externally.
        bool EXIT_REQUESTED = false;

        KEYBOARD_INPUT();
        ~KEYBOARD_INPUT();

        void keyboard_input();

        void reset();
};

// ----

class ollama_system; // for get_response() below - see olla.h

// Opaque ncurses window handle, forward-declared so this header (included
// almost everywhere) never has to pull in <curses.h> - that header #defines
// a long list of common identifiers (clear, erase, move, refresh, ...) as
// macros, which would be a landmine for the rest of the codebase. The real
// <curses.h> (via ncursesw) is only included in user_io.cpp, where the
// actual window handling lives. The struct tag below matches curses.h's own
// "typedef struct _win_st WINDOW;" exactly, so the two declarations refer
// to the same type across translation units.
struct _win_st;
typedef struct _win_st WINDOW;

/**
 * OUTPUT_CLASS
 *
 * The screen-facing counterpart to KEYBOARD_INPUT above: one variable,
 * passed around the same way, responsible for everything that gets shown to
 * the user. Four buckets for the four kinds of text olli produces:
 *   - system_message: things from olli itself (saved a file, tool status, etc)
 *   - user_input:      echo of what the user typed/said
 *   - chat_response:   the assistant's actual reply, as it streams in
 *   - chat_thinking:   the assistant's thinking block, as it streams in
 *
 * chat_response/chat_thinking don't get written to directly - they're
 * populated by get_response(), which pulls (and clears) an ollama_system
 * instance's own response_buffer/thinking_buffer (see olla.h). That's a
 * separate cross-thread hop, since those live on whichever ollama_system's
 * chat_thread is streaming a reply; system_message/user_input are plain
 * public strings any same-thread caller can append to directly.
 *
 * display() is meant to be called once per main-loop tick (see main.cpp),
 * right before the loop goes back around - it prints whatever's accumulated
 * in all four buckets and clears them, the same check-act-clear shape
 * ollama_system::write_to_tts() already uses for tts_buffer.
 */
class OUTPUT_CLASS
{
    private:
        // Tracks whether an opening "<thinking>" tag is currently open, so
        // display() only prints it once per thinking block instead of once
        // per tick - mirrors the old in_thinking_block bookkeeping that used
        // to live inline in ollama_system::send() before this class existed.
        // Also drives display_with_ncurses()'s thinking window showing/hiding
        // - both display paths share this one flag as "is a thinking block
        // currently open," even though only one path runs in a given session.
        bool in_thinking_block = false;

        // ---- ncurses state - see display_with_ncurses() in user_io.cpp ----
        // Lazily set up on the first display_with_ncurses() call (so plain
        // display() never touches ncurses at all, for the "fall back to it"
        // case - nothing here runs unless display_with_ncurses() is actually
        // called). Torn down in the destructor if it was ever set up, same
        // RAII shape as KEYBOARD_INPUT's raw-mode terminal handling.
        bool ncurses_started = false;
        int ncurses_screen_h = 0;
        int ncurses_screen_w = 0;
        bool ncurses_thinking_visible = false;
        WINDOW* win_system = nullptr;
        WINDOW* win_thinking = nullptr;
        WINDOW* win_chat = nullptr;
        WINDOW* win_input = nullptr;

        void ncurses_layout(); // (re)creates the four windows for the current
                                // terminal size and thinking-visibility state

    public:
        std::string system_message = "";
        std::string user_input = "";
        std::string chat_response = "";
        std::string chat_thinking = "";

        ~OUTPUT_CLASS();

        // Ends ncurses (endwin()) and hands the real terminal screen back,
        // if display_with_ncurses() ever started it - a no-op otherwise.
        // Call this explicitly before printing anything with plain cout
        // during shutdown (see main.cpp) - otherwise those prints happen
        // while ncurses' alternate screen is still up and are invisible
        // until the destructor tears it down at the very end of main().
        // Also called from the destructor, so this doesn't have to be
        // called explicitly for correctness, only for shutdown-message
        // visibility.
        void end_ncurses();

        // Pulls and clears chat's response_buffer/thinking_buffer into this
        // instance's own chat_response/chat_thinking, under chat's
        // output_buffer_mutex (olla.h). Safe to call every tick even if
        // chat hasn't produced anything new since the last call.
        void get_response(ollama_system& chat);

        // Prints whatever's in all four buckets to the screen, then clears
        // them.
        void display();

        // Alternate to display() above: same four buckets, rendered into a
        // windowed ncurses layout (system message strip, a thinking window
        // that appears/disappears with the thinking block, a scrolling chat
        // transcript, an input line) instead of one flat scrolling stream.
        // Takes key_input so the input window can show the line as it's
        // being typed, live. Only one of display()/display_with_ncurses()
        // should be used for a given run (see main.cpp) - see RAW_ECHO on
        // KEYBOARD_INPUT_PROPERTIES for the other half of that switch.
        void display_with_ncurses(const KEYBOARD_INPUT& key_input);
};

#endif
