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
#include "stringthings.h"

using json = nlohmann::json;

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

class OLLAMA_SYSTEM_PROPERTIES
{
    public:

    std::filesystem::path path_output = "";
    std::filesystem::path path_history = "";

    std::string model = "qwen3:8b";
    std::string host = "localhost";
    int port = 11434;
    int num_ctx = 8192; // size
    bool stream_output = true; 
    bool use_thinking = true;

};


class ollama_system {
    private:
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
    json tools = json::array();

    std::string tts_buffer = "";

    void open();
    void add_tool(const std::string& name, const std::string& description, json parameters);
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
};

// FIX: Added 'static' to ensure internal linkage. 
// This prevents the "multiple definition" error during linking.
static std::mutex history_mutex;

void consolidate(std::vector<Message>& chat_history, ollama_system& config, KEYBOARD_INPUT& kb);

#endif