#ifndef tools_cpp
#define tools_cpp

#include <algorithm>
#include <regex>
#include <thread>

#include <curl/curl.h>

#include "tools.h"
#include "olla.h"
#include "user_io.h"
#include "io_worker.h"
#include "tool_worker.h"
#include "tools_task_script.h"

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

bool TOOL_SET_THINKING_MODE::check(IO_WORKER_CLASS&, ollama_system& chat, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, TOOL_WORKER_CLASS*, COMMS& comms, const ToolCall& tc) {
    if (tc.name != "set_thinking_mode")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    handle_tool(chat, comms, tc.name, tc.arguments, tc.id);

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_SET_THINKING_MODE::monitor_tool(ollama_system&, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, TOOL_WORKER_CLASS*, COMMS&) {}

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

std::pair<bool, std::string> TOOL_WEB_SEARCH::curl_get(const std::string& url, long timeout_seconds, const std::string& user_agent) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        CURL* curl = curl_easy_init();
        if (!curl) return {false, "Error: Could not initialize libcurl."};

        std::string readBuffer;
        char errbuf[CURL_ERROR_SIZE];
        errbuf[0] = '\0';

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_seconds);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECONDS);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);
        if (!user_agent.empty())
            curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());

        CURLcode res = curl_easy_perform(curl);
        curl_easy_cleanup(curl);

        if (res == CURLE_OK) return {true, readBuffer};

        // Only a timeout gets a second attempt - a slow DNS/TLS/server
        // hiccup can easily resolve itself a moment later, but a bad
        // URL/connection-refused/host-not-found will just fail identically
        // again, so retrying those would only double the wait for nothing.
        if (res == CURLE_OPERATION_TIMEDOUT && attempt == 0) continue;

        std::string detail = errbuf[0] != '\0' ? std::string(": ") + errbuf : "";
        return {false, "Error: libcurl failed (" + std::string(curl_easy_strerror(res)) + ")" + detail};
    }

    return {false, "Error: libcurl failed (unreachable)."};
}

std::pair<bool, std::string> TOOL_WEB_SEARCH::perform_actual_search(const std::string& query, COMMS& comms) {
    CURL* escape_handle = curl_easy_init();
    if (!escape_handle) return {false, "Error: Could not initialize libcurl."};

    char* output = curl_easy_escape(escape_handle, query.c_str(), static_cast<int>(query.length()));
    std::string encodedQuery(output);
    curl_free(output);
    curl_easy_cleanup(escape_handle);

    std::string url = "https://serpapi.com/search.json?q=" + encodedQuery + "&api_key=" + apiKey;

    auto [ok, body] = curl_get(url, SEARCH_TIMEOUT_SECONDS);
    if (!ok) return {false, body};

    try {
        auto data = json::parse(body);

        if (data.contains("error")) {
            return {false, "Search API Error: " + data["error"].get<std::string>()};
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

                if (!link.empty())
                {
                    std::lock_guard<std::mutex> lock(output_buffer_mutex);
                    comms.TOOL_ATTACHMENTS.emplace_back("link", title, link);
                }
            }
        } else {
            summary = "No specific snippets found.";
        }
        summary += "SEARCH_RESULTS_END";
        return {true, summary};
    } catch (const std::exception& e) {
        return {false, "Error: Failed to parse search engine response: " + std::string(e.what())};
    }
}

std::pair<bool, std::string> TOOL_WEB_SEARCH::fetch_url_content(const std::string& url, COMMS& comms) {
    auto [ok, body] = curl_get(url, FETCH_TIMEOUT_SECONDS, "Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    if (!ok) return {false, body};

    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex);
        comms.TOOL_ATTACHMENTS.emplace_back("link", url, url);
    }

    // Strip HTML noise so the model isn't parsing markup as content
    std::string cleanText = strip_html_tags(body);

    if (cleanText.length() > 4000) return {true, cleanText.substr(0, 4000) + "... [truncated]"};
    return {true, cleanText};
}

