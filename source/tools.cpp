#ifndef tools_cpp
#define tools_cpp

#include "tools.h"

// ----

/**
 * @brief Registers the set_thinking_mode tool with the system.
 */
void TOOL_SET_THINKING_MODE::register_tool(ollama_system& chat) {
    // Tool to enable or disable the "thinking" process/mode
    json set_thinking_params = {
        {"type", "object"},
        {"properties", {
            {"enabled", {
                {"type", "boolean"}, 
                {"description", "Set to true to enable thinking mode, false to disable it"}
            }}
        }},
        {"required", {"enabled"}}
    };

    chat.add_tool("set_thinking_mode", "Enables or disables the internal reasoning/thinking process for the model", set_thinking_params);
}

/**
 * @brief Handles the execution of the set_thinking_mode tool.
 */
void TOOL_SET_THINKING_MODE::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "set_thinking_mode") {
        if (args.contains("enabled") && args["enabled"].is_boolean()) {
            // Update the member variable in the ollama_system instance
            chat.PROPS.use_thinking = args["enabled"].get<bool>();

            std::string state_str = chat.PROPS.use_thinking ? "ENABLED" : "DISABLED";
            
            // Log to console for system monitoring
            std::cout << "[System (set_thinking_mode)]: " << state_str << std::endl;

            // Send the result back to the chat system
            chat.send_tool_result(tc_id, "Thinking mode has been successfully " + state_str);
        } else {
            std::string error_msg = "Error: Missing or invalid 'enabled' boolean argument.";
            std::cerr << "[System] " << error_msg << std::endl;
            chat.send_tool_result(tc_id, error_msg);
        }
    } 
    else {
        // Handle unknown tool names for this specific tool class
        std::string error_msg = "Error: Tool '" + name + "' not recognized by TOOL_SET_THINKING_MODE.";
        std::cerr << "[System] " << error_msg << std::endl;
        chat.send_tool_result(tc_id, error_msg);
    }
}

// ---

/**
 * @brief Registers the get_current_time tool with the system.
 */
void TOOL_GET_CURRENT_TIME::register_tool(ollama_system& chat) {
    // Tool to get the current system time
    json get_time_params = {
        {"type", "object"},
        {"properties", {
            {"format", {
                {"type", "string"}, 
                {"description", "Optional: custom strftime format (e.g., '%H:%M')"}
            }}
        }}
    };

    chat.add_tool("get_current_time", "Returns the current system date and time", get_time_params);
}

/**
 * @brief Refactored TOOL_GET_CURRENT_TIME class to match the TOOL_TIMER handle_tool signature.
 */
void TOOL_GET_CURRENT_TIME::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "get_current_time") {
        // Get the current system time
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        
        // Convert to local time string
        std::stringstream ss;
        ss << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S");
        std::string current_time_str = ss.str();

        // Optional: Check for format arguments if your tool schema supports them
        if (args.contains("format")) {
            std::string format = args["format"];
            // Logic to handle custom formatting could go here
        }

        // Log to console for system monitoring
        std::cout << "[System] Tool 'get_current_time' called. Result: " << current_time_str << std::endl;

        // Send the result back to the chat system using the provided tool call ID
        chat.send_tool_result(tc_id, "The current system time is: " + current_time_str);
    } 
    else {
        // Handle unknown tool names for this specific tool class
        std::string error_msg = "Error: Tool '" + name + "' not recognized by TOOL_GET_CURRENT_TIME.";
        std::cerr << "[System] " << error_msg << std::endl;
        chat.send_tool_result(tc_id, error_msg);
    }
}

// ---

