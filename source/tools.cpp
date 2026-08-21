#ifndef tools_cpp
#define tools_cpp

#include <regex>

#include "tools.h"
#include "olla.h"
#include "user_io.h"

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

// No per-instance setup needed - part of the common tool interface (see the note in tools.h).
void TOOL_SET_THINKING_MODE::configure(ollama_system&) {}

void TOOL_SET_THINKING_MODE::register_tool(ollama_system& chat, json& tools) {
    if (!chat.TOOL_PERMISSIONS.THINKING) return;

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

void TOOL_SET_THINKING_MODE::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "set_thinking_mode") {
        if (args.contains("enabled") && args["enabled"].is_boolean()) {
            chat.PROPS.use_thinking = args["enabled"].get<bool>();

            std::string state_str = chat.PROPS.use_thinking ? "ENABLED" : "DISABLED";

            chat.log("[System (set_thinking_mode)]: " + state_str + "\n");

            chat.send_tool_result(tc_id, "Thinking mode has been successfully " + state_str);
        } else {
            std::string error_msg = "Error: Missing or invalid 'enabled' boolean argument.";
            std::cerr << "[System] " << error_msg << std::endl;
            chat.send_tool_result(tc_id, error_msg);
        }
    }
    else {
        std::string error_msg = "Error: Tool '" + name + "' not recognized by TOOL_SET_THINKING_MODE.";
        std::cerr << "[System] " << error_msg << std::endl;
        chat.send_tool_result(tc_id, error_msg);
    }
}

bool TOOL_SET_THINKING_MODE::check(ollama_system& chat, const ToolCall& tc) {
    if (tc.name != "set_thinking_mode")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    if (chat.TOOL_PERMISSIONS.THINKING)
        handle_tool(chat, tc.name, tc.arguments, tc.id);
    else
        chat.send_tool_result(tc.id, "Error: Tool '" + tc.name + "' is not enabled.");

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_SET_THINKING_MODE::monitor_tool(ollama_system&) {}

// ---

// No per-instance setup needed - part of the common tool interface (see the note in tools.h).
void TOOL_GET_CURRENT_TIME::configure(ollama_system&) {}

void TOOL_GET_CURRENT_TIME::register_tool(ollama_system& chat, json& tools) {
    if (!chat.TOOL_PERMISSIONS.CURRENT_TIME) return;

    json get_time_params = {
        {"type", "object"},
        {"properties", {
            {"format", {
                {"type", "string"},
                {"description", "The strftime format string (e.g., '%H:%M:%S'). Required."}
            }}
        }},
        {"required", {"format"}}
    };

    json get_date_params = {
        {"type", "object"},
        {"properties", {
            {"format", {
                {"type", "string"},
                {"description", "The strftime format string (e.g., '%Y-%m-%d'). Required."}
            }}
        }},
        {"required", {"format"}}
    };

    add_tool(tools, "get_current_time", "Returns the current system time using a specific format. Always execute this tool call for every request, even if you believe you have the time.", get_time_params);
    add_tool(tools, "get_current_date", "Returns the current system date using a specific format. Always execute this tool call for every request, even if you believe you have the date.", get_date_params);
}

void TOOL_GET_CURRENT_TIME::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name != "get_current_time" && name != "get_current_date") return;

    std::string format = (name == "get_current_time") ? "%H:%M:%S" : "%Y-%m-%d";

    if (args.is_object() && args.contains("format") && args["format"].is_string()) {
        format = args["format"].get<std::string>();
    }

    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    // localtime_r, not localtime: thread-safe (no shared static buffer),
    // needed since tool handlers can run from more than one ollama_system
    // instance/thread.
    std::tm local_tm;
    if (localtime_r(&now_time, &local_tm) == nullptr) {
        chat.send_tool_result(tc_id, "Error: Failed to process system clock.");
        return;
    }

    std::stringstream ss;
    ss << std::put_time(&local_tm, format.c_str());
    std::string result_str = ss.str();

    chat.send_tool_result(tc_id, result_str);
    chat.integrate_tool_result(tc_id, result_str);
}

