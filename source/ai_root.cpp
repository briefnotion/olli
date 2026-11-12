#ifndef ai_root_cpp
#define ai_root_cpp

#include "ai_root.h"
#include "io_worker.h"
#include "user_io.h"
#include "tool_worker.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

// One instance of every TOOL_* class - callers populate their own tools_list
// with this (see process()'s comment in ai_root.h for why tools_list is a
// reference parameter rather than living on this class). Add a new tool
// here, nowhere else.
void populate_default_tools(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list)
{
    tools_list.push_back(std::make_unique<TOOL_SET_THINKING_MODE>());
    tools_list.push_back(std::make_unique<TOOL_WEB_SEARCH>());
    tools_list.push_back(std::make_unique<TOOL_REMEMBER>());
    tools_list.push_back(std::make_unique<TOOL_QUICK_NOTE>());
    tools_list.push_back(std::make_unique<TOOL_TASK_RUNNER>());
    tools_list.push_back(std::make_unique<TOOL_DELEGATOR>());
}

ai_root::ai_root()
{
}

void ai_root::log(const std::string& text)
{
    // Stubbed out - comms.log() no longer exists (comms itself stopped
    // being a member of ai_root as part of today's COMMS-ownership
    // move) and every chat.log(...) call site still expects this old
    // no-argument signature. Revisit properly when working on system
    // messages - needs a COMMS& parameter threaded through here and every
    // caller (tools.cpp, remote_tools.cpp, main.cpp, ai_root.cpp), same
    // pattern as integrate_tool_result(). See TODO.md.
    (void)text;
}

void ai_root::pull_background_output(OUTPUT_CLASS& output)
{
    for (auto& task_pair : background_tasks)
    {
        output.get_response(*task_pair.second);
    }
}

std::pair<ai_root&, COMMS&> ai_root::spawn_background_task()
{
    // COMMS::audio (a direct pointer to the speaker) doesn't exist anymore -
    // TTS output now runs through IO_WORKER_CLASS's own comms_stt_tts,
    // which only the main chat's own COMMS ever reaches (via exchange()
    // into comms_buffer, then thread_main()'s own copy into comms_stt_tts -
    // io_worker.cpp). Background tasks currently have no speech output at
    // all as a result - unaddressed, see this session's notes.
    background_tasks.emplace_back(make_sibling(), std::make_unique<COMMS>());
    auto& [instance, instance_comms] = background_tasks.back();
    return {*instance, *instance_comms};
}

void ai_root::open(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list)
{
    log("[System] Connecting to " + PROPS.host + ":" + std::to_string(PROPS.port) + " (" + PROPS.model + ")\n");

    std::filesystem::create_directories(PROPS.OLLI_DIRECTORY / "output");
    std::filesystem::create_directories(PROPS.OLLI_DIRECTORY / "input");

    PROPS.path_output = PROPS.OLLI_DIRECTORY / "output";
    PROPS.path_history = ".";

    for (auto& tool : tools_list)
        tool->configure(*this);

    // register_tool() itself is called from send() (below), not here - a
    // remote tool joins tool_worker's own registry independently of open()
    // entirely (tool_worker.cpp), so the tools array sent to Ollama needs
    // rebuilding fresh on every request rather than fixed once at startup.

    if (PROPS.LOAD_SAVE_HISTORY_ON_DISK)
    {
        loadHistoryFromJson(PROPS.OLLI_DIRECTORY / "history.json");
    }

    // Ensure a protected (consolidation_level -1) foundational message is
    // always present: either this is a brand new history, or it was loaded
    // from disk but predates protected messages (and may have already had
    // its original opening prompt consolidated away). Either way, the model
    // needs its persona/instructions to always be present, unsummarized.
    // Exactly -1, not just "< 0": other negative levels (-2, -3...) are
    // separate reserved slots (set_protected_slot()) and must not hide a
    // missing persona.
    bool has_protected_message = false;
    for (const Message& msg : history) {
        if (msg.consolidation_level == -1) {
            has_protected_message = true;
            break;
        }
    }

    if (!has_protected_message)
    {
        Message opening_msg;
        opening_msg.role = "system";
        opening_msg.content = AI_OPENING;
        opening_msg.consolidation_level = -1;
        history.insert(history.begin(), opening_msg);
    }
}

void ai_root::open(std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, AI_PROPERTIES Properties)
{
    PROPS = Properties;

    PROPS.LOAD_SAVE_HISTORY_ON_DISK = false;

    open(tools_list);
}


