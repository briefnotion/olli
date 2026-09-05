#ifndef olla_cpp
#define olla_cpp

#include "olla.h"
#include "io_worker.h"
#include "user_io.h"
#include <algorithm>

// One instance of every TOOL_* class - callers populate their own tools_list
// with this (see process()'s comment in olla.h for why tools_list is a
// reference parameter rather than living on this class). Add a new tool
// here, nowhere else.
void populate_default_tools(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list)
{
    tools_list.push_back(std::make_unique<TOOL_SET_THINKING_MODE>());
    tools_list.push_back(std::make_unique<TOOL_WEB_SEARCH>());
    tools_list.push_back(std::make_unique<TOOL_TASK_RUNNER>());
    tools_list.push_back(std::make_unique<TOOL_DELEGATOR>());
}

ollama_system::ollama_system()
{
}

void ollama_system::log(const std::string& text)
{
    // Stubbed out - comms.log() no longer exists (comms itself stopped
    // being a member of ollama_system as part of today's COMMS-ownership
    // move) and every chat.log(...) call site still expects this old
    // no-argument signature. Revisit properly when working on system
    // messages - needs a COMMS& parameter threaded through here and every
    // caller (tools.cpp, remote_tools.cpp, main.cpp, olla.cpp), same
    // pattern as integrate_tool_result(). See TODO.md.
    (void)text;
}

void ollama_system::pull_background_output(OUTPUT_CLASS& output)
{
    for (auto& task_pair : background_tasks)
    {
        output.get_response(*task_pair.second);
    }
}

std::pair<ollama_system&, COMMS&> ollama_system::spawn_background_task()
{
    // COMMS::audio (a direct pointer to the speaker) doesn't exist anymore -
    // TTS output now runs through comms_buffer_audio, which only the main
    // chat's own COMMS gets fanned into via IO_WORKER_CLASS::exchange()
    // (io_worker.cpp). Background tasks currently have no speech output at
    // all as a result - unaddressed, see this session's notes.
    background_tasks.emplace_back(std::make_unique<ollama_system>(), std::make_unique<COMMS>());
    auto& [instance, instance_comms] = background_tasks.back();
    return {*instance, *instance_comms};
}

void ollama_system::open(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list)
{
    log("[System] Connecting to " + PROPS.host + ":" + std::to_string(PROPS.port) + " (" + PROPS.model + ")\n");

    std::filesystem::create_directories(PROPS.OLLI_DIRECTORY / "output");
    std::filesystem::create_directories(PROPS.OLLI_DIRECTORY / "input");

    PROPS.path_output = PROPS.OLLI_DIRECTORY / "output";
    PROPS.path_history = ".";

    for (auto& tool : tools_list)
        tool->configure(*this);

    // register_tool() itself is called from send() (below), not here - a
    // remote tool (source/remote_tools.h) can join tools_list after open()
    // already ran, so the tools array sent to Ollama needs rebuilding fresh
    // on every request rather than fixed once at startup.

    if (PROPS.LOAD_SAVE_HISTORY_ON_DISK)
    {
        loadHistoryFromJson(PROPS.OLLI_DIRECTORY / "history.json");
    }

    // Ensure a protected (consolidation_level -1) foundational message is
    // always present: either this is a brand new history, or it was loaded
    // from disk but predates protected messages (and may have already had
    // its original opening prompt consolidated away). Either way, the model
    // needs its persona/instructions to always be present, unsummarized.
    bool has_protected_message = false;
    for (const Message& msg : history) {
        if (msg.consolidation_level < 0) {
            has_protected_message = true;
            break;
        }
    }

    if (!has_protected_message)
    {
        Message opening_msg;
        opening_msg.role = "system";
        opening_msg.content = OLLAMA_OPENING;
        opening_msg.consolidation_level = -1;
        history.insert(history.begin(), opening_msg);
    }
}