void TOOL_TIMER::register_tool(ollama_system& chat) {
    // 1. Tool to set/start a timer
    json set_timer_params = {
        {"type", "object"},
        {"properties", {
            {"label", {{"type", "string"}, {"description", "A name for the timer"}}},
            {"seconds", {{"type", "number"}, {"description", "Duration in seconds"}}},
            {"reminder", {{"type", "string"}, {"description", "Optional: Action to perform when finished. Leave empty for a simple notification."}}}
        }},
        {"required", {"label", "seconds"}} // Removed 'reminder' from required list
    };

    // 2. Tool to check status
    json check_timer_params = {
        {"type", "object"},
        {"properties", {
            {"label", {{"type", "string"}, {"description", "The name of the timer to check"}}}
        }},
        {"required", {"label"}}
    };

    chat.add_tool("set_timer", "Starts a countdown and schedules a future action (optional)", set_timer_params);
    chat.add_tool("check_timer", "Checks if a specific named timer has finished", check_timer_params);
}

void TOOL_TIMER::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "set_timer") {
        std::cout << "[System (set_timer)]" << std::endl;
        std::string label = args["label"];
        double seconds = args["seconds"];
        
        // Handle optional reminder field
        std::string reminder = "";
        if (args.contains("reminder") && !args["reminder"].is_null()) {
            reminder = args["reminder"];
        }
        
        TIMER_SIMPLE new_timer(seconds, reminder);
        new_timer.start();
        active_timers[label] = new_timer;

        std::string res = "Timer '" + label + "' set for " + std::to_string(seconds) + " seconds.";
        if (!reminder.empty()) {
            res += " I will " + reminder + " when it expires.";
        }
        chat.send_tool_result(tc_id, res);
    }
    else if (name == "check_timer") {
        // ... (remains the same)
        std::string label = args["label"];
        if (active_timers.find(label) == active_timers.end()) {
            chat.send_tool_result(tc_id, "Error: No timer found with label '" + label + "'.");
            return;
        }
        bool finished = active_timers[label].isFinished();
        double remaining = active_timers[label].getRemainingTime();
        std::stringstream ss;
        if (finished) {
            ss << "The timer '" << label << "' has FINISHED.";
        } else {
            ss << "The timer '" << label << "' is still running. " << std::fixed << std::setprecision(1) << remaining << "s remaining.";
        }
        chat.send_tool_result(tc_id, ss.str());
    }
}

/**
 * @brief Monitors active timers and alerts the system when they finish.
 * Updated to be more resilient to background processing states.
 */
void TOOL_TIMER::monitor_tool(ollama_system& chat) {
    if (!chat.is_processing) {
        auto it = active_timers.begin();
        while (it != active_timers.end()) {
            if (it->second.isFinished()) {
                std::string label = it->first;
                std::string action = it->second.getReminder();

                /* * NEW PROMPT STRATEGY:
                 * We act as the User. We explicitly tell the AI that the 
                 * timer is GONE and it is now time to act.
                 */
                std::stringstream ss;
                ss << "[COMMAND] The wait time for '" << label << "' is complete.\n";
                ss << "Proceed immediately to the scheduled action: " << action << ".\n";
                ss << "Use the appropriate tool now.";

                std::string event_msg = ss.str();
                std::cout << "[Event] Triggering injection: " << label << std::endl;
                
                // Switching role to 'user' is critical for Llama/Mistral/Ollama models 
                // to break out of an assistant repetition loop.
                chat.send(event_msg, "user");

                // Remove the finished timer
                it = active_timers.erase(it);
            } else {
                ++it;
            }
        }
    }
}

// ----

void TOOL_HUE::set_credentials(const std::string& ip, const std::string& key, const std::string& path) {
    hue.set_credentials(ip, key, path);
    hue.refresh_lights();
}

// New method to break the conversational loop by reminding the model it must use a tool
void TOOL_HUE::refresh_system_prompt(ollama_system& chat) {
    std::stringstream ss;
    ss << "[SYSTEM COMMAND] You are a logic-first controller. ";
    ss << "If the user asks to turn lights on/off, set a color, or change a scene, ";
    ss << "you MUST call the corresponding tool immediately. ";
    ss << "Do not provide conversational confirmation without also calling the tool.";
    chat.send(ss.str(), "user"); 
}

