#ifndef DOCUMENT_VIEWER_CPP
#define DOCUMENT_VIEWER_CPP

#include "document_viewer.h"

#include <ncursesw/curses.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <sys/ioctl.h>

namespace {

    // How long (in us) to sleep between polls when nothing's waiting on
    // stdin - same reasoning/value as show_web_links_panel()'s own polling
    // loop (user_io.cpp).
    constexpr int INPUT_POLL_INTERVAL_US = 20000;

    // Set by this file's own SIGWINCH handler - deliberately separate from
    // user_io.cpp's own g_ncurses_resized (same reasoning, different
    // variable: read()-based input here never sees KEY_RESIZE, since that
    // only comes through curses' own getch(), which this file avoids - see
    // read_key()'s own comment for why).
    volatile sig_atomic_t g_dv_resized = 0;

    void handle_dv_sigwinch(int)
    {
        g_dv_resized = 1;
    }

    // A key this file's own input loop can act on - synthesized from raw
    // bytes read directly off STDIN_FILENO (see read_key()) rather than
    // ncurses' getch()/keypad() translation.
    enum class DV_KEY
    {
        NONE, RESIZE, UP, DOWN, PAGE_UP, PAGE_DOWN, HOME, END,
        ENTER, BACKSPACE, CTRL_C, DIGIT, LETTER_Q, OTHER
    };

    struct DV_KEY_EVENT
    {
        DV_KEY key = DV_KEY::NONE;
        char ch = 0; // valid for DIGIT
    };

    // Reads and interprets at most one key, without blocking - mirrors
    // KEYBOARD_INPUT::keyboard_input()'s own raw, non-blocking, VMIN=0/
    // VTIME=0 read(STDIN_FILENO, ...) style (user_io.cpp) rather than
    // curses' getch(), which this file deliberately never calls: getch()
    // needs cbreak()/noecho()/keypad()/timeout() to behave the way this
    // viewer wants, and calling those - even though the caller's
    // reset_prog_mode() nominally restores the terminal modes active
    // before this ran - left KEYBOARD_INPUT's own raw reader broken after
    // returning (confirmed live: typed characters stopped echoing in
    // olli's input box once this viewer had been opened and closed, even
    // though the viewer's own navigation worked perfectly while it was
    // open). Reading stdin the exact same way KEYBOARD_INPUT already does,
    // and never touching curses' input-side terminal modes at all, avoids
    // that entirely - only draw()'s curses calls (erase/mvaddstr/refresh)
    // are still used, and those never showed any problem.
    DV_KEY_EVENT read_key()
    {
        if (g_dv_resized)
        {
            g_dv_resized = 0;
            return { DV_KEY::RESIZE, 0 };
        }

        char ch = 0;
        if (read(STDIN_FILENO, &ch, 1) <= 0) return { DV_KEY::NONE, 0 };

        if (ch == 3) return { DV_KEY::CTRL_C, 0 };
        if (ch == 127 || ch == 8) return { DV_KEY::BACKSPACE, 0 };
        if (ch == '\n' || ch == '\r') return { DV_KEY::ENTER, 0 };
        if (ch >= '0' && ch <= '9') return { DV_KEY::DIGIT, ch };
        if (ch == 'q' || ch == 'Q') return { DV_KEY::LETTER_Q, 0 };

        if (ch == 27) // ESC - CSI escape sequence (arrow/Home/End/PageUp/
                       // PageDown) - see KEYBOARD_INPUT's own equivalent
                       // parsing (user_io.cpp) for the same trust-the-
                       // bytes-are-already-there style (a terminal sends
                       // the whole sequence in one burst).
        {
            char seq0 = 0, code = 0;
            if (read(STDIN_FILENO, &seq0, 1) <= 0 || seq0 != '[') return { DV_KEY::OTHER, 0 };
            if (read(STDIN_FILENO, &code, 1) <= 0) return { DV_KEY::OTHER, 0 };

            switch (code)
            {
                case 'A': return { DV_KEY::UP, 0 };
                case 'B': return { DV_KEY::DOWN, 0 };
                case 'H': return { DV_KEY::HOME, 0 };
                case 'F': return { DV_KEY::END, 0 };
                case '1': case '4': case '5': case '6':
                {
                    char tilde = 0;
                    read(STDIN_FILENO, &tilde, 1); // consume trailing '~'
                    if (code == '1') return { DV_KEY::HOME, 0 };
                    if (code == '4') return { DV_KEY::END, 0 };
                    return { (code == '5') ? DV_KEY::PAGE_UP : DV_KEY::PAGE_DOWN, 0 };
                }
                default: return { DV_KEY::OTHER, 0 };
            }
        }

        return { DV_KEY::OTHER, 0 };
    }

