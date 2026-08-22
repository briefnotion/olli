// The real networked clock (see ../PROTOCOL.md) - connects to olli's
// remote-tool listener, registers get_clock_time, and answers each "call"
// with the real current time. The big ASCII-art digital display (classic
// "tty-clock" style) runs independently of olli's lifecycle: this program
// can start before olli does, keeps ticking while disconnected, notices
// when olli becomes reachable and registers, and if olli goes away
// (cleanly or not) just falls back to "disconnected, retrying" and keeps
// ticking rather than exiting - see the heartbeat/reconnect note in
// ../PROTOCOL.md.
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
#include <ctime>
#include <vector>
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

    std::string centered(const std::string& text, int width)
    {
        int pad = std::max(0, (width - static_cast<int>(text.size())) / 2);
        return std::string(static_cast<size_t>(pad), ' ') + text;
    }

    // \033[K after each line clears any leftover from a previous, wider
    // frame - needed since this repositions to \033[H and overwrites rather
    // than clearing the whole screen every redraw (avoids a visible flicker
    // once a second). Runs regardless of connection state - the clock keeps
    // ticking whether or not olli is reachable right now.
    void redraw_screen(const std::string& status)
    {
        BigClock clock_display = render_big_clock(current_time("%H:%M:%S"));
        int width = terminal_width();

        std::cout << "\033[H";
        for (auto& row : clock_display.rows) {
            int pad = std::max(0, (width - clock_display.width) / 2);
            std::cout << std::string(static_cast<size_t>(pad), ' ') << row << "\033[K\n";
        }

        std::cout << "\n" << centered(current_time("%A, %B %d %Y"), width) << "\033[K\n";
        std::cout << "\n" << status << "\033[K" << std::flush;
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
                    {"description", "Returns the current time from a standalone networked clock."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"format", {{"type", "string"}, {"description", "strftime format string, e.g. '%H:%M:%S'"}}}
                        }},
                        {"required", json::array({"format"})}
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
        FD_SET(STDIN_FILENO, &read_fds);
        int max_fd = STDIN_FILENO;
        if (fd >= 0) {
            FD_SET(fd, &read_fds);
            max_fd = std::max(fd, STDIN_FILENO);
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
                status = "Connection to olli at " + host + " timed out - retrying...";
            }
        }

        if (!quit) redraw_screen(status);
    }

    if (fd >= 0) close(fd);
    return 0;
}
