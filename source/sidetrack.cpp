#ifndef SIDETRACK_CPP
#define SIDETRACK_CPP

#include "sidetrack.h"

#if 0 // Old SIDETRACK_CLASS implementation - being rewritten from scratch,
      // kept here for reference, not compiled. See TODO.md's sidetrack
      // rewrite entry.

#include <set>

// For OUTPUT_CLASS's real definition (chat_thinking/chat_response/
// system_message below) - sidetrack.h only forward-declares it (via
// olla.h), which used to reach here as a full definition transitively
// through system.h -> user_io.h before key_input/output moved off
// CLASS_SYSTEM onto IO_WORKER_CLASS.
#include "user_io.h"

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
    consolidate_client.debug_label = "sidetrack-consolidate";
    debug_log_instance_event("sidetrack-consolidate", "instance created");
    consolidate_client.PROPS.host = config.host;
    consolidate_client.PROPS.port = config.port;
    consolidate_client.PROPS.model = config.model;
    consolidate_client.PROPS.num_ctx = config.num_ctx;
    consolidate_client.PROPS.use_thinking = false;
    consolidate_client.PROPS.stream_output = false;

    // consolidate_client's own private tools_list - same pattern as every
    // other isolated/throwaway ollama_system instance (see process()'s
    // comment in olla.h). Not that summarization has any real use for
    // tools, but send() needs a valid one regardless.
    std::vector<std::unique_ptr<TOOL_BASE>> consolidate_tools_list;
    populate_default_tools(consolidate_tools_list);

    bool llm_failed = false;
    for (size_t level = 0; !llm_failed && level + 1 < levels.size(); ++level) {
        while (levels[level].size() >= starts_at + sizes) {
            // Oldest 'sizes' messages sit at the front of the bucket.
            std::string prompt = "Summarize the following conversation segment concisely while preserving key facts and the current state of the topic:\n";
            for (size_t i = 0; i < sizes; ++i) {
                prompt += "\n[" + levels[level][i].role + "]: " + levels[level][i].content;
            }

            consolidate_client.history.clear();
            consolidate_client.send(consolidate_tools_list, prompt, "system");

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

    debug_log_instance_event("sidetrack-consolidate", "instance closed");
    return any_consolidation_occurred;
}

// ----

SIDETRACK_CLASS::SIDETRACK_CLASS()
{
}

void SIDETRACK_CLASS::create(OLLAMA_SYSTEM_PROPERTIES Ollama_Properties, IO_WORKER_CLASS* audio)
{
    SIDETRACK_CHAT_INSTANCE.debug_label = "sidetrack-review";
    debug_log_instance_event("sidetrack-review", "instance created");

    SIDETRACK_CHAT_INSTANCE.PROPS = Ollama_Properties;

    // This copy inherits LOAD_SAVE_HISTORY_ON_DISK=true and the real
    // OLLI_DIRECTORY straight from chat.PROPS (main.cpp's sidetrack.create()
    // call) - but SIDETRACK_CHAT_INSTANCE.open() is never called (unlike
    // every other secondary ollama_system instance, which goes through the
    // open(Properties) overload specifically to force this off), so it was
    // silently keeping real disk-saving behavior. ollama_system::process()
    // auto-saves history.json whenever an instance's history.size() changes
    // (olla.cpp) - so every second-guess review tick was overwriting the
    // SAME shared history.json with SIDETRACK_CHAT_INSTANCE's own private,
    // in-progress scratch history (temp snapshot + review prompt + review
    // reply), completely bypassing check()'s stage-3 fold decision. Found
    // chasing the repeat bug: a fold correctly skipped in memory still
    // showed up duplicated in history.json, because this had already
    // written the unfiltered version straight to disk first.
    SIDETRACK_CHAT_INSTANCE.PROPS.LOAD_SAVE_HISTORY_ON_DISK = false;

    // COMMS::audio no longer exists - sidetrack has no speech path of its
    // own right now (see olla.h's note near output_buffer_mutex). audio
    // is kept as a parameter since sidetrack is getting reworked and will
    // likely want IO_WORKER_CLASS::speak() directly once that lands.
    (void)audio;

    populate_default_tools(tools_list);
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
                    SIDETRACK_CHAT_INSTANCE.send(tools_list, "You are the 'Internal Monologue' of the assistant. Review the turn "
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

                    std::atomic<bool> dummy_enable_keyboard_input{false}; // This routine does not require keyboard input, but we pass the variable to satisfy the function signature.


                    // send() above already blocked until the response was
                    // complete (or aborted), so by the time execution
                    // reaches here the network call is already done. This
                    // used to loop "a handful of iterations" draining
                    // comms.tts_buffer via repeated process() calls - that
                    // field (and the write_to_tts() chunking process() used
                    // to do with it) is gone now, moved out to
                    // IO_WORKER_CLASS::thread_main() (io_worker.cpp), which
                    // only sees the main chat's own comms, not sidetrack's -
                    // sidetrack has no speech path right now (see olla.h's
                    // note near output_buffer_mutex). process() still needs
                    // to run at least once for its other work (tool
                    // dispatch, history save).
                    if (starts_with(SIDETRACK_CHAT_INSTANCE.last_received.response, "DONE"))
                    {
                        SECOND_GUESS_PROCESSING_STAGE = 4;
                    }
                    else
                    {
                        // nullptr, not the real CLASS_SYSTEM: this runs on
                        // the sidetrack background thread, not the main
                        // thread - see TOOL_BASE::check()'s comment in
                        // tools.h for why every other call site can pass the
                        // real one and this one specifically can't.
                        SIDETRACK_CHAT_INSTANCE.process(nullptr, tools_list, dummy_enable_keyboard_input);
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
    debug_log_instance_event("sidetrack-review", "instance closed");
}

// Rough word-overlap similarity between two strings, used by check()'s
// second-guess fold (SECOND_GUESS_PROCESSING_STAGE == 3) to skip folding in
// a review reply that's just restating the last real assistant turn instead
// of adding something new. qwen3:8b has a real tendency to lock onto and
// reproduce a short answer near-verbatim here, even when its own "thinking"
// explicitly acknowledges the point was already made. Case-insensitive,
// whitespace/punctuation-split; returns the fraction of b's words that also
// appear in a (0.0-1.0). Deliberately simple (no stemming, no word order) -
// this only needs to catch near-verbatim restatement, not paraphrase.
double word_overlap_ratio(const std::string& a, const std::string& b)
{
    auto words_of = [](const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : lower_case(s))
        {
            if (isAlphaNumeric(c)) cur += c;
            else if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    };

    std::vector<std::string> words_b = words_of(b);
    if (words_b.empty()) return 0.0;

    std::vector<std::string> words_a = words_of(a);
    std::set<std::string> set_a(words_a.begin(), words_a.end());

    size_t matched = 0;
    for (const auto& w : words_b)
    {
        if (set_a.count(w)) matched++;
    }
    return static_cast<double>(matched) / static_cast<double>(words_b.size());
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

                // Skip the fold if this review reply is just restating the
                // last real assistant turn rather than adding anything new
                // - see word_overlap_ratio()'s comment above. Compared
                // against main_instance.history (the real conversation),
                // not SIDETRACK_CHAT_INSTANCE.history (just this review's
                // own private prompt/reply pair, which has nothing to
                // compare against).
                const Message& review_reply = SIDETRACK_CHAT_INSTANCE.history.back();
                bool is_restatement = false;
                for (auto it = main_instance.history.rbegin(); it != main_instance.history.rend(); ++it)
                {
                    if (it->role == "assistant")
                    {
                        is_restatement = word_overlap_ratio(it->content, review_reply.content) >= 0.5;
                        break;
                    }
                }

                if (!is_restatement)
                {
                    main_instance.history.push_back(review_reply);
                }
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

    if (starts_with(SIDETRACK_CHAT_INSTANCE.comms.INPUT_FROM_LLM, "DONE"))
    {
        output.chat_thinking += SIDETRACK_CHAT_INSTANCE.comms.INPUT_FROM_LLM;
    }
    else
    {
        output.chat_response += SIDETRACK_CHAT_INSTANCE.comms.INPUT_FROM_LLM;
    }
    SIDETRACK_CHAT_INSTANCE.comms.INPUT_FROM_LLM.clear();

    output.chat_thinking += SIDETRACK_CHAT_INSTANCE.comms.INPUT_FROM_THINKING;
    SIDETRACK_CHAT_INSTANCE.comms.INPUT_FROM_THINKING.clear();

    output.system_message += SIDETRACK_CHAT_INSTANCE.comms.INPUT_FROM_SYSTEM;
    SIDETRACK_CHAT_INSTANCE.comms.INPUT_FROM_SYSTEM.clear();
}

#endif // SIDETRACK_CLASS rewrite - see the #if 0 above


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