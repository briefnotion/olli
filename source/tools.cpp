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

// ---


// Generic function to talk to the Bridge
std::string TOOL_HUE::make_request(const std::string& method, const std::string& endpoint, const std::string& body = "") {
    CURL* curl = curl_easy_init();
    std::string readBuffer;
    if (curl) {
        std::string url = "http://" + bridge_ip + "/api/" + api_key + endpoint;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        
        if (!body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            readBuffer = "{\"error\": \"CURL failed\"}";
        }
        curl_easy_cleanup(curl);
    }
    return readBuffer;
}


// Helper to convert RGB to XY (CIE 1931) for Philips Hue
std::pair<double, double> TOOL_HUE::rgbToXY(int r, int g, int b) {
    // Normalize and Gamma Correction
    auto adjust = [](double val) {
        val /= 255.0;
        return (val > 0.04045) ? pow((val + 0.055) / (1.055), 2.4) : (val / 12.92);
    };

    double R = adjust(r);
    double G = adjust(g);
    double B = adjust(b);

    // Wide RGB D65 conversion
    double X = R * 0.664511 + G * 0.154324 + B * 0.162028;
    double Y = R * 0.283881 + G * 0.668433 + B * 0.047685;
    double Z = R * 0.000088 + G * 0.072310 + B * 0.986039;

    double cx = X / (X + Y + Z);
    double cy = Y / (X + Y + Z);
    
    if (std::isnan(cx)) cx = 0.0;
    if (std::isnan(cy)) cy = 0.0;

    return {cx, cy};
}

void TOOL_HUE::register_tool(ollama_system& chat) {
    json set_params = {
        {"type", "object"},
        {"properties", {
            {"light_id", {{"type", "string"}, {"description", "The numeric ID string (e.g., '2')."}}},
            {"on", {{"type", "boolean"}, {"description", "Power state."}}},
            {"brightness", {{"type", "integer"}, {"description", "0-254. Use ~254 for vivid colors."}}},
            {"preset", {
                {"type", "string"}, 
                {"enum", {"red", "green", "blue", "yellow", "magenta", "cyan", "orange", "purple", "pink", "white"}},
                {"description", "Only use for absolute basic colors."}
            }},
            {"hex", {
                {"type", "string"}, 
                {"description", "Hex code (e.g. #7E60BF). USE THIS for any nuanced colors (e.g. 'Lavender', 'Slate', 'Neon')."}
            }},
            {"alert", {
                {"type", "string"},
                {"enum", {"none", "select", "lselect"}},
                {"description", "Effect: 'select' (flash once), 'lselect' (flash 15s)." }
            }},
            {"flash_count", {
                {"type", "integer"},
                {"description", "Specific number of times to flash. If provided, will automatically stop the 'lselect' alert after the count is reached."}
            }},
            {"transition_ms", {
                {"type", "integer"},
                {"description", "Transition time in milliseconds. (Default is ~400ms)."}
            }}
        }},
        {"required", {"light_id"}} 
    };

    chat.add_tool("set_hue_light", "Controls Hue lights. Priority: 1. Use light_id. 2. Use 'hex' for specific shades.", set_params);
    chat.add_tool("list_hue_lights", "Returns a list of all connected lights", {{"type", "object"}});
}

