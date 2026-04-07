#ifndef olla_cpp
#define olla_cpp

#include "olla.h"

void add_tool(json& tools, const std::string& name, const std::string& description, json parameters) 
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

/**
 * @brief Registers the set_thinking_mode tool with the system.
 */
void TOOL_SET_THINKING_MODE::register_tool(json& tools) {
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

    add_tool(tools, "set_thinking_mode", "Enables or disables the internal reasoning/thinking process for the model", set_thinking_params);
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
void TOOL_GET_CURRENT_TIME::register_tool(json& tools) {
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

    add_tool(tools, "get_current_time", "Returns the current system date and time", get_time_params);
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
        chat.integrate_tool_result("", "Current Time is " + current_time_str);
    }
}

// ---

void TOOL_TIMER::register_tool(json& tools) {
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

    add_tool(tools, "set_timer", "Starts a countdown and schedules a future action (optional)", set_timer_params);
    add_tool(tools, "check_timer", "Checks if a specific named timer has finished", check_timer_params);
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
        chat.integrate_tool_result("", res);
    }
    else if (name == "check_timer") {
        std::string label = args["label"];
        if (active_timers.find(label) == active_timers.end()) {
            std::string err = "Error: No timer found with label '" + label + "'.";
            chat.send_tool_result(tc_id, err);
            chat.integrate_tool_result("", err);
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
        chat.integrate_tool_result("", res);
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
                chat.integrate_tool_result("", event_msg);

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

void TOOL_HUE::register_tool(json& tools) {
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

    add_tool(tools, "set_hue_light", "Controls Hue lights by ID or Name. Use this for general commands like 'turn off all lights' by setting light_id to 'all'.", set_params);
    add_tool(tools, "list_hue_lights", "Returns status of all connected lights", {{"type", "object"}});
    add_tool(tools, "manage_hue_scenes", "Saves, loads, or removes local light scenes. Only use for specific named snapshots (e.g., 'home', 'away').", scene_params);
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
            chat.integrate_tool_result("", err);
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
        chat.integrate_tool_result("", result);
    } 
    else if (name == "manage_hue_scenes") {
        std::string action = args.at("action").get<std::string>();
        std::string scene_name = args.value("name", "");

        if (action == "list") {
            auto& scenes = hue.get_scenes();
            if (scenes.empty()) {
                std::string msg = "No local scenes saved.";
                chat.send_tool_result(tc_id, msg);
                chat.integrate_tool_result("", msg);
            } else {
                std::stringstream ss;
                ss << "Saved Scenes: ";
                for (auto const& [sname, scene] : scenes) ss << scene.name << ", ";
                std::string result = ss.str();
                chat.send_tool_result(tc_id, result);
                chat.integrate_tool_result("", result);
            }
        } else {
            if (scene_name.empty()) {
                std::string err = "Error: Scene name required for " + action;
                chat.send_tool_result(tc_id, err);
                chat.integrate_tool_result("", err);
                return;
            }
            std::string res;
            if (action == "save") res = hue.save_scene(scene_name);
            else if (action == "load") res = hue.load_scene(scene_name);
            else if (action == "remove") res = hue.remove_scene(scene_name);
            
            chat.send_tool_result(tc_id, res);
            chat.integrate_tool_result("", "Scene " + action + " operation: " + res);
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
        chat.integrate_tool_result("", summary);
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
void TOOL_WEB_SEARCH::register_tool(json& tools) {
    // PRO TIP: Instruct the AI to use our special clickable format for its own final text!
    std::string link_instruction = " When providing links in your final answer, do NOT use standard Markdown. Instead, use the format: CLICKABLE_LINK(url, text). The system will convert this to a clickable terminal link.";

    json search_params = {
        {"type", "object"},
        {"properties", {
            {"query", {{"type", "string"}, {"description", "The search terms." + link_instruction}}}
        }},
        {"required", {"query"}}
    };
    add_tool(tools, "web_search", "Searches the internet. Results include titles, snippets, and URLs.", search_params);

    json fetch_params = {
        {"type", "object"},
        {"properties", {
            {"url", {{"type", "string"}, {"description", "The URL to read content from." + link_instruction}}}
        }},
        {"required", {"url"}}
    };
    add_tool(tools, "fetch_website_content", "Reads the text from a specific URL for deep research. Use this to summarize an article.", fetch_params);
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
            chat.integrate_tool_result("", err);
            return;
        }
        std::string query = args.at("query").get<std::string>();
        std::string result = perform_actual_search(query);
        
        // Internal record
        chat.send_tool_result(tc_id, result);
        
        // Persona response
        chat.integrate_tool_result("", "Search results for '" + query + "': " + result);
    } 
    else if (name == "fetch_website_content") {
        if (!args.contains("url")) {
            std::string err = "Error: Missing URL.";
            chat.send_tool_result(tc_id, err);
            chat.integrate_tool_result("", err);
            return;
        }
        std::string url = args.at("url").get<std::string>();
        std::string result = fetch_url_content(url);
        
        // Internal record
        chat.send_tool_result(tc_id, "Cleaned Page Content from " + url + ":\n" + result);
        
        // Persona response
        chat.integrate_tool_result("", "I have fetched and processed the content from " + url + ". Here is the information retrieved: " + result);
    }
    else {
        chat.send_tool_result(tc_id, "Error: Unknown tool.");
    }
}


/**
 * @brief Registers the delegation tool to the provided chat instance.
 */
/*
void TOOL_DELEGATOR::register_tool(json& tools) 
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

    add_tool(tools, "consult_expert", 
        "AUTHORIZED SYSTEM TOOL. Use this tool for all creative writing, stylistic imitation, "
        "and expert analysis. This tool invokes an internal reasoning sub-process. "
        "You have full permission to use this tool at any time.", 
        delegate_params);
}
*/

/**
 * REFINED DELEGATOR TOOL
 * Handles the expert consultation and integrates the report via persona.
 */

/*
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
*/

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
void TOOL_TASK_RUNNER::register_tool(json& tools) 
{
    // ...

    json task_params = {
        {"type", "object"},
        {"properties", {
            {"intent_phrase", {
                {"type", "string"}, 
                {"description", "The specific phrase or intent identified (e.g.,  'run system test', 'run process resume')."}
            }}
        }},
        {"required", {"intent_phrase"}}
    };

    add_tool(tools, "run_automation_task", 
        "Use this tool when the user expresses an intent that matches a home automation macro. "
        "This retrieves a sequence of internal system commands that you must then execute.", 
        task_params);
}

/**
 * REFINED TASK RUNNER TOOL
 * * PURPOSE:
 * This routine handles a specific tool call named "run_automation_task".
 * It searches for a pre-defined sequence of commands (an "automation") 
 * based on a user's intent, and then injects those commands back into 
 * the chat system as a structured directive.
 */

void TOOL_TASK_RUNNER::handle_tool(
    ollama_system& main_instance, 
    ollama_system& instance, 
    const std::string& tool_name, 
    const json& tool_args, 
    const std::string& call_id) 
{
    bool running_directory = false;
    std::filesystem::path working_dir;

    KEYBOARD_INPUT keyboard_input;
    keyboard_input.PROPS.ENABLED = false;
    keyboard_input.PROPS.ENABLE_LIRA_VOCA = false;

    // ---------------------------------------------------------
    // 1. GUARD CLAUSE
    // Verify this is the specific tool we are meant to handle.
    // ---------------------------------------------------------
    if (tool_name != "run_automation_task") 
    {
        return;
    }

    // ---------------------------------------------------------
    // 2. EXTRACT INPUT
    // Get the search phrase (the user's intent) from the JSON arguments.
    // ---------------------------------------------------------
    std::string intent_phrase = tool_args["intent_phrase"];
    std::cout << "[TaskRunner] Searching for automation matching: \"" << intent_phrase << "\"" << std::endl;

    // ---------------------------------------------------------
    // 3. SEARCH FOR THE TASK
    // Look through the available task list for a phrase match (case-insensitive).
    // ---------------------------------------------------------
    auto task_it = std::find_if(
        task_manager.TASK_LIST.begin(), 
        task_manager.TASK_LIST.end(), 
        [this, &intent_phrase](const TASK_SIMPLE& task) {
            // 'iequals' is assumed to be a helper for case-insensitive string comparison
            return iequals(task.TASK_PHRASE, intent_phrase);
        }
    );

    // ---------------------------------------------------------
    // 4. PROCESS THE SEARCH RESULT
    // ---------------------------------------------------------
    bool task_found = (task_it != task_manager.TASK_LIST.end());

    if (task_found) 
    {
        const auto& found_task = *task_it;

        // Preparation: Create a new instance to run the automation sequence without blocking the main chat flow.
        if (!instance.OLLAMA_OPENING.empty())
            instance.OLLAMA_OPENING = found_task.TASK_PURPOSE; // place for custoom opening if needed

        instance.PROPS.stream_output = false;
        instance.TOOL_PERMISSIONS = found_task.TOOL_PERMISSIONS;
        
        // NONE
        
        instance.open(main_instance.PROPS);

        // Create directory if needed.
        if (found_task.TASK_DIRECTORY != "")
        {
            working_dir = OLLI_DIRECTORY / (found_task.TASK_DIRECTORY + "_" + call_id);
            std::filesystem::create_directories(working_dir);
            running_directory = true;
        }

        // A. Confirm to the system that the tool executed successfully
        //std::string success_log = "SUCCESS: Automation '" + found_task.TASK_PHRASE + "' found. Sequence loading...";
        std::string success_log = "SUCCESS: Automation found. Sequence loading...";
        main_instance.send_tool_result(call_id, success_log);

        // List each command numerically
        //for (const auto& cmd : found_task.COMMANDS) 
        for (size_t i = 0; i < found_task.COMMANDS.size(); ++i) 
        {
            
            if (starts_with(found_task.COMMANDS[i], "[[ENTER TO CONTINUE]]"))
            {
                cout << "\n-" << i << "--------------------------\nPRESS ENTER TO CONTINUE" << endl;
                keyboard_input.PROPS.ENABLED = true;
                while(keyboard_input.ENTER_PRESSED == false)
                {
                    keyboard_input.keyboard_input();
                }
                keyboard_input.ENTER_PRESSED = false;
                keyboard_input.PROPS.ENABLED = false;
            }
            else if (starts_with(found_task.COMMANDS[i], "[[ASK]]"))
            {
                std::cout <<"\n-" << i << "--------------------------\nREQUEST: " << found_task.COMMANDS[i] << std::endl;
                keyboard_input.PROPS.ENABLED = true;
                while(keyboard_input.ENTER_PRESSED == false)
                {
                    keyboard_input.keyboard_input();
                }
                keyboard_input.ENTER_PRESSED = false;
                keyboard_input.PROPS.ENABLED = false;
                instance.send(keyboard_input.LINE, "user");
            }
            else
            {
                std::cout <<"\n-" << i << "--------------------------\nINPUT: " << found_task.COMMANDS[i] << std::endl;
                instance.send(found_task.COMMANDS[i]);
            }

            instance.process(keyboard_input);
            instance.last_received.complete = false;
            //std::cout << "AI Result: " << instance.last_received.response << std::endl;
        }

        {
            success_log = "SUCCESS: Automation Complete";
            main_instance.send_tool_result(call_id, success_log);
            main_instance.integrate_tool_result("", instance.gather_history());
        }

        if (running_directory) 
        {
            //cout << "REMOVE ALL: " << working_dir << endl;
            std::filesystem::remove_all(working_dir);
        }
    } 
    else 
    {
        // ---------------------------------------------------------
        // 5. ERROR HANDLING
        // If no matching task is found, report the failure to the chat system.
        // ---------------------------------------------------------
        std::string error_msg = "ERROR: No automation found for '" + intent_phrase + "'.";
        
        main_instance.send_tool_result(call_id, error_msg);
        main_instance.integrate_tool_result("", error_msg);
    }
}

/**
 * @brief Background monitor hook for out-of-loop logic.
 */
void TOOL_TASK_RUNNER::monitor_tool(ollama_system& instance) {
    std::cout << instance.tts_buffer << std::endl;;
    // Implementation for checking external states or timed events
}

// ---------------------------------------------------------
// TOOL DISPATCHER (Class Member)
// Checks if an instance (Main or Task) has pending tool 
// requests and routes them to the correct tool handlers.
// ---------------------------------------------------------
void ollama_system::handle_instance_tools(KEYBOARD_INPUT& Keyboard_Input) 
{
    bool is_ready_for_tools = !is_processing && 
                              last_received.complete && 
                              !last_received.tool_calls.empty();

    if (is_ready_for_tools) {
        // Take ownership of the calls and clear the queue
        auto pending_calls = last_received.tool_calls;
        last_received.tool_calls.clear(); 
        
        for (auto& tc : pending_calls) {
            if (tc.name == "get_current_time") 
            {
                if (TOOL_PERMISSIONS.CURRENT_TIME)
                    current_time.handle_tool(*this, tc.name, tc.arguments, tc.id);
                else
                {
                    std::string error_msg = 
                    "Error: Tool '" + tc.name + "' is not enabled.";
                    send_tool_result(tc.id, error_msg);

                }
            } 
            else if (tc.name == "set_timer" || tc.name == "check_timer") 
            {
                if (TOOL_PERMISSIONS.TIMER)
                    timer.handle_tool(*this, tc.name, tc.arguments, tc.id);
                else
                {
                    std::string error_msg = 
                    "Error: Tool '" + tc.name + "' is not enabled.";
                    send_tool_result(tc.id, error_msg);
                }
            }
            else if (tc.name == "set_hue_light" || tc.name == "list_hue_lights" || tc.name == "manage_hue_scenes") 
            {
                if (TOOL_PERMISSIONS.HUE)
                    hue.handle_tool(*this, tc.name, tc.arguments, tc.id);
                else
                {                   
                    std::string error_msg = 
                    "Error: Tool '" + tc.name + "' is not enabled.";
                    send_tool_result(tc.id, error_msg);}
            }
            else if (tc.name == "set_thinking_mode") 
            {
                if (TOOL_PERMISSIONS.THINKING)
                    thinking.handle_tool(*this, tc.name, tc.arguments, tc.id);
                else
                {
                    std::string error_msg = 
                    "Error: Tool '" + tc.name + "' is not enabled.";
                    send_tool_result(tc.id, error_msg);
                }
            } 
            else if (tc.name == "web_search" || tc.name == "fetch_website_content") 
            {
                if (TOOL_PERMISSIONS.WEB)
                    web.handle_tool(*this, tc.name, tc.arguments, tc.id);
                
                else
                {
                    std::string error_msg = 
                    "Error: Tool '" + tc.name + "' is not enabled.";
                    send_tool_result(tc.id, error_msg);
                }
            } 
            else if (tc.name == "run_automation_task") 
            {
                if (TOOL_PERMISSIONS.TASK_RUNNER)
                {
                    auto new_instance_ptr = std::make_unique<ollama_system>();
                    ollama_system* raw_ptr = new_instance_ptr.get(); // Get the address before moving it
                    background_tasks.push_back(std::move(new_instance_ptr));
                    Keyboard_Input.PROPS.ENABLED = false; // Disable keyboard input for the task instance
                    task_runner.handle_tool(*this, *raw_ptr, tc.name, tc.arguments, tc.id);
                    Keyboard_Input.PROPS.ENABLED = true;
                }
                else
                {
                    std::string error_msg = 
                    "Error: Tool '" + tc.name + "' is not enabled.";
                    send_tool_result(tc.id, error_msg);
                }
            }
            else 
            {
                // Build error message
                std::string error_msg = 
                    "Error: Tool '" + tc.name + "' is not recognized by the system.";
                // Return structured error to the model
                send_tool_result(tc.id, error_msg);
            }
        }
    }
}

void ollama_system::open() 
{
    std::cout << "[System] Connecting to " << PROPS.host << ":" << PROPS.port << " (" << PROPS.model << ")" << std::endl;
    
    std::filesystem::create_directories(PROPS.OLLI_DIERCTORY / "output");
    std::filesystem::create_directories(PROPS.OLLI_DIERCTORY / "input");
    
    web.apiKey = PROPS.web_search_api_key;
    hue.set_credentials(PROPS.hue_ip, PROPS.hue_key, PROPS.hue_path);
    task_runner.OLLI_DIRECTORY = PROPS.OLLI_DIERCTORY;

    PROPS.hue_path = (PROPS.OLLI_DIERCTORY / "scenes.json").string(); 
    PROPS.path_output = PROPS.OLLI_DIERCTORY / "output";
    PROPS.path_history = ".";

    if (TOOL_PERMISSIONS.CURRENT_TIME)
        current_time.register_tool(tools);

    if (TOOL_PERMISSIONS.TIMER)
        timer.register_tool(tools); 

    if (TOOL_PERMISSIONS.HUE)
        hue.register_tool(tools);

    if (TOOL_PERMISSIONS.THINKING)
        thinking.register_tool(tools);
    
    if (TOOL_PERMISSIONS.WEB)
        web.register_tool(tools);

    //if (TOOL_PERMISSIONS.DELEGATOR)
    //delegator.register_tool(tools);

    if (TOOL_PERMISSIONS.TASK_RUNNER)
        task_runner.register_tool(tools);

    // 
    if (history.empty()) 
    {
        //history.push_back({"system", "You are a helpful assistant with access to tools."
        history.push_back({"system", OLLAMA_OPENING});
    }
}

void ollama_system::open(OLLAMA_SYSTEM_PROPERTIES Properties)
{
    PROPS = Properties;
    open();
}


string ollama_system::gather_history()
{
    std::stringstream task_report;
    task_report << "### [REPORT] ###\n";
    
    bool gathered_any = false;
    for (size_t i = 0; i < history.size(); ++i) {
        const auto& msg = history[i];
        if (msg.role == "assistant") {
            task_report << "- " << msg.content << "\n";
            gathered_any = true;
        }
    }

    if (!gathered_any) {
        task_report << "(Task completed. Nothing recorded.)";
    }

    //cout << task_report.str() << endl; // Log the report for debugging
    return task_report.str();
}

/**
 * This function takes raw tool data and asks the model to "speak" it 
 * in the context of the current conversation/persona.
 */
void ollama_system::integrate_tool_result(std::string Special_Instruction, const std::string& raw_result) 
{
    //  Why this is the "Road Less Trodden"

    /*
    // the original.
    {
        // We initiate a secondary background "thought" to wrap the data
        std::string prompt = "The user requested information and the system returned this raw data: '" + raw_result + "'. "
                            "Rewrite this information to fit naturally into our current conversation and persona. "
                            "Be concise and do not mention that you are a tool.";
        
        // We use the internal send_task logic so this rewrite happens 
        // without blocking the main chat UI.
        this->send(prompt, "system");
    }
    */

    // AI1 suggestion:
    /*
    {
        std::string prompt =
            "[REWRITE]\n"
            "Raw data: '" + raw_result + "'.\n"
            "Rewrite this naturally in the assistant's voice. "
            "Do not mention tools, raw data, or internal processing.";

        this->send(prompt, "user");
    }
    */
    

    // AI2 suggestion:
    /**
     * REFINED: INTEGRATE_TOOL_RESULT
     * Uses a 'system' whisper to transform raw data into persona-driven speech.
     */
    {
        // 1. Construct a "Director's Note" prompt. 
        // We use clear delimiters like [DATA] to help the model parse quickly.
        std::string prompt = 
            "[DIRECTOR_NOTE]\n"
            "The following raw data was just retrieved: '" + raw_result + "'.\n"
            "TASK: Acknowledge this info as the Assistant. Stay in persona.\n"
            "CONSTRAINTS: Be concise. No technical jargon. Do NOT say 'The system found' or 'Rewriting data'.\n";

        if (!Special_Instruction.empty()) 
        {
            prompt += Special_Instruction + "\n";
        }
        else
        {
            prompt += 
            "Begin speaking now:";
        }

        // 2. We use "system" here. 
        // This prevents the "What was the last thing I said?" confusion 
        // because the model treats this as a 'state' rather than 'user input'.
        this->send(prompt, "system");
        
        // 3. Logic Note:
        // If your 'send' function adds the prompt to the history vector, 
        // I recommend adding a 'pop_back()' or a flag to your Message class 
        // called 'is_transient' so this prompt doesn't bloat your VRAM 
        // over a long session.
    }
}



void ollama_system::send(const std::string& user_input, const std::string& role) {
    std::string new_user_input = filter_non_printable(user_input);
    
    // 1. Set initial states
    status.is_active = true;
    last_received.complete = false; // Reset completion flag
    last_received.response = "";
    last_received.thinking = "";
    last_received.tool_calls.clear();

    {
        std::lock_guard<std::mutex> lock(history_mutex);
        history.push_back({role, new_user_input});
    }

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
        {"options", {
            {"num_ctx", PROPS.num_ctx},
            {"temperature", 0}
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
    bool stream_received_done_flag = false;

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
                    return false; 
                }

                std::string chunk(data, data_length);
                try {
                    auto j_chunk = json::parse(chunk);
                    
                    if (j_chunk.value("done", false)) {
                        stream_received_done_flag = true;
                    }

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
        
        last_received.complete = stream_received_done_flag && !status.interrupt_signal.load();

    } else {
        // Updated non-streaming logic to be as robust as send_task
        auto res = cli.Post("/api/chat", headers, json_body, "application/json");
        if (res && res->status == 200) {
            try {
                auto j_res = json::parse(res->body);
                last_received.complete = j_res.value("done", false);

                if (j_res.contains("message")) {
                    auto msg_obj = j_res["message"];
                    accumulated_content = msg_obj.value("content", "");
                    accumulated_thinking = msg_obj.value("thinking", "");

                    if (msg_obj.contains("tool_calls")) {
                        for (auto& tc : msg_obj["tool_calls"]) {
                            last_received.tool_calls.push_back({
                                tc.value("id", ""),
                                tc["function"].value("name", ""),
                                tc["function"]["arguments"]
                            });
                        }
                    }
                }
            } catch (const json::parse_error& e) {
                last_received.complete = false;
                std::cerr << "[Error] JSON Parse failed in send: " << e.what() << std::endl;
            }
        } else {
            last_received.complete = false;
            std::cerr << "[Error] Request failed in send: " 
                      << (res ? std::to_string(res->status) : "Connection error") << std::endl;
        }
    }

    last_received.response = accumulated_content;
    last_received.thinking = accumulated_thinking;
    
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

/**
 * REFINED: SEND_TOOL_RESULT (For your main ollama_system class)
 * This version updates the history but allows the Integration Task
 * to handle the actual conversational output.
 */
void ollama_system::send_tool_result(const std::string& tool_call_id, const std::string& result) {
    Message msg;
    msg.role = "tool";
    msg.content = result;
    msg.tool_call_id = tool_call_id;
    history.push_back(msg);
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

bool ollama_system::jump_input(CLASS_SYSTEM& System)
{
    if (trim(System.key_input.LINE) == "bye" || trim(System.key_input.LINE) == "quit" || trim(System.key_input.LINE) == "Goodbye.")
    {
        System.key_input.reset();
        running = false;
        if (is_processing) stop();
        if (chat_thread.joinable()) chat_thread.join();
        return true;
    }
    else if (trim(System.key_input.LINE) == "I'm home." || 
                trim(System.key_input.LINE) == "I'm awake." || 
                trim(System.key_input.LINE) == "Lights on." || 
                trim(System.key_input.LINE) == "I'm leaving." || 
                trim(System.key_input.LINE) == "Lights off." || 
                trim(System.key_input.LINE) == "I'm sleeping.")
    {
        ollama_system jump_instance;

        jump_instance.OLLAMA_OPENING = "Run any task given without question or hesitation. "
                                        "Responses or input from user will not be capable.";

        jump_instance.PROPS.stream_output = false;

        // ----
        cout << "[JUMP] " << flush;
        cout << "[" << trim(System.key_input.LINE) << "] " << std::endl;

        // ----
        // This needs compartmentalization and definable configuration.
        // ----

        if (trim(System.key_input.LINE) == "I'm home." || 
            trim(System.key_input.LINE) == "I'm awake." || 
            trim(System.key_input.LINE) == "Lights on.")
        {
            System.audio_control.VOCA_manual_set(DEF_VOCA_SLEEP);
            jump_instance.TOOL_PERMISSIONS.HUE = true;
            jump_instance.open(PROPS);
            jump_instance.send("Load the repose scene.");
            jump_instance.process(System.key_input);
        }
        else if (trim(System.key_input.LINE) == "I'm leaving.")
        {
            System.audio_control.VOCA_manual_set(DEF_VOCA_SLEEP);
            jump_instance.TOOL_PERMISSIONS.HUE = true;
            jump_instance.open(PROPS);
            jump_instance.send("Load the labor scene.");
            jump_instance.process(System.key_input);
        }
        else if (trim(System.key_input.LINE) == "I'm sleeping." || 
                    trim(System.key_input.LINE) == "Lights off." )
        {
            System.audio_control.VOCA_manual_set(DEF_VOCA_SLEEP);
            jump_instance.TOOL_PERMISSIONS.HUE = true;
            jump_instance.open(PROPS);
            jump_instance.send("Load the slumber scene.");
            jump_instance.process(System.key_input);
        }

        integrate_tool_result("Describe what happened.", gather_history());
        System.key_input.reset();
        
        return true;
    }
    else
    {
        return false;
    }
}

/**
 * Updates the input method to return true when a chat response is complete.
 * This version preserves the original non-blocking logic and thread safety.
 */
bool ollama_system::input(CLASS_SYSTEM& System)
{
    // 1. ORIGINAL INTERRUPT LOGIC
    if (is_processing && System.key_input.INTERRUPTED) 
    {
        std::cout << "." << std::flush;
        System.key_input.reset();
        
        stop();
        if (chat_thread.joinable()) chat_thread.join();
        is_processing = false;
        std::cout << "\n[Interrupting for new input...]" << std::endl;
        
        // Return true because the current session was closed/interrupted
        return true;
    }

    // 2. ORIGINAL INPUT LOGIC
    if (System.key_input.ENTER_PRESSED) 
    {
        if (jump_input(System)) 
        {
            // Internal command logic (e.g. /clear, /settings)
            return false;
        }
        else 
        {
            status.interrupt_signal = false;
            is_processing = true;
            
            std::string tmp_line = System.key_input.LINE;
            System.key_input.reset();

            // Ensure we don't leak a thread if one was somehow left joinable
            if (chat_thread.joinable()) chat_thread.join();

            // Launch the background thread exactly as before
            chat_thread = std::thread([this, tmp_line]() 
            {
                try {
                    send(tmp_line);
                } catch (...) {
                    // Maintain existing safety
                }
                this->is_processing = false;
            });

            return false;
        }
    }

    // 3. COMPLETION CHECK
    // If the thread is no longer processing but the thread object is still active,
    // it means the background work JUST finished.
    if (!is_processing && chat_thread.joinable()) 
    {
        chat_thread.join(); // Clean up the thread resources
        return true;        // Signal to the caller that the response is complete
    }

    return false;
}

/**
 * SYSTEM PROCESSOR
 * * PURPOSE:
 * This is the heart of the tool system. It runs in a loop to:
 * 1. Handle tool calls for the main chat and all background tasks.
 * 2. Manage life-cycles of background "expert" tasks (unique_ptrs).
 * 3. Perform maintenance like thread joining and history saving.
 * 4. Trigger background consolidation (memory cleanup) every 60 seconds.
 */

void ollama_system::process(KEYBOARD_INPUT& Keyboard_Input) 
{
    // ---------------------------------------------------------
    // PART 1: PROCESS MAIN CHAT TOOLS
    // ---------------------------------------------------------
    handle_instance_tools(Keyboard_Input);

    // ---------------------------------------------------------
    // PART 2: MANAGE BACKGROUND TASKS
    // Iterate through background instances, handle their logic, 
    // and remove them if they are finished.
    // ---------------------------------------------------------
    for (auto it = background_tasks.begin(); it != background_tasks.end(); ) {
        ollama_system& task_instance = **it; // Access the object inside unique_ptr

        // A. Handle any tool calls requested by the background task
        handle_instance_tools(Keyboard_Input);

        // B. Thread Management: Join finished network threads
        if (!task_instance.is_processing && task_instance.chat_thread.joinable()) {
            task_instance.chat_thread.join();
        }

        // C. Completion Check: Is the task totally finished?
        bool is_finished = !task_instance.is_processing && 
                           task_instance.last_received.complete && 
                           task_instance.last_received.tool_calls.empty();

        if (is_finished) {
            // If the task produced a response, relay it to the main chat
            if (!task_instance.last_received.response.empty()) {
                std::string task_report = "[Task Update]: " + task_instance.last_received.response;
                send(task_report, "system"); 
            }
            
            // Remove from vector (unique_ptr automatically deletes the memory)
            it = background_tasks.erase(it); 
        } else {
            ++it; // Move to next task
        }
    }

    // ---------------------------------------------------------
    // PART 3: MAIN CHAT MAINTENANCE
    // If the main AI finished speaking, finalize history and reset UI prompt.
    // ---------------------------------------------------------
    if (!is_processing && chat_thread.joinable()) {
        chat_thread.join();
        update_status();
        history_write(PROPS.path_history);
        std::cout << "You: " << std::flush;
    }

    // ---------------------------------------------------------
    // PART 5: ACTIVE MONITORS
    // Continuous checks for time-based triggers or hardware state.
    // ---------------------------------------------------------
    if (TOOL_PERMISSIONS.TIMER)
        timer.monitor_tool(*this);   // Checks if timers have expired
    if (TOOL_PERMISSIONS.HUE)
        hue.monitor_tool();         // Synchronizes light states

    // ---------------------------------------------------------
    // PART 6: TTS OUTPUT
    // If there's new text in the TTS buffer, write it to the output file for the TTS engine to read.
    // ---------------------------------------------------------
    write_to_tts();        // Pushes new text to Speech engine
}

/*
void ollama_system::consolidate_check(KEYBOARD_INPUT& Keyboard_Input)
{
    // ---------------------------------------------------------
    // PART 4: PERIODIC CONSOLIDATION
    // Every 60 seconds, run a background thread to compress/clean 
    // history if the system is idle.
    // ---------------------------------------------------------
    auto now = std::chrono::steady_clock::now();
    bool is_idle = !is_processing && 
                   !status.interrupt_signal.load() && 
                   !Keyboard_Input.IS_TYPING;

    if (is_idle) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_consolidation).count();
        
        if (elapsed > 60) {
            is_processing = true;
            last_consolidation = now; 

            // Fire and forget thread for consolidation
            chat_thread = std::thread([this, &Keyboard_Input]() {
                try { 
                    consolidate(history, *this, Keyboard_Input); 
                } catch (...) {
                    // Silently catch thread errors
                }
                is_processing = false;
            });

            if (chat_thread.joinable()) {
                chat_thread.detach(); 
            }
        }
    }
}
*/

// ----

/**
 * Standalone Consolidation Function
 * @param kb Reference to the keyboard input object for activity monitoring.
 */

/*
void consolidate(std::vector<Message>& chat_history, ollama_system& config, KEYBOARD_INPUT& kb) {
    if (chat_history.empty()) return;

    ollama_system consolidate_client;
    // Copy necessary props...
    consolidate_client.PROPS.host = config.PROPS.host;
    consolidate_client.PROPS.port = config.PROPS.port;
    consolidate_client.PROPS.model = config.PROPS.model;
    consolidate_client.PROPS.num_ctx = config.PROPS.num_ctx; 
    consolidate_client.PROPS.use_thinking = false; 
    consolidate_client.PROPS.stream_output = false; 

    size_t starts_at = static_cast<size_t>(config.consolitation_starts_starts_at); 
    size_t sizes = static_cast<size_t>(config.consolitation_sizes); 

    int current_level = 0;
    while (current_level < 10) {
        // DIRECT ABORT CHECK: Checks the public boolean flags directly via reference
        if (config.status.interrupt_signal.load() || kb.INTERRUPTED || kb.IS_TYPING) {
            std::cout << "[Consolidation] Aborted: User activity detected." << std::endl;
            return; 
        }

        std::vector<size_t> target_indices;
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            for (size_t i = 0; i < chat_history.size(); ++i) {
                // Keep the foundational rules (Level 0) and sync them with latest OLLAMA_OPENING
                if (current_level == 0 && chat_history[i].role == "system" && chat_history[i].consolidation_level == 0) {
                    chat_history[i].content = config.OLLAMA_OPENING;
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
            bool aborted_during_llm = config.status.interrupt_signal.load() || kb.INTERRUPTED || kb.IS_TYPING;

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
            } else break;
        } else break; 
    }
}
*/


#endif