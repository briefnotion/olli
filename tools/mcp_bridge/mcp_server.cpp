#include "mcp_server.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {
    constexpr size_t MAX_LINE_BYTES = 8 * 1024 * 1024;       // one message from the server; longer is dropped
    constexpr size_t MAX_DISCARD_BYTES = 64 * 1024 * 1024;   // ...and a message that never ends gets the server stopped
    constexpr size_t MAX_PENDING_OUTPUT_BYTES = 1024 * 1024; // what we have written but the child has not read
    constexpr size_t MAX_TOOLS_PER_SERVER = 500;
    constexpr int MAX_LIST_PAGES = 20;
    constexpr int MAX_JSON_DEPTH = 64;                       // nesting of a message from the server
    constexpr int MAX_START_FAILURES = 5;                    // in a row, before giving up until told to start again
    constexpr int STABLE_SECONDS = 60;                       // ready this long = the failure count starts over
    constexpr int TERMINATE_GRACE_SECONDS = 3;               // SIGTERM -> SIGKILL
    constexpr off_t MAX_STDERR_LOG_BYTES = 1024 * 1024;
    const char* const PROTOCOL_VERSION = "2025-06-18";

    // The only parts of the bridge's own environment a child gets (see the class comment).
    bool environment_name_allowed(const std::string& name)
    {
        static const char* const exact[] = {
            "PATH", "HOME", "USER", "LOGNAME", "LANG", "LANGUAGE", "TZ", "TMPDIR",
            "XDG_CACHE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME",
            "HTTP_PROXY", "HTTPS_PROXY", "NO_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "no_proxy", "all_proxy",
            "SSL_CERT_FILE", "SSL_CERT_DIR", "REQUESTS_CA_BUNDLE", "CURL_CA_BUNDLE", "NODE_EXTRA_CA_CERTS"
        };
        for (const char* allowed : exact) {
            if (name == allowed) return true;
        }
        return name.rfind("LC_", 0) == 0;
    }

    std::vector<std::string> build_environment(const MCP_SERVER_CONFIG& cfg)
    {
        std::map<std::string, std::string> merged;
        for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
            std::string text = *entry;
            auto eq = text.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string name = text.substr(0, eq);
            if (environment_name_allowed(name)) merged[name] = text.substr(eq + 1);
        }
        for (const auto& [name, value] : cfg.env) merged[name] = value;

        std::vector<std::string> out;
        for (const auto& [name, value] : merged) out.push_back(name + "=" + value);
        return out;
    }

    std::string json_error_text(const json& message)
    {
        auto it = message.find("error");
        if (it == message.end()) return "unknown error";
        std::string text;
        long long code = 0;
        bool have_code = false;
        if (it->is_object()) {
            auto m = it->find("message");
            if (m != it->end() && m->is_string()) text = m->get<std::string>();
            auto c = it->find("code");
            if (c != it->end() && c->is_number_integer()) { code = c->get<long long>(); have_code = true; }
        }
        if (text.empty()) text = "the server returned an error";
        text = clean_description(text, 400);
        if (have_code) text += " (code " + std::to_string(code) + ")";
        return text;
    }

    std::string describe_wait_status(int status)
    {
        if (WIFEXITED(status)) return "exited with code " + std::to_string(WEXITSTATUS(status));
        if (WIFSIGNALED(status)) return "was ended by signal " + std::to_string(WTERMSIG(status));
        return "ended";
    }
}

MCP_SERVER::MCP_SERVER(MCP_SERVER_CONFIG config, fs::path work_root, std::function<void(const std::string&)> log)
    : cfg(std::move(config)), root(std::move(work_root)), log_line(std::move(log))
{
}

MCP_SERVER::~MCP_SERVER()
{
    shutdown_blocking();
}

std::string MCP_SERVER::state_text() const
{
    switch (current_state) {
        case State::STOPPED:  return "stopped";
        case State::STARTING: return "starting";
        case State::READY:    return "ready, " + std::to_string(tool_list.size()) + (tool_list.size() == 1 ? " tool" : " tools");
        case State::BACKOFF: {
            auto seconds = std::chrono::duration_cast<std::chrono::seconds>(restart_at - Clock::now()).count();
            return "restarting in " + std::to_string(std::max<long long>(0, seconds)) + " s (" + last_error_text + ")";
        }
        case State::FAILED:   return "failed: " + last_error_text;
    }
    return "unknown";
}

