// OLLI_LINK - the "talk to olli" plumbing every remote tool needs: connect/
// reconnect over TCP, newline-delimited JSON framing, heartbeat (ping/pong),
// dead-connection detection, and the register/call/result/event message
// shapes. See ../PROTOCOL.md for the wire protocol this implements.
//
// Deliberately tool-agnostic - knows nothing about what this tool registers
// or how it answers a call. See template_tool.cpp's olli_processing() for
// where that tailored logic lives and how it drives this class.
//
// One OLLI_LINK per server a tool talks to, constructed once in main() - a
// tool is a client of olli's listener (port 47601) and, if it wants to be
// callable by the subcon tool too, of subcon's listener (port 47603) via a
// second OLLI_LINK built from the same register message (see the
// constructor below, and ../PROTOCOL.md's "Two servers" section). Each
// link owns its own socket fd but not the wait for activity on it: main()
// still runs the single select() call across stdin + every link's socket
// (via fd() below) exactly as before this split - only the connect/read/
// heartbeat logic that used to sit inline in main()'s loop moved in here.

#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <deque>
#include <string>

#include <netinet/in.h>

// One piece of exact data to attach to a result, alongside whatever gets
// narrated - see ../PROTOCOL.md's `result` message shape. Mirrors
// source/comms.h's TOOL_ATTACHMENT wire-shape (type/label/content), but is
// its own, separate type - tools/ deliberately doesn't share headers with
// source/ (see ../PROTOCOL.md's "Repo / build layout" section), only the
// JSON shape that crosses the wire needs to agree between them. type
// empty means "no attachment" - send_result() below only adds this to the
// wire message when it's set to something.
struct OLLI_ATTACHMENT {
    std::string type;
    std::string label;
    std::string content;
};

// Reserved origin_id (see send_event() below and ../PROTOCOL.md's `event`
// message shape) for an event with no originating call at all - a purely
// ambient one (presence's arrival/departure), as opposed to one that
// traces back to something this tool was asked to do (clock's timers).
// Deliberately a plain word, not opaque/generated the way a real
// ToolCall::id (source/olla.h) always is, so there's no ambiguity by
// construction. Same independent-copy convention as OLLI_ATTACHMENT above
// - source/remote_tools.h keeps its own matching copy, not a shared
// header, per ../PROTOCOL.md's "Repo / build layout" section.
inline const std::string EVENT_NO_ORIGIN_ID = "no_origin_call";

// The two servers a tool can hold a link to (one OLLI_LINK per server -
// see the OLLI_LINK constructor below): olli's own remote-tool listener, and
// the subcon tool's listener (tools/subcon/), which speaks the identical
// protocol so a tool talks to it exactly as it talks to olli.
inline constexpr int OLLI_TOOL_PORT = 47601;
inline constexpr int SUBCON_TOOL_PORT = 47603;

class OLLI_LINK {
    public:
        // host_display is what shows up in status text ("...at 127.0.0.1");
        // host_addr is the already-resolved address try_connect() dials -
        // main() does its own inet_pton() (see its [host] argument
        // handling) so an invalid address is caught with a clean usage
        // message before anything here is constructed.
        //
        // port: which server this link dials. Defaults to olli's, so every
        // tool written before a tool could hold more than one link keeps
        // compiling and behaving exactly as it did. A tool that also wants
        // to be reachable by subcon constructs a second OLLI_LINK on
        // SUBCON_TOOL_PORT with the same register message - each link
        // keeps its own socket, queue, heartbeat and single-instance lock.
        // Results and errors must go back out on the link the call arrived
        // on; send_event() goes only to this link's server, so an event
        // meant for several servers is sent on each link in turn.
        OLLI_LINK(std::string host_display, in_addr host_addr, nlohmann::json register_message, int port = OLLI_TOOL_PORT);
        ~OLLI_LINK();

        OLLI_LINK(const OLLI_LINK&) = delete;
        OLLI_LINK& operator=(const OLLI_LINK&) = delete;

        // Which server this link dials, for status text and the tool's own
        // routing decisions ("olli" or "subcon").
        const char* server_name() const { return port == SUBCON_TOOL_PORT ? "subcon" : "olli"; }

        // Current socket fd, or -1 if not connected right now - for the
        // caller's own select() FD_SET. Never blocks, never does I/O.
        int fd() const { return sock_fd; }
        bool is_connected() const { return sock_fd >= 0; }

        // Call once per main-loop tick, right after select() returns.
        // socket_readable: whether fd() was in select()'s ready set this
        // tick (ignored if fd() is -1). Handles, in order every tick:
        // reconnect-on-interval if not connected; draining and parsing
        // whatever's available if socket_readable (a "ping" is answered
        // with "pong" internally - the caller never sees either); then the
        // heartbeat send/dead-timeout check.
        void service(bool socket_readable);

        // Pops one queued application message ("call"/"identity" today -
        // anything olli sends that isn't handled internally). Returns
        // false once the queue is empty.
        bool next_message(nlohmann::json& out);

        // True exactly once, on the tick a fresh disconnect/timeout is
        // detected - lets the caller run its own cleanup (e.g. resetting
        // per-user state, see ../clock/clock.cpp's
        // reset_to_default_profile() for the pattern) at the same point
        // the connection actually dropped.
        bool consume_disconnected();

