#ifndef olla_cpp
#define olla_cpp

#include "olla.h"




void ollama_system::open() 
{
    std::cout << "[System] Connecting to " << PROPS.host << ":" << PROPS.port << " (" << PROPS.model << ")" << std::endl;
    
    // 
    if (history.empty()) 
    {
        //history.push_back({"system", "You are a helpful assistant with access to tools."

        history.push_back({"system", 
            "You are a helpful assistant with access to tools. "
            "1. For delayed requests, use set_timer. Always summarize the "
            "user's intent in the 'reminder' field (e.g., 'Turn off "
            "the living room fan'). "
            "2. When the system sends a message starting "
            "with 'SYSTEM NOTIFICATION: Timer Expired', look at the "
            "associated reminder and immediately call the relevant tool "
            "to fulfill that action without asking for further confirmation."
        });
    }
}

void ollama_system::add_tool(const std::string& name, const std::string& description, json parameters) 
{
    tools.push_back({
        {"type", "function"},
        {"function", {
            {"name", name},
            {"description", description},
            {"parameters", parameters.empty() ? json({{"type", "object"}, {"properties", json::object()}}) : parameters}
        }}
    });
}


void ollama_system::send(const std::string& user_input, const std::string& role) {
    // Reset interrupt state for a new request 
    ///
    //bool was_interrupted = last_received.interrupted;
    //status.interrupt_signal = false;
    status.is_active = true;

    {
        std::lock_guard<std::mutex> lock(history_mutex);
        if (role == "user")
        {
            // user roles (system/tool) pushed normally
            history.push_back({"user", user_input});
        }
        else
        {
            // Non-user roles (system/tool) pushed normally
            history.push_back({role, user_input});
        }
    }
    
    last_received.complete = false;
    //last_received.interrupted = false;
    last_received.response = "";
    last_received.thinking = "";
    last_received.tool_calls.clear();

    json messages_json = json::array();
    {
        std::lock_guard<std::mutex> lock(history_mutex);
        for (const auto& msg : history) {
            json m = {{"role", msg.role}, {"content", msg.content}};
            if (!msg.tool_call_id.empty()) m["tool_call_id"] = msg.tool_call_id;
            messages_json.push_back(m);
        }
    }

    json body = {
        {"model", PROPS.model},
        {"messages", messages_json},
        {"stream", PROPS.stream_output},
        {"think", PROPS.use_thinking},
        // Add the options block here
        {"options", {
            {"num_ctx", PROPS.num_ctx} 
        }}
    };

    if (!tools.empty()) {
        body["tools"] = tools;
    }

    httplib::Client cli(PROPS.host, PROPS.port);
    cli.set_read_timeout(120);
    
    std::string accumulated_content = "";
    std::string accumulated_thinking = "";
    bool in_thinking_block = false;

    httplib::Headers headers = { {"Content-Type", "application/json"} };
    std::string json_body = body.dump();

    if (PROPS.stream_output) {
        auto res = cli.Post(
            "/api/chat",
            headers,
            json_body,
            "application/json",
            [&](const char *data, size_t data_length) {
                if (status.interrupt_signal.load()) {
                    //last_received.interrupted = true;
                    return false; 
                }

                std::string chunk(data, data_length);
                try {
                    auto j_chunk = json::parse(chunk);
                    if (j_chunk.contains("message")) {
                        auto msg_chunk = j_chunk["message"];
                        
                        if (msg_chunk.contains("thinking")) {
                            if (!in_thinking_block) {
                                std::cout << "\n<thinking>\n";
                                in_thinking_block = true;
                            }
                            std::string t = msg_chunk["thinking"];
                            std::cout << t << std::flush;
                            accumulated_thinking += t;
                        } 
                        else if (msg_chunk.contains("content")) {
                            if (in_thinking_block) {
                                std::cout << "\n</thinking>\n\n";
                                in_thinking_block = false;
                            }
                            std::string c = msg_chunk["content"];
                            std::cout << c << std::flush;
                            accumulated_content += c;

                            tts_buffer += c;
                        }

                        if (msg_chunk.contains("tool_calls")) {
                            for (auto& tc : msg_chunk["tool_calls"]) {
                                last_received.tool_calls.push_back({
                                    tc.value("id", ""),
                                    tc["function"].value("name", ""),
                                    tc["function"]["arguments"]
                                });
                            }
                        }
                    }
                } catch (...) {}
                return true;
            }
        );

        if (status.interrupt_signal.load()) {
            std::cout << "\n[System: Response Interrupted by User]\n";
        } else if (!res || res->status != 200) {
            std::cerr << "\n[Error] Stream failed: " << (res ? std::to_string(res->status) : "Connection error") << std::endl;
        }

        if (in_thinking_block) {
            std::cout << "\n</thinking>\n";
            in_thinking_block = false;
        }
    } else {
        auto res = cli.Post("/api/chat", headers, json_body, "application/json");
        if (res && res->status == 200) {
            auto j_res = json::parse(res->body);
            auto msg_obj = j_res["message"];
            accumulated_content = msg_obj.value("content", "");
            accumulated_thinking = msg_obj.value("thinking", "");
        }
    }

    last_received.response = accumulated_content;
    last_received.thinking = accumulated_thinking;
    last_received.complete = !status.interrupt_signal.load();
    
    // We now save the response even if it was interrupted, 
    // provided we actually received some content.
    if (!last_received.response.empty() && last_received.tool_calls.empty()) {
        std::lock_guard<std::mutex> lock(history_mutex);
        
        std::string final_content = last_received.response;
        if (status.interrupt_signal.load()) {
            final_content += "... [Interrupted]";
        }
        
        history.push_back({"assistant", final_content});
    }
    
    std::cout << std::endl;
    
    status.is_active = false;
}

