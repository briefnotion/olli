#ifndef remote_tools_cpp
#define remote_tools_cpp

#include "remote_tools.h"
#include "olla.h"

#include <cerrno>
#include <cstring>
#include <chrono>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace {
    // Shared by both the listening socket and each accepted connection -
    // every read/accept in poll() must never block, since it's called once
    // per main-loop tick.
    void set_nonblocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) {
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    // Used by TOOL_REMOTE::check() below - writes the whole line even if
    // the kernel's send buffer only takes part of it in one write() call.
    bool write_line(int fd, const std::string& line)
    {
        std::string with_newline = line + "\n";
        size_t total_written = 0;
        while (total_written < with_newline.size()) {
            ssize_t n = write(fd, with_newline.data() + total_written, with_newline.size() - total_written);
            if (n <= 0) return false;
            total_written += static_cast<size_t>(n);
        }
        return true;
    }

    // Extracts one newline-delimited line from the front of buffer, if a
    // complete one is present (stripping a trailing \r for CRLF senders).
    // Shared by REMOTE_TOOL_LISTENER::poll() and both of TOOL_REMOTE's own
    // readers below, rather than each repeating the same few lines.
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

REMOTE_TOOL_LISTENER::REMOTE_TOOL_LISTENER()
{
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return;

    // So restarting olli right after a crash doesn't fail to bind with
    // "Address already in use" while the old socket lingers in TIME_WAIT.
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(PORT));
    // Loopback only, matching the current "localhost for now" scope (see
    // tools/PROTOCOL.md) - not reachable from elsewhere on the network yet.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), static_cast<socklen_t>(sizeof(addr))) < 0) {
        close(listen_fd);
        listen_fd = -1;
        return;
    }

    if (listen(listen_fd, 4) < 0) {
        close(listen_fd);
        listen_fd = -1;
        return;
    }

    set_nonblocking(listen_fd);
}

REMOTE_TOOL_LISTENER::~REMOTE_TOOL_LISTENER()
{
    if (client_fd >= 0) close(client_fd);
    if (listen_fd >= 0) close(listen_fd);
}

std::optional<REMOTE_TOOL_REGISTRATION> REMOTE_TOOL_LISTENER::poll()
{
    if (listen_fd < 0) return std::nullopt;

    if (client_fd < 0) {
        int accepted = accept(listen_fd, nullptr, nullptr);
        if (accepted >= 0) {
            set_nonblocking(accepted);
            client_fd = accepted;
            read_buffer.clear();
        }
        // EWOULDBLOCK/EAGAIN just means nobody's connecting yet - not an
        // error worth doing anything about here.
    }

    if (client_fd < 0) return std::nullopt;

    char buf[4096];
    ssize_t n = read(client_fd, buf, sizeof(buf));

    if (n > 0) {
        read_buffer.append(buf, static_cast<size_t>(n));

        std::string line;
        if (!extract_line(read_buffer, line)) {
            // A partial line - wait for more on a later tick.
            return std::nullopt;
        }

        // The handshake is one message, pass or fail - ownership of fd
        // transfers out of this class either way from this point on
        // (handed to the caller on success, closed by us on failure).
        int handshake_fd = client_fd;
        client_fd = -1;

        json parsed;
        try {
            parsed = json::parse(line);
        } catch (const std::exception&) {
            close(handshake_fd);
            return std::nullopt;
        }

        if (parsed.value("type", "") != "register" || !parsed.contains("tools") || !parsed["tools"].is_array()) {
            close(handshake_fd);
            return std::nullopt;
        }

        REMOTE_TOOL_REGISTRATION reg;
        reg.fd = handshake_fd;
        reg.tools = parsed["tools"];
        return reg;
    }

    if (n == 0) {
        // Peer closed the connection before finishing registration.
        close(client_fd);
        client_fd = -1;
        read_buffer.clear();
        return std::nullopt;
    }

    // n < 0: either EWOULDBLOCK/EAGAIN (nothing to read yet, totally
    // normal for a non-blocking socket) or a real error - either way,
    // nothing arrived this tick.
    return std::nullopt;
}

