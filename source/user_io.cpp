#ifndef user_io_cpp
#define user_io_cpp

#include "user_io.h"

#include <iostream>
#include <unistd.h>
#include <mutex>
#include <clocale>
#include <csignal>
#include <sys/ioctl.h>

// For ollama_system's definition (response_buffer/thinking_buffer) and
// output_buffer_mutex - user_io.h only forward-declares ollama_system to
// avoid a circular include (olla.h -> system.h -> user_io.h), so the actual
// implementation needs the real header.
#include "olla.h"

// Only pulled in here - see the forward-declared WINDOW in user_io.h for why.
#include <ncursesw/curses.h>

// Set by a SIGWINCH handler, consumed once per display_with_ncurses() call.
// Needs to be a plain global (signal-safe types only in a handler) - display
// _with_ncurses() is the only reader, always on the main thread, so no
// mutex needed despite the signal being delivered async.
namespace {
    volatile sig_atomic_t g_ncurses_resized = 0;

    void handle_sigwinch(int)
    {
        g_ncurses_resized = 1;
    }
}

// Constructor: Save state and enter raw mode
KEYBOARD_INPUT::KEYBOARD_INPUT() {
    if (tcgetattr(STDIN_FILENO, &oldt) == 0) {
        newt = oldt;

        // Apply the "Raw" flags we discussed
        newt.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | IEXTEN | ISIG));
        newt.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));

        newt.c_cc[VMIN] = 0;
        newt.c_cc[VTIME] = 0;

        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }
}

// Destructor: Automatically restore the terminal
KEYBOARD_INPUT::~KEYBOARD_INPUT() {
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    // Optional: Ensure the cursor is on a fresh line
    std::cout << std::endl;
}

void KEYBOARD_INPUT::keyboard_input()
{
    if (PROPS.ENABLED)
    {
        char ch;


        // read() will now return 0 if no character is waiting
        while (read(STDIN_FILENO, &ch, 1) > 0)
        {

            double gap_time = enter_ready.elapsed_time();

            //std::cout << static_cast<int>(ch) << std::endl;
            if (ch == 3) // Ctrl+C (ETX) - see EXIT_REQUESTED's comment in user_io.h
            {
                EXIT_REQUESTED = true;
            }
            else if (ch == 10 || ch == 13 || ch == '\r')
            {
                LINE += '\n';
                if (PROPS.RAW_ECHO) std::cout << "\r\n" << std::flush;
                if (gap_time > 0.1)
                {
                    INTERRUPTED = true;
                    ENTER_PRESSED = true;
                }
            }
            else if ((ch == 127 || ch == 8))
            {
                if (!LINE.empty())
                {
                    LINE.pop_back();
                    if (PROPS.RAW_ECHO) std::cout << "\b \b" << std::flush;
                }
            }
            else
            {
                LINE += ch;
                if (PROPS.RAW_ECHO) std::cout << ch << std::flush;
                INTERRUPTED = true;
            }


            enter_ready.start_timer(); // Reset the timer on each key press
        }

        // Voca (speech-to-text) input is drained separately in main.cpp's
        // loop, straight from AUDIO_CONTROL_CLASS - see popVocaEvent().

        IS_TYPING = !LINE.empty();
    }
}


void KEYBOARD_INPUT::reset()
{
    LINE.clear();
    ENTER_PRESSED = false;
    INTERRUPTED = false;
}

// ----

OUTPUT_CLASS::~OUTPUT_CLASS()
{
    // Runs before KEYBOARD_INPUT's destructor (see declaration order in
    // system.h - CLASS_SYSTEM's members are destroyed in reverse order),
    // so endwin() here restores whatever terminal mode ncurses thinks it
    // saved as "shell mode" - possibly our own raw mode, since initscr()
    // was called after KEYBOARD_INPUT's constructor already set that up -
    // and then KEYBOARD_INPUT's destructor still restores the true original
    // settings afterward regardless.
    end_ncurses();
}

void OUTPUT_CLASS::end_ncurses()
{
    if (ncurses_started)
    {
        endwin();
        ncurses_started = false;
    }
}