string ai_root::gather_history()
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
void ai_root::integrate_tool_result(TOOL_WORKER_CLASS* tool_worker, COMMS& comms, std::string Special_Instruction, const std::string& raw_result)
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
        comms.INPUT_FROM_USER.set(prompt);
        this->send(tool_worker, comms, "system");

        // 3. This DIRECTOR_NOTE, and the raw tool result send_tool_result()
        // pushed just before it, both stay in history now rather than being
        // deleted after one turn (see send_tool_result()'s comment for why)
        // - unbounded growth is handled the same way the rest of history
        // is, by consolidate() (sidetrack.cpp) once a level actually fills
        // up, not by specially erasing these two message types early.
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


// Boils a list of token probabilities down to the small summary stored on
// Message (ai_root.h's Confidence). Empty input stays present == false, which is
// how "no data" is told apart from "0% sure".
static Confidence summarize_confidence(const std::vector<TokenProb>& tokens)
{
    Confidence c;
    if (tokens.empty()) return c;

    double sum = 0.0;
    double lowest = 1.0;
    for (const TokenProb& t : tokens) {
        sum += t.probability;
        if (t.probability < lowest) lowest = t.probability;
        if (t.probability < 0.5) ++c.below_half;
    }

    c.present = true;
    c.tokens = static_cast<int>(tokens.size());
    c.mean = sum / static_cast<double>(tokens.size());
    c.lowest = lowest;
    return c;
}

// Asks for a reply shaped like {"answer":"yes"}, "no" or "maybe" - send()
// turns it into last_received.answer (ai_root.h's YesNoMaybe).
const json YES_NO_MAYBE_FORMAT = {
    {"type", "object"},
    {"properties", {
        {"answer", {{"type", "string"}, {"enum", json::array({"yes", "no", "maybe"})}}}
    }},
    {"required", json::array({"answer"})}
};

void ai_root::send(TOOL_WORKER_CLASS* tool_worker, COMMS& comms, const std::string& role, const json& response_format) {
    std::string new_user_input = filter_non_printable(comms.INPUT_FROM_USER.peek());

    // 1. Set initial states
    status.is_active = true;
    last_received.complete = false; // Reset completion flag
    last_received.response = "";
    last_received.thinking = "";
    last_received.tool_calls.clear();
    last_received.response_tokens.clear();
    last_received.thinking_tokens.clear();
    last_received.confidence = Confidence();
    last_received.answer = YesNoMaybe();

    // A real user message (or a background task-runner's next scripted
    // command - also sent with role "user") starts a fresh turn - see
    // tool_calls_this_turn's comment in ai_root.h. A "system"-role send()
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

    // Everything between here and the shared tail below is the backend's:
    // build its request from history + tool defs, make the call, and report
    // the result - see backend_generate()'s contract in ai_root.h.
    std::string accumulated_content = "";
    std::string accumulated_thinking = "";
    backend_generate(tool_worker, comms, response_format, accumulated_content, accumulated_thinking);

    last_received.response = accumulated_content;
    last_received.thinking = accumulated_thinking;

    // Whatever the model thought before answering, as its own record just
    // ahead of the reply's (second guess is the one that runs with thinking
    // on). Logged even when the reply that follows is empty: a pass that
    // thought and then said nothing is exactly the kind worth reading.
    if (!accumulated_thinking.empty()) {
        DEBUG_LOG_CLASS::instance().log_message(debug_label, "thinking", accumulated_thinking);
    }

    // A reply with no text of its own (a pure tool call) has nothing to
    // summarize - stays present == false rather than scoring tool-call tokens.
    // Same for a structured-output reply (response_format given): the grammar
    // forces its opening tokens, which score ~0 whatever the model thought, so
    // a summary over them would be misleading. response_tokens still holds
    // every token for anything that wants to look at the answer word itself.
    if (!accumulated_content.empty() && response_format.empty()) {
        last_received.confidence = summarize_confidence(last_received.response_tokens);
    }

    // Only for a call that asked for YES_NO_MAYBE_FORMAT. Anything we can't
    // read stays at 0 (maybe) - and is noted in the debug log, so a broken
    // reply shows up there instead of quietly becoming a "maybe".
    if (response_format == YES_NO_MAYBE_FORMAT) {
        try {
            std::string word = json::parse(accumulated_content).at("answer").get<std::string>();
            if (word == "yes") last_received.answer.value = 1;
            else if (word == "no") last_received.answer.value = -1;
        } catch (const json::exception& e) {
            DEBUG_LOG_CLASS::instance().log_event(debug_label, std::string("yes/no/maybe reply wasn't readable, treating as maybe: ") + e.what());
        }
    }

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
        assistant_msg.confidence = last_received.confidence;
        history.push_back(assistant_msg);

        DEBUG_LOG_CLASS::instance().log_message(debug_label, "assistant", final_content.empty() ? summarize_tool_calls(last_received.tool_calls) : final_content);

        if (last_received.confidence.present) {
            const Confidence& c = last_received.confidence;
            // Which word was the least certain one - makes "lowest" readable.
            std::string lowest_token;
            for (const TokenProb& t : last_received.response_tokens) {
                if (t.probability <= c.lowest) { lowest_token = t.token; break; }
            }
            char line[256];
            std::snprintf(line, sizeof(line), "confidence: mean %.2f, lowest %.4f (\"%s\"), %d of %d tokens under 50%%",
                          c.mean, c.lowest, lowest_token.c_str(), c.below_half, c.tokens);
            DEBUG_LOG_CLASS::instance().log_event(debug_label, line);
        }
    }

    // Same trailing blank line the old direct-cout version always printed
    // once a response finished - routed through response_buffer instead of
    // printing straight to std::cout, so display() (see user_io.h/.cpp)
    // stays the only thing that actually writes chat output to the screen.
    {
        std::lock_guard<std::mutex> lock(output_buffer_mutex);
        comms.INPUT_FROM_LLM.add_to("\n");
    }
    status.is_active = false;
}

