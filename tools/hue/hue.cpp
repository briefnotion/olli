// Hue remote tool - Philips Hue light control, ported out of olli's core
// (formerly TOOL_HUE, source/tools.cpp/.h + HUE_LIGHT_CLASS,
// source/tools_helper.h/.cpp) onto the tools/template/ plumbing. See
// ../PROTOCOL.md for the wire protocol, and this file's own header comment
// on olli_processing() for the split between tailored and generic code.
//
// Registers set_hue_light/list_hue_lights/manage_hue_scenes with the exact
// same names, descriptions, and argument shapes as the original TOOL_HUE -
// tools/presence/presence.cpp's on_home_action/on_away_action (and its own
// doc comments) already reference these by name, so nothing there needed
// to change for this port.
//
// Talks to the Hue bridge's CLIP v1 REST API directly over libcurl
// (HUE_LIGHT_CLASS below) - same endpoints, same request shapes, same
// scene-file format as the original. Two deliberate differences from the
// original, both explained where they happen:
//   - Bridge IP/API key are per-profile now (~/olli_files_<name>/
//     hue_settings.json, loaded on the "identity" message - see
//     switch_profile()), not baked into olli's own settings.json. First
//     time a profile is seen, its real bridge_ip/api_key get migrated in
//     automatically from the old settings.json if present - see
//     load_hue_settings().
//   - The shared lights_cache/name_to_id/local_scenes maps are now guarded
//     by a mutex (HUE_LIGHT_CLASS::state_mutex) - the original had a real,
//     if narrow, unsynchronized-access gap between the flash-cancel
//     detached thread and the main thread, closed here since this port
//     keeps that same detached-thread pattern.
// Two more fixes, made after the initial port once real testing surfaced
// them:
//   - "brightness" used to be a raw pass-through of Hue's native 0-254
//     `bri` scale - a user asking for "100" expecting near-full brightness
//     got ~39%. The model-facing argument is now a 0-100 percentage,
//     converted to 0-254 in handle_call() (brightness_percent_to_bri()) -
//     list_hue_lights' own "Bri:" report does the same conversion in
//     reverse (bri_to_brightness_percent()) so what's reported matches
//     what a percentage-based request would produce.
//   - A bridge/curl failure on set_hue_light or a scene load used to come
//     back as Hue's raw fabricated error JSON buried inside a "processed"/
//     "activated" sentence that reads like a success - response_is_error()
//     below now catches that (same shape list_hue_lights' own "Could not
//     reach the Hue Bridge" check already used) and reports a clear error
//     instead.
//
// Build: `make` in this directory (needs libcurl - see Makefile). Run:
// `./hue [host]` - does not need olli to already be running.

#include <nlohmann/json.hpp>