void TOOL_HUE::register_tool(ollama_system& chat) {
    json set_params = {
        {"type", "object"},
        {"properties", {
            {"light_id", {{"type", "string"}, {"description", "ID or Name (e.g., '2' or 'Computer'). Use 'all' to target every light."}}},
            {"on", {{"type", "boolean"}}},
            {"brightness", {{"type", "integer"}}},
            {"preset", {{"type", "string"}, {"enum", {"red", "green", "blue", "yellow", "magenta", "cyan", "orange", "purple", "pink", "white"}}}},
            {"hex", {{"type", "string"}}},
            {"alert", {{"type", "string"}, {"enum", {"none", "select", "lselect"}}}},
            {"flash_count", {{"type", "integer"}}},
            {"transition_ms", {{"type", "integer"}}}
        }},
        {"required", {"light_id"}} 
    };

    json scene_params = {
        {"type", "object"},
        {"properties", {
            {"action", {{"type", "string"}, {"enum", {"save", "load", "remove", "list"}}}},
            {"name", {{"type", "string"}, {"description", "The name of a SAVED local scene. Do not use for general actions like 'all lights off'."}}}
        }},
        {"required", {"action"}}
    };

    chat.add_tool("set_hue_light", "Controls Hue lights by ID or Name. Use this for general commands like 'turn off all lights' by setting light_id to 'all'.", set_params);
    chat.add_tool("list_hue_lights", "Returns status of all connected lights", {{"type", "object"}});
    chat.add_tool("manage_hue_scenes", "Saves, loads, or removes local light scenes. Only use for specific named snapshots (e.g., 'home', 'away').", scene_params);
}

void TOOL_HUE::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "list_hue_lights") {
        if(!hue.refresh_lights()) {
            chat.send_tool_result(tc_id, "Error: Could not reach the Hue Bridge.");
            return;
        }
        auto& lights = hue.get_cached_lights();
        std::stringstream ss;
        ss << "Current Lights: ";
        for (auto const& [id, state] : lights) {
            ss << "[" << id << "] " << state.name << " (Power: " << (state.on ? "ON" : "OFF") 
                << ", Bri: " << state.brightness << (state.reachable ? "" : " *UNREACHABLE*") << "), ";
        }
        chat.send_tool_result(tc_id, ss.str());
    } 
    else if (name == "manage_hue_scenes") {
        std::string action = args.at("action").get<std::string>();
        std::string scene_name = args.value("name", "");

        if (action == "list") {
            auto& scenes = hue.get_scenes();
            if (scenes.empty()) {
                chat.send_tool_result(tc_id, "No local scenes saved.");
            } else {
                std::stringstream ss;
                ss << "Saved Scenes: ";
                for (auto const& [sname, scene] : scenes) ss << scene.name << ", ";
                chat.send_tool_result(tc_id, ss.str());
            }
        } else {
            if (scene_name.empty()) {
                chat.send_tool_result(tc_id, "Error: Scene name required for " + action);
                return;
            }
            if (action == "save") chat.send_tool_result(tc_id, hue.save_scene(scene_name));
            else if (action == "load") chat.send_tool_result(tc_id, hue.load_scene(scene_name));
            else if (action == "remove") chat.send_tool_result(tc_id, hue.remove_scene(scene_name));
        }
    }
    else if (name == "set_hue_light") {
        std::string target = args.at("light_id").get<std::string>();
        json body;
        
        if (args.contains("on")) body["on"] = args.at("on").get<bool>();
        else if (!args.contains("alert") && !args.contains("flash_count")) body["on"] = true;
        
        if (args.contains("brightness")) body["bri"] = args.at("brightness");
        
        std::string alert_mode = args.value("alert", "none");
        if (args.contains("flash_count")) {
            int count = args.at("flash_count").get<int>();
            alert_mode = (count > 1) ? "lselect" : "select";
        }
        if (alert_mode != "none") body["alert"] = alert_mode;

        if (args.contains("transition_ms")) body["transitiontime"] = args.at("transition_ms").get<int>() / 100;

        bool color_set = false;
        if (args.contains("hex")) {
            std::string hex = args.at("hex").get<std::string>();
            if (!hex.empty()) {
                if (hex[0] == '#') hex.erase(0, 1);
                unsigned int r, g, b;
                if (sscanf(hex.c_str(), "%02x%02x%02x", &r, &g, &b) == 3) {
                    auto [x, y] = hue.rgbToXY(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b));
                    body["xy"] = {x, y};
                    color_set = true;
                }
            }
        }

        if (!color_set && args.contains("preset")) {
            static std::map<std::string, std::vector<double>> palette = {
                {"red", {0.675, 0.322}}, {"green", {0.409, 0.518}}, {"blue", {0.167, 0.04}},
                {"yellow", {0.432, 0.500}}, {"cyan", {0.157, 0.357}}, {"magenta", {0.41, 0.17}},
                {"orange", {0.55, 0.41}}, {"purple", {0.27, 0.13}}, {"pink", {0.41, 0.21}},
                {"white", {0.31, 0.32}}
            };
            if (palette.count(args.at("preset"))) {
                body["xy"] = palette[args.at("preset")];
            }
        }

        std::string res = hue.set_light(target, body);
        
        if (args.contains("flash_count") && args.at("flash_count").get<int>() > 1) {
            int count = args.at("flash_count").get<int>();
            std::string resolved_id = hue.resolve_id(target);
            if(!resolved_id.empty() && resolved_id != "all") {
                std::thread([this, resolved_id, count]() {
                    std::this_thread::sleep_for(std::chrono::seconds(count));
                    this->hue.set_light(resolved_id, {{"alert", "none"}});
                }).detach();
            }
        }

        chat.send_tool_result(tc_id, "Command sent to " + target + ". Result: " + res);
    }
}