TOOL_REMOTE::TOOL_REMOTE(int socket_fd, json tools_array)
    : fd(socket_fd), tool_defs(std::move(tools_array))
{
}

TOOL_REMOTE::~TOOL_REMOTE()
{
    if (fd >= 0) close(fd);
}

void TOOL_REMOTE::mark_dead()
{
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
    read_buffer.clear();
}

// No per-instance setup needed - part of the common tool interface (see the note in tools.h).
void TOOL_REMOTE::configure(ollama_system&) {}

void TOOL_REMOTE::register_tool(ollama_system&, json& tools)
{
    for (auto& def : tool_defs) {
        add_tool(
            tools,
            def.value("name", ""),
            def.value("description", ""),
            def.value("parameters", json::object())
        );
    }
}

bool TOOL_REMOTE::read_line_blocking(std::string& out, int timeout_ms)
{
    // A line may already be sitting in read_buffer from a previous read
    // that pulled in more than one line's worth of bytes at once - check
    // before touching the socket at all.
    if (extract_line(read_buffer, out)) return true;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) return false;

        timeval tv{};
        tv.tv_sec = static_cast<time_t>(remaining.count() / 1000000);
        tv.tv_usec = static_cast<suseconds_t>(remaining.count() % 1000000);

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);

        int ready = select(fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (ready <= 0) return false; // 0 = timed out, <0 = select() error

        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) return false; // closed or a real error

        read_buffer.append(buf, static_cast<size_t>(n));

        if (extract_line(read_buffer, out)) return true;
        // Partial line - select() again for the rest.
    }
}

bool TOOL_REMOTE::check(ollama_system& chat, const ToolCall& tc)
{
    bool is_mine = false;
    for (auto& def : tool_defs) {
        if (def.value("name", "") == tc.name) {
            is_mine = true;
            break;
        }
    }
    if (!is_mine) return false;

    chat.log("[System] Tool call received: " + tc.name + " (remote)\n");

    json call_msg = {
        {"type", "call"},
        {"call_id", tc.id},
        {"name", tc.name},
        {"arguments", tc.arguments}
    };

    std::string response_str;

    if (fd < 0) {
        response_str = "Error: remote tool connection is down.";
    } else if (!write_line(fd, call_msg.dump())) {
        mark_dead(); // the write itself failed - genuinely broken, not just slow
        response_str = "Error: remote tool connection is down.";
    } else {
        last_sent = std::chrono::steady_clock::now();

        std::string line;
        // 5 seconds - generous for a localhost round trip, short enough
        // that a hung remote tool doesn't stall the whole chat turn
        // indefinitely (same reasoning as TOOL_WEB_SEARCH's own timeouts).
        if (!read_line_blocking(line, 5000)) {
            // Treated as dead rather than "maybe just slow" - otherwise a
            // truly hung remote tool would cost a fresh 5s stall on every
            // future call instead of failing fast once removed from
            // tools_list (see is_alive(), checked in olla.cpp's process()).
            mark_dead();
            response_str = "Error: remote tool did not respond in time.";
        } else {
            last_received = std::chrono::steady_clock::now();
            json result_msg;
            bool parsed_ok = true;
            try {
                result_msg = json::parse(line);
            } catch (const std::exception&) {
                parsed_ok = false;
            }

            // Known gap even with monitor_tool() below implemented: if a
            // remote tool pushes an unsolicited event at the exact moment
            // check() is blocked here waiting for a call's result, that
            // event line gets consumed as this call's (mismatched) response
            // instead of being handled as an event - it's treated as a
            // protocol error and lost rather than passed along. Not a live
            // problem for tools/clock/clock.cpp (it never pushes mid-call),
            // but a real one for anything that might.
            if (!parsed_ok || result_msg.value("type", "") != "result"
                || result_msg.value("call_id", "") != tc.id) {
                response_str = "Error: unexpected response from remote tool.";
            } else if (result_msg.contains("error")) {
                response_str = "Error: " + result_msg.value("error", "unknown remote error");
            } else {
                response_str = result_msg.value("result", "");
            }
        }
    }

    chat.send_tool_result(tc.id, response_str);
    chat.integrate_tool_result("", response_str);

    return true;
}

