#ifndef tools_cpp
#define tools_cpp

#include <regex>

#include <curl/curl.h>

#include "tools.h"
#include "olla.h"
#include "user_io.h"
#include "io_worker.h"

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

void TOOL_SET_THINKING_MODE::register_tool(ollama_system&, json& tools) {
    tool_functions.clear();

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

    tool_functions.push_back("set_thinking_mode");
    add_tool(tools, "set_thinking_mode", "Enables or disables the internal reasoning/thinking process for the model", set_thinking_params);
}

void TOOL_SET_THINKING_MODE::handle_tool(ollama_system& chat, COMMS&, const std::string& name, const json& args, const std::string& tc_id) {
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

bool TOOL_SET_THINKING_MODE::check(IO_WORKER_CLASS&, ollama_system& chat, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, COMMS& comms, const ToolCall& tc) {
    if (tc.name != "set_thinking_mode")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    handle_tool(chat, comms, tc.name, tc.arguments, tc.id);

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_SET_THINKING_MODE::monitor_tool(ollama_system&, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, COMMS&) {}

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

void TOOL_WEB_SEARCH::register_tool(ollama_system&, json& tools) {
    tool_functions.clear();

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
    tool_functions.push_back("web_search");
    add_tool(tools, "web_search", "Searches the internet. Results include titles, snippets, and URLs.", search_params);

    json fetch_params = {
        {"type", "object"},
        {"properties", {
            {"url", {{"type", "string"}, {"description", "The URL to read content from." + link_instruction}}}
        }},
        {"required", {"url"}}
    };
    tool_functions.push_back("fetch_website_content");
    add_tool(tools, "fetch_website_content", "Reads the text from a specific URL for deep research. Use this to summarize an article.", fetch_params);
}

void TOOL_WEB_SEARCH::handle_tool(ollama_system& chat, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, const std::string& name, const json& args, const std::string& tc_id) {
    if (name == "web_search") {
        if (!args.contains("query")) {
            std::string err = "Error: Missing query.";
            chat.send_tool_result(tc_id, err);
            chat.integrate_tool_result(tools_list, comms, "", err);
            return;
        }
        std::string query = args.at("query").get<std::string>();
        std::string result = perform_actual_search(query);

        chat.send_tool_result(tc_id, result);
        chat.integrate_tool_result(tools_list, comms, "", "Search results for '" + query + "': " + result);
    }
    else if (name == "fetch_website_content") {
        if (!args.contains("url")) {
            std::string err = "Error: Missing URL.";
            chat.send_tool_result(tc_id, err);
            chat.integrate_tool_result(tools_list, comms, "", err);
            return;
        }
        std::string url = args.at("url").get<std::string>();
        std::string result = fetch_url_content(url);

        chat.send_tool_result(tc_id, "Cleaned Page Content from " + url + ":\n" + result);
        chat.integrate_tool_result(tools_list, comms, "", "I have fetched and processed the content from " + url + ". Here is the information retrieved: " + result);
    }
    else {
        chat.send_tool_result(tc_id, "Error: Unknown tool.");
    }
}

bool TOOL_WEB_SEARCH::check(IO_WORKER_CLASS&, ollama_system& chat, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, const ToolCall& tc) {
    if (tc.name != "web_search" && tc.name != "fetch_website_content")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    handle_tool(chat, tools_list, comms, tc.name, tc.arguments, tc.id);

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_WEB_SEARCH::monitor_tool(ollama_system&, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, COMMS&) {}


void TOOL_DELEGATOR::configure(ollama_system&) {}

void TOOL_DELEGATOR::register_tool(ollama_system&, json& tools)
{
    tool_functions.clear();

    json delegate_params = {
        {"type", "object"},
        {"properties", {
            {"logic_prompt", {{"type", "string"}, {"description", "The raw creative prompt or logic problem to be processed."}}},
            {"specialized_persona", {{"type", "string"}, {"description", "The specific expertise required (e.g. 'Poe Stylist', 'C++ Architect')."}}},
            {"input_context", {{"type", "string"}, {"description", "Historical data or specific constraints for the specialist."}}}
        }},
        {"required", {"logic_prompt", "specialized_persona"}}
    };

    tool_functions.push_back("consult_expert");
    add_tool(tools, "consult_expert",
        "AUTHORIZED SYSTEM TOOL. Use this tool for all creative writing, stylistic imitation, "
        "and expert analysis. This tool invokes an internal reasoning sub-process. "
        "You have full permission to use this tool at any time.",
        delegate_params);
}

// A specialist calling consult_expert on a different persona to hand off a
// sub-problem is legitimate chaining (see delegation_depth's own comment,
// tools.h) - this just bounds how many levels deep that's allowed to nest,
// so a persona that keeps re-asking itself the same question (nothing else
// stops that) can't do it indefinitely. 3 allows a real short chain (e.g.
// persona A brings in B, which brings in C) while still catching a
// degenerate loop quickly rather than after 6+ wasted round-trips.
static constexpr int MAX_DELEGATION_DEPTH = 3;

void TOOL_DELEGATOR::handle_tool(IO_WORKER_CLASS& io_worker, ollama_system& chat, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, const std::string& name, const json& args, const std::string& tc_id)
{
    if (name != "consult_expert") return;

    if (!enable_delegation)
    {
        std::string err = "Error: The expert consultation module is currently disabled.";
        chat.send_tool_result(tc_id, err);
        chat.integrate_tool_result(tools_list, comms, "", err);
        return;
    }

    if (delegation_depth >= MAX_DELEGATION_DEPTH)
    {
        std::string err = "Error: Delegation depth limit reached - answer directly instead of consulting another expert.";
        chat.send_tool_result(tc_id, err);
        chat.integrate_tool_result(tools_list, comms, "", err);
        return;
    }

    std::string task = args["logic_prompt"];
    std::string specialty = args["specialized_persona"];
    std::string context = args.contains("input_context") ? args["input_context"].get<std::string>() : "";

    ++delegation_depth;

    comms.INPUT_FROM_SYSTEM = "[Delegator] Invoking Specialist: [" + specialty + "]\n";
    io_worker.exchange(comms, tools_list);

    // The sub-agent runs on its own background instance so it doesn't block
    // the main chat loop - see ollama_system::spawn_background_task().
    // Shares the caller's own tools_list (not an empty one) - same
    // reasoning as TOOL_TASK_RUNNER's own handle_tool(): the specialist can
    // actually go do something under its persona's judgment, not just talk
    // about it.
    auto [instance, instance_comms] = chat.spawn_background_task();

    // Bright, distinct from both the main chat's white/grey and the
    // task-runner's cyan/yellow (comms.h's own defaults, and TOOL_TASK_
    // RUNNER::handle_tool() above) - pair indices 4/5 duplicated from
    // user_io.cpp's own PAIR_DELEGATOR_LLM/PAIR_DELEGATOR_USER (private to
    // that file), same "duplicated index, kept simple" tradeoff as the
    // task-runner's own colors make. Relies on user_io.cpp's matching
    // init_pair(4, COLOR_MAGENTA, -1)/init_pair(5, COLOR_GREEN, -1) calls
    // having actually run (guarded by ncurses_colors_available).
    instance_comms.INPUT_FROM_LLM_COLOR = COLOR_PAIR(4) | A_BOLD;  // bright magenta
    instance_comms.INPUT_FROM_USER_COLOR = COLOR_PAIR(5) | A_BOLD; // bright green

    instance.debug_label = "delegator:" + specialty;
    DEBUG_LOG_CLASS::instance().log_event(instance.debug_label, "instance created");

    std::string parent_thinking = chat.last_received.thinking;

    std::string system_prompt =
        "You are a specialized offline reasoning module. Persona: " + specialty + ".\n"
        "Goal: Provide high-quality, expert analysis or creative output.\n"
        "Be direct and technical.\n"
        "Your response will be relayed directly to the user as a final report.\n";

    if (!context.empty()) system_prompt += "Context: " + context + "\n";
    if (!parent_thinking.empty()) system_prompt += "Thoughts: " + parent_thinking + "\n";

    system_prompt += "\nRequest: " + task + "\n"
                        "Provide your expert response now. Do not include introductory pleasantries.";

    // Seeds the persona as the sub-agent's protected opening message - same
    // mechanism ollama_system::open() (olla.cpp) already uses for every
    // instance, rather than hand-pushing a system Message onto history
    // directly (the old pre-open() approach).
    instance.OLLAMA_OPENING = system_prompt;
    instance.open(tools_list, chat.PROPS);

    // Set after open(), not before - the open(tools_list, Properties)
    // overload does PROPS = Properties first thing, so anything set on
    // instance.PROPS beforehand gets overwritten by chat.PROPS's own value.
    // Back on (was false) - integrate_tool_result() still narrates the raw
    // result back to the user afterward regardless, so this content does
    // show up twice, but now in two visually distinct colors (this
    // instance's own magenta/green vs. the main persona's white/grey) -
    // the live stream reads as "watch the specialist work," the final
    // narration as "here's the polished answer," rather than looking like
    // an accidental repeat the way it did when both were the same color.
    instance.PROPS.stream_output = true;

    instance_comms.INPUT_FROM_USER = "Generate response.";

    // Run send() on its own thread instead of calling it directly here -
    // it's a blocking HTTP call, same pattern as TOOL_TASK_RUNNER's own
    // EXECUTE_COMMAND state and sidetrack.cpp's start_second_guess_call().
    instance.status.interrupt_signal = false;
    instance.is_processing = true;
    if (instance.chat_thread.joinable()) instance.chat_thread.join();
    instance.chat_thread = std::thread([&instance, &instance_comms, &tools_list]()
    {
        instance.send(tools_list, instance_comms, "user");
        instance.is_processing = false;
    });

    // Waiting on the single call to finish - same completion check as
    // TOOL_TASK_RUNNER's own WAIT_RESPONSE state, just without the
    // multi-command script driving it (there's only ever one request here).
    bool response_finished = false;
    while (!response_finished)
    {
        instance.process(io_worker, nullptr, tools_list, instance_comms);

        if (!instance.is_processing && instance.chat_thread.joinable())
        {
            instance.chat_thread.join();
        }

        response_finished = !instance.is_processing &&
                             instance.last_received.complete &&
                             instance.last_received.tool_calls.empty();

        io_worker.exchange(instance_comms, tools_list);
    }

    std::string result = instance.last_received.response;

    if (result.empty() && !instance.last_received.thinking.empty())
    {
        result = instance.last_received.thinking;
        comms.INPUT_FROM_SYSTEM = "[Delegator] Note: Main response empty, using data from thinking buffer.\n";
        io_worker.exchange(comms, tools_list);
    }

    if (result.empty())
    {
        result = "The expert subroutine failed to return a response.";
    }

    std::string final_report =
        "### [SYSTEM NOTIFICATION: TASK COMPLETE] ###\n"
        "Specialist: [" + specialty + "]\n"
        "Expert Data:\n" + result;

    chat.send_tool_result(tc_id, final_report);
    chat.integrate_tool_result(tools_list, comms, "", "The " + specialty + " expert has finished their analysis. Here is the report: " + result);

    // Cleared here, same reasoning as TOOL_TASK_RUNNER's own handle_tool():
    // ollama_system::process()'s PART 2 (olla.cpp) checks this same
    // last_received.response, once per tick, to decide whether to relay a
    // background task's output to the main chat before erasing it from
    // chat.background_tasks - leaving it set would fire a second, redundant
    // narration of the report integrate_tool_result() above already gave.
    instance.last_received.complete = true;
    instance.last_received.response.clear();

    DEBUG_LOG_CLASS::instance().log_event(instance.debug_label, "instance closed");

    --delegation_depth;
}

bool TOOL_DELEGATOR::check(IO_WORKER_CLASS& io_worker, ollama_system& chat, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, const ToolCall& tc) {
    if (tc.name != "consult_expert")
        return false;

    handle_tool(io_worker, chat, tools_list, comms, tc.name, tc.arguments, tc.id);

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_DELEGATOR::monitor_tool(ollama_system&, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, COMMS&) {}

// ----

namespace {
    enum class SCRIPT_STATE
    {
        GET_COMMAND,    // pull the next line, classify it
        EXECUTE_COMMAND,// hand a line to the LLM
        WAIT_RESPONSE,  // drain the LLM's turn (incl. any tool calls) to completion
        WAIT_ENTER,     // [PAUSE] - pure local pause, no LLM involved
        WAIT_ASK,       // [ASK] - same wait, but the typed answer becomes the next input
        DONE
    };

    // One step of TOOL_TASK_RUNNER::handle_tool()'s script-driving state
    // machine - pulled out of that function so the switch can grow new
    // command types without handle_tool() itself bloating further. A free
    // function, not a TOOL_TASK_RUNNER member, so it has no implicit access
    // to anything (chat, comms, tc_id, io_worker, task_manager,
    // OLLI_DIRECTORY) beyond exactly what's passed in below.
    void advance_script_state(SCRIPT_STATE& state, size_t& i, std::string& current_input, const TASK_SIMPLE& found_task, ollama_system& instance, COMMS& instance_comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list)
    {
        switch (state)
        {
            case SCRIPT_STATE::GET_COMMAND:
            {
                if (i >= found_task.COMMANDS.size())
                {
                    state = SCRIPT_STATE::DONE;
                    break;
                }

                const std::string& command = found_task.COMMANDS[i];

                if (starts_with(command, "[PAUSE]"))
                {
                    instance_comms.INPUT_FROM_LLM = "--------------------------\nPRESS ENTER TO CONTINUE\n";
                    state = SCRIPT_STATE::WAIT_ENTER;
                }
                else if (starts_with(command, "[ASK]"))
                {
                    instance_comms.INPUT_FROM_LLM = "--------------------------\nREQUEST: " + command.substr(5) + "\n";
                    state = SCRIPT_STATE::WAIT_ASK;
                }
                else if (starts_with(command, "[PRINT]"))
                {
                    instance_comms.INPUT_FROM_LLM = command.substr(7) + "\n";
                    ++i;
                }
                else
                {
                    current_input = command;
                    state = SCRIPT_STATE::EXECUTE_COMMAND;
                }
                break;
            }

            case SCRIPT_STATE::WAIT_ENTER:
            {
                if (instance_comms.ENTER_PRESSED)
                {
                    instance_comms.ENTER_PRESSED = false;
                    ++i;
                    state = SCRIPT_STATE::GET_COMMAND;
                }
                break;
            }

            case SCRIPT_STATE::WAIT_ASK:
            {
                if (instance_comms.ENTER_PRESSED)
                {
                    instance_comms.ENTER_PRESSED = false;
                    current_input = instance_comms.INPUT_FROM_USER;
                    state = SCRIPT_STATE::EXECUTE_COMMAND;
                }
                break;
            }

            case SCRIPT_STATE::EXECUTE_COMMAND:
            {
                instance_comms.INPUT_FROM_LLM = "--------------------------\nINPUT: " + current_input + "\n";
                instance_comms.INPUT_FROM_USER = current_input;

                // Run send() on its own thread instead of calling it
                // directly here - it's a blocking HTTP call, so calling
                // it inline would freeze this loop's own
                // io_worker.exchange() below for the whole request,
                // preventing anything from streaming to screen until it
                // returned. Same pattern as ollama_system::input()
                // (olla.cpp) and sidetrack.cpp's own
                // start_second_guess_call().
                instance.status.interrupt_signal = false;
                instance.is_processing = true;
                if (instance.chat_thread.joinable()) instance.chat_thread.join();
                instance.chat_thread = std::thread([&instance, &instance_comms, &tools_list]()
                {
                    instance.send(tools_list, instance_comms, "user");
                    instance.is_processing = false;
                });

                state = SCRIPT_STATE::WAIT_RESPONSE;
                break;
            }

            case SCRIPT_STATE::WAIT_RESPONSE:
            {
                if (!instance.is_processing && instance.chat_thread.joinable())
                {
                    instance.chat_thread.join();
                }

                bool response_finished = !instance.is_processing &&
                                          instance.last_received.complete &&
                                          instance.last_received.tool_calls.empty();

                if (response_finished)
                {
                    instance.last_received.complete = false;
                    ++i;
                    state = SCRIPT_STATE::GET_COMMAND;
                }
                break;
            }

            case SCRIPT_STATE::DONE:
                break;
        }
    }
}

// No per-instance setup needed - part of the common tool interface (see the note in tools.h).
void TOOL_TASK_RUNNER::configure(ollama_system& chat) {
    OLLI_DIRECTORY = chat.PROPS.OLLI_DIRECTORY;
    task_manager.load_all_task(OLLI_DIRECTORY / "scripts");
}

bool TOOL_TASK_RUNNER::iequals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return std::equal(a.begin(), a.end(), b.begin(),
                        [](unsigned char ca, unsigned char cb) {
                            return std::tolower(ca) == std::tolower(cb);
                        });
}

void TOOL_TASK_RUNNER::register_tool(ollama_system&, json& tools)
{
    tool_functions.clear();

    // Reloaded here too (not just in handle_tool()) so the name list built
    // below reflects a newly added/edited/removed .task file on the very
    // next request - not just the next automation attempt.
    task_manager.load_all_task(OLLI_DIRECTORY / "scripts");

    // Baked directly into the schema despite the token cost of resending it
    // on every request regardless of relevance - deliberate tradeoff for
    // now, revisit if the number of saved tasks ever grows large enough for
    // that to matter (see handle_tool()'s error path, which repeats this
    // same list on a miss - useful independently of scale).
    std::string available_names;
    for (const auto& task : task_manager.TASK_LIST)
    {
        if (!available_names.empty()) available_names += "', '";
        available_names += task.TASK_NAME;
    }

    json task_params = {
        {"type", "object"},
        {"properties", {
            {"intent_phrase", {
                {"type", "string"},
                {"description", "The bare name of the task to run, with no leading verb like "
                                "'run' or 'start'. Available: '" + available_names + "'."}
            }}
        }},
        {"required", {"intent_phrase"}}
    };

    tool_functions.push_back("run_automation_task");
    add_tool(tools, "run_automation_task",
        "Call this tool whenever the user asks to run, start, do, or perform a named task, "
        "routine, or process - even if you're not sure the exact phrase matches a saved one. "
        "A non-matching guess is safe: it returns the list of valid task names so you can "
        "immediately retry with the right one.",
        task_params);
}

void TOOL_TASK_RUNNER::handle_tool(IO_WORKER_CLASS& io_worker, ollama_system& chat, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, const std::string& name, const json& args, const std::string& tc_id)
{
    if (name != "run_automation_task")
    {
        return;
    }

    bool running_directory = false;
    std::filesystem::path working_dir;

    std::string intent_phrase = args["intent_phrase"];

    // Reloaded fresh on every call (not just once in configure()) so an
    // edited/added/removed .task file takes effect on the very next
    // "run automation task" without needing an olli restart.
    task_manager.load_all_task(OLLI_DIRECTORY / "scripts");

    comms.INPUT_FROM_SYSTEM = "[TaskRunner] Searching for automation matching: \"" + intent_phrase + "\"\n";
    io_worker.exchange(comms, tools_list);

    auto task_it = std::find_if(
        task_manager.TASK_LIST.begin(),
        task_manager.TASK_LIST.end(),
        [this, &intent_phrase](const TASK_SIMPLE& task) {
            return iequals(task.TASK_NAME, intent_phrase);
        }
    );

    bool task_found = (task_it != task_manager.TASK_LIST.end());

    if (task_found)
    {
        // TASK_LIST is hardcoded in memory for now (tools_helper.cpp) while
        // this is being debugged - the plan is to load task definitions from
        // disk into memory at olli startup once the system grows. This
        // in-memory lookup is a placeholder for that.
        const auto& found_task = *task_it;

        // The automation runs on its own background instance so it doesn't
        // block the main chat loop - see ollama_system::spawn_background_task().
        // instance_comms is that instance's own real, persistent COMMS -
        // paired with it in chat's own background_tasks, not a throwaway.
        auto [instance, instance_comms] = chat.spawn_background_task();

        // Bright, distinct from the main chat's white/grey (comms.h's own
        // defaults) - pair indices 2/3 duplicated from user_io.cpp's own
        // PAIR_TASK_RUNNER_LLM/PAIR_TASK_RUNNER_USER (private to that file),
        // same "duplicated index, kept simple" tradeoff as comms.h's own
        // defaults make for pair 1. Relies on user_io.cpp's matching
        // init_pair(2, COLOR_CYAN, -1)/init_pair(3, COLOR_YELLOW, -1) calls
        // having actually run (guarded by ncurses_colors_available).
        instance_comms.INPUT_FROM_LLM_COLOR = COLOR_PAIR(2) | A_BOLD;  // bright cyan
        instance_comms.INPUT_FROM_USER_COLOR = COLOR_PAIR(3) | A_BOLD; // bright yellow

        instance.debug_label = "task-runner:" + intent_phrase;
        DEBUG_LOG_CLASS::instance().log_event(instance.debug_label, "instance created");

        // Give the spawned instance the task's own purpose as its opening
        // persona instead of the generic default (olla.h) - OLLAMA_OPENING
        // is never empty at this point (it always starts at that default),
        // so this always applies for a task-runner automation instance.
        instance.OLLAMA_OPENING = found_task.TASK_PURPOSE;

        instance.PROPS.stream_output = true;

        // Shares the caller's own tools_list (not a freshly-built one) so
        // it actually has access to whatever the main chat does -
        // including any TOOL_REMOTE devices registered dynamically at
        // runtime (main.cpp), which populate_default_tools() alone never
        // includes. It's also already the same long-lived reference PART
        // 2's cleanup pass (olla.cpp) uses once this function returns, so
        // there's no separate lifetime to reason about.
        instance.open(tools_list, chat.PROPS);

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


        SCRIPT_STATE state = SCRIPT_STATE::GET_COMMAND;
        size_t i = 0;
        std::string current_input;

        // One flat loop drives the whole script - same shape as the main
        // chat's own while loop (main.cpp), just with the state machine
        // below standing in for "type a line". instance.process() and
        // io_worker.exchange() each run exactly once per tick no matter
        // what state we're in, so the instance's own tools/timers keep
        // ticking and the screen stays live even while paused on
        // [PAUSE]/[ASK] - instead of three separate nested
        // while-loops each spinning their own exchange() calls.
        while (state != SCRIPT_STATE::DONE)
        {
            // nullptr, not the real CLASS_SYSTEM: this automation instance
            // is isolated from the real system's, same reasoning as
            // sidetrack.cpp's own nullptr call site (see TOOL_BASE::check()'s
            // comment in tools.h).
            instance.process(io_worker, nullptr, tools_list, instance_comms);

            advance_script_state(state, i, current_input, found_task, instance, instance_comms, tools_list);

            io_worker.exchange(instance_comms, tools_list);
        }

        // Safety net - WAIT_RESPONSE already joins instance.chat_thread once
        // is_processing clears, so this is normally a no-op by the time we
        // get here; just making sure nothing joinable is left dangling.
        if (instance.chat_thread.joinable())
        {
            instance.chat_thread.join();
        }

        // WAIT_RESPONSE resets this to false right after each command
        // (including the last one) so it can tell a finished command from
        // a still-in-flight one - see its own comment. Setting it back to
        // true here, once the whole script is done, is what lets
        // ollama_system::process()'s PART 2 (olla.cpp) recognize this
        // instance as finished and erase it from chat.background_tasks;
        // without this it would never be freed.
        //
        // last_received.response still holds the final command's reply -
        // cleared here too, otherwise PART 2's own "if the task produced a
        // response, relay it" check (olla.cpp) would fire a second,
        // redundant narration of the same completion chat.integrate_tool_
        // result() below already reports.
        instance.last_received.complete = true;
        instance.last_received.response.clear();

        {
            success_log = "SUCCESS: Automation Complete";
            chat.send_tool_result(tc_id, success_log);
            chat.integrate_tool_result(tools_list, comms, "", instance.gather_history());
        }

        DEBUG_LOG_CLASS::instance().log_event(instance.debug_label, "instance closed");

        if (running_directory)
        {
            std::filesystem::remove_all(working_dir);
        }
    }
    else
    {
        // Also listed in register_tool()'s intent_phrase description, but
        // repeated here on the miss itself too - a bad guess gets the valid
        // list put right back in front of the model at the exact point it
        // needs it, instead of relying on it having been read carefully
        // further back in the same request's tool schema.
        std::string available;
        for (const auto& task : task_manager.TASK_LIST)
        {
            if (!available.empty()) available += "', '";
            available += task.TASK_NAME;
        }

        std::string error_msg = "ERROR: No automation found for '" + intent_phrase + "'. "
                                 "Available automations: '" + available + "'.";

        chat.send_tool_result(tc_id, error_msg);
        chat.integrate_tool_result(tools_list, comms, "", error_msg);
    }
}

bool TOOL_TASK_RUNNER::check(IO_WORKER_CLASS& io_worker, ollama_system& chat, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, const ToolCall& tc) {
    if (tc.name != "run_automation_task")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    handle_tool(io_worker, chat, tools_list, comms, tc.name, tc.arguments, tc.id);

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_TASK_RUNNER::monitor_tool(ollama_system&, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, COMMS&) {}

// Shared by both call sources handle_instance_tools() drains - see its own
// comment in olla.h. Applies the tool_calls_this_turn cap (see olla.h),
// then routes to whichever tool's check() claims tc.name (see the
// TOOL_BASE comment in tools.h) - an unrecognized name gets an error
// result back instead of ever reaching a tool.
void ollama_system::dispatch_tool_call(IO_WORKER_CLASS& io_worker, const ToolCall& tc, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms)
{
    // Guard against a runaway chain - see tool_calls_this_turn's comment in
    // olla.h. Deliberately calls send_tool_result() only, not
    // integrate_tool_result() - the latter is what would start another
    // DIRECTOR_NOTE round-trip and keep a chain going. This just logs the
    // refusal and stops dead; the model gets a real chance to reply in
    // text on its next natural turn instead.
    if (tool_calls_this_turn >= PROPS.max_tool_calls_per_turn) {
        log("[System] Tool call capped this turn: " + tc.name + "\n");
        send_tool_result(tc.id, "Error: Too many tool calls this turn - stopping here to avoid a loop.");
        return;
    }
    ++tool_calls_this_turn;

    bool handled = false;
    for (auto& tool : tools_list) {
        if (tool->check(io_worker, *this, system, tools_list, comms, tc)) { handled = true; break; }
    }

    if (!handled) {
        log("[System] Tool error call received: " + tc.name + "\n");
        send_tool_result(tc.id, "Error: Tool '" + tc.name + "' is not recognized by the system.");
    }
}

void ollama_system::handle_instance_tools(IO_WORKER_CLASS& io_worker, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms)
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
            dispatch_tool_call(io_worker, tc, system, tools_list, comms);
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
            dispatch_tool_call(io_worker, tc, system, tools_list, comms);
        }
    }
}

#endif