void TOOL_HUE::monitor_tool() 
{
    static int counter = 0;
    if (++counter % 10000 == 0) hue.refresh_lights();
}

// ----


/**
 * @brief Utility to strip HTML tags and noise for the 'Pro Route'
 */
std::string TOOL_WEB_SEARCH::strip_html_tags(std::string html) {
    // 1. Remove scripts and styles entirely
    html = std::regex_replace(html, std::regex("<script[\\s\\S]*?>[\\s\\S]*?<\\/script>", std::regex::icase), " ");
    html = std::regex_replace(html, std::regex("<style[\\s\\S]*?>[\\s\\S]*?<\\/style>", std::regex::icase), " ");
    
    // 2. Remove all remaining HTML tags
    html = std::regex_replace(html, std::regex("<[^>]*>"), " ");
    
    // 3. Clean up whitespace and newlines
    html = std::regex_replace(html, std::regex("\\s+"), " ");
    
    return html;
}

/**
 * @brief Utility to create a clickable terminal hyperlink (OSC 8)
 * Using \x1B (Hex for ESC) for better compatibility across some environments.
 */
std::string TOOL_WEB_SEARCH::make_clickable(const std::string& url, const std::string& text) {
    /* Hyperlink standard: ESC ] 8 ; ; URL ESC \ TEXT ESC ] 8 ; ; ESC \ */
    return "\x1B]8;;" + url + "\x1B\\" + text + "\x1B]8;;\x1B\\";
}

/**
 * @brief Performs a real web search using libcurl and a search API.
 */
