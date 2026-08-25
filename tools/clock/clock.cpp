// The real networked clock (see ../PROTOCOL.md) - connects to olli's
// remote-tool listener, registers get_clock_time/set_timer/check_timer, and
// answers each "call" (the current time, or a named countdown - see the
// Timers section below). The big ASCII-art digital display (classic
// "tty-clock" style) runs independently of olli's lifecycle: this program
// can start before olli does, keeps ticking while disconnected, notices
// when olli becomes reachable and registers, and if olli goes away
// (cleanly or not) just falls back to "disconnected, retrying" and keeps
// ticking rather than exiting - see the heartbeat/reconnect note in
// ../PROTOCOL.md. Timers themselves keep running through a disconnect too;
// only the eventual expiry alert waits for a live connection to send on.
//
// Controls: 'q' or Ctrl+C to quit (restores the terminal cleanly either
// way - closing the connection this way is exactly what exercises olli's
// own disconnect handling, TOOL_REMOTE::monitor_tool() noticing the closed
// socket and removing this tool from tools_list).
//
// Build: `make` in this directory (see Makefile). Run: `./clock [host]` any
// time - it does not need olli to already be running. host defaults to
// 127.0.0.1 (olli on this same machine); pass olli's real IP to reach it
// somewhere else on the network. `./clock -h` / `--help` for usage.

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <csignal>
#include <ctime>
#include <vector>
#include <map>
#include <deque>
#include <algorithm>
#include <cerrno>

#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using json = nlohmann::json;

namespace {
    constexpr int REMOTE_TOOL_PORT = 47601;

    // Heartbeat/reconnect timing - see ../PROTOCOL.md. Kept in step with
    // TOOL_REMOTE's own PING_INTERVAL_SECONDS/DEAD_TIMEOUT_SECONDS
    // (source/remote_tools.h) even though nothing enforces the two staying
    // equal - either side can independently notice a timeout regardless of
    // what the other's numbers are.
    constexpr int RECONNECT_INTERVAL_SECONDS = 3;
    constexpr int PING_INTERVAL_SECONDS = 5;
    constexpr int DEAD_TIMEOUT_SECONDS = 15;

    // Caps how long one connection attempt can take - see try_connect()
    // below for why this matters once host isn't loopback.
    constexpr int CONNECT_TIMEOUT_SECONDS = 2;

    void send_line(int fd, const std::string& line)
    {
        std::string with_newline = line + "\n";
        ssize_t written = write(fd, with_newline.data(), with_newline.size());
        (void)written;
    }

    std::string current_time(const std::string& format)
    {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm{};
        localtime_r(&now_time, &local_tm);
        std::stringstream ss;
        ss << std::put_time(&local_tm, format.c_str());
        return ss.str();
    }

    // --- Timers ---
    //
    // Ported from olli's own TOOL_TIMER/TIMER_SIMPLE (source/tools.cpp,
    // source/tools_helper.{h,cpp} - now removed there), so timers keep
    // running independently of olli's own process/restart lifecycle, same
    // reasoning as get_clock_time itself. In-memory only, same as before -
    // restarting this program (or reconnecting) loses every timer, matching
    // the old behavior of restarting olli.
    struct ActiveTimer {
        std::chrono::steady_clock::time_point deadline;
        std::string reminder;
        bool event_sent = false; // whether the expiry `event` line has gone out to olli yet
        bool blinked = false;    // whether the local screen-flash (see below) has fired yet

        // Optional real follow-up action, separate from `reminder`'s plain
        // narration text - see ../PROTOCOL.md's `event` shape and
        // set_timer's registered description below for why this exists
        // (reminder alone was never reliable for "actually do X", only for
        // "say X happened"). Empty on_expire_tool means no action - just
        // the usual narration.
        std::string on_expire_tool;
        json on_expire_arguments = json::object();
    };

