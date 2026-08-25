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
#include <mutex>

#include <nlohmann/json.hpp>

#include "stringthings.h"

// ----

void simulateTyping(const std::string& text);

// Current local time as "YYMMDD.HHMM" (e.g. "260821.1430") - for
// timestamped file names, see e.g. OUTPUT_CLASS::close_chat_log()
// (user_io.cpp).
std::string timestamp_prefix();

// ----

// Raw, unfiltered, append-only debug log of every message ever added to any
// ollama_system's history - main chat, sidetrack (consolidation/second-guess),
// task-runner background instances, all of it, in the order it happened.
// Review only: nothing in the program ever reads this back. 'tool' and
// DIRECTOR_NOTE 'system' messages now stay in live history too (see
// send_tool_result()'s comment, olla.cpp, for why the old immediate
// deletion was removed) rather than only ever existing here - but
// consolidate() (sidetrack.cpp) still eventually folds old messages into an
// LLM-written summary in the live history/history.json, so this remains
// the one place the full, unsummarized original wording survives for
// whoever's debugging a session afterward.
//
// A free function taking plain strings (not a Message&) rather than a
// method on ollama_system: Message is defined in olla.h, which includes
// this header, not the other way around - so this stays decoupled from
// that type instead of creating a circular include.
//
// g_debug_log_file must be 'inline' (C++17), not 'static' - same reasoning
// as history_mutex in olla.h: a 'static' definition here would give every
// translation unit its own private copy, so writes from different threads
// (main chat, sidetrack) would land in different, unconnected file handles
// instead of the one real log.
inline std::mutex g_debug_log_mutex;
inline std::ofstream g_debug_log_file;

// Truncates and (re)opens the debug log fresh - call once at startup,
// before any thread that might log has started.
void debug_log_reset(const std::filesystem::path& filepath);

// Appends one "[role] content" line and flushes immediately, so the log is
// current even if the process is killed rather than exited cleanly.
// Thread-safe - any ollama_system instance, from any thread, can call this.
void debug_log_message(const std::string& role, const std::string& content);

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

// Who olli is talking to this session - lives on CLASS_SYSTEM (system.h),
// reachable everywhere Settings already is, since a tool that wants a name
// to show/act on will want it the same way it'd want anything else there.
// Deliberately just three plain strings, all optional (empty = not set) -
// see the design discussion this came out of for why: 'about' is one open
// field rather than a guessed-at set of named ones (pronouns, timezone,
// preferences...), easy to split out later once something concrete actually
// needs a piece of it split out, instead of guessing now.
class USER_IDENTITY {
    public:
        // Mirrors Settings::profile_name above (set once in main.cpp,
        // right where profile_name itself is resolved) - not a separate
        // concept, just makes the same value reachable from CLASS_SYSTEM
        // too. Empty for the shared/no-profile default.
        std::string name = "";

        // Freeform, optional - "Ron", "Ronald Somebody", whatever's wanted
        // beyond the short profile name.
        std::string full_name = "";

        // Freeform notes for anything that wants more context than just a
        // name - a remote tool's own per-user profile, later maybe folded
        // into the model's own persona prompt.
        std::string about = "";
};

#endif