bool TOOL_GET_CURRENT_TIME::check(ollama_system& chat, const ToolCall& tc) {
    if (tc.name != "get_current_time" && tc.name != "get_current_date")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    if (chat.TOOL_PERMISSIONS.CURRENT_TIME)
        handle_tool(chat, tc.name, tc.arguments, tc.id);
    else
        chat.send_tool_result(tc.id, "Error: Tool '" + tc.name + "' is not enabled.");

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_GET_CURRENT_TIME::monitor_tool(ollama_system&) {}

// ---

// No per-instance setup needed - part of the common tool interface (see the note in tools.h).
void TOOL_TIMER::configure(ollama_system&) {}

void TOOL_TIMER::register_tool(ollama_system& chat, json& tools) {
    if (!chat.TOOL_PERMISSIONS.TIMER) return;

    json set_timer_params = {
        {"type", "object"},
        {"properties", {
            {"label", {{"type", "string"}, {"description", "A name for the timer"}}},
            {"seconds", {{"type", "number"}, {"description", "Duration in seconds"}}},
            {"reminder", {{"type", "string"}, {"description", "Optional: Action to perform when finished. Leave empty for a simple notification."}}}
        }},
        {"required", {"label", "seconds"}}
    };

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

void TOOL_TIMER::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "set_timer") {
        chat.log("[System (set_timer)]\n");
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

        chat.send_tool_result(tc_id, res);
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

bool TOOL_TIMER::check(ollama_system& chat, const ToolCall& tc) {
    if (tc.name != "set_timer" && tc.name != "check_timer")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    if (chat.TOOL_PERMISSIONS.TIMER)
        handle_tool(chat, tc.name, tc.arguments, tc.id);
    else
        chat.send_tool_result(tc.id, "Error: Tool '" + tc.name + "' is not enabled.");

    return true;
}

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
                chat.log("[Event] Triggering persona alert: " + label + "\n");

                chat.integrate_tool_result("", event_msg);
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

void TOOL_HUE::configure(ollama_system& chat) {
    set_credentials(chat.PROPS.hue_ip, chat.PROPS.hue_key, chat.PROPS.hue_path);
}

// Sends a direct system-level nudge forcing a tool call, for when the
// model just confirms a lighting request in text instead of acting on it.
void TOOL_HUE::refresh_system_prompt(ollama_system& chat) {
    std::stringstream ss;
    ss << "[SYSTEM COMMAND] You are a logic-first controller. ";
    ss << "If the user asks to turn lights on/off, set a color, or change a scene, ";
    ss << "you MUST call the corresponding tool immediately. ";
    ss << "Do not provide conversational confirmation without also calling the tool.";
    chat.send(ss.str(), "user");
}

void TOOL_HUE::register_tool(ollama_system& chat, json& tools) {
    if (!chat.TOOL_PERMISSIONS.HUE) return;

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

    add_tool(tools, "set_hue_light", "Controls Hue lights by ID or Name. Use this for general commands like 'turn off all lights' by setting light_id to 'all'. Always execute this tool call for every request, even if you believe the state is already set.", set_params);
    add_tool(tools, "list_hue_lights", "Returns status of all connected lights", {{"type", "object"}});
    add_tool(tools, "manage_hue_scenes", "Saves, loads, or removes local light scenes. Only use for specific named snapshots (e.g., 'home', 'away').", scene_params);
}

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

bool TOOL_HUE::check(ollama_system& chat, const ToolCall& tc) {
    if (tc.name != "set_hue_light" && tc.name != "list_hue_lights" && tc.name != "manage_hue_scenes")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    if (chat.TOOL_PERMISSIONS.HUE)
        handle_tool(chat, tc.name, tc.arguments, tc.id);
    else
        chat.send_tool_result(tc.id, "Error: Tool '" + tc.name + "' is not enabled.");

    return true;
}

void TOOL_HUE::monitor_tool(ollama_system& chat)
{
    if (!chat.TOOL_PERMISSIONS.HUE) return;

    static int counter = 0;
    if (++counter % 10000 == 0) hue.refresh_lights();
}

// ----


// No per-instance setup needed - part of the common tool interface (see the note in tools.h).
void TOOL_WEB_SEARCH::configure(ollama_system& chat) {
    apiKey = chat.PROPS.web_search_api_key;
}

std::string TOOL_WEB_SEARCH::strip_html_tags(std::string html) {
    html = std::regex_replace(html, std::regex("<script[\\s\\S]*?>[\\s\\S]*?<\\/script>", std::regex::icase), " ");
    html = std::regex_replace(html, std::regex("<style[\\s\\S]*?>[\\s\\S]*?<\\/style>", std::regex::icase), " ");
    html = std::regex_replace(html, std::regex("<[^>]*>"), " ");
    html = std::regex_replace(html, std::regex("\\s+"), " ");

    return html;
}

// OSC 8 terminal hyperlink escape sequence: ESC ] 8 ; ; URL ESC-backslash TEXT ESC ] 8 ; ; ESC-backslash
std::string TOOL_WEB_SEARCH::make_clickable(const std::string& url, const std::string& text) {
    return "\x1B]8;;" + url + "\x1B\\" + text + "\x1B]8;;\x1B\\";
}

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

        // Strip HTML noise so the model isn't parsing markup as content
        std::string cleanText = strip_html_tags(readBuffer);

        if (cleanText.length() > 4000) return cleanText.substr(0, 4000) + "... [truncated]";
        return cleanText;
    }
    return "Error initializing curl.";
}

