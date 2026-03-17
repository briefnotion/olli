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
 * REFINED CLOCK TOOL
 * Now uses the integrate_tool_result pattern and correctly handles args.
 */
void TOOL_GET_CURRENT_TIME::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "get_current_time") {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        
        // Use args to determine format if provided, otherwise default
        std::string format = "%Y-%m-%d %H:%M:%S";
        if (args.contains("format") && args["format"].is_string()) {
            format = args["format"];
        }

        std::stringstream ss;
        ss << std::put_time(std::localtime(&now_time), format.c_str());
        std::string current_time_str = ss.str();

        // 1. Acknowledge the tool call internally
        chat.send_tool_result(tc_id, "System Time: " + current_time_str);

        // 2. INTEGRATION: Ask the instance to wrap this in persona
        chat.integrate_tool_result("Current Time is " + current_time_str);
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

/**
 * REFINED TIMER TOOL
 * Handles timer logic and persona-based notifications.
 */
void TOOL_TIMER::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "set_timer") {
        std::cout << "[System (set_timer)]" << std::endl;
        std::string label = args["label"];
        double seconds = args["seconds"];
        
        std::string reminder = "";
        if (args.contains("reminder") && !args["reminder"].is_null()) {
            reminder = args["reminder"];
        }
        
        TIMER_SIMPLE new_timer(seconds, reminder);
        new_timer.start();
        active_timers[label] = new_timer;

        std::string res = "Timer '" + label + "' set for " + std::to_string(seconds) + " seconds.";
        if (!reminder.empty()) {
            res += " Reminder set: " + reminder;
        }

        // 1. Silent history record
        chat.send_tool_result(tc_id, res);

        // 2. Persona Integration
        chat.integrate_tool_result(res);
    }
    else if (name == "check_timer") {
        std::string label = args["label"];
        if (active_timers.find(label) == active_timers.end()) {
            std::string err = "Error: No timer found with label '" + label + "'.";
            chat.send_tool_result(tc_id, err);
            chat.integrate_tool_result(err);
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
        std::string res = ss.str();
        chat.send_tool_result(tc_id, res);
        chat.integrate_tool_result(res);
    }
}

/**
 * REFINED TIMER MONITOR
 * Alerts the system via persona when a timer expires.
 */
