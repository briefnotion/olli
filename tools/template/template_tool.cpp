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
// What this file already gives you, working, for free:
//   - Connects to olli, retries every RECONNECT_INTERVAL_SECONDS if it's
//     not there yet, and keeps retrying if it goes away later - your tool
//     never needs to exit just because olli isn't running (see
//     ../PROTOCOL.md's "Heartbeat + reconnect" section for why this
//     matters, and what it does and doesn't guard against).
//   - Registers whatever tool(s) you declare in make_register_message()
//     below, and answers calls for them via handle_call() below.
//   - A ready-to-use send_event() helper for the unsolicited push path
//     (see ../PROTOCOL.md's event message) - call it whenever your tool has
//     something to say unprompted, e.g. an alarm firing.
//   - Heartbeat (ping/pong) so a hung (not just crashed) connection to olli
//     gets noticed and cleaned up, on both sides.
//   - [host] argument + -h/--help, matching olli's own [name]/--help
//     convention.
//   - A live terminal display (optional - see the note above RawTerminal
//     below if your tool doesn't need one).
//
// Build: `make` in this directory. Run: `./template_tool [host]` - does not
// need olli to already be running.

#include <nlohmann/json.hpp>

#include <iostream>
#include <string>
#include <chrono>
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

    // Sends an unsolicited event - the push equivalent of olli's own
    // TOOL_TIMER::monitor_tool() noticing an expired timer. Call this
    // wherever your tool notices something worth telling olli about on its
    // own, not in response to a call - e.g. inside the main loop below, or
    // from wherever your tool's own logic lives. Not called anywhere by
    // default in this template - [[maybe_unused]] so that's not a build
    // warning until you do use it.
    [[maybe_unused]] void send_event(int fd, const std::string& message)
    {
        send_line(fd, json{{"type", "event"}, {"message", message}}.dump());
    }

    // =====================================================================
    // CUSTOMIZE #1 - what this tool registers, and how it answers a call.
    // Everything below this block, down to its matching end marker, is
    // generic connection plumbing that every remote tool needs unchanged.
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
    // name(s) you registered above. Returns a status string for the
    // display below; if your tool has no display, ignore the return value
    // or replace it with a log line instead.
    std::string handle_call(int fd, const json& msg)
    {
        std::string call_id = msg.value("call_id", "");
        std::string name = msg.value("name", "");

        json result_msg;
        std::string status;
        if (name == "example_tool_action") {
            std::string example_argument;
            if (msg.contains("arguments")) {
                example_argument = msg["arguments"].value("example_argument", "");
            }

            // TODO: do the actual work here, using example_argument (or
            // whatever your own registered parameters are called).
            std::string result = "TODO: real result for '" + example_argument + "'";

            result_msg = {
                {"type", "result"},
                {"call_id", call_id},
                {"result", result}
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

    // =====================================================================
    // End of CUSTOMIZE #1.
    // =====================================================================

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

    // Bounded to CONNECT_TIMEOUT_SECONDS rather than a plain blocking
    // connect(). A loopback target that's simply not listening yet refuses
    // almost instantly either way (ECONNREFUSED), but once host can be a
    // real remote address, an unreachable one - wrong IP, firewalled, host
    // down with packets silently dropped - can otherwise leave a blocking
    // connect() hanging for the platform's full TCP timeout (commonly
    // 20-30+ seconds), freezing the whole display for that entire stretch.
    // The socket is put in non-blocking mode just for the connect attempt
    // itself (select() on it for writability, then check SO_ERROR to see
    // whether it actually succeeded) and returned to blocking mode
    // afterward, matching every other fd this program already handles via
    // select() in the main loop. Returns -1 (silently - the caller's status
    // line already says "retrying") on any failure or timeout.
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

    // Extracts one newline-delimited line from the front of buffer, if a
    // complete one is present (stripping a trailing \r for CRLF senders) -
    // same helper olli's own REMOTE_TOOL_LISTENER/TOOL_REMOTE use
    // (source/remote_tools.cpp).
    bool extract_line(std::string& buffer, std::string& out)
    {
        auto newline_pos = buffer.find('\n');
        if (newline_pos == std::string::npos) return false;

        out = buffer.substr(0, newline_pos);
        buffer.erase(0, newline_pos + 1);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
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
    // socket/display logic at full speed (seen for real: 37GB written in 18
    // minutes at ~95% CPU, testing a tool built on this same plumbing).
    // Watching it only when it's a real terminal sidesteps that entirely:
    // with nothing in read_fds but a (possibly absent) socket, select()
    // genuinely blocks for the timeout, same as intended. There's no
    // 'q'-to-quit to watch for anyway without a real terminal for someone
    // to press it on. If your tool has no display at all (see the note
    // above RawTerminal), this still matters just as much - keep it.
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
                // to "not connected" and keep running rather than exiting.
                close(fd);
                fd = -1;
                status = "Disconnected from olli at " + host + " - retrying...";
            } else {
                read_buffer.append(buf, static_cast<size_t>(n));

                std::string line;
                while (extract_line(read_buffer, line)) {
                    if (line.empty()) continue;

                    last_received = std::chrono::steady_clock::now();
                    try {
                        json msg = json::parse(line);
                        std::string type = msg.value("type", "");
                        if (type == "call") {
                            status = handle_call(fd, msg);
                        } else if (type == "ping") {
                            send_line(fd, json{{"type", "pong"}}.dump());
                            last_sent = std::chrono::steady_clock::now();
                        }
                        // "pong", or anything else: the last_received
                        // update above is already all that's needed.
                    } catch (const std::exception&) {
                        status = "Bad JSON from olli.";
                    }
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
                status = "Connection to olli at " + host + " timed out - retrying...";
            }
        }

        if (!quit) redraw_screen(status);
    }

    if (fd >= 0) close(fd);
    return 0;
}