void ollama_system::send_tool_result(const std::string& tool_call_id, const std::string& result) {
    Message msg;
    msg.role = "tool";
    msg.content = result;
    msg.tool_call_id = tool_call_id;
    history.push_back(msg);
    send("", "tool"); 
}

void ollama_system::stop()
{
    status.interrupt_signal = true; 
}


/**
 * Updates the internal status struct by scanning the history.
 */
void ollama_system::update_status() {
    std::lock_guard<std::mutex> lock(history_mutex);
    status.total_messages = static_cast<int>(history.size());
    status.level_counts.clear();
    status.max_level = 0;
    for (const auto& msg : history) {
        status.level_counts[msg.consolidation_level]++;
        if (msg.consolidation_level > status.max_level) status.max_level = msg.consolidation_level;
    }
}

/**
 * Writes all history to a file in the specified directory.
 * Overwrites the file if it already exists.
 */
void ollama_system::history_write(std::string Directory) {
    namespace fs = std::filesystem;
    try {
        if (!fs::exists(Directory)) {
            fs::create_directories(Directory);
        }

        fs::path filePath = fs::path(Directory) / "history_debug.txt";
        std::ofstream outFile(filePath, std::ios::out | std::ios::trunc);

        if (!outFile.is_open()) {
            std::cerr << "[Error] Could not open file for writing: " << filePath << std::endl;
            return;
        }

        outFile << "=== OLLAMA SYSTEM HISTORY DEBUG ===" << std::endl;
        outFile << "Status: " << status.to_string() << std::endl;
        outFile << "------------------------------------" << std::endl;

        for (size_t i = 0; i < history.size(); ++i) {
            const auto& msg = history[i];
            outFile << "Index: " << i << std::endl;
            outFile << "Level: " << msg.consolidation_level << std::endl;
            outFile << "Role:  " << msg.role << std::endl;
            outFile << "Content: " << msg.content << std::endl;
            if (!msg.tool_call_id.empty()) {
                outFile << "Tool ID: " << msg.tool_call_id << std::endl;
            }
            outFile << "------------------------------------" << std::endl;
        }

        outFile.close();
    } catch (const std::exception& e) {
        std::cerr << "[Error] history_write failed: " << e.what() << std::endl;
    }
}

// ----

/**
 * Standalone Consolidation Function
 * Processes the chat history to merge groups of messages into single summaries based on levels.
 * This version protects foundational system messages at Level 0 and keeps them at the very top.
 */
