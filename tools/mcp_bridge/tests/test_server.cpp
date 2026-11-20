#include "mcp_server.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/select.h>
#include <signal.h>
#include <unistd.h>
using json = nlohmann::json;
static std::string WORK = "/tmp/mcp_server_test_work";   // scratch folder; override with argv[1]
static void pump(MCP_SERVER& s, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        fd_set r, w; FD_ZERO(&r); FD_ZERO(&w); int mx = -1;
        if (s.read_fd() >= 0) { FD_SET(s.read_fd(), &r); mx = s.read_fd(); }
        if (s.write_fd() >= 0) { FD_SET(s.write_fd(), &w); mx = std::max(mx, s.write_fd()); }
        timeval tv{0, 100000};
        int n = select(mx + 1, &r, &w, nullptr, &tv);
        if (n > 0 && s.write_fd() >= 0 && FD_ISSET(s.write_fd(), &w)) s.on_writable();
        if (n > 0 && s.read_fd() >= 0 && FD_ISSET(s.read_fd(), &r)) s.on_readable();
        s.tick();
    }
}
static bool until(MCP_SERVER& s, MCP_SERVER::State st, int sec) {
    for (int i = 0; i < sec * 10; ++i) { pump(s, 100); if (s.state() == st) return true; }
    return false;
}
int main(int argc, char** argv) {
    if (argc > 1) WORK = argv[1];
    signal(SIGPIPE, SIG_IGN);
    auto log = [](const std::string& l) { std::cout << "  LOG " << l << "\n"; };
    int fails = 0;
    #define CHECK(c) do { if (!(c)) { std::cout << "FAIL line " << __LINE__ << ": " #c "\n"; ++fails; } } while (0)

    // 1. real server
    MCP_SERVER_CONFIG cfg; cfg.name = "everything"; cfg.command = {"npx", "-y", "@modelcontextprotocol/server-everything@2.3.1"}; cfg.allow_tools = {"*"};
    MCP_SERVER s(cfg, WORK, log);
    s.start();
    CHECK(until(s, MCP_SERVER::State::READY, 90));
    std::cout << "state: " << s.state_text() << "\n";
    CHECK(s.tools().size() == 16);
    CHECK(s.take_tools_changed()); CHECK(!s.take_tools_changed());
    int64_t id = s.call_tool("echo", {{"message", "hi"}});
    pump(s, 800);
    MCP_SERVER::RESPONSE r; CHECK(s.poll_response(r) && r.ok && r.id == id);
    std::cout << "echo -> " << flatten_call_result(r.result, 1000).text << "\n";
    // slow call, with a second quick call while it is pending
    int64_t slow = s.call_tool("trigger-long-running-operation", {{"duration", 3}, {"steps", 2}});
    int64_t quick = s.call_tool("get-sum", {{"a", 40}, {"b", 2}});
    pump(s, 500);
    int got = 0; while (s.poll_response(r)) { ++got; std::cout << "early: id " << r.id << (r.id == quick ? " (quick)" : "") << " -> " << flatten_call_result(r.result, 200).text << "\n"; }
    CHECK(got == 1);
    pump(s, 4000);
    CHECK(s.poll_response(r) && r.id == slow && r.ok);
    // unknown tool -> error response
    s.call_tool("nosuch", json::object()); pump(s, 800);
    CHECK(s.poll_response(r) && !r.ok); std::cout << "unknown tool -> error: " << r.error << "\n";
    // cancel
    int64_t c = s.call_tool("trigger-long-running-operation", {{"duration", 5}, {"steps", 1}});
    pump(s, 300); s.cancel_call(c, "test"); pump(s, 6000);
    CHECK(!s.poll_response(r));   // cancelled: no answer surfaces
    // 2. kill the child mid-session -> in-flight call fails, server restarts
    int64_t doomed = s.call_tool("trigger-long-running-operation", {{"duration", 20}, {"steps", 1}});
    pump(s, 300);
    std::string cmd = "pkill -KILL -f 'server-everything' ; true";
    if (system(cmd.c_str()) != 0) {}
    pump(s, 1500);
    CHECK(s.state() == MCP_SERVER::State::BACKOFF);
    CHECK(s.poll_response(r) && r.id == doomed && !r.ok); std::cout << "in-flight at kill -> " << r.error << "\n";
    std::cout << "state: " << s.state_text() << "\n";
    CHECK(until(s, MCP_SERVER::State::READY, 60));
    std::cout << "after restart: " << s.state_text() << "\n";
    CHECK(s.call_tool("echo", {{"message", "again"}}) > 0); pump(s, 800); CHECK(s.poll_response(r) && r.ok);
    // 3. stop
    s.stop(); CHECK(s.state() == MCP_SERVER::State::STOPPED && s.tools().empty());
    s.take_tools_changed();
    // 4. bad command -> FAILED, permanent
    MCP_SERVER_CONFIG bad; bad.name = "bad"; bad.command = {"/no/such/binary"};
    MCP_SERVER b(bad, WORK, log);
    b.start(); CHECK(b.state() == MCP_SERVER::State::FAILED); std::cout << "bad: " << b.state_text() << "\n";
    CHECK(b.call_tool("x", json::object()) == -1);
    // 5. process that starts but speaks nonsense / exits at once
    MCP_SERVER_CONFIG junk; junk.name = "junk"; junk.command = {"sh", "-c", "echo not json; echo '{\"id\":1,\"result\":{\"protocolVersion\":\"2025-06-18\"}}'; sleep 1; exit 3"}; junk.startup_timeout_seconds = 5;
    MCP_SERVER j(junk, WORK, log);
    j.start(); pump(j, 3000); std::cout << "junk: " << j.state_text() << "\n";
    CHECK(j.state() == MCP_SERVER::State::BACKOFF || j.state() == MCP_SERVER::State::STARTING);   // exited with code 3, restarting after its delay
    // 6. startup timeout: a server that never answers
    MCP_SERVER_CONFIG mute; mute.name = "mute"; mute.command = {"sleep", "30"}; mute.startup_timeout_seconds = 5;
    MCP_SERVER m(mute, WORK, log);
    m.start(); pump(m, 6500); std::cout << "mute: " << m.state_text() << "\n"; CHECK(m.state() == MCP_SERVER::State::BACKOFF);
    // 7. the environment is small, and fds are closed
    MCP_SERVER_CONFIG envc; envc.name = "envtest"; envc.command = {"sh", "-c", "env > " + WORK + "/env_seen.txt; ls /proc/self/fd >> " + WORK + "/env_seen.txt; sleep 1"};
    envc.env = {{"MY_KEY", "yes"}};
    setenv("SECRET_API_KEY", "leak-me", 1);
    MCP_SERVER e(envc, WORK, log);
    e.start(); pump(e, 1500);
    {
        std::ifstream seen(WORK + "/env_seen.txt"); std::stringstream all; all << seen.rdbuf(); std::string text = all.str();
        CHECK(text.find("MY_KEY=yes") != std::string::npos);                 // the config's own env entry arrives
        CHECK(text.find("SECRET_API_KEY") == std::string::npos);             // the bridge's other environment does not
        CHECK(text.find("leak-me") == std::string::npos);
        CHECK(text.find("PATH=") != std::string::npos && text.find("HOME=") != std::string::npos);
        // only 0,1,2 (+ the directory listing's own descriptor) are open in the child
        size_t at = text.find("\n0\n"); CHECK(at != std::string::npos);
        int open_fds = 0; std::stringstream lines(text.substr(at + 1)); std::string l; while (std::getline(lines, l)) if (!l.empty() && std::isdigit(static_cast<unsigned char>(l[0]))) ++open_fds;
        CHECK(open_fds <= 4);
    }
    std::cout << (fails ? "FAILED " : "all passed ") << fails << "\n";
    return fails ? 1 : 0;
}
