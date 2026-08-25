// TEMPLATE for a new olli remote tool - see ../PROTOCOL.md for the wire
// protocol this implements, and ../clock/clock.cpp for a fuller worked
// example (this file is where that one grew from).
//
// To make a new tool from this: copy this whole directory
// (`cp -r tools/template tools/your_tool_name`), rename template_tool.cpp
// and the Makefile's target to match, then fill in the two spots below
// marked "CUSTOMIZE" - nothing else needs to change unless your tool
// genuinely needs different connection behavior, not just different logic.
// See tools/template/README.md for the full walkthrough.
//
// Layout: all the generic "talk to olli" plumbing (connect/reconnect,
// heartbeat, socket read/write, message framing) lives in olli_link.hpp/
// olli_link.cpp, in this same directory - see that pair for how it works.
// This file is everything specific to what THIS tool actually does:
//   - make_register_message() / handle_call() (below) - what this tool is
//     called, what it does, and what happens when the model calls it.
//   - olli_processing() - called once per main-loop tick; its own top half
//     is where you route each message type to the logic that answers it
//     (handle_call() above is where the actual per-name work happens),
//     its bottom half is the calls into OLLI_LINK that do the actual
//     talking - you shouldn't need to touch that part.
//   - main() - user setup, one OLLI_LINK, then a loop that calls
//     olli_processing() once per tick alongside whatever else your tool
//     needs to do (a display, background polling, etc).
//
// What OLLI_LINK already gives you, working, for free:
//   - Connects to olli, retries every few seconds if it's not there yet,
//     and keeps retrying if it goes away later - your tool never needs to
//     exit just because olli isn't running (see ../PROTOCOL.md's
//     "Heartbeat + reconnect" section for why this matters, and what it
//     does and doesn't guard against).
//   - Sends whatever you declare in make_register_message() below on every
//     (re)connect, and a ready-to-use send_result()/send_error()/
//     send_event() for answering calls and pushing unsolicited events (see
//     ../PROTOCOL.md's `event` message) - call send_event() whenever your
//     tool has something to say unprompted, e.g. an alarm firing.
//   - Heartbeat (ping/pong) so a hung (not just crashed) connection to olli
//     gets noticed and cleaned up, on both sides.
//
// This file separately gives you (not olli-specific, so not in OLLI_LINK):
//   - [host] argument + -h/--help, matching olli's own [name]/--help
//     convention.
//   - A live terminal display (optional - see the note above RawTerminal
//     below if your tool doesn't need one).
//
// Build: `make` in this directory. Run: `./template_tool [host]` - does not
// need olli to already be running.

#include <nlohmann/json.hpp>

#include "olli_link.hpp"

#include <algorithm>
#include <iostream>
#include <string>

#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using json = nlohmann::json;

namespace {
    // =====================================================================
    // CUSTOMIZE #1 - what this tool registers, and how it answers a call.
    // =====================================================================