void TOOL_TIMER::monitor_tool(ollama_system& chat) {
    if (!chat.is_processing) {
        auto it = active_timers.begin();
        while (it != active_timers.end()) {
            if (it->second.isFinished()) {
                std::string label = it->first;
                std::string action = it->second.getReminder();

                std::stringstream ss;
                ss << "### [TIMER EXPIRED] ###\n";
                ss << "The wait time for '" << label << "' is complete.\n";
                if (!action.empty()) {
                    ss << "Target action: " << action << ".\n";
                }
                ss << "Inform the user in character.";

                std::string event_msg = ss.str();
                std::cout << "[Event] Triggering persona alert: " << label << std::endl;
                
                // Integrate via persona to maintain conversation flow
                chat.integrate_tool_result(event_msg);

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

/**
 * REFINED HUE TOOL
 * Applies the integration pattern to lighting controls.
 */
void TOOL_HUE::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "list_hue_lights") {
        if(!hue.refresh_lights()) {
            std::string err = "Error: Could not reach the Hue Bridge.";
            chat.send_tool_result(tc_id, err);
            chat.integrate_tool_result(err);
            return;
        }
        auto& lights = hue.get_cached_lights();
        std::stringstream ss;
        ss << "Current Lights: ";
        for (auto const& [id, state] : lights) {
            ss << "[" << id << "] " << state.name << " (Power: " << (state.on ? "ON" : "OFF") 
                << ", Bri: " << state.brightness << (state.reachable ? "" : " *UNREACHABLE*") << "), ";
        }
        std::string result = ss.str();
        chat.send_tool_result(tc_id, result);
        chat.integrate_tool_result(result);
    } 
    else if (name == "manage_hue_scenes") {
        std::string action = args.at("action").get<std::string>();
        std::string scene_name = args.value("name", "");

        if (action == "list") {
            auto& scenes = hue.get_scenes();
            if (scenes.empty()) {
                std::string msg = "No local scenes saved.";
                chat.send_tool_result(tc_id, msg);
                chat.integrate_tool_result(msg);
            } else {
                std::stringstream ss;
                ss << "Saved Scenes: ";
                for (auto const& [sname, scene] : scenes) ss << scene.name << ", ";
                std::string result = ss.str();
                chat.send_tool_result(tc_id, result);
                chat.integrate_tool_result(result);
            }
        } else {
            if (scene_name.empty()) {
                std::string err = "Error: Scene name required for " + action;
                chat.send_tool_result(tc_id, err);
                chat.integrate_tool_result(err);
                return;
            }
            std::string res;
            if (action == "save") res = hue.save_scene(scene_name);
            else if (action == "load") res = hue.load_scene(scene_name);
            else if (action == "remove") res = hue.remove_scene(scene_name);
            
            chat.send_tool_result(tc_id, res);
            chat.integrate_tool_result("Scene " + action + " operation: " + res);
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

        std::string summary = "Light command for " + target + " processed. Result: " + res;
        chat.send_tool_result(tc_id, summary);
        chat.integrate_tool_result(summary);
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
 * REFINED WEB SEARCH TOOL
 * Integrates search results and page content into the persona.
 */
void TOOL_WEB_SEARCH::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "web_search") {
        if (!args.contains("query")) {
            std::string err = "Error: Missing query.";
            chat.send_tool_result(tc_id, err);
            chat.integrate_tool_result(err);
            return;
        }
        std::string query = args.at("query").get<std::string>();
        std::string result = perform_actual_search(query);
        
        // Internal record
        chat.send_tool_result(tc_id, result);
        
        // Persona response
        chat.integrate_tool_result("Search results for '" + query + "': " + result);
    } 
    else if (name == "fetch_website_content") {
        if (!args.contains("url")) {
            std::string err = "Error: Missing URL.";
            chat.send_tool_result(tc_id, err);
            chat.integrate_tool_result(err);
            return;
        }
        std::string url = args.at("url").get<std::string>();
        std::string result = fetch_url_content(url);
        
        // Internal record
        chat.send_tool_result(tc_id, "Cleaned Page Content from " + url + ":\n" + result);
        
        // Persona response
        chat.integrate_tool_result("I have fetched and processed the content from " + url + ". Here is the information retrieved: " + result);
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
 * REFINED DELEGATOR TOOL
 * Handles the expert consultation and integrates the report via persona.
 */
void TOOL_DELEGATOR::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name != "consult_expert") return;

    if (!enable_delegation) {
        std::string err = "Error: The expert consultation module is currently disabled.";
        chat.send_tool_result(tc_id, err);
        chat.integrate_tool_result(err);
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

    // 4. Wait for completion
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    int wait_limit = 600; 
    int count = 0;

    if (sub_agent->is_processing) {
        std::cout << "[Delegator] Sub-agent is busy reasoning..." << std::endl;
        while (sub_agent->is_processing && count < wait_limit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            count++;
        }
    }

    // 5. Retrieve result
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
    
    // 6. Integration: Store silent history and speak via persona
    std::string final_report = 
        "### [SYSTEM NOTIFICATION: TASK COMPLETE] ###\n"
        "Specialist: [" + specialty + "]\n"
        "Expert Data:\n" + result;
                                
    chat.send_tool_result(tc_id, final_report);
    chat.integrate_tool_result("The " + specialty + " expert has finished their analysis. Here is the report: " + result);
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
 * REFINED TASK RUNNER TOOL
 * Handles the automation sequence by injecting a directive into the persona flow.
 */
void TOOL_TASK_RUNNER::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name != "run_automation_task") return;

    std::string target = args["intent_phrase"];
    std::cout << "[TaskRunner] Searching for automation matching: \"" << target << "\"" << std::endl;

    auto it = std::find_if(task_manager.TASK_LIST.begin(), task_manager.TASK_LIST.end(), 
        [this, &target](const TASK_SIMPLE& t) {
            return iequals(t.TASK_PHRASE, target);
        });

    if (it != task_manager.TASK_LIST.end()) {
        // 1. Silent history record
        chat.send_tool_result(tc_id, "SUCCESS: Automation '" + it->TASK_PHRASE + "' found. Sequence loading...");

        // 2. Build the directive
        std::stringstream ss;
        ss << "### [SYSTEM DIRECTIVE: EXECUTE SEQUENCE] ###\n";
        ss << "Trigger: '" << it->TASK_PHRASE << "'.\n";
        ss << "Commands:\n";
        for (size_t i = 0; i < it->COMMANDS.size(); ++i) {
            ss << (i + 1) << ". " << it->COMMANDS[i] << "\n";
        }
        ss << "\nInitiate the first step and narrate your progress.";

        // 3. Integration via persona
        chat.integrate_tool_result(ss.str());

    } else {
        std::string error_msg = "ERROR: No automation found for '" + target + "'. ";
        chat.send_tool_result(tc_id, error_msg);
        chat.integrate_tool_result(error_msg);
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
 * UPDATED TOOL_SYSTEM_CLASS::PROCESS
 * Now handles a vector of background tasks stored as unique_ptrs to avoid
 * move/copy errors with atomics and threads.
 */
void TOOL_SYSTEM_CLASS::process(ollama_system& chat, std::vector<std::unique_ptr<ollama_system>>& tasks, KEYBOARD_INPUT& Keyboard_Input)
{
    // --- PART A: Handle Tools for ALL active instances (Main + Tasks) ---
    auto handle_instance_tools = [&](ollama_system& instance) {
        if (!instance.is_processing && instance.last_received.complete && !instance.last_received.tool_calls.empty()) 
        {
            auto calls = instance.last_received.tool_calls;
            instance.last_received.tool_calls.clear(); 
            
            for (auto& tc : calls) {
                // Logic remains same, but operates on 'instance' instead of 'chat'
                if (tc.name == "get_current_time") current_time.handle_tool(instance, tc.name, tc.arguments, tc.id);
                else if (tc.name == "set_timer" || tc.name == "check_timer") timer.handle_tool(instance, tc.name, tc.arguments, tc.id);
                else if (tc.name == "set_hue_light" || tc.name == "list_hue_lights" || tc.name == "manage_hue_scenes") hue.handle_tool(instance, tc.name, tc.arguments, tc.id);
                else if (tc.name == "set_thinking_mode") thinking.handle_tool(instance, tc.name, tc.arguments, tc.id);
                else if (tc.name == "web_search" || tc.name == "fetch_website_content") web.handle_tool(instance, tc.name, tc.arguments, tc.id);
                else if (tc.name == "consult_expert") delegator.handle_tool(instance, tc.name, tc.arguments, tc.id);
                else if (tc.name == "run_automation_task") task_runner.handle_tool(instance, tc.name, tc.arguments, tc.id);
            }
        }
    };

    // Process main chat tools
    handle_instance_tools(chat);

    // Process tools and cleanup for background tasks
    for (auto it = tasks.begin(); it != tasks.end(); ) {
        ollama_system& task_instance = **it; // Dereference unique_ptr

        // 1. Handle tool calls requested by the background task
        handle_instance_tools(task_instance);

        // 2. Join the thread if the task finished its network call
        if (!task_instance.is_processing && task_instance.chat_thread.joinable()) {
            task_instance.chat_thread.join();
        }

        // 3. Check if the task is "Done" (No more tools to call, and response is ready)
        if (!task_instance.is_processing && task_instance.last_received.complete && task_instance.last_received.tool_calls.empty()) {
            // INJECT RESULT BACK TO MAIN CHAT
            if (!task_instance.last_received.response.empty()) {
                std::string task_report = "[Task Update]: " + task_instance.last_received.response;
                chat.send(task_report, "system"); 
            }
            
            it = tasks.erase(it); // Remove finished task (unique_ptr deletes the object)
        } else {
            ++it;
        }
    }

    // --- PART B: Main Chat Maintenance (Thread joining for user messages) ---
    if (!chat.is_processing && chat.chat_thread.joinable()) {
        chat.chat_thread.join();
        chat.update_status();
        chat.history_write(chat.PROPS.path_history);
        std::cout << "You: " << std::flush;
    }

    // --- PART C: Maintenance & Monitors ---
    auto now = std::chrono::steady_clock::now();
    if (!chat.is_processing && !chat.status.interrupt_signal.load() && !Keyboard_Input.IS_TYPING) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_consolidation).count() > 60) {
            chat.is_processing = true;
            last_consolidation = now; 
            chat.chat_thread = std::thread([&chat, &Keyboard_Input]() {
                try { consolidate(chat.history, chat, Keyboard_Input); } catch (...) {}
                chat.is_processing = false;
            });
            if (chat.chat_thread.joinable()) chat.chat_thread.detach(); 
        }
    }
    
    timer.monitor_tool(chat);
    hue.monitor_tool();
    chat.write_to_tts();
}


#endif