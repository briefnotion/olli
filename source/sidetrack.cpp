#ifndef SIDETRACK_CPP
#define SIDETRACK_CPP

#include "sidetrack.h"

// ----

/**
 * Consolidation
 *
 * 1. Bucket every message by its consolidation_level (protected messages,
 *    level < 0, are set aside untouched).
 * 2. Walk the buckets bottom-up. Any bucket holding at least
 *    (starts_at + sizes) messages gets its oldest 'sizes' messages
 *    summarized into one message, promoted to the next level up.
 * 3. Flatten back into a single chronological vector: protected messages
 *    first, then oldest (highest level) down to newest (level 0).
 *
 * Note: chat_history here is the sidetrack thread's private working copy
 * (see SIDETRACK_CLASS::check's handoff) - nothing else touches it while
 * this runs, so no locking is needed inside this function.
 */
bool consolidate(std::vector<Message>& chat_history, OLLAMA_SYSTEM_PROPERTIES& config)
{
    if (chat_history.empty()) return false;

    size_t starts_at = static_cast<size_t>(config.consolitation_starts_starts_at);
    size_t sizes = static_cast<size_t>(config.consolitation_sizes);
    if (sizes == 0) return false;

    // 1. Bucket by level.
    std::vector<Message> protected_messages;
    std::vector<std::vector<Message>> levels;

    for (const Message& msg : chat_history) {
        if (msg.consolidation_level < 0) {
            protected_messages.push_back(msg);
            continue;
        }
        size_t level = static_cast<size_t>(msg.consolidation_level);
        if (level >= levels.size()) levels.resize(level + 1);
        levels[level].push_back(msg);
    }
    levels.resize(levels.size() + 1); // headroom for a promotion out of the top level

    // 2. Drain each level in turn; promotions feed the next iteration.
    bool any_consolidation_occurred = false;

    ollama_system consolidate_client;
    consolidate_client.PROPS.host = config.host;
    consolidate_client.PROPS.port = config.port;
    consolidate_client.PROPS.model = config.model;
    consolidate_client.PROPS.num_ctx = config.num_ctx;
    consolidate_client.PROPS.use_thinking = false;
    consolidate_client.PROPS.stream_output = false;

    bool llm_failed = false;
    for (size_t level = 0; !llm_failed && level + 1 < levels.size(); ++level) {
        while (levels[level].size() >= starts_at + sizes) {
            // Oldest 'sizes' messages sit at the front of the bucket.
            std::string prompt = "Summarize the following conversation segment concisely while preserving key facts and the current state of the topic:\n";
            for (size_t i = 0; i < sizes; ++i) {
                prompt += "\n[" + levels[level][i].role + "]: " + levels[level][i].content;
            }

            consolidate_client.history.clear();
            consolidate_client.send(prompt, "system");

            std::string summary_text = consolidate_client.last_received.response;
            if (summary_text.empty()) summary_text = consolidate_client.last_received.thinking;

            if (!consolidate_client.last_received.complete || summary_text.empty()) {
                llm_failed = true; // give up; keep whatever progress was already made
                break;
            }

            levels[level].erase(levels[level].begin(), levels[level].begin() + static_cast<std::ptrdiff_t>(sizes));

            Message summary_msg;
            summary_msg.role = "system";
            summary_msg.content = "Summary of previous context: " + summary_text;
            summary_msg.consolidation_level = static_cast<int>(level) + 1;
            levels[level + 1].push_back(summary_msg);

            any_consolidation_occurred = true;
        }
    }

    if (any_consolidation_occurred) {
        chat_history.clear();
        chat_history.insert(chat_history.end(), protected_messages.begin(), protected_messages.end());
        for (size_t level = levels.size(); level-- > 0; ) {
            chat_history.insert(chat_history.end(), levels[level].begin(), levels[level].end());
        }
    }

    return any_consolidation_occurred;
}

// ----

SIDETRACK_CLASS::SIDETRACK_CLASS()
{
}

void SIDETRACK_CLASS::create(OLLAMA_SYSTEM_PROPERTIES Ollama_Properties)
{
    SIDETRACK_CHAT_INSTANCE.PROPS = Ollama_Properties;
}

