#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "comms.h" // TOOL_ATTACHMENT

namespace httplib { class Server; }

/**
 * WEB_SERVER_CLASS
 *
 * Lets olli's own chat be driven from a browser on the LAN, alongside
 * (not instead of) the local terminal - same relationship TextToSpeech/
 * Voca have to the ncurses/keyboard channel: a self-contained engine with
 * its own background thread, polled non-blockingly once per
 * IO_WORKER_CLASS::thread_main() tick rather than reached into directly.
 *
 * Owns a cpp-httplib httplib::Server (see olla.cpp for this codebase's
 * other use of cpp-httplib, there as a client instead of a server) and its
 * own accept/serve thread - unavoidable, since accepting sockets can't be
 * polled synchronously inside thread_main()'s ~20ms tick, same reasoning
 * that already applies to Voca's capture/transcribe threads and
 * TextToSpeech's own worker thread.
 *
 * Three routes: GET / (the page itself, a single self-contained HTML/JS/
 * CSS string, no external assets), GET /events (a Server-Sent Events
 * stream - one long-lived connection per open browser tab, pushing new
 * chat/thinking/system text, links, and tools-panel updates as they
 * arrive), POST /input (one submitted line's plain-text body).
 *
 * Three small thread-safe entry points are the entire surface
 * IO_WORKER_CLASS touches: poll_input() (called from thread_main(),
 * mirrors popVocaEvent()'s shape), push_output() (called from
 * IO_WORKER_CLASS::display_with_web(), mirrors what display_with_tts()
 * does with tts_pending), and push_tool_names() (same caller, current
 * tools-panel state rather than an append-only stream) - everything else
 * (the HTTP routes, the SSE loop, the page's own HTML/JS) is private to
 * this file.
 *
 * Port 47601 is already the remote-tools protocol's (tools/PROTOCOL.md,
 * loopback-only) - this listens on a different port, 47602, and binds
 * 0.0.0.0 rather than loopback-only, since the whole point is LAN reach.
 * No authentication - trusted home LAN only, same trust boundary as the
 * remote-tools protocol.
 */
class WEB_SERVER_CLASS
{
    public:
        explicit WEB_SERVER_CLASS(int port = 47602);
        ~WEB_SERVER_CLASS();

        WEB_SERVER_CLASS(const WEB_SERVER_CLASS&) = delete;
        WEB_SERVER_CLASS& operator=(const WEB_SERVER_CLASS&) = delete;

        // Starts listening on its own background thread. Returns once the
        // listen socket is bound (or logs to stderr and returns false on
        // failure) - does not block for the server's whole lifetime.
        bool start();

        // Stops the server and joins its thread. Safe to call even if
        // start() was never called or already failed.
        void stop();

        // Pops one pending line submitted via POST /input, if any -
        // mirrors IO_WORKER_CLASS::popVocaEvent()'s shape/contract
        // exactly (false if nothing pending).
        bool poll_input(std::string& out);

        // Hands new output text/attachments to any currently-connected
        // browser tab(s), broadcast to every open /events stream. Safe to
        // call every tick even when nothing's connected (see has_client())
        // - just queues into pending_* below, which nothing ever reads
        // until a client connects.
        void push_output(const std::string& llm_text, const std::string& thinking_text,
                          const std::string& system_text,
                          const std::vector<TOOL_ATTACHMENT>& attachments);

        // Updates the tools-panel list, broadcasting it to connected
        // browser tab(s) only if it actually changed since the last call -
        // unlike push_output() above, this is current STATE (what tools
        // exist right now), not an append-only stream, so it's compared
        // against current_tool_names_ rather than just queued. A brand new
        // connection always gets sent the current list once, regardless of
        // whether it's changed (see register_routes()' /events handler).
        void push_tool_names(const std::vector<std::string>& names);

        // Whether at least one browser tab currently has an open /events
        // connection - the "connected" gate IO_WORKER_CLASS::
        // display_with_web() uses to skip work when nobody's watching.
        bool has_client() const;

    private:
        int port_;
        std::unique_ptr<httplib::Server> server_;
        std::thread server_thread_;
        std::atomic<bool> running_{false};

        std::mutex input_mutex_;
        std::deque<std::string> pending_input_;

        // One shared holding place for not-yet-sent output, same spirit
        // as comms_web itself (IO_WORKER_CLASS::display_with_web() calls
        // push_output() only once comms_web has something new - see its
        // own comment, io_worker.cpp) - accumulated here, drained by
        // whichever /events connection's own loop notices it next.
        // Deliberately NOT a per-connection queue/cursor - phase 1 is
        // built for the expected case (one browser tab watching at a
        // time); two tabs open simultaneously would race for the same
        // pending text instead of each seeing everything. Fine for now,
        // worth revisiting if multi-tab ever actually matters.
        mutable std::mutex output_mutex_;
        std::string pending_llm_;
        std::string pending_thinking_;
        std::string pending_system_;
        std::vector<TOOL_ATTACHMENT> pending_attachments_;

        // Latest known tools-panel state (not append-only, see
        // push_tool_names()'s own comment) - tool_names_dirty_ set true on
        // an actual change AND forced true whenever a new /events
        // connection opens (so it gets the current list at least once
        // even if nothing's changed lately), cleared once some
        // connection's own drain loop sends it. Shared, not per-connection
        // - same phase-1 simplification as pending_llm_ etc. above, so a
        // second simultaneous tab can rarely miss one send if it opens
        // right as another connection's drain clears the flag first;
        // harmless, the next actual change (or reconnect) catches it up.
        std::vector<std::string> current_tool_names_;
        bool tool_names_dirty_ = false;

        std::atomic<int> client_count_{0};

        void register_routes();
};

#endif
