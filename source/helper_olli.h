#ifndef helper_h
#define helper_h

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <future>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <ctime>
#include <cstdio>

#include <nlohmann/json.hpp>

#include "stringthings.h"

// ----

void simulateTyping(const std::string& text);

// Current local time as "YYMMDD.HHMM" (e.g. "260821.1430") - for
// timestamped file names, see e.g. OUTPUT_CLASS::close_chat_log()
// (user_io.cpp).
std::string timestamp_prefix();

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

#endif