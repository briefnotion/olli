#ifndef SIDETRACK_CPP
#define SIDETRACK_CPP

#include "sidetrack.h"

// Small helper for building the "(Took action on review: ...)" fallback
// note run_second_guess() commits whenever a real tool call happened with
// no accompanying spoken text to fold it into - three separate exit points
// need the identical join, not worth its own header.
static std::string join_strings(const std::vector<std::string>& items, const std::string& separator)
{
    std::string joined;
    for (size_t i = 0; i < items.size(); ++i)
    {
        if (i > 0) joined += separator;
        joined += items[i];
    }
    return joined;
}

// Commits a plain "(Took action on review: ...)" note to the real
// conversation - the two early-exit branches in run_second_guess()'s stage
// 4 need this (the DONE-check call itself can still have dispatched a real
// tool call while forming its answer), same rule stage 6 applies to its
// own, richer commit path.
static void commit_action_only_note(ollama_system& main_instance, const std::vector<std::string>& actions)
{
    if (actions.empty()) return;

    Message action_note;
    action_note.role = "assistant";
    action_note.content = "(Took action on review: " + join_strings(actions, ", ") + ".)";
    action_note.consolidation_level = 0;
    {
        std::lock_guard<std::mutex> lock(history_mutex);
        main_instance.history.push_back(action_note);
    }
    main_instance.save_history();
}

// Starts SIDETRACK_CHAT_INSTANCE generating a reply to whatever's currently
// sitting in comms.INPUT_FROM_USER, using its own chat_thread - same
// mechanism ollama_system::input() uses for the main chat (olla.cpp) - so
// run_second_guess() can poll for completion instead of blocking. Only
// entered from a stage that already set INPUT_FROM_USER. comms here is the
// real main chat's own COMMS (passed through from check()), not a separate
// one - see run_second_guess()'s own comment for the tradeoff that implies.
// response_format forwards straight to ollama_system::send() - default-
// empty (plain text) for the stage 4 "go ahead" call, set for the stage 2
// DONE-check call (see SECOND_GUESS_RESULT_FORMAT below).
static void start_second_guess_call(ollama_system& instance, COMMS& comms,
                                     TOOL_WORKER_CLASS* tool_worker,
                                     const json& response_format = json())
{
    instance.status.interrupt_signal = false;
    instance.is_processing = true;
    if (instance.chat_thread.joinable()) instance.chat_thread.join();
    instance.chat_thread = std::thread([&instance, tool_worker, &comms, response_format]()
    {
        instance.send(tool_worker, comms, "user", response_format);
        instance.is_processing = false;
    });
}

// Structured-output schema for the stage 2 DONE-check call - forces the
// reply into this exact shape via Ollama's own constrained decoding
// (send()'s own comment, olla.h) instead of asking for a magic word and
// hoping it lands somewhere findable in free text. Replaces the old
// leading/trailing "DONE" text-matching, which real model replies (an
// explanation, then the marker at the END, not the start) kept missing -
// found live 2026-09-24, a real review self-chained all the way to
// SECOND_GUESS_MAX_CHAIN without a single one of its "DONE" answers ever
// being recognized as such.
static const json SECOND_GUESS_RESULT_FORMAT = {
    {"type", "object"},
    {"properties", {
        {"needs_correction", {{"type", "boolean"}}}
    }},
    {"required", json::array({"needs_correction"})}
};

// Dispatches any tool calls the last call produced - same shape
// ollama_system::process()'s own background_tasks handling uses for
// exactly this kind of tick-spanning secondary instance (olla.cpp, PART 2)
// - joins the thread once it's done, and reports whether this round is
// truly finished: not just "the network call returned," but also "no tool
// call is still sitting there waiting to be dispatched or narrated."
static bool poll_second_guess_call(IO_WORKER_CLASS& io_worker, ollama_system& instance, COMMS& comms,
                                    std::vector<std::unique_ptr<TOOL_BASE>>& tools_list,
                                    TOOL_WORKER_CLASS* tool_worker,
                                    CLASS_SYSTEM* system)
{
    // comms is the real main chat's own. ollama_system::input() (olla.cpp)
    // only clears INTERRUPTED when it's the one consuming it - gated on its
    // own is_processing, which is false the whole time second-guess is
    // running (it only starts once the main turn has already finished). So
    // a bare interrupt (no ENTER_PRESSED - e.g. a spoken interrupt phrase or
    // a keystroke) fired during this window never reaches that clear and
    // would dangle true otherwise. Safe to clear it here: main.cpp's loop
    // always runs chat.input() before sidetrack.check() each tick, so by
    // the time we see it as true, main already had its chance to consume it
    // this same tick - and every later is_processing transition on main's
    // side comes bundled with its own fresh ENTER_PRESSED clear regardless.
    if (comms.INTERRUPTED)
    {
        instance.stop();
        comms.INTERRUPTED = false;
        DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess", "interrupted mid-response - stopping");
    }

    instance.handle_instance_tools(io_worker, system, tools_list, tool_worker, comms);

    if (!instance.is_processing && instance.chat_thread.joinable())
    {
        instance.chat_thread.join();
    }

    // last_received.complete deliberately not checked here - an interrupted
    // call is still "over" (is_processing false, thread joined), it just
    // finished uncleanly. Stage 4/6 (run_second_guess) already handle a
    // !complete result explicitly; gating advancement on complete here
    // stalled stage 3/5 forever on any interrupt, since a call that ends
    // interrupted never becomes complete after the fact.
    return !instance.is_processing && instance.last_received.tool_calls.empty();
}

