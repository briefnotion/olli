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

// get_response() takes COMMS& directly (comms.h, included via user_io.h)
// rather than ollama_system& - no need for olla.h here at all, which
// sidesteps what would otherwise be a circular include (olla.h -> system.h
// -> user_io.h). timestamp_prefix() below used to come in transitively
// through olla.h -> helper_olli.h; needs its own include now that path's
// gone.
#include "helper_olli.h"

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

    // Bright, distinct pair for a background task-runner automation
    // instance's own comms (TOOL_TASK_RUNNER::handle_tool(), tools.cpp) -
    // set on that instance's own instance_comms.INPUT_FROM_LLM_COLOR/
    // INPUT_FROM_USER_COLOR (comms.h) so its output reads as visually
    // separate from the main chat's white/grey. init_pair() calls below,
    // guarded by ncurses_colors_available, same as pair 1 above.
    constexpr int PAIR_TASK_RUNNER_LLM = 2;
    constexpr int PAIR_TASK_RUNNER_USER = 3;

    // Same idea, for a delegator sub-agent's own comms
    // (TOOL_DELEGATOR::handle_tool(), tools.cpp) - distinct from both the
    // main chat's white/grey and the task-runner's cyan/yellow above.
    constexpr int PAIR_DELEGATOR_LLM = 4;
    constexpr int PAIR_DELEGATOR_USER = 5;

    // How long the thinking box lingers after in_thinking_block drops
    // before it actually closes - see ncurses_thinking_closing's comment
    // in user_io.h.
    constexpr double THINKING_BOX_LINGER_MS = 2000.0;

    // Rows reserved for the system-message strip at the top of the screen -
    // shared by ncurses_layout() and OUTPUT_CLASS::ncurses_compute_rows() so
    // the two always agree on where win_chat starts.
    constexpr int SYS_PANEL_ROWS = 3;

    // Cap on how tall win_input is allowed to grow (see
    // OUTPUT_CLASS::ncurses_update_input_box()) - beyond this a very long
    // typed/pasted line would eat too much of a small terminal's chat area.
    constexpr int MAX_INPUT_ROWS = 6;

    // Greedy word-wrap of a single already-newline-free paragraph at width
    // columns - used by NCURSES_TEXT_PANEL::append()/rewrap() (user_io.h)
    // and by ncurses_update_input_box(). A token longer than width on its
    // own is hard-broken into width-sized chunks rather than left to
    // overflow. An empty paragraph still yields one empty line, so blank-
    // line spacing in the original text survives wrapping.
    std::vector<std::string> word_wrap(const std::string& paragraph, int width)
    {
        if (width <= 0) return { paragraph };
        if (paragraph.empty()) return { std::string() };

        std::vector<std::string> lines;
        std::string current_line;
        size_t i = 0;
        size_t n = paragraph.size();

        while (i < n)
        {
            while (i < n && paragraph[i] == ' ') ++i;
            if (i >= n) break;

            size_t tok_start = i;
            while (i < n && paragraph[i] != ' ') ++i;
            std::string token = paragraph.substr(tok_start, i - tok_start);

            while (static_cast<int>(token.size()) > width)
            {
                if (!current_line.empty())
                {
                    lines.push_back(current_line);
                    current_line.clear();
                }
                lines.push_back(token.substr(0, static_cast<size_t>(width)));
                token = token.substr(static_cast<size_t>(width));
            }

            if (current_line.empty())
            {
                current_line = token;
            }
            else if (static_cast<int>(current_line.size() + 1 + token.size()) <= width)
            {
                current_line += ' ';
                current_line += token;
            }
            else
            {
                lines.push_back(current_line);
                current_line = token;
            }
        }

        if (!current_line.empty() || lines.empty()) lines.push_back(current_line);
        return lines;
    }

    // Fixed-width chunking, not word-wrap - preserves every character
    // verbatim (including trailing spaces). Used only for the input box:
    // its cursor column is derived from the wrapped text's own length, and
    // word_wrap() silently drops a trailing space at a wrap point, which
    // makes the cursor fail to advance on a space keystroke.
    std::vector<std::string> hard_wrap(const std::string& text, int width)
    {
        if (width <= 0) return { text };
        if (text.empty()) return { std::string() };

        std::vector<std::string> lines;
        for (size_t i = 0; i < text.size(); i += static_cast<size_t>(width))
        {
            lines.push_back(text.substr(i, static_cast<size_t>(width)));
        }
        if (lines.back().size() == static_cast<size_t>(width)) lines.push_back(std::string());
        return lines;
    }
}

