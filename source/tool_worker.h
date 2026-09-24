#ifndef TOOL_WORKER_H
#define TOOL_WORKER_H

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "olla.h" // ToolCall
#include "remote_tools.h" // TOOL_RESULT, TOOL_REMOTE
#include "threading.h"

/**
 * TOOL_WORKER_CLASS
 *
 * Modeled on IO_WORKER_CLASS (io_worker.h/.cpp): owns a background thread
 * via thread_main(). Unlike IO_WORKER_CLASS's single combined exchange()
 * (one relay covering several COMMS fields every tick, because keyboard/
 * audio genuinely need continuous two-way syncing), this worker's two data
 * flows are independent and arrive at different rhythms - a call only
 * happens when the LLM issues one, a result only when one's ready - so
 * they get their own small functions instead: put_pending_call() and
 * get_pending_result(). Each just locks state_mutex for as long as it takes
 * to touch this worker's own state directly, same as thread_main() does for
 * its own tick - a real mutex, not sharing one combined call.
 *
 * Was an INTERUPTED/PROCESSING flag pair (still IO_WORKER_CLASS's scheme,
 * io_worker.h) until a live crash (TODO.md, SIGSEGV inside thread_main()'s
 * own json copy) showed it only ever excluded thread_main() from a single
 * caller - nothing stopped two callers (e.g. the main chat's process() and
 * a task-runner script's own instance.process(), running on different
 * threads) from being in their own "critical sections" at the same time,
 * since PROCESSING is only ever set by thread_main() and INTERUPTED is one
 * shared flag any finishing caller clears unconditionally, whether or not
 * another caller is still mid-access.
 */
class TOOL_WORKER_CLASS
{
    private:
        THREADING_INFO THREAD_CONTROL;
        std::mutex state_mutex;

        bool RUN = false;

        // Runs on the background thread - only ever invoked internally,
        // via thread_start()'s own lambda (same access rights as any
        // other member-function code, since the lambda is defined inside
        // one), never meant to be called directly from outside.
        void thread_main();

        // The communication variables tools_list's calls/results actually
        // cross put_pending_call()/get_pending_result() through - the same
        // worker-private copies io_worker's comms_buffer/comms_keyboard/
        // comms_stt_tts (io_worker.h) are for that class, just two of them
        // instead of one shared COMMS.
        //
        // FIFO: pushed at the back, drained from the front - oldest call/
        // result serviced first.
        //
        // Outgoing: main hands a call off via put_pending_call() for this
        // thread to actually dispatch to the matching TOOL_REMOTE - reuses
        // ToolCall (olla.h) rather than inventing a duplicate id/name/
        // arguments type.
        std::vector<ToolCall> pending_calls;

        // Incoming: this thread queues a TOOL_RESULT as each one lands, for
        // get_pending_result() to drain back to main.
        std::vector<TOOL_RESULT> pending_results;

        // Incoming, same as pending_results but for unsolicited `event`
        // messages (TOOL_EVENT's own comment, remote_tools.h) - kept
        // separate since an event isn't a response to any call_id.
        std::vector<TOOL_EVENT> pending_events;

        // Call ids passed to abandon_call() above - checked against every
        // fresh pending_results entry each thread_main() tick (both a real
        // answer and a timeout-sweep-synthesized one), dropping a match
        // instead of leaving it stranded. An id only leaves this list once
        // its matching result has actually shown up and been dropped, so a
        // slow answer (up to CALL_TIMEOUT_SECONDS) still gets caught.
        std::vector<std::string> abandoned_call_ids;

        // How long a call gets to produce a real result before thread_main()
        // gives up on it and synthesizes an error TOOL_RESULT instead -
        // covers both a call nothing ever claims (no connected tool
        // declares that name) and a call a tool claimed but never answered
        // (crashed, disconnected, or just never replies). Generous on
        // purpose: a legitimately busy tool (e.g. rag mid-search) shouldn't
        // get cut off just because it's slow - a queued call costs nothing
        // but a slot in a vector while it waits, so there's no pressure to
        // reclaim it quickly the way a held resource would need.
        static constexpr int CALL_TIMEOUT_SECONDS = 300;

        // call_id -> when put_pending_call() first queued it. Tracks a
        // call's whole lifecycle regardless of whether it's still sitting
        // unclaimed in pending_calls or already claimed as some
        // TOOL_REMOTE's own awaiting_call_id - thread_main() doesn't need
        // to know which, just how long it's been waiting. Entry removed as
        // soon as a real result exists for that call_id (whether or not
        // main's drained it yet via get_pending_result()), or once
        // thread_main()'s own timeout sweep gives up on it.
        std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> call_deadlines;

        // Fixed set, not tied to any connection - a built-in tool (still
        // dispatched/run inline on main's own thread, unchanged - see
        // add_manual_tool_def()'s own comment) has no handshake to learn
        // its schema from. Already in Ollama's final wrapped schema shape
        // ({"type":"function","function":{...}}), not the raw wire-protocol
        // shape a remote tool's own tool_defs has - the caller gets this by
        // constructing the real tool and calling its own register_tool()
        // (tools.cpp) into a scratch array, the same call send() used to
        // make directly, so there's exactly one place that ever describes a
        // built-in's schema, not two.
        json manual_tool_defs = json::array();