void ollama_system::open(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, OLLAMA_SYSTEM_PROPERTIES Properties)
{
    PROPS = Properties;

    PROPS.LOAD_SAVE_HISTORY_ON_DISK = false;

    open(tools_list);
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
void ollama_system::integrate_tool_result(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, std::string Special_Instruction, const std::string& raw_result)
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
        //
        // TASK/CONSTRAINTS wording deliberately frames raw_result as a real
        // fact to *report*, not raw material to freely improvise around -
        // the previous wording ("Acknowledge this info as the Assistant.
        // Stay in persona") said nothing about staying faithful to it, and
        // "Do NOT say 'The system found'" pushed toward making a real tool
        // result and a made-up one read identically. Confirmed contributing
        // to a real failure mode alongside this history no longer getting
        // deleted after one turn - see send_tool_result()'s comment further
        // down for the full history.
        std::string prompt =
            "[DIRECTOR_NOTE]\n"
            "The following raw data was just retrieved: '" + raw_result + "'.\n"
            "TASK: Report this real result to the user, in persona, without changing the facts/values it contains.\n"
            "CONSTRAINTS: Be concise. No technical jargon. Do not claim this didn't come from a real check.\n";

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
        comms.INPUT_FROM_USER = prompt;
        this->send(tools_list, comms, "system");

        // 3. This DIRECTOR_NOTE, and the raw tool result send_tool_result()
        // pushed just before it, both stay in history now rather than being
        // deleted after one turn (see send_tool_result()'s comment for why)
        // - unbounded growth is handled the same way the rest of history
        // is, by consolidate() (sidetrack.cpp) once a level actually fills
        // up, not by specially erasing these two message types early.
    }
}



// Parses one "message" chunk's "tool_calls" array (if present) into out,
// appending to whatever's already there - shared by send()'s streaming and
// non-streaming paths below, which used to each hand-roll this with
// tc["function"].value("name", "") / tc["function"]["arguments"]: plain
// operator[] on a possibly-missing key, which nlohmann::json auto-vivifies
// to null rather than failing loudly, and calling .value() on that null
// throws json::type_error. That throw used to propagate into either the
// streaming callback's blanket 'catch (...) {}' (silently dropping that
// tool call, and every later one in the same chunk, with zero trace) or
// past the non-streaming path's narrower 'catch (const json::parse_error&)'
// (which doesn't even match json::type_error) into chat_thread's own
// outermost catch-all - same silent loss, different route. Confirmed
// exploitable: is_ready_for_tools (tools.cpp) only dispatches when
// last_received.tool_calls is non-empty, so a swallowed parse failure here
// was indistinguishable from "the model chose not to call anything."
// .at()/.value() throughout instead - never auto-vivifies - and a per-entry
// try/catch with an actual log line, so one malformed entry can't take out
// the rest of the array, and a real failure is visible instead of silent.
static void parse_tool_calls(const json& msg_chunk, std::vector<ToolCall>& out)
{
    if (!msg_chunk.contains("tool_calls")) return;

    for (const auto& tc : msg_chunk["tool_calls"]) {
        try {
            json function = tc.at("function");
            out.push_back({
                tc.value("id", ""),
                function.value("name", ""),
                function.value("arguments", json::object())
            });
        } catch (const std::exception& e) {
            std::cerr << "[Error] Skipping malformed tool_calls entry from Ollama (" << e.what() << "): "
                      << tc.dump() << std::endl;
        }
    }
}

// One-line human-readable summary of a pure tool-call turn (no
// accompanying text) for DEBUG_LOG_CLASS::log_message()/history_write()'s debug
// text, e.g. "[tool_calls: set_hue_light({\"light_id\":\"2\",\"on\":false})]" -
// used wherever there'd otherwise just be an empty content string.
static std::string summarize_tool_calls(const std::vector<ToolCall>& calls)
{
    std::stringstream ss;
    ss << "[tool_calls: ";
    for (size_t i = 0; i < calls.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << calls[i].name << "(" << calls[i].arguments.dump() << ")";
    }
    ss << "]";
    return ss.str();
}

