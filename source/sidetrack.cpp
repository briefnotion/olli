#ifndef SIDETRACK_CPP
#define SIDETRACK_CPP

#include "sidetrack.h"

// ----

/**
 * Refactored Consolidation Function
 * * Logic:
 * 1. Identify all messages at current_level.
 * 2. If count > (starts_at + sizes), we consolidate the OLDEST messages 
 * past the 'starts_at' threshold.
 */
bool consolidate(std::vector<Message>& chat_history, OLLAMA_SYSTEM_PROPERTIES& config) 
{
    if (chat_history.empty()) return false;

    ollama_system consolidate_client;
    consolidate_client.PROPS.host = config.host;
    consolidate_client.PROPS.port = config.port;
    consolidate_client.PROPS.model = config.model;
    consolidate_client.PROPS.num_ctx = config.num_ctx; 
    consolidate_client.PROPS.use_thinking = false; 
    consolidate_client.PROPS.stream_output = false; 

    size_t starts_at = static_cast<size_t>(config.consolitation_starts_starts_at); 
    size_t sizes = static_cast<size_t>(config.consolitation_sizes); 

    bool any_consolidation_occurred = false;

    // We iterate through levels. If a level is too full, we shrink it and move up.
    for (int current_level = 0; current_level < 10; ++current_level) 
    {
        std::vector<size_t> level_indices;
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            for (size_t i = 0; i < chat_history.size(); ++i) {
                // Ignore L0 system prompts if they are "protected"
                if (current_level == 0 && chat_history[i].role == "system" && chat_history[i].consolidation_level == 0) {
                    continue;
                }
                if (chat_history[i].consolidation_level == current_level) {
                    level_indices.push_back(i);
                }
            }
        }

        // Only consolidate if we exceed the allowed buffer (starts_at) + the chunk size to merge (sizes)
        if (level_indices.size() >= (starts_at + sizes)) {
            
            // We want to keep 'starts_at' messages untouched.
            // Usually, we consolidate the OLDEST ones (the ones at the start of the level_indices list)
            // Or we consolidate ones at the end of the list. 
            // Assuming we want to keep the most RECENT 'starts_at' messages:
            
            std::vector<size_t> merge_batch;
            // Target the oldest 'sizes' messages that are ABOVE the threshold
            for (size_t i = 0; i < sizes; ++i) {
                merge_batch.push_back(level_indices[i]);
            }

            std::string prompt = "Summarize the following conversation segment concisely while preserving key facts and the current state of the topic:\n";
            {
                std::lock_guard<std::mutex> lock(history_mutex);
                for (size_t idx : merge_batch) {
                    prompt += "\n[" + chat_history[idx].role + "]: " + chat_history[idx].content;
                }
            }

            //cout << "[Consolidation] Level " << current_level << " has " << level_indices.size() 
            //     << " messages. Merging " << sizes << " messages..." << endl;

            consolidate_client.history.clear();
            consolidate_client.send(prompt, "system");

            std::string summary_text = consolidate_client.last_received.response;
            if (summary_text.empty()) summary_text = consolidate_client.last_received.thinking;

            if (consolidate_client.last_received.complete && !summary_text.empty()) {
                Message summary_msg;
                summary_msg.role = "system";
                summary_msg.content = "Summary of previous context: " + summary_text;
                summary_msg.consolidation_level = current_level + 1;

                {
                    std::lock_guard<std::mutex> lock(history_mutex);
                    
                    // 1. Remove old messages (Reverse order to maintain index integrity)
                    std::vector<size_t> erase_indices = merge_batch;
                    std::sort(erase_indices.rbegin(), erase_indices.rend());
                    for (size_t idx : erase_indices) {
                        chat_history.erase(chat_history.begin() + static_cast<std::ptrdiff_t>(idx));
                    }

                    // 2. Insert summary. 
                    // To keep history chronological, we insert it at the position where the first merged message was.
                    size_t insert_pos = merge_batch[0]; 
                    if (insert_pos > chat_history.size()) insert_pos = chat_history.size();
                    
                    chat_history.insert(chat_history.begin() + static_cast<std::ptrdiff_t>(insert_pos), summary_msg);
                }
                
                any_consolidation_occurred = true;
                // Stay on this level to see if we need to consolidate more (until we are under the limit)
                current_level--; 
            } else {
                return any_consolidation_occurred;
            }
        }
        // If this level is fine, the loop continues to the next level (current_level++)
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

            // Consolidation
            if (IDLE_WAIT_TIMER_FOR_CONSOLIDATION.is_ready(thread_time.now()))
            {
                if (chat_history_processing_stage == 0)
                {
                    //std::cout << "Sidetrack: Requesting chat history for consolidation." << std::endl;
                    chat_history_processing_stage = 1;
                }

                if (chat_history_processing_stage == 2)
                {
                    if (consolidate(temp_chat_history, SIDETRACK_CHAT_INSTANCE.PROPS))
                    {
                        chat_history_processing_stage = 3;
                    }
                    else
                    {
                        chat_history_processing_stage = 0;
                    }
                    
                    IDLE_WAIT_TIMER_FOR_CONSOLIDATION.set(thread_time.current_frame_time(), IDLE_WAIT_TIME_FOR_CONSOLIDATION);
                }
            }

            if (CHAT_FINISHED.load())
            {
                //std::cout << "Sidetrack: Chat finished signal received." << std::endl;
                CHAT_FINISHED.store(false);
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
    while (RUN)
    {
        RUN = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
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

    if (chat_history_processing_stage == 1)
    {
        temp_chat_history =  main_instance.history;
        chat_history_processing_stage = 2;
    }

    if (chat_history_processing_stage == 3)
    {
        if (INTERUPT.load() == false)
        {
            main_instance.history = temp_chat_history;
        }
        chat_history_processing_stage = 0;
    }
}


#endif