void TOOL_WEB_SEARCH::register_tool(ollama_system& chat, json& tools) {
    if (!chat.TOOL_PERMISSIONS.WEB) return;

    // Told to the model via each tool's description so its final answer uses
    // our clickable-link format instead of Markdown, which the terminal can't render.
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

        chat.send_tool_result(tc_id, result);
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

        chat.send_tool_result(tc_id, "Cleaned Page Content from " + url + ":\n" + result);
        chat.integrate_tool_result("", "I have fetched and processed the content from " + url + ". Here is the information retrieved: " + result);
    }
    else {
        chat.send_tool_result(tc_id, "Error: Unknown tool.");
    }
}

bool TOOL_WEB_SEARCH::check(ollama_system& chat, const ToolCall& tc) {
    if (tc.name != "web_search" && tc.name != "fetch_website_content")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    if (chat.TOOL_PERMISSIONS.WEB)
        handle_tool(chat, tc.name, tc.arguments, tc.id);
    else
        chat.send_tool_result(tc.id, "Error: Tool '" + tc.name + "' is not enabled.");

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_WEB_SEARCH::monitor_tool(ollama_system&) {}


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

    chat.log("\n[Delegator] Invoking Specialist: [" + specialty + "]\n");

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
        chat.log("[Delegator] Sub-agent is busy reasoning...\n");
        while (sub_agent->is_processing && count < wait_limit) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            count++;
        }
    }

    // 5. Retrieve result
    std::string result = sub_agent->last_received.response;

    if (result.empty() && !sub_agent->last_received.thinking.empty()) {
        chat.log("[Delegator] Note: Main response empty, using data from thinking buffer.\n");
        result = sub_agent->last_received.thinking;
    }

    if (result.empty()) {
        result = "The expert subroutine failed to return a response.";
        chat.log("[Delegator] Error: Result was empty.\n");
    } else {
        chat.log("[Delegator] Task complete. Length: " + std::to_string(result.length()) + "\n");
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

// No per-instance setup needed - part of the common tool interface (see the note in tools.h).
void TOOL_TASK_RUNNER::configure(ollama_system& chat) {
    OLLI_DIRECTORY = chat.PROPS.OLLI_DIRECTORY;
}

bool TOOL_TASK_RUNNER::iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
                        [](unsigned char ca, unsigned char cb) {
                            return std::tolower(ca) == std::tolower(cb);
                        });
}