void ollama_system::send(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms, const std::string& role) {
    std::string new_user_input = filter_non_printable(comms.INPUT_FROM_USER);
    
    // 1. Set initial states
    status.is_active = true;
    last_received.complete = false; // Reset completion flag
    last_received.response = "";
    last_received.thinking = "";
    last_received.tool_calls.clear();

    // A real user message (or a background task-runner's next scripted
    // command - also sent with role "user") starts a fresh turn - see
    // tool_calls_this_turn's comment in olla.h. A "system"-role send()
    // (DIRECTOR_NOTE follow-ups) deliberately does NOT reset this; those
    // are still part of the same turn the guard is bounding.
    if (role == "user")
    {
        tool_calls_this_turn = 0;
    }

    {
        std::lock_guard<std::mutex> lock(history_mutex);
        Message input_msg;
        input_msg.role = role;
        input_msg.content = new_user_input;
        history.push_back(input_msg);
    }
    DEBUG_LOG_CLASS::instance().log_message(debug_label, role, new_user_input);

    json messages_json = json::array();
    {
        std::lock_guard<std::mutex> lock(history_mutex);
        for (const auto& msg : history) {
            json m = {{"role", msg.role}, {"content", msg.content}};
            if (!msg.tool_call_id.empty()) m["tool_call_id"] = msg.tool_call_id;
            // Echoed back in the same shape parse_tool_calls() (below)
            // parses it out of Ollama's own responses in - see
            // Message::tool_calls' comment (olla.h) for why this exists:
            // without it, the model's own past tool invocations were
            // invisible in its own context, only ever their results were.
            if (!msg.tool_calls.empty()) {
                json tc_array = json::array();
                for (const auto& tc : msg.tool_calls) {
                    tc_array.push_back({
                        {"id", tc.id},
                        {"type", "function"},
                        {"function", {
                            {"name", tc.name},
                            {"arguments", tc.arguments}
                        }}
                    });
                }
                m["tool_calls"] = tc_array;
            }
            messages_json.push_back(m);
        }
    }

    // Rebuilt fresh every call (see the comment in open()) rather than
    // fixed once at startup, so a tool that joined tools_list after open()
    // - a remote tool connecting mid-session - shows up on the very next
    // request instead of never.
    tools = json::array();
    {
        // tools_list_mutex (olla.h) - this loop runs on whatever thread is
        // driving this instance's own chat_thread, concurrently with
        // process()'s erase of a dead TOOL_REMOTE (below) on the main
        // thread. See tools_list_mutex's own comment for the crash this
        // closes.
        std::lock_guard<std::mutex> lock(tools_list_mutex);
        for (auto& tool : tools_list)
            tool->register_tool(*this, tools);
    }

    // Ollama's own streaming API is what makes either channel show up live
    // at all - needed whenever EITHER comms.INPUT_FROM_LLM (stream_output)
    // or comms.INPUT_FROM_THINKING (stream_thinking) wants that, not just
    // stream_output alone (see their shared comment, olla.h).
    bool use_streaming_request = PROPS.stream_output || PROPS.stream_thinking;

    json body = {
        {"model", PROPS.model},
        {"messages", messages_json},
        {"stream", use_streaming_request},
        {"think", PROPS.use_thinking},
        {"keep_alive", PROPS.keep_alive_seconds},
        {"options", {
            {"num_ctx", PROPS.num_ctx},
            {"repeat_penalty", PROPS.repeat_penalty},
            //{"temperature", 0}
        }}
    };

    if (!tools.empty()) {
        body["tools"] = tools;
    }

    httplib::Client cli(PROPS.host, PROPS.port);
    cli.set_read_timeout(120);

    std::string accumulated_content = "";
    std::string accumulated_thinking = "";
    bool stream_received_done_flag = false;

    httplib::Headers headers = { {"Content-Type", "application/json"} };
    std::string json_body = body.dump();

    if (use_streaming_request) {
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

                        // Independent ifs, not if/else if - a chunk that
                        // happens to carry both a "thinking" key (even an
                        // empty one) and real "content" used to have its
                        // content silently dropped, since checking key
                        // *presence* (not whether thinking is non-empty)
                        // made the content branch unreachable whenever
                        // "thinking" was present at all. The non-streaming
                        // path below never had this restriction - it reads
                        // both fields independently via .value() - so this
                        // was an inconsistency between the two, not a
                        // deliberate choice.
                        if (msg_chunk.contains("thinking")) {
                            std::string t = msg_chunk["thinking"];
                            accumulated_thinking += t;
                            // accumulated_thinking (-> last_received.thinking)
                            // always gets it regardless - only whether it
                            // also goes to comms live is gated.
                            if (PROPS.stream_thinking) {
                                std::lock_guard<std::mutex> lock(output_buffer_mutex);
                                comms.INPUT_FROM_THINKING += t;
                            }
                        }
                        if (msg_chunk.contains("content")) {
                            std::string c = msg_chunk["content"];
                            accumulated_content += c;
                            // TTS reads from its own comms_buffer_audio copy
                            // (io_worker.cpp), fed from comms.INPUT_FROM_LLM
                            // by IO_WORKER_CLASS::exchange() - no separate
                            // append needed here. accumulated_content (->
                            // last_received.response) always gets it
                            // regardless - only whether it also goes to
                            // comms live is gated.
                            if (PROPS.stream_output) {
                                std::lock_guard<std::mutex> lock(output_buffer_mutex);
                                comms.INPUT_FROM_LLM += c;
                            }
                        }

                        parse_tool_calls(msg_chunk, last_received.tool_calls);
                    }
                } catch (...) {}
                return true;
            }
        );

        if (status.interrupt_signal.load()) {
            //log("\n[System: Response Interrupted by User]\n");
        } else if (!res || res->status != 200) {
            std::cerr << "\n[Error] Stream failed: " << (res ? std::to_string(res->status) : "Connection error") << std::endl;
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

                    parse_tool_calls(msg_obj, last_received.tool_calls);
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
    
    // No longer requires tool_calls to be empty - a response that includes
    // both explanatory text AND a tool call used to have that text silently
    // discarded here: it was already live in response_buffer (so the user
    // saw/heard it), but never recorded in history, meaning the model had
    // no memory of having said it on the very next turn.
    if (!last_received.response.empty() || !last_received.tool_calls.empty()) {
        std::lock_guard<std::mutex> lock(history_mutex);
        std::string final_content = last_received.response;
        if (status.interrupt_signal.load()) {
            final_content += "... [Interrupted]";
        }

        Message assistant_msg;
        assistant_msg.role = "assistant";
        assistant_msg.content = final_content;
        assistant_msg.tool_calls = last_received.tool_calls;
        history.push_back(assistant_msg);

        DEBUG_LOG_CLASS::instance().log_message(debug_label, "assistant", final_content.empty() ? summarize_tool_calls(last_received.tool_calls) : final_content);
    }

    // Same trailing blank line the old direct-cout version always printed
    // once a response finished - routed through response_buffer instead of
    // printing straight to std::cout, so display() (see user_io.h/.cpp)
    // stays the only thing that actually writes chat output to the screen.
    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex);
        comms.INPUT_FROM_LLM += "\n";
    }
    status.is_active = false;
}