    // TODO: describe what your tool actually registers - name/description/
    // parameters, the same shape every TOOL_*::register_tool in olli's own
    // source/tools.cpp builds via add_tool() (description is prose, for the
    // model to know *when* to reach for it; parameters is a JSON-schema,
    // for *how* to call it). Register more than one callable name from the
    // same program by adding more entries to the "tools" array - see
    // TOOL_GET_CURRENT_TIME in olli's own source/tools.cpp for an example
    // of one class/program registering two names at once.
    json make_register_message()
    {
        return {
            {"type", "register"},
            {"tools", json::array({
                {
                    {"name", "example_tool_action"},
                    {"description", "TODO: describe what this does and when the model should call it."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"example_argument", {{"type", "string"}, {"description", "TODO: describe this argument"}}}
                        }},
                        {"required", json::array({"example_argument"})}
                    }}
                }
            })}
        };
    }

    // TODO: answer one already-parsed "call" message - matches whatever
    // name(s) you registered above. Send the answer via link.send_result()
    // or link.send_error() (a tool with something to say unprompted, not in
    // answer to a call, uses link.send_event() instead - see
    // ../clock/clock.cpp's set_timer/handle_expired_timers() for a worked
    // example of that). Returns a status string for the display below; if
    // your tool has no display, ignore the return value or replace it with
    // a log line instead.
    std::string handle_call(OLLI_LINK& link, const json& msg)
    {
        std::string call_id = msg.value("call_id", "");
        std::string name = msg.value("name", "");

        if (name == "example_tool_action") {
            std::string example_argument;
            if (msg.contains("arguments")) {
                example_argument = msg["arguments"].value("example_argument", "");
            }

            // TODO: do the actual work here, using example_argument (or
            // whatever your own registered parameters are called).
            std::string result = "TODO: real result for '" + example_argument + "'";

            link.send_result(call_id, result);
            return "Call answered: " + name;
        }

        link.send_error(call_id, "Unknown tool name: " + name);
        return "Unknown call received: " + name;
    }

    // =====================================================================
    // End of CUSTOMIZE #1.
    // =====================================================================

    // Called once per main-loop tick - see the file-level comment above for
    // the shape. socket_readable is whatever this tick's select() found for
    // link.fd(); status is main()'s display status line, updated here.
    void olli_processing(OLLI_LINK& link, bool socket_readable, std::string& status)
    {
        // =================================================================
        // CUSTOMIZE #2 - route each message type your tool cares about to
        // the logic that answers it. Add another branch here for any other
        // message type you want to handle (e.g. "identity" - see
        // ../clock/clock.cpp's handle_identity() for a worked example this
        // template doesn't include yet).
        // =================================================================
        auto dispatch = [&](const json& msg) {
            std::string type = msg.value("type", "");
            if (type == "call") status = handle_call(link, msg);
        };
        // =================================================================
        // End of CUSTOMIZE #2.
        // =================================================================

        // ---------------------------------------------------------------
        // Below this line: olli communication plumbing (see
        // olli_link.hpp / olli_link.cpp). Nothing here needs to change for
        // a new tool.
        // ---------------------------------------------------------------
        link.service(socket_readable);

        json msg;
        while (link.next_message(msg)) dispatch(msg);

        if (!link.status().empty()) status = link.status();
    }

    // --- Terminal handling ---
    //
    // Optional: if your tool has no live display of its own (a background
    // daemon, say), delete RawTerminal, redraw_screen(), the stdin-watching
    // half of main()'s select() call, and the redraw_screen() call at the
    // bottom of the loop. Nothing else here depends on any of it - the
    // connection/registration/heartbeat logic works the same either way.

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
            }

            RawTerminal(const RawTerminal&) = delete;
            RawTerminal& operator=(const RawTerminal&) = delete;

        private:
            termios old_termios{};
            bool active = false;
    };

    // Minimal placeholder display - just the status line, repositioned to
    // the top-left each tick rather than scrolling. Replace this with your
    // own tool's actual display (see tools/clock/clock.cpp's
    // render_big_clock()/redraw_screen() for a fuller worked example - a
    // big ASCII-art digit renderer), or delete it entirely (see the note
    // above RawTerminal) if this tool has nothing to show.
    void redraw_screen(const std::string& status)
    {
        std::cout << "\033[H\033[2K" << status << std::flush;
    }

    // Takes argv[0] rather than a hardcoded name, so this can't go stale
    // when this file (and the Makefile target) get renamed for a new tool -
    // see README.md in this directory.
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
    // ---- user declarations ----
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

    // ---- olli communications declaration ----
    OLLI_LINK link(host, host_addr, make_register_message());

    // ---- user code ----
    RawTerminal raw_terminal; // hides cursor, enables raw stdin - restores both on scope exit
    std::cout << "\033[2J"; // one full clear at startup, redraw_screen() only overwrites from here on

    // A file at EOF (stdin redirected from /dev/null, or genuinely closed -
    // e.g. this program ever run unattended, with no controlling terminal)
    // is always "ready to read" as far as select() is concerned, since
    // reading it returns immediately (0 bytes) rather than blocking. If
    // STDIN_FILENO were unconditionally watched below, that would make
    // select()'s 200ms timeout never actually apply - the loop would spin
    // as fast as the CPU allows instead of pacing itself, hammering the
    // socket/display logic at full speed (seen for real: 37GB written in 18
    // minutes at ~95% CPU, testing a tool built on this same plumbing).
    // Watching it only when it's a real terminal sidesteps that entirely:
    // with nothing in read_fds but a (possibly absent) socket, select()
    // genuinely blocks for the timeout, same as intended. There's no
    // 'q'-to-quit to watch for anyway without a real terminal for someone
    // to press it on. If your tool has no display at all (see the note
    // above RawTerminal), this still matters just as much - keep it.
    bool has_real_terminal = isatty(STDIN_FILENO) != 0;

    std::string status = "Not connected to olli at " + host + " - retrying...";

    bool quit = false;
    while (!quit) {
        // ---- user code: wait for stdin/socket activity ----

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
        if (link.fd() >= 0) {
            FD_SET(link.fd(), &read_fds);
            max_fd = std::max(link.fd(), max_fd);
        }

        int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);

        if (ready > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                if (c == 'q' || c == 'Q' || c == 3) quit = true; // 3 = Ctrl+C
            }
        }

        bool socket_readable = link.fd() >= 0 && ready > 0 && FD_ISSET(link.fd(), &read_fds);

        // ---- olli communications ----
        if (!quit) olli_processing(link, socket_readable, status);

        // ---- user code ----
        if (!quit) redraw_screen(status);
    }

    // ---- closing code ----
    return 0;
}
