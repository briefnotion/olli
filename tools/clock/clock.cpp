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

#include "../olli_link/olli_link.hpp"
#include "../olli_display/olli_display.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <vector>
#include <map>
#include <algorithm>

#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using json = nlohmann::json;

namespace {

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
    void handle_expired_timers(OLLI_LINK& link, OLLI_DISPLAY& display, std::string& status)
    {
        auto now = std::chrono::steady_clock::now();
        for (auto& [label, timer] : active_timers) {
            if (now < timer.deadline) continue;

            if (!timer.blinked) {
                timer.blinked = true;
                blink_ticks_left = BLINK_TOTAL_TICKS;

                // The countdown line update_running_timer_lines() has been
                // keeping alive stops the moment a timer crosses its
                // deadline (see that function) - this is the one-shot
                // handoff to a "went off at..." line of its own, armed to
                // auto-clear from the activity area 30s after this exact
                // moment rather than lingering there forever.
                std::string activity_line = "Timer '" + label + "' went off at " + current_time("%I:%M:%S %p");
                if (!timer.reminder.empty()) activity_line += " - " + timer.reminder;
                display.set_activity_line(label, activity_line, 30);
            }

            if (timer.event_sent || !link.is_connected()) continue;

            std::stringstream ss;
            ss << "### [TIMER EXPIRED] ###\n";
            ss << "The wait time for '" << label << "' is complete.\n";
            if (!timer.reminder.empty()) {
                ss << "Target action: " << timer.reminder << ".\n";
            }
            ss << "Inform the user in character.";

            json action = nullptr;
            if (!timer.on_expire_tool.empty()) {
                action = {
                    {"tool", timer.on_expire_tool},
                    {"arguments", timer.on_expire_arguments}
                };
            }

            link.send_event(ss.str(), action);
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

    // --- Display ---

    // Rows the clock face itself needs inside OLLI_DISPLAY's tool area: 5
    // for the big digits (see BigClock), a blank line, the date line, and
    // one more blank line before the shared connection/profile/status
    // lines start - see draw_clock_face() below, which lays out exactly
    // this many rows and no more.
    constexpr int TOOL_AREA_HEIGHT = 8;

    // Draws the big digit clock + date into display's tool area. Runs every
    // tick regardless of connection state - the clock keeps ticking whether
    // or not olli is reachable right now.
    void draw_clock_face(OLLI_DISPLAY& display)
    {
        WINDOW* win = display.tool_area();
        int height = 0, width = 0;
        getmaxyx(win, height, width);
        (void)height;

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

        werase(win);
        if (inverted) wattron(win, A_REVERSE);

        int pad = std::max(0, (width - clock_display.width) / 2);
        for (size_t r = 0; r < clock_display.rows.size(); ++r) {
            mvwaddstr(win, static_cast<int>(r), pad, clock_display.rows[r].c_str());
        }

        std::string date_line = current_time("%A, %B %d %Y");
        int date_pad = std::max(0, (width - static_cast<int>(date_line.size())) / 2);
        mvwaddstr(win, static_cast<int>(clock_display.rows.size()) + 1, date_pad, date_line.c_str());

        if (inverted) wattroff(win, A_REVERSE);
        display.refresh_tool_area();
    }

    // One activity-area line per timer still counting down - keyed by
    // label, same as active_timers itself, so this and
    // handle_expired_timers()'s own set_activity_line() call never fight
    // over the same line: this owns it strictly before the deadline,
    // handle_expired_timers() strictly from the moment it's crossed.
    void update_running_timer_lines(OLLI_DISPLAY& display)
    {
        auto now = std::chrono::steady_clock::now();
        for (auto& [label, timer] : active_timers) {
            if (now >= timer.deadline) continue;

            int total_seconds = static_cast<int>(std::chrono::duration<double>(timer.deadline - now).count() + 0.5);
            int mins = total_seconds / 60;
            int secs = total_seconds % 60;

            std::stringstream ss;
            ss << "Timer '" << label << "': " << mins << ":" << std::setfill('0') << std::setw(2) << secs << " remaining";
            if (!timer.reminder.empty()) ss << " - will " << timer.reminder;

            display.set_activity_line(label, ss.str());
        }
    }

    // Answers one already-parsed "identity" message (see ../PROTOCOL.md) -
    // who's running olli right now, sent once, right after this program's
    // own registration completes.
    //
    // Unlike handle_call() below, this doesn't return a status string for
    // the shared tool-status line - OLLI_DISPLAY already has a dedicated,
    // always-current Profile line (see main()'s
    // display.set_profile(current_user_name...) call, run every tick), so
    // a one-shot "Identified: X" on the status line would just be the same
    // fact shown twice, and would blot out whatever real tool activity
    // (timer set, call answered, ...) was on that line beforehand.
    //
    // clock has no real per-user settings to reload, so this just records
    // the identity for display. A future remote tool that DOES have its own
    // settings - e.g. reworking TOOL_HUE (source/tools.cpp) into a remote
    // tool, per-user light preferences or scenes - would do real work here
    // instead of the commented-out sketch below: look for a settings file
    // of its own keyed by this name and load it, falling back to defaults
    // if none exists for this user yet.
    void handle_identity(const json& msg)
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
    std::string handle_call(OLLI_LINK& link, const json& msg)
    {
        std::string call_id = msg.value("call_id", "");
        std::string name = msg.value("name", "");

        std::string status;
        if (name == "get_clock_time") {
            std::string format = "%H:%M:%S";
            if (msg.contains("arguments")) {
                format = msg["arguments"].value("format", format);
            }
            link.send_result(call_id, current_time(format));
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

            link.send_result(call_id, res.str());
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

            link.send_result(call_id, res);
            status = "Call answered: " + name;
        } else {
            link.send_error(call_id, "Unknown tool name: " + name);
            status = "Unknown call received: " + name;
        }
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

    OLLI_LINK link(host, host_addr, make_register_message());
    OLLI_DISPLAY display(TOOL_AREA_HEIGHT); // owns the terminal from here on - restores it on scope exit

    // connection_status/tool_status are the display's two lower fixed
    // lines (see olli_display.hpp) - kept as separate variables rather than
    // the single shared `status` this used to be, since a connection change
    // and a tool event (a call answered, a timer set) are unrelated facts
    // that used to silently overwrite each other on whichever happened most
    // recently.
    std::string connection_status = "Not connected to olli at " + host + " - retrying...";
    std::string tool_status;

    bool quit = false;
    while (!quit) {
        display.tick(); // picks up a resize, expires any timed-out activity line

        // A short, repeating wait - frequent enough for a smoothly ticking
        // display without busy-looping. Keyboard input is read separately
        // below via display.get_key(), which - unlike the raw
        // read(STDIN_FILENO) this replaced - is routed through ncurses and
        // doesn't need its own guard against a closed/non-terminal stdin
        // spinning this loop (see get_key()'s own comment).
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;
        if (link.fd() >= 0) {
            FD_SET(link.fd(), &read_fds);
            max_fd = link.fd();
        }

        int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);

        int key = display.get_key();
        if (key == 'q' || key == 'Q' || key == 3) quit = true; // 3 = Ctrl+C

        bool socket_readable = link.fd() >= 0 && ready > 0 && FD_ISSET(link.fd(), &read_fds);

        if (!quit) {
            link.service(socket_readable);

            if (link.consume_disconnected()) reset_to_default_profile();

            json msg;
            while (link.next_message(msg)) {
                std::string type = msg.value("type", "");
                if (type == "call") tool_status = handle_call(link, msg);
                else if (type == "identity") handle_identity(msg);
            }

            if (!link.status().empty()) connection_status = link.status();
        }

        // Timer expiry - independent of whatever arrived this tick above,
        // same as the heartbeat handled inside link.service(). See
        // handle_expired_timers()'s comment.
        if (!quit) handle_expired_timers(link, display, tool_status);

        if (!quit) {
            update_running_timer_lines(display);
            draw_clock_face(display);

            display.set_connection(connection_status);
            display.set_profile(current_user_name.empty() ? "Profile: (shared default)" : "Profile: " + current_user_name);
            display.set_tool_status(tool_status);

            display.present();
        }
    }

    return 0;
}
