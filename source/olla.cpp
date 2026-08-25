#ifndef olla_cpp
#define olla_cpp

#include "olla.h"
#include "audio_control.h"
#include "user_io.h"
#include <algorithm>

// One instance of every TOOL_* class, in tools_list for the lifetime of this
// ollama_system - see the field comment in olla.h and the TOOL_BASE comment
// in tools.h for why. Add a new tool here, nowhere else in this class.
ollama_system::ollama_system()
{
    tools_list.push_back(std::make_unique<TOOL_SET_THINKING_MODE>());
    tools_list.push_back(std::make_unique<TOOL_WEB_SEARCH>());
    tools_list.push_back(std::make_unique<TOOL_TASK_RUNNER>());
}

void ollama_system::log(const std::string& text)
{
    std::lock_guard<std::mutex> lock(output_buffer_mutex);
    log_buffer += text;
}

void ollama_system::pull_background_output(OUTPUT_CLASS& output)
{
    for (auto& task : background_tasks)
    {
        output.get_response(*task);
    }
}

ollama_system& ollama_system::spawn_background_task()
{
    auto new_instance = std::make_unique<ollama_system>();
    ollama_system& ref = *new_instance;
    background_tasks.push_back(std::move(new_instance));
    return ref;
}

void ollama_system::register_remote_tool(std::unique_ptr<TOOL_BASE> tool)
{
    tools_list.push_back(std::move(tool));
}

void ollama_system::open()
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

void ollama_system::open(OLLAMA_SYSTEM_PROPERTIES Properties)
{
    PROPS = Properties;

    PROPS.LOAD_SAVE_HISTORY_ON_DISK = false;

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
        this->send(prompt, "system");

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
// accompanying text) for debug_log_message()/history_write()'s debug
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

void ollama_system::send(const std::string& user_input, const std::string& role) {
    std::string new_user_input = filter_non_printable(user_input);
    
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
    debug_log_message(role, new_user_input);

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
    for (auto& tool : tools_list)
        tool->register_tool(*this, tools);

    json body = {
        {"model", PROPS.model},
        {"messages", messages_json},
        {"stream", PROPS.stream_output},
        {"think", PROPS.use_thinking},
        {"keep_alive", PROPS.keep_alive_seconds},
        {"options", {
            {"num_ctx", PROPS.num_ctx},
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
                            {
                                std::lock_guard<std::mutex> lock(output_buffer_mutex);
                                thinking_buffer += t;
                            }
                        }
                        if (msg_chunk.contains("content")) {
                            std::string c = msg_chunk["content"];
                            accumulated_content += c;
                            tts_buffer += c;
                            {
                                std::lock_guard<std::mutex> lock(output_buffer_mutex);
                                response_buffer += c;
                            }
                        }

                        parse_tool_calls(msg_chunk, last_received.tool_calls);
                    }
                } catch (...) {}
                return true;
            }
        );

        if (status.interrupt_signal.load()) {
            log("\n[System: Response Interrupted by User]\n");
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

        debug_log_message("assistant", final_content.empty() ? summarize_tool_calls(last_received.tool_calls) : final_content);
    }

    // Same trailing blank line the old direct-cout version always printed
    // once a response finished - routed through response_buffer instead of
    // printing straight to std::cout, so display() (see user_io.h/.cpp)
    // stays the only thing that actually writes chat output to the screen.
    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex);
        response_buffer += "\n";
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
    debug_log_message(msg.role, msg.content);
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


void ollama_system::write_to_tts()
{
    if (!tts_buffer.empty())
    {
        if ((tts_buffer.find_first_of(".!?,:;") != std::string::npos || tts_buffer.length() > 60) ||
        status.is_active == false)
        {
            if (g_audio_control != nullptr)
            {
                g_audio_control->speak(tts_filter(tts_buffer));
            }
            tts_buffer.clear();
        }
    }
}

