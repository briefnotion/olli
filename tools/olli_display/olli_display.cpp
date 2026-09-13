#include "olli_display.hpp"

#include <clocale>
#include <algorithm>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
    constexpr int FIXED_LINE_COUNT = 3; // connection, profile, tool_status
}

OLLI_DISPLAY::OLLI_DISPLAY(int tool_area_height_)
    : tool_area_height(tool_area_height_)
{
    setlocale(LC_ALL, ""); // required before initscr() for wide/UTF-8 output - see source/user_io.cpp
    initscr();
    cbreak();   // no line buffering - a keypress is available immediately
    noecho();
    curs_set(0); // no blinking terminal cursor - nothing here tracks one
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE); // get_key() never blocks

    getmaxyx(stdscr, last_lines, last_cols);
    layout();
}

OLLI_DISPLAY::~OLLI_DISPLAY()
{
    if (tool_win) delwin(tool_win);
    if (fixed_win) delwin(fixed_win);
    if (activity_win) delwin(activity_win);
    endwin();
}

void OLLI_DISPLAY::layout()
{
    int rows = 0, cols = 0;
    getmaxyx(stdscr, rows, cols);
    if (cols < 1) cols = 1;

    if (tool_win) { delwin(tool_win); tool_win = nullptr; }
    if (fixed_win) { delwin(fixed_win); fixed_win = nullptr; }
    if (activity_win) { delwin(activity_win); activity_win = nullptr; }

    // Guarded against a terminal shorter than the tool area itself - the
    // fixed/activity windows just end up zero-height rather than this
    // crashing on a negative derwin() height.
    int th = std::max(1, std::min(tool_area_height, rows));
    int fixed_top = std::min(th, std::max(0, rows - 1));
    int fh = std::max(0, std::min(FIXED_LINE_COUNT, rows - fixed_top));
    int activity_top = std::min(fixed_top + fh, std::max(0, rows - 1));
    int ah = std::max(1, rows - activity_top);

    tool_win = derwin(stdscr, th, cols, 0, 0);
    fixed_win = fh > 0 ? derwin(stdscr, fh, cols, fixed_top, 0) : derwin(stdscr, 1, cols, fixed_top, 0);
    activity_win = derwin(stdscr, ah, cols, activity_top, 0);

    werase(stdscr);
    wnoutrefresh(stdscr);

    redraw_fixed_lines();
    redraw_activity_area();
}

void OLLI_DISPLAY::set_tool_area_height(int new_height)
{
    if (new_height == tool_area_height) return;
    tool_area_height = new_height;
    layout();
}

void OLLI_DISPLAY::tick()
{
    winsize w{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0 && w.ws_col > 0) {
        if (w.ws_row != last_lines || w.ws_col != last_cols) {
            resizeterm(w.ws_row, w.ws_col);
            last_lines = w.ws_row;
            last_cols = w.ws_col;
            layout();
        }
    }

    auto now = std::chrono::steady_clock::now();
    bool any_expired = false;
    for (auto it = activity_lines.begin(); it != activity_lines.end();) {
        if (it->has_ttl && now >= it->expires_at) {
            it = activity_lines.erase(it);
            any_expired = true;
        } else {
            ++it;
        }
    }
    if (any_expired) redraw_activity_area();
}

void OLLI_DISPLAY::present()
{
    doupdate();
}

void OLLI_DISPLAY::refresh_tool_area()
{
    wnoutrefresh(tool_win);
}

void OLLI_DISPLAY::redraw_fixed_lines()
{
    werase(fixed_win);
    mvwaddstr(fixed_win, 0, 0, connection_text.c_str());
    mvwaddstr(fixed_win, 1, 0, profile_text.c_str());
    mvwaddstr(fixed_win, 2, 0, tool_status_text.c_str());
    wnoutrefresh(fixed_win);
}

void OLLI_DISPLAY::set_connection(const std::string& text)
{
    if (text == connection_text) return;
    connection_text = text;
    mvwaddstr(fixed_win, 0, 0, connection_text.c_str());
    wclrtoeol(fixed_win);
    wnoutrefresh(fixed_win);
}

void OLLI_DISPLAY::set_profile(const std::string& text)
{
    if (text == profile_text) return;
    profile_text = text;
    mvwaddstr(fixed_win, 1, 0, profile_text.c_str());
    wclrtoeol(fixed_win);
    wnoutrefresh(fixed_win);
}

void OLLI_DISPLAY::set_tool_status(const std::string& text)
{
    if (text == tool_status_text) return;
    tool_status_text = text;
    mvwaddstr(fixed_win, 2, 0, tool_status_text.c_str());
    wclrtoeol(fixed_win);
    wnoutrefresh(fixed_win);
}

void OLLI_DISPLAY::redraw_activity_area()
{
    werase(activity_win);
    for (size_t i = 0; i < activity_lines.size(); ++i) {
        mvwaddstr(activity_win, static_cast<int>(i), 0, activity_lines[i].text.c_str());
    }
    wnoutrefresh(activity_win);
}

void OLLI_DISPLAY::set_activity_line(const std::string& key, const std::string& text, int ttl_seconds)
{
    for (size_t i = 0; i < activity_lines.size(); ++i) {
        if (activity_lines[i].key != key) continue;

        activity_lines[i].has_ttl = ttl_seconds > 0;
        if (activity_lines[i].has_ttl) {
            activity_lines[i].expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
        }

        if (activity_lines[i].text == text) return; // nothing actually changed on screen
        activity_lines[i].text = text;

        mvwaddstr(activity_win, static_cast<int>(i), 0, text.c_str());
        wclrtoeol(activity_win);
        wnoutrefresh(activity_win);
        return;
    }

    // A new key - the activity area gains a line, so everything below the
    // tool area needs a real repaint (this is the one case something other
    // than the single changed line moves).
    ActivityLine line;
    line.key = key;
    line.text = text;
    line.has_ttl = ttl_seconds > 0;
    if (line.has_ttl) line.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(ttl_seconds);
    activity_lines.push_back(std::move(line));
    redraw_activity_area();
}

void OLLI_DISPLAY::clear_activity_line(const std::string& key)
{
    auto it = std::find_if(activity_lines.begin(), activity_lines.end(),
                            [&key](const ActivityLine& l) { return l.key == key; });
    if (it == activity_lines.end()) return;
    activity_lines.erase(it);
    redraw_activity_area();
}

int OLLI_DISPLAY::get_key()
{
    return wgetch(stdscr);
}
