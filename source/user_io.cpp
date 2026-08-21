#ifndef user_io_cpp
#define user_io_cpp

#include "user_io.h"

#include <iostream>
#include <fstream>
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

// Only pulled in here - see the forward-declared WINDOW/PANEL in user_io.h
// for why.
#include <ncursesw/curses.h>
#include <ncursesw/panel.h>

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

    // COLOR_WHITE dimmed down to a light-grey look for what the user typed
    // (see display_with_ncurses()'s user_input block) - chat_response stays
    // whatever the terminal's default foreground is, no pair applied.
    // init_pair(1, ...) below, guarded by ncurses_colors_available.
    constexpr int PAIR_USER_INPUT_GREY = 1;

    // How long the thinking box lingers after in_thinking_block drops
    // before it actually closes - see ncurses_thinking_closing's comment
    // in user_io.h.
    constexpr double THINKING_BOX_LINGER_MS = 2000.0;
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

void OUTPUT_CLASS::append_to_chat_log(bool is_user, const std::string& text)
{
    if (chat_log_path.empty() || text.empty()) return;

    bool first_write_this_run = !chat_log_last_speaker_was_user.has_value();

    std::ofstream log_file(chat_log_path, std::ios::app);
    if (!log_file.is_open()) return;

    if (first_write_this_run)
    {
        // Separate this run's entries from whatever a previous session
        // already left in the file (if anything) with a blank line.
        std::error_code ec;
        if (std::filesystem::file_size(chat_log_path, ec) > 0) log_file << "\n";
    }

    if (chat_log_last_speaker_was_user != is_user)
    {
        if (chat_log_last_speaker_was_user.has_value()) log_file << "\n";
        log_file << (is_user ? (chat_log_user_label + ": ") : "Olli: ");
        chat_log_last_speaker_was_user = is_user;
    }

    log_file << text;
}

void OUTPUT_CLASS::close_chat_log()
{
    if (chat_log_path.empty()) return;

    std::error_code ec;
    if (!std::filesystem::exists(chat_log_path, ec) ||
        std::filesystem::file_size(chat_log_path, ec) == 0)
    {
        return; // nothing written (or path doesn't exist yet) - nothing to archive
    }

    std::filesystem::path archive_dir = chat_log_path.parent_path() / "chat_logs";
    std::filesystem::create_directories(archive_dir, ec);

    std::string stamp = timestamp_prefix(); // "YYMMDD.HHMM" - see helper_olli.h
    std::filesystem::path archive_path = archive_dir / (stamp + ".chat_log.txt");

    // Disambiguate the rare case of two closes landing in the same minute -
    // rename() would otherwise silently overwrite an earlier archive rather
    // than erroring.
    int suffix = 2;
    while (std::filesystem::exists(archive_path, ec))
    {
        archive_path = archive_dir / (stamp + "." + std::to_string(suffix) + ".chat_log.txt");
        suffix++;
    }

    std::filesystem::rename(chat_log_path, archive_path, ec);

    // Next append_to_chat_log() call recreates chat_log_path fresh (an
    // ofstream in app mode creates a missing file) and re-writes the
    // "You: "/"Olli: " label from scratch, same as a brand-new run.
    chat_log_last_speaker_was_user.reset();
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
        append_to_chat_log(true, user_input);
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
        append_to_chat_log(false, chat_response);
        chat_response.clear();
    }
}

// ----

/**
 * (Re)creates/repositions the three body windows for the current terminal
 * size, plus the floating thinking box when ncurses_thinking_visible.
 * Existing windows are resized/moved in place (wresize + mvwin) rather than
 * destroyed and recreated, so win_chat's scrollback content survives a
 * resize or a thinking window appearing/disappearing - recreating it on
 * every thinking-visibility toggle (which happens most turns) would
 * otherwise wipe the visible transcript constantly.
 *
 * Layout, top to bottom: a 3-line scrolling system-message strip, a separator, the
 * scrolling chat transcript (user_input + chat_response, in that order - a
 * classic chat feel) filling everything down to a separator and a 1-line
 * input line. win_chat's height/position never depends on thinking
 * visibility any more - the thinking box floats ON TOP of it instead of
 * claiming its own reserved rows and pushing win_chat down (that was the
 * old design: a full-width strip that inserted itself into the flow, so
 * every "thinking" block bumped the whole transcript and input line down a
 * few lines and back). It's a plain WINDOW like the others, just given a
 * single-line box() border, tucked into the upper-right corner of win_chat
 * rather than spanning the full width - recreated fresh each time it's
 * shown, since unlike
 * win_chat it's a short-lived per-turn display with no scrollback worth
 * preserving across a resize.
 */