void TOOL_WEB_SEARCH::register_tool(ollama_system&, json& tools) {
    tool_functions.clear();

    // Told to the model via each tool's description so its final answer
    // doesn't dump a raw URL into the chat text - every link from this
    // tool is already surfaced separately (see COMMS::TOOL_ATTACHMENTS,
    // comms.h) and shown/opened through its own UI, not through anything
    // the model writes.
    std::string link_instruction = " Do not include the raw URL in your final answer - links are shown to the user separately. Refer to a source by name instead (e.g. \"according to weather.com\").";

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

void TOOL_WEB_SEARCH::handle_tool(ollama_system& chat, std::vector<std::unique_ptr<TOOL_BASE>>&, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const std::string& name, const json& args, const std::string& tc_id) {
    // integrate_tool_result()'s default framing ("report this real result...
    // without changing the facts") reads the same whether the tool
    // succeeded or not, so a plain curl/API error used to get relayed as if
    // it were a normal answer. This override tells the model plainly it
    // isn't real data, and trusts it to explain the raw error text (e.g.
    // "Timeout was reached") in its own words rather than us translating it.
    static const std::string failure_instruction =
        "This attempt did NOT succeed - it is an error, not real data. Tell "
        "the user plainly, in your own words, that it failed and what "
        "likely went wrong. Do not present this as a real answer to their "
        "question.";

    if (name == "web_search") {
        if (!args.contains("query")) {
            std::string err = "Error: Missing query.";
            chat.send_tool_result(tc_id, err);
            chat.integrate_tool_result(tool_worker, comms, failure_instruction, err);
            return;
        }
        std::string query = args.at("query").get<std::string>();
        auto [ok, result] = perform_actual_search(query, comms);

        chat.send_tool_result(tc_id, result);
        if (ok)
            chat.integrate_tool_result(tool_worker, comms, "", "Search results for '" + query + "': " + result);
        else
            chat.integrate_tool_result(tool_worker, comms, failure_instruction, "The web search for '" + query + "' failed: " + result);
    }
    else if (name == "fetch_website_content") {
        if (!args.contains("url")) {
            std::string err = "Error: Missing URL.";
            chat.send_tool_result(tc_id, err);
            chat.integrate_tool_result(tool_worker, comms, failure_instruction, err);
            return;
        }
        std::string url = args.at("url").get<std::string>();
        auto [ok, result] = fetch_url_content(url, comms);

        chat.send_tool_result(tc_id, "Cleaned Page Content from " + url + ":\n" + result);
        if (ok)
            chat.integrate_tool_result(tool_worker, comms, "", "I have fetched and processed the content from " + url + ". Here is the information retrieved: " + result);
        else
            chat.integrate_tool_result(tool_worker, comms, failure_instruction, "Fetching content from " + url + " failed: " + result);
    }
    else {
        chat.send_tool_result(tc_id, "Error: Unknown tool.");
    }
}

bool TOOL_WEB_SEARCH::check(IO_WORKER_CLASS&, ollama_system& chat, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const ToolCall& tc) {
    if (tc.name != "web_search" && tc.name != "fetch_website_content")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    handle_tool(chat, tools_list, tool_worker, comms, tc.name, tc.arguments, tc.id);

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_WEB_SEARCH::monitor_tool(ollama_system&, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, TOOL_WORKER_CLASS*, COMMS&) {}


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

void TOOL_DELEGATOR::handle_tool(IO_WORKER_CLASS& io_worker, ollama_system& chat, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const std::string& name, const json& args, const std::string& tc_id)
{
    if (name != "consult_expert") return;

    if (!enable_delegation)
    {
        std::string err = "Error: The expert consultation module is currently disabled.";
        chat.send_tool_result(tc_id, err);
        chat.integrate_tool_result(tool_worker, comms, "", err);
        return;
    }

    if (delegation_depth >= MAX_DELEGATION_DEPTH)
    {
        std::string err = "Error: Delegation depth limit reached - answer directly instead of consulting another expert.";
        chat.send_tool_result(tc_id, err);
        chat.integrate_tool_result(tool_worker, comms, "", err);
        return;
    }

    std::string task = args["logic_prompt"];
    std::string specialty = args["specialized_persona"];
    std::string context = args.contains("input_context") ? args["input_context"].get<std::string>() : "";

    ++delegation_depth;

    comms.INPUT_FROM_SYSTEM = "[Delegator] Invoking Specialist: [" + specialty + "]\n";
    io_worker.exchange(comms, tool_worker);

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
    instance.chat_thread = std::thread([&instance, tool_worker, &instance_comms]()
    {
        instance.send(tool_worker, instance_comms, "user");
        instance.is_processing = false;
    });

    // Waiting on the single call to finish - same completion check as
    // TOOL_TASK_RUNNER's own WAIT_RESPONSE state, just without the
    // multi-command script driving it (there's only ever one request here).
    bool response_finished = false;
    while (!response_finished)
    {
        instance.process(io_worker, nullptr, tools_list, tool_worker, instance_comms);

        if (!instance.is_processing && instance.chat_thread.joinable())
        {
            instance.chat_thread.join();
        }

        response_finished = !instance.is_processing &&
                             instance.last_received.complete &&
                             instance.last_received.tool_calls.empty();

        io_worker.exchange(instance_comms, tool_worker);
    }

    std::string result = instance.last_received.response;

    if (result.empty() && !instance.last_received.thinking.empty())
    {
        result = instance.last_received.thinking;
        comms.INPUT_FROM_SYSTEM = "[Delegator] Note: Main response empty, using data from thinking buffer.\n";
        io_worker.exchange(comms, tool_worker);
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
    chat.integrate_tool_result(tool_worker, comms, "", "The " + specialty + " expert has finished their analysis. Here is the report: " + result);

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

bool TOOL_DELEGATOR::check(IO_WORKER_CLASS& io_worker, ollama_system& chat, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const ToolCall& tc) {
    if (tc.name != "consult_expert")
        return false;

    handle_tool(io_worker, chat, tools_list, tool_worker, comms, tc.name, tc.arguments, tc.id);

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_DELEGATOR::monitor_tool(ollama_system&, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, TOOL_WORKER_CLASS*, COMMS&) {}

// ----
// TOOL_TASK_RUNNER's script-driving state machine (SCRIPT_STATE,
// advance_script_state(), and its command_*() functions) lives in
// tools_task_script.h/.cpp now, not here - see that file for the full
// switch this class's handle_tool() (below) drives every tick.

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

void TOOL_TASK_RUNNER::handle_tool(IO_WORKER_CLASS& io_worker, ollama_system& chat, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const std::string& name, const json& args, const std::string& tc_id)
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
    io_worker.exchange(comms, tool_worker);

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

        instance.open(tools_list, chat.PROPS);

        // A scratch directory for the task, cleaned up (remove_all below)
        // once the automation finishes.
        if (found_task.TASK_DIRECTORY != "")
        {
            working_dir = OLLI_DIRECTORY / (found_task.TASK_DIRECTORY);
            std::filesystem::create_directories(working_dir);
            running_directory = true;
        }

        // Separate from working_dir above, and separate from working_dir's
        // own lifetime too - a plain local, never remove_all()'d, so
        // whatever FILE_IN/FILE_APPEND (advance_script_state()) leaves here
        // persists indefinitely after this function returns. Always
        // computed, regardless of whether TASK_DIRECTORY is set - falls
        // back to TASK_NAME (also run through sanitize_path_segment()) when
        // it isn't.
        std::filesystem::path files_dir = OLLI_DIRECTORY / "files" /
            sanitize_path_segment(found_task.TASK_DIRECTORY.empty() ? found_task.TASK_NAME : found_task.TASK_DIRECTORY);

        std::string success_log = "SUCCESS: Automation found. Sequence loading...";
        chat.send_tool_result(tc_id, success_log);


        SCRIPT_STATE state = SCRIPT_STATE::GET_COMMAND;
        size_t i = 0;
        std::string current_input;

        // Held here (this function's own stack, nothing new on tool_worker
        // or the instance) instead of letting instance.process() drain them
        // live, when found_task.delay_tool_returns (the default) - a stray
        // remote-tool event/result mid-script (a timer firing, presence
        // changing) used to get absorbed into this throwaway instance and
        // narrated under its own system prompt, out of context, instead of
        // ever reaching the real conversation. Replayed once the script's
        // own command list is done, below.
        std::vector<TOOL_RESULT> held_results;
        std::vector<TOOL_EVENT> held_events;

        // [WAIT_FOR_RESULT] support. wait_baseline_results is how many
        // items were already sitting in held_results before the command
        // this marker is waiting on even started - captured in
        // pre_command_result_baseline at the one tick EXECUTE_COMMAND runs
        // (see below), NOT when the marker itself is later reached. That
        // distinction matters: a fast remote tool (a local clock, say) can
        // answer within a tick or two, well before the script even gets to
        // its own [WAIT_FOR_RESULT] line - capturing "late" would fold that
        // already-arrived answer into the baseline instead of catching it,
        // confirmed live (the wait just hung, waiting for a second answer
        // that was never coming). Anything already queued before the
        // triggering command even started (e.g. an earlier, unmarked
        // command's own result) is a stale backlog item, not what this
        // wait is for - left completely alone, still reported normally in
        // the end-of-script replay below. held_events is deliberately not
        // part of this at all, see the WAIT_TOOL_RESULT handling below for
        // why.
        size_t pre_command_result_baseline = 0;
        size_t wait_baseline_results = 0;
        bool wait_baseline_set = false;

        // Save slot for ENABLE_KEYBOARD_INPUT, persistent across whatever
        // multi-tick WAIT_ENTER/WAIT_ASK wait is currently in progress - see
        // command_pause()'s own comment (tools_task_script.h) for what this
        // is for. Initial value never actually read before being set by
        // whichever command first uses it.
        bool keyboard_was_enabled = true;

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
            // EXECUTE_COMMAND is transient - one tick only, always moving
            // straight to WAIT_RESPONSE before the loop comes back around
            // (command_execute_command(), tools_task_script.cpp) - so
            // catching state here, before this same tick's own pre-drain
            // below runs, is exactly "held_results.size() the instant
            // before this command's own request goes out". See
            // pre_command_result_baseline's own comment above for why this
            // has to happen here and not when [WAIT_FOR_RESULT] is reached.
            if (state == SCRIPT_STATE::EXECUTE_COMMAND)
            {
                pre_command_result_baseline = held_results.size();
            }

            // Taken out from under instance.process()'s own PART 5 (olla.cpp)
            // before it runs, so there's nothing left for it to drain/narrate
            // live this tick - held_results/held_events above just accumulate
            // whatever shows up, tool_worker's own buffering unaffected
            // either way. instance.process() itself is called completely
            // unchanged right after (see its own comment below) - this
            // doesn't touch its signature or behavior at all, so the loop
            // keeps whatever incidental pacing it already had from that call.
            if (found_task.delay_tool_returns && tool_worker)
            {
                // Only claims results for calls this instance itself
                // dispatched (instance.outstanding_tool_call_ids, olla.h) -
                // the old blind get_pending_result(out) drained whatever
                // was oldest in tool_worker's shared queue regardless of
                // which instance it actually belonged to, which could (and
                // did) steal a different instance's own result out from
                // under it. held_events is left as a blind drain for now -
                // events have no call_id to match against at all yet (see
                // TODO.md's event/result correlation entry).
                TOOL_RESULT held_result;
                for (auto it = instance.outstanding_tool_call_ids.begin(); it != instance.outstanding_tool_call_ids.end(); )
                {
                    if (tool_worker->get_pending_result(*it, held_result))
                    {
                        held_results.push_back(held_result);
                        it = instance.outstanding_tool_call_ids.erase(it);
                    }
                    else
                    {
                        ++it;
                    }
                }

                TOOL_EVENT held_event;
                while (tool_worker->get_pending_event(held_event))
                    held_events.push_back(held_event);
            }

            // nullptr, not the real CLASS_SYSTEM: this automation instance
            // is isolated from the real system's, same reasoning as
            // sidetrack.cpp's own nullptr call site (see TOOL_BASE::check()'s
            // comment in tools.h). tools_list/tool_worker are both real,
            // though - shares the caller's own, same reasoning as
            // TOOL_DELEGATOR's own handle_tool(): the script can actually go
            // do something under its own judgment, not just talk about it.
            instance.process(io_worker, nullptr, tools_list, tool_worker, instance_comms);

            // [WAIT_FOR_RESULT] - advance_script_state()'s own case for this
            // state is a no-op (tools_task_script.cpp); resolving it needs
            // held_results, which only this loop has. Waits specifically for
            // a result arriving after the triggering command started
            // (wait_baseline_results, copied from pre_command_result_
            // baseline above - not re-captured here, see its own comment for
            // why), not just whatever's at the front - an earlier, unmarked
            // command's own stale result sitting ahead of it in the queue is
            // left untouched, still reported normally in the end-of-script
            // replay below, not mistaken for the answer this particular
            // wait is for. held_events is deliberately never
            // checked here at all - an event is by definition unsolicited
            // (a timer firing on its own, presence changing), never a direct
            // response to anything this script just asked, so one landing
            // during the wait (confirmed live: a timer set earlier in the
            // same script expiring mid-wait) must not be mistaken for the
            // answer either - it just joins the normal backlog like any
            // other event would. Narrates the real result immediately,
            // in-context, instead of holding it that long. Nothing new yet
            // just means staying in this state and checking again next tick;
            // tool_worker's own CALL_TIMEOUT_SECONDS (tool_worker.h) already
            // bounds how long that can go on for, so this can't hang forever
            // on a broken remote tool.
            if (state == SCRIPT_STATE::WAIT_TOOL_RESULT)
            {
                if (!wait_baseline_set)
                {
                    wait_baseline_results = pre_command_result_baseline;
                    wait_baseline_set = true;
                }

                if (held_results.size() > wait_baseline_results)
                {
                    TOOL_RESULT result = held_results[wait_baseline_results];
                    held_results.erase(held_results.begin() + static_cast<std::ptrdiff_t>(wait_baseline_results));
                    instance.send_tool_result(result.call_id, result.response);
                    instance.integrate_tool_result(tool_worker, instance_comms, result.special_instruction, result.response);
                    ++i;
                    state = SCRIPT_STATE::GET_COMMAND;
                    wait_baseline_set = false;
                }
            }
            else
            {
                advance_script_state(state, i, current_input, found_task, instance, instance_comms, tool_worker, files_dir, keyboard_was_enabled);
            }

            io_worker.exchange(instance_comms, tool_worker);

            // Ctrl+C during a running task - exchange() just above relays
            // it onto instance_comms (this loop passes its own instance_
            // comms, not the main chat's), where nothing else reads it:
            // main.cpp's own EXIT_REQUESTED check never gets a turn while
            // this loop has the main thread blocked. Treat it as "the
            // command list just ended" rather than plumbing a real olli
            // shutdown through here - lets a big/runaway automation be
            // stopped without killing the whole program.
            if (instance_comms.EXIT_REQUESTED)
            {
                instance_comms.EXIT_REQUESTED = false;
                state = SCRIPT_STATE::DONE;
            }

            // This loop has no pacing of its own otherwise - every call in
            // it (instance.process(), advance_script_state(), exchange())
            // can in principle return almost immediately, so without this
            // it can spin as fast as the CPU allows instead of ticking at a
            // sane rate. Same ~20ms cadence as tool_worker's own thread_main()
            // (tool_worker.cpp) and IO_WORKER_CLASS's main loop (main.cpp).
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // Safety net - WAIT_RESPONSE already joins instance.chat_thread once
        // is_processing clears, so this is normally a no-op by the time we
        // get here; just making sure nothing joinable is left dangling.
        if (instance.chat_thread.joinable())
        {
            instance.chat_thread.join();
        }

        // Replay whatever got held above, on instance/instance_comms - same
        // treatment instance.process()'s own PART 5 (olla.cpp) would have
        // given each one live, just deferred to here so it happens in the
        // script's own voice/context instead of interleaved with whatever
        // command was running when it actually arrived. send()/
        // integrate_tool_result() block until the model's reply is fully
        // generated (no internal threading), so each of these completes
        // before the next starts - no separate wait loop needed for them.
        for (auto& result : held_results)
        {
            instance.send_tool_result(result.call_id, result.response);
            instance.integrate_tool_result(tool_worker, instance_comms, result.special_instruction, result.response);
        }

        for (auto& event : held_events)
        {
            if (!event.message.empty())
            {
                // Same is_own framing distinction instance.process()'s own
                // PART 5 (olla.cpp) would apply - DELIBERATELY not acted on
                // right now, for the same reason: it reliably provoked a
                // follow-up tool call that then collided with a live,
                // reproduced full-program hang. See olla.cpp's PART 5
                // comment (same event-narration code, just here for the
                // task-runner's own end-of-script replay) for the full
                // writeup - not repeated here.
                std::string framing = "";

                instance.integrate_tool_result(tool_worker, instance_comms, framing, event.message);
            }

            // NOT instance.pending_tool_calls - instance is a background_
            // tasks entry (chat.spawn_background_task()) that gets erased
            // once this function marks it complete below, and process()'s
            // own PART 2 (olla.cpp) explicitly passes nullptr for tool_worker
            // on every tick after that (by design - a background task is
            // assumed fully drained by the time handle_tool() returns), so
            // anything still sitting in instance's own queue by then could
            // never actually reach a real remote tool. chat.pending_tool_
            // calls is the real, long-lived conversation's own queue - its
            // regular ticking (main.cpp) picks this up and dispatches it
            // for real, whenever tool_worker's response actually arrives,
            // same as any other out-of-band event already does today.
            if (!event.action_tool.empty())
            {
                chat.tool_calls_this_turn = 0;
                chat.pending_tool_calls.push({
                    "system_action_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()),
                    event.action_tool,
                    event.action_arguments
                });
            }
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
            chat.integrate_tool_result(tool_worker, comms, "", instance.gather_history());
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
        chat.integrate_tool_result(tool_worker, comms, "", error_msg);
    }
}

bool TOOL_TASK_RUNNER::check(IO_WORKER_CLASS& io_worker, ollama_system& chat, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const ToolCall& tc) {
    if (tc.name != "run_automation_task")
        return false;

    chat.log("[System] Tool call received: " + tc.name + "\n");

    handle_tool(io_worker, chat, tools_list, tool_worker, comms, tc.name, tc.arguments, tc.id);

    return true;
}

// No periodic work needed - part of the common tool interface (see the note in tools.h).
void TOOL_TASK_RUNNER::monitor_tool(ollama_system&, CLASS_SYSTEM*, std::vector<std::unique_ptr<TOOL_BASE>>&, TOOL_WORKER_CLASS*, COMMS&) {}

// Shared by both call sources handle_instance_tools() drains - see its own
// comment in olla.h. Applies the tool_calls_this_turn cap (see olla.h),
// then routes to whichever tool's check() claims tc.name (see the
// TOOL_BASE comment in tools.h) - an unrecognized name gets an error
// result back instead of ever reaching a tool.
void ollama_system::dispatch_tool_call(IO_WORKER_CLASS& io_worker, const ToolCall& tc, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms)
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

    // Built-ins first, same check()-loop as always - each tool decides for
    // itself whether tc.name is its own. Only if nothing in tools_list
    // claims it does this fall through to tool_worker: queued via
    // put_pending_call(), with a real answer (or a timeout error,
    // tool_worker.h's own CALL_TIMEOUT_SECONDS) arriving later via
    // get_pending_result(), drained in process()'s own PART 5 (olla.cpp) -
    // not synchronously here, unlike the check()-based path built-ins use.
    bool handled = false;
    for (auto& tool : tools_list) {
        if (tool->check(io_worker, *this, system, tools_list, tool_worker, comms, tc)) { handled = true; break; }
    }

    if (!handled && tool_worker) {
        outstanding_tool_call_ids.push_back(tc.id);
        owned_tool_call_ids.push_back(tc.id);
        tool_worker->put_pending_call(tc);
        handled = true;
    }

    if (!handled) {
        log("[System] Tool error call received: " + tc.name + "\n");
        send_tool_result(tc.id, "Error: Tool '" + tc.name + "' is not recognized by the system.");
    }
}

void ollama_system::handle_instance_tools(IO_WORKER_CLASS& io_worker, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms)
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
            dispatch_tool_call(io_worker, tc, system, tools_list, tool_worker, comms);
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
            dispatch_tool_call(io_worker, tc, system, tools_list, tool_worker, comms);
        }
    }
}

#endif