void SIDETRACK_CLASS::run_second_guess(IO_WORKER_CLASS& io_worker, ollama_system& main_instance, COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, CLASS_SYSTEM* system)
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
                DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess",
                    "chain limit (" + std::to_string(SECOND_GUESS_MAX_CHAIN) + ") reached - parked until a real user turn");
            }
        }
    }
    SECOND_GUESS_PREVIOUS_HISTORY_SIZE = main_instance.history.size();

    // comms.INTERRUPTED means "abort in-flight generation/speech" (comms.h).
    // Cleared here once consumed, same reasoning as poll_second_guess_call()'s
    // own comment above: ollama_system::input() (olla.cpp) only clears it
    // when gated on main's own is_processing, which is false throughout this
    // whole pre-start window, so a bare interrupt (no ENTER_PRESSED) would
    // otherwise dangle true indefinitely. This is the same comms the real
    // chat uses for its own submissions - reused rather than an isolated
    // one, on the accepted risk that second-guess's own send() calls
    // (which read/write comms.INPUT_FROM_USER too) could in principle
    // collide with a real user submission landing in the same window.
    if (second_guess_stage < 2 && comms.INTERRUPTED)
    {
        if (second_guess_stage != 0)
        {
            DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess", "aborted before starting - main chat interrupted");
        }
        second_guess_stage = 0; // abort - main chat got interrupted before we even started
        comms.INTERRUPTED = false;
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
        // here, and it gets the real tool_worker (passed in, not nullptr
        // like other sub-agent instances) so it's actually capable of doing
        // something about what it decides needs doing, not just talking
        // about it.
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
        // (DEBUG_LOG_CLASS::log_message(), called unconditionally inside send(),
        // olla.cpp) from consolidation's use of the same
        // SIDETRACK_CHAT_INSTANCE in debug_full_history.txt - both used to
        // log under one generic "sidetrack" label, impossible to tell apart.
        SIDETRACK_CHAT_INSTANCE.debug_label = "sidetrack-second-guess";
        DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess", "review started");

        SIDETRACK_CHAT_INSTANCE.clear_history();
        Message task_note;
        task_note.role = "system";
        task_note.content = "You are reviewing your own last response to the user, given fast without "
                             "much thought so it wouldn't be slow to reply. Check it for two things "
                             "only: was it factually accurate, and did you actually follow through on "
                             "anything you claimed to do (a tool call you said you made but didn't)? "
                             "If either is wrong, correct it. Do not introduce any new action the user "
                             "didn't ask for, and never override or second-guess a decision the user "
                             "has already explicitly made - their own most recent instruction always "
                             "wins, even if you think a different one would have been better.";
        task_note.consolidation_level = -1;
        SIDETRACK_CHAT_INSTANCE.history.push_back(task_note);

        // task_note above only describes what to do, not what's actually
        // being reviewed - without the real exchange, this call was judging
        // completeness of a response it had never seen, which produced
        // fabricated "follow-up" tool calls with no basis in the real
        // conversation (e.g. checking a timer label that was never
        // mentioned). Copy the real last turn in: walk back from the end of
        // main_instance.history to (and including) the last "user" message,
        // so the review gets the actual question, any tool activity in
        // between, and the actual reply - same idea as run_consolidation()'s
        // own working_history copy below, just scoped to one turn instead
        // of the whole conversation.
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            size_t start = main_instance.history.size();
            while (start > 0 && main_instance.history[start - 1].role != "user") --start;
            for (size_t i = start; i < main_instance.history.size(); ++i) {
                SIDETRACK_CHAT_INSTANCE.history.push_back(main_instance.history[i]);
            }
        }

        comms.INPUT_FROM_USER.set("Was your last reply accurate, and did you really do anything you said "
                                 "you did? Set needs_correction to true only if something was wrong or "
                                 "left unfinished; false if it was fine as-is.");
        start_second_guess_call(SIDETRACK_CHAT_INSTANCE, comms, tool_worker, SECOND_GUESS_RESULT_FORMAT);

        second_guess_stage = 3;
    }
    else if (second_guess_stage == 3)
    {
        // Waiting on the first ("is there more?") call - including
        // dispatching/narrating any tool call it decides to make (e.g.
        // actually checking whether a light it claimed was turned on
        // really is). Captured here, on the exact tick handle_instance_
        // tools() (inside poll_second_guess_call(), tools.cpp's own
        // is_ready_for_tools gate) is about to dispatch and clear
        // last_received.tool_calls - see second_guess_actions_taken's own
        // comment (sidetrack.h). Must match that gate's !is_processing
        // check exactly, not just last_received.complete: send() (olla.cpp)
        // sets complete=true before it returns, but is_processing only
        // flips false slightly later, once the spawned thread's next line
        // runs - during that narrow gap this would otherwise re-capture
        // the same names on every tick that lands in it, since neither
        // is_processing nor tool_calls has changed yet.
        if (!SIDETRACK_CHAT_INSTANCE.is_processing && SIDETRACK_CHAT_INSTANCE.last_received.complete && !SIDETRACK_CHAT_INSTANCE.last_received.tool_calls.empty())
        {
            for (const ToolCall& tc : SIDETRACK_CHAT_INSTANCE.last_received.tool_calls)
                second_guess_actions_taken.push_back(tc.name);
        }
        if (!poll_second_guess_call(io_worker, SIDETRACK_CHAT_INSTANCE, comms, tools_list, tool_worker, system))
        {
            return; // still working - try again next tick, do nothing else this one
        }
        second_guess_stage = 4;
    }
    else if (second_guess_stage == 4)
    {
        // Structured reply (SECOND_GUESS_RESULT_FORMAT, above) - guaranteed
        // valid-JSON-shaped by Ollama's own constrained decoding, not text
        // to search for a marker in. Replaces the old leading/trailing
        // "DONE" matching, which kept missing real replies (explanation
        // first, marker last - found live 2026-09-24, a review self-chained
        // 10 times straight without ever being recognized as done).
        // needs_correction defaults true on a parse failure - constrained
        // output should make that rare, and treating an unreadable answer
        // as "needs a look" is the safer failure direction on its own, but
        // SECOND_GUESS_MAX_CHAIN (sidetrack.h) still bounds the worst case
        // either way.
        bool needs_correction = true;
        try
        {
            json parsed = json::parse(SIDETRACK_CHAT_INSTANCE.last_received.response);
            needs_correction = parsed.at("needs_correction").get<bool>();
        }
        catch (const std::exception& e)
        {
            DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess", std::string("DONE-check reply wasn't valid structured JSON, treating as needs_correction: ") + e.what());
        }

        // Either exit here skips stage 6 (the DONE-check call itself can
        // still have dispatched a real tool call while forming its answer
        // - e.g. checking a timer before deciding "DONE" - see stage 3's
        // own comment) - same "a real action is never silent" rule stage 6
        // applies, just inlined here since there's no later stage to reach.
        if (!SIDETRACK_CHAT_INSTANCE.last_received.complete)
        {
            DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess", "interrupted during the DONE check - nothing to add");
            commit_action_only_note(main_instance, second_guess_actions_taken);
            second_guess_actions_taken.clear();
            second_guess_stage = 100;
        }
        else if (!needs_correction)
        {
            DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess", "DONE - nothing more needed");
            commit_action_only_note(main_instance, second_guess_actions_taken);
            second_guess_actions_taken.clear();
            second_guess_stage = 100;
        }
        else
        {
            DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess", "not done - asking it to say/do what's needed");
            // Streaming back on for this one - this is the real content the
            // user should actually see/hear, unlike the DONE-check above.
            SIDETRACK_CHAT_INSTANCE.PROPS.stream_output = true;
            comms.INPUT_FROM_USER.set("Go ahead - correct what was wrong, or actually follow through on "
                                     "what you already claimed. Nothing beyond that.");
            start_second_guess_call(SIDETRACK_CHAT_INSTANCE, comms, tool_worker);
            second_guess_stage = 5;
        }
    }
    else if (second_guess_stage == 5)
    {
        // Waiting on the second ("say/do it") call - same shape as stage 3,
        // including the same action-capture step (see its own comment,
        // including why !is_processing has to be checked too).
        if (!SIDETRACK_CHAT_INSTANCE.is_processing && SIDETRACK_CHAT_INSTANCE.last_received.complete && !SIDETRACK_CHAT_INSTANCE.last_received.tool_calls.empty())
        {
            for (const ToolCall& tc : SIDETRACK_CHAT_INSTANCE.last_received.tool_calls)
                second_guess_actions_taken.push_back(tc.name);
        }
        if (!poll_second_guess_call(io_worker, SIDETRACK_CHAT_INSTANCE, comms, tools_list, tool_worker, system))
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
            DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess", "interrupted mid-answer - keeping partial response");
        }

        // A real action (second_guess_actions_taken, sidetrack.h) must
        // never go unmentioned just because the accompanying spoken text
        // was empty - a tool this instance calls has real side effects
        // (the real tool_worker/comms, same as the main chat itself uses),
        // so silently taking one with nothing in the visible conversation
        // to explain it is the actual bug this whole block exists to
        // avoid. Falls back to a plain, honest note naming what was
        // called when there's no answer text to fold it into.
        if (answer.empty() && !second_guess_actions_taken.empty())
        {
            answer = "(Took action on review: " + join_strings(second_guess_actions_taken, ", ") + ".)";
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
            DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess", "committed follow-up to main history");
        }
        else
        {
            DEBUG_LOG_CLASS::instance().log_event("sidetrack-second-guess", "empty answer - nothing committed");
        }

        second_guess_actions_taken.clear();

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
        // Also gated on second_guess_stage == 100 (its fully-idle resting
        // state, run_second_guess()) - stage 2 below reuses the exact same
        // shared SIDETRACK_CHAT_INSTANCE run_second_guess() owns for the
        // whole span of its own review cycle, not just while its
        // background chat_thread is literally mid-request - the gap
        // between second-guess's own two calls (chat_thread finished, but
        // it hasn't yet decided on/started the next one) is just as
        // unsafe, since consolidation's own stage 2 clears/reconfigures
        // that instance synchronously, on this same main thread. Real
        // cross-thread collision confirmed live: a second-guess review's
        // own follow-up call ended up dispatched under consolidation's
        // clobbered state/label, with a real tool call - set_hue_light -
        // firing off already-corrupted context. No new mutex needed -
        // second_guess_stage only ever changes from this same main
        // thread's own tick, so a plain int comparison is enough to keep
        // the two from ever touching the instance at the same time.
        if (IDLE_WAIT_TIMER_FOR_CONSOLIDATION.is_ready() && second_guess_stage == 100)
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
        // (DEBUG_LOG_CLASS::log_message(), called unconditionally inside send(),
        // olla.cpp) from second-guess's use of the same
        // SIDETRACK_CHAT_INSTANCE in debug_full_history.txt - both used to
        // log under one generic "sidetrack" label, impossible to tell apart.
        SIDETRACK_CHAT_INSTANCE.debug_label = "sidetrack-consolidate";
        DEBUG_LOG_CLASS::instance().log_event("sidetrack-consolidate", "pass started");

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

            DEBUG_LOG_CLASS::instance().log_event("sidetrack-consolidate",
                "squashing " + std::to_string(overflow_count) + " messages at level " + std::to_string(level));

            blank_comms.INPUT_FROM_USER.set("What happened in all your memory? Summarize it.");
            SIDETRACK_CHAT_INSTANCE.send(nullptr, blank_comms, "system");

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
                DEBUG_LOG_CLASS::instance().log_event("sidetrack-consolidate",
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

        DEBUG_LOG_CLASS::instance().log_event("sidetrack-consolidate",
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
    // SIDETRACK_CHAT_INSTANCE gets the real tools_list (passed in via
    // check(), not this throwaway one) - see run_second_guess()'s own
    // comment for why.
    std::vector<std::unique_ptr<TOOL_BASE>> tools_list;
    populate_default_tools(tools_list);
    SIDETRACK_CHAT_INSTANCE.debug_label = "sidetrack";
    SIDETRACK_CHAT_INSTANCE.open(tools_list, Properties);

    IDLE_WAIT_TIMER_FOR_CONTEXT_CLEAR.set(IDLE_WAIT_TIME_FOR_CONTEXT_CLEAR);
    PERSISTENT_CHECK_TIMER.set(PERSISTENT_CHECK_INTERVAL);
}

void SIDETRACK_CLASS::shutdown()
{
    SIDETRACK_CHAT_INSTANCE.request_exit();
}

void SIDETRACK_CLASS::check(IO_WORKER_CLASS& io_worker, ollama_system& main_instance, COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, CLASS_SYSTEM* system)
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
    run_second_guess(io_worker, main_instance, comms, tools_list, tool_worker, system);


    // if all stages at 100, do not reset until something happens in main.

    PREVIOUS_HISTORY_SIZE = main_instance.history.size();
}




#endif