    // One screen row's worth of content. A source line longer than the
    // content width becomes several consecutive VISUAL_ROWs (wrapping),
    // each pointing back at the source line it came from. Mirrors
    // file_watch's ui.h VISUAL_ROW.
    struct VISUAL_ROW
    {
        int source_line = 0;
        bool is_first_segment = true;
        std::string text;
    };

    // Splits Text into rows of at most Width bytes, breaking on word
    // boundaries where possible (a single token longer than Width is hard-
    // broken). Byte-based rather than display-width-aware, same convention
    // as olli's own word_wrap() (user_io.cpp) - simpler than file_watch's
    // wcwidth-based wrapping and consistent with how the rest of olli
    // already wraps text.
    std::vector<std::string> word_wrap(const std::string& text, int width)
    {
        if (width <= 0) return { text };
        if (text.empty()) return { std::string() };

        std::vector<std::string> lines;
        std::string current_line;
        size_t i = 0;
        size_t n = text.size();

        while (i < n)
        {
            while (i < n && text[i] == ' ') ++i;
            if (i >= n) break;

            size_t tok_start = i;
            while (i < n && text[i] != ' ') ++i;
            std::string token = text.substr(tok_start, i - tok_start);

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

    std::vector<std::string> split_into_lines(const std::string& content)
    {
        std::vector<std::string> lines;
        size_t start = 0;

        for (size_t i = 0; i <= content.size(); ++i)
        {
            if (i == content.size() || content[i] == '\n')
            {
                lines.push_back(content.substr(start, i - start));
                start = i + 1;
            }
        }

        if (lines.empty()) lines.push_back(std::string());

        return lines;
    }

    std::vector<VISUAL_ROW> wrap_lines(const std::vector<std::string>& lines, int width)
    {
        std::vector<VISUAL_ROW> rows;

        for (int i = 0; i < static_cast<int>(lines.size()); ++i)
        {
            std::vector<std::string> segments = word_wrap(lines[static_cast<size_t>(i)], width);

            bool first = true;
            for (const std::string& segment : segments)
            {
                rows.push_back(VISUAL_ROW{ i, first, segment });
                first = false;
            }
        }

        return rows;
    }

    int digit_count(size_t value)
    {
        int count = 1;
        while (value >= 10)
        {
            value /= 10;
            ++count;
        }
        return count;
    }

    int gutter_width(size_t total_source_lines)
    {
        return std::max(3, digit_count(total_source_lines));
    }

    // Gutter, then a space and a separator before the text starts - feed
    // this to wrap_lines() so wrapping matches what draw() below actually
    // draws.
    int content_text_width(int cols, size_t total_source_lines)
    {
        return cols - (gutter_width(total_source_lines) + 3);
    }

    int content_height(int rows)
    {
        // One row for the status bar, one for the command line.
        return std::max(0, rows - 2);
    }

    int clamp_top_row(int top_row, int total_visual_rows, int height)
    {
        int max_top = std::max(0, total_visual_rows - height);
        if (top_row < 0) top_row = 0;
        if (top_row > max_top) top_row = max_top;
        return top_row;
    }

    // Finds the visual row where a (0-indexed) source line first appears
    // after wrapping. Clamps out-of-range source lines to the nearest end.
    int visual_row_for_source_line(const std::vector<VISUAL_ROW>& visual_rows, int source_line)
    {
        if (visual_rows.empty()) return 0;

        if (source_line <= visual_rows.front().source_line) return 0;

        for (size_t i = 0; i < visual_rows.size(); ++i)
        {
            if (visual_rows[i].source_line == source_line && visual_rows[i].is_first_segment)
            {
                return static_cast<int>(i);
            }
        }

        for (size_t i = visual_rows.size(); i-- > 0;)
        {
            if (visual_rows[i].is_first_segment) return static_cast<int>(i);
        }

        return 0;
    }

    void draw(WINDOW* screen, const std::vector<VISUAL_ROW>& visual_rows, size_t total_source_lines,
              const std::string& label, int top_row, int rows, int cols,
              const std::string& command_buffer)
    {
        werase(screen);

        // ---- Status bar ----
        std::string status = " " + label + "    ";

        if (top_row >= 0 && top_row < static_cast<int>(visual_rows.size()))
        {
            status += "line " + std::to_string(visual_rows[static_cast<size_t>(top_row)].source_line + 1) +
                       "/" + std::to_string(total_source_lines);
        }
        else
        {
            status += "line 0/" + std::to_string(total_source_lines);
        }

        if (static_cast<int>(status.size()) > cols) status.resize(static_cast<size_t>(std::max(0, cols)));

        wattron(screen, A_REVERSE);
        mvwhline(screen, 0, 0, ' ', cols);
        mvwaddstr(screen, 0, 0, status.c_str());
        wattroff(screen, A_REVERSE);

        // ---- Content area, with a line-number gutter ----
        int content_h = content_height(rows);
        int gutter_w = gutter_width(total_source_lines);

        for (int i = 0; i < content_h; ++i)
        {
            int row_index = top_row + i;
            int screen_row = 1 + i;

            if (row_index >= 0 && row_index < static_cast<int>(visual_rows.size()))
            {
                const VISUAL_ROW& row = visual_rows[static_cast<size_t>(row_index)];

                if (row.is_first_segment)
                {
                    std::string num_str = std::to_string(row.source_line + 1);
                    if (static_cast<int>(num_str.size()) < gutter_w)
                    {
                        num_str = std::string(static_cast<size_t>(gutter_w) - num_str.size(), ' ') + num_str;
                    }

                    wattron(screen, A_DIM);
                    mvwaddnstr(screen, screen_row, 0, num_str.c_str(), gutter_w);
                    wattroff(screen, A_DIM);
                }

                mvwaddch(screen, screen_row, gutter_w + 1, '|');

                int text_x = gutter_w + 3;
                if (cols - text_x > 0)
                {
                    mvwaddnstr(screen, screen_row, text_x, row.text.c_str(), cols - text_x);
                }
            }
            else
            {
                mvwaddch(screen, screen_row, 0, '~');
            }
        }

        // ---- Command line ----
        int cmd_row = rows - 1;
        std::string cmd_display = "> " + command_buffer + "    (Enter, or q + Enter, to return)";
        mvwaddnstr(screen, cmd_row, 0, cmd_display.c_str(), cols);
        wmove(screen, cmd_row, std::min(cols - 1, static_cast<int>(("> " + command_buffer).size())));

        wrefresh(screen);
    }

} // namespace

void DOCUMENT_VIEWER::show(WINDOW* screen, const std::string& label, const std::string& content)
{
    // Deliberately no initscr()/endwin(), and deliberately no cbreak()/
    // noecho()/keypad()/curs_set()/timeout() either - see read_key()'s own
    // comment for why the latter turned out to matter just as much as the
    // former. olli already has exactly one curses screen and one raw
    // terminal mode alive for the whole process; both are already exactly
    // what this viewer needs (KEYBOARD_INPUT's own raw mode, set up once,
    // outside curses entirely - see its constructor, user_io.h), so this
    // function only ever draws (via curses) and reads (via plain read()),
    // never reconfigures either.
    //
    // Own SIGWINCH handler, saved/restored around the loop below, since
    // read_key() never sees KEY_RESIZE (that only comes through curses'
    // own getch(), which this file avoids) - same reasoning as user_io.cpp's
    // g_ncurses_resized, just not sharing its variable (see g_dv_resized).
    void (*prev_sigwinch)(int) = std::signal(SIGWINCH, handle_dv_sigwinch);

    int rows = 0, cols = 0;
    getmaxyx(screen, rows, cols);

    std::vector<std::string> lines = split_into_lines(content);
    std::vector<VISUAL_ROW> visual_rows = wrap_lines(lines, content_text_width(cols, lines.size()));

    int top_row = 0;
    std::string command_buffer;
    bool running = true;

    while (running)
    {
        draw(screen, visual_rows, lines.size(), label, top_row, rows, cols, command_buffer);

        DV_KEY_EVENT event = read_key();

        switch (event.key)
        {
            case DV_KEY::NONE:
                usleep(INPUT_POLL_INTERVAL_US);
                break;

            case DV_KEY::RESIZE:
            {
                struct winsize ws{};
                if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
                {
                    resizeterm(ws.ws_row, ws.ws_col);
                }
                getmaxyx(screen, rows, cols);
                visual_rows = wrap_lines(lines, content_text_width(cols, lines.size()));
                top_row = clamp_top_row(top_row, static_cast<int>(visual_rows.size()), content_height(rows));
                break;
            }

            case DV_KEY::UP:
                top_row = clamp_top_row(top_row - 1, static_cast<int>(visual_rows.size()), content_height(rows));
                break;

            case DV_KEY::DOWN:
                top_row = clamp_top_row(top_row + 1, static_cast<int>(visual_rows.size()), content_height(rows));
                break;

            case DV_KEY::PAGE_UP:
                top_row = clamp_top_row(top_row - content_height(rows), static_cast<int>(visual_rows.size()), content_height(rows));
                break;

            case DV_KEY::PAGE_DOWN:
                top_row = clamp_top_row(top_row + content_height(rows), static_cast<int>(visual_rows.size()), content_height(rows));
                break;

            case DV_KEY::HOME:
                top_row = 0;
                break;

            case DV_KEY::END:
                top_row = clamp_top_row(static_cast<int>(visual_rows.size()), static_cast<int>(visual_rows.size()), content_height(rows));
                break;

            case DV_KEY::BACKSPACE:
                if (!command_buffer.empty()) command_buffer.pop_back();
                break;

            // Ctrl+C: olli disables ISIG at the tty-driver level for its own
            // raw keyboard reader (see KEYBOARD_INPUT, user_io.h), so it
            // arrives here as a plain byte rather than a real SIGINT - treat
            // it the same as 'q'.
            case DV_KEY::CTRL_C:
                running = false;
                break;

            case DV_KEY::ENTER:
                if (command_buffer.empty() || command_buffer == "q")
                {
                    running = false;
                }
                else
                {
                    int goto_line = std::atoi(command_buffer.c_str());
                    top_row = clamp_top_row(visual_row_for_source_line(visual_rows, goto_line - 1),
                                             static_cast<int>(visual_rows.size()), content_height(rows));
                }
                command_buffer.clear();
                break;

            case DV_KEY::DIGIT:
                if (static_cast<int>(command_buffer.size()) < cols - 4) command_buffer += event.ch;
                break;

            case DV_KEY::LETTER_Q:
                if (command_buffer.empty()) command_buffer += 'q';
                break;

            case DV_KEY::OTHER:
                break;
        }
    }

    std::signal(SIGWINCH, prev_sigwinch);

    // No endwin() here - see this function's opening comment. The caller
    // (show_web_links_panel()) owns suspending/resuming the one real
    // curses screen and does its own reset_prog_mode()+clearok() after
    // this returns, which is what actually needs to happen to repaint
    // over whatever's left in stdscr from the loop above.
}

#endif // DOCUMENT_VIEWER_CPP
