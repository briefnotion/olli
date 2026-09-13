// OLLI_DISPLAY - the "draw the screen" plumbing shared by every remote
// tool's terminal UI (clock, presence, hue, rag_tool, ...), the display-side
// counterpart to OLLI_LINK (../olli_link/olli_link.hpp) on the networking
// side. Before this existed, each tool hand-rolled its own RawTerminal +
// raw ANSI escape-code redraw loop - four independent copies of the same
// cursor-hiding/raw-mode/resize bookkeeping, each with its own take on what
// to display and where, and no shared way to answer "is this thing
// connected, and to what profile" at a glance across tools.
//
// Every tool's screen now has the same three-part shape, top to bottom:
//
//   1. A "tool area" - full width, fixed height, owned entirely by the
//      tool. Draw whatever's specific to that tool here (the big clock
//      face, the light list, the people list, ...) with plain ncurses
//      calls against tool_area(), then call refresh_tool_area().
//   2. Three fixed one-line fields, common to every remote tool and never
//      scrolled away: set_connection(), set_profile(), set_tool_status().
//      Each is a no-op unless its text actually changed, so a tool can
//      call all three unconditionally every tick - only the line(s) that
//      changed get redrawn.
//   3. An "activity area" of named lines the tool adds/updates/removes
//      itself as things happen (one line per running timer, tracked
//      light, sync job, ...) - see set_activity_line()/
//      clear_activity_line(). No scrolling: a line updates in place, and
//      only adding/removing a line (not updating one) repaints the whole
//      area, since that's the only case where anything else moves.
//
// One OLLI_DISPLAY per tool process, constructed once in main() in place
// of the old RawTerminal. It owns the terminal (raw ncurses mode instead of
// raw termios, restored on destruction) but nothing about the connection -
// main() still owns its own OLLI_LINK and its own select()-based main loop;
// call tick() once per iteration of that loop, draw/update whatever changed
// this tick, then present() once to flush it all to the terminal in one go.

#pragma once

#include <ncursesw/curses.h>

#include <chrono>
#include <string>
#include <vector>

class OLLI_DISPLAY
{
    public:
        // tool_area_height: rows reserved at the top for the tool's own
        // content - fixed for the life of the program for a tool whose
        // content is always the same shape (clock's big-digit face + date),
        // or a starting guess for one that isn't (see set_tool_area_height()
        // below).
        explicit OLLI_DISPLAY(int tool_area_height);
        ~OLLI_DISPLAY();

        OLLI_DISPLAY(const OLLI_DISPLAY&) = delete;
        OLLI_DISPLAY& operator=(const OLLI_DISPLAY&) = delete;

        // For a tool whose own content genuinely changes shape at runtime
        // (presence's person list growing/shrinking as people are tracked
        // or a profile switch loads a different household) - a no-op
        // unless new_height actually differs from the current one, so a
        // tool can call this unconditionally every tick, same as the fixed-
        // line setters below. Rebuilds every window when it does change
        // (same cost as a resize - see tick()), so the tool's own draw call
        // right after this should always assume a blank tool area rather
        // than trying to preserve anything from the previous frame.
        void set_tool_area_height(int new_height);

        // Call once per main-loop tick, before drawing anything else this
        // tick. Picks up a terminal resize (rebuilds every window and forces
        // a full repaint - the tool area comes back blank until the tool's
        // own next draw, same one-blank-frame cost the old \033[2J-on-resize
        // approach had) and expires any activity line whose ttl_seconds
        // (see set_activity_line()) has run out.
        void tick();

        // Flushes everything drawn/updated so far this tick to the terminal
        // in one go. Call once, after the tool area is drawn and the fixed/
        // activity lines are updated for this tick.
        void present();

        // The tool's own drawing surface - mvwprintw/wclear/whatever
        // directly against this, then call refresh_tool_area() once done
        // for the frame. Never touched by OLLI_DISPLAY itself beyond
        // resize/construction.
        WINDOW* tool_area() { return tool_win; }
        void refresh_tool_area();

        // The three fixed lines below the tool area.
        void set_connection(const std::string& text);
        void set_profile(const std::string& text);
        void set_tool_status(const std::string& text);

        // The activity area below the three fixed lines - one line per key,
        // in first-set order. Calling this again for an existing key
        // replaces its text in place (e.g. a countdown updating every
        // tick) without disturbing any other line.
        //
        // ttl_seconds > 0 arms this line to remove itself that many seconds
        // after *this* call - e.g. call once with a "went off at..." text
        // and ttl_seconds=30 the moment a timer expires, not on every tick
        // while it's still counting down. 0 (default) means no auto-removal
        // - the line stays until changed again or explicitly cleared.
        void set_activity_line(const std::string& key, const std::string& text, int ttl_seconds = 0);
        void clear_activity_line(const std::string& key);

        // Non-blocking key read - ncurses' replacement for the old
        // RawTerminal + read(STDIN_FILENO) pattern (same "'q' to quit"
        // role), routed through ncurses' own input handling instead of a
        // raw fd read so it can't step on the display. Returns ERR (from
        // <ncursesw/curses.h>) if nothing is waiting.
        int get_key();

    private:
        struct ActivityLine {
            std::string key;
            std::string text;
            bool has_ttl = false;
            std::chrono::steady_clock::time_point expires_at{};
        };

        void layout(); // (re)creates tool_win/fixed_win/activity_win for the current terminal size
        void redraw_fixed_lines();   // full repaint of all 3 fixed lines - layout()/resize only
        void redraw_activity_area(); // full repaint of the activity area - membership change/resize only

        int tool_area_height;

        WINDOW* tool_win = nullptr;
        WINDOW* fixed_win = nullptr;    // the 3-line block right below the tool area
        WINDOW* activity_win = nullptr; // everything below that

        std::string connection_text;
        std::string profile_text;
        std::string tool_status_text;

        std::vector<ActivityLine> activity_lines;

        int last_lines = 0;
        int last_cols = 0;
};
