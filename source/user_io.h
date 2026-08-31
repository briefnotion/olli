#ifndef user_io_h
#define user_io_h

#include <string>
#include <vector>
#include <deque>
#include <termios.h>
#include <filesystem>
#include <optional>
#include <atomic>

#include "fled_time.h"

// ----

// Page Up/Down, as recognized by KEYBOARD_INPUT::keyboard_input()'s escape-
// sequence parsing - consumed-and-cleared by IO_WORKER_CLASS::thread_main()
// each tick (see io_worker.h/.cpp) and forwarded into
// OUTPUT_CLASS::display_with_ncurses() to scroll whichever ncurses panel
// currently has focus.
enum class SCROLL_KEY { NONE, PAGE_UP, PAGE_DOWN };

// ----

// All the ways a person can talk to olli funnel through here. Speech-to-text
// input (Voca) is no longer a separate process talking through files - it's
// in-process now, owned privately by IO_WORKER_CLASS (see io_worker.h).
// io_worker.cpp drains its transcripts each tick and feeds them into
// KEYBOARD_INPUT's LINE/INTERRUPTED/ENTER_PRESSED below, the same fields a
// typed line sets.

class KEYBOARD_INPUT_PROPERTIES
{
    public:

    // atomic, not plain bool: toggled from ollama_system::dispatch_tool_call()
    // (main thread, during run_automation_task) to stop IO_WORKER_CLASS's
    // background thread from also reading stdin while that tool's own
    // separate blocking keyboard loop (tools.cpp) is - two real threads
    // touching this, not just two logical call sites.
    std::atomic<bool> ENABLED{false};

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

        // Set by a recognized Page Up/Down escape sequence (ESC [ 5/6 ~) -
        // like EXIT_REQUESTED above, read-and-cleared explicitly by the
        // caller each tick (IO_WORKER_CLASS::thread_main()), not touched by
        // reset(). See keyboard_input()'s own comment for the parsing.
        SCROLL_KEY SCROLL_REQUEST = SCROLL_KEY::NONE;

        // Set by a Tab keypress (byte 9), repurposed away from literal-tab
        // insertion the same way Ctrl+C is repurposed away from literal
        // insertion above - cycles which ncurses panel SCROLL_REQUEST
        // applies to. Same read-and-cleared-by-caller contract as
        // SCROLL_REQUEST.
        bool FOCUS_CYCLE_REQUESTED = false;

        KEYBOARD_INPUT();
        ~KEYBOARD_INPUT();

        void keyboard_input();

        void reset();
};

// ----

#include "comms.h" // for get_response() below

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

// Same reasoning/mechanism as WINDOW above, for ncurses' panel library
// (panel.h, only included in user_io.cpp) - matches "typedef struct panel
// PANEL;" there. Panels are what actually make win_chat/win_thinking's
// overlap correct: a plain window's own refresh only knows about writes to
// its own buffer, so it has no way to notice (and repaint over) another
// window that drew on top of it and then went away - which is exactly the
// bug a first, panel-free version of this hit (the thinking box's border
// left behind after it closed). The panel library tracks that stacking
// itself and repairs obscured regions correctly on update_panels().
struct panel;
typedef struct panel PANEL;

// ----
// Scrollable/word-wrapped/resize-safe text panel model, shared by
// win_system, win_chat, and win_tools (see OUTPUT_CLASS below). Each panel
// keeps its own logical text (entries) separate from the word-wrapped
// display cache (wrapped_lines) built from it at a given width - this is
// what makes a terminal resize recoverable: ncurses' own wresize() does NOT
// re-wrap a window's existing content, it just changes the buffer
// dimensions, so relying on that (the old design's scrollok(TRUE) approach)
// corrupts the transcript on resize. Rebuilding wrapped_lines from entries
// at the new width instead sidesteps that entirely. Method bodies live in
// user_io.cpp, same split as OUTPUT_CLASS itself.

// One already-wrapped display row, with the ncurses attribute it should be
// drawn with (e.g. dimmed grey for user_input vs plain for chat_response).
struct NCURSES_WRAPPED_LINE
{
    std::string text;
    int attr = 0;
};

// One logical (unwrapped) paragraph pushed into a panel - a chat_response
// chunk, a user_input line, one tool name. Consecutive appends sharing the
// same attr get merged into the same entry (see NCURSES_TEXT_PANEL::append()
// in user_io.cpp) so a streamed reply arriving in many small pieces still
// wraps as one continuous paragraph instead of re-wrapping at each chunk's
// boundary. wrapped_line_count tracks how many of the panel's current
// wrapped_lines this entry contributes, so eviction (once max_wrapped_lines
// is exceeded) knows how many lines to drop along with it.
struct NCURSES_TEXT_ENTRY
{
    std::string raw_text;
    int attr = 0;
    int wrapped_line_count = 0;
};

