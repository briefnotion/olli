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

/**
 * Checks the control file to see if Lira is currently speaking.
 * Returns true if is_speaking is true, false otherwise.
 */
bool LIRA_CONTROL::isLiraSpeaking(std::filesystem::path Control_File_JSON) 
{
    try {
        std::ifstream file(Control_File_JSON);
        if (!file.is_open()) return false;

        json data;
        file >> data;
        return data.value("is_speaking", false);
    } catch (...) {
        // If file is being written to by Python (locked), just return false
        return false;
    }
}

/**
 * Sets the interrupt signal to true in the control file.
 * This will trigger the Python monitor to kill the speech process.
 */
void LIRA_CONTROL::setLiraInterrupt(std::filesystem::path Control_File_JSON) 
{
    try {
        json data;
        
        // 1. Read existing data first to preserve is_speaking status
        std::ifstream inFile(Control_File_JSON);
        if (inFile.is_open()) {
            inFile >> data;
            inFile.close();
        }

        // 2. Set interrupt to true
        data["interrupt"] = true;

        // 3. Write back to file
        std::ofstream outFile(Control_File_JSON);
        outFile << data.dump(4); // Pretty print with 4 spaces
    } catch (const std::exception& e) {
        std::cerr << "Failed to set interrupt: " << e.what() << std::endl;
    }
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
    std::string home_dir;
    #ifdef _WIN32
        home_dir = std::getenv("USERPROFILE");
    #else
        home_dir = std::getenv("HOME");
    #endif
    return fs::path(home_dir) / "olli_files";
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
        if (home) {
            m_olli_dir = std::string(home) + "/olli_files";
        }
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

                // check wakeup and sleep
                if (starts_with(content, "stop talking"))
                {
                    std::cout << "[INTERUPTION]" << std::endl;
                    LIRA.setLiraInterrupt(PROPS.lira_control_file);
                    INTERRUPTED = true;
                    ENTER_PRESSED = true;
                }
                else
                {
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
}

// Destructor: Automatically restore the terminal
KEYBOARD_INPUT::~KEYBOARD_INPUT() {
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    // Optional: Ensure the cursor is on a fresh line
    std::cout << std::endl; 
}

void KEYBOARD_INPUT::keyboard_input()
{
    char ch;
    // read() will now return 0 if no character is waiting
    while (read(STDIN_FILENO, &ch, 1) > 0) 
    {
        //std::cout << static_cast<int>(ch) << std::endl;
        if (ch == 10 || ch == 13 || ch == '\r') 
        {
            LINE += '\n';
            std::cout << "\r\n" << std::flush;
            INTERRUPTED = true;
            ENTER_PRESSED = true;
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
    }

    // or get text from voca
    getNextInteraction(PROPS.path_input);

    IS_TYPING = !LINE.empty();
}


void KEYBOARD_INPUT::reset()
{
    LINE.clear();
    ENTER_PRESSED = false;
    INTERRUPTED = false;
}

#endif