/**
 * REFINED: SEND_TOOL_RESULT (For your main ai_root class)
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
void ai_root::send_tool_result(const std::string& tool_call_id, const std::string& result) {
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


void ai_root::stop()
{
    status.interrupt_signal = true;
}

void ai_root::request_exit()
{
    running = false;
    if (is_processing) stop();
    if (chat_thread.joinable()) chat_thread.join();
}

void ai_root::shutdown_background_tasks()
{
    for (auto& task_pair : background_tasks)
    {
        task_pair.first->request_exit();
    }
}

/**
 * Updates the internal status struct by scanning the history.
 */
void ai_root::update_status() {
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
bool ai_root::saveHistoryToJson(std::filesystem::path filepath) {
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
bool ai_root::loadHistoryFromJson(std::filesystem::path filepath) {
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
void ai_root::history_write(std::string Directory) 
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

void ai_root::save_history()
{
    if (PROPS.LOAD_SAVE_HISTORY_ON_DISK)
    {
        history_write(PROPS.path_history);
        PREVIOUS_HISTORY_SIZE = history.size();
    }
}

void ai_root::clear_history()
{
    std::lock_guard<std::mutex> lock(history_mutex);
    history.clear();
}

void ai_root::clear_history_keep_protected()
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

void ai_root::replace_history(std::vector<Message> new_history)
{
    std::lock_guard<std::mutex> lock(history_mutex);
    history = std::move(new_history);
}

void ai_root::set_protected_slot(int level, Message message)
{
    message.consolidation_level = level;
    std::lock_guard<std::mutex> lock(history_mutex);

    size_t position = static_cast<size_t>(-level - 1); // -1 -> 0, -2 -> 1, -3 -> 2
    if (position < history.size() && history[position].consolidation_level == level)
    {
        history[position] = message;
    }
    else
    {
        if (position > history.size()) position = history.size();
        history.insert(history.begin() + static_cast<std::ptrdiff_t>(position), message);
    }
}


// ----


// write_to_tts() used to chunk comms.tts_buffer (punctuation/length/
// generation-finished heuristic) and hand it to comms.audio->speak() -
// both are gone now. That job moved to IO_WORKER_CLASS::display_with_tts()
// (io_worker.cpp), which chunks its own comms_stt_tts.INPUT_FROM_LLM
// instead (fed from comms.INPUT_FROM_LLM by exchange() into comms_buffer,
// then thread_main()'s own tick copying that into comms_stt_tts), gated on
// tts->isSpeaking() rather than a punctuation/length heuristic.

bool ai_root::jump_input(COMMS& comms)
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
    // pattern (a throwaway ai_root instance short-circuiting straight
    // to a scripted action, bypassing the LLM) is ever wanted again.
    if (trim(comms.INPUT_FROM_USER.peek()) == "bye" || trim(comms.INPUT_FROM_USER.peek()) == "quit" || trim(comms.INPUT_FROM_USER.peek()) == "Goodbye.")
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
bool ai_root::input(COMMS& comms, TOOL_WORKER_CLASS* tool_worker)
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

            std::string tmp_line = comms.INPUT_FROM_USER.drain();
            comms.ENTER_PRESSED = false;

            // No output.user_input echo needed here - IO_WORKER_CLASS
            // already did it (io_worker.cpp thread_main(), step 8) at the
            // moment it captured the line, same-thread as output itself.
            // output isn't reachable from ai_root anymore anyway.

            // Ensure we don't leak a thread if one was somehow left joinable
            if (chat_thread.joinable()) chat_thread.join();

            // Launch the background thread exactly as before. tool_worker
            // captured by value (a plain pointer, trivially copyable) is
            // safe here - it's the caller's own long-lived object (main.cpp's
            // real one for the main chat), which outlives this thread by
            // construction, same reasoning tools_list's own reference
            // capture used to have. comms is captured by reference - send()
            // now reads its content from comms.INPUT_FROM_USER rather than
            // taking it as its own parameter, so it has to be restored there
            // first; locked since exchange() (io_worker.cpp) can write that
            // same field from the main thread at the same time.
            chat_thread = std::thread([this, tmp_line, tool_worker, &comms]()
            {
                try {
                    {
                        std::lock_guard<std::mutex> lock(output_buffer_mutex);
                        comms.INPUT_FROM_USER.set(tmp_line);
                    }
                    send(tool_worker, comms, "user");
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

void ai_root::process(IO_WORKER_CLASS& io_worker, CLASS_SYSTEM* system, std::vector<std::unique_ptr<TOOL_BASE>>& tools_list, TOOL_WORKER_CLASS* tool_worker, COMMS& comms)
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
    handle_instance_tools(io_worker, system, tools_list, tool_worker, comms);

    // ---------------------------------------------------------
    // PART 2: MANAGE BACKGROUND TASKS
    // Iterate through background instances, handle their logic, 
    // and remove them if they are finished.
    // ---------------------------------------------------------
    for (auto it = background_tasks.begin(); it != background_tasks.end(); ) {
        ai_root& task_instance = *(it->first); // Access the object inside unique_ptr
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
        // already done. tool_worker stays nullptr for the same "nothing
        // left pending" reason.
        task_instance.handle_instance_tools(io_worker, system, tools_list, nullptr, task_comms);

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
                comms.INPUT_FROM_USER.set(task_report);
                send(tool_worker, comms, "system");
            }

            // task_instance is finished, but a call it dispatched can still
            // be outstanding (nothing forces a task/delegate script to wait
            // for every call it fired before ending) - about to destroy
            // the only thing that was ever going to claim that id's
            // result, so tell tool_worker to drop it instead of keeping it
            // forever with no one left to ask (see abandon_call()'s own
            // comment, tool_worker.h).
            if (tool_worker) {
                for (const std::string& call_id : task_instance.outstanding_tool_call_ids) {
                    tool_worker->abandon_call(call_id);
                }
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
    // PART 5: REMOTE TOOL COMMUNICATIONS
    // Used to loop tools_list calling monitor_tool() + erase dead
    // TOOL_REMOTE entries here (flagged, now retired) - that whole routine
    // only ran when process() happened to be called, the exact fragility
    // TOOL_WORKER_CLASS exists to fix. Ping/pong, dead-connection cleanup,
    // and the actual socket I/O all run continuously on tool_worker's own
    // background thread now (tool_worker.cpp) regardless of what this
    // instance is doing - this just drains whatever landed since last tick.
    // ---------------------------------------------------------
    if (tool_worker)
    {
        // Only ever claims results for calls THIS instance dispatched
        // (outstanding_tool_call_ids, ai_root.h) - tool_worker's queue is
        // shared by every ai_root instance (main chat, a
        // task-runner's own instance, a delegate's own instance), so
        // blindly popping "whatever's oldest" here would sometimes hand
        // this instance a different instance's own result. An id still
        // outstanding after this just means its result isn't ready yet;
        // it stays in the list for a future tick to claim.
        TOOL_RESULT result;
        for (auto it = outstanding_tool_call_ids.begin(); it != outstanding_tool_call_ids.end(); )
        {
            if (tool_worker->get_pending_result(*it, result))
            {
                send_tool_result(result.call_id, result.response);
                integrate_tool_result(tool_worker, comms, result.special_instruction, result.response);
                it = outstanding_tool_call_ids.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // Same handling the old monitor_tool()'s own event branch had
        // (remote_tools.cpp, untouched) - narration via
        // integrate_tool_result(), an optional structured follow-up queued
        // the same way a real model-issued call would be. A timestamp-based
        // id (not a static counter - this project avoids hidden/global
        // state) is enough since nothing needs to correlate a result back
        // to it, same reasoning as TOOL_EVENT's own comment (remote_tools.h).
        //
        // Still drained by whichever instance's process() tick happens to
        // run next, same as before - this doesn't redirect an event to its
        // rightful owner elsewhere (a bigger, still-open question, see
        // TODO.md). What origin_id (remote_tools.h) does let this instance
        // do is tell honestly whether the event is actually its own -
        // owned_tool_call_ids (ai_root.h) is this instance's own record of
        // every call it's ever dispatched - and frame the narration
        // accordingly instead of always implying it just answered whatever
        // the current conversation happens to be about.
        //
        // Gated on drains_events (ai_root.h's own comment on it) - unlike the
        // result-claiming loop just above, which is safe for any instance
        // to share (id-based), this one has no way to tell "mine" from
        // "someone else's" at all, so an instance that shouldn't compete
        // for ambient events (the removed in-olli subcon's subcon_llm did) opts out
        // here entirely rather than risk silently absorbing one meant for
        // the real chat.
        if (drains_events)
        {
            TOOL_EVENT event;
            while (tool_worker->get_pending_event(event))
            {
                tool_calls_this_turn = 0;

                // Generic var update (TOOL_EVENT's own comment, remote_tools.h) -
                // applied before narration/action below, so if anything ever
                // reads comms.user.presence while building this turn's own
                // prompt/persona, it sees the fresh value, not last tick's.
                // Presence (tools/presence/presence.cpp) is the one real
                // producer right now; a second var name would just be another
                // else-if here, no wire/struct/parsing changes needed.
                if (event.var_name == "user.presence")
                    comms.user.presence = event.var_value;

                if (!event.message.empty())
                {
                    log("[RemoteTools] Event from remote tool: " + event.message + "\n");

                    // origin_id (remote_tools.h) and owned_tool_call_ids
                    // (ai_root.h) are fully wired end-to-end and verified correct -
                    // DELIBERATELY not yet acted on here. An is_own-based
                    // "this wasn't triggered by anything you asked" framing
                    // reliably provoked the model into issuing its own follow-up
                    // tool call (instead of plain text) far more often than the
                    // default framing ever did, and that collided - live,
                    // reproduced 3 of 4 attempts - with the pre-authored
                    // on_expire_tool action from the SAME event, which dispatches
                    // through the separate pending_tool_calls queue
                    // (handle_instance_tools(), tools.cpp). integrate_tool_result()
                    // -> send() below is a fully synchronous, blocking call on
                    // whichever thread is running process() - ai_root.h's own
                    // comment on pending_tool_calls already documents the
                    // underlying gap ("last_received gets reset at the top of
                    // every send() call, so anything sitting in it could be
                    // silently dropped if a new turn started first" - a
                    // PRE-EXISTING race, not introduced here) - when it hung,
                    // the entire program stopped ticking for minutes, not just a
                    // narration quality issue. Root cause not confirmed (no
                    // ptrace access in the sandbox this was investigated in) -
                    // revisit once it can be debugged properly with a real
                    // debugger, then compute is_own again here and restore the
                    // framing distinction (see git history around 2026-09-22 for
                    // the removed version).
                    std::string framing = "";

                    integrate_tool_result(tool_worker, comms, framing, event.message);
                }

                if (!event.action_tool.empty())
                {
                    pending_tool_calls.push({
                        "system_action_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()),
                        event.action_tool,
                        event.action_arguments
                    });
                }
            }
        }
    }

    // PART 6 used to be TTS output (write_to_tts()) - that job now runs
    // inside IO_WORKER_CLASS::thread_main() instead (io_worker.cpp), fed by
    // exchange() rather than called from here - see write_to_tts()'s own
    // former-site comment above for details.
}


#endif