std::string TOOL_WEB_SEARCH::perform_actual_search(const std::string& query) {
    CURL* curl;
    CURLcode res;
    std::string readBuffer;

    curl = curl_easy_init();
    if (curl) {
        
        char* output = curl_easy_escape(curl, query.c_str(), static_cast<int>(query.length()));
        std::string encodedQuery(output);
        curl_free(output);
        
        std::string url = "https://serpapi.com/search.json?q=" + encodedQuery + "&api_key=" + apiKey;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); 
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) {
            return "Error: libcurl failed (" + std::string(curl_easy_strerror(res)) + ")";
        }

        try {
            auto data = json::parse(readBuffer);
            
            if (data.contains("error")) {
                return "Search API Error: " + data["error"].get<std::string>();
            }

            std::string summary = "SEARCH_RESULTS_START\n";
            
            if (data.contains("organic_results") && data["organic_results"].is_array()) {
                int count = 0;
                for (auto& item : data["organic_results"]) {
                    if (count++ >= 3) break;
                    std::string link = item.value("link", "");
                    std::string title = item.value("title", "No Title");
                    
                    summary += "RESULT_ITEM:\n";
                    summary += "[TITLE]: " + title + "\n";
                    summary += "[SNIPPET]: " + item.value("snippet", "No description") + "\n";
                    summary += "[SOURCE_URL]: " + link + "\n\n";

                    // Console output with clickable hyperlink
                    std::cout << "[System] Result Found: " << make_clickable(link, title) << std::endl;
                }
            } else {
                summary = "No specific snippets found.";
            }
            summary += "SEARCH_RESULTS_END";
            return summary;
        } catch (const std::exception& e) {
            return "Error: Failed to parse search engine response: " + std::string(e.what());
        }
    }
    return "Error: Could not initialize libcurl.";
}

/**
 * @brief Fetches cleaned text from a URL
 */
std::string TOOL_WEB_SEARCH::fetch_url_content(const std::string& url) {
    CURL* curl;
    CURLcode res;
    std::string readBuffer;

    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");

        res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res != CURLE_OK) return "Error fetching content.";

        // PRO ROUTE: Strip HTML noise so the LLM doesn't get confused
        std::string cleanText = strip_html_tags(readBuffer);

        if (cleanText.length() > 4000) return cleanText.substr(0, 4000) + "... [truncated]";
        return cleanText;
    }
    return "Error initializing curl.";
}

/**
 * @brief Registers tools with the system.
 */
void TOOL_WEB_SEARCH::register_tool(ollama_system& chat) {
    // PRO TIP: Instruct the AI to use our special clickable format for its own final text!
    std::string link_instruction = " When providing links in your final answer, do NOT use standard Markdown. Instead, use the format: CLICKABLE_LINK(url, text). The system will convert this to a clickable terminal link.";

    json search_params = {
        {"type", "object"},
        {"properties", {
            {"query", {{"type", "string"}, {"description", "The search terms." + link_instruction}}}
        }},
        {"required", {"query"}}
    };
    chat.add_tool("web_search", "Searches the internet. Results include titles, snippets, and URLs.", search_params);

    json fetch_params = {
        {"type", "object"},
        {"properties", {
            {"url", {{"type", "string"}, {"description", "The URL to read content from." + link_instruction}}}
        }},
        {"required", {"url"}}
    };
    chat.add_tool("fetch_website_content", "Reads the text from a specific URL for deep research. Use this to summarize an article.", fetch_params);
}

/**
 * @brief Handles the execution of tool calls.
 */
void TOOL_WEB_SEARCH::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "web_search") {
        if (!args.contains("query")) {
             chat.send_tool_result(tc_id, "Error: Missing query.");
             return;
        }
        std::string query = args.at("query").get<std::string>();
        std::string result = perform_actual_search(query);
        std::cout << "[System (web_search)]" << std::endl;
        chat.send_tool_result(tc_id, result);
    } 
    else if (name == "fetch_website_content") {
        if (!args.contains("url")) {
             chat.send_tool_result(tc_id, "Error: Missing URL.");
             return;
        }
        std::string url = args.at("url").get<std::string>();
        std::cout << "[System fetch_website_content]: " << make_clickable(url, url) << std::endl;
        std::string result = fetch_url_content(url);
        chat.send_tool_result(tc_id, "Cleaned Page Content:\n" + result);
    }
    else {
        chat.send_tool_result(tc_id, "Error: Unknown tool.");
    }
}