void MCP_SERVER::set_state(State next)
{
    if (next == current_state) return;
    current_state = next;
    state_changed_flag = true;
}

void MCP_SERVER::set_tools(std::vector<json> new_tools)
{
    if (json(new_tools) == json(tool_list)) return;
    tool_list = std::move(new_tools);
    tools_changed_flag = true;
    ++tools_generation_count;
}

bool MCP_SERVER::take_tools_changed() { bool was = tools_changed_flag; tools_changed_flag = false; return was; }
bool MCP_SERVER::take_state_changed() { bool was = state_changed_flag; state_changed_flag = false; return was; }

void MCP_SERVER::start()
{
    if (current_state == State::STARTING || current_state == State::READY) return;
    consecutive_failures = 0;
    last_error_text.clear();
    begin();
}

void MCP_SERVER::begin()
{
    read_buffer.clear();
    out_buffer.clear();
    pending.clear();
    discarding_oversized = false;
    discarded_bytes = 0;
    list_accumulator.clear();
    relist_after_ready = false;

    spawn_failure_is_permanent = false;
    if (!spawn()) {
        // A bad command fails the same way every time; running out of pipes or processes may pass.
        fail(last_error_text, spawn_failure_is_permanent);
        return;
    }
    started_at = Clock::now();
    set_state(State::STARTING);
    log_line("[" + cfg.name + "] started '" + cfg.command[0] + "' (process " + std::to_string(pid) + ")");

    send_request(Kind::INITIALIZE, "initialize", {
        {"protocolVersion", PROTOCOL_VERSION},
        {"capabilities", json::object()},
        {"clientInfo", {{"name", "olli-mcp-bridge"}, {"version", "0.1"}}}
    });
}

bool MCP_SERVER::spawn()
{
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::path work_dir = cfg.cwd.empty() ? root / cfg.name : fs::path(cfg.cwd);
    if (cfg.cwd.empty()) fs::create_directories(work_dir, ec);

    // The child's stderr goes to a file (private, trimmed when it gets big), never to the screen.
    const fs::path log_path = root / (cfg.name + ".stderr.log");
    struct stat st{};
    if (stat(log_path.c_str(), &st) == 0 && st.st_size > MAX_STDERR_LOG_BYTES) {
        if (truncate(log_path.c_str(), 0) != 0) { /* keep appending */ }
    }
    int log_fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (log_fd < 0) log_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);

    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    int exec_error[2] = {-1, -1};
    if (pipe2(to_child, O_CLOEXEC) != 0 || pipe2(from_child, O_CLOEXEC) != 0 || pipe2(exec_error, O_CLOEXEC) != 0) {
        last_error_text = std::string("could not create pipes: ") + std::strerror(errno);
        for (int fd : {to_child[0], to_child[1], from_child[0], from_child[1], exec_error[0], exec_error[1], log_fd}) {
            if (fd >= 0) close(fd);
        }
        return false;
    }

    // Everything the child needs is built BEFORE fork(): no allocation between fork() and exec().
    std::vector<std::string> env_strings = build_environment(cfg);
    std::vector<char*> argv;
    for (auto& part : cfg.command) argv.push_back(const_cast<char*>(part.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (auto& entry : env_strings) envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);
    const std::string work_dir_text = work_dir.string();

    pid_t child = fork();
    if (child < 0) {
        last_error_text = std::string("could not start a process: ") + std::strerror(errno);
        for (int fd : {to_child[0], to_child[1], from_child[0], from_child[1], exec_error[0], exec_error[1], log_fd}) close(fd);
        return false;
    }

    if (child == 0) {
        // ---- the child ----
        setpgid(0, 0);                       // its own group, so stop() can end everything it starts
        prctl(PR_SET_PDEATHSIG, SIGTERM);    // the bridge dying takes the server with it
        std::signal(SIGPIPE, SIG_DFL);       // we ignore it; "ignored" would be inherited across exec
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd < 0 || max_fd > 4096) max_fd = 4096;
        for (int fd = 3; fd < max_fd; ++fd) {
            if (fd != exec_error[1]) close(fd);      // olli's sockets, the instance lock, other servers' pipes
        }
        int failure = 0;
        if (chdir(work_dir_text.c_str()) != 0) {
            failure = errno;
            if (write(exec_error[1], &failure, sizeof failure) < 0) { /* nothing more to do */ }
            _exit(126);
        }
        execvpe(argv[0], argv.data(), envp.data());
        failure = errno;
        if (write(exec_error[1], &failure, sizeof failure) < 0) { /* nothing more to do */ }
        _exit(127);
    }

    // ---- the bridge ----
    close(to_child[0]);
    close(from_child[1]);
    close(exec_error[1]);
    close(log_fd);

    // A successful exec closes the child's end of exec_error (it is close-on-exec) without writing anything.
    int failure = 0;
    ssize_t got;
    do { got = read(exec_error[0], &failure, sizeof failure); } while (got < 0 && errno == EINTR);
    close(exec_error[0]);
    if (got == static_cast<ssize_t>(sizeof failure)) {
        int status = 0;
        waitpid(child, &status, 0);
        close(to_child[1]);
        close(from_child[0]);
        last_error_text = "could not start '" + cfg.command[0] + "': " + std::strerror(failure);
        spawn_failure_is_permanent = true;
        return false;
    }

    pid = child;
    in_fd = to_child[1];
    out_fd = from_child[0];
    fcntl(in_fd, F_SETFL, fcntl(in_fd, F_GETFL, 0) | O_NONBLOCK);
    fcntl(out_fd, F_SETFL, fcntl(out_fd, F_GETFL, 0) | O_NONBLOCK);
    return true;
}

