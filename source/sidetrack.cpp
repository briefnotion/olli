#ifndef SIDETRACK_CPP
#define SIDETRACK_CPP

#include "sidetrack.h"

// Starts SIDETRACK_CHAT_INSTANCE generating a reply to whatever's currently
// sitting in comms.INPUT_FROM_USER, using its own chat_thread - same
// mechanism ollama_system::input() uses for the main chat (olla.cpp) - so
// run_second_guess() can poll for completion instead of blocking. Only
// entered from a stage that already set INPUT_FROM_USER. comms here is the
// real main chat's own COMMS (passed through from check()), not a separate
// one - see run_second_guess()'s own comment for the tradeoff that implies.
static void start_second_guess_call(ollama_system& instance, COMMS& comms,
                                     std::vector<std::unique_ptr<TOOL_BASE>>& tools_list)
{
    instance.status.interrupt_signal = false;
    instance.is_processing = true;
    if (instance.chat_thread.joinable()) instance.chat_thread.join();
    instance.chat_thread = std::thread([&instance, &tools_list, &comms]()
    {
        instance.send(tools_list, comms, "user");
        instance.is_processing = false;
    });
}

// Dispatches any tool calls the last call produced - same shape
// ollama_system::process()'s own background_tasks handling uses for
// exactly this kind of tick-spanning secondary instance (olla.cpp, PART 2)
// - joins the thread once it's done, and reports whether this round is
// truly finished: not just "the network call returned," but also "no tool
// call is still sitting there waiting to be dispatched or narrated."
static bool poll_second_guess_call(ollama_system& instance, COMMS& comms,
                                    std::vector<std::unique_ptr<TOOL_BASE>>& tools_list,
                                    CLASS_SYSTEM* system, std::atomic<bool>& keyboard_enabled)
{
    // comms is the real main chat's own - unlike an isolated comms, its
    // INTERRUPTED is never ours to clear: the real consumer
    // (ollama_system::input(), gated on is_processing, olla.cpp) owns
    // that. Only read it here to decide whether to stop SIDETRACK_CHAT_
    // INSTANCE's own in-flight call - clearing it ourselves too could race
    // with that and suppress a real interrupt to the main chat.
    if (comms.INTERRUPTED)
    {
        instance.stop();
        debug_log_instance_event("sidetrack-second-guess", "interrupted mid-response - stopping");
    }

    instance.handle_instance_tools(system, tools_list, comms, keyboard_enabled);

    if (!instance.is_processing && instance.chat_thread.joinable())
    {
        instance.chat_thread.join();
    }

    return !instance.is_processing && instance.last_received.complete && instance.last_received.tool_calls.empty();
}