bool ollama_system::jump_input(CLASS_SYSTEM& System)
{
    if (trim(System.key_input.LINE) == "bye" || trim(System.key_input.LINE) == "quit" || trim(System.key_input.LINE) == "Goodbye.")
    {
        //System.key_input.reset();
        request_exit();
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
        log("[JUMP] [" + trim(System.key_input.LINE) + "] \n");

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
            jump_instance.process(&System, System.key_input.PROPS.ENABLED);
        }
        else if (trim(System.key_input.LINE) == "I'm leaving.")
        {
            System.audio_control.VOCA_manual_set(DEF_VOCA_SLEEP);
            jump_instance.TOOL_PERMISSIONS.HUE = true;
            jump_instance.open(PROPS);
            jump_instance.send("Load the labor scene.");
            jump_instance.process(&System, System.key_input.PROPS.ENABLED);
        }
        else if (trim(System.key_input.LINE) == "I'm sleeping." || 
                    trim(System.key_input.LINE) == "Lights off." )
        {
            System.audio_control.VOCA_manual_set(DEF_VOCA_SLEEP);
            jump_instance.TOOL_PERMISSIONS.HUE = true;
            jump_instance.open(PROPS);
            jump_instance.send("Load the slumber scene.");
            jump_instance.process(&System, System.key_input.PROPS.ENABLED);
        }

        integrate_tool_result("Describe what happened.", gather_history());
        //System.key_input.reset();
        
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
        log(".");
        System.key_input.reset();

        stop();
        if (chat_thread.joinable()) chat_thread.join();
        is_processing = false;
        log("\n[Interrupting for new input...]\n");
    }

    // 2. ORIGINAL INPUT LOGIC
    if (System.key_input.ENTER_PRESSED) 
    {
        if (jump_input(System)) 
        {
            System.key_input.reset();
            return false;
        }
        else 
        {
            status.interrupt_signal = false;
            is_processing = true;
            
            std::string tmp_line = System.key_input.LINE;
            System.key_input.reset();

            // Record what was actually submitted in the transcript - LINE
            // already ends in '\n' (see KEYBOARD_INPUT::keyboard_input()),
            // matching the voice path's own user_input append in main.cpp.
            // Without this, a typed message vanishes the instant Enter is
            // pressed (key_input.reset() above just cleared LINE) and never
            // appears anywhere in OUTPUT_CLASS - only voice input did.
            System.output.user_input += tmp_line;

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

void ollama_system::process(CLASS_SYSTEM* system, bool& Keyboard_Input_Enabled)
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
    handle_instance_tools(system, Keyboard_Input_Enabled);

    // ---------------------------------------------------------
    // PART 2: MANAGE BACKGROUND TASKS
    // Iterate through background instances, handle their logic, 
    // and remove them if they are finished.
    // ---------------------------------------------------------
    for (auto it = background_tasks.begin(); it != background_tasks.end(); ) {
        ollama_system& task_instance = **it; // Access the object inside unique_ptr

        // A. Handle any tool calls requested by the background task.
        // NOTE: this dispatches on the task instance, not on the main
        // instance (*this) — otherwise a background task's tool calls would
        // never be serviced and the main instance would be re-processed
        // once per background task instead.
        task_instance.handle_instance_tools(system, Keyboard_Input_Enabled);

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
        log("\n[Response complete. Awaiting user input...]\n");
        update_status();
        //std::cout << "You: " << std::flush;
    }
    
    // ---------------------------------------------------------
    // PART 5: ACTIVE MONITORS
    // Continuous checks for time-based triggers or hardware state.
    // ---------------------------------------------------------
    for (auto& tool : tools_list)
        tool->monitor_tool(*this, system);

    // Drop any tool that's no longer alive (currently only ever a
    // TOOL_REMOTE whose connection closed - see is_alive()'s comment in
    // tools.h) so it stops showing up in the tools array sent to Ollama.
    // monitor_tool() above is what actually notices a closed connection and
    // flips is_alive() to false; this is just where that gets acted on.
    {
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

    // ---------------------------------------------------------
    // PART 6: TTS OUTPUT
    // If there's new text in the TTS buffer, write it to the output file for the TTS engine to read.
    // ---------------------------------------------------------
    write_to_tts();        // Pushes new text to Speech engine
}


#endif