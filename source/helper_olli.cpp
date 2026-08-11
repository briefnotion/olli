#ifndef helper_cpp
#define helper_cpp

#include "helper_olli.h"

/**
 * Simulates the visual effect of the custom getline
 * by printing the string character by character.
 */
void simulateTyping(const std::string& text) {
    for (char ch : text) {
        // Handle the visual newline fix we discussed earlier
        if (ch == '\n' || ch == '\r') {
            std::cout << "\r\n" << std::flush;
        } else {
            std::cout << ch << std::flush;
        }
        // Small delay to make it look "natural" or 0 for instant
        // std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
    }
    std::cout << std::endl; // Final break after the message is "typed"
}

// ----

void Settings::load_settings() {
    fs::path config_path = get_settings_path() / "settings.json";;

    // Create directory if it doesn't exist
    if (!fs::exists(config_path.parent_path())) {
        fs::create_directories(config_path.parent_path());
    }

    // If file doesn't exist, save defaults immediately
    if (!fs::exists(config_path)) {
        save_settings();
        return;
    }

    // Read and parse the file
    try {
        std::cout << config_path << std::endl;
        std::ifstream file(config_path);
        json j;
        file >> j;

        // Map JSON values back to variables
        // Use .value() to provide a fallback if a key is missing
        //username = j.value("username", username);
        //window_width = j.value("window_width", window_width);
        //window_height = j.value("window_height", window_height);
        //fullscreen = j.value("fullscreen", fullscreen);
        //volume = j.value("volume", volume);

        tool_web_search_apiKey = j.value("tool_web_search_apiKey", tool_web_search_apiKey);
        tool_hue_lights_apiKey = j.value("tool_hue_lights_apiKey", tool_hue_lights_apiKey);
        tool_hue_lights_bridge_ip = j.value("tool_hue_lights_bridge_ip", tool_hue_lights_bridge_ip);
        
        std::cout << "Settings loaded successfully from: " << config_path << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error loading settings: " << e.what() << " - Using defaults." << std::endl;
    }
}

// Save current variables to the JSON file
void Settings::save_settings() {
    fs::path config_path = get_settings_path() / "settings.json";

    json j;
    //j["username"] = username;
    //j["window_width"] = window_width;
    //j["window_height"] = window_height;
    //j["fullscreen"] = fullscreen;
    //j["volume"] = volume;

    j["tool_web_search_apiKey"] = tool_web_search_apiKey;
    j["tool_hue_lights_apiKey"] = tool_hue_lights_apiKey;
    j["tool_hue_lights_bridge_ip"] = tool_hue_lights_bridge_ip;

    std::ofstream file(config_path);
    if (file.is_open()) {
        file << j.dump(4); // Use 4 spaces for pretty-printing
        std::cout << "Settings saved to: " << config_path << std::endl;
    }
}

// Helper to find the home directory across Windows/Linux/macOS
fs::path Settings::get_settings_path() {
    #ifdef _WIN32
        const char* home_dir = std::getenv("USERPROFILE");
    #else
        const char* home_dir = std::getenv("HOME");
    #endif
    // std::getenv may return nullptr; constructing a std::string / fs::path
    // from nullptr is undefined behaviour. Fall back to the current
    // directory so the program degrades gracefully instead of crashing.
    fs::path base = (home_dir != nullptr) ? fs::path(home_dir) : fs::current_path();
    return base / "olli_files";
}

// ----



/**
 * VOCA_CONTROL Class
 * * Provides a C++ interface to communicate with the Voca Pro (v3.2.9) Python node.
 * Handles reading status from 'voca_status.json' and sending commands to 'voca_command.json'.
 */


VOCA_CONTROL::VOCA_CONTROL(std::string olli_dir = "") 
{
    if (olli_dir.empty()) {
        const char* home = std::getenv("HOME");
        // Fall back to "." when HOME is unset so the derived paths below
        // remain valid relative paths rather than an empty prefix.
        m_olli_dir = (home != nullptr) ? std::string(home) + "/olli_files" : "./olli_files";
    } else {
        m_olli_dir = olli_dir;
    }

    m_status_path = m_olli_dir + "/voca_status.json";
    m_command_path = m_olli_dir + "/voca_command.json";
}

/**
 * Sends a command to Voca.
 * Automatically attaches a high-resolution timestamp to ensure 
 * the Python script processes it even if the command text hasn't changed.
 */