        // The "identification bubble" for every tool currently available -
        // both this fixed manual_tool_defs set and every currently-
        // connected remote tool, combined, and both already in the same
        // final wrapped shape. Refreshed by thread_main() itself once per
        // tick from its own local tools_list (each TOOL_REMOTE's own raw
        // get_tool_defs(), remote_tools.h, wrapped via add_tool() - tools.h
        // - the same conversion TOOL_REMOTE::register_tool() used to do
        // itself) plus manual_tool_defs above, so get_registered_tool_defs()
        // below has something of its own to hand back without reaching
        // into tools_list directly (that stays local/guarded, same as
        // IO_WORKER_CLASS::tool_names does for its own list, io_worker.h),
        // and send() (olla.cpp) can fold the result straight into its own
        // outgoing tools array with no further conversion needed.
        json registered_tool_defs = json::array();

        // What thread_main() sends via send_identity() (tools/PROTOCOL.md)
        // right after a new registration completes - main.cpp has the real
        // USER_IDENTITY (CLASS_SYSTEM::user, system.h) this thread has no
        // path to otherwise, so it's handed over once via set_identity()
        // instead. Default-constructed (empty strings) until that's called.
        USER_IDENTITY identity;

    public:
        void thread_start();
        void thread_stop();

        // Both run on the MAIN/owner thread, called independently as main
        // actually has something to send/check for - not tied to a fixed
        // per-tick cadence the way IO_WORKER_CLASS::exchange() is.

        // Registers one manually-inserted tool def, already in Ollama's
        // final wrapped schema shape (see manual_tool_defs' own comment) -
        // for a built-in tool whose actual code still runs inline on main's
        // own thread (tools.cpp), not here; this only makes it show up in
        // get_registered_tool_defs()'s combined list so send() only has one
        // source to advertise to Ollama instead of two. Called once per
        // built-in, before thread_start() (main.cpp) - no lasting harm
        // calling it later either, same rendezvous guard as the rest.
        void add_manual_tool_def(const json& def);

        // Seeds manual_tool_defs for every built-in in tools_list, reusing
        // each real instance's own register_tool() (chat is only needed
        // because that's register_tool()'s own signature) rather than
        // duplicating any schema here - same trick add_manual_tool_def()'s
        // caller used to do inline in main.cpp. Called once, before
        // thread_start(), same as a manual add_manual_tool_def() call would be.
        void register_local_tools(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, ollama_system& chat);

        // Sets what a newly-registered remote tool is told via
        // send_identity() (tools/PROTOCOL.md) - see identity's own comment
        // above for why this exists. Called once, before thread_start()
        // (main.cpp) - no lasting harm calling it later either, same
        // rendezvous guard as the rest.
        void set_identity(const USER_IDENTITY& user_identity);

        // Queues one call for this worker to dispatch.
        void put_pending_call(const ToolCall& call);

        // Claims one specific call's result, if it's ready - false (and
        // 'out' untouched) otherwise. Every ollama_system instance sharing
        // this one worker (main chat, a task-runner's own instance, a
        // delegate's own instance) needs to ask by id, not just take
        // whatever's oldest: dispatching a call and blindly popping
        // "whatever's next" back is only correct when exactly one instance
        // is ever waiting at a time, which stopped being true the moment
        // more than one of them could have a call in flight together (see
        // TODO.md's tool_worker event/result correlation entry - this
        // replaced an earlier blind get_pending_result(TOOL_RESULT&) for
        // exactly that reason).
        bool get_pending_result(const std::string& call_id, TOOL_RESULT& out);

        // Marks one call's eventual result (whether a real answer or a
        // CALL_TIMEOUT_SECONDS-triggered synthesized error) to be silently
        // dropped by thread_main() instead of kept in pending_results
        // forever - for when the ollama_system instance that dispatched
        // the call is being destroyed with that call still outstanding
        // (see olla.cpp PART 2's own call site: a background task/delegate
        // instance can finish without ever waiting for every call it
        // fired). Without this, get_pending_result(call_id, ...) above
        // would never be asked for that id again - nothing's
        // outstanding_tool_call_ids (olla.h) would still contain it - so
        // the result would sit in pending_results with no one left to
        // ever claim it.
        void abandon_call(const std::string& call_id);

        // Same shape as get_pending_result(), for pending_events instead.
        bool get_pending_event(TOOL_EVENT& out);

        // Snapshot of registered_tool_defs, for main's send() to fold into
        // its own outgoing tools array. A plain copy, not a pop/drain like
        // the three above - this is a standing fact ("what's connected right
        // now"), not a one-shot event, so there's nothing to consume.
        json get_registered_tool_defs();
};

#endif