/**
 * REFINED: SEND_TOOL_RESULT (For your main ollama_system class)
 * This version updates the history but allows the Integration Task
 * to handle the actual conversational output.
 *
 * The 'tool'-role message this pushes used to get deleted from history
 * (along with integrate_tool_result()'s DIRECTOR_NOTE) right before the
 * model's very next turn, via a since-removed prune_turn_scaffolding() -
 * see where that function used to be defined, further down this file, for
 * why: confirmed against real session logs, erasing this evidence one turn
 * later is what let the model drift into confidently narrating fake tool
 * successes once enough tool-trace-free turns had piled up in history. It
 * now stays, aged out by consolidate() (sidetrack.cpp) like everything
 * else, not specially deleted early.
 */
void ollama_system::send_tool_result(const std::string& tool_call_id, const std::string& result) {
    Message msg;
    msg.role = "tool";
    msg.content = result;
    msg.tool_call_id = tool_call_id;

    {
        std::lock_guard<std::mutex> lock(history_mutex);
        history.push_back(msg);
    }
    DEBUG_LOG_CLASS::instance().log_message(debug_label, msg.role, msg.content);
}

// prune_turn_scaffolding() used to live here - deleted every 'tool' message
// and every non-protected 'system' message (raw tool results and
// DIRECTOR_NOTE prompts) right before the next real user turn. Removed:
// that erased all evidence a tool was ever called before the model's very
// next turn, which - confirmed against real session logs - is what let the
// model drift into narrating fake tool successes once enough clean-looking
// "user asks, assistant confidently answers" turns had accumulated with no
// trace of the tool machinery behind them. 'tool'/'system' messages now
// just ride along as normal consolidation_level 0 history like everything
// else, aged out by consolidate() (sidetrack.cpp) once a level actually
// fills up, instead of being specially deleted after one turn regardless.


void ollama_system::stop()
{
    status.interrupt_signal = true;
}

