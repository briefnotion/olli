#include "tool_server.hpp"

#include <algorithm>
#include <cerrno>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using json = nlohmann::json;

namespace {
    // Heartbeat timing - the same numbers as olli's TOOL_REMOTE
    // (source/remote_tools.h) and OLLI_LINK (../olli_link/olli_link.cpp),
    // so a tool sees this server behave exactly as it sees olli.
    constexpr int PING_INTERVAL_SECONDS = 5;
    constexpr int DEAD_TIMEOUT_SECONDS = 15;

    // A connection that never sends its register line is dropped - olli's
    // listener has no such timeout, but it only ever holds one half-open
    // connection; this one holds many, and a stalled one would sit forever.
    constexpr int REGISTER_TIMEOUT_SECONDS = 10;

    // Sanity limits, not tuning: loopback tools send short lines.
    constexpr size_t MAX_CONNECTIONS = 64;            // also keeps well inside select()'s FD_SETSIZE
    constexpr size_t MAX_LINE_BYTES = 4 * 1024 * 1024;   // an unterminated line this long is garbage
    constexpr size_t MAX_UNSENT_BYTES = 8 * 1024 * 1024; // a peer that never reads

    void set_nonblocking(int fd)
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    bool extract_line(std::string& buffer, std::string& out)
    {
        auto newline_pos = buffer.find('\n');
        if (newline_pos == std::string::npos) return false;
        out = buffer.substr(0, newline_pos);
        buffer.erase(0, newline_pos + 1);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    }

    // Tool names the model will see in a "Error: ..." line, so keep them short.
    std::string join_names(const std::set<std::string>& names)
    {
        std::string joined;
        for (const std::string& n : names) joined += (joined.empty() ? "" : ", ") + n;
        return joined;
    }
}

SUBCON_TOOL_SERVER::SUBCON_TOOL_SERVER(int port, int call_timeout_seconds_)
    : call_timeout_seconds(call_timeout_seconds_)
{
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) return;

    // So restarting right after a crash doesn't fail to bind while the old
    // socket lingers in TIME_WAIT.
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), static_cast<socklen_t>(sizeof(addr))) < 0 ||
        listen(listen_fd, 16) < 0) {
        close(listen_fd);
        listen_fd = -1;
        return;
    }
    set_nonblocking(listen_fd);
}

SUBCON_TOOL_SERVER::~SUBCON_TOOL_SERVER()
{
    for (CONNECTION& conn : connections)
        if (conn.fd >= 0) close(conn.fd);
    if (listen_fd >= 0) close(listen_fd);
}

void SUBCON_TOOL_SERVER::add_fds(fd_set& read_fds, fd_set& write_fds, int& max_fd) const
{
    if (listen_fd >= 0) {
        FD_SET(listen_fd, &read_fds);
        max_fd = std::max(max_fd, listen_fd);
    }
    for (const CONNECTION& conn : connections) {
        if (conn.fd < 0) continue;
        FD_SET(conn.fd, &read_fds);
        if (!conn.out.empty()) FD_SET(conn.fd, &write_fds);
        max_fd = std::max(max_fd, conn.fd);
    }
}

std::string SUBCON_TOOL_SERVER::describe(const CONNECTION& conn) const
{
    std::string text = "tool #" + std::to_string(conn.id);
    if (!conn.names.empty()) text += " (" + join_names(conn.names) + ")";
    return text;
}

void SUBCON_TOOL_SERVER::accept_new()
{
    while (true) {
        int fd = accept(listen_fd, nullptr, nullptr);
        if (fd < 0) return; // EAGAIN: nobody else waiting (or a real error - nothing useful to do either way)

        if (connections.size() >= MAX_CONNECTIONS) {
            close(fd);
            continue;
        }
        set_nonblocking(fd);

        CONNECTION conn;
        conn.fd = fd;
        conn.id = next_connection_id++;
        conn.accepted_at = conn.last_sent = conn.last_received = clock::now();
        connections.push_back(std::move(conn));
    }
}

void SUBCON_TOOL_SERVER::finish(PENDING_CALL& call, SERVER_CALL_RESULT result)
{
    if (call.callback) completions.emplace_back(std::move(call.callback), std::move(result));
    call.callback = nullptr;
}

