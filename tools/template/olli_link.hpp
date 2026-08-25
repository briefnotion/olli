// OLLI_LINK - the "talk to olli" plumbing every remote tool needs: connect/
// reconnect over TCP, newline-delimited JSON framing, heartbeat (ping/pong),
// dead-connection detection, and the register/call/result/event message
// shapes. See ../PROTOCOL.md for the wire protocol this implements.
//
// Deliberately tool-agnostic - knows nothing about what this tool registers
// or how it answers a call. See template_tool.cpp's olli_processing() for
// where that tailored logic lives and how it drives this class.
//
// One OLLI_LINK per tool process, constructed once in main(). It owns its
// own socket fd but not the wait for activity on it: main() still runs the
// single select() call across stdin + the socket (via fd() below) exactly
// as before this split - only the connect/read/heartbeat logic that used to
// sit inline in main()'s loop moved in here.

#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <deque>
#include <string>

#include <netinet/in.h>

class OLLI_LINK {
    public:
        // host_display is what shows up in status text ("...at 127.0.0.1");
        // host_addr is the already-resolved address try_connect() dials -
        // main() does its own inet_pton() (see its [host] argument
        // handling) so an invalid address is caught with a clean usage
        // message before anything here is constructed.
        OLLI_LINK(std::string host_display, in_addr host_addr, nlohmann::json register_message);
        ~OLLI_LINK();

        OLLI_LINK(const OLLI_LINK&) = delete;
        OLLI_LINK& operator=(const OLLI_LINK&) = delete;

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

        void send_result(const std::string& call_id, const std::string& result);
        void send_error(const std::string& call_id, const std::string& error);
        // action: the optional real tool-call payload described in
        // ../PROTOCOL.md's `event` message shape - omit for narration-only.
        void send_event(const std::string& message, const nlohmann::json& action = nullptr);

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
};