void OUTPUT_CLASS::get_response(ollama_system& chat)
{
    std::lock_guard<std::mutex> lock(output_buffer_mutex);
    chat_response += chat.response_buffer;
    chat.response_buffer.clear();
    chat_thinking += chat.thinking_buffer;
    chat.thinking_buffer.clear();
    system_message += chat.log_buffer;
    chat.log_buffer.clear();
}

void OUTPUT_CLASS::display()
{
    if (!system_message.empty())
    {
        std::cout << system_message << std::flush;
        system_message.clear();
    }

    if (!user_input.empty())
    {
        std::cout << user_input << std::flush;
        user_input.clear();
    }

    // Open/close the <thinking> bracket once per block, not once per tick -
    // see in_thinking_block's comment in user_io.h.
    if (!chat_thinking.empty())
    {
        if (!in_thinking_block)
        {
            std::cout << "\n<thinking>\n";
            in_thinking_block = true;
        }
        std::cout << chat_thinking << std::flush;
        chat_thinking.clear();
    }

    if (!chat_response.empty())
    {
        if (in_thinking_block)
        {
            std::cout << "\n</thinking>\n\n";
            in_thinking_block = false;
        }
        std::cout << chat_response << std::flush;
        chat_response.clear();
    }
}

// ----

/**
 * (Re)creates/repositions the four windows for the current terminal size
 * and thinking-visibility state. Existing windows are resized/moved in
 * place (wresize + mvwin) rather than destroyed and recreated, so their
 * scrollback content survives a resize or a thinking window appearing/
 * disappearing - recreating win_chat on every thinking-visibility toggle
 * (which happens most turns) would otherwise wipe the visible transcript
 * constantly. win_thinking gets explicitly cleared each time it reappears
 * though - stale thinking text from a previous turn showing up again would
 * be confusing (see the class comment in user_io.h).
 *
 * Layout, top to bottom: a 1-line system-message strip, a separator, the
 * thinking window (only when ncurses_thinking_visible), another separator
 * if so, the scrolling chat transcript (user_input + chat_response, in that
 * order - a classic chat feel), a separator, and a 1-line input line.
 */
void OUTPUT_CLASS::ncurses_layout()
{
    int screen_h = ncurses_screen_h;
    int screen_w = ncurses_screen_w;

    const int sys_h = 1;
    const int input_h = 1;
    const int thinking_h = ncurses_thinking_visible ? 6 : 0;

    erase();   // stdscr - just the separator lines live directly on it
    refresh();

    int row = 0;

    if (win_system == nullptr)
    {
        win_system = newwin(sys_h, screen_w, row, 0);
    }
    else
    {
        wresize(win_system, sys_h, screen_w);
        mvwin(win_system, row, 0);
    }
    row += sys_h;

    mvhline(row, 0, ACS_HLINE, screen_w);
    row += 1;

    if (ncurses_thinking_visible)
    {
        if (win_thinking == nullptr)
        {
            win_thinking = newwin(thinking_h, screen_w, row, 0);
            scrollok(win_thinking, TRUE);
        }
        else
        {
            wresize(win_thinking, thinking_h, screen_w);
            mvwin(win_thinking, row, 0);
        }
        werase(win_thinking); // fresh block, not leftover text from last time
        row += thinking_h;

        mvhline(row, 0, ACS_HLINE, screen_w);
        row += 1;
    }
    // Not visible: win_thinking (if it exists from a previous turn) is left
    // alone - unpositioned, unrefreshed - until it's needed again.

    int chat_h = screen_h - row - 1 /* separator before input */ - input_h;
    if (chat_h < 1) chat_h = 1;

    if (win_chat == nullptr)
    {
        win_chat = newwin(chat_h, screen_w, row, 0);
        scrollok(win_chat, TRUE);
    }
    else
    {
        wresize(win_chat, chat_h, screen_w);
        mvwin(win_chat, row, 0);
    }
    row += chat_h;

    mvhline(row, 0, ACS_HLINE, screen_w);
    row += 1;

    if (win_input == nullptr)
    {
        win_input = newwin(input_h, screen_w, row, 0);
    }
    else
    {
        wresize(win_input, input_h, screen_w);
        mvwin(win_input, row, 0);
    }

    refresh(); // pushes the separator hlines drawn on stdscr above

    // Redraw every currently-visible window's existing content in its
    // (possibly new) position/size immediately, rather than waiting for the
    // next tick's fresh content to trigger it - avoids a blank flash where
    // a window just got taller/moved but nothing new has arrived for it yet.
    wrefresh(win_system);
    if (ncurses_thinking_visible) wrefresh(win_thinking);
    wrefresh(win_chat);
    wrefresh(win_input);
}

