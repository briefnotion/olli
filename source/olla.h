#ifndef olla_h
#define olla_h

#include <string>
#include <vector>
#include <iostream>
#include <chrono>
#include <ctime>
#include <atomic>
#include <filesystem>
#include <queue>

#include "httplib.h"
#include <nlohmann/json.hpp>

#include "helper_olli.h"
#include "tools_helper.h"
#include "stringthings.h"

using json = nlohmann::json;

// --- FORWARD DECLARATION ---

class ollama_system;
struct Message {
    std::string role = "";    
    std::string content = "";
    std::string tool_call_id = ""; 
    int consolidation_level = 0;
};

struct ToolCall {
    std::string id;
    std::string name;
    json arguments;
};

struct ChatResult {
    std::string response;
    std::string thinking;
    std::vector<ToolCall> tool_calls;
    bool complete = false;
};

/**
 * Simple data struct to hold metrics.
 */
struct ollama_system_status {
    bool is_active = false;
    // The interrupt flag. Set this to true from another thread to stop 'send'
    std::atomic<bool> interrupt_signal{false};

    int total_messages = 0;
    std::map<int, int> level_counts;
    int max_level = 0;

    // Helper for easy printing
    std::string to_string() const {
        if (total_messages == 0) return "History Empty";
        std::stringstream ss;
        ss << "Total: " << total_messages << " | ";
        for (int i = 0; i <= max_level; ++i) {
            auto it = level_counts.find(i);
            int count = (it != level_counts.end()) ? it->second : 0;
            ss << "L" << i << ": " << count;
            if (i < max_level) ss << ", ";
        }
        return ss.str();
    }
};

void add_tool(json& tools, const std::string& name, const std::string& description, json parameters);

class TOOL_SET_THINKING_MODE
{
public:
    void register_tool(json& tools);
    void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
};

class TOOL_GET_CURRENT_TIME
{
public:
    void register_tool(json& tools);
    void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
};

class TOOL_TIMER {
public:
    std::map<std::string, TIMER_SIMPLE> active_timers;

    void register_tool(json& tools);
    void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
    
    // Updated signature:
    void monitor_tool(ollama_system& chat);
};

/**
 * TOOL_HUE
 * Interacts with the Ollama Chat System using the HUE_LIGHT_CLASS.
 */
class TOOL_HUE 
{
    private:
        HUE_LIGHT_CLASS hue;

    public:
        void set_credentials(const std::string& ip, const std::string& key, const std::string& path);

        // New method to break the conversational loop by reminding the model it must use a tool
        void refresh_system_prompt(ollama_system& chat);
        void register_tool(json& tools);
        void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
        void monitor_tool();
};

class TOOL_WEB_SEARCH
{
    private:        
        
        std::string strip_html_tags(std::string html);
        std::string make_clickable(const std::string& url, const std::string& text);

        std::string perform_actual_search(const std::string& query);
        std::string fetch_url_content(const std::string& url);

        /**
         * @brief Helper to handle data returned by libcurl
         */
        static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
            static_cast<std::string*>(userp)->append(static_cast<const char*>(contents), size * nmemb);
            return size * nmemb;
        }

    public:
    
        std::string apiKey = "Enter_API_key_for_serpapi.com";

        void register_tool(json& tools);
        void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
};

/**
 * DELEGATION SYSTEM (Recursive Sub-Agents)
 * This system allows a primary 'ollama_system' to spawn secondary instances
 * to handle complex, isolated, or parallel tasks.
 */

//using json = nlohmann::json;


//class TOOL_DELEGATOR {
//    private:
//    
//    public:
//        // Testing switch: Turn this off to prevent the AI from spawning sub-agents
//        bool enable_delegation = true;
//
//        /**
//         * @brief Registers the delegation tool to the provided chat instance.
//         */
//        void register_tool(json& tools);
//
//        /**
//         * @brief Handles the tool call and manages the lifecycle of the sub-agent.
//         */
//        void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
//};

class TOOL_TASK_RUNNER 
{
private:
    TASK_SIMPLE_MANAGER task_manager;