    // Keyed by label; set_timer overwrites an existing label outright - a
    // fresh countdown replaces whatever was there. Deliberately never
    // pruned once finished: a real bug in the old olli-side TOOL_TIMER
    // erased a timer the instant its expiry event fired, so a check_timer
    // call landing right after - e.g. the model following up on its own
    // "[TIMER EXPIRED]" alert - got "no timer found" instead of
    // confirmation (seen firsthand in a real history.json). Leaving
    // finished timers in place, queryable indefinitely, fixes that; nothing
    // here accumulates fast enough for the lack of pruning to matter.
    std::map<std::string, ActiveTimer> active_timers;

    // Who's currently running olli, per its own "identity" message (see
    // ../PROTOCOL.md and handle_identity() below) - empty means either not
    // told yet, or olli's shared/no-profile default. Reset back to empty by
    // reset_to_default_profile() on disconnect, so a stale identity from a
    // previous session/user never lingers once olli goes away.
    std::string current_user_name;
    std::string current_user_full_name;
    std::string current_user_about;

    // Purely cosmetic: flashes the whole screen in reverse video briefly
    // when a timer fires, in addition to (never instead of) the real
    // `event` notification pushed to olli below. redraw_screen() ticks this
    // down once per main-loop iteration (~200ms, see main()'s select()
    // timeout), alternating on/off each tick, rather than a blocking
    // sleep-based flash - keeps the loop non-blocking like everything else
    // here.
    constexpr int BLINK_TOTAL_TICKS = 6; // ~1.2s at the ~200ms tick rate - 3 on/off flashes
    int blink_ticks_left = 0;

    // Handles every timer that just crossed its deadline: fires the local
    // screen-flash immediately regardless of connection state (a timer
    // finishing is a local fact, not something that needs olli reachable to
    // be true), and separately pushes one `event` line (see ../PROTOCOL.md)
    // once connected - deferred, not skipped, if disconnected right now, so
    // it fires on the first tick after reconnecting instead of being lost.
    // Updates `status` too, same as handle_call()'s return value, so the
    // display reflects it immediately.
    void handle_expired_timers(int fd, std::string& status, std::chrono::steady_clock::time_point& last_sent)
    {
        auto now = std::chrono::steady_clock::now();
        for (auto& [label, timer] : active_timers) {
            if (now < timer.deadline) continue;

            if (!timer.blinked) {
                timer.blinked = true;
                blink_ticks_left = BLINK_TOTAL_TICKS;
            }

            if (timer.event_sent || fd < 0) continue;

            std::stringstream ss;
            ss << "### [TIMER EXPIRED] ###\n";
            ss << "The wait time for '" << label << "' is complete.\n";
            if (!timer.reminder.empty()) {
                ss << "Target action: " << timer.reminder << ".\n";
            }
            ss << "Inform the user in character.";

            json event_msg = {{"type", "event"}, {"message", ss.str()}};
            if (!timer.on_expire_tool.empty()) {
                event_msg["action"] = {
                    {"tool", timer.on_expire_tool},
                    {"arguments", timer.on_expire_arguments}
                };
            }

            send_line(fd, event_msg.dump());
            // Same last_sent gap as handle_call()'s own comment in main() -
            // this sends real data over the wire but, unlike that call
            // site, had no way to update main()'s last_sent at all before
            // last_sent was added to this function's own parameters.
            last_sent = std::chrono::steady_clock::now();
            timer.event_sent = true;
            status = "Timer '" + label + "' expired.";
        }
    }

    // --- Big ASCII-art digit display ---

    // 5 rows x 3 cols each - a simple block/7-segment-style digit font.
    // '#' marks a lit cell, ' ' unlit. Rendered at 2 terminal columns per
    // lit cell (see render_big_clock below) for a chunky "tty-clock" look.
    const char* const DIGIT_FONT[10][5] = {
        {"###","# #","# #","# #","###"}, // 0
        {" # "," # "," # "," # "," # "}, // 1
        {"###","  #","###","#  ","###"}, // 2
        {"###","  #","###","  #","###"}, // 3
        {"# #","# #","###","  #","  #"}, // 4
        {"###","#  ","###","  #","###"}, // 5
        {"###","#  ","###","# #","###"}, // 6
        {"###","  #","  #","  #","  #"}, // 7
        {"###","# #","###","# #","###"}, // 8
        {"###","# #","###","  #","###"}, // 9
    };
    // 1 col x 5 rows - narrower than a digit, two centered dots.
    const char* const COLON_FONT[5] = {" ", "#", " ", "#", " "};