void OUTPUT_CLASS::display_with_ncurses(const KEYBOARD_INPUT& key_input)
{
    if (!ncurses_started)
    {
        setlocale(LC_ALL, ""); // required before initscr() for wide/UTF-8 output
        initscr();
        // Deliberately NOT calling cbreak()/noecho() here: those exist to
        // prepare the terminal for ncurses' OWN input functions (getch() et
        // al), which this integration never calls - all input still goes
        // through KEYBOARD_INPUT's own raw-mode read() loop (user_io.cpp).
        // That's not enough on its own, though: initscr() ITSELF resets
        // VMIN/VTIME on stdin even without cbreak() ever being called
        // (confirmed by testing - the read() loop hung the instant ncurses
        // started, before any input was ever typed), silently turning that
        // loop's non-blocking VMIN=0 reads into a blocking wait. Force it
        // back to VMIN=0/VTIME=0 right here, same values KEYBOARD_INPUT's
        // constructor already set before initscr() ever ran.
        {
            struct termios t;
            if (tcgetattr(STDIN_FILENO, &t) == 0)
            {
                t.c_cc[VMIN] = 0;
                t.c_cc[VTIME] = 0;
                tcsetattr(STDIN_FILENO, TCSANOW, &t);
            }
        }
        curs_set(0); // no blinking terminal cursor - none of our windows track one
        std::signal(SIGWINCH, handle_sigwinch);
        ncurses_started = true;
    }

    if (g_ncurses_resized)
    {
        g_ncurses_resized = 0;
        // ncurses never sees SIGWINCH update the terminal size on its own
        // here, since nothing in this integration calls wgetch() (the usual
        // place it would notice) - it has to be told explicitly: ask the
        // kernel for the real new size (ioctl TIOCGWINSZ) and hand that to
        // resizeterm(), which updates LINES/COLS and clips/extends stdscr
        // to match. A blind endwin()+refresh() (tried first) does NOT
        // reliably pick up the new size on its own - confirmed by testing,
        // the screen stayed broken after a real terminal resize until this.
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
        {
            resizeterm(ws.ws_row, ws.ws_col);
        }
    }

    int screen_h = 0;
    int screen_w = 0;
    getmaxyx(stdscr, screen_h, screen_w);

    // Same open/close rule display() uses above: a thinking block opens the
    // moment new thinking text arrives, closes the moment new response text
    // arrives. Shared in_thinking_block also drives ncurses_thinking_visible.
    if (!chat_thinking.empty() && !in_thinking_block)
    {
        in_thinking_block = true;
    }
    if (!chat_response.empty() && in_thinking_block)
    {
        in_thinking_block = false;
    }

    bool need_layout = (screen_h != ncurses_screen_h) ||
                        (screen_w != ncurses_screen_w) ||
                        (in_thinking_block != ncurses_thinking_visible);

    if (need_layout)
    {
        ncurses_screen_h = screen_h;
        ncurses_screen_w = screen_w;
        ncurses_thinking_visible = in_thinking_block;
        ncurses_layout();
    }

    if (!system_message.empty())
    {
        waddstr(win_system, system_message.c_str());
        wrefresh(win_system);
        system_message.clear();
    }

    if (!user_input.empty())
    {
        waddstr(win_chat, user_input.c_str());
        wrefresh(win_chat);
        user_input.clear();
    }

    if (!chat_thinking.empty())
    {
        waddstr(win_thinking, chat_thinking.c_str());
        wrefresh(win_thinking);
        chat_thinking.clear();
    }

    if (!chat_response.empty())
    {
        waddstr(win_chat, chat_response.c_str());
        wrefresh(win_chat);
        chat_response.clear();
    }

    // Live-render the line as it's being typed - key_input.LINE isn't a
    // buffer this class owns/clears, just read fresh each tick.
    werase(win_input);
    waddstr(win_input, ("> " + key_input.LINE).c_str());
    wrefresh(win_input);
}

#endif