// Non-blocking (unlike check()'s read_line_blocking) - this runs every
// process() tick alongside every other tool's monitor_tool(), so it must
// never wait. Three things happen here, in order: (1) check for one line
// arriving unprompted - an event gets forwarded via
// chat.integrate_tool_result(), e.g. tools/clock/clock.cpp's set_timer
// noticing an expired timer, a ping gets an immediate pong reply, anything else
// (including a plain pong) just counts as proof of life; (2) send our own
// ping if nothing's been sent in PING_INTERVAL_SECONDS; (3) mark_dead() if
// nothing's been received at all in DEAD_TIMEOUT_SECONDS - see the class
// comment and tools/PROTOCOL.md for why. mark_dead() also runs here on a
// cleanly closed connection, same as before the heartbeat existed - either
// way, is_alive() flips to false, which ollama_system::process() (olla.cpp)
// checks every tick to actually drop this instance from tools_list.
void TOOL_REMOTE::monitor_tool(ollama_system& chat)
{
    if (fd < 0) return;

    std::string line;
    bool got_line = extract_line(read_buffer, line);

    if (!got_line) {
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n > 0) {
            read_buffer.append(buf, static_cast<size_t>(n));
            got_line = extract_line(read_buffer, line);
        }
        else if (n == 0) {
            mark_dead(); // Peer closed the connection normally.
            return;
        }
        // n < 0: EWOULDBLOCK/EAGAIN (normal) or a real error - either way no
        // line arrived this tick; fall through to the heartbeat check below
        // regardless.
    }

    if (got_line) {
        last_received = std::chrono::steady_clock::now();

        json msg;
        bool parsed_ok = true;
        try {
            msg = json::parse(line);
        } catch (const std::exception&) {
            parsed_ok = false; // malformed - still counts as proof of life, nothing else to do with it
        }

        if (parsed_ok) {
            std::string type = msg.value("type", "");
            if (type == "ping") {
                if (write_line(fd, json{{"type", "pong"}}.dump())) {
                    last_sent = std::chrono::steady_clock::now();
                } else {
                    mark_dead();
                    return;
                }
            } else if (type == "event") {
                std::string message = msg.value("message", "");
                if (!message.empty()) {
                    chat.log("[RemoteTools] Event from remote tool: " + message + "\n");
                    chat.integrate_tool_result("", message);
                }

                // Optional structured follow-up action, separate from
                // message's narration above - see tools/PROTOCOL.md's
                // `event` shape. Queued via pending_tool_calls (olla.h) for
                // real execution through the normal tools_list dispatch
                // (ollama_system::handle_instance_tools()), same as any
                // model-issued call - this class has no idea what the
                // named tool actually is, same as everywhere else here.
                if (msg.contains("action") && msg["action"].is_object()) {
                    std::string action_tool = msg["action"].value("tool", "");
                    json action_args = msg["action"].value("arguments", json::object());
                    if (!action_tool.empty()) {
                        static int next_id = 0;
                        chat.pending_tool_calls.push({
                            "system_action_" + std::to_string(++next_id),
                            action_tool,
                            action_args
                        });
                    }
                }
            }
            // "pong", or anything else: the last_received update above is
            // already all that's needed.
        }
    }

    auto now = std::chrono::steady_clock::now();

    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_sent).count() >= PING_INTERVAL_SECONDS) {
        if (write_line(fd, json{{"type", "ping"}}.dump())) {
            last_sent = now;
        } else {
            mark_dead();
            return;
        }
    }

    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_received).count() >= DEAD_TIMEOUT_SECONDS) {
        mark_dead();
    }
}

#endif