void OUTPUT_CLASS::ncurses_layout()
{
    int screen_h = ncurses_screen_h;
    int screen_w = ncurses_screen_w;

    const int sys_h = 3;
    const int input_h = 1;

    erase();   // stdscr - just the separator lines live directly on it
    refresh();

    int row = 0;

    if (win_system == nullptr)
    {
        win_system = newwin(sys_h, screen_w, row, 0);
        scrollok(win_system, TRUE);
    }
    else
    {
        wresize(win_system, sys_h, screen_w);
        mvwin(win_system, row, 0);
    }
    row += sys_h;

    mvhline(row, 0, ACS_HLINE, screen_w);
    row += 1;

    int chat_row = row;
    int chat_h = screen_h - row - 1 /* separator before input */ - input_h;
    if (chat_h < 1) chat_h = 1;

    if (win_chat == nullptr)
    {
        win_chat = newwin(chat_h, screen_w, chat_row, 0);
        scrollok(win_chat, TRUE);
        pan_chat = new_panel(win_chat);
    }
    else
    {
        wresize(win_chat, chat_h, screen_w);
        // move_panel(), not mvwin() directly - win_chat is paneled (see
        // user_io.h), and moving its window without going through the
        // panel library would leave the panel's own position bookkeeping
        // out of sync with reality.
        move_panel(pan_chat, chat_row, 0);
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
    // win_chat isn't included here - it's paneled (see user_io.h), so its
    // repaint happens through ncurses_update_thinking_box()'s panel commit
    // below instead, correctly composited with win_thinking if visible.
    wrefresh(win_system);
    wrefresh(win_input);

    ncurses_update_thinking_box();
}

/**
 * (Re)creates or tears down just the floating thinking box - win_thinking
 * plus its interior win_thinking_content (see the member comment in
 * user_io.h) - without touching win_system/win_input or doing a stdscr
 * erase(), unlike ncurses_layout(). Split out as its own path because
 * chat_thinking briefly gets real content most turns even with
 * PROPS.use_thinking off (Ollama's streamed "thinking" field is read
 * unconditionally - see olla.cpp), so in_thinking_block flips on then off
 * almost every turn; routing that through the full ncurses_layout() meant a
 * whole-screen blank-and-rebuild flash twice a turn for a change that only
 * ever affects this one small window. Reads win_chat's current geometry via
 * getmaxyx()/getbegyx() rather than duplicating ncurses_layout()'s row/col
 * math, so there's one source of truth for where the chat area actually is.
 *
 * win_thinking is paneled (pan_thinking, see user_io.h) precisely because
 * it overlaps win_chat: a first version of this used plain wrefresh() calls
 * ordered "chat, then thinking on top," which drew the box correctly but
 * had no way to notice, once the box closed, that win_chat's own refresh
 * needed to repaint over cells it never itself wrote to - a window's
 * refresh only knows about writes to ITS buffer, not that some OTHER
 * window drew there and is now gone. That left the border behind. The
 * panel library tracks the stacking itself, so ncurses_commit_panels() at
 * the end here always composites and repairs correctly regardless of which
 * branch ran.
 */
void OUTPUT_CLASS::ncurses_update_thinking_box()
{
    if (ncurses_thinking_visible)
    {
        int chat_h = 0, chat_w = 0, chat_row = 0, chat_col = 0;
        getmaxyx(win_chat, chat_h, chat_w);
        getbegyx(win_chat, chat_row, chat_col);
        (void)chat_col;

        int think_h = 8; // 6 interior rows + top/bottom border
        if (think_h > chat_h) think_h = chat_h;

        int think_w = chat_w - 4;
        if (think_w > 70) think_w = 70;
        if (think_w < 10) think_w = (chat_w < 10) ? chat_w : 10;

        // Upper-right corner of the chat area, with a small margin off the
        // top and right edges.
        int think_row = chat_row + 1;
        int think_col = chat_w - think_w - 2;
        if (think_col < 0) think_col = 0;

        // Recreated from scratch (not wresize/mvwin in place) rather than
        // repositioned: win_thinking_content is a derived subwindow
        // (derwin) sharing win_thinking's storage, and derived windows
        // don't stay valid if their parent is resized/moved out from under
        // them - so the whole win_thinking/win_thinking_content/pan_thinking
        // trio is always torn down and rebuilt together. Only runs on an
        // actual visibility toggle, not every tick, and there's no
        // scrollback worth preserving across it anyway (short-lived,
        // per-turn display).
        if (win_thinking_content != nullptr)
        {
            delwin(win_thinking_content);
            win_thinking_content = nullptr;
        }
        if (pan_thinking != nullptr)
        {
            // Before delwin(win_thinking) below - a panel still referencing
            // an already-deleted window is a dangling pointer.
            del_panel(pan_thinking);
            pan_thinking = nullptr;
        }
        if (win_thinking != nullptr)
        {
            delwin(win_thinking);
        }
        win_thinking = newwin(think_h, think_w, think_row, think_col);
        box(win_thinking, 0, 0);
        mvwaddstr(win_thinking, 0, 2, " thinking ");

        // Interior-only, offset one row/col in on every side, so a window's
        // ordinary line-wrap-at-full-width behavior can't write over the
        // border's left/right columns - waddstr()/scrolling below happens
        // in here, never directly in win_thinking once the border's drawn.
        win_thinking_content = derwin(win_thinking, think_h - 2, think_w - 2, 1, 1);
        scrollok(win_thinking_content, TRUE);
        // A derived window's own writes don't automatically mark its
        // PARENT as changed - ncurses_commit_panels() below only refreshes
        // win_thinking (the panel's window), never win_thinking_content
        // directly, so without this, wnoutrefresh(win_thinking) sees
        // nothing new in ITS OWN dirty-tracking and skips pushing the
        // interior text entirely (confirmed by testing: the box appeared
        // but stayed permanently empty). syncok(TRUE) makes every write to
        // win_thinking_content automatically propagate a "changed" mark up
        // to win_thinking too.
        syncok(win_thinking_content, TRUE);

        // Stacks above pan_chat by construction - new_panel() always puts
        // the new panel on top of every existing one.
        pan_thinking = new_panel(win_thinking);
    }
    else if (win_thinking != nullptr)
    {
        // No longer visible - drop the (now stale-positioned) trio rather
        // than leaving them around unused; rebuilt fresh next time thinking
        // starts again.
        if (win_thinking_content != nullptr)
        {
            delwin(win_thinking_content);
            win_thinking_content = nullptr;
        }
        if (pan_thinking != nullptr)
        {
            del_panel(pan_thinking);
            pan_thinking = nullptr;
        }
        delwin(win_thinking);
        win_thinking = nullptr;
    }

    ncurses_commit_panels();
}

// update_panels() restacks/composites every PANEL (pan_chat, and pan_thinking
// when it exists) onto ncurses' virtual screen in the correct top-to-bottom
// order, including repainting any area a now-hidden/deleted panel used to
// obscure - the exact thing a plain per-window wrefresh() can't do (see
// ncurses_update_thinking_box()'s doc comment). doupdate() then pushes that
// virtual screen to the real terminal, same role wrefresh()'s second half
// plays for a single non-paneled window. Called after every change to
// win_chat or win_thinking(_content) instead of wrefresh()-ing them directly.
void OUTPUT_CLASS::ncurses_commit_panels()
{
    update_panels();
    doupdate();
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

        ncurses_colors_available = has_colors();
        if (ncurses_colors_available)
        {
            start_color();
            use_default_colors(); // -1 below = terminal's own background, not a hardcoded one
            init_pair(PAIR_USER_INPUT_GREY, COLOR_WHITE, -1);
        }

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

    // The box itself opens immediately (no delay) but lingers for a couple
    // seconds after in_thinking_block drops instead of closing the instant
    // it does - see ncurses_thinking_closing's comment in user_io.h.
    if (in_thinking_block)
    {
        ncurses_thinking_closing = false; // fresh thinking cancels any pending close
    }
    else if (ncurses_thinking_visible && !ncurses_thinking_closing)
    {
        ncurses_thinking_closing = true;
        ncurses_thinking_close_timer.set_e(THINKING_BOX_LINGER_MS);
    }
    if (ncurses_thinking_closing && ncurses_thinking_close_timer.is_ready_e())
    {
        ncurses_thinking_closing = false;
    }
    bool want_thinking_visible = in_thinking_block || ncurses_thinking_closing;

    bool screen_resized = (screen_h != ncurses_screen_h) || (screen_w != ncurses_screen_w);
    bool thinking_toggled = (want_thinking_visible != ncurses_thinking_visible);

    if (screen_resized)
    {
        ncurses_screen_h = screen_h;
        ncurses_screen_w = screen_w;
        ncurses_thinking_visible = want_thinking_visible;
        ncurses_layout(); // full rebuild - includes the thinking box, if visible
    }
    else if (thinking_toggled)
    {
        // No screen-size change - only the floating thinking box needs to
        // appear/disappear, not a full relayout (see
        // ncurses_update_thinking_box()'s comment for why that distinction
        // matters here).
        ncurses_thinking_visible = want_thinking_visible;
        ncurses_update_thinking_box();
    }

    if (!system_message.empty())
    {
        waddstr(win_system, system_message.c_str());
        wrefresh(win_system);
        system_message.clear();
    }

    if (!user_input.empty())
    {
        // Light grey (dimmed white) so what the user typed reads as
        // visually distinct from the assistant's replies (chat_response
        // below stays plain/undecorated - see PAIR_USER_INPUT_GREY's comment).
        if (ncurses_colors_available) wattron(win_chat, COLOR_PAIR(PAIR_USER_INPUT_GREY) | A_DIM);
        waddstr(win_chat, user_input.c_str());
        if (ncurses_colors_available) wattroff(win_chat, COLOR_PAIR(PAIR_USER_INPUT_GREY) | A_DIM);
        append_to_chat_log(true, user_input);
        ncurses_commit_panels(); // win_chat is paneled - see user_io.h
        user_input.clear();
    }

    if (!chat_thinking.empty())
    {
        // ncurses_thinking_visible-guarded: win_thinking/win_thinking_content
        // now overlap win_chat (see ncurses_layout()), so writing to them
        // while logically hidden - stale, already-deleted-or-about-to-be
        // from a previous turn - would draw over live chat text instead of
        // just redundantly redrawing a dedicated strip the way the old
        // full-width design safely did. chat_thinking is still drained
        // either way, same as display()'s no-ncurses path never held text
        // back.
        if (ncurses_thinking_visible)
        {
            waddstr(win_thinking_content, chat_thinking.c_str());
            ncurses_commit_panels(); // win_thinking is paneled - see user_io.h
        }
        chat_thinking.clear();
    }

    if (!chat_response.empty())
    {
        // Plain/undecorated (whatever the terminal's default foreground
        // is) - user_input above is the one that gets dimmed grey now.
        waddstr(win_chat, chat_response.c_str());
        append_to_chat_log(false, chat_response);
        ncurses_commit_panels(); // win_chat is paneled - see user_io.h
        chat_response.clear();
    }

    // Live-render the line as it's being typed - key_input.LINE isn't a
    // buffer this class owns/clears, just read fresh each tick.
    werase(win_input);
    waddstr(win_input, ("> " + key_input.LINE).c_str());
    // Cursor: LINE only ever grows/shrinks at its end (see
    // KEYBOARD_INPUT::keyboard_input() - no mid-line editing), so the cursor
    // is always right after the last typed char. A reverse-video space
    // there stands in for a real terminal cursor (curs_set(0) above hides
    // that entirely).
    wattron(win_input, A_REVERSE);
    waddch(win_input, ' ');
    wattroff(win_input, A_REVERSE);
    wrefresh(win_input);
}

#endif