void TOOL_HUE::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "list_hue_lights") {
        std::cout << "[System (list_hue_lights)]" << std::endl;
        std::string raw_json = make_request("GET", "/lights");
        try {
            json lights = json::parse(raw_json);
            std::stringstream ss;
            ss << "Lights found: ";
            for (auto& [id, data] : lights.items()) {
                ss << id << " (" << data["name"].get<std::string>() << "), ";
            }
            chat.send_tool_result(tc_id, ss.str());
        } catch (...) {
            chat.send_tool_result(tc_id, "Error: Bridge communication failed.");
        }
    } 
    else if (name == "set_hue_light") {
        std::cout << "[System (set_hue_light)]" << std::endl;
        if (!args.contains("light_id")) {
            chat.send_tool_result(tc_id, "Error: Missing light_id.");
            return;
        }

        std::string id = args.at("light_id").get<std::string>();
        if (!id.empty() && !std::isdigit(id[0])) {
             chat.send_tool_result(tc_id, "Error: Use numeric ID (e.g. '2') not name.");
             return;
        }

        json body;
        
        // Handle "on" logic
        if (args.contains("on")) {
            body["on"] = args.at("on").get<bool>();
        } else if (!args.contains("alert") && !args.contains("flash_count")) {
            body["on"] = true;
        }
        
        if (args.contains("brightness")) body["bri"] = args.at("brightness");
        
        // Determine alert mode
        std::string alert_mode = args.value("alert", "none");
        if (args.contains("flash_count")) {
            int count = args.at("flash_count").get<int>();
            if (count > 1) {
                alert_mode = "lselect";
            } else if (count == 1) {
                alert_mode = "select";
            }
        }
        
        if (alert_mode != "none") {
            body["alert"] = alert_mode;
        }

        if (args.contains("transition_ms")) {
            int ms = args.at("transition_ms").get<int>();
            body["transitiontime"] = ms / 100;
        }

        bool color_set = false;
        if (args.contains("hex")) {
            std::string hex = args.at("hex").get<std::string>();
            if (!hex.empty()) {
                if (hex[0] == '#') hex.erase(0, 1);
                unsigned int r, g, b;
                if (sscanf(hex.c_str(), "%02x%02x%02x", &r, &g, &b) == 3) {
                    auto [x, y] = rgbToXY(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b));
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
            std::string p = args.at("preset").get<std::string>();
            if (palette.count(p)) {
                body["xy"] = palette[p];
                color_set = true;
            }
        }

        // Execute the initial request
        std::string res = make_request("PUT", "/lights/" + id + "/state", body.dump());
        
        // Handle timed stop if flash_count was provided
        if (args.contains("flash_count") && args.at("flash_count").get<int>() > 1) {
            std::cout << "[System (flash_count)]" << std::endl;
            int count = args.at("flash_count").get<int>();
            // Hue "lselect" flashes roughly once per second. 
            // We spawn a thread to stop it after X seconds so we don't block the main tool handler.
            std::thread([this, id, count]() {
                std::this_thread::sleep_for(std::chrono::seconds(count));
                json stop_body = {{"alert", "none"}};
                make_request("PUT", "/lights/" + id + "/state", stop_body.dump());
            }).detach();
        }

        chat.send_tool_result(tc_id, "Sent to ID " + id + ". Response: " + res);
    }
}

/**
 * Proactive Monitoring
 * This runs in your main loop to handle background tasks or system alerts.
 */
void TOOL_HUE::monitor_tool() {
    // Example: Heartbeat check every X iterations
    static int counter = 0;
    if (++counter % 5000 == 0) { 
        // In a real app, you might ping the bridge here.
        // If the bridge goes offline, you could inject a system message:
        // if (!bridge_reachable && !is_processing) {
        //    chat.inject_system_message("ALERT: Philips Hue Bridge is no longer reachable on the network.");
        // }
    }
}



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

void TOOL_SYSTEM_CLASS::process(ollama_system& chat)
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
            else if (tc.name == "set_hue_light" || tc.name == "list_hue_lights") {
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
    if (!chat.is_processing && std::chrono::duration_cast<std::chrono::seconds>(now - last_consolidation).count() > 60) {
        chat.is_processing = true;
        last_consolidation = now; // Update the timer
        chat.status.interrupt_signal = false; 
        
        bool tmp_is_processing = chat.is_processing;
        chat.chat_thread = std::thread([&chat, &tmp_is_processing]() {
            try {
                consolidate(chat.history, chat); 
            } catch (...) {
                std::cerr << "[System] Consolidation error." << std::endl;
            }
            chat.is_processing = false;
        });

        if (chat.chat_thread.joinable()) chat.chat_thread.detach(); 
    }
    
    // 5. Monitor background events (like timers)
    //timer.monitor_tool(chat, chat.is_processing);
    timer.monitor_tool(chat);
    hue.monitor_tool();

    // 6. Other
    chat.write_to_tts();
}


#endif