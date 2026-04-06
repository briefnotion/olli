#ifndef SIDETRACK_CPP
#define SIDETRACK_CPP

#include "sidetrack.h"

// ----

/**
 * Standalone Consolidation Function
 */
bool consolidate(std::vector<Message>& chat_history, OLLAMA_SYSTEM_PROPERTIES& config) 
{
    if (chat_history.empty())
    {
        cout << "[Consolidation] Chat history is empty. Skipping consolidation." << endl;
        return false;
    }

    ollama_system consolidate_client;
    // Copy necessary props...
    consolidate_client.PROPS.host = config.host;
    consolidate_client.PROPS.port = config.port;
    consolidate_client.PROPS.model = config.model;
    consolidate_client.PROPS.num_ctx = config.num_ctx; 
    consolidate_client.PROPS.use_thinking = false; 
    consolidate_client.PROPS.stream_output = false; 

    size_t starts_at = static_cast<size_t>(config.consolitation_starts_starts_at); 
    size_t sizes = static_cast<size_t>(config.consolitation_sizes); 

    cout << "[Consolidation] Starting consolidation process..." << endl;

    int current_level = 0;
    while (current_level < 10) 
    {



        /*
        // DIRECT ABORT CHECK: Checks the public boolean flags directly via reference
        if (config.status.interrupt_signal.load() || kb.INTERRUPTED || kb.IS_TYPING) {
            std::cout << "[Consolidation] Aborted: User activity detected." << std::endl;
            return; 
        }
        */




        std::vector<size_t> target_indices;
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            for (size_t i = 0; i < chat_history.size(); ++i) {
                // Keep the foundational rules (Level 0) and sync them with latest OLLAMA_OPENING
                if (current_level == 0 && chat_history[i].role == "system" && chat_history[i].consolidation_level == 0) {
                    //chat_history[i].content = config.OLLAMA_OPENING;
                    
                    continue;
                }
                if (chat_history[i].consolidation_level == current_level) {
                    target_indices.push_back(i);
                }
            }
        }

        if (target_indices.size() >= (starts_at + sizes)) {
            std::vector<size_t> merge_batch;
            for (size_t i = 0; i < sizes; ++i) merge_batch.push_back(target_indices[i]);

            std::string prompt = "Summarize the following history concisely:\n";
            {
                std::lock_guard<std::mutex> lock(history_mutex);
                for (size_t idx : merge_batch) prompt += "\n[" + chat_history[idx].role + "]: " + chat_history[idx].content;
            }

            consolidate_client.history.clear();
            consolidate_client.send(prompt, "system");

            std::string summary_text = consolidate_client.last_received.response;
            if (summary_text.empty()) summary_text = consolidate_client.last_received.thinking;




            // Final check before modifying shared history
            //bool aborted_during_llm = config.status.interrupt_signal.load() || kb.INTERRUPTED || kb.IS_TYPING;
            bool aborted_during_llm = false;



            if (!aborted_during_llm && consolidate_client.last_received.complete && !summary_text.empty()) {
                Message summary_msg;
                summary_msg.role = "system";
                summary_msg.content = "Context Summary: " + summary_text;
                summary_msg.consolidation_level = current_level + 1;

                {
                    std::lock_guard<std::mutex> lock(history_mutex);
                    std::vector<size_t> erase_indices = merge_batch;
                    std::sort(erase_indices.rbegin(), erase_indices.rend());
                    
                    for (size_t idx : erase_indices) {
                        chat_history.erase(chat_history.begin() + static_cast<std::ptrdiff_t>(idx));
                    }

                    size_t insert_pos = 0;
                    while (insert_pos < chat_history.size() && 
                           chat_history[insert_pos].role == "system" && 
                           chat_history[insert_pos].consolidation_level == 0) {
                        insert_pos++;
                    }
                    
                    chat_history.insert(chat_history.begin() + static_cast<std::ptrdiff_t>(insert_pos), summary_msg);
                }
                current_level++; 
                return true;
            } 
            else 
            {
                cout << "[Consolidation] Aborted during LLM processing or empty summary received." << endl;
                return false;
            }
        } 
        else 
        {
            cout << "[Consolidation] Not enough messages at level " << current_level 
                 << " to consolidate (found " << target_indices.size() 
                 << ", need " << (starts_at + sizes) << ")." << endl;
            return false;
        }
    }
    return false;
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
        }

        // MAIN THREAD ROUTINE GOES HERE
        {
            PROCESSING.store(true);

            if (IDLE_WAIT_TIMER_FOR_CONSOLIDATION.is_ready(thread_time.now()))
            {
                if (chat_history_requested == false && 
                    chat_history_needs_processing == false && 
                    chat_history_is_processed == false)
                {
                    std::cout << "Sidetrack: Requesting chat history for consolidation." << std::endl;
                    chat_history_requested = true;
                }

                if (chat_history_needs_processing)
                {
                    if (consolidate(temp_chat_history, SIDETRACK_CHAT_INSTANCE.PROPS))
                    {
                        chat_history_is_processed = true;
                    }
                    chat_history_needs_processing = false;
                }

                IDLE_WAIT_TIMER_FOR_CONSOLIDATION.set(thread_time.current_frame_time(), IDLE_WAIT_TIME_FOR_CONSOLIDATION);
            }
            
            PROCESSING.store(false);
        }

        // Reset interupt signal on main thread routine completion
        INTERUPT.store(false);

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

void SIDETRACK_CLASS::check(bool Interupt_Signal, ollama_system& main_instance)
{
    if (Interupt_Signal)
    {
        INTERUPT.store(true);
    }

    if (chat_history_requested)
    {
        temp_chat_history =  main_instance.history;
        chat_history_requested = false;
        chat_history_needs_processing = true;
    }

    if (chat_history_is_processed)
    {
        if (INTERUPT.load() == false)
        {
            main_instance.history = temp_chat_history;
        }
        chat_history_is_processed = false;
    }
}


#endif