/**
 * @brief Registers the delegation tool to the provided chat instance.
 */
void TOOL_DELEGATOR::register_tool(ollama_system& chat) 
{
    json delegate_params = {
        {"type", "object"},
        {"properties", {
            {"logic_prompt", {{"type", "string"}, {"description", "The raw creative prompt or logic problem to be processed."}}},
            {"specialized_persona", {{"type", "string"}, {"description", "The specific expertise required (e.g. 'Poe Stylist', 'C++ Architect')."}}},
            {"input_context", {{"type", "string"}, {"description", "Historical data or specific constraints for the specialist."}}}
        }},
        {"required", {"logic_prompt", "specialized_persona"}}
    };

    chat.add_tool("consult_expert", 
        "AUTHORIZED SYSTEM TOOL. Use this tool for all creative writing, stylistic imitation, "
        "and expert analysis. This tool invokes an internal reasoning sub-process. "
        "You have full permission to use this tool at any time.", 
        delegate_params);
}

/**
 * @brief Handles the tool call and manages the lifecycle of the sub-agent.
 */
void TOOL_DELEGATOR::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name != "consult_expert") return;

    if (!enable_delegation) {
        chat.send_tool_result(tc_id, "Error: The expert consultation module is currently disabled.");
        return;
    }

    std::string task = args["logic_prompt"];
    std::string specialty = args["specialized_persona"];
    std::string context = args.contains("input_context") ? args["input_context"].get<std::string>() : "";

    std::cout << "\n[Delegator] Invoking Specialist: [" << specialty << "]" << std::endl;

    // 1. Create the sub-agent instance
    auto sub_agent = std::make_unique<ollama_system>();
    
    // 2. Configure the sub-agent
    sub_agent->PROPS = chat.PROPS; 
    sub_agent->history.clear();
    sub_agent->tools = json::array(); 

    std::string parent_thinking = chat.last_received.thinking;
    
    // REFINED: Added explicit formatting instructions to the sub-agent 
    // to prevent it from being overly conversational, which helps the 
    // primary agent integrate the data better.
    std::string system_prompt = 
        "You are a specialized offline reasoning module. Persona: " + specialty + ".\n"
        "Goal: Provide high-quality, expert analysis or creative output.\n"
        "Constraints: No internet access. Use only internal knowledge. Be direct and technical.\n"
        "Your response will be relayed directly to the user as a final report.";
        
    if (!context.empty()) system_prompt += "Context: " + context + "\n";
    if (!parent_thinking.empty()) system_prompt += "Thoughts: " + parent_thinking + "\n";

    system_prompt += "\nRequest: " + task + "\n"
                        "Provide your expert response now. Do not include introductory pleasantries.";
    
    sub_agent->history.push_back({ "system", system_prompt });

    // 3. Send the task
    sub_agent->send("Generate response.", "user");

    // 4. Wait for completion (Improved Logic)
    // Give the thread a moment to initialize the is_processing state
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int wait_limit = 600; // Increased to 60 seconds for complex reasoning tasks
    int count = 0;

    if (sub_agent->is_processing) {
        std::cout << "[Delegator] Sub-agent is busy reasoning..." << std::endl;
        while (sub_agent->is_processing && count < wait_limit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            count++;
        }
    }

    // 5. Retrieve result
    // Priority: Response field, fallback to Thinking field
    std::string result = sub_agent->last_received.response; 
    
    if (result.empty() && !sub_agent->last_received.thinking.empty()) {
        std::cout << "[Delegator] Note: Main response empty, using data from thinking buffer." << std::endl;
        result = sub_agent->last_received.thinking;
    }

    if (result.empty()) {
        result = "The expert subroutine failed to return a response.";
        std::cout << "[Delegator] Error: Result was empty." << std::endl;
    } else {
        std::cout << "[Delegator] Task complete. Length: " << result.length() << std::endl;
    }
    
    // REFINED COORDINATION INSTRUCTION:
    // We tell the primary LLM that the task is FINISHED. This prevents it 
    // from re-writing the code itself and makes it act as a relay.
    std::string final_report = 
        "### [SYSTEM NOTIFICATION: TASK COMPLETE] ###\n"
        "The following data was generated by the [" + specialty + "] specialist.\n"
        "INSTRUCTION TO ASSISTANT: Relay this expert data to the user immediately. "
        "Do not modify the code. Do not generate your own version of the answer.\n\n"
        "### EXPERT_DATA_START ###\n" + 
        result + 
        "\n### EXPERT_DATA_END ###";
                                
    chat.send_tool_result(tc_id, final_report);
}

