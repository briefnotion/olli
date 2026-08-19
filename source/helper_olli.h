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

        // Set from the command line (e.g. `./olli ron`) so each person gets
        // their own settings/history/scenes under ~/olli_files_<profile_name>
        // instead of the shared ~/olli_files. Empty means the shared default.
        std::string profile_name = "";

        // Load settings from the home directory
        void load_settings();

        // Save current variables to the JSON file
        void save_settings();

        // Helper to find the home directory across Windows/Linux/macOS
        fs::path get_settings_path();

        // The shared ~/olli_files directory, regardless of profile_name -
        // for things that stay common across profiles (see load_settings()).
        fs::path get_shared_path();
};

// ----

// Speech-to-text input (Voca) is no longer a separate process talking
// through files - it's in-process now (see audio_control.h/AUDIO_CONTROL_CLASS
// and voca.hpp). main.cpp drains its transcripts each loop tick and feeds
// them into KEYBOARD_INPUT's LINE/INTERRUPTED/ENTER_PRESSED below, the same
// fields a typed line sets.

class KEYBOARD_INPUT_PROPERTIES
{
    public:

    bool ENABLED = false;
};

class KEYBOARD_INPUT
{
    private:

        struct termios oldt, newt;
        EFFICIANTCY_TIMER_EASY enter_ready;

    public:

        KEYBOARD_INPUT_PROPERTIES PROPS;

        std::string LINE = "";
        bool ENTER_PRESSED = false;
        bool INTERRUPTED = false;
        bool IS_TYPING = false;

        KEYBOARD_INPUT();
        ~KEYBOARD_INPUT();

        void keyboard_input();

        void reset();
};



#endif