    // U+2588 FULL BLOCK, doubled - one lit cell, 2 terminal columns wide.
    const std::string BLOCK = "\xE2\x96\x88\xE2\x96\x88";

    struct BigClock {
        std::vector<std::string> rows = std::vector<std::string>(5);
        int width = 0; // terminal columns - tracked directly while building,
                        // rather than inferred from byte length afterward
                        // (BLOCK is multi-byte UTF-8, plain spaces aren't,
                        // so byte count isn't proportional to column count).
    };

    BigClock render_big_clock(const std::string& time_str)
    {
        BigClock result;

        for (char c : time_str) {
            const char* const* font = nullptr;
            int cols = 0;
            if (c >= '0' && c <= '9') {
                font = DIGIT_FONT[static_cast<size_t>(c - '0')];
                cols = 3;
            } else if (c == ':') {
                font = COLON_FONT;
                cols = 1;
            } else {
                continue;
            }

            for (size_t r = 0; r < 5; ++r) {
                for (int col = 0; col < cols; ++col) {
                    result.rows[r] += (font[r][col] == '#') ? BLOCK : "  ";
                }
                result.rows[r] += "  "; // gap before the next character
            }
            result.width += cols * 2 + 2;
        }

        return result;
    }

    // --- Terminal handling ---

    // RAII: puts stdin into raw, non-canonical, non-echoing mode so a
    // keypress ('q' to quit) can be read immediately without waiting for
    // Enter, and hides the cursor while the display is live - restores both
    // exactly as found on destruction. Same reasoning as olli's own
    // KEYBOARD_INPUT (source/user_io.cpp), just self-contained here since
    // this program builds independently of olli's source tree.
    class RawTerminal {
        public:
            RawTerminal()
            {
                if (tcgetattr(STDIN_FILENO, &old_termios) == 0) {
                    termios raw = old_termios;
                    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
                    raw.c_cc[VMIN] = 0;
                    raw.c_cc[VTIME] = 0;
                    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
                    active = true;
                }
                std::cout << "\033[?25l" << std::flush; // hide cursor
            }

            ~RawTerminal()
            {
                std::cout << "\033[?25h" << std::flush; // show cursor again
                if (active) tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
                std::cout << '\n'; // leave the cursor on its own fresh line, not behind the last frame
            }

            RawTerminal(const RawTerminal&) = delete;
            RawTerminal& operator=(const RawTerminal&) = delete;

        private:
            termios old_termios{};
            bool active = false;
    };

    int terminal_width()
    {
        winsize w{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
            return w.ws_col;
        }
        return 80; // reasonable fallback if the ioctl fails
    }

    int terminal_height()
    {
        winsize w{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_row > 0) {
            return w.ws_row;
        }
        return 24; // reasonable fallback if the ioctl fails
    }

    // Size seen on the previous redraw_screen() call - lets a resize
    // (either dimension) be detected below and answered with a full
    // \033[2J clear instead of the usual per-line \033[K overwrite. Most
    // terminals reflow or scroll their buffer on resize, which can leave
    // stale fragments of the old, differently-sized frame that a per-line
    // clear alone never reaches - see redraw_screen()'s use of this.
    int last_terminal_cols = -1;
    int last_terminal_rows = -1;

    std::string centered(const std::string& text, int width)
    {
        int pad = std::max(0, (width - static_cast<int>(text.size())) / 2);
        return std::string(static_cast<size_t>(pad), ' ') + text;
    }

    // How many recent status lines stay on screen at once - oldest scrolls
    // off the top as new ones arrive, like a small log tail.
    constexpr size_t STATUS_LOG_LINES = 6;

    std::deque<std::string> status_log;
    std::string last_logged_status;