void SUBCON_TOOL_SERVER::flush(CONNECTION& conn)
{
    while (conn.fd >= 0 && !conn.out.empty()) {
        // MSG_NOSIGNAL: a peer that vanished must not raise SIGPIPE here
        // (OLLI_LINK ignores it process-wide too, but this class shouldn't
        // depend on that).
        ssize_t n = send(conn.fd, conn.out.data(), conn.out.size(), MSG_NOSIGNAL);
        if (n > 0) {
            conn.out.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return; // kernel buffer full - add_fds() asks select() to tell us when it drains
        } else {
            conn.dead = true;
            return;
        }
    }
}

void SUBCON_TOOL_SERVER::send_line(CONNECTION& conn, const std::string& line)
{
    if (conn.dead) return;
    conn.out += line + "\n";
    if (conn.out.size() > MAX_UNSENT_BYTES) { conn.dead = true; return; }
    flush(conn);
    conn.last_sent = clock::now();
}

void SUBCON_TOOL_SERVER::pump_calls(CONNECTION& conn)
{
    if (conn.dead || !conn.registered || conn.has_inflight || conn.queued.empty()) return;

    conn.inflight = std::move(conn.queued.front());
    conn.queued.pop_front();
    conn.has_inflight = true;
    conn.inflight.sent_at = clock::now();

    json call_msg = {
        {"type", "call"},
        {"call_id", conn.inflight.call_id},
        {"name", conn.inflight.name},
        {"arguments", conn.inflight.arguments}
    };
    // replace, not strict: arguments are model-built text.
    send_line(conn, call_msg.dump(-1, ' ', false, json::error_handler_t::replace));
}

void SUBCON_TOOL_SERVER::handle_register(CONNECTION& conn, const json& msg)
{
    if (!msg.contains("tools") || !msg["tools"].is_array()) {
        conn.dead = true; // not a well-formed register - same as olli's listener, silently dropped
        return;
    }

    conn.tool_defs = json::array();
    conn.names.clear();
    for (const json& def : msg["tools"]) {
        if (!def.is_object() || !def.contains("name") || !def["name"].is_string()) continue;
        std::string name = def["name"].get<std::string>();
        if (name.empty() || conn.names.count(name)) continue;
        conn.names.insert(name);
        conn.tool_defs.push_back({
            {"name", name},
            {"description", def.value("description", "")},
            {"parameters", def.contains("parameters") ? def["parameters"] : json{{"type", "object"}, {"properties", json::object()}}}
        });
    }
    conn.registered = true;

    SERVER_NOTICE notice;
    notice.kind = SERVER_NOTICE::REGISTERED;
    notice.peer = describe(conn);
    notice.tool_names.assign(conn.names.begin(), conn.names.end());
    notices.push_back(std::move(notice));

    if (identity.is_object()) send_line(conn, identity.dump(-1, ' ', false, json::error_handler_t::replace));
}

void SUBCON_TOOL_SERVER::handle_line(CONNECTION& conn, const std::string& line)
{
    // Found in review 2026-10-29: nlohmann's value() throws type_error when
    // a field exists with the wrong type ("description": 5, "message": null,
    // "type": 5, "call_id": 7), and nothing above this caught it, so one
    // misbehaving tool took the whole process down with std::terminate. Any
    // such message now just drops that one connection (its pending calls
    // complete with a disconnect error) and says why.
    try {
        handle_message(conn, line);
    } catch (const std::exception& e) {
        conn.dead = true;
        SERVER_NOTICE notice;
        notice.kind = SERVER_NOTICE::PROBLEM;
        notice.peer = describe(conn);
        notice.message = std::string("sent a message with a wrongly-typed field (") + e.what() + ") - dropping the connection";
        notices.push_back(std::move(notice));
    }
}