void MCP_SERVER::kill_child()
{
    if (in_fd >= 0) close(in_fd);
    if (out_fd >= 0) close(out_fd);
    in_fd = out_fd = -1;
    out_buffer.clear();
    read_buffer.clear();
    if (pid > 0) {
        kill(-pid, SIGTERM);   // the whole group: an "npx" wrapper and the real server under it
        dying.push_back({pid, Clock::now() + std::chrono::seconds(TERMINATE_GRACE_SECONDS), false, false});
        pid = -1;
    }
}

void MCP_SERVER::leave_group_behind(pid_t leader)
{
    // The leader was just reaped. Whatever it started may still be running in its process group (a node
    // child under npx, say) - ask it to end now, and keep the group on the list so it can be forced later.
    // While any member is alive the group id cannot be reused by another process, so signalling it is safe.
    if (kill(-leader, SIGTERM) == 0) {
        dying.push_back({leader, Clock::now() + std::chrono::seconds(TERMINATE_GRACE_SECONDS), false, true});
    }
}

void MCP_SERVER::reap()
{
    const auto now = Clock::now();
    for (auto it = dying.begin(); it != dying.end();) {
        if (!it->leader_reaped) {
            pid_t r = waitpid(it->pid, nullptr, WNOHANG);
            if (r == it->pid || (r < 0 && errno == ECHILD)) it->leader_reaped = true;
        }
        if (it->leader_reaped && kill(-it->pid, 0) != 0 && errno == ESRCH) {
            it = dying.erase(it);       // nobody left in the group
            continue;
        }
        if (!it->killed && now >= it->kill_at) {
            kill(-it->pid, SIGKILL);
            it->killed = true;
        } else if (it->killed && now >= it->kill_at + std::chrono::seconds(10)) {
            it = dying.erase(it);       // still listed 10 s after SIGKILL (a zombie nobody reaps): stop watching it
            continue;
        }
        ++it;
    }
}

void MCP_SERVER::fail_all_calls(const std::string& why)
{
    for (const auto& [id, kind] : pending) {
        if (kind != Kind::CALL) continue;
        RESPONSE response;
        response.id = id;
        response.ok = false;
        response.error = why;
        finished.push_back(std::move(response));
    }
    pending.clear();
}

void MCP_SERVER::fail_pending_calls_only(const std::string& why)
{
    for (auto it = pending.begin(); it != pending.end();) {
        if (it->second != Kind::CALL) { ++it; continue; }
        RESPONSE response;
        response.id = it->first;
        response.ok = false;
        response.error = why;
        finished.push_back(std::move(response));
        it = pending.erase(it);
    }
}

