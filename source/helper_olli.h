#ifndef helper_h
#define helper_h

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <termios.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "stringthings.h"
#include "fled_time.h"

// ----

void simulateTyping(const std::string& text);

// ----

namespace fs = std::filesystem;
using json = nlohmann::json;

class Settings {
    public:
        // 1. Define your variables with default values
        //std::string username = "DefaultUser";
        //int window_width = 1280;    // test setting
        //int window_height = 720;    // test setting
        //bool fullscreen = false;    // test setting
        //double volume = 0.75;       // test setting

        std::string tool_web_search_apiKey = "Enter_API_key_for_serpapi.com";
        std::string tool_hue_lights_apiKey = "Enter_API_key_for_HUE_Lights";
        std::string tool_hue_lights_bridge_ip = "127.0.0.1";

        // Load settings from the home directory
        void load_settings();

        // Save current variables to the JSON file
        void save_settings();

        // Helper to find the home directory across Windows/Linux/macOS
        fs::path get_settings_path();
};

// ----

class LIRA_CONTROL 
{
    public:
        /**
         * Checks the control file to see if Lira is currently speaking.
         * Returns true if is_speaking is true, false otherwise.
         */
        bool isLiraSpeaking(std::filesystem::path Control_File_JSON);

        /**
         * Sets the interrupt signal to true in the control file.
         * This will trigger the Python monitor to kill the speech process.
         */
        void setLiraInterrupt(std::filesystem::path Control_File_JSON);
};


// ----


/**
 * VOCA_CONTROL Class
 * * Provides a C++ interface to communicate with the Voca Pro (v3.2.9) Python node.
 * Handles reading status from 'voca_status.json' and sending commands to 'voca_command.json'.
 */
class VOCA_CONTROL {
public:
    struct VocaStatus {
        std::string status;
        double timestamp;
        bool is_awake;
        bool is_busy;
        bool valid = false;
    };

    VOCA_CONTROL(std::string olli_dir);

    /**
     * Sends a command to Voca.
     * Automatically attaches a high-resolution timestamp to ensure 
     * the Python script processes it even if the command text hasn't changed.
     */
    bool sendCommand(const std::string& command); 

    /**
     * Reads the current status of Voca from the status file.
     */
    VocaStatus getStatus(); 

    /**
     * Helper functions for common state checks
     */
    bool isAwake();
    bool isPaused();

private:
    std::string m_olli_dir;
    std::string m_status_path;
    std::string m_command_path;
};


// ----

class KEYBOARD_INPUT_PROPERTIES
{
    public:

    std::filesystem::path path_input = "";
    std::filesystem::path lira_control_file = "";

    bool ENABLED = false;
    bool ENABLE_LIRA_VOCA = true;
};

class KEYBOARD_INPUT
{
    private:

        struct termios oldt, newt;
        EFFICIANTCY_TIMER_EASY enter_ready;

        void getNextInteraction(std::filesystem::path& folderPath);

    public:

        KEYBOARD_INPUT_PROPERTIES PROPS;

        std::string LINE = "";
        bool ENTER_PRESSED = false;
        bool INTERRUPTED = false;
        bool IS_TYPING = false;

        // Voca
        VOCA_CONTROL VOCA;
        LIRA_CONTROL LIRA;

        KEYBOARD_INPUT();
        ~KEYBOARD_INPUT();

        void keyboard_input();

        void reset();
};



#endif