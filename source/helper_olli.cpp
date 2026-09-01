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

std::string timestamp_prefix() {
    std::time_t now = std::time(nullptr);
    std::tm local_tm{};
    localtime_r(&now, &local_tm); // _r: thread-safe, unlike plain localtime()'s static buffer

    char buf[32]; // comfortably over the realistic ~12 bytes - satisfies
                   // -Wformat-truncation's worst-case (%d could in theory
                   // print an 11-digit int) rather than the realistic one
    std::snprintf(buf, sizeof(buf), "%02d%02d%02d.%02d%02d",
                  (local_tm.tm_year + 1900) % 100,
                  local_tm.tm_mon + 1,
                  local_tm.tm_mday,
                  local_tm.tm_hour,
                  local_tm.tm_min);

    return std::string(buf);
}

// ----

namespace {
    // Matches history_write()'s own rule-line convention (olla.cpp) rather
    // than inventing a second style for a second debug file.
    constexpr const char* DEBUG_LOG_RULE = "------------------------------------";
}

DEBUG_LOG_CLASS& DEBUG_LOG_CLASS::instance() {
    static DEBUG_LOG_CLASS the_instance;
    return the_instance;
}

// HH:MM:SS.mmm, local time - finer-grained than timestamp_prefix()'s
// minute resolution above (that one's sized for filenames; this one's
// sized for telling rapid-fire events apart, which is exactly what this
// log gets used to diagnose - a flapping sensor firing several times
// inside one minute needs more than minute resolution to make sense of
// afterward).
std::string DEBUG_LOG_CLASS::time_now() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm local_tm{};
    localtime_r(&now_c, &local_tm);

    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec,
                  static_cast<int>(ms.count()));
    return std::string(buf);
}

// One record shape shared by log_message() and log_event() below, so every
// entry in debug_full_history.txt reads the same way regardless of which
// one wrote it - a real message, a DIRECTOR_NOTE, a raw tool result, an
// instance created/closed marker. Bounding every record with the same
// rule line on the way out is what actually fixes the readability
// problem the old one-line-per-entry format had: content that itself
// spans many lines, or happens to contain bracket-looking text of its
// own (sidetrack-consolidate's own summarization prompt quotes older
// [role]: content verbatim - see sidetrack.cpp), used to be visually
// indistinguishable from a real header on the line right above or below
// it. Caller already holds file_mutex.
void DEBUG_LOG_CLASS::write_record(const std::string& instance_label, const std::string& kind, const std::string& content) {
    if (!file.is_open()) return;
    file << "=== " << instance_label << " / " << kind << " ===\n"
         << "Time:    " << time_now() << "\n"
         << "Content: " << content << "\n"
         << DEBUG_LOG_RULE << "\n" << std::flush;
}

void DEBUG_LOG_CLASS::reset(const std::filesystem::path& filepath) {
    std::lock_guard<std::mutex> lock(file_mutex);
    file.open(filepath, std::ios::out | std::ios::trunc);
}

void DEBUG_LOG_CLASS::log_message(const std::string& instance_label, const std::string& role, const std::string& content) {
    std::lock_guard<std::mutex> lock(file_mutex);
    write_record(instance_label, role, content);
}

void DEBUG_LOG_CLASS::log_event(const std::string& instance_label, const std::string& event) {
    std::lock_guard<std::mutex> lock(file_mutex);
    write_record(instance_label, "EVENT", event);
}

// ----

// Helper to find the home directory across Windows/Linux/macOS
static fs::path get_home_dir() {
    #ifdef _WIN32
        const char* home_dir = std::getenv("USERPROFILE");
    #else
        const char* home_dir = std::getenv("HOME");
    #endif
    // std::getenv may return nullptr; constructing a std::string / fs::path
    // from nullptr is undefined behaviour. Fall back to the current
    // directory so the program degrades gracefully instead of crashing.
    return (home_dir != nullptr) ? fs::path(home_dir) : fs::current_path();
}

void Settings::load_settings() {
    fs::path config_path = get_settings_path() / "settings.json";;

    // A named profile that doesn't exist yet starts as a copy of the shared
    // ~/olli_files, if one exists, instead of empty defaults - so switching
    // profiles doesn't mean re-entering API keys or losing history. "models"
    // is skipped: it holds the (large) whisper model file, which stays
    // shared out of ~/olli_files rather than duplicated per profile - see
    // Settings::get_shared_path(), used for that lookup instead.
    if (!profile_name.empty() && !fs::exists(config_path.parent_path())) {
        fs::path default_path = get_home_dir() / "olli_files";
        if (fs::exists(default_path)) {
            fs::create_directories(config_path.parent_path());
            for (const auto& entry : fs::directory_iterator(default_path)) {
                if (entry.path().filename() == "models") continue;
                fs::copy(entry.path(), config_path.parent_path() / entry.path().filename(),
                          fs::copy_options::recursive);
            }
        }
    }

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

    std::ofstream file(config_path);
    if (file.is_open()) {
        file << j.dump(4); // Use 4 spaces for pretty-printing
        std::cout << "Settings saved to: " << config_path << std::endl;
    }
}

fs::path Settings::get_settings_path() {
    std::string dir_name = profile_name.empty() ? "olli_files" : "olli_files_" + profile_name;
    return get_home_dir() / dir_name;
}

// The shared, profile-independent ~/olli_files directory - used for things
// meant to stay common to every profile (currently just the whisper model).
fs::path Settings::get_shared_path() {
    return get_home_dir() / "olli_files";
}

#endif