/**
 * Runs on the sidetrack background thread (started via thread_start, joined
 * via thread_stop) for the lifetime of the program. Ticks roughly every
 * INTERVAL ms. Drives ROUTINE (0=idle, 1=consolidation, 2=second-guess
 * review) and each routine's *_PROCESSING_STAGE - see the class-level
 * comment in sidetrack.h for how this hands off to check() (main thread).
 */
void SIDETRACK_CLASS::thread_main()
{
    TIMED_IS_READY  frame_limit;     // Controls sleep time
    FLED_TIME thread_time;           // Thread gets its own Time
    thread_time.create();

    // TIMED_IS_READY defaults its ready-time to 0, which is_ready() always
    // considers already-elapsed. Without arming it here, consolidation would
    // fire on the very first check after every program start, instead of
    // waiting the full configured idle interval.
    IDLE_WAIT_TIMER_FOR_CONSOLIDATION.set(thread_time.now(), IDLE_WAIT_TIME_FOR_CONSOLIDATION);
    IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR.set(thread_time.now(), IDLE_WAIT_TIME_FOR_CONTEXT_CLEAR);

    RUN = true;
    while (RUN)
    {
        // prepare thread
        thread_time.setframetime();
        frame_limit.set(thread_time.current_frame_time(), INTERVAL);

        //std::cout << "Sidetrack Thread Running Routine" << std::endl;

        // Check for interupt signal.  Reset timers if found.
        if (INTERUPT.load())
        {
            //std::cout << "Sidetrack Thread Interupted" << std::endl;
            IDLE_WAIT_TIMER_FOR_CONSOLIDATION.set(thread_time.current_frame_time(), IDLE_WAIT_TIME_FOR_CONSOLIDATION);
            IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR.set(thread_time.current_frame_time(), IDLE_WAIT_TIME_FOR_CONTEXT_CLEAR);
            // Un-finish the clear routine too, so real activity starts its
            // idle wait fresh instead of staying "already cleared" forever.
            CLEAR_CONTEXT_STAGE = 0;
            INTERUPT.store(false);
        }

        // Sidetrack's per-tick routine dispatch (despite the name, this
        // whole block runs on the SIDETRACK thread, not the main one).
        {
            PROCESSING.store(true);


            if (ROUTINE == 0)
            {
                // Lowest priority of the three - checked first so either of
                // the assignments below silently overrides it if they're
                // also ready on the same tick. Nuking the whole
                // conversation is the most drastic outcome here, so a real
                // user turn or a routine cleanup pass should always win.
                // CLEAR_CONTEXT_STAGE == 3 means a clear already ran during
                // this idle stretch and there's nothing left to clear -
                // don't re-arm the routine every time the timer happens to
                // still read ready. Only an interrupt (see above) resets
                // that stage back to 0.
                if (IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR.is_ready(thread_time.now()) && CLEAR_CONTEXT_STAGE != 3)
                {
                    ROUTINE = 3; // Start context-clear routine
                }

                if (IDLE_WAIT_TIMER_FOR_CONSOLIDATION.is_ready(thread_time.now()))
                {
                    ROUTINE = 1; // Start consolidation routine
                }

                // Note: if both conditions are true on the same tick, this
                // silently wins over the ROUTINE = 1 assignment just above -
                // a second-guess review always takes priority over a
                // consolidation pass that happened to become ready at the
                // same moment.
                if (CHAT_FINISHED.load())
                {
                    //std::cout << "Sidetrack: Chat finished signal received." << std::endl;
                    ROUTINE = 2; // Start second guessing routine.
                    CHAT_FINISHED.store(false);
                }
            }

            // Consolidation - see CHAT_HISTORY_PROCESSING_STAGE's comments
            // in sidetrack.h for what each stage means and which thread
            // handles it. Note: SIDETRACK_CHAT_INSTANCE.PROPS is passed
            // here purely as a config bundle (model/host/port/etc) for
            // consolidate()'s own local, separate ollama_system - this
            // doesn't touch SIDETRACK_CHAT_INSTANCE itself or its history.
            if (ROUTINE == 1)
            {
                if (CHAT_HISTORY_PROCESSING_STAGE == 0)
                {
                    //std::cout << "Sidetrack: Requesting chat history for consolidation." << std::endl;
                    CHAT_HISTORY_PROCESSING_STAGE = 1;
                }

                //if (chat_history_processing_stage == 1)
                // Handled in the check function

                if (CHAT_HISTORY_PROCESSING_STAGE == 2)
                {
                    if (consolidate(temp_chat_history, SIDETRACK_CHAT_INSTANCE.PROPS))
                    {
                        CHAT_HISTORY_PROCESSING_STAGE = 3;
                    }
                    else
                    {
                        CHAT_HISTORY_PROCESSING_STAGE = 4;
                    }
                }

                //if (chat_history_processing_stage == 3)
                // Handled in the check function

                if (CHAT_HISTORY_PROCESSING_STAGE == 4)
                {
                    //std::cout << "Sidetrack: Consolidation complete. Wrapping up." << std::endl;
                    CHAT_HISTORY_PROCESSING_STAGE = 0;
                    ROUTINE = 0;
                }

                // Re-armed every tick this routine is active (not just once
                // at the end), so the idle wait always starts fresh
                // relative to whenever consolidation last did anything -
                // including while it's still mid-pass across several ticks.
                IDLE_WAIT_TIMER_FOR_CONSOLIDATION.set(thread_time.current_frame_time(), IDLE_WAIT_TIME_FOR_CONSOLIDATION);
            }

            if (ROUTINE == 2)
            {

                if (SIGNALS.INTERUPT_SIGNAL == true)
                {
                    // This only ever catches an interrupt that arrives
                    // BEFORE stage 2's SIDETRACK_CHAT_INSTANCE.send() call
                    // below starts - once that call is running, this
                    // thread is blocked inside it and can't reach this
                    // check again until send() returns on its own (see the
                    // GOTCHA in the class-level comment in sidetrack.h).
                    // Actually aborting an in-flight send() is check()'s
                    // job, via SIDETRACK_CHAT_INSTANCE.stop() - it runs on
                    // the main thread, which isn't blocked.
                    SECOND_GUESS_PROCESSING_STAGE = 3;
                }

                // ----

                if (SECOND_GUESS_PROCESSING_STAGE == 0)
                {
                    //std::cout << "Sidetrack: Starting post-chat review routine." << std::endl;
                    SECOND_GUESS_PROCESSING_STAGE = 1;
                    //SECOND_GUESS_PROCESSING_STAGE = 3;
                }

                //if (SECOND_GUESS_PROCESSING_STAGE == 1)
                // Handled in the check function

                if (SECOND_GUESS_PROCESSING_STAGE == 2)
                {
                    // std::cout << "Sidetrack: Post-chat review complete." << std::endl;

                    SIDETRACK_CHAT_INSTANCE.history = temp_chat_history;
                    SIDETRACK_CHAT_INSTANCE.PROPS.use_thinking = true; // Enable thinking mode to get more detailed analysis from the model.
                    SIDETRACK_CHAT_INSTANCE.status.interrupt_signal = false; // clear any flag left by a previous interrupt

                    //SIDETRACK_CHAT_INSTANCE.send("Review the conversation that just finished. Identify any potential misunderstandings, missed user intents, or areas where the assistant's response could have been improved. Provide a concise analysis of what could be done better in future interactions.", "system");
                    // NOTE: this is a direct, BLOCKING call - not launched
                    // on its own thread like the main chat's sends are. The
                    // sidetrack thread sits here for the entire duration of
                    // the model's response (can be several seconds), unable
                    // to do anything else, including notice a fresh
                    // interrupt (see the check above).
                    SIDETRACK_CHAT_INSTANCE.send("You are the 'Internal Monologue' of the assistant. Review the turn "
                        "that just ended. If there are technical details, edge cases, or deeper insights that were "
                        "missed for the sake of brevity, provide them now. "

                        "IMPORTANT: You are shown the whole conversation, not just the turn that just ended. If you "
                        "(this Internal Monologue) already raised a point in an earlier note here, do not raise it "
                        "again - only speak up if you have something genuinely new to add about the turn that just "
                        "ended specifically. "

                        "CRITICAL: Speak directly to the user as if you just had a 'lightbulb moment.' Do not use "
                        "phrases like 'The assistant should have...' or 'Analysis shows...' "

                        //"Format: Start immediately with the additional info or a follow-up thought. If the previous "
                        //"response was truly sufficient, respond with only '[FIN].'");

                        "Format: Start immediately with the additional info or a follow-up thought. If the previous "
                        "response was truly sufficient, respond with ONLY the word DONE. "
                        "Do not use brackets, do not use periods, do not say anything else.",
                        // Explicit role, not the default "user" - this is
                        // scaffolding prompting the model to review itself,
                        // not something a human typed. Left as the default
                        // before, it was stored in SIDETRACK_CHAT_INSTANCE's
                        // own history as role "user", structurally
                        // indistinguishable from real user input.
                        "system");

                    bool dummy_enable_keyboard_input = false; // This routine does not require keyboard input, but we pass the variable to satisfy the function signature.


                    // send() above already blocked until the response was
                    // complete (or aborted), so by the time execution
                    // reaches here the network call is already done. Note
                    // SIDETRACK_CHAT_INSTANCE.is_processing is never true in
                    // this loop - send() only ever sets that flag when it's
                    // launched on its own thread (as the main chat does),
                    // which this call isn't. So this loop is really just
                    // draining tts_buffer via process() calls (a handful of
                    // iterations at most), not "waiting for streaming."
                    while (SIDETRACK_CHAT_INSTANCE.is_processing || !SIDETRACK_CHAT_INSTANCE.tts_buffer.empty())
                    {
                        if (starts_with(SIDETRACK_CHAT_INSTANCE.last_received.response, "DONE"))
                        {
                            SECOND_GUESS_PROCESSING_STAGE = 4;
                            break;
                        }

                        // nullptr, not the real CLASS_SYSTEM: this runs on
                        // the sidetrack background thread, not the main
                        // thread - see TOOL_BASE::check()'s comment in
                        // tools.h for why every other call site can pass the
                        // real one and this one specifically can't.
                        SIDETRACK_CHAT_INSTANCE.process(nullptr, dummy_enable_keyboard_input);
                    }

                    // Reaching here without hitting the DONE break above
                    // means the review said something worth keeping (or
                    // was interrupted mid-generation) - stage 3 is where
                    // check() decides whether it actually has anything new
                    // to fold in.
                    if (SECOND_GUESS_PROCESSING_STAGE != 4)
                    {
                        SECOND_GUESS_PROCESSING_STAGE = 3;
                    }
                }

                if (SECOND_GUESS_PROCESSING_STAGE == 4)
                {
                    //std::cout << "Sidetrack: Post-chat review complete. Wrapping up." << std::endl;

                    SIDETRACK_CHAT_INSTANCE.history.clear();
                    SIDETRACK_CHAT_INSTANCE.tts_buffer.clear();

                    SECOND_GUESS_PROCESSING_STAGE = 0;
                    ROUTINE = 0;
                }

            }

            // Context Clear - see CLEAR_CONTEXT_STAGE's comments in
            // sidetrack.h for what each stage means and which thread
            // handles it.
            if (ROUTINE == 3)
            {
                if (CLEAR_CONTEXT_STAGE == 0)
                {
                    //std::cout << "Sidetrack: Requesting context clear." << std::endl;
                    CLEAR_CONTEXT_STAGE = 1;
                }

                //if (CLEAR_CONTEXT_STAGE == 1)
                // Handled in the check function

                if (CLEAR_CONTEXT_STAGE == 2)
                {
                    //std::cout << "Sidetrack: Context clear complete. Wrapping up." << std::endl;
                    // NOT reset to 0 here - see stage 3's comment in
                    // sidetrack.h for why this stays "finished" until an
                    // interrupt resets it.
                    CLEAR_CONTEXT_STAGE = 3;
                    ROUTINE = 0;
                }

                // Re-armed every tick this routine is active, same as
                // IDLE_WAIT_TIMER_FOR_CONSOLIDATION above.
                IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR.set(thread_time.current_frame_time(), IDLE_WAIT_TIME_FOR_CONTEXT_CLEAR);
            }

            PROCESSING.store(false);
        }

        //sleep thread
        thread_time.request_ready_time(frame_limit.get_ready_time());
        thread_time.sleep_till_next_frame();
    }
    std::cout << "Sidetrack Thread Ended" << std::endl;
}