    // redraw_screen() runs ~5x/sec regardless of whether anything actually
    // happened, so `status` is the same string on most ticks - this is what
    // keeps the log from filling up with duplicate "Registered with
    // olli..." lines instead of an actual history of events. Timestamped so
    // a quiet stretch between events is still visible in the log.
    void log_status(const std::string& status)
    {
        if (status.empty() || status == last_logged_status) return;
        last_logged_status = status;

        status_log.push_back("[" + current_time("%H:%M:%S") + "] " + status);
        while (status_log.size() > STATUS_LOG_LINES) status_log.pop_front();
    }

    // \033[K after each line clears any leftover from a previous, wider
    // frame - needed since this repositions to \033[H and overwrites rather
    // than clearing the whole screen every redraw (avoids a visible flicker
    // once a second). Runs regardless of connection state - the clock keeps
    // ticking whether or not olli is reachable right now.
    void redraw_screen(const std::string& status)
    {
        log_status(status);

        // Timer-expiry flash (see blink_ticks_left's comment) - alternates
        // on/off each tick rather than staying solid for its whole
        // duration, which is what actually reads as a "blink" instead of
        // one plain color swap. Ticked down here, the one place every
        // frame is guaranteed to pass through.
        bool inverted = false;
        if (blink_ticks_left > 0) {
            inverted = (blink_ticks_left % 2 == 0);
            --blink_ticks_left;
        }

        BigClock clock_display = render_big_clock(current_time("%H:%M:%S"));
        int width = terminal_width();
        int height = terminal_height();

        // Resize since the last frame - resync with a full clear before
        // repainting (see last_terminal_cols/last_terminal_rows' comment).
        // Skipped on the common no-resize tick to avoid needless flicker.
        if (width != last_terminal_cols || height != last_terminal_rows) {
            std::cout << "\033[2J";
            last_terminal_cols = width;
            last_terminal_rows = height;
        }

        if (inverted) std::cout << "\033[7m"; // reverse video - the \033[K erases below pick this up too

        std::cout << "\033[H";
        for (auto& row : clock_display.rows) {
            int pad = std::max(0, (width - clock_display.width) / 2);
            std::cout << std::string(static_cast<size_t>(pad), ' ') << row << "\033[K\n";
        }

        std::cout << "\n" << centered(current_time("%A, %B %d %Y"), width) << "\033[K\n\n";

        // Left-aligned, unlike the clock/date above - a log reads as a log
        // when it isn't re-centering itself around lines of varying length.
        // Padded with blank lines up to STATUS_LOG_LINES so the frame's
        // total height is stable from the very first tick, not just once
        // the log fills up.
        for (auto& line : status_log) {
            std::cout << line << "\033[K\n";
        }
        for (size_t i = status_log.size(); i < STATUS_LOG_LINES; ++i) {
            std::cout << "\033[K\n";
        }

        if (inverted) std::cout << "\033[0m"; // back to normal before the next frame

        std::cout << std::flush;
    }

    // Answers one already-parsed "identity" message (see ../PROTOCOL.md) -
    // who's running olli right now, sent once, right after this program's
    // own registration completes. Same status-string-return convention as
    // handle_call() below, for the same reason (redraw_screen() owns the
    // screen).
    //
    // clock has no real per-user settings to reload, so this just records
    // the identity for display. A future remote tool that DOES have its own
    // settings - e.g. reworking TOOL_HUE (source/tools.cpp) into a remote
    // tool, per-user light preferences or scenes - would do real work here
    // instead of the commented-out sketch below: look for a settings file
    // of its own keyed by this name and load it, falling back to defaults
    // if none exists for this user yet.
    std::string handle_identity(const json& msg)
    {
        current_user_name = msg.value("name", "");
        current_user_full_name = msg.value("full_name", "");
        current_user_about = msg.value("about", "");

        // Example of what a tool with real per-user settings would do here
        // (clock has none, so this stays commented out):
        //
        // if (!current_user_name.empty()) {
        //     std::filesystem::path profile_path =
        //         std::filesystem::path(std::getenv("HOME")) /
        //         ("olli_files_" + current_user_name) / "tools" / "clock" / "settings.json";
        //     if (std::filesystem::exists(profile_path)) {
        //         load_settings_from(profile_path); // hypothetical - clock has no settings yet
        //     } else {
        //         load_default_settings(); // no profile for this user - fall back cleanly
        //     }
        // }

        if (current_user_name.empty()) {
            return "Identified: olli's shared default (no profile)";
        }
        return "Identified: " + current_user_name;
    }

