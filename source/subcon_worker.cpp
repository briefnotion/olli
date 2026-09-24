#include "subcon_worker.h"

#include "olla.h" // ollama_system
#include "io_worker.h" // IO_WORKER_CLASS - see subcon_io_worker's own comment below

#include <chrono>
#include <thread>

void SUBCON_WORKER_CLASS::thread_start()
{
    THREAD_CONTROL.create(1000);
    THREAD_CONTROL.start_render_thread([this]() { thread_main(); });
}

void SUBCON_WORKER_CLASS::thread_stop()
{
    RUN = false;
    THREAD_CONTROL.wait_for_thread_to_finish();
}

void SUBCON_WORKER_CLASS::thread_main()
{
    // Local, not a class member - same reasoning TOOL_WORKER_CLASS::
    // thread_main() already has for its own tools_list/remote_tools
    // (tool_worker.cpp): only this thread ever touches it, and nothing
    // outside thread_main() needs to reach in directly. This is the
    // subconscious's own LLM.
    ollama_system subcon_llm;

    // Short human-readable tag for debug_full_history.txt (debug_label's
    // own comment, olla.h) - a member of ollama_system itself, not PROPS,
    // so it's set here directly rather than carried over by the PROPS copy
    // below.
    subcon_llm.debug_label = "subcon";

    // One-way copy from PROPS (set once by main.cpp, before thread_start()
    // - see that class member's own comment, subcon_worker.h) into this
    // instance's own PROPS - same model/host/port as the real chat. Just a
    // stepping stone, not canon: real overrides on top of it below.
    subcon_llm.PROPS = PROPS;

    // Never persist this instance's own history to disk - a wholesale
    // PROPS copy above would otherwise carry over the real
    // LOAD_SAVE_HISTORY_ON_DISK=true and OLLI_DIRECTORY, silently letting
    // this instance overwrite the real history.json with its own - the
    // exact trap SIDETRACK_CLASS::run_consolidation() already had to avoid
    // (sidetrack.cpp).
    subcon_llm.PROPS.LOAD_SAVE_HISTORY_ON_DISK = false;

    // Its own subdirectory, separate from the profile's own top-level one -
    // keeps whatever it eventually needs of its own cleanly separated
    // rather than cluttering the main directory. Not yet acted on - nothing
    // calls subcon_llm.open() yet, so nothing's created on disk until that
    // lands.
    subcon_llm.PROPS.OLLI_DIRECTORY = PROPS.OLLI_DIRECTORY / "subcon";

    // Chat itself runs with use_thinking = false (main.cpp) - the
    // subconscious wants its own reasoning regardless of that, so this is
    // forced true rather than inherited from the PROPS copy above.
    subcon_llm.PROPS.use_thinking = true;

    // No display to stream into - subcon_comms below is blank/never shown
    // anywhere, so there's nothing for incremental chunks to write to.
    // send() (olla.cpp) only takes the streaming code path at all if either
    // of these is true; with both off it uses the plain non-streaming
    // request instead and still populates last_received.response/.thinking
    // the same way at the end - no difference in the actual result, just
    // skips work with nowhere to go.
    subcon_llm.PROPS.stream_output = false;
    subcon_llm.PROPS.stream_thinking = false;

    // Empty on purpose - subcon has no tools yet (deliberately passive for
    // now, IDEAS.md's own "subconscious" section). Local, not a class
    // member, same reasoning as subcon_llm above: self-contained to this
    // thread, nothing outside thread_main() needs to reach in. open()'s
    // only use of it is looping tool->configure(*this) per entry - a no-op
    // on empty.
    std::vector<std::unique_ptr<TOOL_BASE>> subcon_tools_list;

    // Own persona, not the default (which is written for a general
    // assistant with tool guidance and was leaking through - visible in an
    // earlier live test as an in-character reply that had nothing to do
    // with what subcon actually is). Placeholder honest about current
    // reality, not the real design yet - revisit once that's worked out
    // (IDEAS.md). Set before open() - that's what seeds the protected
    // opening message with it (same order TOOL_DELEGATOR's own instance
    // uses, tools.cpp).
    subcon_llm.OLLAMA_OPENING =
        "You are Olli's subconscious: a background reasoning process, "
        "separate from the main conversation the user is having with Olli. "
        "You have no tools and nothing real to do yet - this is still a "
        "test skeleton. When prompted, respond briefly and plainly.";

    // One-arg overload, not the (tools_list, Properties) one - that one
    // does PROPS = Properties first thing (olla.cpp), which would stomp the
    // overrides just set above.
    subcon_llm.open(subcon_tools_list);

    // Every real ollama_system call (send(), process(), integrate_tool_
    // result()) takes a COMMS&. Local, not a class member, same self-
    // contained reasoning as subcon_llm/subcon_tools_list above - and
    // blank on purpose, same as SIDETRACK_CLASS::run_consolidation()'s own
    // blank_comms (sidetrack.cpp): nothing here speaks/displays or calls a
    // tool yet, so there's nothing for it to carry.
    COMMS subcon_comms;

    // process()/handle_instance_tools() take IO_WORKER_CLASS& (a reference,
    // not a pointer - unlike system/tool_worker below, there's no nullptr
    // option), but only ever actually touch it inside dispatch_tool_call(),
    // which only runs when a tool call exists to dispatch - structurally
    // impossible here since subcon_tools_list is empty. So this is a real
    // object only to satisfy the signature, never started (thread_start()
    // never called on it) and never the real main.cpp io_worker - fully
    // self-contained to this thread, same as everything else here.
    IO_WORKER_CLASS subcon_io_worker;

    // Placeholder delay for the one-shot test prompt below, not a
    // considered design choice yet - same as the old sleep_for placeholder
    // this loop used to just sit in.
    TIMED_IS_READY_SIMPLE test_prompt_timer;
    test_prompt_timer.set(60000); // ms
    bool test_prompt_sent = false;

    RUN = true;
    while (RUN)
    {
        // Temporary: proves the whole pipe (open() -> real model -> a real
        // send()/process() round trip) actually works end to end. Not real
        // design - subcon has nothing of its own to think about yet, this
        // just gives it one thing to say once, on a delay, so there's
        // something to observe - not a repeating nag every 60s.
        if (!test_prompt_sent && test_prompt_timer.is_ready())
        {
            test_prompt_sent = true;

            subcon_comms.INPUT_FROM_USER = "Say hello and confirm you're running.";
            subcon_comms.ENTER_PRESSED = true;
        }

        subcon_llm.input(subcon_comms, nullptr);
        subcon_llm.process(subcon_io_worker, nullptr, subcon_tools_list, nullptr, subcon_comms);

        // "Do something with the output" - for now just prove it arrived,
        // via the same shared debug log everything else uses (subcon has
        // no UI of its own to show this in yet). Cleared after logging,
        // same reasoning as tools.cpp's own "instance closed" sites: leaving
        // it set would log the same response again next tick.
        //
        // !is_processing is required, not just last_received.complete - a
        // real, live crash (SIGABRT in ollama_system::~ollama_system(),
        // 2026-09-24) traced back to this same missing guard sidetrack.cpp
        // already had to add for its own action-capture (TODO.md's
        // 2026-09-23 entry): send() (olla.cpp) sets last_received.complete
        // true near its own tail end, but that runs on the chat_thread it
        // spawned - is_processing only flips false slightly later, once
        // that thread's own lambda finishes its next line. Reading/clearing
        // last_received.response in that narrow window is an unsynchronized
        // race against whatever chat_thread is still doing, undefined
        // behavior with no happens-before relationship - not just a stale
        // read, capable of real memory corruption.
        if (!subcon_llm.is_processing && subcon_llm.last_received.complete && !subcon_llm.last_received.response.empty())
        {
            DEBUG_LOG_CLASS::instance().log_event("subcon", "test prompt response: " + subcon_llm.last_received.response);
            subcon_llm.last_received.response.clear();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