void consolidate(std::vector<Message>& chat_history, ollama_system& config) {
    if (chat_history.empty()) return;

    ollama_system consolidate_client;
    consolidate_client.PROPS.host = config.PROPS.host;
    consolidate_client.PROPS.port = config.PROPS.port;
    consolidate_client.PROPS.model = config.PROPS.model;
    consolidate_client.PROPS.num_ctx = config.PROPS.num_ctx; 
    consolidate_client.PROPS.use_thinking = false; 
    consolidate_client.PROPS.stream_output = false; 

    size_t starts_at = static_cast<size_t>(config.consolitation_starts_starts_at); 
    size_t sizes = static_cast<size_t>(config.consolitation_sizes); 

    int current_level = 0;
    const int MAX_SUPPORTED_LEVEL = 10;

    while (current_level < MAX_SUPPORTED_LEVEL) {
        if (config.status.interrupt_signal.load()) break;

        std::vector<size_t> target_indices;
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            for (size_t i = 0; i < chat_history.size(); ++i) {
                // PROTECTION: Level 0 System messages are the "Rules". Never consolidate them.
                if (current_level == 0 && chat_history[i].role == "system") {
                    continue;
                }

                if (chat_history[i].consolidation_level == current_level) {
                    target_indices.push_back(i);
                }
            }
        }

        // Check if we have enough messages at this level to trigger consolidation
        if (target_indices.size() >= (starts_at + sizes)) {
            std::vector<size_t> merge_batch;
            
            for (size_t i = 0; i < sizes; ++i) {
                merge_batch.push_back(target_indices[i]);
            }

            std::string prompt = "Summarize the following " + std::to_string(merge_batch.size()) + 
                                 " entries into a single concise paragraph. This is part of a Level " + 
                                 std::to_string(current_level) + " context history.\n";
            
            {
                std::lock_guard<std::mutex> lock(history_mutex);
                for (size_t idx : merge_batch) {
                    prompt += "\n[" + chat_history[idx].role + "]: " + chat_history[idx].content;
                }
            }

            consolidate_client.history.clear();
            consolidate_client.send(prompt, "system");

            std::string summary_text = consolidate_client.last_received.response;
            if (summary_text.empty() && !consolidate_client.last_received.thinking.empty()) {
                summary_text = consolidate_client.last_received.thinking;
            }

            if (!consolidate_client.status.interrupt_signal.load() && 
                consolidate_client.last_received.complete && 
                !summary_text.empty()) {

                Message summary_msg;
                summary_msg.role = "system";
                summary_msg.content = "Context Summary: " + summary_text;
                summary_msg.consolidation_level = current_level + 1;

                {
                    std::lock_guard<std::mutex> lock(history_mutex);
                    
                    // 1. Erase the old messages safely by iterating backwards
                    std::vector<size_t> erase_indices = merge_batch;
                    std::sort(erase_indices.rbegin(), erase_indices.rend());
                    
                    for (size_t idx : erase_indices) {
                        chat_history.erase(chat_history.begin() + static_cast<std::ptrdiff_t>(idx));
                    }

                    // 2. Find insertion point. 
                    // We must ensure foundational system messages (Level 0) stay at indices 0, 1, etc.
                    // Find the first index after the foundational "Rules".
                    size_t first_non_foundational_idx = 0;
                    while (first_non_foundational_idx < chat_history.size() && 
                           chat_history[first_non_foundational_idx].role == "system" && 
                           chat_history[first_non_foundational_idx].consolidation_level == 0) {
                        first_non_foundational_idx++;
                    }

                    // Now find the insertion point within the "Summary" block (Level 1+)
                    size_t insert_pos = first_non_foundational_idx;
                    for (size_t i = first_non_foundational_idx; i < chat_history.size(); ++i) {
                        // Keep higher levels at the top of the summary block
                        if (chat_history[i].consolidation_level >= summary_msg.consolidation_level) {
                            insert_pos = i + 1;
                        } else {
                            break;
                        }
                    }
                    
                    chat_history.insert(chat_history.begin() + static_cast<std::ptrdiff_t>(insert_pos), summary_msg);
                }

                std::cout << "[Consolidation] Merged " << sizes << " messages from L" << current_level 
                          << " into L" << (current_level + 1) << std::endl;
                
                current_level++; 
            } else {
                std::cout << "[Consolidation] Failed to get summary. Aborting." << std::endl;
                break;
            }
        } else {
            break; 
        }
    }
}

void ollama_system::write_to_tts() 
{
    if (!tts_buffer.empty())
    {
        if ((tts_buffer.find_first_of(".!?,:;") != std::string::npos || tts_buffer.length() > 60) ||
        status.is_active == false)
        {
            if (!std::filesystem::exists(PROPS.path_output / "output.txt"))
            {
                std::ofstream out(PROPS.path_output / "output.txt");
                if (out.is_open()) 
                {
                    out << tts_filter(tts_buffer);
                    out.close();
                    tts_buffer.clear();
                }
            }
        }
    }
}

void ollama_system::input(KEYBOARD_INPUT& Key_Input)
{

    if (is_processing && Key_Input.INTERRUPTED) 
    {
        std::cout<< "." << std::flush;
        Key_Input.reset();
        // If the assistant is currently talking and we get NEW input, 
        // we trigger the interrupt.
        stop();
        if (chat_thread.joinable()) chat_thread.join();
        is_processing = false;
        std::cout << "\n[Interrupting for new input...]" << std::endl;
    }

    if (Key_Input.ENTER_PRESSED) {
        if (trim(Key_Input.LINE) == "bye" || trim(Key_Input.LINE) == "quit" || trim(Key_Input.LINE) == "Goodbye.") {
            Key_Input.reset();
            running = false;
            if (is_processing) stop();
            if (chat_thread.joinable()) chat_thread.join();
        }
        else 
        {
            status.interrupt_signal = false;
            // Process the user message in a background thread
            is_processing = true;
            std::string tmp_line = Key_Input.LINE;
            Key_Input.reset();
            bool tmp_is_processing = is_processing;
            // Capture chat by reference, user_input by value, and is_processing by reference
            chat_thread = std::thread([this, tmp_line, &tmp_is_processing]() 
                                        {
                                            try {
                                                send(tmp_line);
                                            } catch (...) {
                                                // Safety: Ensure flag is reset even if send throws
                                            }
                                            is_processing = false;
                                        });
            is_processing = tmp_is_processing;
        }
    }


}





#endif