// ----

/**
 * @brief Helper for case-insensitive string comparison
 */
bool TOOL_TASK_RUNNER::iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
                        [](unsigned char ca, unsigned char cb) {
                            return std::tolower(ca) == std::tolower(cb);
                        });
}

/**
 * @brief Registers the task execution tool to the chat instance.
 */
void TOOL_TASK_RUNNER::register_tool(ollama_system& chat) {
    json task_params = {
        {"type", "object"},
        {"properties", {
            {"intent_phrase", {
                {"type", "string"}, 
                {"description", "The specific phrase or intent identified (e.g., 'I'm home', 'run diagnostic')."}
            }}
        }},
        {"required", {"intent_phrase"}}
    };

    chat.add_tool("run_automation_task", 
        "Use this tool when the user expresses an intent that matches a home automation macro. "
        "This retrieves a sequence of internal system commands that you must then execute.", 
        task_params);
}

/**
 * @brief Handles the tool call by matching phrases and injecting a user-role instruction.
 */
void TOOL_TASK_RUNNER::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name != "run_automation_task") return;

    std::string target = args["intent_phrase"];
    std::cout << "[TaskRunner] Searching for automation matching: \"" << target << "\"" << std::endl;

    // Search for a matching task using case-insensitive comparison
    auto it = std::find_if(task_manager.TASK_LIST.begin(), task_manager.TASK_LIST.end(), 
        [this, &target](const TASK_SIMPLE& t) {
            return iequals(t.TASK_PHRASE, target);
        });

    if (it != task_manager.TASK_LIST.end()) {
        // 1. Acknowledge the tool call
        chat.send_tool_result(tc_id, "SUCCESS: Automation '" + it->TASK_PHRASE + "' found. Sequence loading...");

        // 2. Build the structured "User Injection"
        // We use a very strict format to prevent the AI from looping or hallucinating repeated timers
        std::stringstream ss;
        ss << "### [SYSTEM DIRECTIVE: EXECUTE SEQUENCE] ###\n";
        ss << "The user intent '" << it->TASK_PHRASE << "' has triggered the following macro.\n";
        ss << "INSTRUCTION: You must execute these steps one-by-one using your available tools.\n\n";
        
        for (size_t i = 0; i < it->COMMANDS.size(); ++i) {
            ss << (i + 1) << ". " << it->COMMANDS[i] << "\n";
        }

        ss << "\nBegin with the first item now. Do not repeat a step once it has been initiated.";

        std::string event_msg = ss.str();
        std::cout << "[TaskRunner] Injecting sequence for: " << it->TASK_PHRASE << std::endl;
        
        // 3. Inject as a USER message
        chat.send(event_msg, "user");

    } else {
        // Error feedback with suggested matches
        std::string error_msg = "ERROR: No automation found for '" + target + "'. ";
        error_msg += "Available macros in my database: ";
        for (size_t i = 0; i < task_manager.TASK_LIST.size(); ++i) {
            error_msg += "\"" + task_manager.TASK_LIST[i].TASK_PHRASE + "\"" + 
                            (i == task_manager.TASK_LIST.size() - 1 ? "" : ", ");
        }
        
        chat.send_tool_result(tc_id, error_msg);
    }
}