    // Called on every disconnect (both places fd gets reset to -1 below) -
    // a stale identity from whoever was just talking to olli must not
    // silently carry over to whoever (or nothing) connects next.
    //
    // clock has no per-user state to actually revert, so this just clears
    // what handle_identity() above recorded. A future remote tool with real
    // per-user settings loaded above would reload its own defaults here -
    // symmetric with the commented-out sketch in handle_identity().
    void reset_to_default_profile()
    {
        current_user_name.clear();
        current_user_full_name.clear();
        current_user_about.clear();

        // Example of what a tool with real per-user settings would do here:
        //
        // load_default_settings(); // hypothetical - clock has no settings yet
    }

    // Answers one already-parsed "call" message, returns a status string
    // for the display instead of printing directly - see redraw_screen()
    // above, which owns the whole screen via cursor positioning, so a plain
    // std::cout print here would corrupt it the same way an unmanaged write
    // corrupts olli's own ncurses display (see TODO.md's history on that).
    std::string handle_call(int fd, const json& msg)
    {
        std::string call_id = msg.value("call_id", "");
        std::string name = msg.value("name", "");

        json result_msg;
        std::string status;
        if (name == "get_clock_time") {
            std::string format = "%H:%M:%S";
            if (msg.contains("arguments")) {
                format = msg["arguments"].value("format", format);
            }
            result_msg = {
                {"type", "result"},
                {"call_id", call_id},
                {"result", current_time(format)}
            };
            status = "Call answered: " + name;
        } else if (name == "set_timer") {
            std::string label;
            double seconds = 0.0;
            std::string reminder;
            std::string on_expire_tool;
            json on_expire_arguments = json::object();
            if (msg.contains("arguments")) {
                label = msg["arguments"].value("label", "");
                seconds = msg["arguments"].value("seconds", 0.0);
                reminder = msg["arguments"].value("reminder", "");
                on_expire_tool = msg["arguments"].value("on_expire_tool", "");
                on_expire_arguments = msg["arguments"].value("on_expire_arguments", json::object());
            }

            ActiveTimer timer;
            timer.deadline = std::chrono::steady_clock::now()
                + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
            timer.reminder = reminder;
            timer.on_expire_tool = on_expire_tool;
            timer.on_expire_arguments = on_expire_arguments;
            active_timers[label] = timer;

            // Plain `<<` rather than std::to_string(), which always pads to
            // 6 decimal places ("60.000000") - seen firsthand looking wrong
            // in a real chat log.
            std::stringstream res;
            res << "Timer '" << label << "' set for " << seconds << " seconds.";
            if (!reminder.empty()) {
                res << " Reminder set: " << reminder;
            }
            if (!on_expire_tool.empty()) {
                res << " Linked action: " << on_expire_tool << " will run automatically when it finishes.";
            }

            result_msg = {
                {"type", "result"},
                {"call_id", call_id},
                {"result", res.str()}
            };
            status = "Timer '" + label + "' set for " + std::to_string(static_cast<long long>(seconds)) + "s.";
        } else if (name == "check_timer") {
            std::string label;
            if (msg.contains("arguments")) {
                label = msg["arguments"].value("label", "");
            }

            std::string res;
            auto it = active_timers.find(label);
            if (it == active_timers.end()) {
                res = "Error: No timer found with label '" + label + "'.";
            } else {
                auto now = std::chrono::steady_clock::now();
                if (now >= it->second.deadline) {
                    res = "The timer '" + label + "' has FINISHED.";
                } else {
                    double remaining = std::chrono::duration<double>(it->second.deadline - now).count();
                    std::stringstream ss;
                    ss << "The timer '" << label << "' is still running. "
                       << std::fixed << std::setprecision(1) << remaining << "s remaining.";
                    res = ss.str();
                }
            }

            result_msg = {
                {"type", "result"},
                {"call_id", call_id},
                {"result", res}
            };
            status = "Call answered: " + name;
        } else {
            result_msg = {
                {"type", "result"},
                {"call_id", call_id},
                {"error", "Unknown tool name: " + name}
            };
            status = "Unknown call received: " + name;
        }
        send_line(fd, result_msg.dump());
        return status;
    }