void SUBCON_TOOL_SERVER::handle_message(CONNECTION& conn, const std::string& line)
{
    if (line.empty()) return;
    conn.last_received = clock::now(); // any message, even garbage, is proof of life - same as olli

    json msg;
    try { msg = json::parse(line); }
    catch (const std::exception&) {
        if (!conn.registered) conn.dead = true; // first line must be a real register
        return;
    }
    if (!msg.is_object()) return;
    std::string type = msg.value("type", "");

    if (!conn.registered) {
        if (type != "register") conn.dead = true;
        else handle_register(conn, msg);
        return;
    }

    if (type == "ping") {
        send_line(conn, json{{"type", "pong"}}.dump());
    } else if (type == "register") {
        // A tool re-registering on the same connection: take the new list.
        handle_register(conn, msg);
    } else if (type == "result") {
        std::string call_id = msg.value("call_id", "");
        if (!conn.has_inflight || conn.inflight.call_id != call_id) return; // late answer to a timed-out/abandoned call
        SERVER_CALL_RESULT result;
        if (msg.contains("error")) {
            result.ok = false;
            result.text = "Error: " + (msg["error"].is_string() ? msg["error"].get<std::string>() : msg["error"].dump());
        } else if (msg.contains("result") && msg["result"].is_string()) {
            result.text = msg["result"].get<std::string>();
        } else if (msg.contains("result")) {
            result.text = msg["result"].dump();
        }
        conn.has_inflight = false;
        finish(conn.inflight, std::move(result));
        pump_calls(conn);
    } else if (type == "event") {
        SERVER_NOTICE notice;
        notice.kind = SERVER_NOTICE::EVENT;
        notice.peer = describe(conn);
        notice.message = msg.value("message", "");
        notice.origin_id = msg.value("origin_id", "");
        if (msg.contains("var") && msg["var"].is_object()) {
            notice.var_name = msg["var"].value("name", "");
            notice.var_value = msg["var"].value("value", "");
        }
        notices.push_back(std::move(notice));
    }
    // "pong", "identity" echoes, anything unknown: last_received above is all that's needed.
}

void SUBCON_TOOL_SERVER::read_from(CONNECTION& conn)
{
    char buf[8192];
    while (!conn.dead) {
        ssize_t n = read(conn.fd, buf, sizeof(buf));
        if (n > 0) {
            conn.in.append(buf, static_cast<size_t>(n));
            if (static_cast<size_t>(n) < sizeof(buf)) break;
        } else if (n == 0) {
            conn.dead = true; // the peer closed
            return;
        } else {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) conn.dead = true;
            break;
        }
    }

    std::string line;
    while (!conn.dead && extract_line(conn.in, line)) handle_line(conn, line);
    if (conn.in.size() > MAX_LINE_BYTES) conn.dead = true;
}

void SUBCON_TOOL_SERVER::service(const fd_set& read_fds, const fd_set& write_fds)
{
    if (listen_fd >= 0 && FD_ISSET(listen_fd, &read_fds)) accept_new();

    for (CONNECTION& conn : connections) {
        if (conn.fd < 0 || conn.dead) continue;
        if (FD_ISSET(conn.fd, &write_fds)) flush(conn);
        if (FD_ISSET(conn.fd, &read_fds)) read_from(conn);
    }

    auto now = clock::now();
    auto seconds_since = [&](clock::time_point t) {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(now - t).count());
    };

    for (CONNECTION& conn : connections) {
        if (conn.dead) continue;

        if (!conn.registered) {
            if (seconds_since(conn.accepted_at) >= REGISTER_TIMEOUT_SECONDS) conn.dead = true;
            continue;
        }

        // A dispatched call that never got an answer. The connection itself
        // stays (the heartbeat decides whether the tool is alive) - the
        // caller just gets a result it can tell the model about, so a stuck
        // tool can never hang a think cycle.
        if (conn.has_inflight && seconds_since(conn.inflight.sent_at) >= call_timeout_seconds) {
            conn.has_inflight = false;
            finish(conn.inflight, {"Error: the tool did not answer within " + std::to_string(call_timeout_seconds) + " seconds.", false});
        }
        pump_calls(conn);

        if (seconds_since(conn.last_sent) >= PING_INTERVAL_SECONDS) send_line(conn, json{{"type", "ping"}}.dump());

        // Found in review 2026-10-29: the tools are single-threaded, so one
        // busy running a call cannot answer our pings, and the 15 s dead
        // timeout dropped it mid-call - the effective call timeout was 15 s
        // however high call_timeout_seconds was set. While a call to this
        // connection is in flight and still within its own timeout, silence
        // is expected and the heartbeat is not held against it. Once the
        // call times out (above) the normal rule applies again, so a tool
        // that hangs outright is still dropped at the call timeout.
        bool call_in_time = conn.has_inflight && seconds_since(conn.inflight.sent_at) < call_timeout_seconds;
        if (!call_in_time && seconds_since(conn.last_received) >= DEAD_TIMEOUT_SECONDS) conn.dead = true;
    }

    // Retire dead connections: anything still pending on them completes
    // with an error now rather than being forgotten.
    for (auto it = connections.begin(); it != connections.end();) {
        if (!it->dead) { ++it; continue; }

        if (it->registered) {
            SERVER_NOTICE notice;
            notice.kind = SERVER_NOTICE::DISCONNECTED;
            notice.peer = describe(*it);
            notice.tool_names.assign(it->names.begin(), it->names.end());
            notices.push_back(std::move(notice));
        }
        SERVER_CALL_RESULT gone{"Error: the tool disconnected before it answered.", false};
        if (it->has_inflight) finish(it->inflight, gone);
        for (PENDING_CALL& pending : it->queued) finish(pending, gone);
        if (it->fd >= 0) close(it->fd);
        it = connections.erase(it);
    }

    // Callbacks last, from a local copy: a callback is free to call call()
    // or abandon(), which touch 'connections', and may even complete more
    // calls (an immediate failure) - those land in 'completions' for the
    // next tick rather than being run re-entrantly.
    std::vector<std::pair<SERVER_CALL_CALLBACK, SERVER_CALL_RESULT>> to_run;
    to_run.swap(completions);
    for (auto& entry : to_run) entry.first(entry.second);
}