#include "olli_link.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
    // How often the light cache gets refreshed in the background, purely
    // so resolve_id()-by-name stays reasonably fresh between calls (list_
    // hue_lights already refreshes synchronously on every call regardless -
    // see handle_call() below). Replaces the original's tick-counted
    // refresh (every 10,000 TOOL_HUE::monitor_tool() calls, an arbitrary
    // amount of real time) with an actual wall-clock interval.
    constexpr int LIGHT_REFRESH_INTERVAL_SECONDS = 300;

    // =====================================================================
    // Scene storage - unchanged shape from source/tools_helper.h/.cpp, so
    // existing ~/olli_files_<name>/scenes.json files (yours already have a
    // real "away" scene, identical across profiles) keep working with zero
    // migration.
    // =====================================================================

    struct HUE_SCENE {
        std::string name;
        std::map<std::string, json> light_states; // light ID -> state snapshot
    };

    void to_json(json& j, const HUE_SCENE& s)
    {
        j = json{{"name", s.name}, {"light_states", s.light_states}};
    }

    void from_json(const json& j, HUE_SCENE& s)
    {
        j.at("name").get_to(s.name);
        j.at("light_states").get_to(s.light_states);
    }

    struct LightState {
        std::string id;
        std::string name;
        bool on = false;
        int brightness = 0;
        std::vector<double> xy;
        bool reachable = false;
    };

    // Model-facing brightness is a 0-100 percentage; Hue's own "bri" field
    // is 0-254. Both directions round to nearest and clamp their input, so
    // a stray out-of-range value from either side (a bad model argument, a
    // bridge reporting something odd) can't produce a wildly wrong result.
    int brightness_percent_to_bri(int percent)
    {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        return static_cast<int>(std::lround(percent / 100.0 * 254.0));
    }

    int bri_to_brightness_percent(int bri)
    {
        if (bri < 0) bri = 0;
        if (bri > 254) bri = 254;
        return static_cast<int>(std::lround(bri / 254.0 * 100.0));
    }

    // A Hue bridge response is either an error - a JSON object with an
    // "error" key (make_request()'s own fabricated
    // {"error": "CURL failed: ..."} on a curl-level failure, or
    // set_light()'s {"error": "Light 'X' not found"} for an unresolvable
    // id/name) or a JSON array with an "error" object as its first element
    // (the bridge itself rejecting the request, CLIP v1's own error shape)
    // - or it's a real result (an array of "success" objects, or the
    // /lights payload refresh_lights() parses separately). Used wherever a
    // call site needs to tell those apart instead of just forwarding
    // whatever came back - see handle_call()'s set_hue_light branch and
    // HUE_LIGHT_CLASS::load_scene() below for where a raw pass-through
    // used to read like a success.
    bool response_is_error(const std::string& response, std::string& detail)
    {
        try {
            json data = json::parse(response);
            if (data.is_object() && data.contains("error")) {
                detail = data["error"].is_string() ? data["error"].get<std::string>() : data["error"].dump();
                return true;
            }
            if (data.is_array() && !data.empty() && data[0].contains("error")) {
                detail = data[0]["error"].dump();
                return true;
            }
            return false;
        } catch (...) {
            detail = "unreadable response from bridge";
            return true;
        }
    }

    // =====================================================================
    // HUE_LIGHT_CLASS - direct bridge communication, state caching, and
    // name->ID resolution. Ported from source/tools_helper.h/.cpp with two
    // additions: state_mutex, guarding the three maps below against the
    // flash-cancel detached thread (see set_light()'s callers in
    // handle_call()) racing the main thread - the original had no locking
    // at all here - and load_scene() now checking each per-light request
    // via response_is_error() instead of firing them off and assuming
    // success.
    // =====================================================================

    class HUE_LIGHT_CLASS {
        public:
            void set_credentials(const std::string& ip, const std::string& key, const std::string& scenes_path)
            {
                bridge_ip = ip;
                api_key = key;
                std::lock_guard<std::mutex> lock(state_mutex);
                scene_filepath = scenes_path;
                load_scenes_from_disk();
            }

            std::string make_request(const std::string& method, const std::string& endpoint, const std::string& body = "")
            {
                CURL* curl = curl_easy_init();
                std::string read_buffer;
                if (curl) {
                    std::string url = "http://" + bridge_ip + "/api/" + api_key + endpoint;
                    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
                    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

                    if (!body.empty()) {
                        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                    }

                    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
                    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);

                    CURLcode res = curl_easy_perform(curl);
                    if (res != CURLE_OK) {
                        read_buffer = "{\"error\": \"CURL failed: " + std::string(curl_easy_strerror(res)) + "\"}";
                    }
                    curl_easy_cleanup(curl);
                }
                return read_buffer;
            }

            bool refresh_lights()
            {
                std::string response = make_request("GET", "/lights");
                try {
                    json data = json::parse(response);
                    if (data.is_array() && !data.empty() && data[0].contains("error")) return false;

                    std::map<std::string, LightState> new_cache;
                    std::map<std::string, std::string> new_name_to_id;

                    for (auto& [id, info] : data.items()) {
                        std::string name = info.value("name", "Unknown");
                        LightState state;
                        state.id = id;
                        state.name = name;

                        if (info.contains("state")) {
                            state.on = info["state"].value("on", false);
                            state.brightness = info["state"].value("bri", 0);
                            state.reachable = info["state"].value("reachable", false);
                            if (info["state"].contains("xy")) {
                                state.xy = info["state"]["xy"].get<std::vector<double>>();
                            }
                        }

                        new_cache[id] = state;
                        new_name_to_id[to_lower(name)] = id;
                    }

                    std::lock_guard<std::mutex> lock(state_mutex);
                    lights_cache = std::move(new_cache);
                    name_to_id = std::move(new_name_to_id);
                    return true;
                } catch (...) {
                    return false;
                }
            }

            std::string resolve_id(const std::string& input)
            {
                if (input.empty()) return "";
                if (to_lower(input) == "all") return "all";
                if (std::all_of(input.begin(), input.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) return input;

                std::string lower_input = to_lower(input);
                std::lock_guard<std::mutex> lock(state_mutex);
                auto it = name_to_id.find(lower_input);
                return it != name_to_id.end() ? it->second : "";
            }

            std::string set_light(const std::string& id_or_name, const json& state_body)
            {
                std::string id = resolve_id(id_or_name);
                if (id == "all") {
                    // Group 0 always contains every light on the bridge.
                    return make_request("PUT", "/groups/0/action", state_body.dump());
                }
                if (id.empty()) return "{\"error\": \"Light '" + id_or_name + "' not found\"}";
                return make_request("PUT", "/lights/" + id + "/state", state_body.dump());
            }

            // --- Scene logic ---

            std::string save_scene(const std::string& name)
            {
                std::string lower_name = to_lower(name);
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (local_scenes.count(lower_name)) {
                        return "Scene '" + name + "' already exists. Please remove it first or use a different name.";
                    }
                }

                // Not held across refresh_lights() - it takes state_mutex
                // itself, and this is a single, non-recursive mutex.
                if (!refresh_lights()) return "Failed to refresh lights for scene capture.";

                HUE_SCENE new_scene;
                new_scene.name = name;

                std::lock_guard<std::mutex> lock(state_mutex);
                for (auto const& [id, state] : lights_cache) {
                    json state_json;
                    state_json["on"] = state.on;
                    state_json["bri"] = state.brightness;
                    if (!state.xy.empty()) state_json["xy"] = state.xy;
                    new_scene.light_states[id] = state_json;
                }
                local_scenes[lower_name] = new_scene;
                save_scenes_to_disk();
                return "Scene '" + name + "' saved to disk.";
            }

            std::string load_scene(const std::string& name)
            {
                std::string lower_name = to_lower(name);
                std::map<std::string, json> states;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    auto it = local_scenes.find(lower_name);
                    if (it == local_scenes.end()) return "Scene '" + name + "' not found.";
                    states = it->second.light_states;
                }

                int failed = 0;
                std::string last_detail;
                for (auto const& [id, state_json] : states) {
                    std::string res = make_request("PUT", "/lights/" + id + "/state", state_json.dump());
                    if (response_is_error(res, last_detail)) ++failed;
                }

                if (failed == 0) return "Scene '" + name + "' activated.";
                if (static_cast<size_t>(failed) == states.size()) {
                    return "Error: Scene '" + name + "' could not be activated - bridge unreachable or rejected every light (" + last_detail + ").";
                }
                return "Scene '" + name + "' partially activated - " + std::to_string(failed) + " of "
                       + std::to_string(states.size()) + " light(s) failed (" + last_detail + ").";
            }

            std::string remove_scene(const std::string& name)
            {
                std::string lower_name = to_lower(name);
                std::lock_guard<std::mutex> lock(state_mutex);
                if (local_scenes.erase(lower_name)) {
                    save_scenes_to_disk();
                    return "Scene '" + name + "' removed.";
                }
                return "Scene '" + name + "' not found.";
            }

            std::map<std::string, HUE_SCENE> get_scenes()
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                return local_scenes;
            }

            std::map<std::string, LightState> get_cached_lights()
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                return lights_cache;
            }

            size_t cached_light_count()
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                return lights_cache.size();
            }

            static std::pair<double, double> rgb_to_xy(int r, int g, int b)
            {
                auto adjust = [](double val) {
                    val /= 255.0;
                    return (val > 0.04045) ? std::pow((val + 0.055) / 1.055, 2.4) : (val / 12.92);
                };
                double R = adjust(static_cast<double>(r));
                double G = adjust(static_cast<double>(g));
                double B = adjust(static_cast<double>(b));
                double X = R * 0.664511 + G * 0.154324 + B * 0.162028;
                double Y = R * 0.283881 + G * 0.668433 + B * 0.047685;
                double Z = R * 0.000088 + G * 0.072310 + B * 0.986039;
                double sum = X + Y + Z;
                double cx = (sum == 0) ? 0.0 : X / sum;
                double cy = (sum == 0) ? 0.0 : Y / sum;
                return {cx, cy};
            }

        private:
            static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp)
            {
                static_cast<std::string*>(userp)->append(static_cast<const char*>(contents), size * nmemb);
                return size * nmemb;
            }

            static std::string to_lower(std::string s)
            {
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                return s;
            }

            // Both assume the caller already holds state_mutex.
            void save_scenes_to_disk()
            {
                if (scene_filepath.empty()) return;
                try {
                    std::ofstream file(scene_filepath);
                    if (file.is_open()) file << json(local_scenes).dump(4);
                } catch (...) {
                    // Silently fail - matches the original's behavior.
                }
            }

            void load_scenes_from_disk()
            {
                if (scene_filepath.empty()) return;
                try {
                    std::ifstream file(scene_filepath);
                    if (file.is_open()) {
                        json j;
                        file >> j;
                        local_scenes = j.get<std::map<std::string, HUE_SCENE>>();
                    }
                } catch (...) {
                    // File might not exist yet, which is fine.
                }
            }

            std::string bridge_ip;
            std::string api_key;
            std::string scene_filepath;

            // Guards the three maps below - see the file-level comment on
            // why this exists (it didn't, in the original).
            std::mutex state_mutex;
            std::map<std::string, LightState> lights_cache;
            std::map<std::string, std::string> name_to_id;
            std::map<std::string, HUE_SCENE> local_scenes;
    };

    // =====================================================================
    // Per-profile settings - bridge_ip/api_key, following
    // ../presence/presence.cpp's PresenceSettings/settings_path_for()
    // pattern exactly (~/olli_files_<name>/, or ~/olli_files/ for the
    // shared default).
    // =====================================================================

    struct HueSettings {
        std::string bridge_ip = "127.0.0.1";
        std::string api_key = "Enter_Hue_Bridge_API_Key";
    };

    fs::path get_home_dir()
    {
        const char* home = std::getenv("HOME");
        return (home != nullptr) ? fs::path(home) : fs::current_path();
    }

    fs::path profile_dir_for(const std::string& profile_name)
    {
        std::string dir_name = profile_name.empty() ? "olli_files" : "olli_files_" + profile_name;
        return get_home_dir() / dir_name;
    }

    fs::path hue_settings_path_for(const std::string& profile_name)
    {
        return profile_dir_for(profile_name) / "hue_settings.json";
    }

    fs::path scenes_path_for(const std::string& profile_name)
    {
        return profile_dir_for(profile_name) / "scenes.json";
    }

    void save_hue_settings(const fs::path& path, const HueSettings& s)
    {
        json j;
        j["bridge_ip"] = s.bridge_ip;
        j["api_key"] = s.api_key;
        std::ofstream file(path);
        if (file.is_open()) file << j.dump(4);
    }

    // One-time migration source only - olli-core's old settings.json, from
    // before Hue support moved out here. Never written to; read only the
    // first time a profile has no hue_settings.json of its own yet (see
    // load_hue_settings() below), so this isn't an ongoing dependency on
    // olli-core's file format.
    HueSettings migrate_from_core_settings(const std::string& profile_name)
    {
        HueSettings s;
        fs::path old_path = profile_dir_for(profile_name) / "settings.json";
        if (!fs::exists(old_path)) return s;

        try {
            std::ifstream file(old_path);
            json j;
            file >> j;
            std::string old_ip = j.value("tool_hue_lights_bridge_ip", "");
            std::string old_key = j.value("tool_hue_lights_apiKey", "");
            // Only carry over values that look like they were actually
            // set, not olli-core's own historical placeholders
            // (source/helper_olli.h) - those are no more meaningful than
            // this file's own placeholder defaults above.
            if (!old_ip.empty() && old_ip != "127.0.0.1") s.bridge_ip = old_ip;
            if (!old_key.empty() && old_key != "Enter_API_key_for_HUE_Lights") s.api_key = old_key;
        } catch (...) {
            // Malformed/unreadable old file - fall back to placeholders,
            // same as a genuinely fresh profile.
        }
        return s;
    }

    HueSettings load_hue_settings(const std::string& profile_name)
    {
        fs::path path = hue_settings_path_for(profile_name);

        if (fs::exists(path)) {
            HueSettings s;
            try {
                std::ifstream file(path);
                json j;
                file >> j;
                s.bridge_ip = j.value("bridge_ip", s.bridge_ip);
                s.api_key = j.value("api_key", s.api_key);
            } catch (...) {
                // Malformed - fall back to placeholders.
            }
            return s;
        }

        // First time this profile has been seen here - bootstrap
        // hue_settings.json, seeded from olli-core's old settings.json if
        // it already has real values (see migrate_from_core_settings()),
        // same "write a placeholder file to edit" convention presence.cpp
        // uses for its own settings file.
        HueSettings s = migrate_from_core_settings(profile_name);
        fs::create_directories(path.parent_path());
        save_hue_settings(path, s);
        return s;
    }

    // =====================================================================
    // Active profile state + registration + call handling.
    // =====================================================================

    HUE_LIGHT_CLASS hue;
    std::string current_profile_name;
    HueSettings hue_settings;

    // Called once at startup (for the shared default) and again on every
    // "identity" message - loads this profile's bridge_ip/api_key, points
    // HUE_LIGHT_CLASS at this profile's scenes.json, and refreshes the
    // light cache. Returns a status string for the display.
    std::string switch_profile(const std::string& profile_name)
    {
        current_profile_name = profile_name;
        hue_settings = load_hue_settings(profile_name);
        hue.set_credentials(hue_settings.bridge_ip, hue_settings.api_key, scenes_path_for(profile_name).string());
        bool reached_bridge = hue.refresh_lights();

        std::string who = profile_name.empty() ? "shared default" : profile_name;
        if (!reached_bridge) {
            return "Profile: " + who + " - bridge " + hue_settings.bridge_ip + " unreachable";
        }
        return "Profile: " + who + " - bridge " + hue_settings.bridge_ip + ", "
               + std::to_string(hue.cached_light_count()) + " light(s)";
    }

    std::string handle_identity(const json& msg)
    {
        return switch_profile(msg.value("name", ""));
    }

    // Called on every disconnect - falls back to the shared-default
    // profile, same idea as ../presence/presence.cpp's
    // reset_to_default_profile(), just implemented as "reload the shared
    // profile" rather than a separate all-placeholder struct, since here
    // the shared profile is itself a real, meaningful one
    // (~/olli_files/hue_settings.json).
    void reset_to_default_profile()
    {
        switch_profile("");
    }

    json make_register_message()
    {
        json set_params = {
            {"type", "object"},
            {"properties", {
                {"light_id", {{"type", "string"}, {"description", "ID or Name (e.g., '2' or 'Computer'). Use 'all' to target every light."}}},
                {"on", {{"type", "boolean"}}},
                // 0-100 percentage - converted to Hue's native 0-254 "bri"
                // scale in handle_call() (brightness_percent_to_bri()).
                {"brightness", {{"type", "integer"}, {"description", "Brightness as a percentage, 0 (dimmest) to 100 (maximum)."}}},
                {"preset", {{"type", "string"}, {"enum", {"red", "green", "blue", "yellow", "magenta", "cyan", "orange", "purple", "pink", "white"}}}},
                {"hex", {{"type", "string"}}},
                {"alert", {{"type", "string"}, {"enum", {"none", "select", "lselect"}}}},
                {"flash_count", {{"type", "integer"}}},
                {"transition_ms", {{"type", "integer"}}}
            }},
            {"required", json::array({"light_id"})}
        };

        json scene_params = {
            {"type", "object"},
            {"properties", {
                {"action", {{"type", "string"}, {"enum", {"save", "load", "remove", "list"}}}},
                {"name", {{"type", "string"}, {"description", "The name of a SAVED local scene. Do not use for general actions like 'all lights off'."}}}
            }},
            {"required", json::array({"action"})}
        };

        return {
            {"type", "register"},
            {"tools", json::array({
                {
                    {"name", "set_hue_light"},
                    {"description", "Controls Hue lights by ID or Name. Use this for general commands like 'turn off all lights' by setting light_id to 'all'. Always execute this tool call for every request, even if you believe the state is already set."},
                    {"parameters", set_params}
                },
                {
                    {"name", "list_hue_lights"},
                    {"description", "Returns status of all connected lights. Always execute this tool call for every request, even if you believe you already know the state - never answer a status question (on/off, brightness, color) from a value stated earlier in the conversation."},
                    {"parameters", {{"type", "object"}}}
                },
                {
                    {"name", "manage_hue_scenes"},
                    {"description", "Saves, loads, or removes local light scenes. Only use for specific named snapshots (e.g., 'home', 'away')."},
                    {"parameters", scene_params}
                }
            })}
        };
    }

    std::string handle_call(OLLI_LINK& link, const json& msg)
    {
        std::string call_id = msg.value("call_id", "");
        std::string name = msg.value("name", "");
        json args = msg.value("arguments", json::object());

        if (name == "list_hue_lights") {
            if (!hue.refresh_lights()) {
                std::string err = "Error: Could not reach the Hue Bridge.";
                link.send_result(call_id, err);
                return err;
            }
            auto lights = hue.get_cached_lights();
            std::stringstream ss;
            ss << "Current Lights: ";
            for (auto const& [id, state] : lights) {
                ss << "[" << id << "] " << state.name << " (Power: " << (state.on ? "ON" : "OFF")
                   << ", Bri: " << bri_to_brightness_percent(state.brightness) << "%"
                   << (state.reachable ? "" : " *UNREACHABLE*") << "), ";
            }
            link.send_result(call_id, ss.str());
            return "Call answered: list_hue_lights (" + std::to_string(lights.size()) + " light(s))";
        }

        if (name == "manage_hue_scenes") {
            std::string action = args.value("action", "");
            std::string scene_name = args.value("name", "");

            if (action.empty()) {
                std::string err = "Error: 'action' is required for manage_hue_scenes.";
                link.send_result(call_id, err);
                return err;
            }

            if (action == "list") {
                auto scenes = hue.get_scenes();
                std::string result;
                if (scenes.empty()) {
                    result = "No local scenes saved.";
                } else {
                    std::stringstream ss;
                    ss << "Saved Scenes: ";
                    for (auto const& [sname, scene] : scenes) ss << scene.name << ", ";
                    result = ss.str();
                }
                link.send_result(call_id, result);
                return "Call answered: manage_hue_scenes (list)";
            }

            if (scene_name.empty()) {
                std::string err = "Error: Scene name required for " + action;
                link.send_result(call_id, err);
                return err;
            }

            std::string res;
            if (action == "save") res = hue.save_scene(scene_name);
            else if (action == "load") {
                res = hue.load_scene(scene_name);
                // load_scene() only PUTs new state to the bridge, it
                // doesn't re-read it back - without this, the cached
                // lights_cache (what the live display and list_hue_lights
                // both read) would keep showing whatever it was before
                // the scene loaded until the next periodic refresh, up to
                // LIGHT_REFRESH_INTERVAL_SECONDS later.
                hue.refresh_lights();
            }
            else if (action == "remove") res = hue.remove_scene(scene_name);
            else res = "Error: Unknown scene action '" + action + "'";

            link.send_result(call_id, res);
            return "Scene " + action + ": " + scene_name;
        }

        if (name == "set_hue_light") {
            std::string target = args.value("light_id", "");
            json body;

            if (args.contains("on")) body["on"] = args.at("on").get<bool>();
            else if (!args.contains("alert") && !args.contains("flash_count")) body["on"] = true;

            // Model sends 0-100 - see make_register_message()'s "brightness"
            // description and brightness_percent_to_bri()'s own comment.
            if (args.contains("brightness")) body["bri"] = brightness_percent_to_bri(args.at("brightness").get<int>());

            std::string alert_mode = args.value("alert", "none");
            bool has_flash = args.contains("flash_count");
            int flash_count = 0;
            if (has_flash) {
                flash_count = args.at("flash_count").get<int>();
                alert_mode = (flash_count > 1) ? "lselect" : "select";
            }
            if (alert_mode != "none") body["alert"] = alert_mode;

            if (args.contains("transition_ms")) body["transitiontime"] = args.at("transition_ms").get<int>() / 100;

            bool color_set = false;
            if (args.contains("hex")) {
                std::string hex = args.value("hex", "");
                if (!hex.empty()) {
                    if (hex[0] == '#') hex.erase(0, 1);
                    unsigned int r, g, b;
                    if (std::sscanf(hex.c_str(), "%02x%02x%02x", &r, &g, &b) == 3) {
                        auto [x, y] = HUE_LIGHT_CLASS::rgb_to_xy(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b));
                        body["xy"] = {x, y};
                        color_set = true;
                    }
                }
            }

            if (!color_set && args.contains("preset")) {
                static const std::map<std::string, std::vector<double>> palette = {
                    {"red", {0.675, 0.322}}, {"green", {0.409, 0.518}}, {"blue", {0.167, 0.04}},
                    {"yellow", {0.432, 0.500}}, {"cyan", {0.157, 0.357}}, {"magenta", {0.41, 0.17}},
                    {"orange", {0.55, 0.41}}, {"purple", {0.27, 0.13}}, {"pink", {0.41, 0.21}},
                    {"white", {0.31, 0.32}}
                };
                auto it = palette.find(args.value("preset", ""));
                if (it != palette.end()) body["xy"] = it->second;
            }

            std::string res = hue.set_light(target, body);

            std::string err_detail;
            if (response_is_error(res, err_detail)) {
                std::string err = "Error controlling light '" + target + "': " + err_detail;
                link.send_result(call_id, err);
                return "Error: set_hue_light (" + target + ")";
            }

            // set_light() only PUTs the new state to the bridge, it
            // doesn't re-read it back - without this, the live display
            // (and the next list_hue_lights call) would keep showing
            // whatever this light's state was before this command until
            // the next periodic refresh, up to
            // LIGHT_REFRESH_INTERVAL_SECONDS later.
            hue.refresh_lights();

            if (has_flash && flash_count > 1) {
                std::string resolved_id = hue.resolve_id(target);
                if (!resolved_id.empty() && resolved_id != "all") {
                    // Detached, fire-and-forget - matches the original
                    // TOOL_HUE::handle_tool()'s flash-cancel thread exactly.
                    // References the anonymous-namespace-scope `hue` above
                    // directly (static storage duration for the process's
                    // whole lifetime, same assumption the original made
                    // about its own TOOL_HUE-owned `hue` member).
                    std::thread([resolved_id, flash_count]() {
                        std::this_thread::sleep_for(std::chrono::seconds(flash_count));
                        hue.set_light(resolved_id, {{"alert", "none"}});
                    }).detach();
                }
            }

            std::string summary = "Light command for " + target + " processed. Result: " + res;
            link.send_result(call_id, summary);
            return "Call answered: set_hue_light (" + target + ")";
        }

        link.send_error(call_id, "Unknown tool name: " + name);
        return "Unknown call received: " + name;
    }

    // Called once per main-loop tick - see tools/template/template_tool.cpp
    // for the shape this follows. Top half (CUSTOMIZE #2, inherited from
    // the template) routes each message type; bottom half is the calls
    // into OLLI_LINK.
    void olli_processing(OLLI_LINK& link, bool socket_readable, std::string& status)
    {
        auto dispatch = [&](const json& msg) {
            std::string type = msg.value("type", "");
            if (type == "call") status = handle_call(link, msg);
            else if (type == "identity") status = handle_identity(msg);
        };

        // ---------------------------------------------------------------
        // Below this line: olli communication plumbing (see
        // ../template/olli_link.hpp / olli_link.cpp, carried over
        // unmodified). Nothing here needs to change for this tool.
        // ---------------------------------------------------------------
        link.service(socket_readable);

        if (link.consume_disconnected()) reset_to_default_profile();

        json msg;
        while (link.next_message(msg)) dispatch(msg);

        if (!link.status().empty()) status = link.status();
    }

    // --- Terminal handling (unchanged from tools/template/, see its own
    // comment for the full explanation) ---

    class RawTerminal {
        public:
            RawTerminal()
            {
                if (tcgetattr(STDIN_FILENO, &old_termios) == 0) {
                    termios raw = old_termios;
                    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
                    raw.c_cc[VMIN] = 0;
                    raw.c_cc[VTIME] = 0;
                    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
                    active = true;
                }
                std::cout << "\033[?25l" << std::flush;
            }

            ~RawTerminal()
            {
                std::cout << "\033[?25h" << std::flush;
                if (active) tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
            }

            RawTerminal(const RawTerminal&) = delete;
            RawTerminal& operator=(const RawTerminal&) = delete;

        private:
            termios old_termios{};
            bool active = false;
    };

    struct RGB { int r = 255, g = 255, b = 255; };

    // The published inverse of the exact "Wide RGB D65" matrix
    // HUE_LIGHT_CLASS::rgb_to_xy() uses forward - same reference (the
    // matrices are a matched pair). Normalized so the brightest channel
    // always hits 255: this is only ever used to show a light's HUE on
    // screen (light_row()'s color swatch below), and the brightness bar
    // right next to it already carries how bright the light actually is,
    // so there's no need for this to also encode real luminance - the Y
    // (luminance) term cancels out of the final ratio once normalized,
    // which is why it isn't a parameter here.
    RGB xy_to_rgb(double x, double y)
    {
        if (y <= 0.0) y = 0.0001; // guards a degenerate/zero xy
        double z = 1.0 - x - y;
        double X = x / y;
        double Y = 1.0;
        double Z = z / y;

        double r =  X * 1.656492 - Y * 0.354851 - Z * 0.255038;
        double g = -X * 0.707196 + Y * 1.655397 + Z * 0.036152;
        double b =  X * 0.051713 - Y * 0.121364 + Z * 1.011530;

        auto gamma_correct = [](double c) {
            c = std::max(c, 0.0);
            c = (c <= 0.0031308) ? (12.92 * c) : (1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
            return std::clamp(c, 0.0, 1.0);
        };
        r = gamma_correct(r);
        g = gamma_correct(g);
        b = gamma_correct(b);

        double max_channel = std::max({r, g, b, 1e-6});
        return RGB{
            static_cast<int>(std::lround(r / max_channel * 255.0)),
            static_cast<int>(std::lround(g / max_channel * 255.0)),
            static_cast<int>(std::lround(b / max_channel * 255.0))
        };
    }

    // [##########] style, 10 cells, filled left-to-right by percent
    // (0-100, already clamped by bri_to_brightness_percent()'s own caller).
    std::string brightness_bar(int percent)
    {
        constexpr int WIDTH = 10;
        int filled = std::clamp((percent * WIDTH + 50) / 100, 0, WIDTH); // rounded, then clamped
        std::string bar = "[";
        for (int i = 0; i < WIDTH; ++i) bar += (i < filled) ? "\xE2\x96\x88" : "\xE2\x96\x91"; // full block / light shade
        bar += "]";
        return bar;
    }

    // One line of the light list below - id, name, on/off, a brightness
    // bar, and (if this light supports color - a plain white/ambiance bulb
    // has no "xy" at all) an actual colored swatch via 24-bit truecolor,
    // not just a text label.
    std::string light_row(const LightState& state)
    {
        int percent = bri_to_brightness_percent(state.brightness);

        std::stringstream ss;
        ss << " [" << std::setw(3) << std::right << state.id << "] "
           << std::setw(14) << std::left << state.name.substr(0, 14) << " "
           << (state.on ? "ON " : "OFF") << "  "
           << brightness_bar(percent) << " " << std::setw(3) << std::right << percent << "%";

        if (state.xy.size() == 2) {
            RGB c = xy_to_rgb(state.xy[0], state.xy[1]);
            ss << "  \033[38;2;" << c.r << ";" << c.g << ";" << c.b << "m\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\033[0m";
        } else {
            ss << "  (no color data)";
        }

        if (!state.reachable) ss << "  *UNREACHABLE*";
        return ss.str();
    }

    void redraw_screen(const std::string& status)
    {
        std::cout << "\033[H\033[2K" << "Profile: "
                   << (current_profile_name.empty() ? "(shared default)" : current_profile_name) << "\n";
        std::cout << "\033[2K" << "Bridge: " << hue_settings.bridge_ip << " - "
                   << hue.cached_light_count() << " light(s) cached\n";
        std::cout << "\033[2K" << "\n";

        // Sorted numerically for display, not the map's own lexicographic
        // key order (which would put "10" before "2") - purely cosmetic,
        // doesn't touch how lights are actually looked up anywhere else.
        auto lights = hue.get_cached_lights();
        std::vector<LightState> sorted_lights;
        sorted_lights.reserve(lights.size());
        for (auto& [id, state] : lights) sorted_lights.push_back(state);
        std::sort(sorted_lights.begin(), sorted_lights.end(), [](const LightState& a, const LightState& b) {
            return std::atoi(a.id.c_str()) < std::atoi(b.id.c_str());
        });

        if (sorted_lights.empty()) {
            std::cout << "\033[2K" << "  (no lights cached yet)\n";
        } else {
            for (auto const& state : sorted_lights) {
                std::cout << "\033[2K" << light_row(state) << "\n";
            }
        }

        std::cout << "\033[2K" << "\n";
        std::cout << "\033[2K" << status << "\n";
        // Erases anything left over below this point from a previous,
        // taller frame - the light list's height varies (a bridge refresh
        // can add/drop a light), unlike every other line here, so a plain
        // per-line \033[2K alone isn't enough to keep a shrinking frame
        // clean the way it is for tools/clock/clock.cpp's fixed-height
        // display.
        std::cout << "\033[J" << std::flush;
    }

    void print_usage(const char* argv0)
    {
        std::string prog = argv0;
        auto slash = prog.find_last_of('/');
        if (slash != std::string::npos) prog = prog.substr(slash + 1);

        std::cout << "Usage: " << prog << " [host] [-h|--help]\n\n"
                      "  host          IP address of the machine running olli. Defaults to\n"
                      "                127.0.0.1 (olli running on this same machine).\n\n"
                      "  -h, --help    Show this help and exit.\n";
    }
}