void MCP_SERVER::fail(const std::string& why, bool permanent)
{
    last_error_text = why;
    log_line("[" + cfg.name + "] " + why);

    const bool was_stable = current_state == State::READY &&
        std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - ready_at).count() >= STABLE_SECONDS;

    kill_child();
    fail_all_calls("the MCP server '" + cfg.name + "' stopped (" + why + ")");

    if (was_stable) consecutive_failures = 0;
    ++consecutive_failures;

    if (permanent || consecutive_failures >= MAX_START_FAILURES) {
        if (!permanent) last_error_text = why + " (gave up after " + std::to_string(consecutive_failures) + " tries)";
        set_state(State::FAILED);
        set_tools({});
    } else {
        const int delay = std::min(60, 2 << (consecutive_failures - 1));   // 2, 4, 8, 16 s
        restart_at = Clock::now() + std::chrono::seconds(delay);
        set_state(State::BACKOFF);   // its tool list is kept: a restart is expected to bring the same tools back
    }
}

void MCP_SERVER::stop()
{
    kill_child();
    fail_all_calls("the MCP server '" + cfg.name + "' was stopped");
    consecutive_failures = 0;
    set_state(State::STOPPED);
    set_tools({});
}

void MCP_SERVER::shutdown_blocking()
{
    stop();
    for (int i = 0; i < 70 && !dying.empty(); ++i) {   // up to ~3.5 s, same as the SIGTERM grace period
        reap();
        if (dying.empty()) break;
        usleep(50000);
    }
    for (auto& d : dying) {
        kill(-d.pid, SIGKILL);
        if (!d.leader_reaped) waitpid(d.pid, nullptr, 0);
    }
    dying.clear();
}

void MCP_SERVER::tick()
{
    reap();
    const auto now = Clock::now();

    if (current_state == State::BACKOFF && now >= restart_at) {
        begin();
        return;
    }

    if (pid > 0) {
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            const pid_t leader = pid;
            pid = -1;
            leave_group_behind(leader);   // anything it left running in its group
            fail("the server process " + describe_wait_status(status));
            return;
        }
        trim_stderr_log();
    }

    if (current_state == State::STARTING &&
        std::chrono::duration_cast<std::chrono::seconds>(now - started_at).count() >= cfg.startup_timeout_seconds) {
        fail("no answer after " + std::to_string(cfg.startup_timeout_seconds) + " s while starting up");
    }
}

void MCP_SERVER::trim_stderr_log()
{
    // The child keeps its stderr open in append mode, so truncating the file by name is safe: it just
    // carries on writing at the new end. Checked every 10 s so a chatty (not hostile - a hostile server can
    // write anywhere as the user anyway) server cannot fill the disk with logging.
    const auto now = Clock::now();
    if (now - last_log_trim < std::chrono::seconds(10)) return;
    last_log_trim = now;
    const fs::path log_path = root / (cfg.name + ".stderr.log");
    struct stat st{};
    if (stat(log_path.c_str(), &st) == 0 && st.st_size > MAX_STDERR_LOG_BYTES) {
        if (truncate(log_path.c_str(), 0) != 0) { /* try again next time */ }
    }
}

void MCP_SERVER::send_message(const json& message)
{
    if (in_fd < 0) return;
    out_buffer += message.dump(-1, ' ', false, json::error_handler_t::replace);
    out_buffer += '\n';
    if (out_buffer.size() > MAX_PENDING_OUTPUT_BYTES) {
        fail("the server is not reading what we send it");
        return;
    }
    on_writable();
}

void MCP_SERVER::on_writable()
{
    while (in_fd >= 0 && !out_buffer.empty()) {
        ssize_t n = write(in_fd, out_buffer.data(), out_buffer.size());
        if (n > 0) {
            out_buffer.erase(0, static_cast<size_t>(n));
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            fail("could not send to the server (it probably exited)");
            return;
        }
    }
}

