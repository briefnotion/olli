#include "olli_link.hpp"

#include <cerrno>
#include <csignal>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>

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

    // Extracts one newline-delimited line from the front of buffer, if a
    // complete one is present (stripping a trailing \r for CRLF senders).
    bool extract_line(std::string& buffer, std::string& out)
    {
        auto newline_pos = buffer.find('\n');
        if (newline_pos == std::string::npos) return false;

        out = buffer.substr(0, newline_pos);
        buffer.erase(0, newline_pos + 1);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
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
}

OLLI_LINK::OLLI_LINK(std::string host_display, in_addr host_addr_, json register_message_)
    : host(std::move(host_display)),
      host_addr(host_addr_),
      register_message(std::move(register_message_)),
      last_sent(std::chrono::steady_clock::now()),
      last_received(std::chrono::steady_clock::now())
{
    // Writing to a socket right as olli closes its end raises SIGPIPE,
    // whose default disposition kills this whole process - ignoring it
    // makes write() (send_line(), above) just return -1 (EPIPE) instead,
    // same as olli's own core does for the exact same reason (see
    // source/main.cpp's std::signal(SIGPIPE, SIG_IGN) call). Set here
    // rather than in each tool's own main() so every tool gets this for
    // free just by constructing an OLLI_LINK, same as everything else this
    // class already handles - and early enough, since this constructor
    // always runs before the first connection attempt.
    std::signal(SIGPIPE, SIG_IGN);
}

OLLI_LINK::~OLLI_LINK()
{
    if (sock_fd >= 0) close(sock_fd);
}

void OLLI_LINK::handle_disconnect(const std::string& reason)
{
    close(sock_fd);
    sock_fd = -1;
    status_text = reason;
    disconnected_flag = true;
}

void OLLI_LINK::service(bool socket_readable)
{
    status_text.clear();
    auto now = std::chrono::steady_clock::now();

    // Set when a "call"/"identity"/etc. line gets queued below - the
    // caller answers a queued "call" via send_result()/send_error() in
    // this very same tick, right after service() returns (see
    // olli_processing() in the tool's own .cpp). If that hasn't happened
    // yet when the heartbeat check further down runs, and the connection
    // had been quiet long enough, a ping would go out first - and olli's
    // TOOL_REMOTE::check() (source/remote_tools.cpp) only ever reads ONE
    // line as "the" answer to its call, so it would read that ping instead
    // of the real result moments behind it, report "unexpected response
    // from remote tool", and leave the real result orphaned in its buffer
    // to desync a later, unrelated call. Real bug, caught the hard way
    // building tools/hue/ - see its own history.json for what this looked
    // like from olli's side. Suppressing the ping for this one tick
    // defers it at most until the next 200ms tick, which is nothing next
    // to PING_INTERVAL_SECONDS.
    bool queued_message_this_tick = false;

    if (sock_fd < 0) {
        // Not connected - retry periodically rather than on every tick.
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_connect_attempt).count()
                >= RECONNECT_INTERVAL_SECONDS) {
            last_connect_attempt = now;
            sock_fd = try_connect(host_addr);
            if (sock_fd >= 0) {
                send_line(sock_fd, register_message.dump());
                status_text = "Registered with olli at " + host + ". Waiting for calls...";
                last_sent = last_received = now;
                read_buffer.clear();
            } else {
                status_text = "Not connected to olli at " + host + " - retrying...";
            }
        }
    } else if (socket_readable) {
        char buf[4096];
        ssize_t n = read(sock_fd, buf, sizeof(buf));

        if (n <= 0) {
            // olli closed the connection (or a real error) - drop back to
            // "not connected" and let the caller keep running rather than
            // exiting.
            handle_disconnect("Disconnected from olli at " + host + " - retrying...");
        } else {
            read_buffer.append(buf, static_cast<size_t>(n));

            std::string line;
            while (extract_line(read_buffer, line)) {
                if (line.empty()) continue;

                last_received = std::chrono::steady_clock::now();
                try {
                    json msg = json::parse(line);
                    std::string type = msg.value("type", "");
                    if (type == "ping") {
                        send_line(sock_fd, json{{"type", "pong"}}.dump());
                        last_sent = std::chrono::steady_clock::now();
                    } else if (type != "pong") {
                        // "pong" needs nothing beyond the last_received
                        // update above. Everything else (call/identity/
                        // anything a future message type adds) is the
                        // caller's business, not this class's.
                        incoming.push_back(std::move(msg));
                        queued_message_this_tick = true;
                    }
                } catch (const std::exception&) {
                    status_text = "Bad JSON from olli.";
                }
            }
        }
    }

    // Heartbeat - independent of whatever happened above this tick, so a
    // quiet stretch still gets pinged and still gets timed out if olli
    // stops answering. Also covers the tick a fresh connection was just
    // made above (sock_fd >= 0 by now in that case too).
    if (sock_fd >= 0) {
        now = std::chrono::steady_clock::now();

        if (!queued_message_this_tick &&
                std::chrono::duration_cast<std::chrono::seconds>(now - last_sent).count() >= PING_INTERVAL_SECONDS) {
            send_line(sock_fd, json{{"type", "ping"}}.dump());
            last_sent = now;
        }

        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_received).count() >= DEAD_TIMEOUT_SECONDS) {
            handle_disconnect("Connection to olli at " + host + " timed out - retrying...");
        }
    }
}

bool OLLI_LINK::next_message(json& out)
{
    if (incoming.empty()) return false;
    out = std::move(incoming.front());
    incoming.pop_front();
    return true;
}

bool OLLI_LINK::consume_disconnected()
{
    bool was = disconnected_flag;
    disconnected_flag = false;
    return was;
}

void OLLI_LINK::send_result(const std::string& call_id, const std::string& result)
{
    if (sock_fd < 0) return;
    send_line(sock_fd, json{{"type", "result"}, {"call_id", call_id}, {"result", result}}.dump());
    // Counts as "we sent something" for heartbeat purposes, same as a ping
    // or pong - see PROTOCOL.md's ping/pong section ("either message
    // counts as proof of life either way") and service()'s own
    // queued_message_this_tick comment for why this matters, not just
    // being thorough: without it, a call answered right after an idle
    // stretch could still get a redundant ping queued behind it on the
    // very next tick.
    last_sent = std::chrono::steady_clock::now();
}

void OLLI_LINK::send_error(const std::string& call_id, const std::string& error)
{
    if (sock_fd < 0) return;
    send_line(sock_fd, json{{"type", "result"}, {"call_id", call_id}, {"error", error}}.dump());
    last_sent = std::chrono::steady_clock::now();
}

void OLLI_LINK::send_event(const std::string& message, const json& action)
{
    if (sock_fd < 0) return;
    json msg{{"type", "event"}, {"message", message}};
    if (action.is_object() && !action.empty()) msg["action"] = action;
    send_line(sock_fd, msg.dump());
    last_sent = std::chrono::steady_clock::now();
}