void SIDETRACK_CLASS::thread_start()
{
    {
        THREAD_CONTROL.create(1000);
        // Start the camera update on a separate thread.
        // This call is non-blocking, so the main loop can continue immediately.
        THREAD_CONTROL.start_render_thread([&]() 
                  {  thread_main();  });
    }
}

void SIDETRACK_CLASS::thread_stop()
{
    // Signal the loop to exit, then actually wait for the background
    // thread to finish before returning. If it's mid-way through a
    // blocking LLM call (consolidation or the second-guess review), that
    // call runs to completion first. Returning early here (as before) let
    // callers proceed to destroy this object - and everything it
    // references - while the thread was still running against it, which
    // caused a heap-corruption crash on shutdown.
    RUN = false;
    THREAD_CONTROL.wait_for_thread_to_finish();
}

/**
 * Runs on the MAIN thread - called once per main-loop tick, right after
 * chat.process() (see main.cpp). This is the only place allowed to read or
 * write main_instance.history directly, so it handles every *_PROCESSING_
 * STAGE step that touches the real conversation, plus consuming SIGNALS
 * (see sidetrack.h) into their atomic counterparts for the sidetrack
 * background thread to see.
 */
void SIDETRACK_CLASS::check(ollama_system& main_instance)
{
    if (SIGNALS.INTERUPT_SIGNAL)
    {
        INTERUPT.store(true);
        SIGNALS.INTERUPT_SIGNAL = false;

        // The sidetrack thread can't notice an interrupt itself while it's
        // blocked inside SIDETRACK_CHAT_INSTANCE.send() - only this (main)
        // thread can. This actually aborts an in-flight second-guess LLM
        // call instead of just letting it run to completion.
        SIDETRACK_CHAT_INSTANCE.stop();
    }

    if (SIGNALS.CHAT_FINISHED_SIGNAL)
    {
        CHAT_FINISHED.store(true);
        SIGNALS.CHAT_FINISHED_SIGNAL = false;
    }

    // ----

    // Consolidation Routine
    {
        // Snapshot the real history for consolidate() (sidetrack thread,
        // stage 2) to work on. Locked: this (main_instance.history) is the
        // same vector chat_thread's own send() (olla.cpp) pushes into under
        // history_mutex - check() runs on the main thread, but chat_thread
        // is a genuinely separate, concurrently-running thread for as long
        // as a response is still streaming (ollama_system::input()'s poll
        // loop doesn't join it until completion), so an unlocked read/write
        // here would race a live send() call. Real, not theoretical - see
        // git history for how this was found.
        if (CHAT_HISTORY_PROCESSING_STAGE == 1)
        {
            {
                std::lock_guard<std::mutex> lock(history_mutex);
                temp_chat_history = main_instance.history;
            }
            CHAT_HISTORY_PROCESSING_STAGE = 2;
        }

        // consolidate() finished with a real change to commit. Unlike the
        // second-guess routine (see SIDETRACK_CHAT_INSTANCE.stop() above),
        // consolidation's own LLM call is never actually aborted mid-flight
        // if the user interacts while it's running - it always runs to
        // completion. This is the "abandon and revert" part instead: if an
        // interrupt happened at any point during the pass, INTERUPT is
        // still true here, and the result is simply discarded rather than
        // written back - by design, so a change never lands mid-interaction.
        if (CHAT_HISTORY_PROCESSING_STAGE == 3)
        {
            if (INTERUPT.load() == false)
            {
                // Lock scoped to just the assignment, not save_history() -
                // history_write() (olla.cpp) takes history_mutex itself to
                // read history back out, and it's a plain (non-recursive)
                // std::mutex, so holding this across that call would
                // deadlock the main thread against itself.
                {
                    std::lock_guard<std::mutex> lock(history_mutex);
                    main_instance.history = temp_chat_history;
                }
                main_instance.save_history();
            }
            CHAT_HISTORY_PROCESSING_STAGE = 4;
        }
    }

    // Second Guess Routine
    {
        // Snapshot the real history as context for the review prompt
        // (sidetrack thread, stage 2, seeds SIDETRACK_CHAT_INSTANCE with it).
        // Locked - see the consolidation snapshot's comment above.
        if (SECOND_GUESS_PROCESSING_STAGE == 1)
        {
            {
                std::lock_guard<std::mutex> lock(history_mutex);
                temp_chat_history = main_instance.history;
            }
            SECOND_GUESS_PROCESSING_STAGE = 2;
        }

        // Reached either because the review had something to say (normal
        // path out of stage 2), or forced here directly by an interrupt
        // (thread_main's SIGNALS.INTERUPT_SIGNAL check, or the stop() call
        // above in this function).
        if (SECOND_GUESS_PROCESSING_STAGE == 3)
        {
            // Fold the review's reply into the main conversation only if
            // this cycle actually generated one. send() always pushes its
            // own prompt onto SIDETRACK_CHAT_INSTANCE.history immediately,
            // before generation even starts, so a non-empty history alone
            // doesn't mean anything new was said - it could just be that
            // prompt, or a stale reply left over from an earlier cycle that
            // this one never got the chance to replace (e.g. interrupted
            // before any content was generated). last_received.response is
            // reset at the top of every send() call, so a non-empty value
            // here reliably means this specific call produced real content
            // (complete or partial/interrupted, either way worth keeping).
            if (!SIDETRACK_CHAT_INSTANCE.last_received.response.empty())
            {
                // Locked - history_mutex is a single mutex shared by every
                // ollama_system instance (olla.h), so this also covers the
                // SIDETRACK_CHAT_INSTANCE.history read right below: the
                // sidetrack thread's own send() calls push into that vector
                // under the same lock.
                std::lock_guard<std::mutex> lock(history_mutex);
                main_instance.history.push_back(SIDETRACK_CHAT_INSTANCE.history.back());
            }
            SECOND_GUESS_PROCESSING_STAGE = 4;
        }
    }

    // Context Clear Routine
    {
        if (CLEAR_CONTEXT_STAGE == 1)
        {
            // Same "abandon if interrupted" rule as consolidation's stage 3
            // above - if the user did anything in the meantime, INTERUPT is
            // already true here, and the clear is simply skipped rather
            // than wiping history out from under a conversation that just
            // became active again.
            if (INTERUPT.load() == false)
            {
                // Locked - see the consolidation snapshot's comment further
                // up for why (same main_instance.history, same chat_thread
                // race), scoped to just the read+reassignment, not
                // save_history() (history_write() takes this same lock
                // itself).
                {
                    std::lock_guard<std::mutex> lock(history_mutex);
                    std::vector<Message> protected_messages;
                    for (const Message& msg : main_instance.history) {
                        if (msg.consolidation_level < 0) {
                            protected_messages.push_back(msg);
                        }
                    }
                    main_instance.history = protected_messages;
                }
                main_instance.save_history();

                // Tell main.cpp to close the chat log too - see
                // SIDETRACK_SIGNALS::CONTEXT_CLEARED_SIGNAL's comment.
                SIGNALS.CONTEXT_CLEARED_SIGNAL = true;
            }
            CLEAR_CONTEXT_STAGE = 2;
        }
    }
}