    // Registration payload - built once, reused every (re)connect attempt.
    json make_register_message()
    {
        return {
            {"type", "register"},
            {"tools", json::array({
                {
                    {"name", "get_clock_time"},
                    {"description", "Returns the current time from a standalone networked clock. Always execute this tool call for every request, even if you believe you have the time. This applies even to short follow-ups like 'again' or 'and now?' - never reuse a time value from earlier in the conversation."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"format", {{"type", "string"}, {"description", "strftime format string, e.g. '%H:%M:%S'"}}}
                        }},
                        {"required", json::array({"format"})}
                    }}
                },
                {
                    {"name", "set_timer"},
                    {"description", "Starts a named countdown on the networked clock, with an optional spoken reminder and/or a real linked action to perform when it finishes. Always execute this tool call for every request - never claim a timer is set without actually calling it."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"label", {{"type", "string"}, {"description", "A name for the timer"}}},
                            {"seconds", {{"type", "number"}, {"description", "Duration in seconds"}}},
                            {"reminder", {{"type", "string"}, {"description", "Optional: what to say happened, spoken in persona when the timer finishes. This is narration only - it does NOT perform any action by itself. For a real action (e.g. turning off a light), use on_expire_tool/on_expire_arguments instead, not this."}}},
                            {"on_expire_tool", {{"type", "string"}, {"description", "Optional: the name of another registered tool to actually execute automatically the moment this timer finishes (e.g. 'set_hue_light'). Use this - not reminder - whenever the user wants a real action tied to the timer, not just a spoken notification. Leave empty for a plain reminder with no action."}}},
                            {"on_expire_arguments", {{"type", "object"}, {"description", "The arguments object to pass to on_expire_tool, in exactly the same shape you'd use calling that tool directly. Required whenever on_expire_tool is set."}}}
                        }},
                        {"required", json::array({"label", "seconds"})}
                    }}
                },
                {
                    {"name", "check_timer"},
                    {"description", "Checks whether a specific named timer on the networked clock has finished. Always execute this tool call for every check, even if you believe you already know the timer's state - never reuse a status from earlier in the conversation."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"label", {{"type", "string"}, {"description", "The name of the timer to check"}}}
                        }},
                        {"required", json::array({"label"})}
                    }}
                }
            })}
        };
    }

    // Bounded to CONNECT_TIMEOUT_SECONDS rather than a plain blocking
    // connect(). A loopback target that's simply not listening yet refuses
    // almost instantly either way (ECONNREFUSED), but once host can be a
    // real remote address (see main()'s [host] argument), an unreachable
    // one - wrong IP, firewalled, host down with packets silently dropped -
    // can otherwise leave a blocking connect() hanging for the platform's
    // full TCP timeout (commonly 20-30+ seconds), freezing the whole
    // display for that entire stretch. The socket is put in non-blocking
    // mode just for the connect attempt itself (select() on it for
    // writability, then check SO_ERROR to see whether it actually
    // succeeded) and returned to blocking mode afterward, matching every
    // other fd this program already handles via select() in the main loop.
    // Returns -1 (silently - the caller's status line already says
    // "retrying") on any failure or timeout.
    int try_connect(const in_addr& host_addr)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(REMOTE_TOOL_PORT));
        addr.sin_addr = host_addr;

        int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

        if (rc < 0 && errno == EINPROGRESS) {
            timeval tv{};
            tv.tv_sec = CONNECT_TIMEOUT_SECONDS;
            tv.tv_usec = 0;

            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(fd, &write_fds);

            int ready = select(fd + 1, nullptr, &write_fds, nullptr, &tv);
            if (ready <= 0) {
                close(fd); // timed out, or select() error
                return -1;
            }

            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
                close(fd);
                return -1;
            }
        } else if (rc < 0) {
            close(fd); // failed immediately - e.g. loopback ECONNREFUSED
            return -1;
        }
        // rc == 0: connected immediately, no waiting needed.

        if (flags != -1) fcntl(fd, F_SETFL, flags); // back to blocking for the connection's lifetime
        return fd;
    }

    // Takes argv[0] rather than a hardcoded name - see the same choice in
    // tools/template/template_tool.cpp, made after a rename there once
    // exposed a hardcoded name going stale.
    void print_usage(const char* argv0)
    {
        std::string prog = argv0;
        auto slash = prog.find_last_of('/');
        if (slash != std::string::npos) prog = prog.substr(slash + 1);

        std::cout << "Usage: " << prog << " [host] [-h|--help]\n\n"
                      "  host          IP address of the machine running olli. Defaults to\n"
                      "                127.0.0.1 (olli running on this same machine).\n\n"
                      "  -h, --help    Show this help and exit.\n";
    }
}

