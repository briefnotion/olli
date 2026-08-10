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
            INTERUPT.store(false);
        }

        // MAIN THREAD ROUTINE GOES HERE
        {
            PROCESSING.store(true);


            if (ROUTINE == 0)
            {
                if (IDLE_WAIT_TIMER_FOR_CONSOLIDATION.is_ready(thread_time.now()))
                {
                    ROUTINE = 1; // Start consolidation routine
                }

                if (CHAT_FINISHED.load())
                {
                    //std::cout << "Sidetrack: Chat finished signal received." << std::endl;
                    ROUTINE = 2; // Start second guessing routine.
                    CHAT_FINISHED.store(false);
                }
            }

            // Consolidation
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

                IDLE_WAIT_TIMER_FOR_CONSOLIDATION.set(thread_time.current_frame_time(), IDLE_WAIT_TIME_FOR_CONSOLIDATION);
            }

            if (ROUTINE == 2)
            {

                if (SIGNALS.INTERUPT_SIGNAL == true)
                {
                    // abort routine if interupt signal received.
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

                    //SIDETRACK_CHAT_INSTANCE.send("Review the conversation that just finished. Identify any potential misunderstandings, missed user intents, or areas where the assistant's response could have been improved. Provide a concise analysis of what could be done better in future interactions.", "system");
                    SIDETRACK_CHAT_INSTANCE.send("You are the 'Internal Monologue' of the assistant. Review the turn "
                        "that just ended. If there are technical details, edge cases, or deeper insights that were "
                        "missed for the sake of brevity, provide them now. "

                        "CRITICAL: Speak directly to the user as if you just had a 'lightbulb moment.' Do not use "
                        "phrases like 'The assistant should have...' or 'Analysis shows...' "

                        //"Format: Start immediately with the additional info or a follow-up thought. If the previous "
                        //"response was truly sufficient, respond with only '[FIN].'");

                        "Format: Start immediately with the additional info or a follow-up thought. If the previous "
                        "response was truly sufficient, respond with ONLY the word DONE. "
                        "Do not use brackets, do not use periods, do not say anything else.");

                    bool dummy_enable_keyboard_input = false; // This routine does not require keyboard input, but we pass the variable to satisfy the function signature.


                    while (SIDETRACK_CHAT_INSTANCE.is_processing || !SIDETRACK_CHAT_INSTANCE.tts_buffer.empty())                    
                    {
                        if (starts_with(SIDETRACK_CHAT_INSTANCE.last_received.response, "DONE"))
                        {
                            SECOND_GUESS_PROCESSING_STAGE = 4;
                            break;
                        }

                        SIDETRACK_CHAT_INSTANCE.process(dummy_enable_keyboard_input);
                    }

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

void SIDETRACK_CLASS::check(ollama_system& main_instance)
{
    if (SIGNALS.INTERUPT_SIGNAL)
    {
        INTERUPT.store(true);
        SIGNALS.INTERUPT_SIGNAL = false;
    }

    if (SIGNALS.CHAT_FINISHED_SIGNAL)
    {
        CHAT_FINISHED.store(true);
        SIGNALS.CHAT_FINISHED_SIGNAL = false;
    }

    // ----

    // Consolidation Routine
    {
        if (CHAT_HISTORY_PROCESSING_STAGE == 1)
        {
            temp_chat_history =  main_instance.history;
            CHAT_HISTORY_PROCESSING_STAGE = 2;
        }

        if (CHAT_HISTORY_PROCESSING_STAGE == 3)
        {
            if (INTERUPT.load() == false)
            {
                main_instance.history = temp_chat_history;
                main_instance.save_history();
            }
            CHAT_HISTORY_PROCESSING_STAGE = 4;
        }
    }

    // Second Guess Routine
    {
        if (SECOND_GUESS_PROCESSING_STAGE == 1)
        {
            temp_chat_history =  main_instance.history;
            SECOND_GUESS_PROCESSING_STAGE = 2;
        }

        // Second Guess Routine
        if (SECOND_GUESS_PROCESSING_STAGE == 3)
        {
            // Only fold the review back into the main conversation when the
            // side instance actually produced something. Calling .back() on an
            // empty vector (e.g. the routine was interrupted before any
            // response) is undefined behaviour.
            if (!SIDETRACK_CHAT_INSTANCE.history.empty())
            {
                main_instance.history.push_back(SIDETRACK_CHAT_INSTANCE.history.back());
            }
            SECOND_GUESS_PROCESSING_STAGE = 4;
        }
    }
}


#endif