void SIDETRACK_CLASS::pull_output(OUTPUT_CLASS& output)
{
    // Same pull-and-clear shape as OUTPUT_CLASS::get_response() (inlined
    // here instead of just calling it, so this can lock output_buffer_mutex
    // once rather than double-locking it) with one addition: once the
    // review's response settles on a lone "DONE" (see the prompt in
    // check()'s SECOND_GUESS_PROCESSING_STAGE==2 block, and the matching
    // starts_with() check further down in this file - it means the review
    // found nothing worth adding), that's not a real reply worth cluttering
    // the permanent chat transcript with. Route it into chat_thinking
    // instead of chat_response - same place the rest of the review's
    // reasoning already goes, where it disappears once real content
    // arrives, rather than sitting in the transcript as a stray "DONE."
    // line (see the TODO.md entry this replaces, "Filter DONE-only
    // responses from display").
    std::lock_guard<std::mutex> lock(output_buffer_mutex);

    if (starts_with(SIDETRACK_CHAT_INSTANCE.response_buffer, "DONE"))
    {
        output.chat_thinking += SIDETRACK_CHAT_INSTANCE.response_buffer;
    }
    else
    {
        output.chat_response += SIDETRACK_CHAT_INSTANCE.response_buffer;
    }
    SIDETRACK_CHAT_INSTANCE.response_buffer.clear();

    output.chat_thinking += SIDETRACK_CHAT_INSTANCE.thinking_buffer;
    SIDETRACK_CHAT_INSTANCE.thinking_buffer.clear();

    output.system_message += SIDETRACK_CHAT_INSTANCE.log_buffer;
    SIDETRACK_CHAT_INSTANCE.log_buffer.clear();
}


#endif