// ---- NCURSES_TEXT_PANEL (user_io.h) ----

void NCURSES_TEXT_PANEL::evict_to_cap()
{
    if (max_wrapped_lines == 0) return;

    while (wrapped_lines.size() > max_wrapped_lines && !entries.empty())
    {
        wrapped_lines.pop_front();
        if (!pinned_to_bottom && scroll_top_line > 0) --scroll_top_line;

        NCURSES_TEXT_ENTRY& front = entries.front();
        --front.wrapped_line_count;
        if (front.wrapped_line_count <= 0) entries.pop_front();
    }
}

void NCURSES_TEXT_PANEL::rewrap(int new_width)
{
    wrap_width = new_width;
    wrapped_lines.clear();

    for (auto& e : entries)
    {
        std::vector<std::string> lines = word_wrap(e.raw_text, new_width);
        e.wrapped_line_count = static_cast<int>(lines.size());
        for (auto& l : lines) wrapped_lines.push_back({ l, e.attr });
    }

    evict_to_cap();
}

void NCURSES_TEXT_PANEL::append(const std::string& text, int attr, int width)
{
    if (text.empty()) return;

    // Catch the buffer up to the current width FIRST, so the new/merged
    // entry below lands on a wrapped_lines cache that's already fully
    // consistent - otherwise only the newly appended text would be wrapped
    // at the new width while everything before it stayed stale, and the
    // wrap_width bookkeeping this sets afterward would incorrectly mark the
    // whole buffer as up to date.
    if (width != wrap_width) rewrap(width);

    // Split on embedded newlines into paragraphs - a newline is a
    // deliberate break, not a wrap point, so each piece is wrapped/merged
    // independently.
    std::vector<std::string> paragraphs;
    size_t start = 0;
    while (true)
    {
        size_t pos = text.find('\n', start);
        if (pos == std::string::npos)
        {
            paragraphs.push_back(text.substr(start));
            break;
        }
        paragraphs.push_back(text.substr(start, pos - start));
        start = pos + 1;
    }

    for (size_t p = 0; p < paragraphs.size(); ++p)
    {
        const std::string& para = paragraphs[p];

        // Only the FIRST paragraph of this call can continue the existing
        // last entry - this is what keeps a streamed chat_response arriving
        // in many small chunks reading as one continuous wrapped paragraph
        // instead of re-wrapping at each chunk's boundary. Any later
        // paragraph in this same call (text contained an embedded '\n')
        // always starts fresh.
        bool merge = (p == 0) && !entries.empty() && entries.back().attr == attr;

        if (merge)
        {
            NCURSES_TEXT_ENTRY& e = entries.back();
            // Drop this entry's current contribution from the tail of
            // wrapped_lines (it's always the most recent entry, so always
            // at the tail) before re-wrapping the merged text.
            for (int i = 0; i < e.wrapped_line_count && !wrapped_lines.empty(); ++i)
            {
                wrapped_lines.pop_back();
            }
            e.raw_text += para;
            std::vector<std::string> lines = word_wrap(e.raw_text, width);
            e.wrapped_line_count = static_cast<int>(lines.size());
            for (auto& l : lines) wrapped_lines.push_back({ l, attr });
        }
        else
        {
            NCURSES_TEXT_ENTRY e;
            e.raw_text = para;
            e.attr = attr;
            std::vector<std::string> lines = word_wrap(para, width);
            e.wrapped_line_count = static_cast<int>(lines.size());
            for (auto& l : lines) wrapped_lines.push_back({ l, attr });
            entries.push_back(std::move(e));
        }
    }

    evict_to_cap();
}