void MCP_SERVER::send_request(Kind kind, const std::string& method, const json& params, int64_t* id_out)
{
    const int64_t id = next_id++;
    pending[id] = kind;
    if (id_out != nullptr) *id_out = id;
    send_message({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
}

void MCP_SERVER::send_notification(const std::string& method, const json& params)
{
    json message = {{"jsonrpc", "2.0"}, {"method", method}};
    if (params.is_object()) message["params"] = params;
    send_message(message);
}

void MCP_SERVER::on_readable()
{
    if (out_fd < 0) return;

    char buffer[65536];
    ssize_t n = read(out_fd, buffer, sizeof buffer);
    if (n == 0) {
        // The pipe closed: find out how it ended, if it already has.
        std::string how = "the server closed its output (it probably exited)";
        if (pid > 0) {
            int status = 0;
            if (waitpid(pid, &status, WNOHANG) == pid) {
                const pid_t leader = pid;
                pid = -1;
                leave_group_behind(leader);
                how = "the server process " + describe_wait_status(status);
            }
        }
        fail(how);
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) fail(std::string("could not read from the server: ") + std::strerror(errno));
        return;
    }

    read_buffer.append(buffer, static_cast<size_t>(n));

    if (discarding_oversized) {
        // Throwing away the rest of a message that was too long: everything up to and including the next newline.
        const size_t end_of_line = read_buffer.find('\n');
        if (end_of_line == std::string::npos) {
            discarded_bytes += read_buffer.size();
            read_buffer.clear();
            if (discarded_bytes > MAX_DISCARD_BYTES) fail("the server keeps sending one endless message");
            return;
        }
        read_buffer.erase(0, end_of_line + 1);
        discarding_oversized = false;
    }

    size_t newline;
    while (out_fd >= 0 && (newline = read_buffer.find('\n')) != std::string::npos) {
        std::string line = read_buffer.substr(0, newline);
        read_buffer.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) handle_line(line);   // may fail() the server, which closes out_fd and ends this loop
    }
    if (out_fd >= 0 && read_buffer.size() > MAX_LINE_BYTES) {
        // An honest big answer (a 5 MB file read comes back doubled and escaped) is not a reason to stop the server: drop it,
        // answer the outstanding calls with an error, and carry on once its line ends.
        log_line("[" + cfg.name + "] dropped an answer over 8 MB (its call gets an error); the server keeps running");
        // Which call the dropped answer belonged to is unknown (the line was never parsed) and guessing could cancel the wrong one,
        // so every call still waiting on this server is answered with an error that says so.
        fail_pending_calls_only("an answer from this server was too large to handle (over 8 MB) and was dropped, so this call was cancelled - try again");
        discarded_bytes = read_buffer.size();
        read_buffer.clear();
        discarding_oversized = true;
    }
}

void MCP_SERVER::handle_line(const std::string& line)
{
    if (json_too_deep(line, MAX_JSON_DEPTH)) {
        log_line("[" + cfg.name + "] ignored a message nested more than " + std::to_string(MAX_JSON_DEPTH) + " levels deep");
        return;
    }
    json message;
    try {
        message = json::parse(line);
    } catch (const std::exception&) {
        log_line("[" + cfg.name + "] ignored a line that is not JSON: " + clean_description(line, 80));
        return;
    }
    if (!message.is_object()) return;

    const bool has_method = message.contains("method") && message["method"].is_string();
    const bool has_id = message.contains("id");
    if (has_method && has_id) handle_server_request(message);
    else if (has_method) handle_notification(message);
    else if (has_id) handle_response(message);
}