struct NCURSES_TEXT_PANEL
{
    std::deque<NCURSES_TEXT_ENTRY> entries;         // source of truth
    std::deque<NCURSES_WRAPPED_LINE> wrapped_lines; // derived cache
    int wrap_width = 0;            // width wrapped_lines was built for; 0 = never wrapped yet
    int scroll_top_line = 0;       // absolute index into wrapped_lines
    bool pinned_to_bottom = true;  // false once the user scrolls away from the tail
    size_t max_wrapped_lines = 0;  // 0 = unbounded (win_tools - see OUTPUT_CLASS)

    // max_lines: 0 = unbounded. Chosen per-panel where OUTPUT_CLASS declares
    // its system_panel/chat_panel/tools_panel members below.
    explicit NCURSES_TEXT_PANEL(size_t max_lines = 0) : max_wrapped_lines(max_lines) {}

    // Splits text on '\n' into paragraphs, word-wraps each at width, and
    // appends to (or merges into, per the attr rule above) entries/
    // wrapped_lines, evicting from the front past max_wrapped_lines.
    void append(const std::string& text, int attr, int width);

    // Tools panel only: tool_names is already a fresh, complete list every
    // tick (not an accumulating log), so it fully replaces entries/
    // wrapped_lines rather than appending.
    void rebuild_from(const std::vector<std::string>& fresh_lines, int width);

    // Rebuilds wrapped_lines from entries at a new width - the single
    // mechanism behind both word-wrap and resize recovery (see
    // OUTPUT_CLASS::ncurses_render_panel()'s comment in user_io.cpp).
    void rewrap(int new_width);

    // Moves scroll_top_line by delta_lines (negative = up/back through
    // history), clamping into range and re-pinning to the bottom if the
    // result reaches/passes the true bottom. Named scroll_by(), not
    // scroll() - ncursesw's <curses.h> #defines scroll(win) as a macro
    // (wscrl(win,1)), which would otherwise silently mangle this call.
    void scroll_by(int delta_lines, int viewport_h);

    // Keeps scroll_top_line valid for the given viewport height - follows
    // the tail when pinned_to_bottom, otherwise clamps the stored value.
    void clamp_scroll(int viewport_h);

    // Drops lines from the front of wrapped_lines (and their owning
    // entries) until back under max_wrapped_lines - a no-op when
    // max_wrapped_lines is 0 (unbounded). Called by append()/rewrap()
    // after either changes wrapped_lines' size.
    void evict_to_cap();
};

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
 * populated by get_response(), which pulls (and clears) a passed-in
 * COMMS's own response_buffer/thinking_buffer (see comms.h). That's a
 * separate cross-thread hop, since that COMMS lives on whichever
 * ollama_system's chat_thread is streaming a reply; system_message/
 * user_input are plain public strings any same-thread caller can append
 * to directly.
 *
 * display() is meant to be called once per main-loop tick (see main.cpp),
 * right before the loop goes back around - it prints whatever's accumulated
 * in all four buckets and clears them, the same check-act-clear shape
 * ollama_system::write_to_tts() already uses for tts_buffer.
 */