void NCURSES_TEXT_PANEL::rebuild_from(const std::vector<std::string>& fresh_lines, int width)
{
    entries.clear();
    wrapped_lines.clear();
    wrap_width = width;

    for (const auto& line : fresh_lines)
    {
        NCURSES_TEXT_ENTRY e;
        e.raw_text = line;
        e.attr = 0;
        e.wrapped_line_count = 1;

        // Truncated, not word-wrapped - one row per tool name; a name too
        // long to fit is just cut off rather than spilling onto a second row.
        std::string clipped = (width > 0 && static_cast<int>(line.size()) > width)
                               ? line.substr(0, static_cast<size_t>(width)) : line;
        wrapped_lines.push_back({ clipped, 0 });
        entries.push_back(std::move(e));
    }

    // max_wrapped_lines is 0 (unbounded) for every panel that uses
    // rebuild_from() (win_tools - see OUTPUT_CLASS), so no eviction here.
}

void NCURSES_TEXT_PANEL::clamp_scroll(int viewport_h)
{
    int total = static_cast<int>(wrapped_lines.size());
    int max_top = (total > viewport_h) ? (total - viewport_h) : 0;

    if (pinned_to_bottom)
    {
        scroll_top_line = max_top;
    }
    else
    {
        if (scroll_top_line > max_top) scroll_top_line = max_top;
        if (scroll_top_line < 0) scroll_top_line = 0;
    }
}

void NCURSES_TEXT_PANEL::scroll_by(int delta_lines, int viewport_h)
{
    int total = static_cast<int>(wrapped_lines.size());
    int max_top = (total > viewport_h) ? (total - viewport_h) : 0;
    int new_top = scroll_top_line + delta_lines;

    if (new_top >= max_top)
    {
        new_top = max_top;
        pinned_to_bottom = true; // reached/passed the true bottom - resume auto-follow
    }
    else
    {
        pinned_to_bottom = false;
    }
    if (new_top < 0) new_top = 0;

    scroll_top_line = new_top;
}

// ----

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
            else if (ch == 9) // Tab - focus-cycle, repurposed away from
                               // literal-tab insertion the same way Ctrl+C
                               // above is repurposed away from literal
                               // insertion - see FOCUS_CYCLE_REQUESTED's
                               // comment in user_io.h
            {
                FOCUS_CYCLE_REQUESTED = true;
            }
            else if (ch == 27) // ESC - possible CSI escape sequence; only
                                // Page Up/Down (ESC [ 5/6 ~) are recognized
                                // - see SCROLL_REQUEST's comment in user_io.h
            {
                char seq0 = 0, code = 0, tilde = 0;
                if (read(STDIN_FILENO, &seq0, 1) > 0 && seq0 == '[' &&
                    read(STDIN_FILENO, &code, 1) > 0)
                {
                    if (code == '5' || code == '6')
                    {
                        read(STDIN_FILENO, &tilde, 1); // consume trailing '~'
                        SCROLL_REQUEST = (code == '5') ? SCROLL_KEY::PAGE_UP : SCROLL_KEY::PAGE_DOWN;
                    }
                    // any other CSI code (arrow keys, Home/End, ...) -
                    // recognized as "not plain text" and discarded; no
                    // handling defined for those yet
                }
                // Every byte read above (seq0/code/tilde) - and the ESC
                // itself - is consumed here either way, never reaching
                // LINE, so an incomplete/unrecognized sequence can't
                // corrupt typed text. The nested read()s reuse the same
                // non-blocking (VMIN=0/VTIME=0) fd as the outer loop, so
                // this can't hang even when fewer bytes are available than
                // expected.
            }
            else
            {
                LINE += ch;
                if (PROPS.RAW_ECHO) std::cout << ch << std::flush;
                INTERRUPTED = true;
            }


            enter_ready.start_timer(); // Reset the timer on each key press
        }

        // Voca (speech-to-text) input is drained separately in
        // IO_WORKER_CLASS::thread_main() - see its popVocaEvent().

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