void MCP_SERVER::handle_server_request(const json& message)
{
    // We declared no capabilities, so a well-behaved server asks for nothing - except ping, which is always allowed.
    const std::string method = message["method"].get<std::string>();
    if (!message["id"].is_number_integer() && !message["id"].is_string()) return;   // not a valid request id
    if (method == "ping") {
        send_message({{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", json::object()}});
    } else {
        send_message({{"jsonrpc", "2.0"}, {"id", message["id"]}, {"error", {{"code", -32601}, {"message", "Method not found"}}}});
    }
}

void MCP_SERVER::handle_notification(const json& message)
{
    // Progress, log messages, resource updates: nothing in olli to hand them to yet, so they are dropped.
    if (message["method"].get<std::string>() != "notifications/tools/list_changed") return;

    for (const auto& [id, kind] : pending) {
        if (kind == Kind::LIST) { relist_after_ready = true; return; }   // a listing is already under way
    }
    if (current_state == State::READY) request_tool_list("");
    else relist_after_ready = true;
}

void MCP_SERVER::request_tool_list(const std::string& cursor)
{
    if (cursor.empty()) {
        list_accumulator.clear();
        list_pages = 0;
    }
    json params = json::object();
    if (!cursor.empty()) params["cursor"] = cursor;
    send_request(Kind::LIST, "tools/list", params);
}

void MCP_SERVER::finish_tool_list()
{
    const size_t count = list_accumulator.size();
    set_tools(std::move(list_accumulator));
    list_accumulator.clear();
    if (current_state == State::STARTING) {
        ready_at = Clock::now();
        set_state(State::READY);
        log_line("[" + cfg.name + "] ready: " + std::to_string(count) + (count == 1 ? " tool" : " tools"));
    }
    if (relist_after_ready) {
        relist_after_ready = false;
        request_tool_list("");
    }
}

void MCP_SERVER::handle_response(const json& message)
{
    const json& id_value = message["id"];
    if (!id_value.is_number_integer()) return;
    const int64_t id = id_value.get<int64_t>();

    auto it = pending.find(id);
    if (it == pending.end()) return;     // cancelled or timed out earlier: its late answer is dropped
    const Kind kind = it->second;
    pending.erase(it);

    const bool is_error = message.contains("error");

    if (kind == Kind::CALL) {
        RESPONSE response;
        response.id = id;
        if (is_error) {
            response.ok = false;
            response.error = json_error_text(message);
        } else {
            response.ok = true;
            response.result = message.contains("result") ? message["result"] : json::object();
        }
        finished.push_back(std::move(response));
        return;
    }

    if (kind == Kind::INITIALIZE) {
        if (is_error) { fail("it refused to start up: " + json_error_text(message)); return; }
        const json& result = message.contains("result") ? message["result"] : json::object();
        std::string version = result.is_object() && result.contains("protocolVersion") && result["protocolVersion"].is_string()
            ? result["protocolVersion"].get<std::string>() : std::string();
        if (version.empty()) { fail("its answer to initialize had no protocol version"); return; }
        if (version != PROTOCOL_VERSION) log_line("[" + cfg.name + "] speaks protocol version " + clean_description(version, 20) + " (we asked for " + PROTOCOL_VERSION + ") - carrying on");
        send_notification("notifications/initialized");
        request_tool_list("");
        return;
    }

    // Kind::LIST
    if (is_error) { fail("tools/list failed: " + json_error_text(message)); return; }
    const json& result = message.contains("result") ? message["result"] : json::object();
    if (result.is_object() && result.contains("tools") && result["tools"].is_array()) {
        for (const auto& tool : result["tools"]) {
            // A tool without a text name cannot be offered (and would trip every later lookup of its name).
            if (tool.is_object() && tool.contains("name") && tool["name"].is_string() && list_accumulator.size() < MAX_TOOLS_PER_SERVER) {
                list_accumulator.push_back(tool);
            }
        }
    }
    std::string cursor = result.is_object() && result.contains("nextCursor") && result["nextCursor"].is_string()
        ? result["nextCursor"].get<std::string>() : std::string();
    if (!cursor.empty() && ++list_pages < MAX_LIST_PAGES) request_tool_list(cursor);
    else finish_tool_list();
}

int64_t MCP_SERVER::call_tool(const std::string& real_name, const json& arguments)
{
    if (current_state != State::READY || in_fd < 0) return -1;
    int64_t id = 0;
    send_request(Kind::CALL, "tools/call", {{"name", real_name}, {"arguments", arguments.is_object() ? arguments : json::object()}}, &id);
    return id;
}

void MCP_SERVER::cancel_call(int64_t id, const std::string& reason)
{
    auto it = pending.find(id);
    if (it == pending.end() || it->second != Kind::CALL) return;
    pending.erase(it);
    send_notification("notifications/cancelled", {{"requestId", id}, {"reason", reason}});
}

bool MCP_SERVER::poll_response(RESPONSE& out)
{
    if (finished.empty()) return false;
    out = std::move(finished.front());
    finished.pop_front();
    return true;
}