        // special_instruction: optional, see ../PROTOCOL.md's `result`
        // message shape - replaces olli's default "be concise, no jargon"
        // framing for this one result with a custom instruction for how
        // the model should relay it (e.g. "relay this in full, verbatim -
        // do not summarize"). Omit for the default framing, same as every
        // remote tool got before this parameter existed.
        //
        // attachment: optional, see OLLI_ATTACHMENT above - exact data
        // (e.g. a full document's real text) that reaches olli's
        // COMMS::TOOL_ATTACHMENTS directly, bypassing the model's own
        // narration entirely, unlike everything else in `result`. Omit
        // (default-constructed, type empty) for a result with nothing to
        // attach, same as every remote tool got before this parameter
        // existed.
        void send_result(const std::string& call_id, const std::string& result, const std::string& special_instruction = "", const OLLI_ATTACHMENT& attachment = {});
        void send_error(const std::string& call_id, const std::string& error);
        // action: the optional real tool-call payload described in
        // ../PROTOCOL.md's `event` message shape - omit for narration-only.
        //
        // origin_id: which call set up whatever just produced this event
        // (e.g. the call_id of the set_timer call this expiry traces back
        // to) - lets olli recognize which conversation actually asked for
        // this instead of narrating it as if it just answered whatever's
        // currently being discussed. Defaults to the reserved
        // EVENT_NO_ORIGIN_ID above for an event with no such call at all.
        //
        // var: optional {"name", "value"} pair (../PROTOCOL.md's `event`
        // shape again) - a generic, olli-side state update this event
        // represents, independent of action above (a person's own
        // configured on_near_action/on_away_action already occupies action
        // on a presence event - var is a separate slot precisely so the two
        // don't collide). Appended last, after origin_id (not next to
        // action, where it conceptually belongs) specifically so existing
        // positional call sites - e.g. clock.cpp's
        // send_event(message, action, timer.origin_call_id) - keep meaning
        // exactly what they already do; nlohmann::json's implicit
        // std::string constructor means inserting a json param before
        // origin_id would have silently compiled while actually passing
        // origin_call_id as var instead, leaving origin_id at its default.
        // olli-side code matches on name (a plain string, same "no
        // header-wide change for a new kind" reasoning OLLI_ATTACHMENT's
        // own type field already uses) - a tool sends whatever name/value
        // it wants; whether anything currently reads that name is olli's
        // own business, not this tool's.
        void send_event(const std::string& message, const nlohmann::json& action = nullptr, const std::string& origin_id = EVENT_NO_ORIGIN_ID, const nlohmann::json& var = nullptr);

        // Connection status text for display - what just happened to the
        // connection this tick (connecting/registered/disconnected/timed
        // out). Empty when nothing connection-related changed, so the
        // caller can leave its own last status line alone.
        std::string status() const { return status_text; }

    private:
        void handle_disconnect(const std::string& reason);

        std::string host;
        in_addr host_addr{};
        nlohmann::json register_message;
        int port;

        int sock_fd = -1;
        std::string read_buffer;
        std::deque<nlohmann::json> incoming;

        std::chrono::steady_clock::time_point last_sent;
        std::chrono::steady_clock::time_point last_received;
        // Epoch (not "now"), so the very first service() call attempts a
        // connection immediately instead of waiting a full
        // RECONNECT_INTERVAL_SECONDS first.
        std::chrono::steady_clock::time_point last_connect_attempt{};

        std::string status_text;
        bool disconnected_flag = false;

        // Every remote tool connects to the exact same fixed port per server
        // (OLLI_TOOL_PORT / SUBCON_TOOL_PORT, above) - a second copy of the
        // SAME tool running at once would both try to register the identical
        // set of names on that server, with no way for it to know which one should
        // actually answer a given call. Held for this process's whole
        // lifetime (never explicitly closed) via a non-blocking flock() on
        // a lock file keyed off this tool's own registered names
        // (register_message above) - acquired once, in the constructor,
        // before the first connection attempt. sock_fd stays -1 forever
        // and service() never calls try_connect() while this is true - see
        // both bodies (olli_link.cpp) for exactly where.
        int instance_lock_fd = -1;
        bool duplicate_instance = false;
        // True once service() has reported duplicate_instance via
        // status_text one time - status() is documented as "empty when
        // nothing connection-related changed", so this keeps that promise
        // instead of re-announcing the same fact every single tick forever.
        bool duplicate_instance_reported = false;
};

// --- Helpers for a tool holding one link per server (olli + subcon) ---
//
// Free functions rather than OLLI_LINK members: the class itself stays
// strictly one-link-per-server, these only combine two of them.

// True exactly once per drop: if either link was just found disconnected
// (consume_disconnected() on both is always called, so neither flag can be
// left stale) AND neither link is still connected. A tool's per-profile
// state (identity, settings loaded from it) must only be reset in that
// case - one server going away must not wipe state the other server's
// still-live link is relying on. See ../clock/clock.cpp's main() for the
// reference use.
inline bool all_links_dropped(OLLI_LINK& first, OLLI_LINK& second)
{
    bool a = first.consume_disconnected();
    bool b = second.consume_disconnected();
    return (a || b) && !first.is_connected() && !second.is_connected();
}

// Short text for a tool's connection line about the subcon link - the olli
// link's own status() text keeps the line to itself, this is just appended
// to it. A subcon that isn't running is simply "not connected", never an
// error: the link keeps quietly retrying and nothing else is affected.
inline std::string subcon_link_indicator(const OLLI_LINK& subcon_link)
{
    return std::string("  |  subcon: ") + (subcon_link.is_connected() ? "connected" : "not connected");
}