void OUTPUT_CLASS::get_response(COMMS& comms)
{
    std::lock_guard<std::mutex> lock(output_buffer_mutex);
    chat_response += comms.INPUT_FROM_LLM;
    comms.INPUT_FROM_LLM.clear();
    chat_thinking += comms.INPUT_FROM_THINKING;
    comms.INPUT_FROM_THINKING.clear();
    system_message += comms.INPUT_FROM_SYSTEM;
    comms.INPUT_FROM_SYSTEM.clear();
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
 * Existing windows are resized/moved in place (wresize + mvwin, or
 * move_panel() for the paneled win_chat) rather than destroyed and
 * recreated. Their actual CONTENT is no longer this function's concern at
 * all, though: win_system/win_chat/win_tools are rendered from their own
 * NCURSES_TEXT_PANEL (user_io.h) by ncurses_render_panel(), called
 * unconditionally every tick from display_with_ncurses() - including the
 * very next moment after this function runs, in the same tick. That's what
 * makes a resize recoverable: ncurses' own wresize() does NOT re-wrap a
 * window's existing character-matrix content to the new width (the old
 * design's failure mode), but rebuilding wrapped_lines from each panel's
 * untouched raw entries at the new width, which ncurses_render_panel()'s
 * own lazy width check does, sidesteps that entirely - so this function
 * only needs to get geometry right, not content.
 *
 * Layout, top to bottom: a fixed SYS_PANEL_ROWS-line system-message strip, a
 * separator, the chat transcript filling everything down to a separator and
 * an input line (ncurses_input_rows tall - see ncurses_update_input_box(),
 * which grows/shrinks it independently of a full layout rebuild). win_chat's
 * height/position never depends on thinking visibility - the thinking box
 * floats ON TOP of it instead of claiming its own reserved rows (that was
 * the old design: a full-width strip that inserted itself into the flow, so
 * every "thinking" block bumped the whole transcript and input line down a
 * few lines and back). It's a plain WINDOW like the others, just given a
 * single-line box() border, tucked into the upper-right corner of win_chat
 * rather than spanning the full width - recreated fresh each time it's
 * shown, since unlike win_chat it's a short-lived per-turn display with no
 * scrollback worth preserving across a resize.
 */
void OUTPUT_CLASS::ncurses_layout()
{
    int screen_h = ncurses_screen_h;
    int screen_w = ncurses_screen_w;

    // Right-side tools panel: fixed width, full height, never overlaps
    // anything (see win_tools' comment in user_io.h) - reserving its width
    // here shrinks every other window's width to make room. Skipped
    // entirely on a too-narrow terminal rather than squeezing everything
    // else unreadably thin.
    //
    // 'gap' is its own dedicated column for the vline separator, distinct
    // from main_w - win_system/win_chat/win_input span exactly [0, main_w),
    // so the vline at column main_w never falls inside any of their own
    // window areas. Sharing that last column with them (an earlier version
    // of this did) meant their own wrefresh() would silently paint over the
    // vline with blank content on every row except the separator rows drawn
    // directly on stdscr - the line only appeared to "flicker in and out."
    const int tools_w = 22;
    const int gap = 1;
    bool show_tools_panel = (screen_w - tools_w - gap) >= 20;
    int main_w = show_tools_panel ? (screen_w - tools_w - gap) : screen_w;
    int tools_col = main_w + gap;
    ncurses_main_w = main_w;

    erase();   // stdscr - just the separator lines live directly on it
    refresh();

    int chat_row = 0, chat_h = 0, input_row = 0;
    ncurses_compute_rows(ncurses_input_rows, chat_row, chat_h, input_row);

    if (win_system == nullptr)
    {
        win_system = newwin(SYS_PANEL_ROWS, main_w, 0, 0);
    }
    else
    {
        wresize(win_system, SYS_PANEL_ROWS, main_w);
        mvwin(win_system, 0, 0);
    }

    if (win_chat == nullptr)
    {
        win_chat = newwin(chat_h, main_w, chat_row, 0);
        pan_chat = new_panel(win_chat);
    }
    else
    {
        wresize(win_chat, chat_h, main_w);
        // move_panel(), not mvwin() directly - win_chat is paneled (see
        // user_io.h), and moving its window without going through the
        // panel library would leave the panel's own position bookkeeping
        // out of sync with reality.
        move_panel(pan_chat, chat_row, 0);
    }

    if (win_input == nullptr)
    {
        win_input = newwin(ncurses_input_rows, main_w, input_row, 0);
    }
    else
    {
        wresize(win_input, ncurses_input_rows, main_w);
        mvwin(win_input, input_row, 0);
    }

    if (show_tools_panel)
    {
        if (win_tools == nullptr)
        {
            win_tools = newwin(screen_h, tools_w, 0, tools_col);
        }
        else
        {
            wresize(win_tools, screen_h, tools_w);
            mvwin(win_tools, 0, tools_col);
        }
    }
    else if (win_tools != nullptr)
    {
        delwin(win_tools);
        win_tools = nullptr;
    }

    // Draws all three separators (both hlines plus the tools vline),
    // bolding whichever one matches ncurses_focus - the single place that
    // owns separator drawing now, since it needs win_chat's up-to-date
    // geometry to place them correctly regardless of what changed.
    ncurses_draw_focus_indicators();

    ncurses_update_thinking_box();
}

// Shared row-geometry math for the system/chat/input stack - the single
// source of truth both ncurses_layout() (full rebuild) and
// ncurses_update_input_box() (input-height-only change) use, so the two
// never disagree about where each window sits. chat_row is fixed (right
// below the system strip/separator) regardless of input_rows - only
// chat_h/input_row move as the input box grows/shrinks from the bottom.
void OUTPUT_CLASS::ncurses_compute_rows(int input_rows, int& chat_row, int& chat_h, int& input_row) const
{
    chat_row = SYS_PANEL_ROWS + 1; // +1 for the separator below win_system
    chat_h = ncurses_screen_h - chat_row - 1 /* separator before win_input */ - input_rows;
    if (chat_h < 1) chat_h = 1;
    input_row = chat_row + chat_h + 1;
}

// Bolds whichever separator segment matches ncurses_focus, plain otherwise -
// the only visible feedback for Tab/Page Up/Down, since the layout itself
// never changes shape to show focus. Each of the three scrollable panels
// gets its own distinct segment so they can't be confused: win_system's is
// the hline directly below it (shared boundary with win_chat would be
// ambiguous otherwise, so win_chat instead claims the hline below IT,
// between win_chat and win_input); win_tools claims the vline to its left.
// Reads win_chat's current geometry via getmaxyx()/getbegyx() rather than
// duplicating ncurses_layout()'s row math, so this stays correct regardless
// of what just changed (a full relayout, or only ncurses_update_input_box()
// moving the chat/input boundary).
void OUTPUT_CLASS::ncurses_draw_focus_indicators()
{
    if (win_chat == nullptr) return;

    int chat_h_now = 0, chat_w_now = 0, chat_row_now = 0, dummy_col = 0;
    getmaxyx(win_chat, chat_h_now, chat_w_now);
    getbegyx(win_chat, chat_row_now, dummy_col);
    (void)chat_w_now;
    (void)dummy_col;

    int sys_sep_row = chat_row_now - 1;            // between win_system and win_chat
    int chat_sep_row = chat_row_now + chat_h_now;  // between win_chat and win_input

    if (ncurses_focus == NCURSES_FOCUS::SYSTEM) attron(A_BOLD);
    mvhline(sys_sep_row, 0, ACS_HLINE, ncurses_main_w);
    if (ncurses_focus == NCURSES_FOCUS::SYSTEM) attroff(A_BOLD);

    if (ncurses_focus == NCURSES_FOCUS::CHAT) attron(A_BOLD);
    mvhline(chat_sep_row, 0, ACS_HLINE, ncurses_main_w);
    if (ncurses_focus == NCURSES_FOCUS::CHAT) attroff(A_BOLD);

    if (win_tools != nullptr)
    {
        int tools_row = 0, tools_col = 0;
        getbegyx(win_tools, tools_row, tools_col);
        (void)tools_row;

        if (ncurses_focus == NCURSES_FOCUS::TOOLS) attron(A_BOLD);
        mvvline(0, tools_col - 1, ACS_VLINE, ncurses_screen_h);
        if (ncurses_focus == NCURSES_FOCUS::TOOLS) attroff(A_BOLD);
    }

    refresh(); // stdscr - pushes the (re-)drawn separators
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

// Renders one NCURSES_TEXT_PANEL's current visible slice into win. Rewraps
// first if win's width (minus content_col's margin on each side) doesn't
// match what the panel was last wrapped at - this lazy check is the actual
// resize-recovery mechanism (see ncurses_layout()'s comment): since this
// runs unconditionally every tick, the very first render after any width
// change catches it and rebuilds wrapped_lines from the panel's untouched
// raw entries at the new width. start_row/content_col reserve room for an
// optional header (win_tools' "Tools:" label); paneled selects
// ncurses_commit_panels() (win_chat, which overlaps win_thinking) vs a
// plain wrefresh() (win_system/win_tools, which don't overlap anything).
void OUTPUT_CLASS::ncurses_render_panel(WINDOW* win, NCURSES_TEXT_PANEL& panel, int start_row,
                                         bool paneled, const std::string& header, int content_col)
{
    if (win == nullptr) return;

    int h = 0, w = 0;
    getmaxyx(win, h, w);
    int content_width = w - content_col * 2;
    if (content_width < 1) content_width = 1;

    if (panel.wrap_width != content_width) panel.rewrap(content_width);

    int viewport_h = h - start_row;
    if (viewport_h < 0) viewport_h = 0;
    panel.clamp_scroll(viewport_h);

    werase(win);
    if (!header.empty()) mvwaddstr(win, 0, content_col, header.c_str());

    int total = static_cast<int>(panel.wrapped_lines.size());
    for (int i = 0; i < viewport_h; ++i)
    {
        int idx = panel.scroll_top_line + i;
        if (idx < 0 || idx >= total) break;

        const NCURSES_WRAPPED_LINE& line = panel.wrapped_lines[static_cast<size_t>(idx)];
        if (line.attr != 0) wattron(win, line.attr);
        mvwaddstr(win, start_row + i, content_col, line.text.c_str());
        if (line.attr != 0) wattroff(win, line.attr);
    }

    if (paneled) ncurses_commit_panels();
    else wrefresh(win);
}

// Word-wraps the live-typed line, grows/shrinks win_input (and
// correspondingly win_chat, taking the rows from/giving them back to it) to
// exactly the row count currently needed, up to MAX_INPUT_ROWS - only when
// that row count actually changes, not every keystroke - then draws it.
// Beyond the cap, the TAIL of the wrapped text is shown rather than the
// head: LINE only ever grows/shrinks at its end (no mid-line editing - see
// its own comment in user_io.h), so tail-clipping keeps the cursor always
// visible at the cost of hiding the start of a very long line.
void OUTPUT_CLASS::ncurses_update_input_box(const std::string& input_from_user_echo)
{
    if (win_input == nullptr || win_chat == nullptr) return;

    int content_width = ncurses_main_w - 2; // 2 cols reserved for "> "/"  " prefix
    if (content_width < 1) content_width = 1;

    std::vector<std::string> wrapped = hard_wrap(input_from_user_echo, content_width);

    int needed_rows = static_cast<int>(wrapped.size());
    if (needed_rows < 1) needed_rows = 1;
    if (needed_rows > MAX_INPUT_ROWS) needed_rows = MAX_INPUT_ROWS;

    // Keep win_chat at a usable minimum even on a very short terminal,
    // rather than letting the input box claim rows it doesn't have.
    int max_allowed = ncurses_screen_h - SYS_PANEL_ROWS - 1 /* sys/chat separator */
                       - 1 /* chat/input separator */ - 3 /* minimum chat rows */;
    if (max_allowed < 1) max_allowed = 1;
    if (needed_rows > max_allowed) needed_rows = max_allowed;

    if (needed_rows != ncurses_input_rows)
    {
        ncurses_input_rows = needed_rows;

        int chat_row = 0, chat_h = 0, input_row = 0;
        ncurses_compute_rows(ncurses_input_rows, chat_row, chat_h, input_row);

        wresize(win_chat, chat_h, ncurses_main_w);
        move_panel(pan_chat, chat_row, 0); // move_panel(), not mvwin() - win_chat is paneled
        wresize(win_input, ncurses_input_rows, ncurses_main_w);
        mvwin(win_input, input_row, 0);

        // The chat/input separator moved with them - redraw all three
        // separators at their current (possibly new) positions, preserving
        // whichever is currently bolded for focus.
        ncurses_draw_focus_indicators();
    }

    // wrapped always has at least one element (word_wrap()'s own contract -
    // even an empty string yields one empty line), so total >= 1 here.
    int total = static_cast<int>(wrapped.size());
    int start = (total > ncurses_input_rows) ? (total - ncurses_input_rows) : 0;

    werase(win_input);
    int row = 0;
    for (int i = start; i < total; ++i)
    {
        std::string prefix = (i == start) ? "> " : "  ";
        mvwaddstr(win_input, row, 0, (prefix + wrapped[static_cast<size_t>(i)]).c_str());
        ++row;
    }

    // Reverse-video cursor stand-in, at the end of the LAST drawn row -
    // cursor is always right after the last typed char (see LINE's own
    // comment in user_io.h), which is always on the last row once wrapped.
    wattron(win_input, A_REVERSE);
    waddch(win_input, ' ');
    wattroff(win_input, A_REVERSE);

    wrefresh(win_input);
}

void OUTPUT_CLASS::display_with_ncurses(const std::string& input_from_user_echo, const COMMS& comms,
                                         const std::vector<std::string>& tool_names,
                                         SCROLL_KEY scroll_request, bool focus_cycle_requested)
{
    (void)comms; // not read yet - passed through for future use, see io_worker.h's own comment

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
            init_pair(PAIR_TASK_RUNNER_LLM, COLOR_CYAN, -1);
            init_pair(PAIR_TASK_RUNNER_USER, COLOR_YELLOW, -1);
            init_pair(PAIR_DELEGATOR_LLM, COLOR_MAGENTA, -1);
            init_pair(PAIR_DELEGATOR_USER, COLOR_GREEN, -1);
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

    // Focus cycling - Tab. CHAT -> SYSTEM -> TOOLS -> CHAT, skipping TOOLS
    // when the terminal's too narrow to show it (see show_tools_panel in
    // ncurses_layout()).
    if (focus_cycle_requested)
    {
        if (ncurses_focus == NCURSES_FOCUS::CHAT)
        {
            ncurses_focus = NCURSES_FOCUS::SYSTEM;
        }
        else if (ncurses_focus == NCURSES_FOCUS::SYSTEM)
        {
            ncurses_focus = (win_tools != nullptr) ? NCURSES_FOCUS::TOOLS : NCURSES_FOCUS::CHAT;
        }
        else // TOOLS
        {
            ncurses_focus = NCURSES_FOCUS::CHAT;
        }
        ncurses_draw_focus_indicators();
    }

    // Input box first - it may resize win_chat (see ncurses_update_input_box()),
    // so the chat render below this uses win_chat's final geometry for the
    // tick rather than a stale size from before an in-tick input growth.
    ncurses_update_input_box(input_from_user_echo);

    // Page Up/Down - applies to whichever panel currently has focus.
    if (scroll_request != SCROLL_KEY::NONE)
    {
        WINDOW* target_win = win_chat;
        NCURSES_TEXT_PANEL* target_panel = &chat_panel;
        int header_rows = 0;

        if (ncurses_focus == NCURSES_FOCUS::SYSTEM)
        {
            target_win = win_system;
            target_panel = &system_panel;
        }
        else if (ncurses_focus == NCURSES_FOCUS::TOOLS)
        {
            target_win = win_tools;
            target_panel = &tools_panel;
            header_rows = 2; // "Tools:" header + blank row - see the tools block below
        }

        if (target_win != nullptr)
        {
            int h = 0, w = 0;
            getmaxyx(target_win, h, w);
            (void)w;
            int viewport_h = h - header_rows;
            if (viewport_h < 1) viewport_h = 1;

            target_panel->pinned_to_bottom = false;
            target_panel->scroll_by(scroll_request == SCROLL_KEY::PAGE_UP ? -viewport_h : viewport_h, viewport_h);
        }
    }

    if (!system_message.empty())
    {
        system_panel.append(system_message, 0, ncurses_main_w);
        system_message.clear();
    }
    ncurses_render_panel(win_system, system_panel, 0, false);

    // Light grey (dimmed white) so what the user typed reads as visually
    // distinct from the assistant's replies (chat_response below stays
    // plain/undecorated - see PAIR_USER_INPUT_GREY's comment). Sourced from
    // comms.INPUT_FROM_USER_COLOR (comms.h) now, not a local constant here -
    // still defaults to the same COLOR_PAIR(1)|A_DIM this file's own
    // init_pair(1, ...) call above sets up.
    int user_attr = 0;
    if (ncurses_colors_available) user_attr = comms.INPUT_FROM_USER_COLOR;

    if (!user_input.empty())
    {
        chat_panel.append(user_input, user_attr, ncurses_main_w);
        append_to_chat_log(true, user_input);
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
        // Sourced from comms.INPUT_FROM_LLM_COLOR (comms.h) now - defaults
        // to 0, same plain/undecorated (terminal's own default foreground)
        // rendering as before. user_input above is the one that gets
        // dimmed grey now.
        chat_panel.append(chat_response, comms.INPUT_FROM_LLM_COLOR, ncurses_main_w);
        append_to_chat_log(false, chat_response);
        chat_response.clear();
    }

    // Renders once per tick regardless of whether either bucket above had
    // anything new - this is also what makes a scroll-only tick (no new
    // text) redraw correctly for free, and what recovers a resize (see this
    // function's own comment).
    ncurses_render_panel(win_chat, chat_panel, 0, true);

    // Tools panel - rebuilt fresh from tool_names every tick (it's already
    // a small, complete, non-accumulating list, not a growing log - see
    // tools_panel's comment in user_io.h) and rendered like the other two,
    // so it word-wraps and scrolls the same way instead of silently
    // truncating when it doesn't fit.
    if (win_tools != nullptr)
    {
        int dummy_h = 0, win_w = 0;
        getmaxyx(win_tools, dummy_h, win_w);
        (void)dummy_h;
        tools_panel.rebuild_from(tool_names, win_w - 2);
        ncurses_render_panel(win_tools, tools_panel, 2, false, "Tools:", 1);
    }
}

#endif
