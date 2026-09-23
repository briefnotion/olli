#ifndef remote_tools_h
#define remote_tools_h

#include <string>
#include <optional>
#include <chrono>
#include <vector>

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

// What TOOL_REMOTE::poll_communications() (new, alongside the existing
// check()/monitor_tool() - see its own comment) queues up as a call's
// result lands, for TOOL_WORKER_CLASS::get_pending_result() (tool_worker.h)
// to hand back to main. Mirrors tools/PROTOCOL.md's `result` message and
// check()'s own response_str/special_instruction locals - a wire-level
// "error" is folded into 'response' as plain text, same as check() already
// does, so there's no separate error field to carry here either.
struct TOOL_RESULT
{
    std::string call_id;
    std::string response;
    std::string special_instruction;
};

// What poll_communications() queues up for an unsolicited `event` message
// (tools/PROTOCOL.md) - separate from TOOL_RESULT since an event isn't a
// response to any call_id, unlike a result. Mirrors the old monitor_tool()'s
// own event handling (remote_tools.cpp, untouched): 'message' is the
// narration text (main's job to log/integrate_tool_result(), same as
// monitor_tool() already does); 'action_tool'/'action_arguments' are the
// optional structured follow-up (main's job to push onto its own
// chat.pending_tool_calls, same as monitor_tool() already does) - empty
// action_tool means no action, same convention REMOTE_TOOL_REGISTRATION's
// own 'tools' field above uses (raw, not yet a ToolCall - this header can't
// see ToolCall's full definition without an olla.h include that would
// circle back here through system.h, and there's nothing here that needs
// to correlate a result back to it the way a real dispatched call's id
// would, so there's nothing lost by leaving it raw for whoever actually
// dispatches it to turn into a real ToolCall).
//
// 'origin_id' (2026-09-22) - which call originally set up whatever just
// produced this event (e.g. the call_id of the set_timer call a timer
// expiry traces back to), or EVENT_NO_ORIGIN_ID below for an event with no
// such call at all (a purely ambient one, like presence). Lets whichever
// ollama_system instance drains this (olla.cpp PART 5) tell its own
// dispatched work apart from someone else's/nothing's, instead of
// narrating every event as if it just answered whatever's currently being
// discussed - it's still not a call_id in TOOL_RESULT's sense (nothing
// here is "the response to" this id), just a birth certificate.
struct TOOL_EVENT
{
    std::string message;
    std::string action_tool;
    json action_arguments;
    std::string origin_id;
};

// Matches tools/olli_link/olli_link.hpp's own EVENT_NO_ORIGIN_ID exactly -
// two independent copies (this side and every remote tool's own binary
// don't share a header, see tools/PROTOCOL.md's "Repo / build layout"
// section), kept in sync by convention/documentation, not by compiling
// against one shared definition.
inline const std::string EVENT_NO_ORIGIN_ID = "no_origin_call";

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

        // True if the constructor's bind()/listen() didn't succeed - most
        // likely another olli process (a different profile, or the same
        // profile run twice) already has PORT, since every olli instance
        // on a machine currently shares this one fixed port with no way to
        // tell them apart. Not fatal - poll() just always returns
        // std::nullopt, so this instance runs fine on its own built-in
        // tools alone - but the caller should say so loudly (see
        // tool_worker.cpp's own use of this), not leave it as silent as
        // it used to be.
        bool bind_failed() const { return listen_fd < 0; }
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

        // poll_communications()-only state (see its own comment below) -
        // the call_id it's currently waiting on a result for, if any. Where
        // check() kept this on its own call stack across a blocking wait,
        // poll_communications() never blocks, so this has to survive
        // between ticks instead.
        std::optional<std::string> awaiting_call_id;

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
        bool check(IO_WORKER_CLASS& io_worker, ollama_system& chat, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const ToolCall& tc) override;
        void monitor_tool(ollama_system& chat, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms) override;

        // New, alongside check()/monitor_tool() above - neither is touched.
        // Not part of TOOL_BASE's virtual interface, so callers need this
        // tool's concrete type - tool_worker.cpp's own tools_list is already
        // typed as TOOL_REMOTE specifically (never holds a built-in), so it
        // calls this directly with no cast needed. A deliberate, additive
        // prototype rather than a change to the existing polymorphic path
        // every tool still goes through today.
        //
        // Single non-blocking pass, no chat/comms needed at all (unlike
        // both methods above): (1) service ping/pong; if a result arrives
        // for whichever call_id we're currently awaiting, package it as a
        // TOOL_RESULT into pending_results and clear awaiting_call_id; if
        // an event arrives, package it as a TOOL_EVENT into pending_events
        // - unlike a result, an event needs no awaiting_call_id match,
        // since it isn't a response to anything; (2) if not already
        // awaiting a result, send the oldest queued call in pending_calls
        // addressed to this tool (matched against tool_defs, same as
        // check()'s own is_mine check) and start awaiting its result.
        // Never waits on the socket - check()'s own blocking
        // read_line_blocking() loop is what this is meant to eventually
        // replace.
        void poll_communications(std::vector<ToolCall>& pending_calls, std::vector<TOOL_RESULT>& pending_results, std::vector<TOOL_EVENT>& pending_events);

        // Read-only access to what this tool declared at registration - the
        // same array register_tool() (above) re-emits into Ollama's own
        // tools schema, for tool_worker.cpp to fold into its own
        // registered_tool_defs digest instead (TOOL_WORKER_CLASS,
        // tool_worker.h) now that registration doesn't route through
        // register_tool()'s chat/tools-array path for tools that join here.
        const json& get_tool_defs() const { return tool_defs; }

        // False once the connection's closed (monitor_tool() or check()
        // noticing a dead fd resets it to -1) - ollama_system::process()
        // checks this every tick to drop this instance from tools_list.
        bool is_alive() const override { return fd >= 0; }
};

#endif
