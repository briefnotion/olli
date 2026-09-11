#ifndef COMMS_H
#define COMMS_H

#include <string>
#include <mutex>
#include <atomic>
#include <vector>
#include <utility>

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

// One piece of exact data a tool result carries alongside whatever the
// model narrates about it - the generalized form of what used to be just
// (title, url) web-search links (see COMMS::TOOL_ATTACHMENTS below).
// "type" is a plain string, not an enum, so a new kind (e.g. a future
// remote tool's screen-grab) never needs a header-wide change to add -
// only wherever actually reads type needs to learn a new value, same
// spirit as a remote tool's name never needing olli-side code to exist
// (see TOOL_REMOTE, remote_tools.h). "link" (label=title, content=url)
// and "document" (label=title, content=full text) are the two kinds that
// exist right now.
struct TOOL_ATTACHMENT {
    std::string type;
    std::string label;
    std::string content;

    TOOL_ATTACHMENT() = default;
    // Explicit (not aggregate) so vector::emplace_back(type, label,
    // content) keeps working under C++17 - parenthesized aggregate init
    // is a C++20 feature, and this codebase targets C++17 (Makefile/
    // CMakeLists.txt).
    TOOL_ATTACHMENT(std::string type_, std::string label_, std::string content_)
        : type(std::move(type_)), label(std::move(label_)), content(std::move(content_)) {}
};

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

        // --------------------------------------------------------------
        // Simple on/off toggles. Deliberately NOT checked by
        // IO_WORKER_CLASS::exchange() (io_worker.cpp) - that function is a
        // plain pass-through in both directions, no policy decisions. The
        // actual gating lives wherever each thing is produced/captured
        // instead: ncurses_update_input_box() (user_io.cpp) dims the input
        // box when ENABLE_KEYBOARD_INPUT is false, KEYBOARD_INPUT::
        // keyboard_input()'s own PROPS.CHAT_INPUT_ENABLED (synced from this
        // each tick in IO_WORKER_CLASS::thread_main()) skips its
        // content-building keys, and thread_main()'s TTS-speaking step
        // checks ENABLE_TTS_OUTPUT directly before calling speakAsync().
        // Default true on both, matching the always-on behavior before
        // either was wired in. Settable from a running .task script via
        // [KEYBOARD_INPUT:on/off] and [TTS_OUTPUT:on/off]
        // (tools_task_script.cpp) - [ASK]/[PAUSE] save and restore
        // ENABLE_KEYBOARD_INPUT automatically around themselves, since they
        // inherently need a human to press a key regardless of this flag.
        // --------------------------------------------------------------
        bool ENABLE_KEYBOARD_INPUT = true;
        bool ENABLE_TTS_OUTPUT = true;
        // --------------------------------------------------------------

        // --------------------------------------------------------------
        // Exact-data attachments surfaced by a tool call - e.g.
        // TOOL_WEB_SEARCH's (tools.cpp) links, pushed here directly from
        // the raw search/fetch result rather than parsed back out of the
        // model's own rewritten response text (which may paraphrase or
        // drop them entirely). Accumulate-then-drain, same shape as
        // INPUT_FROM_LLM above: appended under output_buffer_mutex by
        // whatever thread's running the tool call, relayed on by
        // IO_WORKER_CLASS::exchange() (io_worker.cpp), which clears this
        // copy once copied.
        //
        // Was WEB_LINKS (a plain vector<pair<string,string>>, links only)
        // until 2026-09-10 - generalized to TOOL_ATTACHMENT's tagged shape
        // so a future remote tool's own exact-data result (e.g. a
        // document's full text) can travel the same path instead of
        // needing a second, parallel mechanism. Display side
        // (OUTPUT_CLASS, user_io.cpp/.h) still only actually renders the
        // "link" type as of this change - see its own comments for the
        // "type"-branching work that's still pending before a "document"
        // entry displays as anything other than a mis-rendered link.
        //
        // Links deliberately NOT rendered inline as a real clickable link
        // in the chat panel - confirmed by a standalone test that ncurses'
        // waddstr()/addstr() sanitizes the OSC 8 escape bytes into visible
        // caret-notation garbage instead of passing them to the terminal,
        // worse the longer the URL. Instead: OUTPUT_CLASS shows a short
        // "[Links: ...]" notice near the response (display_with_ncurses(),
        // user_io.cpp) and Ctrl+L (KEYBOARD_INPUT::SHOW_LINKS_REQUESTED)
        // opens a full list via OUTPUT_CLASS::show_web_links_panel(),
        // which drops out of curses mode (endwin()) and writes the OSC 8
        // sequence with a raw std::cout instead - confirmed working
        // (real clickable link, terminal opened it) once ncurses is out of
        // the way.
        std::vector<TOOL_ATTACHMENT> TOOL_ATTACHMENTS;
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

        // std::atomic has no copy-assignment operator, which would
        // otherwise implicitly delete COMMS's own operator= entirely -
        // provided explicitly instead, copying every field above except
        // close_chat_log_requested itself (deliberately left alone; it's
        // a standalone signal, not part of the rest of a COMMS snapshot -
        // see its own comment just above). Needed because
        // IO_WORKER_CLASS::thread_main() (io_worker.cpp) round-trips whole
        // COMMS copies between comms_buffer and its per-channel copies
        // (comms_keyboard/comms_stt_tts, io_worker.h), treating COMMS as a
        // plain holding place you assign wholesale rather than something
        // requiring field-by-field merge logic at each call site.
        COMMS& operator=(const COMMS& other)
        {
            if (this == &other) return *this;
            INPUT_FROM_LLM = other.INPUT_FROM_LLM;
            INPUT_FROM_THINKING = other.INPUT_FROM_THINKING;
            INPUT_FROM_SYSTEM = other.INPUT_FROM_SYSTEM;
            INPUT_FROM_LLM_COLOR = other.INPUT_FROM_LLM_COLOR;
            INPUT_FROM_USER_COLOR = other.INPUT_FROM_USER_COLOR;
            ENTER_PRESSED = other.ENTER_PRESSED;
            INPUT_FROM_USER = other.INPUT_FROM_USER;
            INTERRUPTED = other.INTERRUPTED;
            IS_TYPING = other.IS_TYPING;
            EXIT_REQUESTED = other.EXIT_REQUESTED;
            ENABLE_KEYBOARD_INPUT = other.ENABLE_KEYBOARD_INPUT;
            ENABLE_TTS_OUTPUT = other.ENABLE_TTS_OUTPUT;
            TOOL_ATTACHMENTS = other.TOOL_ATTACHMENTS;
            return *this;
        }
};

#endif