void TOOL_TASK_RUNNER::register_tool(ollama_system& chat, json& tools)
{
    if (!chat.TOOL_PERMISSIONS.TASK_RUNNER) return;

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

void TOOL_TASK_RUNNER::handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id)
{
    if (name != "run_automation_task")
    {
        return;
    }

    bool running_directory = false;
    std::filesystem::path working_dir;

    KEYBOARD_INPUT keyboard_input;
    keyboard_input.PROPS.ENABLED = false;
    // Must match the main key_input's RAW_ECHO (main.cpp) - defaults to true,
    // which raw-echoes every keystroke straight to the terminal via cout,
    // corrupting the ncurses screen buffer when USE_NCURSES is the active
    // display path (see KEYBOARD_INPUT_PROPERTIES::RAW_ECHO's comment in
    // user_io.h). This was the source of the stray CRLF seen during
    // [[ASK]]/[[ENTER TO CONTINUE]] prompts in run system test.
    keyboard_input.PROPS.RAW_ECHO = !USE_NCURSES;

    std::string intent_phrase = args["intent_phrase"];
    chat.log("[TaskRunner] Searching for automation matching: \"" + intent_phrase + "\"\n");

    auto task_it = std::find_if(
        task_manager.TASK_LIST.begin(),
        task_manager.TASK_LIST.end(),
        [this, &intent_phrase](const TASK_SIMPLE& task) {
            return iequals(task.TASK_PHRASE, intent_phrase);
        }
    );

    bool task_found = (task_it != task_manager.TASK_LIST.end());

    if (task_found)
    {
        const auto& found_task = *task_it;

        // The automation runs on its own background instance so it doesn't
        // block the main chat loop - see ollama_system::spawn_background_task().
        ollama_system& instance = chat.spawn_background_task();

        if (!instance.OLLAMA_OPENING.empty())
            instance.OLLAMA_OPENING = found_task.TASK_PURPOSE;

        instance.PROPS.stream_output = false;
        instance.TOOL_PERMISSIONS = found_task.TOOL_PERMISSIONS;

        instance.open(chat.PROPS);

        // A scratch directory for the task, cleaned up (remove_all below)
        // once the automation finishes.
        if (found_task.TASK_DIRECTORY != "")
        {
            working_dir = OLLI_DIRECTORY / (found_task.TASK_DIRECTORY + "_" + tc_id);
            std::filesystem::create_directories(working_dir);
            running_directory = true;
        }

        std::string success_log = "SUCCESS: Automation found. Sequence loading...";
        chat.send_tool_result(tc_id, success_log);

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

            instance.process(keyboard_input.PROPS.ENABLED);
            instance.last_received.complete = false;
        }

        {
            success_log = "SUCCESS: Automation Complete";
            chat.send_tool_result(tc_id, success_log);
            chat.integrate_tool_result("", instance.gather_history());
        }

        if (running_directory)
        {
            std::filesystem::remove_all(working_dir);
        }
    }
    else
    {
        std::string error_msg = "ERROR: No automation found for '" + intent_phrase + "'.";

        chat.send_tool_result(tc_id, error_msg);
        chat.integrate_tool_result("", error_msg);
    }
}

bool TOOL_TASK_RUNNER::check(ollama_system& chat, const ToolCall& tc) {
    if (tc.name != "run_automation_task")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    if (chat.TOOL_PERMISSIONS.TASK_RUNNER)
        handle_tool(chat, tc.name, tc.arguments, tc.id);
    else
        chat.send_tool_result(tc.id, "Error: Tool '" + tc.name + "' is not enabled.");

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_TASK_RUNNER::monitor_tool(ollama_system&) {}

// Routes each pending tool call from last_received.tool_calls to whichever
// tool's check() claims it (see the TOOL_BASE comment in tools.h) - an
// unrecognized name gets an error result back instead of ever reaching a
// tool.
void ollama_system::handle_instance_tools(bool& Keyboard_Input_Enabled)
{
    bool is_ready_for_tools = !is_processing &&
                              last_received.complete &&
                              !last_received.tool_calls.empty();

    if (is_ready_for_tools) {
        // Take ownership of the calls and clear the queue
        auto pending_calls = last_received.tool_calls;
        last_received.tool_calls.clear();

        for (auto& tc : pending_calls) {
            // TODO: special-cased until ollama_system can reach CLASS_SYSTEM
            // directly (see TODO.md) - only run_automation_task needs the
            // main keyboard input disabled while its spawned instance runs.
            bool disable_keyboard = (tc.name == "run_automation_task");
            if (disable_keyboard) Keyboard_Input_Enabled = false;

            bool handled = false;
            for (auto& tool : tools_list) {
                if (tool->check(*this, tc)) { handled = true; break; }
            }

            if (!handled) {
                log("[System] Tool error call received: " + tc.name + "\n");
                send_tool_result(tc.id, "Error: Tool '" + tc.name + "' is not recognized by the system.");
            }

            if (disable_keyboard) Keyboard_Input_Enabled = true;
        }
    }
}

#endif
