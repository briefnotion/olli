#include "tool_worker.h"
#include "health.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "remote_tools.h" // REMOTE_TOOL_LISTENER, TOOL_REMOTE
#include "tools.h" // add_tool() - already pulled in via remote_tools.h, named explicitly since it's used directly here

void TOOL_WORKER_CLASS::thread_start()
{
    THREAD_CONTROL.create(1000);
    THREAD_CONTROL.start_render_thread([this]() { thread_main(); });
}

void TOOL_WORKER_CLASS::thread_stop()
{
    RUN = false;
    THREAD_CONTROL.wait_for_thread_to_finish();
}

namespace
{
    // How long a call for a tool that no connected tool offers waits before it is answered with an error (see the sweep in
    // thread_main()) - long enough for a tool that is re-registering to claim it, short enough that nobody notices a wait.
    constexpr int UNAVAILABLE_TOOL_GRACE_SECONDS = 3;

    // Same wording tools.cpp's failure_instruction uses for a failed web search / Hue call / note save.
    // The last sentence is aimed at the "Let me try another way..." promise a small model tends to make and then never
    // keep, which the second-guess pass then flags every round (a 10-round loop was seen on ron, 2026-11-20): if another
    // tool could answer the question (a web search for a time the clock tool could not give), the model should use it NOW,
    // not promise to; otherwise it should say so and stop. Deliberately does not forbid looking elsewhere.
    const std::string UNAVAILABLE_TOOL_INSTRUCTION =
        "This attempt did NOT succeed - it is an error, not real data. Tell "
        "the user plainly, in your own words, that it failed and what "
        "likely went wrong. Do not present this as a real answer to their "
        "question. If another tool you have could still get the answer a "
        "different way (a web search, for example), use it now instead of "
        "only saying you will; if not, say so and stop.";
}