void ollama_system::request_exit()
{
    running = false;
    if (is_processing) stop();
    if (chat_thread.joinable()) chat_thread.join();
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
 * @brief Saves a vector of Messages to a JSON file.
 * * @param filename The destination file path.
 * @param history The vector containing message data.
 * @return true if successful, false otherwise.
 */
bool ollama_system::saveHistoryToJson(std::filesystem::path filepath) {
    try {
        // Create a json object from the vector
        // nlohmann::json handles std::vector automatically if to_json is defined for the element
        json j;
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            j = history;
        }

        std::ofstream file(filepath);
        if (!file.is_open()) {
            return false;
        }

        // Write to file with 4-space indentation for readability
        file << j.dump(4);
        file.close();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error saving history: " << e.what() << std::endl;
        return false;
    }
}

/**
 * @brief Loads a vector of Messages from a JSON file.
 * * @param filename The source file path.
 * @param history A reference to the vector to be populated.
 * @return true if successful, false otherwise.
 */
bool ollama_system::loadHistoryFromJson(std::filesystem::path filepath) {
    try {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return false;
        }

        json j;
        file >> j;

        // Convert the JSON array back into the vector of Messages
        history = j.get<std::vector<Message>>();
        
        file.close();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "Error loading history: " << e.what() << std::endl;
        return false;
    }
}

/**
 * Writes all history to a file in the specified directory.
 * Overwrites the file if it already exists.
 */
void ollama_system::history_write(std::string Directory) 
{
    // First, we save the history to a JSON file for structured access and debugging.
    saveHistoryToJson(PROPS.OLLI_DIRECTORY / "history.json");

    // Next, we create a human-readable text file for quick inspection.
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

        {
            std::lock_guard<std::mutex> lock(history_mutex);
            for (size_t i = 0; i < history.size(); ++i) {
                const auto& msg = history[i];
                outFile << "Index: " << i << std::endl;
                outFile << "Level: " << msg.consolidation_level << std::endl;
                outFile << "Role:  " << msg.role << std::endl;
                outFile << "Content: " << msg.content << std::endl;
                if (!msg.tool_call_id.empty()) {
                    outFile << "Tool ID: " << msg.tool_call_id << std::endl;
                }
                if (!msg.tool_calls.empty()) {
                    outFile << "Tool Calls: " << summarize_tool_calls(msg.tool_calls) << std::endl;
                }
                outFile << "------------------------------------" << std::endl;
            }
        }

        outFile.close();
    } catch (const std::exception& e) {
        std::cerr << "[Error] history_write failed: " << e.what() << std::endl;
    }
}

void ollama_system::save_history()
{
    if (PROPS.LOAD_SAVE_HISTORY_ON_DISK)
    {
        history_write(PROPS.path_history);
        PREVIOUS_HISTORY_SIZE = history.size();
    }
}

void ollama_system::clear_history()
{
    std::lock_guard<std::mutex> lock(history_mutex);
    history.clear();
}

void ollama_system::clear_history_keep_protected()
{
    std::lock_guard<std::mutex> lock(history_mutex);
    std::vector<Message> protected_messages;
    for (const Message& msg : history) {
        if (msg.consolidation_level < 0) {
            protected_messages.push_back(msg);
        }
    }
    history = protected_messages;
}

void ollama_system::replace_history(std::vector<Message> new_history)
{
    std::lock_guard<std::mutex> lock(history_mutex);
    history = std::move(new_history);
}

void ollama_system::unload_model()
{
    // /api/generate with no "prompt" and keep_alive: 0 is Ollama's
    // documented way to unload a model on demand, independent of whatever
    // keep_alive value normal requests use (see PROPS.keep_alive_seconds).
    json body = {
        {"model", PROPS.model},
        {"keep_alive", 0}
    };

    httplib::Headers headers = { {"Content-Type", "application/json"} };
    httplib::Client cli(PROPS.host, PROPS.port);
    cli.set_read_timeout(10); // just an unload signal - no generation happens, should return almost instantly
    cli.Post("/api/generate", headers, body.dump(), "application/json");
}

// ----


// write_to_tts() used to chunk comms.tts_buffer (punctuation/length/
// generation-finished heuristic) and hand it to comms.audio->speak() -
// both are gone now. That job moved to IO_WORKER_CLASS::thread_main()
// (io_worker.cpp), which chunks comms_buffer_audio.INPUT_FROM_LLM instead
// (fed from comms.INPUT_FROM_LLM by exchange()), gated on tts->isSpeaking()
// rather than a punctuation/length heuristic.

