// SUBCON_TOOL_SERVER - the server half of the subcon tool: a small listener
// on SUBCON_TOOL_PORT (47603) that speaks the same protocol as olli's own
// remote-tool listener (../PROTOCOL.md, source/remote_tools.h/.cpp), so any
// other tool (clock, hue, presence, rag) can hold a second OLLI_LINK to it
// and register the same tools it registers with olli. Subcon then calls
// those tools itself during its think cycles.
//
// Re-implements the SERVER side of the protocol compactly rather than
// sharing source/remote_tools.*: tools/ never shares headers with source/
// (PROTOCOL.md's "Repo / build layout"), and olli's version is welded to its
// one-listener-one-pending-connection / TOOL_BASE shape, where this needs
// many simultaneous connections.
//
// Single-threaded and non-blocking: everything here runs on the caller's
// (main loop's) thread, driven by one select() the caller owns - add_fds()
// contributes this server's fds, service() acts on what select() reported.
// No method ever waits on the network. Slow work (model requests, web
// fetches) lives on job_worker's thread precisely so this stays responsive.
//
// Per connection: register (required first message), identity (sent to the
// tool once we know who the user is), ping/pong heartbeat with a dead
// timeout, one call in flight at a time (the others queue - the same
// one-call-then-wait shape olli's own TOOL_REMOTE uses, which every tool
// built on OLLI_LINK is written for), and unsolicited events.
//
// A call is correlated by id and ALWAYS completes its callback exactly once
// unless abandon()ed first: with the tool's result, with an error result if
// the tool reports one, with a timeout error if it doesn't answer in time,
// or with a disconnect error if its connection dies with the call pending.
// An abandoned call is removed entirely - a late result for it is ignored -
// so nothing leaks however a cycle ends.

#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <deque>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <sys/select.h>

struct SERVER_CALL_RESULT {
    std::string text;       // the tool's result, or an "Error: ..." description
    bool ok = true;         // false: the tool reported an error, timed out, or went away
};
using SERVER_CALL_CALLBACK = std::function<void(const SERVER_CALL_RESULT&)>;

// Things service() noticed that the caller may want to react to, in order.
struct SERVER_NOTICE {
    enum Kind { REGISTERED, DISCONNECTED, EVENT, PROBLEM } kind = EVENT;
    std::string peer;                    // "tool #3 (get_clock_time, ...)" - for log lines
    std::vector<std::string> tool_names; // REGISTERED / DISCONNECTED: what that connection offered

    // PROBLEM: a connection sent something this server couldn't handle (a
    // field of the wrong type, say) and was dropped; 'message' says why.
    // 'peer' and 'message' only.

    // EVENT: the fields of an unsolicited {"type": "event"} message that
    // matter to subcon. 'action' is ignored by subcon (it is olli's to
    // execute) and so isn't carried.
    std::string message;
    std::string origin_id;
    std::string var_name;
    std::string var_value;
};

class SUBCON_TOOL_SERVER {
    public:
        // Binds 127.0.0.1:port (loopback only, same as olli's listener).
        // call_timeout_seconds: how long a dispatched call may go
        // unanswered before it completes with a timeout error.
        SUBCON_TOOL_SERVER(int port, int call_timeout_seconds);
        ~SUBCON_TOOL_SERVER();

        SUBCON_TOOL_SERVER(const SUBCON_TOOL_SERVER&) = delete;
        SUBCON_TOOL_SERVER& operator=(const SUBCON_TOOL_SERVER&) = delete;

        // How long a dispatched call may go unanswered (applies to calls
        // dispatched from now on).
        void set_call_timeout_seconds(int seconds) { call_timeout_seconds = seconds; }

        // True if bind()/listen() failed (most likely another subcon already
        // owns the port). Unlike olli's listener this is fatal for the tool -
        // main() exits - since serving tools is half of what it is.
        bool bind_failed() const { return listen_fd < 0; }

        // Adds this server's fds to the caller's select() sets and raises
        // max_fd. Connections with unsent bytes also go in write_fds.
        void add_fds(fd_set& read_fds, fd_set& write_fds, int& max_fd) const;

        // Call once per main-loop tick after select(): accepts, reads,
        // flushes, heartbeats, times out calls, and completes callbacks.
        // Callbacks run here, on the caller's thread, after the server's own
        // bookkeeping for the tick is finished (so a callback may safely
        // call call() or abandon()).
        void service(const fd_set& read_fds, const fd_set& write_fds);

        std::vector<SERVER_NOTICE> take_notices();

        // Whether some live, registered connection offers this tool name.
        bool has_tool(const std::string& name) const;

        // Every registered tool from every live connection, as plain
        // {name, description, parameters} entries, first connection winning a
        // duplicated name. The caller wraps/filters them for the model.
        nlohmann::json tool_defs() const;

        // Dispatches a call to whichever connection offers 'name'. Returns
        // the new call id, or an empty string (callback never invoked) if no
        // live connection offers it - the caller turns that into its own
        // error result.
        std::string call(const std::string& name, const nlohmann::json& arguments, SERVER_CALL_CALLBACK callback);

        // Cancels a call whose answer is no longer wanted (a cycle was
        // abandoned): its callback will not run, and a late result is dropped.
        void abandon(const std::string& call_id);

        // Who is running olli, as received on our own olli link (or pinned
        // on the command line). Sent to every registered connection now and
        // to each one that registers later - exactly what olli itself does,
        // so a tool's second link picks the same profile as its first.
        void set_identity(const nlohmann::json& identity_message);

        size_t connection_count() const;
        // One short line per registered connection, for the status display.
        std::vector<std::string> connection_summaries() const;

    private:
        using clock = std::chrono::steady_clock;

        struct PENDING_CALL {
            std::string call_id;
            std::string name;
            nlohmann::json arguments;
            SERVER_CALL_CALLBACK callback;
            clock::time_point sent_at{};
        };

        struct CONNECTION {
            int fd = -1;
            unsigned id = 0;
            std::string in;
            std::string out;
            bool registered = false;
            nlohmann::json tool_defs = nlohmann::json::array();
            std::set<std::string> names;
            clock::time_point accepted_at;
            clock::time_point last_sent;
            clock::time_point last_received;
            std::deque<PENDING_CALL> queued;
            bool has_inflight = false;
            PENDING_CALL inflight;
            bool dead = false;
        };

        void accept_new();
        void read_from(CONNECTION& conn);
        void handle_line(CONNECTION& conn, const std::string& line);
        void handle_message(CONNECTION& conn, const std::string& line); // the body of handle_line(), which only adds the catch
        void handle_register(CONNECTION& conn, const nlohmann::json& msg);
        void send_line(CONNECTION& conn, const std::string& line);
        void flush(CONNECTION& conn);
        void pump_calls(CONNECTION& conn);
        void finish(PENDING_CALL& call, SERVER_CALL_RESULT result);
        std::string describe(const CONNECTION& conn) const;

        int listen_fd = -1;
        int call_timeout_seconds;
        unsigned next_connection_id = 1;
        unsigned next_call_number = 1;
        std::vector<CONNECTION> connections;
        std::vector<SERVER_NOTICE> notices;
        nlohmann::json identity; // null until known

        // Completions gathered during service() and fired at its very end -
        // see service()'s comment.
        std::vector<std::pair<SERVER_CALL_CALLBACK, SERVER_CALL_RESULT>> completions;
};