    /**
     * @brief Helper for case-insensitive string comparison
     */
    bool iequals(const std::string& a, const std::string& b);

public:
    
    std::filesystem::path OLLI_DIRECTORY;    

    /**    
    * @brief Registers the task execution tool to the chat instance.
     */
    void register_tool(json& tools);
    
    /**
     * @brief Handles the tool call by matching phrases and injecting a user-role instruction.
     */
    void handle_tool(
        ollama_system& main_instance, 
        ollama_system& instance, 
        const std::string& tool_name, 
        const json& tool_args, 
        const std::string& call_id);

    /**
     * @brief Background monitor hook for out-of-loop logic.
     */
    void monitor_tool(ollama_system& instance);
};


class OLLAMA_SYSTEM_PROPERTIES
{
    public:

    std::filesystem::path OLLI_DIERCTORY = "";
    std::filesystem::path path_output = "";
    std::filesystem::path path_history = "";

    std::string model = "qwen3:8b";
    std::string host = "localhost";
    int port = 11434;
    int num_ctx = 8192; // size
    bool stream_output = true; 
    bool use_thinking = true;

    string web_search_api_key = "Enter_API_key_for_serpapi.com";
    std::string hue_ip = "127.0.0.1";
    std::string hue_key = "Enter_Hue_Bridge_API_Key";
    std::string hue_path = "scenes.json";
};

class ollama_system {
    private:

    std::vector<std::unique_ptr<ollama_system>> background_tasks;
    
    json tools = json::array();
    std::chrono::steady_clock::time_point last_consolidation = std::chrono::steady_clock::now();

    TOOL_GET_CURRENT_TIME current_time;
    TOOL_TIMER timer;
    TOOL_HUE hue;
    TOOL_SET_THINKING_MODE thinking;
    TOOL_WEB_SEARCH web;
    //TOOL_DELEGATOR delegator;
    TOOL_TASK_RUNNER task_runner;

    void handle_instance_tools(KEYBOARD_INPUT& Keyboard_Input);

    public:

    std::string OLLAMA_OPENING =             
            //"You are a helpful assistant with access to tools. "
            //"1. For delayed requests, use set_timer. Always summarize the "
            //"user's intent in the 'reminder' field (e.g., 'Turn off "
            //"the living room fan'). "
            //"2. When the system sends a message starting "
            //"with 'SYSTEM NOTIFICATION: Timer Expired', look at the "
            //"associated reminder and immediately call the relevant tool "
            //"to fulfill that action without asking for further confirmation.";

            "You are a assistant with access to tools. "
            "You will responses will be short and sweet. ";


    OLLAMA_SYSTEM_PROPERTIES PROPS;

    // Use Atomics for thread safety to avoid data races
    std::thread chat_thread;
    std::atomic<bool> is_processing{false};
    std::atomic<bool> running{true};

    std::queue<ToolCall> pending_tool_calls;

    // parameters
    int consolitation_starts_starts_at = 20;
    int consolitation_sizes = 10;

    ollama_system_status status;    

    std::vector<Message> history;
    ChatResult last_received;

    std::string tts_buffer = "";

    void open();
    void open(OLLAMA_SYSTEM_PROPERTIES Properties);
    
    void integrate_tool_result(const std::string& raw_result);
    void send(const std::string& user_input, const std::string& role = "user");
    void send_tool_result(const std::string& tool_call_id, const std::string& result);

    // Helper to reset the signal
    void stop();

    /**
     * Updates the internal status struct by scanning the history.
     */
    void update_status();

    void history_write(std::string Directory);

    void write_to_tts();

    void input(KEYBOARD_INPUT& Key_Input);
    
    void process(KEYBOARD_INPUT& Keyboard_Input);

    void consolidate_check(KEYBOARD_INPUT& Keyboard_Input);

};

// FIX: Added 'static' to ensure internal linkage. 
// This prevents the "multiple definition" error during linking.
static std::mutex history_mutex;

void consolidate(std::vector<Message>& chat_history, ollama_system& config, KEYBOARD_INPUT& kb);

#endif