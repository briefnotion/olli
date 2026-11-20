// MCP_SERVER - one MCP server run as a child process and spoken to over its
// stdin/stdout (the MCP "stdio" transport: one JSON-RPC 2.0 message per line).
//
// Written to live inside mcp_bridge's single select() loop, like OLLI_LINK is:
// it owns no thread and never blocks on the child. The caller puts read_fd()
// (and write_fd(), when it is not -1) in its select() sets, calls
// on_readable()/on_writable() when they fire, and calls tick() every pass.
// Everything the server says comes back as plain data - poll_response() for a
// finished tools/call, take_tools_changed()/take_state_changed() flags - so
// this class never touches olli, the links, or the bridge's tool list.
//
// Handshake, started by start(): initialize -> (server answers) ->
// notifications/initialized -> tools/list (following nextCursor) -> READY.
// A server that dies is restarted with a growing delay; one that cannot even
// be started (command not found) or keeps failing before it is ready is
// parked as FAILED until the bridge is told to start it again.
//
// Safety, since this runs somebody else's program as the user:
//   - the command is run directly (execvp), never through a shell
//   - the child gets a SMALL environment (PATH, HOME, locale, proxy/cert
//     settings, XDG dirs, plus whatever the config's "env" adds) - not the
//     bridge's own environment, which may hold other programs' keys
//   - it is its own process group, so stop() takes down everything it
//     started (npx -> node), and PR_SET_PDEATHSIG ends it if the bridge dies
//   - every other file descriptor is closed in the child (olli's sockets, the
//     single-instance lock)
//   - its stderr goes to <work_root>/<name>.stderr.log, never to the screen
//   - a server that sends a line over 8 MB, or stops reading its input, is
//     treated as failed

#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <sys/types.h>

#include "mcp_pure.hpp"

class MCP_SERVER
{
    public:
        enum class State { STOPPED, STARTING, READY, BACKOFF, FAILED };

        // A finished tools/call. ok == false: 'error' says why (a JSON-RPC error,
        // or the server stopping before it answered). ok == true: 'result' is the
        // MCP result object (content / isError / structuredContent) for flatten_call_result().
        struct RESPONSE
        {
            int64_t id = 0;
            bool ok = false;
            nlohmann::json result;
            std::string error;
        };

        // work_root: a folder this server may keep its stderr log and (unless the config
        // names a cwd) its working directory in. log: one line of bridge log output.
        MCP_SERVER(MCP_SERVER_CONFIG config, std::filesystem::path work_root, std::function<void(const std::string&)> log);
        ~MCP_SERVER();

        MCP_SERVER(const MCP_SERVER&) = delete;
        MCP_SERVER& operator=(const MCP_SERVER&) = delete;

        const MCP_SERVER_CONFIG& config() const { return cfg; }
        State state() const { return current_state; }
        std::string state_text() const;        // "ready, 16 tools", "restarting in 4 s (...)", ...
        const std::string& last_error() const { return last_error_text; }
        const std::vector<nlohmann::json>& tools() const { return tool_list; }
        // Goes up every time the tool list changes - lets the caller skip work while nothing changed.
        uint64_t tools_generation() const { return tools_generation_count; }

        // Begin (or restart) the child now. No effect while it is already starting/ready.
        void start();
        // End the child (and everything it started) and drop its tool list. State becomes STOPPED.
        void stop();
        // Leave no child behind: stop(), then wait (a few seconds at most) for the processes to be gone.
        void shutdown_blocking();

        // For the caller's select(): -1 when there is nothing to watch.
        int read_fd() const { return out_fd; }
        int write_fd() const { return out_buffer.empty() ? -1 : in_fd; }
        void on_readable();
        void on_writable();
        // Deadlines, restart delay, reaping finished children. Call every loop pass.
        void tick();

        // tools/call. Returns the request id, or -1 if the server is not READY.
        int64_t call_tool(const std::string& real_name, const nlohmann::json& arguments);
        // Stop waiting for a call (tells the server too). Its late answer, if any, is dropped.
        void cancel_call(int64_t id, const std::string& reason);
        bool poll_response(RESPONSE& out);

        // True once each time the tool list (or whether it exists) changed / the state changed.
        bool take_tools_changed();
        bool take_state_changed();

    private:
        using Clock = std::chrono::steady_clock;
        enum class Kind { INITIALIZE, LIST, CALL };

        void begin();       // spawn the child and send initialize (state -> STARTING)
        bool spawn();
        void send_message(const nlohmann::json& message);
        void send_request(Kind kind, const std::string& method, const nlohmann::json& params, int64_t* id_out = nullptr);
        void send_notification(const std::string& method, const nlohmann::json& params = nullptr);
        void handle_line(const std::string& line);
        void handle_response(const nlohmann::json& message);
        void handle_server_request(const nlohmann::json& message);
        void handle_notification(const nlohmann::json& message);
        void request_tool_list(const std::string& cursor);
        void finish_tool_list();
        // The child is gone or unusable: stop it, answer every outstanding call with an error, and
        // either schedule a restart or (permanent == true / too many failures) park as FAILED.
        void fail(const std::string& why, bool permanent = false);
        void kill_child();
        void reap();
        void leave_group_behind(pid_t leader);   // the leader was reaped: keep watching its group
        void trim_stderr_log();
        void set_state(State next);
        void set_tools(std::vector<nlohmann::json> new_tools);
        void fail_all_calls(const std::string& why);
        void fail_pending_calls_only(const std::string& why);   // answers outstanding tools/call requests, leaves handshake requests alone

        MCP_SERVER_CONFIG cfg;
        std::filesystem::path root;
        std::function<void(const std::string&)> log_line;

        State current_state = State::STOPPED;
        std::string last_error_text;

        pid_t pid = -1;
        int in_fd = -1;     // we write the child's stdin
        int out_fd = -1;    // we read the child's stdout
        std::string read_buffer;
        std::string out_buffer;
        // An answer longer than MAX_LINE_BYTES is dropped (its calls get an error) and the rest of its line is thrown away as it
        // arrives; the server itself keeps running. Only an endless message ends the server (MAX_DISCARD_BYTES).
        bool discarding_oversized = false;
        size_t discarded_bytes = 0;

        int64_t next_id = 1;
        std::map<int64_t, Kind> pending;
        std::deque<RESPONSE> finished;

        std::vector<nlohmann::json> list_accumulator;
        std::vector<nlohmann::json> tool_list;
        bool relist_after_ready = false;
        int list_pages = 0;

        Clock::time_point started_at{};
        Clock::time_point ready_at{};
        Clock::time_point restart_at{};
        int consecutive_failures = 0;

        // A process group on its way out. 'leader_reaped': the main process is gone but others it started
        // may still be running in the group, so it stays listed (and gets SIGKILL after the grace period)
        // until the group is empty.
        struct DYING { pid_t pid; Clock::time_point kill_at; bool killed; bool leader_reaped; };
        std::vector<DYING> dying;

        bool tools_changed_flag = false;
        uint64_t tools_generation_count = 0;
        bool spawn_failure_is_permanent = false;   // exec failed (bad command): trying again would fail the same way
        Clock::time_point last_log_trim{};
        bool state_changed_flag = false;
};