void SIDETRACK_CLASS::run_second_guess(ollama_system& main_instance, COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, CLASS_SYSTEM* system)
{
    // A finished review (stage 100) otherwise has no way back to 0 - a new
    // ASSISTANT REPLY completing is what should make that happen, same idea
    // as check()'s own shared history-size reset for consolidation/context-
    // clear, just self-contained here instead of shared with those. Not
    // just "history grew": send() (olla.cpp) pushes the user's own
    // submitted message onto history immediately, well before the
    // assistant's reply is pushed once it actually finishes streaming - a
    // bare size check fired on that first growth, triggering second-guess
    // the instant the user hit enter instead of after olli actually
    // responded. Checking the newest message's role filters that out.
    //
    // A real reply and second-guess's own committed follow-up (stage 6) are
    // both "assistant"-role messages, indistinguishable by role alone - so
    // this can restart itself, chaining off its own last answer. Allowed on
    // purpose (second-guessing its own second-guess is fine), but capped by
    // second_guess_chain_count so it can't chain forever. Any genuine new
    // "user" message resets the chain - that's the only thing that proves a
    // human, not the review itself, is what's driving the conversation now.
    if (main_instance.history.size() != SECOND_GUESS_PREVIOUS_HISTORY_SIZE)
    {
        std::string newest_role;
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            if (!main_instance.history.empty()) newest_role = main_instance.history.back().role;
        }

        if (newest_role == "user")
        {
            second_guess_chain_count = 0;
        }
        else if (newest_role == "assistant" && second_guess_stage == 100)
        {
            if (second_guess_chain_count < SECOND_GUESS_MAX_CHAIN)
            {
                ++second_guess_chain_count;
                second_guess_stage = 0; // a real reply just landed - ready to review it
            }
            else
            {
                debug_log_instance_event("sidetrack-second-guess",
                    "chain limit (" + std::to_string(SECOND_GUESS_MAX_CHAIN) + ") reached - parked until a real user turn");
            }
        }
    }
    SECOND_GUESS_PREVIOUS_HISTORY_SIZE = main_instance.history.size();

    // comms.INTERRUPTED means "abort in-flight generation/speech" (comms.h)
    // - read-only here, deliberately never cleared: the real consumer
    // (ollama_system::input(), olla.cpp, gated on is_processing) owns
    // clearing it. Clearing it here too could race with that and suppress
    // a real interrupt to the main chat. This is the same comms the real
    // chat uses for its own submissions - reused rather than an isolated
    // one, on the accepted risk that second-guess's own send() calls
    // (which read/write comms.INPUT_FROM_USER too) could in principle
    // collide with a real user submission landing in the same window.
    if (second_guess_stage < 2 && comms.INTERRUPTED)
    {
        if (second_guess_stage != 0)
        {
            debug_log_instance_event("sidetrack-second-guess", "aborted before starting - main chat interrupted");
        }
        second_guess_stage = 0; // abort - main chat got interrupted before we even started
        return;
    }

    if (second_guess_stage == 0)
    {
        SECOND_GUESS_WAIT_TIMER.set(SECOND_GUESS_WAIT_TIME);
        second_guess_stage = 1;
    }
    else if (second_guess_stage == 1)
    {
        if (SECOND_GUESS_WAIT_TIMER.is_ready())
        {
            second_guess_stage = 2; // ready to start the review
        }
    }
    else if (second_guess_stage == 2)
    {
        // Dumbest-form throwaway instance, same reasoning as consolidation's
        // (run_consolidation()'s own comment) - except thinking mode is on
        // here, and it gets the real tools_list (passed in, not
        // populate_default_tools()'s throwaway set) so it's actually
        // capable of doing something about what it decides needs doing,
        // not just talking about it.
        SIDETRACK_CHAT_INSTANCE.PROPS.model = main_instance.PROPS.model;
        SIDETRACK_CHAT_INSTANCE.PROPS.host = main_instance.PROPS.host;
        SIDETRACK_CHAT_INSTANCE.PROPS.port = main_instance.PROPS.port;
        SIDETRACK_CHAT_INSTANCE.PROPS.use_thinking = true;
        SIDETRACK_CHAT_INSTANCE.PROPS.LOAD_SAVE_HISTORY_ON_DISK = false;

        // stream_output/stream_thinking are independent gates on
        // comms.INPUT_FROM_LLM/THINKING (olla.h/olla.cpp) - Ollama's
        // streaming API still runs either way, so thinking still shows up
        // live here, but this call's own plain-text "DONE" answer never
        // reaches comms.INPUT_FROM_LLM at all, so there's nothing to
        // suppress afterward (by the time the call completes, anything
        // that WAS written to comms has already reached the screen live -
        // see the standalone discussion on this). last_received.response
        // still gets the full answer regardless, for the DONE check below.
        SIDETRACK_CHAT_INSTANCE.PROPS.stream_output = false;
        SIDETRACK_CHAT_INSTANCE.PROPS.stream_thinking = true;

        // Distinguishes this instance's own send()-logged messages
        // (debug_log_message(), called unconditionally inside send(),
        // olla.cpp) from consolidation's use of the same
        // SIDETRACK_CHAT_INSTANCE in debug_full_history.txt - both used to
        // log under one generic "sidetrack" label, impossible to tell apart.
        SIDETRACK_CHAT_INSTANCE.debug_label = "sidetrack-second-guess";
        debug_log_instance_event("sidetrack-second-guess", "review started");

        SIDETRACK_CHAT_INSTANCE.clear_history();
        Message task_note;
        task_note.role = "system";
        task_note.content = "You are reviewing your own last response to the user. Decide whether "
                             "anything more needs to be said or done, or whether it was already complete.";
        task_note.consolidation_level = -1;
        SIDETRACK_CHAT_INSTANCE.history.push_back(task_note);

        comms.INPUT_FROM_USER = "More needed to be done or said? Respond DONE if not.";
        start_second_guess_call(SIDETRACK_CHAT_INSTANCE, comms, tools_list);

        second_guess_stage = 3;
    }
    else if (second_guess_stage == 3)
    {
        // Waiting on the first ("is there more?") call - including
        // dispatching/narrating any tool call it decides to make (e.g.
        // actually checking whether a light it claimed was turned on
        // really is).
        if (!poll_second_guess_call(SIDETRACK_CHAT_INSTANCE, comms, tools_list, system, second_guess_keyboard_enabled))
        {
            return; // still working - try again next tick, do nothing else this one
        }
        second_guess_stage = 4;
    }
    else if (second_guess_stage == 4)
    {
        // Trim leading whitespace/newlines (thinking-mode responses often
        // have some) before checking for the DONE marker.
        std::string answer = SIDETRACK_CHAT_INSTANCE.last_received.response;
        size_t first_non_space = answer.find_first_not_of(" \t\r\n");
        if (first_non_space != std::string::npos) answer = answer.substr(first_non_space);

        if (!SIDETRACK_CHAT_INSTANCE.last_received.complete)
        {
            debug_log_instance_event("sidetrack-second-guess", "interrupted during the DONE check - nothing to add");
            second_guess_stage = 100;
        }
        else if (starts_with(answer, "DONE"))
        {
            debug_log_instance_event("sidetrack-second-guess", "DONE - nothing more needed");
            second_guess_stage = 100;
        }
        else
        {
            debug_log_instance_event("sidetrack-second-guess", "not done - asking it to say/do what's needed");
            // Streaming back on for this one - this is the real content the
            // user should actually see/hear, unlike the DONE-check above.
            SIDETRACK_CHAT_INSTANCE.PROPS.stream_output = true;
            comms.INPUT_FROM_USER = "Go ahead - say or do what needs to happen.";
            start_second_guess_call(SIDETRACK_CHAT_INSTANCE, comms, tools_list);
            second_guess_stage = 5;
        }
    }
    else if (second_guess_stage == 5)
    {
        // Waiting on the second ("say/do it") call - same shape as stage 3.
        if (!poll_second_guess_call(SIDETRACK_CHAT_INSTANCE, comms, tools_list, system, second_guess_keyboard_enabled))
        {
            return;
        }
        second_guess_stage = 6;
    }
    else if (second_guess_stage == 6)
    {
        // Commit whatever came back onto the real conversation. An
        // interrupted/incomplete answer still gets kept, marked with "..."
        // to show it was cut short, rather than discarded outright.
        std::string answer = SIDETRACK_CHAT_INSTANCE.last_received.response;
        if (!SIDETRACK_CHAT_INSTANCE.last_received.complete)
        {
            answer += "...";
            debug_log_instance_event("sidetrack-second-guess", "interrupted mid-answer - keeping partial response");
        }

        if (!answer.empty())
        {
            Message followup;
            followup.role = "assistant";
            followup.content = answer;
            followup.consolidation_level = 0;
            {
                std::lock_guard<std::mutex> lock(history_mutex);
                main_instance.history.push_back(followup);
            }
            main_instance.save_history();
            debug_log_instance_event("sidetrack-second-guess", "committed follow-up to main history");
        }
        else
        {
            debug_log_instance_event("sidetrack-second-guess", "empty answer - nothing committed");
        }

        second_guess_stage = 100;
    }

    // Thinking is never saved anywhere - SIDETRACK_CHAT_INSTANCE.last_received.
    // thinking never reaches main_instance.history. It does still reach the
    // screen: comms.INPUT_FROM_THINKING is the same field IO_WORKER_CLASS
    // already drains for the main chat (exchange()/thread_main(),
    // io_worker.cpp) - since second-guess reuses that same comms rather
    // than a separate one, no extra wiring is needed for it to show up
    // there too.
}