std::vector<SERVER_NOTICE> SUBCON_TOOL_SERVER::take_notices()
{
    std::vector<SERVER_NOTICE> out;
    out.swap(notices);
    return out;
}

bool SUBCON_TOOL_SERVER::has_tool(const std::string& name) const
{
    for (const CONNECTION& conn : connections)
        if (conn.registered && !conn.dead && conn.names.count(name)) return true;
    return false;
}

json SUBCON_TOOL_SERVER::tool_defs() const
{
    json all = json::array();
    std::set<std::string> seen;
    for (const CONNECTION& conn : connections) {
        if (!conn.registered || conn.dead) continue;
        for (const json& def : conn.tool_defs) {
            if (seen.insert(def["name"].get<std::string>()).second) all.push_back(def);
        }
    }
    return all;
}

std::string SUBCON_TOOL_SERVER::call(const std::string& name, const json& arguments, SERVER_CALL_CALLBACK callback)
{
    for (CONNECTION& conn : connections) {
        if (!conn.registered || conn.dead || !conn.names.count(name)) continue;

        PENDING_CALL pending;
        pending.call_id = "subcon_call_" + std::to_string(next_call_number++);
        pending.name = name;
        pending.arguments = arguments.is_object() ? arguments : json::object();
        pending.callback = std::move(callback);
        std::string id = pending.call_id;
        conn.queued.push_back(std::move(pending));
        pump_calls(conn);
        return id;
    }
    return "";
}

void SUBCON_TOOL_SERVER::abandon(const std::string& call_id)
{
    for (CONNECTION& conn : connections) {
        if (conn.has_inflight && conn.inflight.call_id == call_id) {
            conn.has_inflight = false;
            conn.inflight = PENDING_CALL{};
            pump_calls(conn);
            return;
        }
        for (auto it = conn.queued.begin(); it != conn.queued.end(); ++it) {
            if (it->call_id == call_id) { conn.queued.erase(it); return; }
        }
    }
    // Possibly already completed and waiting in 'completions' - drop it there too.
    // (Completions carry no id, so the callback itself must tolerate a stale
    // firing; the reasoner's callbacks check their cycle token for this.)
}

void SUBCON_TOOL_SERVER::set_identity(const json& identity_message)
{
    if (identity == identity_message) return; // olli re-sends identity on each of its own reconnects; tools needn't hear it twice
    identity = identity_message;
    for (CONNECTION& conn : connections)
        if (conn.registered && !conn.dead) send_line(conn, identity.dump(-1, ' ', false, json::error_handler_t::replace));
}

size_t SUBCON_TOOL_SERVER::connection_count() const
{
    size_t n = 0;
    for (const CONNECTION& conn : connections) if (conn.registered && !conn.dead) ++n;
    return n;
}

std::vector<std::string> SUBCON_TOOL_SERVER::connection_summaries() const
{
    std::vector<std::string> lines;
    for (const CONNECTION& conn : connections)
        if (conn.registered && !conn.dead) lines.push_back(describe(conn));
    return lines;
}