// Which OUTPUT_CLASS display path this run uses, chosen once here rather
// than duplicated per call site. Flip to false to fall back to the plain
// scrolling display() if ncurses ever needs to be ruled out (a bug, a
// terminal it doesn't handle well, etc). Anything that constructs its own
// KEYBOARD_INPUT must set that instance's PROPS.RAW_ECHO = !USE_NCURSES too
// (see KEYBOARD_INPUT_PROPERTIES::RAW_ECHO's comment above) - raw cout
// echoing from a keypress corrupts the ncurses screen buffer if left at its
// default true while ncurses owns the screen.
inline constexpr bool USE_NCURSES = true;

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

        // ---- chat log - see append_to_chat_log() in user_io.cpp ----
        // nullopt until the first line is actually written this run; true/
        // false after that tracks whether the LAST line written was the
        // user's or the assistant's, so a run of streamed chat_response
        // chunks (which arrive in many small pieces, not one shot - unlike
        // user_input) gets appended as one continuous block under a single
        // "Olli: " label instead of repeating the label every chunk.
        std::optional<bool> chat_log_last_speaker_was_user;

        // Appends text to chat_log_path (theatrical-script style:
        // chat_log_user_label/"Olli: " labels, only on a speaker change) -
        // a no-op if
        // chat_log_path was never set (see its own comment below) or text
        // is empty. Flat text, not JSON: this is meant to be human-readable
        // and simply appendable, independent of history.json's own
        // structured rewrite-the-whole-file persistence. Called from both
        // display() and display_with_ncurses() at the same points
        // user_input/chat_response get shown, so the log always matches
        // exactly what the user actually saw - not threaded through
        // ollama_system's own response_buffer cross-thread plumbing.
        void append_to_chat_log(bool is_user, const std::string& text);

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
        // True for a couple seconds after in_thinking_block drops (chat
        // content arrived, ending the block) - the box stays visible for
        // that stretch instead of vanishing the instant it happens, so it
        // actually registers on screen. Cleared early if fresh thinking
        // content starts again first. TIMED_IS_READY (fled_time.h) tracks
        // the countdown - already used elsewhere in this codebase (e.g.
        // sidetrack.cpp's idle-wait timers) for the same "has enough time
        // passed" shape.
        bool ncurses_thinking_closing = false;
        TIMED_IS_READY ncurses_thinking_close_timer;

        // Set from has_colors() at init - guards every COLOR_PAIR() use
        // below, since attron()-ing a pair that was never init_pair()'d
        // (because the terminal doesn't support color at all) is undefined.
        bool ncurses_colors_available = false;
        WINDOW* win_system = nullptr;
        WINDOW* win_thinking = nullptr;
        // Interior-only subwindow (derwin) of win_thinking, offset one row/
        // col in from its border on every side - streamed thinking text is
        // written/scrolled in here instead of directly into win_thinking,
        // so a window's ordinary line-wrap-at-full-width behavior can't
        // write over the border's left/right columns. Always created and
        // torn down together with win_thinking (see ncurses_layout()) since
        // a derived window doesn't stay valid if its parent is resized/
        // moved out from under it.
        WINDOW* win_thinking_content = nullptr;
        WINDOW* win_chat = nullptr;
        WINDOW* win_input = nullptr;

        // Logical text buffers backing win_system/win_chat/win_tools (see
        // NCURSES_TEXT_PANEL above) - the actual source of truth for what's
        // shown, rendered into each window fresh every tick by
        // ncurses_render_panel(). Caps: chat gets 5000 wrapped lines
        // (~500-750KB, generous scrollback for a long session); system gets
        // 500 (a low-volume status strip); tools stays unbounded since
        // rebuild_from() always replaces its content wholesale from
        // tool_names, which is already a small, fresh, non-accumulating
        // list each tick - there's no history to cap.
        NCURSES_TEXT_PANEL system_panel{500};
        NCURSES_TEXT_PANEL chat_panel{5000};
        NCURSES_TEXT_PANEL tools_panel{0};

        // Which of the three scrollable panels Page Up/Down currently
        // applies to - cycled by Tab (see KEYBOARD_INPUT::FOCUS_CYCLE_REQUESTED).
        enum class NCURSES_FOCUS { CHAT, SYSTEM, TOOLS };
        NCURSES_FOCUS ncurses_focus = NCURSES_FOCUS::CHAT;

        // Current height of win_input in rows (see ncurses_update_input_box())
        // and the cached width shared by win_system/win_chat/win_input (not
        // win_tools, which has its own fixed tools_w) - both needed by
        // ncurses_update_input_box() to resize win_input/win_chat without
        // duplicating ncurses_layout()'s own row math.
        int ncurses_input_rows = 1;
        int ncurses_main_w = 0;

        // Right-side panel listing available tool names (see
        // display_with_ncurses()'s tool_names parameter) - full height,
        // never overlaps anything else, so it stays a plain window like
        // win_system/win_input rather than needing a panel.
        WINDOW* win_tools = nullptr;

        // win_chat and win_thinking are the only two windows that ever
        // spatially overlap, so those two - and only those two - are
        // wrapped in panels (win_system/win_input never overlap anything
        // and stay plain windows, refreshed with plain wrefresh()).
        // Repositioning either paneled window must go through
        // move_panel(), never a raw mvwin() on the WINDOW directly, or the
        // panel library's own position bookkeeping falls out of sync with
        // reality.
        PANEL* pan_chat = nullptr;
        PANEL* pan_thinking = nullptr;

        void ncurses_layout(); // full rebuild - all windows, for a resize
        void ncurses_update_thinking_box(); // just the floating thinking box
        void ncurses_commit_panels(); // update_panels()+doupdate() - see .cpp

        // Shared row-geometry math for the system/chat/input stack - used by
        // both ncurses_layout() (full rebuild) and ncurses_update_input_box()
        // (input-height-only change), so the two never disagree about where
        // each window sits. Takes the input box's current row count and
        // hands back chat's row/height and the input row.
        void ncurses_compute_rows(int input_rows, int& chat_row, int& chat_h, int& input_row) const;

        // Renders one NCURSES_TEXT_PANEL's current visible slice into win -
        // rewrapping first if win's width doesn't match what the panel was
        // last wrapped at (this lazy check is also the resize-recovery
        // mechanism - see the .cpp comment). start_row/content_col leave room
        // for a header (used by win_tools' "Tools:" label); paneled selects
        // ncurses_commit_panels() (win_chat) vs a plain wrefresh().
        void ncurses_render_panel(WINDOW* win, NCURSES_TEXT_PANEL& panel, int start_row,
                                   bool paneled, const std::string& header = "", int content_col = 0);

        // Word-wraps the live-typed line, grows/shrinks win_input (and
        // correspondingly win_chat) only when the needed row count actually
        // changes, and draws it. Replaces display_with_ncurses()'s old
        // fixed-one-row input block.
        void ncurses_update_input_box(const std::string& input_from_user_echo);

        // Bolds whichever separator segment (the hline above win_system/
        // win_chat, or the vline left of win_tools) matches ncurses_focus,
        // plain otherwise - the only visible feedback for Tab/Page Up/Down
        // given the layout itself can't change to show focus.
        void ncurses_draw_focus_indicators();

    public:
        std::string system_message = "";
        std::string user_input = "";
        std::string chat_response = "";
        std::string chat_thinking = "";

        // Set once by main.cpp (alongside ollama_system::PROPS.OLLI_DIRECTORY
        // - same profile directory, see Settings::get_settings_path()) right
        // after settings load. Left empty (the default) disables the chat
        // log entirely - append_to_chat_log() no-ops on an empty path.
        std::filesystem::path chat_log_path;

        // The speaker label append_to_chat_log() writes for user_input -
        // set by main.cpp from the profile name given at startup (argv[1]
        // or the "What is your name?" prompt), capitalized. Left at the
        // "You" default when there's no real name (the shared/no-profile
        // case).
        std::string chat_log_user_label = "You";

        ~OUTPUT_CLASS();

        // Archives whatever's currently at chat_log_path into a
        // "chat_logs" subdirectory of its parent, renamed
        // "<YYMMDD.HHMM>.chat_log.txt" (see timestamp_prefix() in
        // helper_olli.h) - leaves chat_log_path itself free, so the next
        // append_to_chat_log() call starts a brand-new file there, same as
        // a fresh run. A no-op if chat_log_path was never set, doesn't
        // exist, or is empty (nothing worth archiving). Called from
        // main.cpp at shutdown, and from main.cpp's main loop when
        // sidetrack's context-clear routine signals it just cleared
        // history (see SIDETRACK_SIGNALS::CONTEXT_CLEARED_SIGNAL in
        // sidetrack.h) - both moments a conversation is considered "over,"
        // independent of each other.
        void close_chat_log();

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

        // Pulls and clears comms.response_buffer/comms.thinking_buffer/
        // comms.log_buffer into this instance's own chat_response/
        // chat_thinking/system_message, under output_buffer_mutex
        // (comms.h). Only ever touches the passed-in COMMS, not the rest
        // of whatever ollama_system instance it belongs to - hence taking
        // COMMS& directly rather than ollama_system&. Safe to call every
        // tick even if comms hasn't picked up anything new since the last
        // call.
        void get_response(COMMS& comms);

        // Prints whatever's in all four buckets to the screen, then clears
        // them.
        void display();

        // Alternate to display() above: same four buckets, rendered into a
        // windowed ncurses layout (system message strip, a thinking window
        // that appears/disappears with the thinking block, a scrolling chat
        // transcript, an input line) instead of one flat scrolling stream.
        // input_from_user_echo: live mirror of what's currently being
        // typed (IO_WORKER_CLASS::thread_main() passes its own
        // input_from_user_echo - see io_worker.cpp), shown in the input
        // window as-is. comms: IO_WORKER_CLASS's own comms_buffer, passed
        // through same as input_from_user_echo. Only one of display()/
        // display_with_ncurses() should be used for a given run (see
        // main.cpp) - see RAW_ECHO on KEYBOARD_INPUT_PROPERTIES for the
        // other half of that switch. tool_names: current list of available
        // tool names (see IO_WORKER_CLASS::exchange(), io_worker.cpp),
        // rendered one per line in a right-side panel spanning the full
        // screen height, scrollable like the other panels if it overflows.
        // scroll_request/focus_cycle_requested: this tick's keyboard scroll
        // signals (see SCROLL_KEY above and KEYBOARD_INPUT::SCROLL_REQUEST/
        // FOCUS_CYCLE_REQUESTED), relayed through IO_WORKER_CLASS::thread_main().
        void display_with_ncurses(const std::string& input_from_user_echo, const COMMS& comms,
                                   const std::vector<std::string>& tool_names,
                                   SCROLL_KEY scroll_request, bool focus_cycle_requested);
};

#endif