// Runs every PERSISTENT_CHECK_INTERVAL regardless of activity - no stage
// machine, just a periodic safety check. If history has grown too large
// (e.g. because consolidation isn't keeping up), wipe it the same way
// run_clear_context() does.
void SIDETRACK_CLASS::persistent_time_checks(ollama_system& main_instance)
{
    if (PERSISTENT_CHECK_TIMER.is_ready())
    {
        PERSISTENT_CHECK_TIMER.set(PERSISTENT_CHECK_INTERVAL);

        bool too_big = false;
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            too_big = main_instance.history.size() > MAX_CONTEXT_SIZE;
        }

        if (too_big)
        {
            // Same wipe as run_clear_context() - keep only protected
            // (consolidation_level < 0) messages.
            main_instance.clear_history_keep_protected();
            main_instance.save_history();
        }
    }
}

void SIDETRACK_CLASS::run_consolidation(ollama_system& main_instance)
{
    if (consolidation_stage == 0)
    {
        IDLE_WAIT_TIMER_FOR_CONSOLIDATION.set(IDLE_WAIT_TIME_FOR_CONSOLIDATION);
        consolidation_stage = 1;
    }
    else if (consolidation_stage == 1)
    {
        if (IDLE_WAIT_TIMER_FOR_CONSOLIDATION.is_ready())
        {
            consolidation_stage = 2; // ready to consolidate
        }
    }
    else if (consolidation_stage == 2)
    {
        // Working set copied out of main_instance.history - read-only source
        // for everything below, thrown away when this stage finishes. Never
        // mutated in place; consolidated_history (built further down) is
        // the only thing that ever gets written back.
        std::vector<Message> working_history;
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            working_history = main_instance.history;
        }

        // Copy over only what a throwaway summarizer needs to hit the same
        // model/server as the real conversation - not a blind PROPS = ...
        // struct copy. A full copy would also drag over LOAD_SAVE_HISTORY_
        // ON_DISK=true and the real OLLI_DIRECTORY, silently overwriting the
        // real history.json with this scratch instance's own history - the
        // exact bug documented in the old create()'s comment above (the #if
        // 0 block). SIDETRACK_CHAT_INSTANCE stays in its dumbest form here:
        // no thinking, no streaming, no disk saves, no tools.
        SIDETRACK_CHAT_INSTANCE.PROPS.model = main_instance.PROPS.model;
        SIDETRACK_CHAT_INSTANCE.PROPS.host = main_instance.PROPS.host;
        SIDETRACK_CHAT_INSTANCE.PROPS.port = main_instance.PROPS.port;
        SIDETRACK_CHAT_INSTANCE.PROPS.use_thinking = false;
        SIDETRACK_CHAT_INSTANCE.PROPS.stream_output = false;
        SIDETRACK_CHAT_INSTANCE.PROPS.LOAD_SAVE_HISTORY_ON_DISK = false;

        // Distinguishes this instance's own send()-logged messages
        // (debug_log_message(), called unconditionally inside send(),
        // olla.cpp) from second-guess's use of the same
        // SIDETRACK_CHAT_INSTANCE in debug_full_history.txt - both used to
        // log under one generic "sidetrack" label, impossible to tell apart.
        SIDETRACK_CHAT_INSTANCE.debug_label = "sidetrack-consolidate";
        debug_log_instance_event("sidetrack-consolidate", "pass started");

        // Fresh scratch history, seeded with one protected (level -1)
        // instruction message - the "persona" for this throwaway instance,
        // survives every clear_history_keep_protected() call below so each
        // level's consolidation pass starts from the same instructions
        // without re-stating them.
        SIDETRACK_CHAT_INSTANCE.clear_history();
        {
            Message consolidation_instructions;
            consolidation_instructions.role = "system";
            consolidation_instructions.content =
                "You are a summarization assistant. You will be given a batch "
                "of conversation messages. Merge them into a single concise "
                "message that preserves the key facts, decisions, and the "
                "user's overall intent. Respond with only the summary - no "
                "preamble, no meta-commentary.";
            consolidation_instructions.consolidation_level = -1;
            SIDETRACK_CHAT_INSTANCE.history.push_back(consolidation_instructions);
        }

        // 1. Bucket every message by consolidation_level - protected
        //    (level < 0) messages are set aside untouched, same as
        //    clear_history_keep_protected() treats them elsewhere.
        std::vector<Message> protected_messages;
        std::vector<std::vector<Message>> levels;
        for (const Message& msg : working_history) {
            if (msg.consolidation_level < 0) {
                protected_messages.push_back(msg);
                continue;
            }
            size_t level = static_cast<size_t>(msg.consolidation_level);
            if (level >= levels.size()) levels.resize(level + 1);
            levels[level].push_back(msg);
        }
        levels.resize(levels.size() + 1); // headroom for a promotion out of the top level

        // 2. Walk levels oldest-tier-first. A level only gets touched once
        //    it holds more than keep_count messages AND that overflow is at
        //    least trigger_count - at which point the ENTIRE overflow
        //    (however large) is fed to SIDETRACK_CHAT_INSTANCE as one
        //    prompt and squashed into a single summary message, promoted to
        //    the next level up. Not a fixed-size sliding window like the
        //    old implementation - one summary per trigger, whatever the
        //    overflow count happens to be. The newest keep_count messages
        //    in the level are left alone.
        size_t keep_count = static_cast<size_t>(main_instance.PROPS.consolitation_starts_starts_at);
        size_t trigger_count = static_cast<size_t>(main_instance.PROPS.consolitation_sizes);

        // No tools, blank comms - this instance never calls a tool or
        // speaks/displays anything, it only ever answers one summarization
        // prompt at a time.
        std::vector<std::unique_ptr<TOOL_BASE>> no_tools;
        COMMS blank_comms;

        for (size_t level = 0; level + 1 < levels.size(); ++level)
        {
            if (levels[level].size() <= keep_count) continue;

            size_t overflow_count = levels[level].size() - keep_count;
            if (overflow_count < trigger_count) continue;

            // Level 0 holds raw conversation, where a "turn" isn't always a
            // fixed 2-message user/assistant pair - a tool-call exchange
            // (user, assistant, tool, DIRECTOR_NOTE, assistant) is 5. An
            // even-count rule assumes strict 2-message alternation and
            // breaks the first time a tool call appears anywhere earlier in
            // the level - the parity shift can still leave the slice ending
            // on a dangling question even at an even count (seen for real:
            // rounding to even alone wasn't enough once a tool exchange was
            // in the mix - see the standalone test's notes). So shrink by
            // content instead of count: never let the slice end on
            // anything but a completed "assistant" turn - a trailing user
            // question, tool result, or DIRECTOR_NOTE all mean "still
            // waiting for a reply," and leaving one dangling confused the
            // model in testing. Levels above 0 hold only our own past
            // promoted summaries (always "system", each a complete
            // standalone unit) - no such pairing exists there, so this only
            // applies to level 0.
            if (level == 0) {
                while (overflow_count > 0 && levels[level][overflow_count - 1].role != "assistant") {
                    --overflow_count;
                }
                if (overflow_count < trigger_count) continue; // shrank below the trigger - wait for more
            }

            // Oldest overflow_count messages in this level are the ones
            // being squashed - the newest keep_count stay at this level.
            // Reset back to just the protected instruction message, then
            // replay these messages onto SIDETRACK_CHAT_INSTANCE's own
            // history so the model reads a genuine multi-turn transcript
            // instead of one flattened text blob - with two exceptions,
            // both because "tool" and DIRECTOR_NOTE messages don't mean
            // anything replayed out of their original context: a "tool"
            // message has no meaning to the API without the preceding
            // assistant tool_calls entry we're not replaying, so it gets
            // relabeled to "user" with a plain-text marker instead; a
            // DIRECTOR_NOTE (also "system") is redundant with the "tool"
            // message right before it (integrate_tool_result(), olla.cpp,
            // embeds the exact same raw result verbatim) so it's dropped
            // rather than doubling up on the same fact. Anything else
            // tagged "system" here is our own past promoted summary, not a
            // DIRECTOR_NOTE - replayed unchanged, same as user/assistant.
            // Safe without history_mutex here: this instance's history has
            // no concurrent writer - everything in this stage runs
            // synchronously on the main thread (see the still-open
            // sync-vs-threaded question).
            SIDETRACK_CHAT_INSTANCE.clear_history_keep_protected();
            for (size_t i = 0; i < overflow_count; ++i) {
                const Message& msg = levels[level][i];
                if (msg.role == "tool") {
                    Message flattened;
                    flattened.role = "user";
                    flattened.content = "[Tool result]: " + msg.content;
                    SIDETRACK_CHAT_INSTANCE.history.push_back(flattened);
                } else if (msg.role == "system" && starts_with(msg.content, "[DIRECTOR_NOTE]")) {
                    continue; // redundant with the "tool" message right before it
                } else {
                    SIDETRACK_CHAT_INSTANCE.history.push_back(msg);
                }
            }

            debug_log_instance_event("sidetrack-consolidate",
                "squashing " + std::to_string(overflow_count) + " messages at level " + std::to_string(level));

            blank_comms.INPUT_FROM_USER = "What happened in all your memory? Summarize it.";
            SIDETRACK_CHAT_INSTANCE.send(no_tools, blank_comms, "system");

            std::string summary_text = SIDETRACK_CHAT_INSTANCE.last_received.response;
            if (SIDETRACK_CHAT_INSTANCE.last_received.complete && !summary_text.empty())
            {
                levels[level].erase(levels[level].begin(), levels[level].begin() + static_cast<std::ptrdiff_t>(overflow_count));

                Message summary_msg;
                summary_msg.role = "system";
                summary_msg.content = "Summary of previous context: " + summary_text;
                summary_msg.consolidation_level = static_cast<int>(level) + 1;
                levels[level + 1].push_back(summary_msg);
            }
            else
            {
                // If the LLM call failed or produced nothing, leave this
                // level's messages untouched - it'll be retried next time
                // consolidation runs.
                debug_log_instance_event("sidetrack-consolidate",
                    "level " + std::to_string(level) + " squash failed/empty - left untouched, will retry later");
            }
        }

        // 3. Flatten back into one chronological vector: protected messages
        //    first, then oldest (highest level) down to newest (level 0) -
        //    same ordering the old consolidate() rebuilt.
        std::vector<Message> consolidated_history;
        consolidated_history.insert(consolidated_history.end(), protected_messages.begin(), protected_messages.end());
        for (size_t level = levels.size(); level-- > 0; ) {
            consolidated_history.insert(consolidated_history.end(), levels[level].begin(), levels[level].end());
        }

        main_instance.replace_history(consolidated_history);
        main_instance.save_history();

        debug_log_instance_event("sidetrack-consolidate",
            "pass finished - " + std::to_string(consolidated_history.size()) + " messages remain");

        consolidation_stage = 100;
    }
}