bool ollama_system::jump_input(COMMS& comms)
{
    // key_input now lives on IO_WORKER_CLASS, not CLASS_SYSTEM - the
    // submitted line arrives here via comms instead (see input(), which
    // calls this). comms is already this instance's own member, no
    // parameter needed for it.
    //
    // The "I'm home."/"I'm leaving."/etc phrase-triggered scene-loading
    // jump_instance block that used to live here was removed - never
    // actually used in practice (exact-phrase matching that real
    // conversation/voice input never naturally produced), along with the
    // TOOL_PERMISSIONS system it depended on. See git history if this
    // pattern (a throwaway ollama_system instance short-circuiting straight
    // to a scripted action, bypassing the LLM) is ever wanted again.
    if (trim(comms.INPUT_FROM_USER) == "bye" || trim(comms.INPUT_FROM_USER) == "quit" || trim(comms.INPUT_FROM_USER) == "Goodbye.")
    {
        request_exit();
        return true;
    }

    return false;
}

/**
 * Updates the input method to return true when a chat response is complete.
 * This version preserves the original non-blocking logic and thread safety.
 */
bool ollama_system::input(COMMS& comms, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list)
{
    // 1. INTERRUPT - key_input.INTERRUPTED now lives on IO_WORKER_CLASS;
    // comms.INTERRUPTED is how it reaches here (relayed by
    // IO_WORKER_CLASS::exchange() - see io_worker.cpp). Cleared here,
    // right where it's actually consumed - the "host clears the hosted
    // side comms when needed" half of that design.
    if (is_processing && comms.INTERRUPTED)
    {
        log(".");
        comms.INTERRUPTED = false;

        stop();
        if (chat_thread.joinable()) chat_thread.join();
        is_processing = false;
        log("\n[Interrupting for new input...]\n");
    }

    // 2. INPUT - comms.ENTER_PRESSED/INPUT_FROM_USER replace
    // key_input.ENTER_PRESSED/LINE.
    if (comms.ENTER_PRESSED)
    {
        // A real Enter keypress sets INTERRUPTED alongside ENTER_PRESSED
        // (see KEYBOARD_INPUT::keyboard_input(), user_io.cpp) - staged and
        // relayed together as comms.INTERRUPTED/comms.ENTER_PRESSED from
        // the same event (io_worker.cpp). If branch 1 above didn't consume
        // INTERRUPTED (is_processing was false - the normal case for a
        // fresh submission), it would otherwise dangle true and fire
        // branch 1 spuriously on some LATER tick once is_processing does
        // become true, right as this very submission starts streaming -
        // aborting it and logging a bogus "[Interrupting for new
        // input...]"/"[System: Response Interrupted by User]" the user
        // never asked for. The original code avoided this for free since
        // KEYBOARD_INPUT::reset() cleared LINE/ENTER_PRESSED/INTERRUPTED
        // together, unconditionally, right here - this replicates that.
        comms.INTERRUPTED = false;

        if (jump_input(comms))
        {
            comms.ENTER_PRESSED = false;
            comms.INPUT_FROM_USER.clear();
            return false;
        }
        else
        {
            status.interrupt_signal = false;
            is_processing = true;

            std::string tmp_line = comms.INPUT_FROM_USER;
            comms.ENTER_PRESSED = false;
            comms.INPUT_FROM_USER.clear();

            // No output.user_input echo needed here - IO_WORKER_CLASS
            // already did it (io_worker.cpp thread_main(), step 8) at the
            // moment it captured the line, same-thread as output itself.
            // output isn't reachable from ollama_system anymore anyway.

            // Ensure we don't leak a thread if one was somehow left joinable
            if (chat_thread.joinable()) chat_thread.join();

            // Launch the background thread exactly as before. tools_list
            // captured by reference is safe here - it's the caller's own
            // long-lived vector (main.cpp's real one for the main chat),
            // which outlives this thread by construction. comms is also
            // captured by reference - send() now reads its content from
            // comms.INPUT_FROM_USER rather than taking it as its own
            // parameter, so it has to be restored there first; locked
            // since exchange() (io_worker.cpp) can write that same field
            // from the main thread at the same time.
            chat_thread = std::thread([this, tmp_line, &tools_list, &comms]()
            {
                try {
                    {
                        std::lock_guard<std::mutex> lock(output_buffer_mutex);
                        comms.INPUT_FROM_USER = tmp_line;
                    }
                    send(tools_list, comms, "user");
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

void ollama_system::process(IO_WORKER_CLASS& io_worker, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, COMMS& comms)
{
    // ---------------------------------------------------------
    // PART 0: WRITE HISTORY TO FILE
    // ---------------------------------------------------------
    {
        update_status();

        if (PROPS.LOAD_SAVE_HISTORY_ON_DISK)
        {
            // status.total_messages was just set from history.size() under
            // history_mutex in update_status() above - reusing it here
            // avoids reading history.size() again without the lock.
            size_t current_history_size = static_cast<size_t>(status.total_messages);
            if (current_history_size != PREVIOUS_HISTORY_SIZE)
            {
                history_write(PROPS.path_history);
                PREVIOUS_HISTORY_SIZE = current_history_size;
            }
        }
    }

    // ---------------------------------------------------------
    // PART 1: PROCESS MAIN CHAT TOOLS
    // ---------------------------------------------------------
    handle_instance_tools(io_worker, system, tools_list, comms);

    // ---------------------------------------------------------
    // PART 2: MANAGE BACKGROUND TASKS
    // Iterate through background instances, handle their logic, 
    // and remove them if they are finished.
    // ---------------------------------------------------------
    for (auto it = background_tasks.begin(); it != background_tasks.end(); ) {
        ollama_system& task_instance = *(it->first); // Access the object inside unique_ptr
        COMMS& task_comms = *(it->second); // This task's own real, persistent COMMS

        // A. Handle any tool calls requested by the background task.
        // NOTE: this dispatches on the task instance, not on the main
        // instance (*this) — otherwise a background task's tool calls would
        // never be serviced and the main instance would be re-processed
        // once per background task instead.
        //
        // Passes *this* process() call's own tools_list, not some private
        // one of task_instance's - a background task (currently only ever
        // TOOL_TASK_RUNNER's automation instances, tools.cpp) already gets
        // fully drained by its own synchronous send()/process() loop before
        // handle_tool() returns (see tools.cpp), so by the time this reaches
        // here there's nothing left pending to actually dispatch - the very
        // next completion check below erases it. Reusing the parent's own
        // tools_list keeps this call always-valid without needing to keep a
        // second tools_list alive across ticks for an instance that's
        // already done.
        task_instance.handle_instance_tools(io_worker, system, tools_list, task_comms);

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
                comms.INPUT_FROM_USER = task_report;
                send(tools_list, comms, "system");
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
        log("\n[Response complete. Awaiting user input...]\n");
        update_status();
        //std::cout << "You: " << std::flush;
    }
    
    // ---------------------------------------------------------
    // PART 5: ACTIVE MONITORS
    // Continuous checks for time-based triggers or hardware state.
    // ---------------------------------------------------------
    for (auto& tool : tools_list)
        tool->monitor_tool(*this, system, tools_list, comms);

    // Drop any tool that's no longer alive (currently only ever a
    // TOOL_REMOTE whose connection closed - see is_alive()'s comment in
    // tools.h) so it stops showing up in the tools array sent to Ollama.
    // monitor_tool() above is what actually notices a closed connection and
    // flips is_alive() to false; this is just where that gets acted on.
    //
    // tools_list_mutex (olla.h) guards just this erase, not the
    // monitor_tool() loop above - monitor_tool() can recurse back into
    // send() on this same thread (a pushed event -> integrate_tool_result()
    // -> send()), which takes the same lock itself, and both only ever run
    // on the main thread anyway so they were never racing each other.
    {
        std::lock_guard<std::mutex> lock(tools_list_mutex);
        size_t before = tools_list.size();
        tools_list.erase(
            std::remove_if(tools_list.begin(), tools_list.end(),
                [](const std::unique_ptr<TOOL_BASE>& tool) { return !tool->is_alive(); }),
            tools_list.end());
        size_t removed = before - tools_list.size();
        if (removed > 0) {
            log("[RemoteTools] Removed " + std::to_string(removed) + " disconnected tool(s)\n");
        }
    }

    // PART 6 used to be TTS output (write_to_tts()) - that job now runs
    // inside IO_WORKER_CLASS::thread_main() instead (io_worker.cpp), fed by
    // exchange() rather than called from here - see write_to_tts()'s own
    // former-site comment above for details.
}


#endif