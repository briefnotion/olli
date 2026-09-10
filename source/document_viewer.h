#ifndef DOCUMENT_VIEWER_H
#define DOCUMENT_VIEWER_H

#include <string>

// Opaque ncurses window handle, forward-declared so this header never has
// to pull in <curses.h> - same reasoning/mechanism as user_io.h's own
// identical forward declaration (see its comment there): curses.h #defines
// a long list of common identifiers as macros, which would be a landmine
// for anything else that includes this header. The struct tag matches
// curses.h's own "typedef struct _win_st WINDOW;" exactly, so the two
// declarations refer to the same type across translation units.
struct _win_st;
typedef struct _win_st WINDOW;

// Full-screen, scrollable, line-numbered, word-wrapped viewer for a single
// piece of already-in-memory text (e.g. a COMMS::TOOL_ATTACHMENT's content -
// see OUTPUT_CLASS::show_web_links_panel(), user_io.cpp). Adapted from
// delmanel's standalone file_watch program (../../file_watcher's ui.h/.cpp),
// stripped of its live file-reload machinery since there's no file to poll
// here, just a fixed string already in memory.
//
// Self-contained: draws onto whatever WINDOW* the caller passes in (in
// practice always stdscr, but taken as a parameter rather than reached for
// implicitly - see this project's own no-hidden-globals convention),  and
// deliberately does NOT call initscr()/endwin() itself - olli only ever has
// one real curses screen for its whole process lifetime, and a second
// initscr() while the first is still alive corrupts it. The caller must
// already have suspended curses (def_prog_mode()+endwin(), the same idiom
// OUTPUT_CLASS::show_web_links_panel() already uses) before calling show(),
// and must resume it (reset_prog_mode()+clearok(curscr, TRUE)) after
// show() returns.
class DOCUMENT_VIEWER
{
    public:
    // Blocks until the user quits (a blank Enter, q, or Ctrl+C - see this
    // file's .cpp for why Ctrl+C is handled as a plain keystroke here
    // rather than a signal).
    // Screen is the window to draw into (the caller's stdscr - see this
    // class's own comment). Label is shown in the status bar; Content is
    // split on '\n' and displayed with line numbers and word-wrapping.
    // Navigation: Up/Down (line), PageUp/PageDown (screen), Home/End (top/
    // bottom), a typed number then Enter (go to that line), a blank Enter
    // or q then Enter (quit).
    static void show(WINDOW* screen, const std::string& label, const std::string& content);
};

#endif
