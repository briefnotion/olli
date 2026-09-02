#ifndef remote_tools_h
#define remote_tools_h

#include <string>
#include <optional>
#include <chrono>

#include <nlohmann/json.hpp>

#include "tools.h"

using json = nlohmann::json;

// What one completed "register" handshake hands back (see tools/PROTOCOL.md)
// - fd ownership passes to the caller here, REMOTE_TOOL_LISTENER stops
// tracking it and goes back to accept()ing the next connection.
struct REMOTE_TOOL_REGISTRATION
{
    int fd = -1;
    json tools;   // the "tools" array from the register message, unparsed further
};

/**
 * REMOTE_TOOL_LISTENER
 *
 * Step 1-3 of the remote-tools plan (see tools/PROTOCOL.md). Owns a
 * non-blocking TCP listening socket on port PORT, accepts at most one
 * pending connection at a time, and reads newline-delimited JSON from it.
 * Its only job is the initial registration handshake: once a connection
 * sends a well-formed "register" line, poll() hands the fd and its declared
 * tools off to the caller (main.cpp constructs a TOOL_REMOTE from it and
 * registers that with chat - see ollama_system::register_remote_tool) and
 * this class forgets about that connection entirely, free to accept the
 * next one. Anything that isn't a well-formed register message on a fresh
 * connection is silently dropped - register is always the first message per
 * the protocol, so there's nothing else a first line could legitimately be.
 */
class REMOTE_TOOL_LISTENER
{
    private:
        int listen_fd = -1;
        int client_fd = -1;
        std::string read_buffer;

    public:
        static constexpr int PORT = 47601;

        // Binds and listens on 127.0.0.1:PORT. Failure (port in use, etc.)
        // leaves listen_fd at -1 - poll() then just always returns
        // std::nullopt rather than crashing the whole program over an
        // optional feature.
        REMOTE_TOOL_LISTENER();
        ~REMOTE_TOOL_LISTENER();

        // Call once per main-loop tick (non-blocking - never waits).
        std::optional<REMOTE_TOOL_REGISTRATION> poll();
};

/**
 * TOOL_REMOTE
 *
 * A TOOL_BASE proxy for one connected remote-tool process (see
 * tools/PROTOCOL.md) - one instance per connection, constructed from a
 * REMOTE_TOOL_REGISTRATION once REMOTE_TOOL_LISTENER's handshake completes.
 * Owns the connection's socket for its whole lifetime.
 *
 * register_tool() just re-emits whatever the remote program declared at
 * registration - this class never has any tool-specific knowledge, which is
 * the whole point (see the TOOL_BASE comment in tools.h): a future, wildly
 * different remote program registers the same way and needs no changes
 * here.
 *
 * check() forwards a matched call over the socket and blocks (with a
 * timeout) for the matching result - a real network round trip sitting in
 * the synchronous tool-dispatch path, same as TOOL_WEB_SEARCH's libcurl
 * calls already do with their own timeout.
 *
 * Heartbeat (see tools/PROTOCOL.md): both sides track time since anything
 * was last sent/received (a call/result/event/ping/pong all count).
 * monitor_tool() sends a ping if PING_INTERVAL_SECONDS have passed with
 * nothing sent, and mark_dead()s the connection if DEAD_TIMEOUT_SECONDS
 * have passed with nothing received - catches a hung (not crashed) peer
 * during an otherwise idle stretch, which a clean-close event never would.
 *
 * Not permission-gated - anything that completes the handshake on the
 * loopback-only socket is trusted and registered.
 */
class TOOL_REMOTE : public TOOL_BASE
{
    private:
        int fd = -1;
        json tool_defs;

        // Bytes read from fd but not yet consumed as a complete line -
        // persists across calls (rather than a fresh local buffer each
        // time) so a line that arrives split across reads, or extra bytes
        // beyond the one line being waited for, aren't silently dropped.
        std::string read_buffer;

        // Heartbeat bookkeeping - see the class comment above and
        // tools/PROTOCOL.md. Both set to "now" at construction (a fresh
        // connection shouldn't look idle from the moment it registers).
        std::chrono::steady_clock::time_point last_sent = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point last_received = std::chrono::steady_clock::now();
        static constexpr int PING_INTERVAL_SECONDS = 5;
        static constexpr int DEAD_TIMEOUT_SECONDS = 15;

        // Blocks up to timeout_ms waiting for one complete newline-
        // delimited line on fd (checking read_buffer for one already
        // waiting before touching the socket at all). Returns false on
        // timeout, a closed connection, or a real error - out is untouched
        // in that case.
        bool read_line_blocking(std::string& out, int timeout_ms);

        // Closes fd (if not already closed) and resets it to -1, so
        // is_alive() reports false and every other method's existing
        // "fd < 0" handling takes over. Called from wherever a connection
        // problem is actually detected - a failed write, a call that timed
        // out, or the peer closing normally - rather than each of those
        // duplicating the same close()+reset.
        void mark_dead();

    public:
        TOOL_REMOTE(int socket_fd, json tools_array);
        ~TOOL_REMOTE() override;

        TOOL_REMOTE(const TOOL_REMOTE&) = delete;
        TOOL_REMOTE& operator=(const TOOL_REMOTE&) = delete;

        // One-way "identity" message (see tools/PROTOCOL.md) - who's
        // running olli right now. Sent once, right after registration
        // completes (see the call site in main.cpp), not part of the
        // request/response call path, so there's no result to wait for.
        // Takes plain strings rather than USER_IDENTITY/CLASS_SYSTEM
        // directly (system.h) - this class doesn't otherwise need to know
        // either type exists, matching the "JSON in, plain string(s) out"
        // convention the rest of the tool-call path already uses. A failed
        // write just marks the connection dead, same as any other.
        void send_identity(const std::string& name, const std::string& full_name, const std::string& about);

        void configure(ollama_system& chat) override;
        void register_tool(ollama_system& chat, json& tools) override;
        bool check(IO_WORKER_CLASS& io_worker, ollama_system& chat, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, const ToolCall& tc) override;
        void monitor_tool(ollama_system& chat, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms) override;

        // False once the connection's closed (monitor_tool() or check()
        // noticing a dead fd resets it to -1) - ollama_system::process()
        // checks this every tick to drop this instance from tools_list.
        bool is_alive() const override { return fd >= 0; }
};

#endif