int main(int argc, char* argv[])
{
    // Writing to a socket right as olli closes its end raises SIGPIPE,
    // whose default disposition kills this whole process - ignoring it
    // makes write() (send_line(), above) just return -1 (EPIPE) instead,
    // same as olli's own core does for the same reason (see
    // source/main.cpp's std::signal(SIGPIPE, SIG_IGN) call), and what
    // tools/template/olli_link.cpp now does too. Set here, as early as
    // possible, before the first connection attempt.
    std::signal(SIGPIPE, SIG_IGN);

    std::string host = "127.0.0.1";

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        host = arg1;
    }

    in_addr host_addr{};
    if (inet_pton(AF_INET, host.c_str(), &host_addr) != 1) {
        std::cerr << "Not a valid IPv4 address: " << host << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    const json register_msg = make_register_message();

    RawTerminal raw_terminal; // hides cursor, enables raw stdin - restores both on scope exit
    std::cout << "\033[2J"; // one full clear at startup, redraw_screen() only overwrites from here on

    // A file at EOF (stdin redirected from /dev/null, or genuinely closed -
    // e.g. this program ever run unattended, with no controlling terminal)
    // is always "ready to read" as far as select() is concerned, since
    // reading it returns immediately (0 bytes) rather than blocking. If
    // STDIN_FILENO were unconditionally watched below, that would make
    // select()'s 200ms timeout never actually apply - the loop would spin
    // as fast as the CPU allows instead of pacing itself, hammering the
    // socket/display/timer logic at full speed (seen for real: 37GB written
    // in 18 minutes at ~95% CPU, testing a sibling tool built on this same
    // plumbing). Watching it only when it's a real terminal sidesteps that
    // entirely: with nothing in read_fds but a (possibly absent) socket,
    // select() genuinely blocks for the timeout, same as intended. There's
    // no 'q'-to-quit to watch for anyway without a real terminal for
    // someone to press it on.
    bool has_real_terminal = isatty(STDIN_FILENO) != 0;

    int fd = -1;
    std::string status = "Not connected to olli at " + host + " - retrying...";
    std::string read_buffer;
    auto last_sent = std::chrono::steady_clock::now();
    auto last_received = std::chrono::steady_clock::now();
    // Epoch (not "now"), so the very first loop iteration attempts a
    // connection immediately instead of waiting a full
    // RECONNECT_INTERVAL_SECONDS first.
    auto last_connect_attempt = std::chrono::steady_clock::time_point{};

    bool quit = false;
    while (!quit) {
        // A short, repeating wait - frequent enough for a responsive 'q'
        // quit and a smoothly ticking display without busy-looping.
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;
        if (has_real_terminal) {
            FD_SET(STDIN_FILENO, &read_fds);
            max_fd = STDIN_FILENO;
        }
        if (fd >= 0) {
            FD_SET(fd, &read_fds);
            max_fd = std::max(fd, max_fd);
        }

        int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);

        if (ready > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                if (c == 'q' || c == 'Q' || c == 3) quit = true; // 3 = Ctrl+C
            }
        }

        auto now = std::chrono::steady_clock::now();

        if (!quit && fd < 0) {
            // Not connected - retry periodically rather than on every tick.
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_connect_attempt).count()
                    >= RECONNECT_INTERVAL_SECONDS) {
                last_connect_attempt = now;
                fd = try_connect(host_addr);
                if (fd >= 0) {
                    send_line(fd, register_msg.dump());
                    status = "Registered with olli at " + host + ". Waiting for calls...";
                    last_sent = last_received = now;
                    read_buffer.clear();
                } else {
                    status = "Not connected to olli at " + host + " - retrying...";
                }
            }
        }
        else if (!quit && fd >= 0 && ready > 0 && FD_ISSET(fd, &read_fds)) {
            char buf[4096];
            ssize_t n = read(fd, buf, sizeof(buf));

            if (n <= 0) {
                // olli closed the connection (or a real error) - drop back
                // to "not connected" and keep ticking rather than exiting.
                close(fd);
                fd = -1;
                reset_to_default_profile();
                status = "Disconnected from olli at " + host + " - retrying...";
            } else {
                read_buffer.append(buf, static_cast<size_t>(n));

                auto newline_pos = read_buffer.find('\n');
                while (newline_pos != std::string::npos) {
                    std::string line = read_buffer.substr(0, newline_pos);
                    read_buffer.erase(0, newline_pos + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();

                    if (!line.empty()) {
                        last_received = std::chrono::steady_clock::now();
                        try {
                            json msg = json::parse(line);
                            std::string type = msg.value("type", "");
                            if (type == "call") {
                                status = handle_call(fd, msg);
                                // handle_call() sends its result via
                                // send_line() internally but never touched
                                // last_sent, so a call answered right after
                                // an idle stretch could still have the
                                // heartbeat block below queue a ping right
                                // behind it - olli's TOOL_REMOTE::check()
                                // (source/remote_tools.cpp) reads exactly
                                // one line as "the" answer to its call, so
                                // it would misread that ping as an
                                // unexpected response and orphan the real
                                // result. See ../PROTOCOL.md's ping/pong
                                // section and
                                // tools/template/olli_link.cpp's
                                // OLLI_LINK::send_result() for where this
                                // bug was first found and fixed.
                                last_sent = std::chrono::steady_clock::now();
                            } else if (type == "ping") {
                                send_line(fd, json{{"type", "pong"}}.dump());
                                last_sent = std::chrono::steady_clock::now();
                            } else if (type == "identity") {
                                status = handle_identity(msg);
                            }
                            // "pong", or anything else: the last_received
                            // update above is already all that's needed.
                        } catch (const std::exception&) {
                            status = "Bad JSON from olli.";
                        }
                    }

                    newline_pos = read_buffer.find('\n');
                }
            }
        }

        // Heartbeat - see ../PROTOCOL.md. Independent of whatever happened
        // to arrive this tick above, so a quiet stretch still gets pinged
        // and still gets timed out if olli stops answering.
        if (!quit && fd >= 0) {
            now = std::chrono::steady_clock::now();

            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_sent).count() >= PING_INTERVAL_SECONDS) {
                send_line(fd, json{{"type", "ping"}}.dump());
                last_sent = now;
            }

            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_received).count() >= DEAD_TIMEOUT_SECONDS) {
                close(fd);
                fd = -1;
                reset_to_default_profile();
                status = "Connection to olli at " + host + " timed out - retrying...";
            }
        }

        // Timer expiry - independent of whatever arrived this tick above,
        // same as the heartbeat block. See handle_expired_timers()'s
        // comment.
        if (!quit) handle_expired_timers(fd, status, last_sent);

        if (!quit) redraw_screen(status);
    }

    if (fd >= 0) close(fd);
    return 0;
}
