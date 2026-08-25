#ifndef tools_cpp
#define tools_cpp

#include <regex>

#include <curl/curl.h>

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

bool TOOL_SET_THINKING_MODE::check(ollama_system& chat, CLASS_SYSTEM*, const ToolCall& tc) {
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
void TOOL_SET_THINKING_MODE::monitor_tool(ollama_system&, CLASS_SYSTEM*) {}

// ---

// TOOL_TIMER used to live here - moved to tools/clock/clock.cpp as a
// remote tool (set_timer/check_timer, alongside get_clock_time) so it runs
// independently of olli's own process/restart lifecycle, same reasoning as
// get_clock_time's own move. See PROTOCOL.md's `event` message type for how
// expiry alerts reach olli now (TOOL_REMOTE::monitor_tool(), same
// integrate_tool_result() path this used to call directly).

// TOOL_HUE used to live here - moved to tools/hue/hue.cpp as a remote tool
// (set_hue_light/list_hue_lights/manage_hue_scenes, same names/arguments,
// same HUE_LIGHT_CLASS bridge logic) so it runs independently of olli's own
// process/restart lifecycle, same reasoning as TOOL_TIMER's own move above.
// TOOL_PERMISSIONS.HUE (tools_helper.h) and every place that sets it
// (main.cpp, olla.cpp's jump-phrase block) deliberately stay - see
// tools/hue/README.md and tools/PROTOCOL.md for why.

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

bool TOOL_WEB_SEARCH::check(ollama_system& chat, CLASS_SYSTEM*, const ToolCall& tc) {
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
void TOOL_WEB_SEARCH::monitor_tool(ollama_system&, CLASS_SYSTEM*) {}


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

    {
        Message system_msg;
        system_msg.role = "system";
        system_msg.content = system_prompt;
        sub_agent->history.push_back(system_msg);
    }

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

            // nullptr, not the real CLASS_SYSTEM: this automation instance
            // deliberately drives its own local keyboard_input above (see
            // its declaration/comment near the top of this function),
            // isolated from the real system's - it has no business reaching
            // the real one, same reasoning as sidetrack.cpp's own nullptr
            // call site (see TOOL_BASE::check()'s comment in tools.h).
            instance.process(nullptr, keyboard_input.PROPS.ENABLED);
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

bool TOOL_TASK_RUNNER::check(ollama_system& chat, CLASS_SYSTEM*, const ToolCall& tc) {
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
void TOOL_TASK_RUNNER::monitor_tool(ollama_system&, CLASS_SYSTEM*) {}

// Shared by both call sources handle_instance_tools() drains - see its own
// comment in olla.h. Applies the tool_calls_this_turn cap (see olla.h),
// then routes to whichever tool's check() claims tc.name (see the
// TOOL_BASE comment in tools.h) - an unrecognized name gets an error
// result back instead of ever reaching a tool.
void ollama_system::dispatch_tool_call(const ToolCall& tc, CLASS_SYSTEM* system, bool& Keyboard_Input_Enabled)
{
    // TODO: special-cased until ollama_system can reach CLASS_SYSTEM
    // directly (see TODO.md) - only run_automation_task needs the main
    // keyboard input disabled while its spawned instance runs.
    bool disable_keyboard = (tc.name == "run_automation_task");
    if (disable_keyboard) Keyboard_Input_Enabled = false;

    // Guard against a runaway chain - see tool_calls_this_turn's comment in
    // olla.h. Deliberately calls send_tool_result() only, not
    // integrate_tool_result() - the latter is what would start another
    // DIRECTOR_NOTE round-trip and keep a chain going. This just logs the
    // refusal and stops dead; the model gets a real chance to reply in
    // text on its next natural turn instead.
    if (tool_calls_this_turn >= PROPS.max_tool_calls_per_turn) {
        log("[System] Tool call capped this turn: " + tc.name + "\n");
        send_tool_result(tc.id, "Error: Too many tool calls this turn - stopping here to avoid a loop.");
        if (disable_keyboard) Keyboard_Input_Enabled = true;
        return;
    }
    ++tool_calls_this_turn;

    bool handled = false;
    for (auto& tool : tools_list) {
        if (tool->check(*this, system, tc)) { handled = true; break; }
    }

    if (!handled) {
        log("[System] Tool error call received: " + tc.name + "\n");
        send_tool_result(tc.id, "Error: Tool '" + tc.name + "' is not recognized by the system.");
    }

    if (disable_keyboard) Keyboard_Input_Enabled = true;
}

void ollama_system::handle_instance_tools(CLASS_SYSTEM* system, bool& Keyboard_Input_Enabled)
{
    // System-injected calls (e.g. a timer's on_expire action - see
    // TOOL_REMOTE::monitor_tool()) - drained independently of the model's
    // own last_received.tool_calls below, see pending_tool_calls' comment
    // in olla.h for why. Held back while a response is actively streaming,
    // same as the model-issued path below, so it never interleaves with
    // in-flight generation.
    if (!is_processing) {
        while (!pending_tool_calls.empty()) {
            ToolCall tc = pending_tool_calls.front();
            pending_tool_calls.pop();
            dispatch_tool_call(tc, system, Keyboard_Input_Enabled);
        }
    }

    bool is_ready_for_tools = !is_processing &&
                              last_received.complete &&
                              !last_received.tool_calls.empty();

    if (is_ready_for_tools) {
        // Take ownership of the calls and clear the queue
        auto pending_calls = last_received.tool_calls;
        last_received.tool_calls.clear();

        for (auto& tc : pending_calls) {
            dispatch_tool_call(tc, system, Keyboard_Input_Enabled);
        }
    }
}

#endif