bool VOCA_CONTROL::sendCommand(const std::string& command) 
{
    try {
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        );
        double now = static_cast<double>(duration.count()) / 1000.0;

        json j;
        j["command"] = command;
        j["last_update"] = now;

        std::ofstream file(m_command_path);
        if (!file.is_open()) return false;

        file << j.dump(4);
        file.close();
        return true;
    } catch (...) {
        return false;
    }
}

/**
 * Reads the current status of Voca from the status file.
 */
VOCA_CONTROL::VocaStatus VOCA_CONTROL::getStatus() 
{
    VocaStatus vs;
    if (!fs::exists(m_status_path)) return vs;

    try {
        std::ifstream file(m_status_path);
        json j;
        file >> j;

        vs.status = j.value("status", "unknown");
        vs.timestamp = j.value("timestamp", 0.0);
        vs.is_awake = j.value("is_awake", false);
        vs.is_busy = j.value("is_busy", false);
        vs.valid = true;
    } catch (...) {
        vs.valid = false;
    }
    return vs;
}

/**
 * Helper functions for common state checks
 */
bool VOCA_CONTROL::isAwake() 
{ 
    return getStatus().is_awake; 
}

bool VOCA_CONTROL::isPaused() 
{ 
    return getStatus().is_busy; 
}

// ----

// Constructor: Save state and enter raw mode
KEYBOARD_INPUT::KEYBOARD_INPUT() {
    if (tcgetattr(STDIN_FILENO, &oldt) == 0) {
        newt = oldt;

        // Apply the "Raw" flags we discussed
        newt.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | IEXTEN | ISIG));
        newt.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
        
        newt.c_cc[VMIN] = 0;
        newt.c_cc[VTIME] = 0;

        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }
}

// ----

void KEYBOARD_INPUT::getNextInteraction(std::filesystem::path& folderPath) 
{
    if (std::filesystem::exists(folderPath))
    {
        for (const auto& entry : std::filesystem::directory_iterator(folderPath)) 
        {
            if (entry.is_regular_file()) 
            {
                std::ifstream file(entry.path());

                if (!file.is_open()) continue;

                std::string content((std::istreambuf_iterator<char>(file)),
                                    std::istreambuf_iterator<char>());
                file.close();

                std::filesystem::remove(entry.path()); 

                LINE = content;

                // ---- 

                if (starts_with(content, "go to sleep"))
                {
                    VOCA.sendCommand("sleep");
                    std::cout << LINE << std::endl;
                    INTERRUPTED = true;
                    ENTER_PRESSED = true;
                }
                else
                {
                    std::cout << LINE << std::endl;
                    INTERRUPTED = true;
                    ENTER_PRESSED = true;
                }
            }
        }
    }
}

// Destructor: Automatically restore the terminal
KEYBOARD_INPUT::~KEYBOARD_INPUT() {
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    // Optional: Ensure the cursor is on a fresh line
    std::cout << std::endl; 
}

void KEYBOARD_INPUT::keyboard_input()
{
    if (PROPS.ENABLED)
    {
        char ch;


        // read() will now return 0 if no character is waiting
        while (read(STDIN_FILENO, &ch, 1) > 0) 
        {
            
            double gap_time = enter_ready.elapsed_time();

            //std::cout << static_cast<int>(ch) << std::endl;
            if (ch == 10 || ch == 13 || ch == '\r') 
            {
                LINE += '\n';
                std::cout << "\r\n" << std::flush;
                if (gap_time > 0.1)
                {
                    INTERRUPTED = true;
                    ENTER_PRESSED = true;
                }
            }
            else if ((ch == 127 || ch == 8)) 
            {
                if (!LINE.empty()) 
                {
                    LINE.pop_back();
                    std::cout << "\b \b" << std::flush;
                }
            }
            else 
            {
                LINE += ch;
                std::cout << ch << std::flush;
                INTERRUPTED = true;
            }

            
            enter_ready.start_timer(); // Reset the timer on each key press
        }

        // or get text from voca
        
        if (PROPS.ENABLE_LIRA_VOCA)
        {
            getNextInteraction(PROPS.path_input);
        }

        IS_TYPING = !LINE.empty();
    }
}


void KEYBOARD_INPUT::reset()
{
    LINE.clear();
    ENTER_PRESSED = false;
    INTERRUPTED = false;
}

#endif