void TOOL_WORKER_CLASS::thread_main()
{
    // Local, not a class member - same guarding main.cpp's own tools_list
    // relies on (ai_root.h's process() comment). Only this thread ever
    // touches it; nothing crosses out of thread_main() but through
    // exchange(), so there's no other code path that could reach in.
    //
    // TOOL_REMOTE specifically, not TOOL_BASE - unlike main's tools_list,
    // this one never holds the 4 built-ins (populate_default_tools() isn't
    // called here at all). They have nothing to do in this loop (empty
    // monitor_tool() bodies, no wire component), so there's no reason for
    // them to exist in this thread's copy - simpler than holding them and
    // skipping them every tick.
    std::vector<std::unique_ptr<TOOL_REMOTE>> tools_list;

    // Local, not a CLASS_SYSTEM member (unlike main's system.remote_tools,
    // system.h) - same guarding as tools_list above. Own listening socket,
    // touched only by this thread.
    REMOTE_TOOL_LISTENER remote_tools;

    // Loud, one-time notice instead of the silent std::nullopt-forever
    // bind_failed() itself documents (remote_tools.h) - a real, live-caught
    // gap (2026-09-23): with no per-profile port, a second concurrently-
    // running olli instance (a different profile, or the same one twice)
    // silently loses remote tools for its whole session, no error anywhere
    // a user would ever see. Still not fatal - this instance just runs on
    // its own built-in tools alone from here on - but now says so once, up
    // front, rather than leaving whoever's debugging it to guess.
    if (remote_tools.bind_failed())
    {
        DEBUG_LOG_CLASS::instance().log_event("tool_worker",
            "Could not bind the remote-tool port - likely another olli instance "
            "already has it. Running with built-in tools only this session.");
    }

    RUN = true;
    while (RUN)
    {
        {
            std::lock_guard<std::mutex> lock(state_mutex);

            // Non-blocking, same call main.cpp makes on its own
            // remote_tools (main.cpp) - just polled every tick here instead
            // of once per main-loop tick.
            auto remote_registration = remote_tools.poll();
            if (remote_registration)
            {
                // Same handshake main.cpp does with its own registration
                // (main.cpp) - identity is whatever set_identity() was last
                // called with (default-constructed/empty until main calls
                // it, before thread_start()).
                auto remote_tool = std::make_unique<TOOL_REMOTE>(
                    remote_registration->fd, std::move(remote_registration->tools));
                remote_tool->send_identity(identity.name, identity.full_name, identity.about);

                tools_list.push_back(std::move(remote_tool));
            }

            // Takes over ai_root::process()'s PART 5 (ai_root.cpp,
            // flagged there) for TOOL_REMOTE specifically - serviced
            // through its new poll_communications() (remote_tools.h/.cpp):
            // non-blocking, driven by pending_calls/pending_results, no
            // chat/comms dependency at all. No type check needed - every
            // entry in this tools_list already is a TOOL_REMOTE (see its
            // own declaration comment above).
            for (auto& tool : tools_list)
                tool->poll_communications(pending_calls, pending_results, pending_events);

            // Any call_id that now has a real result doesn't need its
            // deadline tracked anymore - whether or not main's actually
            // drained it yet via get_pending_result().
            call_deadlines.erase(
                std::remove_if(call_deadlines.begin(), call_deadlines.end(),
                    [this](const std::pair<std::string, std::chrono::steady_clock::time_point>& deadline)
                    {
                        return std::any_of(pending_results.begin(), pending_results.end(),
                            [&deadline](const TOOL_RESULT& result) { return result.call_id == deadline.first; });
                    }),
                call_deadlines.end());

            // Whatever's left has genuinely gone unanswered - either no
            // connected tool ever claimed it, or one claimed it and never
            // came back. CALL_TIMEOUT_SECONDS (tool_worker.h) is long on
            // purpose (a legitimately busy tool, a network hiccup) - this
            // only fires once that generous a wait has actually passed.
            auto now = std::chrono::steady_clock::now();
            for (auto it = call_deadlines.begin(); it != call_deadlines.end(); )
            {
                if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count() < CALL_TIMEOUT_SECONDS)
                {
                    ++it;
                    continue;
                }

                // Drop it from pending_calls too, if it's still sitting
                // there unclaimed - no point a tool picking it up right
                // after we've already given up and answered for it.
                pending_calls.erase(
                    std::remove_if(pending_calls.begin(), pending_calls.end(),
                        [&it](const ToolCall& call) { return call.id == it->first; }),
                    pending_calls.end());

                TOOL_RESULT timeout_result;
                timeout_result.call_id = it->first;
                timeout_result.response = "Error: no response within "
                    + std::to_string(CALL_TIMEOUT_SECONDS) + " seconds.";
                pending_results.push_back(std::move(timeout_result));
                olli_health::note(olli_health::ERROR_KIND::TOOL_CALL_TIMEOUT);

                it = call_deadlines.erase(it);
            }

            // Drops any pending_results entry (whether just landed via
            // poll_communications() above, or just synthesized by the
            // timeout sweep above) that abandon_call() marked - its
            // dispatching instance no longer exists to ever claim it via
            // get_pending_result(call_id, ...). Only removed from
            // abandoned_call_ids once its matching result is actually
            // found, so a slower answer is still caught on a later tick.
            if (!abandoned_call_ids.empty())
            {
                pending_results.erase(
                    std::remove_if(pending_results.begin(), pending_results.end(),
                        [this](const TOOL_RESULT& result)
                        {
                            auto match = std::find(abandoned_call_ids.begin(), abandoned_call_ids.end(), result.call_id);
                            if (match == abandoned_call_ids.end()) return false;
                            abandoned_call_ids.erase(match);
                            return true;
                        }),
                    pending_results.end());
            }

            // tools_list itself still needs no separate locking - it's this
            // thread's own local variable that nothing else touches
            // directly; anything from outside only ever reaches this
            // thread's state through state_mutex (put_pending_call() and
            // the other cross-thread functions below), never tools_list.
            tools_list.erase(
                std::remove_if(tools_list.begin(), tools_list.end(),
                    [](const std::unique_ptr<TOOL_REMOTE>& tool) { return !tool->is_alive(); }),
                tools_list.end());

            // Rebuilt fresh every tick from whatever's actually connected
            // right now (same reasoning send()'s own tools rebuild has,
            // ai_root.cpp) rather than patched incrementally - tools_list is
            // small, so this is cheap, and it's simpler than tracking
            // per-tool add/remove deltas separately. manual_tool_defs isn't
            // tied to a connection at all, so it's just copied in fresh
            // each time too, rather than needing its own separate tracking.
            // Each remote def gets wrapped via add_tool() (tools.h) here -
            // the same conversion TOOL_REMOTE::register_tool() used to do
            // itself - so registered_tool_defs ends up uniformly in
            // Ollama's final schema shape, manual and remote alike.
            registered_tool_defs = manual_tool_defs;
            for (auto& tool : tools_list)
                for (auto& def : tool->get_tool_defs())
                    add_tool(registered_tool_defs, def.value("name", ""), def.value("description", ""), def.value("parameters", json::object()));

            // A call for a tool nothing offers at all - it was there earlier in the conversation and has since stopped,
            // disconnected or crashed, or the model invented the name - would otherwise sit in pending_calls for the whole
            // CALL_TIMEOUT_SECONDS, which is right for a slow tool but means minutes of silence for one that is simply not
            // there. After a short grace period (a tool that is mid-reconnect gets its chance to claim it) it is answered
            // with an error right away, with the same "this is a failure, say so" instruction other failed tools use, so
            // the model tells the user in its own words. Judged against registered_tool_defs just rebuilt above: a call
            // for an offered tool that is only busy is never touched.
            {
                const auto sweep_now = std::chrono::steady_clock::now();
                for (auto call = pending_calls.begin(); call != pending_calls.end(); )
                {
                    bool offered = false;
                    for (const auto& def : registered_tool_defs)
                    {
                        if (def.value("function", json::object()).value("name", "") == call->name) { offered = true; break; }
                    }

                    auto waiting = std::find_if(call_deadlines.begin(), call_deadlines.end(),
                        [&call](const std::pair<std::string, std::chrono::steady_clock::time_point>& entry) { return entry.first == call->id; });
                    const bool waited_long_enough = waiting != call_deadlines.end() &&
                        std::chrono::duration_cast<std::chrono::seconds>(sweep_now - waiting->second).count() >= UNAVAILABLE_TOOL_GRACE_SECONDS;

                    if (offered || !waited_long_enough)
                    {
                        ++call;
                        continue;
                    }

                    std::string shown_name = call->name.substr(0, 80);   // model-written text: cut short, no control characters
                    for (char& c : shown_name)
                        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) c = ' ';

                    TOOL_RESULT unavailable;
                    unavailable.call_id = call->id;
                    unavailable.response = "Error: the tool '" + shown_name + "' is not available right now - nothing that is connected offers it.";
                    unavailable.special_instruction = UNAVAILABLE_TOOL_INSTRUCTION;
                    pending_results.push_back(std::move(unavailable));
                    olli_health::note(olli_health::ERROR_KIND::TOOL_NOT_AVAILABLE);

                    call_deadlines.erase(waiting);
                    call = pending_calls.erase(call);
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void TOOL_WORKER_CLASS::add_manual_tool_def(const json& def)
{
    std::lock_guard<std::mutex> lock(state_mutex);

    manual_tool_defs.push_back(def);
}

void TOOL_WORKER_CLASS::register_local_tools(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, ai_root& chat)
{
    json scratch = json::array();
    for (auto& tool : tools_list)
        tool->register_tool(chat, scratch);
    for (auto& def : scratch)
        add_manual_tool_def(def);
}

void TOOL_WORKER_CLASS::set_identity(const USER_IDENTITY& user_identity)
{
    std::lock_guard<std::mutex> lock(state_mutex);

    identity = user_identity;
}

void TOOL_WORKER_CLASS::put_pending_call(const ToolCall& call)
{
    std::lock_guard<std::mutex> lock(state_mutex);

    pending_calls.push_back(call);
    call_deadlines.emplace_back(call.id, std::chrono::steady_clock::now());
}

bool TOOL_WORKER_CLASS::get_pending_result(const std::string& call_id, TOOL_RESULT& out)
{
    // try_lock via unique_lock (std::try_to_lock), not the blocking
    // lock_guard every other method here uses - safe specifically because
    // this is already a polling function, called every tick until it
    // returns true. A false from lock contention is behaviorally
    // identical to a genuine "not ready yet" false (the caller just
    // checks again next tick either way) - unlike put_pending_call()/
    // abandon_call()/etc., nothing here only gets one chance to happen.
    // owns_lock() reports whether the attempt actually succeeded;
    // unique_lock still unlocks automatically in its own destructor if it
    // did, same RAII guarantee lock_guard gives everywhere else.
    std::unique_lock<std::mutex> lock(state_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;

    for (auto it = pending_results.begin(); it != pending_results.end(); ++it)
    {
        if (it->call_id == call_id)
        {
            out = std::move(*it);
            pending_results.erase(it);
            return true;
        }
    }

    return false;
}

void TOOL_WORKER_CLASS::abandon_call(const std::string& call_id)
{
    std::lock_guard<std::mutex> lock(state_mutex);

    abandoned_call_ids.push_back(call_id);
}

bool TOOL_WORKER_CLASS::get_pending_event(TOOL_EVENT& out)
{
    // Same try_lock reasoning as get_pending_result() just above - already
    // a polling function, so a lock-contention false is indistinguishable
    // from a genuine "nothing right now" false.
    std::unique_lock<std::mutex> lock(state_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;

    bool got_one = false;
    if (!pending_events.empty())
    {
        out = std::move(pending_events.front());
        pending_events.erase(pending_events.begin());
        got_one = true;
    }

    return got_one;
}

json TOOL_WORKER_CLASS::get_registered_tool_defs()
{
    std::lock_guard<std::mutex> lock(state_mutex);

    return registered_tool_defs;
}