void SIDETRACK_CLASS::force_consolidation(ollama_system& main_instance)
{
    consolidation_stage = 2;
    run_consolidation(main_instance);
}

void SIDETRACK_CLASS::run_clear_context(ollama_system& main_instance)
{
    if (context_clear_stage == 0)
    {
        IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR.set(IDLE_WAIT_TIME_FOR_CONTEXT_CLEAR);
        context_clear_stage = 1;
    }
    else if (context_clear_stage == 1)
    {
        if (IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR.is_ready())
        {
            context_clear_stage = 2; // ready to clear context
        }
    }
    else if (context_clear_stage == 2)
    {
        // Wipe everything except protected (consolidation_level < 0)
        // messages - e.g. the persona/opening prompt. clear_history_keep_
        // protected() locks history_mutex itself: main_instance.history is
        // the same vector chat_thread's own send() (olla.cpp) pushes into
        // under that same mutex while a response is still streaming.
        main_instance.clear_history_keep_protected();
        main_instance.save_history();

        context_clear_stage = 100;
    }
}





void SIDETRACK_CLASS::create(OLLAMA_SYSTEM_PROPERTIES Properties)
{
    // Only needed transiently here, for open()'s own tool->configure(*this)
    // pass - not stored as a member (see SIDETRACK_CHAT_INSTANCE's own
    // comment in sidetrack.h). Whatever later calls .send()/.process() on
    // SIDETRACK_CHAT_INSTANCE builds its own local tools_list the same way.
    std::vector<std::unique_ptr<TOOL_BASE>> tools_list;
    populate_default_tools(tools_list);
    SIDETRACK_CHAT_INSTANCE.debug_label = "sidetrack";
    SIDETRACK_CHAT_INSTANCE.open(tools_list, Properties);

    IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR.set(IDLE_WAIT_TIME_FOR_CONTEXT_CLEAR);
    PERSISTENT_CHECK_TIMER.set(PERSISTENT_CHECK_INTERVAL);
}

void SIDETRACK_CLASS::check(ollama_system& main_instance, COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, CLASS_SYSTEM* system)
{
    // I'm trying to keep this function non blocking.

    // A real submission, tool result, or anything else that grows history
    // counts as activity - compared against the size as of the end of
    // last tick (see the update at the bottom of this function), so this
    // doesn't trip itself on the same tick run_clear_context() below does
    // its own wipe.
    if (main_instance.history.size() != PREVIOUS_HISTORY_SIZE)
    {
        consolidation_stage = 0;
        context_clear_stage = 0;
    }


    // Persistent Checks - runs on its own interval, independent of the
    // stages/activity-reset above.
    persistent_time_checks(main_instance);


    // Consolidation Routine
    run_consolidation(main_instance);



    // Clear Context Routine
    run_clear_context(main_instance);


    // Second Guess Routine
    run_second_guess(main_instance, comms, tools_list, system);


    // if all stages at 100, do not reset until something happens in main.

    PREVIOUS_HISTORY_SIZE = main_instance.history.size();
}




#endif