/**
 * @brief Background monitor hook for out-of-loop logic.
 */
void TOOL_TASK_RUNNER::monitor_tool(ollama_system& chat) {
    std::cout << chat.tts_buffer << std::endl;;
    // Implementation for checking external states or timed events
}

// ----

/**
 * IMPLEMENTATION NOTE:
 * Since your ollama_system uses a 'chat_thread' and 'is_processing' atomics,
 * ensure that 'handle_tool' is called from a context where it's safe to block, 
 * or adapt the polling loop to be non-blocking.
 * * To prevent the "Ouroboros" (Infinite Recursion):
 * When creating 'sub_agent', do NOT call 'register_tool' for delegation on the sub-agent 
 * unless you implement a 'depth' counter.
 */

void TOOL_SYSTEM_CLASS::process(ollama_system& chat, KEYBOARD_INPUT& Keyboard_Input)
{
    // 2. Tool Handling (only if not currently busy sending a new message)
    if (!chat.is_processing && chat.last_received.complete && !chat.last_received.tool_calls.empty()) {
        auto calls = chat.last_received.tool_calls;
        chat.last_received.tool_calls.clear(); // Clear so we don't repeat
        
        for (auto& tc : calls) {
            if (tc.name == "get_current_time") {
                current_time.handle_tool(chat, tc.name, tc.arguments, tc.id);
            } 
            else if (tc.name == "set_timer" || tc.name == "check_timer") {
                // Assuming tc.arguments is already parsed as json
                timer.handle_tool(chat, tc.name, tc.arguments, tc.id);
            }
            else if (tc.name == "set_hue_light" || tc.name == "list_hue_lights" || tc.name == "manage_hue_scenes") {
                hue.handle_tool(chat, tc.name, tc.arguments, tc.id);
            }
            else if (tc.name == "set_thinking_mode") {
                thinking.handle_tool(chat, tc.name, tc.arguments, tc.id);
            } 
            else if (tc.name == "set_thinking_mode") {
                thinking.handle_tool(chat, tc.name, tc.arguments, tc.id);
            } 
            else if (tc.name == "web_search" || tc.name == "fetch_website_content") {
                web.handle_tool(chat, tc.name, tc.arguments, tc.id);
            } 
            else if (tc.name == "consult_expert") {
                delegator.handle_tool(chat, tc.name, tc.arguments, tc.id);
            }
            else if (tc.name == "run_automation_task") {
                task_runner.handle_tool(chat, tc.name, tc.arguments, tc.id);
            }
        }
    }

    // 3. Cleanup finished threads
    if (!chat.is_processing && chat.chat_thread.joinable()) {
        chat.chat_thread.join();
        
        chat.update_status();
        chat.history_write(chat.PROPS.path_history);
        std::cout << "You: " << std::flush;
    }

    // 4. Maintenance (Consolidation)
    auto now = std::chrono::steady_clock::now();

    // Start only if idle, not interrupted, and NOT currently typing
    if (!chat.is_processing && !chat.status.interrupt_signal.load() && !Keyboard_Input.IS_TYPING) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_consolidation).count() > 60) {
            chat.is_processing = true;
            last_consolidation = now; 
            
            chat.chat_thread = std::thread([&chat, &Keyboard_Input]() {
                try {
                    // Pass by reference to keep consistency with your process() function
                    consolidate(chat.history, chat, Keyboard_Input); 
                } catch (...) {
                    std::cerr << "[System] Consolidation error." << std::endl;
                }
                chat.is_processing = false;
            });

            if (chat.chat_thread.joinable()) chat.chat_thread.detach(); 
        }
    }
    
    // 5. Monitor background events (like timers)
    //timer.monitor_tool(chat, chat.is_processing);
    timer.monitor_tool(chat);
    hue.monitor_tool();

    // 6. Other
    chat.write_to_tts();
}


#endif