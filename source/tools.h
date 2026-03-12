#ifndef tools_h
#define tools_h

#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>

#include <curl/curl.h>

#include "olla.h"

class TOOL_SET_THINKING_MODE
{
public:
    void register_tool(ollama_system& chat);
    void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
};

class TIMER_SIMPLE {
public:
    explicit TIMER_SIMPLE(double seconds = 0.0, std::string reminder = "") 
        : m_duration(seconds), m_reminder(reminder), m_running(false) {}
    void start();
    bool isFinished() const;
    double getRemainingTime() const;
    std::string getReminder() const;

private:
    std::chrono::duration<double> m_duration;
    std::chrono::steady_clock::time_point m_startTime;
    std::string m_reminder; 
    bool m_running;
};


class TOOL_GET_CURRENT_TIME
{
public:
    void register_tool(ollama_system& chat);
    void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
};


class TOOL_TIMER {
public:
    std::map<std::string, TIMER_SIMPLE> active_timers;

    void register_tool(ollama_system& chat);
    void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
    
    // Updated signature:
    void monitor_tool(ollama_system& chat);
};

class TOOL_HUE 
{
    private:
        // Helper to handle CURL responses
        static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
            // 1. Cast userp to a std::string pointer
            // 2. Cast contents to a const char pointer
            static_cast<std::string*>(userp)->append(static_cast<const char*>(contents), size * nmemb);
            return size * nmemb;
        }

        // Generic function to talk to the Bridge
        std::string make_request(const std::string& method, const std::string& endpoint, const std::string& body);
        std::pair<double, double> rgbToXY(int r, int g, int b);

    public:
        // SET THESE TO YOUR REAL VALUES
        std::string bridge_ip = ""; // Your Bridge IP  
        std::string api_key = "";   // Your generated username

        void register_tool(ollama_system& chat);
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

        void register_tool(ollama_system& chat);
        void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
};


/**
 * DELEGATION SYSTEM (Recursive Sub-Agents)
 * This system allows a primary 'ollama_system' to spawn secondary instances
 * to handle complex, isolated, or parallel tasks.
 */

//using json = nlohmann::json;


class TOOL_DELEGATOR {
    private:
    

public:
    // Testing switch: Turn this off to prevent the AI from spawning sub-agents
    bool enable_delegation = true;

    /**
     * @brief Registers the delegation tool to the provided chat instance.
     */
    void register_tool(ollama_system& chat);

    /**
     * @brief Handles the tool call and manages the lifecycle of the sub-agent.
     */
    void handle_tool(ollama_system& chat, const std::string& name, const json& args, const std::string& tc_id);
};

/**
 * IMPLEMENTATION NOTE:
 * Since your ollama_system uses a 'chat_thread' and 'is_processing' atomics,
 * ensure that 'handle_tool' is called from a context where it's safe to block, 
 * or adapt the polling loop to be non-blocking.
 * * To prevent the "Ouroboros" (Infinite Recursion):
 * When creating 'sub_agent', do NOT call 'register_tool' for delegation on the sub-agent 
 * unless you implement a 'depth' counter.
 */




class TOOL_SYSTEM_CLASS
{
    private:

    public:
    
    std::chrono::steady_clock::time_point last_consolidation = std::chrono::steady_clock::now();

    TOOL_GET_CURRENT_TIME current_time;
    TOOL_TIMER timer;
    TOOL_HUE hue;
    TOOL_SET_THINKING_MODE thinking;
    TOOL_WEB_SEARCH web;
    TOOL_DELEGATOR delegator;

    void process(ollama_system& chat);

};


#endif // EGG_TIMER_H