int main(int argc, char* argv[])
{
    // ---- user declarations ----
    std::string host = "127.0.0.1";

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        host = arg1;
    }

    in_addr host_addr{};
    if (inet_pton(AF_INET, host.c_str(), &host_addr) != 1) {
        std::cerr << "Not a valid IPv4 address: " << host << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    // libcurl's lazy global init on first curl_easy_init() isn't
    // thread-safe (its own docs are explicit about this) - a real risk
    // here since the flash-cancel path in handle_call() above runs curl
    // from a detached thread, concurrently with whatever the main thread
    // is doing. Same reasoning as olli-core's own main.cpp; done once,
    // here, before anything can spawn.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Shared-default profile until an "identity" message says otherwise -
    // this is also what picks up a real bridge_ip/api_key migrated from
    // olli-core's old settings.json on first run (see load_hue_settings()).
    std::string status = switch_profile("");

    // ---- olli communications declaration ----
    OLLI_LINK link(host, host_addr, make_register_message());

    // ---- user code ----
    RawTerminal raw_terminal;
    std::cout << "\033[2J";

    bool has_real_terminal = isatty(STDIN_FILENO) != 0;
    auto last_light_refresh = std::chrono::steady_clock::now();

    bool quit = false;
    while (!quit) {
        // ---- user code: wait for stdin/socket activity ----
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;
        if (has_real_terminal) {
            FD_SET(STDIN_FILENO, &read_fds);
            max_fd = STDIN_FILENO;
        }
        if (link.fd() >= 0) {
            FD_SET(link.fd(), &read_fds);
            max_fd = std::max(link.fd(), max_fd);
        }

        int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);

        if (ready > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                if (c == 'q' || c == 'Q' || c == 3) quit = true; // 3 = Ctrl+C
            }
        }

        bool socket_readable = link.fd() >= 0 && ready > 0 && FD_ISSET(link.fd(), &read_fds);

        // ---- olli communications ----
        if (!quit) olli_processing(link, socket_readable, status);

        // ---- user code: periodic light-cache refresh, independent of
        // whatever happened above this tick - see LIGHT_REFRESH_INTERVAL_
        // SECONDS' comment. ----
        if (!quit) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_light_refresh).count()
                    >= LIGHT_REFRESH_INTERVAL_SECONDS) {
                last_light_refresh = now;
                hue.refresh_lights();
            }
        }

        if (!quit) redraw_screen(status);
    }

    // ---- closing code ----
    curl_global_cleanup();
    return 0;
}
