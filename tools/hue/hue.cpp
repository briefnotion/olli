// Hue remote tool - Philips Hue light control, ported out of olli's core
// (formerly TOOL_HUE, source/tools.cpp/.h + HUE_LIGHT_CLASS,
// source/tools_helper.h/.cpp) onto the tools/template/ plumbing. See
// ../PROTOCOL.md for the wire protocol.
//
// Registers set_hue_light/list_hue_lights/manage_hue_scenes with the same
// names as the original TOOL_HUE (tools/presence/presence.cpp's
// on_home_action/on_away_action reference these by name, so nothing there
// needed to change for this port). 2026-10-30 added to them, without
// changing anything that already worked: set_hue_light takes several lights
// ("Computer, Sink"), "all" with an "except", brightness_change, warm/cool
// white (white / kelvin), and falls back to a saved default fade;
// list_hue_lights reports color/white, effect, light type, which saved scene
// the lights match and the default fade (detail "full" adds the bridge, its
// rooms and the Hue app's scenes); and a fourth tool, set_hue_default_fade,
// sets that default. Scene loads use the default fade too.
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
#include "../olli_display/olli_display.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
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
        // Added 2026-10-30 so other parts of olli (the main chat, subcon) get
        // the fuller picture list_hue_lights reports, not just on/off and
        // brightness. All straight from the bridge's /lights payload.
        std::string colormode;     // "xy", "ct", "hs", or "" for a light with no color support
        int ct = 0;                // color temperature in mireds (153 = cool/6500K .. 500 = warm/2000K); 0 = not reported
        int hue = -1, sat = -1;    // Hue's hue/saturation form of the color; -1 = not reported
        std::string effect;        // "none" or "colorloop"
        std::string alert;         // "none", "select", "lselect"
        std::string mode;          // bridge's "mode" (e.g. "homeautomation")
        std::string type;          // e.g. "Extended color light", "Color temperature light", "Dimmable light"
        std::string modelid, productname, manufacturername, swversion;
    };

    // The scene the lights are in, and what was last loaded: see
    // HUE_LIGHT_CLASS::scene_status() below.
    struct LastScene {
        std::string name;
        long long at_unix = 0;
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

    std::string format_seconds(double seconds)
    {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", seconds);
        return buf;
    }

    // Color temperature. Hue's "ct" is in mireds (1,000,000 / kelvin); the
    // range a bridge accepts is 153 (6500K, cool) to 500 (2000K, warm).
    constexpr int CT_MIN_MIRED = 153;
    constexpr int CT_MAX_MIRED = 500;

    int kelvin_to_mired(int kelvin)
    {
        if (kelvin < 1) kelvin = 1;
        int mired = static_cast<int>(std::lround(1000000.0 / kelvin));
        return std::clamp(mired, CT_MIN_MIRED, CT_MAX_MIRED);
    }

    int mired_to_kelvin(int mired)
    {
        if (mired < 1) mired = 1;
        return static_cast<int>(std::lround(1000000.0 / mired));
    }

    // Named whites the model can ask for -> kelvin.
    const std::map<std::string, int>& white_presets()
    {
        static const std::map<std::string, int> presets = {
            {"candle", 2000}, {"warm", 2700}, {"neutral", 3500}, {"cool", 4500}, {"daylight", 6500}
        };
        return presets;
    }

    std::string describe_kelvin(int kelvin)
    {
        const char* word = kelvin < 2400 ? "candle" : kelvin < 3200 ? "warm white" : kelvin < 4200 ? "neutral white"
                         : kelvin < 5500 ? "cool white" : "daylight";
        return std::to_string(kelvin) + "K " + word;
    }

    // Named colors for set_hue_light's "preset" and for naming what a light
    // is showing in list_hue_lights (nearest one) - one table so the two
    // can't drift apart.
    const std::map<std::string, std::vector<double>>& color_palette()
    {
        static const std::map<std::string, std::vector<double>> palette = {
            {"red", {0.675, 0.322}}, {"green", {0.409, 0.518}}, {"blue", {0.167, 0.04}},
            {"yellow", {0.432, 0.500}}, {"cyan", {0.157, 0.357}}, {"magenta", {0.41, 0.17}},
            {"orange", {0.55, 0.41}}, {"purple", {0.27, 0.13}}, {"pink", {0.41, 0.21}},
            {"white", {0.31, 0.32}}
        };
        return palette;
    }

    // Defined further down next to the other color code; list_hue_lights
    // (handle_call) needs it first.
    std::string describe_light_color(const LightState& s);

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
    // Same convention, same wording, as source/tools.cpp's TOOL_WEB_SEARCH::
    // handle_tool() failure_instruction - integrate_tool_result()'s default
    // framing ("report this real result... without changing the facts")
    // reads the same whether a call succeeded or not, so a plain bridge
    // error used to risk getting relayed as if it were a normal answer.
    // Passed as send_result()'s own special_instruction (../PROTOCOL.md's
    // `result` message shape) at every call site here that reports a
    // genuine connectivity failure (not a bad argument from the model,
    // which is already unambiguous on its own) - see handle_call()'s
    // list_hue_lights/set_hue_light branches.
    const std::string failure_instruction =
        "This attempt did NOT succeed - it is an error, not real data. Tell "
        "the user plainly, in your own words, that it failed and what "
        "likely went wrong. Do not present this as a real answer to their "
        "question.";

    bool response_is_error(const std::string& response, std::string& detail)
    {
        try {
            json data = json::parse(response);
            if (data.is_object() && data.contains("error")) {
                detail = data["error"].is_string() ? data["error"].get<std::string>() : data["error"].dump();
                return true;
            }
            if (data.is_array()) {
                // Any entry, not just the first: a request with several fields
                // gets one answer per field, and a rejected field (a color sent
                // to a white-only lamp) must not hide behind an accepted "on".
                for (auto const& entry : data) {
                    if (entry.is_object() && entry.contains("error")) {
                        detail = entry["error"].dump();
                        return true;
                    }
                }
            }
            return false;
        } catch (...) {
            detail = "unreadable response from bridge";
            return true;
        }
    }

    // The bridge's own error object, in words a model will relay properly:
    // type 6 = the light does not have that feature (color on a white lamp),
    // 201 = the light is off. Anything else is passed through as it came.
    std::string friendly_bridge_error(const std::string& detail)
    {
        try {
            json e = json::parse(detail);
            if (!e.is_object()) return detail;
            int type = e.value("type", 0);
            std::string address = e.value("address", "");
            std::string param = address.substr(address.find_last_of('/') == std::string::npos ? 0 : address.find_last_of('/') + 1);
            if (type == 6) {
                std::string what = param == "xy" || param == "hue" || param == "sat" ? "color"
                                 : param == "ct" ? "color temperature (warm/cool white)"
                                 : param == "effect" ? "effects" : param;
                return "this light does not support " + what + " (it can only be turned on/off and dimmed)";
            }
            if (type == 201) return "the light is off, so that setting can't be changed";
            return detail;
        } catch (...) {
            return detail;
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

            // Timeout/retry shape matches source/tools.cpp's TOOL_WEB_SEARCH::
            // curl_get() - a real, live-caught bug there too (curl's own
            // timeout firing on an ordinary transient hiccup, reported back
            // as a flat failure with no second chance). Only a genuine
            // timeout gets retried once - a bad URL/connection-refused/
            // host-not-found will just fail identically again, so retrying
            // those doubles the wait for nothing. 8s overall (was a flat 5s
            // for everything, including a group "all lights" command, which
            // can take the bridge a moment longer than a single light) plus
            // its own separate, short connect-timeout - the bridge is a
            // local LAN device, so a connection that hasn't opened within a
            // few seconds is genuinely unreachable, not just slow to
            // respond, and shouldn't eat the whole request budget finding
            // that out.
            static constexpr long REQUEST_TIMEOUT_SECONDS = 8L;
            static constexpr long CONNECT_TIMEOUT_SECONDS = 3L;

            std::string make_request(const std::string& method, const std::string& endpoint, const std::string& body = "")
            {
                for (int attempt = 0; attempt < 2; ++attempt) {
                    CURL* curl = curl_easy_init();
                    std::string read_buffer;
                    CURLcode res = CURLE_FAILED_INIT;
                    if (curl) {
                        std::string url = "http://" + bridge_ip + "/api/" + api_key + endpoint;
                        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
                        curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT_SECONDS);
                        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, CONNECT_TIMEOUT_SECONDS);

                        if (!body.empty()) {
                            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
                        }

                        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
                        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &read_buffer);

                        res = curl_easy_perform(curl);
                        curl_easy_cleanup(curl);
                    }

                    if (res == CURLE_OK) return read_buffer;
                    if (res == CURLE_OPERATION_TIMEDOUT && attempt == 0) continue;

                    return "{\"error\": \"CURL failed: " + std::string(curl_easy_strerror(res)) + "\"}";
                }
                return "{\"error\": \"CURL failed: unreachable\"}";
            }

            bool refresh_lights()
            {
                std::string response = make_request("GET", "/lights");
                try {
                    json data = json::parse(response);
                    if (data.is_array() && !data.empty() && data[0].contains("error")) return false;
                    if (data.is_object() && data.contains("error")) return false;

                    std::map<std::string, LightState> new_cache;
                    std::map<std::string, std::string> new_name_to_id;

                    for (auto& [id, info] : data.items()) {
                        std::string name = info.value("name", "Unknown");
                        LightState state;
                        state.id = id;
                        state.name = name;

                        if (info.contains("state")) {
                            const json& st = info["state"];
                            state.on = st.value("on", false);
                            state.brightness = st.value("bri", 0);
                            state.reachable = st.value("reachable", false);
                            if (st.contains("xy")) {
                                state.xy = st["xy"].get<std::vector<double>>();
                            }
                            state.colormode = st.value("colormode", "");
                            state.ct = st.value("ct", 0);
                            state.hue = st.value("hue", -1);
                            state.sat = st.value("sat", -1);
                            state.effect = st.value("effect", "");
                            state.alert = st.value("alert", "");
                            state.mode = st.value("mode", "");
                        }
                        state.type = info.value("type", "");
                        state.modelid = info.value("modelid", "");
                        state.productname = info.value("productname", "");
                        state.manufacturername = info.value("manufacturername", "");
                        state.swversion = info.value("swversion", "");

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

            // Returns {ok, message} - ok is specifically "did a genuine
            // bridge/connectivity problem happen", not a catch-all for
            // every non-ideal outcome. A clear, already-unambiguous
            // informational result (scene already exists, scene not
            // found) is still ok=true - it's not raw/ambiguous technical
            // noise a model could mistake for real success data the way
            // a bare bridge error could, so it doesn't need the same
            // explicit "this is not real data" framing handle_call()
            // attaches on ok=false (see manage_hue_scenes below).
            std::pair<bool, std::string> save_scene(const std::string& name)
            {
                std::string lower_name = to_lower(name);
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    if (local_scenes.count(lower_name)) {
                        return {true, "Scene '" + name + "' already exists. Please remove it first or use a different name."};
                    }
                }

                // Not held across refresh_lights() - it takes state_mutex
                // itself, and this is a single, non-recursive mutex.
                if (!refresh_lights()) return {false, "Failed to refresh lights for scene capture."};

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
                return {true, "Scene '" + name + "' saved to disk."};
            }

            std::pair<bool, std::string> load_scene(const std::string& name)
            {
                std::string lower_name = to_lower(name);
                std::map<std::string, json> states;
                std::string stored_name;
                int fade_ms = -1;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    auto it = local_scenes.find(lower_name);
                    if (it == local_scenes.end()) return {true, "Scene '" + name + "' not found."};
                    states = it->second.light_states;
                    stored_name = it->second.name;
                    fade_ms = default_transition_ms;
                }

                int failed = 0;
                std::string last_detail;
                for (auto const& [id, state_json] : states) {
                    json body = state_json;
                    // A light the scene wants OFF gets just that. Sending its
                    // saved brightness/color along with on:false is rejected by
                    // the bridge ("device is set to off") whenever the light is
                    // already off, which made loading an all-off scene report a
                    // partial failure.
                    if (!body.value("on", true)) body = json{{"on", false}};
                    // The saved default fade applies to scene loads too, so
                    // "load the evening scene" fades the same way "turn on
                    // all the lights" does. Unset (-1) leaves the bridge's
                    // own default.
                    if (fade_ms >= 0) body["transitiontime"] = std::clamp(fade_ms / 100, 0, 65535);
                    std::string res = make_request("PUT", "/lights/" + id + "/state", body.dump());
                    if (response_is_error(res, last_detail)) ++failed;
                }

                if (failed == 0) {
                    note_scene_loaded(stored_name.empty() ? name : stored_name);
                    return {true, "Scene '" + name + "' activated."};
                }
                if (static_cast<size_t>(failed) == states.size()) {
                    return {false, "Error: Scene '" + name + "' could not be activated - bridge unreachable or rejected every light (" + last_detail + ")."};
                }
                return {false, "Scene '" + name + "' partially activated - " + std::to_string(failed) + " of "
                       + std::to_string(states.size()) + " light(s) failed (" + last_detail + ")."};
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

            // --- Global fade (2026-10-30) ---------------------------------
            // -1 = not set: the bridge's own default applies (about 0.4 s).
            // 0 = instant. Otherwise milliseconds. Persisted by the caller
            // (hue_settings.json); this class only holds the live value.
            void set_default_transition_ms(int ms)
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                default_transition_ms = ms;
            }

            int get_default_transition_ms()
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                return default_transition_ms;
            }

            // --- Which scene the lights are in (2026-10-30) ----------------
            // Two separate facts, reported together: which saved scene the
            // lights match RIGHT NOW (works however they got that way - a
            // scene load, the Hue app, the wall switch), and which scene
            // olli last loaded (remembered across restarts in
            // hue_state.json). Only local scenes (manage_hue_scenes) are
            // known here: the v1 bridge API doesn't say which of the Hue
            // app's own scenes is active.
            void set_state_file(const std::string& path)
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                state_filepath = path;
                last_scene = LastScene{};
                if (state_filepath.empty()) return;
                try {
                    std::ifstream file(state_filepath);
                    if (file.is_open()) {
                        json j;
                        file >> j;
                        if (j.contains("last_scene") && j["last_scene"].is_object()) {
                            last_scene.name = j["last_scene"].value("name", "");
                            last_scene.at_unix = j["last_scene"].value("at_unix", 0LL);
                        }
                    }
                } catch (...) {
                    // Missing or unreadable: no remembered scene, which is fine.
                }
            }

            void note_scene_loaded(const std::string& name)
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                last_scene.name = name;
                last_scene.at_unix = static_cast<long long>(std::time(nullptr));
                if (state_filepath.empty()) return;
                try {
                    std::ofstream file(state_filepath);
                    if (file.is_open()) {
                        file << json{{"last_scene", {{"name", last_scene.name}, {"at_unix", last_scene.at_unix}}}}.dump(4);
                    }
                } catch (...) {
                    // Same as save_scenes_to_disk(): remembering is best-effort.
                }
            }

            // One short paragraph for list_hue_lights. Uses the cache as it
            // stands (the caller has just refreshed it).
            std::string scene_status()
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (local_scenes.empty()) return "Scenes: none saved.";

                std::vector<std::string> matching;
                for (auto const& [key, scene] : local_scenes)
                    if (scene_matches_lights(scene)) matching.push_back(scene.name);

                std::string out;
                if (!matching.empty()) {
                    out = "Scene: the lights currently match the saved scene ";
                    for (size_t i = 0; i < matching.size(); ++i) out += (i ? ", " : "") + std::string("'") + matching[i] + "'";
                    out += ".";
                    if (!last_scene.name.empty() && std::find(matching.begin(), matching.end(), last_scene.name) == matching.end())
                        out += " (Last loaded by olli: '" + last_scene.name + "', " + age_text(last_scene.at_unix) + ".)";
                } else if (!last_scene.name.empty()) {
                    out = "Scene: no saved scene matches the lights now. Last loaded by olli: '" + last_scene.name + "', "
                        + age_text(last_scene.at_unix) + "; the lights have been changed since.";
                } else {
                    out = "Scene: no saved scene matches the lights now, and none has been loaded by olli.";
                }
                return out;
            }

            // Everything below is for set_hue_light's multi-light targeting.

            struct TARGETS {
                std::vector<std::string> ids;   // resolved bridge light IDs
                bool whole_group = false;       // plain "all": send to group 0 in one request
                std::string error;              // non-empty = could not resolve; nothing should be sent
            };

            // "Computer, Sink", "Computer and Sink", "2,3", "all", or "all" with
            // 'except' naming lights to leave alone. Unknown names are an error
            // (listing the real ones) rather than being silently skipped.
            TARGETS resolve_targets(const std::string& target, const std::string& except_list, bool force_individual = false)
            {
                TARGETS out;
                std::vector<std::string> wanted, excluded;
                {
                    std::lock_guard<std::mutex> lock(state_mutex);
                    wanted = split_light_list(target, name_to_id);
                    excluded = split_light_list(except_list, name_to_id);
                }
                if (wanted.empty()) { out.error = "No light given."; return out; }

                bool has_all = false;
                for (auto const& w : wanted) if (to_lower(w) == "all") has_all = true;

                if (has_all && excluded.empty() && !force_individual) {
                    out.whole_group = (wanted.size() == 1);
                    if (out.whole_group) return out;
                }

                std::lock_guard<std::mutex> lock(state_mutex);
                auto lookup = [&](const std::string& word, std::string& id) {
                    if (std::all_of(word.begin(), word.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) { id = word; return lights_cache.count(id) > 0; }
                    auto it = name_to_id.find(to_lower(word));
                    if (it == name_to_id.end()) return false;
                    id = it->second;
                    return true;
                };

                std::vector<std::string> unknown;
                std::vector<std::string> ids;
                for (auto const& w : wanted) {
                    if (to_lower(w) == "all") { for (auto const& [id, st] : lights_cache) ids.push_back(id); continue; }
                    std::string id;
                    if (lookup(w, id)) ids.push_back(id); else unknown.push_back(w);
                }
                std::vector<std::string> skip;
                for (auto const& w : excluded) {
                    std::string id;
                    if (lookup(w, id)) skip.push_back(id); else unknown.push_back(w);
                }

                if (!unknown.empty()) {
                    out.error = "Light(s) not found:";
                    for (auto const& u : unknown) out.error += " '" + u + "'";
                    out.error += ". Known lights:";
                    for (auto const& [id, st] : lights_cache) out.error += " [" + id + "] " + st.name + ";";
                    return out;
                }

                for (auto const& id : ids) {
                    if (std::find(skip.begin(), skip.end(), id) != skip.end()) continue;
                    if (std::find(out.ids.begin(), out.ids.end(), id) != out.ids.end()) continue;
                    out.ids.push_back(id);
                }
                if (out.ids.empty()) out.error = "No lights left to change after the exclusions.";
                return out;
            }

            // The light's cached name, for messages ("" if unknown).
            std::string light_name(const std::string& id)
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                auto it = lights_cache.find(id);
                return it == lights_cache.end() ? "" : it->second.name;
            }

            bool light_is_on(const std::string& id)
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                auto it = lights_cache.find(id);
                return it != lights_cache.end() && it->second.on;
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

            // "A, B", "A and B", "A & B", "A; B" -> {"A","B"}; trims, drops empties.
            // Commas, semicolons and & always separate. A piece that is exactly
            // a known light's name (or an ID, or "all") is kept whole, so a
            // light called "Lamp and Shade" is still addressable; only a piece
            // that is not one gets split further on " and ".
            static std::vector<std::string> split_light_list(const std::string& input, const std::map<std::string, std::string>& known_names)
            {
                auto trim = [](const std::string& t) {
                    size_t b = t.find_first_not_of(" \t");
                    size_t e = t.find_last_not_of(" \t");
                    return b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
                };
                std::vector<std::string> pieces;
                std::string cur;
                for (char c : input) {
                    if (c == ',' || c == ';' || c == '&') { pieces.push_back(trim(cur)); cur.clear(); } else cur += c;
                }
                pieces.push_back(trim(cur));

                std::vector<std::string> out;
                for (auto const& piece : pieces) {
                    if (piece.empty()) continue;
                    bool is_id = std::all_of(piece.begin(), piece.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
                    if (is_id || to_lower(piece) == "all" || known_names.count(to_lower(piece))) { out.push_back(piece); continue; }
                    std::string rest = piece;
                    for (;;) {
                        size_t at = std::string::npos;
                        for (size_t pos = 0; pos + 5 <= rest.size(); ++pos)
                            if (to_lower(rest.substr(pos, 5)) == " and ") { at = pos; break; }
                        if (at == std::string::npos) break;
                        std::string head = trim(rest.substr(0, at));
                        if (!head.empty()) out.push_back(head);
                        rest = rest.substr(at + 5);
                    }
                    rest = trim(rest);
                    if (!rest.empty()) out.push_back(rest);
                }
                return out;
            }

            static std::string age_text(long long at_unix)
            {
                if (at_unix <= 0) return "some time ago";
                long long secs = static_cast<long long>(std::time(nullptr)) - at_unix;
                if (secs < 90) return "just now";
                if (secs < 90 * 60) return std::to_string((secs + 30) / 60) + " minutes ago";
                if (secs < 36 * 3600) return std::to_string((secs + 1800) / 3600) + " hours ago";
                return std::to_string((secs + 43200) / 86400) + " days ago";
            }

            // Does every light in the scene look like it did when the scene
            // was saved? Brightness and color get a little slack: the bridge
            // rounds both, and a fade may not have fully finished. Only
            // lights named in the scene count. Assumes state_mutex is held.
            bool scene_matches_lights(const HUE_SCENE& scene) const
            {
                if (scene.light_states.empty()) return false;
                for (auto const& [id, saved] : scene.light_states) {
                    auto it = lights_cache.find(id);
                    if (it == lights_cache.end() || !it->second.reachable) return false;
                    const LightState& live = it->second;
                    bool saved_on = saved.value("on", false);
                    if (live.on != saved_on) return false;
                    if (!saved_on) continue;
                    if (saved.contains("bri") && std::abs(live.brightness - saved.value("bri", 0)) > 10) return false;
                    if (saved.contains("xy") && saved["xy"].is_array() && saved["xy"].size() == 2 && live.xy.size() == 2) {
                        if (std::abs(live.xy[0] - saved["xy"][0].get<double>()) > 0.03) return false;
                        if (std::abs(live.xy[1] - saved["xy"][1].get<double>()) > 0.03) return false;
                    }
                }
                return true;
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
            int default_transition_ms = -1;   // see set_default_transition_ms()
            std::string state_filepath;       // hue_state.json; empty = don't remember
            LastScene last_scene;
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
        // Global fade for every light change that doesn't say otherwise
        // (2026-10-30). -1 = not set (the bridge's own ~0.4 s), 0 = instant.
        // Set by asking olli (set_hue_default_fade) or by editing this file.
        int default_transition_ms = -1;
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

    // Which scene olli last loaded - runtime state, kept apart from the
    // hand-edited hue_settings.json and from scenes.json (whose format stays
    // exactly as it always was).
    fs::path state_path_for(const std::string& profile_name)
    {
        return profile_dir_for(profile_name) / "hue_state.json";
    }

    void save_hue_settings(const fs::path& path, const HueSettings& s)
    {
        json j;
        j["bridge_ip"] = s.bridge_ip;
        j["api_key"] = s.api_key;
        j["default_transition_ms"] = s.default_transition_ms;
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
                s.default_transition_ms = j.value("default_transition_ms", s.default_transition_ms);
                if (s.default_transition_ms < -1) s.default_transition_ms = -1;
                if (s.default_transition_ms > 600000) s.default_transition_ms = 600000;
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
    // light cache.
    //
    // Doesn't return a status string for the shared tool-status line -
    // OLLI_DISPLAY already has a dedicated, always-current Profile line
    // (see main()'s display.set_profile(current_profile_name...) call, run
    // every tick) and draw_light_list() above shows live bridge/light-count
    // info as part of the tool area on every tick regardless - a one-shot
    // status here would just repeat both of those and blot out whatever
    // real tool activity was on the status line beforehand. Same reasoning
    // as ../clock/clock.cpp's and ../presence/presence.cpp's
    // handle_identity().
    void switch_profile(const std::string& profile_name)
    {
        current_profile_name = profile_name;
        hue_settings = load_hue_settings(profile_name);
        hue.set_credentials(hue_settings.bridge_ip, hue_settings.api_key, scenes_path_for(profile_name).string());
        hue.set_default_transition_ms(hue_settings.default_transition_ms);
        hue.set_state_file(state_path_for(profile_name).string());
        hue.refresh_lights();
    }

    void handle_identity(const json& msg)
    {
        switch_profile(msg.value("name", ""));
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
                {"light_id", {{"type", "string"}, {"description", "One light by ID or name (e.g. '2' or 'Computer'), several separated by commas or 'and' (e.g. 'Computer, Sink'), or 'all' for every light."}}},
                {"except", {{"type", "string"}, {"description", "Only with light_id 'all': lights to leave alone, by name or ID (e.g. 'Sofa' for 'all the lights except the sofa')."}}},
                {"on", {{"type", "boolean"}}},
                // 0-100 percentage - converted to Hue's native 0-254 "bri"
                // scale in handle_call() (brightness_percent_to_bri()).
                {"brightness", {{"type", "integer"}, {"description", "Set brightness to this exact percentage, 0 (dimmest) to 100 (maximum)."}}},
                {"brightness_change", {{"type", "integer"}, {"description", "Make the light this many percentage points brighter (positive) or dimmer (negative), -100 to 100, e.g. -20 for 'a bit dimmer', 25 for 'brighter'. Only changes lights that are already on. Do not combine with brightness."}}},
                {"preset", {{"type", "string"}, {"enum", {"red", "green", "blue", "yellow", "magenta", "cyan", "orange", "purple", "pink", "white"}}}},
                {"hex", {{"type", "string"}}},
                {"white", {{"type", "string"}, {"enum", {"candle", "warm", "neutral", "cool", "daylight"}}, {"description", "A true white by warmth: candle (2000K), warm (2700K), neutral (3500K), cool (4500K), daylight (6500K). Use this, not a color preset, for warm or cool white light. Ignored if a color is also given."}}},
                {"kelvin", {{"type", "integer"}, {"description", "A true white at an exact color temperature, 2000 (warm) to 6500 (cool). Ignored if a color or white is also given."}}},
                {"alert", {{"type", "string"}, {"enum", {"none", "select", "lselect"}}}},
                {"flash_count", {{"type", "integer"}}},
                {"transition_ms", {{"type", "integer"}, {"description", "How long the change takes to fade in, in milliseconds (10000 = 10 seconds). 0 means instant - use it when the user says 'right now' or 'immediately'. Leave it out to use the saved default fade."}}}
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

        json list_params = {
            {"type", "object"},
            {"properties", {
                {"detail", {{"type", "string"}, {"enum", {"summary", "full"}}, {"description", "'summary' (default) is enough for almost every question. 'full' also lists light models and firmware, the bridge's rooms/groups, the Hue app's own scenes, and bridge info."}}}
            }}
        };

        json fade_params = {
            {"type", "object"},
            {"properties", {
                {"seconds", {{"type", "number"}, {"description", "Default fade time in seconds, 0 for instant (e.g. 10 for 'make fades 10 seconds')."}}},
                {"reset", {{"type", "boolean"}, {"description", "True to go back to the bridge's own quick default instead of a set time."}}}
            }}
        };

        return {
            {"type", "register"},
            {"tools", json::array({
                {
                    {"name", "set_hue_light"},
                    {"description", "Controls Hue lights by ID or Name: one light, several (comma-separated), or all. Use this for general commands like 'turn off all lights' by setting light_id to 'all'. Also does dimming or brightening by an amount, warm or cool white, colors, and fade times. Always execute this tool call for every request, even if you believe the state is already set."},
                    {"parameters", set_params}
                },
                {
                    {"name", "list_hue_lights"},
                    {"description", "Returns status of all connected lights: on/off, brightness, color or warm/cool white, any effect, which saved scene (if any) the lights currently match, and the saved default fade. Always execute this tool call for every request, even if you believe you already know the state - never answer a status question (on/off, brightness, color, scene) from a value stated earlier in the conversation."},
                    {"parameters", list_params}
                },
                {
                    {"name", "manage_hue_scenes"},
                    {"description", "Saves, loads, or removes local light scenes. Only use for specific named snapshots (e.g., 'home', 'away')."},
                    {"parameters", scene_params}
                },
                {
                    {"name", "set_hue_default_fade"},
                    {"description", "Sets the default fade time for light changes (turning lights on or off, brightness, color, loading a scene) that don't say how long to take - e.g. 'make fades 10 seconds' is seconds 10, and 0 is instant. It is saved and stays until changed. A single request can still override it ('right now' means instant for that request only)."},
                    {"parameters", fade_params}
                }
            })}
        };
    }

    // A light name argument as the model may actually send it: a string
    // ("Computer, Sink"), or a list (["Computer", "Sink"], [2, 3]) which is
    // joined with commas. Anything else reads as empty. nlohmann's value()
    // throws on a wrongly typed field, which took the whole tool down.
    std::string light_list_arg(const json& args, const char* key)
    {
        if (!args.is_object() || !args.contains(key)) return "";
        const json& v = args[key];
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number_integer()) return std::to_string(v.get<long long>());
        if (v.is_array()) {
            std::string out;
            for (auto const& item : v) {
                std::string one = item.is_string() ? item.get<std::string>() : item.is_number_integer() ? std::to_string(item.get<long long>()) : "";
                if (one.empty()) continue;
                out += (out.empty() ? "" : ",") + one;
            }
            return out;
        }
        return "";
    }

    std::string handle_call(OLLI_LINK& link, const json& msg)
    {
        std::string call_id = msg.value("call_id", "");
        std::string name = msg.value("name", "");
        json args = msg.value("arguments", json::object());

        if (name == "list_hue_lights") {
            if (!hue.refresh_lights()) {
                std::string err = "Error: Could not reach the Hue Bridge.";
                link.send_result(call_id, err, failure_instruction);
                return err;
            }
            bool full = args.is_object() && args.contains("detail") && args["detail"].is_string() && args["detail"].get<std::string>() == "full";
            auto lights = hue.get_cached_lights();
            std::stringstream ss;
            ss << "Current Lights (" << lights.size() << "):\n";
            for (auto const& [id, state] : lights) {
                ss << "[" << id << "] " << state.name << ": ";
                if (!state.reachable) ss << "*UNREACHABLE* (last known: ";
                if (state.on) ss << "ON " << bri_to_brightness_percent(state.brightness) << "%, " << describe_light_color(state);
                else ss << "OFF";
                if (!state.effect.empty() && state.effect != "none") ss << ", effect: " << state.effect;
                // "select" is a one-off flash the bridge keeps reporting long after it ended; only the
                // 15-second "lselect" is worth mentioning, and only while it may still be running.
                if (state.alert == "lselect") ss << ", alert: flashing";
                if (!state.reachable) ss << ")";
                if (!state.type.empty()) ss << " [" << state.type << "]";
                ss << "\n";
                if (full) {
                    ss << "    " << (state.manufacturername.empty() ? "" : state.manufacturername + " ")
                       << (state.productname.empty() ? state.modelid : state.productname)
                       << (state.modelid.empty() ? "" : " (model " + state.modelid + ")")
                       << (state.swversion.empty() ? "" : ", firmware " + state.swversion)
                       << (state.mode.empty() ? "" : ", mode " + state.mode);
                    if (!state.colormode.empty()) ss << ", colormode " << state.colormode;
                    if (state.hue >= 0 && state.sat >= 0) ss << ", hue " << state.hue << "/sat " << state.sat;
                    ss << "\n";
                }
            }
            ss << hue.scene_status() << "\n";
            int fade_ms = hue.get_default_transition_ms();
            if (fade_ms < 0) ss << "Default fade: not set (the bridge's own quick default, about 0.4 seconds).\n";
            else if (fade_ms == 0) ss << "Default fade: instant.\n";
            else ss << "Default fade: " << format_seconds(fade_ms / 1000.0) << " seconds.\n";
            auto scenes = hue.get_scenes();
            if (!scenes.empty()) {
                ss << "Saved scenes:";
                for (auto const& [key, scene] : scenes) ss << " " << scene.name << ";";
                ss << "\n";
            }

            if (full) {
                // Bridge-level facts. Three extra requests, only when asked.
                try {
                    json cfg = json::parse(hue.make_request("GET", "/config"));
                    if (cfg.is_object() && cfg.contains("name")) {
                        ss << "Bridge: " << cfg.value("name", "") << ", API " << cfg.value("apiversion", "?")
                           << ", software " << cfg.value("swversion", "?") << ", zigbee channel " << cfg.value("zigbeechannel", 0)
                           << ", model " << cfg.value("modelid", "?") << "\n";
                    }
                } catch (...) { ss << "Bridge: (could not read its settings)\n"; }
                try {
                    json groups = json::parse(hue.make_request("GET", "/groups"));
                    if (groups.is_object() && !groups.empty() && !groups.contains("error")) {
                        ss << "Rooms and groups:";
                        for (auto& [gid, g] : groups.items()) {
                            if (!g.is_object()) continue;
                            ss << " " << g.value("name", "?") << " (" << g.value("type", "?") << ", ";
                            bool first = true;
                            if (g.contains("lights") && g["lights"].is_array())
                                for (auto& l : g["lights"]) { ss << (first ? "lights " : ",") << (l.is_string() ? l.get<std::string>() : l.dump()); first = false; }
                            if (first) ss << "no lights";
                            bool any_on = g.contains("state") && g["state"].is_object() && g["state"].value("any_on", false);
                            ss << ", " << (any_on ? "some on" : "all off") << ");";
                        }
                        ss << "\n";
                    }
                } catch (...) { ss << "Rooms and groups: (could not read them)\n"; }
                try {
                    json hs = json::parse(hue.make_request("GET", "/scenes"));
                    if (hs.is_object() && !hs.empty() && !hs.contains("error")) {
                        ss << "Hue app scenes (names only; which one is active isn't available):";
                        int shown = 0;
                        for (auto& [sid, sc] : hs.items()) {
                            if (!sc.is_object()) continue;
                            if (++shown > 30) { ss << " ...and more;"; break; }
                            ss << " " << sc.value("name", "?") << ";";
                        }
                        ss << "\n";
                    }
                } catch (...) { ss << "Hue app scenes: (could not read them)\n"; }
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
            bool ok = true;
            if (action == "save") std::tie(ok, res) = hue.save_scene(scene_name);
            else if (action == "load") {
                std::tie(ok, res) = hue.load_scene(scene_name);
                // load_scene() only PUTs new state to the bridge, it
                // doesn't re-read it back - without this, the cached
                // lights_cache (what the live display and list_hue_lights
                // both read) would keep showing whatever it was before
                // the scene loaded until the next periodic refresh, up to
                // LIGHT_REFRESH_INTERVAL_SECONDS later.
                hue.refresh_lights();
            }
            // remove_scene() stays a plain string - purely local (a map
            // erase + a disk write), no bridge/network call at all, so
            // there's no connectivity failure it could ever report.
            else if (action == "remove") res = hue.remove_scene(scene_name);
            else { ok = false; res = "Error: Unknown scene action '" + action + "'"; }

            // Same "this is not real data" framing set_hue_light/
            // list_hue_lights already use (failure_instruction, above) -
            // save/load can genuinely fail against the bridge same as
            // those do (save_scene()'s own refresh_lights() call,
            // load_scene()'s own per-light PUTs), just used to report it
            // as a plain string with no way for handle_call() to tell
            // that apart from an ordinary success.
            link.send_result(call_id, res, ok ? "" : failure_instruction);
            return "Scene " + action + ": " + scene_name;
        }

        if (name == "set_hue_default_fade") {
            int new_ms = -1;
            bool reset = args.is_object() && args.contains("reset") && args["reset"].is_boolean() && args["reset"].get<bool>();
            if (!reset) {
                if (!args.is_object() || !args.contains("seconds") || !args["seconds"].is_number()) {
                    std::string err = "Error: give 'seconds' (0 for instant) or set 'reset' to true.";
                    link.send_result(call_id, err);
                    return err;
                }
                double seconds = args["seconds"].get<double>();
                new_ms = static_cast<int>(std::lround(std::clamp(seconds, 0.0, 600.0) * 1000.0));
            }
            hue_settings.default_transition_ms = new_ms;
            hue.set_default_transition_ms(new_ms);
            save_hue_settings(hue_settings_path_for(current_profile_name), hue_settings);
            std::string text = new_ms < 0 ? "Default fade cleared: lights now use the bridge's own quick default (about 0.4 seconds)."
                             : new_ms == 0 ? "Default fade set to instant. It is saved."
                             : "Default fade set to " + format_seconds(new_ms / 1000.0) + " seconds. It is saved and applies to every light change and scene load that doesn't give its own fade time.";
            link.send_result(call_id, text);
            return "Call answered: set_hue_default_fade";
        }

        if (name == "set_hue_light") {
            std::string target = light_list_arg(args, "light_id");
            std::string except_list = light_list_arg(args, "except");
            json body;
            std::vector<std::string> applied;   // echoed back so the model reports what really happened

            // Numbers from the model are not always the type the schema asked
            // for (a 20.0 where an integer was wanted, a string). Read them
            // without throwing.
            auto read_number = [&](const char* key, double& out) {
                if (!args.is_object() || !args.contains(key) || !args[key].is_number()) return false;
                out = args[key].get<double>();
                return true;
            };

            double brightness_in = 0, change_in = 0, kelvin_in = 0, transition_in = 0, flash_in = 0;
            bool has_brightness = read_number("brightness", brightness_in);
            bool has_change = !has_brightness && read_number("brightness_change", change_in);
            bool has_kelvin = read_number("kelvin", kelvin_in);
            bool has_transition = read_number("transition_ms", transition_in);
            bool has_flash = read_number("flash_count", flash_in);
            bool has_hex = args.contains("hex") && args["hex"].is_string() && !args["hex"].get<std::string>().empty();
            bool has_preset = args.contains("preset") && args["preset"].is_string();
            std::string white_name = args.contains("white") && args["white"].is_string() ? args["white"].get<std::string>() : "";
            bool has_alert_arg = args.contains("alert") && args["alert"].is_string() && args["alert"].get<std::string>() != "none";

            bool explicit_on = args.contains("on") && args["on"].is_boolean();
            if (explicit_on) {
                body["on"] = args["on"].get<bool>();
                applied.push_back(args["on"].get<bool>() ? "on" : "off");
            } else if (!has_alert_arg && !has_flash) {
                // A bare relative change ("a bit dimmer") must not switch an
                // off light on; anything else that sets a look turns it on, as before.
                bool bare_relative = has_change && !has_hex && !has_preset && white_name.empty() && !has_kelvin;
                if (!bare_relative) { body["on"] = true; applied.push_back("on"); }
            }

            // Model sends 0-100 - see make_register_message()'s "brightness"
            // description and brightness_percent_to_bri()'s own comment.
            if (has_brightness) {
                int pct = static_cast<int>(std::lround(brightness_in));
                body["bri"] = brightness_percent_to_bri(pct);
                applied.push_back("brightness " + std::to_string(std::clamp(pct, 0, 100)) + "%");
            } else if (has_change) {
                int pts = std::clamp(static_cast<int>(std::lround(change_in)), -100, 100);
                body["bri_inc"] = std::clamp(static_cast<int>(std::lround(pts / 100.0 * 254.0)), -254, 254);
                applied.push_back(std::string("brightness ") + (pts >= 0 ? "+" : "") + std::to_string(pts) + " points");
            }

            std::string alert_mode = args.contains("alert") && args["alert"].is_string() ? args["alert"].get<std::string>() : "none";
            int flash_count = 0;
            if (has_flash) {
                flash_count = static_cast<int>(std::lround(flash_in));
                alert_mode = (flash_count > 1) ? "lselect" : "select";
            }
            if (alert_mode != "none") { body["alert"] = alert_mode; applied.push_back("alert " + alert_mode); }

            // Color: hex wins over a named preset, which wins over a white.
            bool color_set = false;
            if (has_hex) {
                std::string hex = args["hex"].get<std::string>();
                if (hex[0] == '#') hex.erase(0, 1);
                unsigned int r, g, b;
                if (std::sscanf(hex.c_str(), "%02x%02x%02x", &r, &g, &b) == 3) {
                    auto [x, y] = HUE_LIGHT_CLASS::rgb_to_xy(static_cast<int>(r), static_cast<int>(g), static_cast<int>(b));
                    body["xy"] = {x, y};
                    color_set = true;
                    applied.push_back("color #" + hex);
                }
            }
            if (!color_set && has_preset) {
                auto it = color_palette().find(args["preset"].get<std::string>());
                if (it != color_palette().end()) { body["xy"] = it->second; color_set = true; applied.push_back("color " + it->first); }
            }
            if (!color_set && (!white_name.empty() || has_kelvin)) {
                int kelvin = 0;
                auto wp = white_presets().find(white_name);
                if (wp != white_presets().end()) kelvin = wp->second;
                else if (has_kelvin) kelvin = static_cast<int>(std::lround(kelvin_in));
                if (kelvin > 0) {
                    body["ct"] = kelvin_to_mired(kelvin);
                    applied.push_back("white " + describe_kelvin(mired_to_kelvin(body["ct"].get<int>())));
                }
            }

            // Fade: this request's own time if it gave one (0 = instant),
            // otherwise the saved default - but only for changes that have a
            // look to fade to, not a bare alert/flash.
            bool changes_look = body.contains("on") || body.contains("bri") || body.contains("bri_inc") || body.contains("xy") || body.contains("ct");
            if (has_transition) {
                int tt = std::clamp(static_cast<int>(std::lround(transition_in / 100.0)), 0, 65535);
                body["transitiontime"] = tt;
                applied.push_back(tt == 0 ? "instant" : "fade " + format_seconds(tt / 10.0) + " s");
            } else if (changes_look) {
                int default_ms = hue.get_default_transition_ms();
                if (default_ms >= 0) {
                    int tt = std::clamp(default_ms / 100, 0, 65535);
                    body["transitiontime"] = tt;
                    applied.push_back(tt == 0 ? "instant (saved default)" : "fade " + format_seconds(tt / 10.0) + " s (saved default)");
                }
            }

            // ---- who gets it
            // A relative change needs each light's own level (and must skip
            // lights that are off), and a power command must not go through
            // group 0 (see below), so "all" is then expanded to individual lights.
            // A power command (on or off) with no brightness of its own is never
            // sent as the bridge's group-0 action. Found 2026-10-30 on the real
            // bridge: a group "off" throws away each light's stored brightness
            // (it drops to 1, or to some other wrong level), and a group "on"
            // can bring lights up at odd levels (120 became 12, 60 became 2), so
            // "turn on all the lights" after "turn off all the lights" came back
            // at the lowest setting. The same commands sent to each light on its
            // own leave its level alone and bring it back at exactly that level.
            // With an explicit brightness the group action is fine (it states the
            // level), and so is a color or white change that doesn't touch power.
            bool power_without_level = body.contains("on") && !body.contains("bri");
            bool per_light = has_change || power_without_level;
            HUE_LIGHT_CLASS::TARGETS targets = hue.resolve_targets(target, except_list, per_light);
            if (!targets.error.empty()) {
                // The cache may simply be stale or empty (bridge was down at startup).
                hue.refresh_lights();
                targets = hue.resolve_targets(target, except_list, per_light);
            }
            if (!targets.error.empty()) {
                std::string err = "Error controlling light '" + target + "': " + targets.error;
                link.send_result(call_id, err);
                return "Error: set_hue_light (" + target + ")";
            }

            if (has_change) hue.refresh_lights();  // current on/off before skipping lights that are off

            std::string res;                         // single / whole-group: the bridge's own answer
            std::vector<std::string> ok_names, skipped_off, failed;
            std::string last_detail;
            if (targets.whole_group) {
                res = hue.set_light("all", body);
                std::string d;
                if (response_is_error(res, d)) failed.push_back("all: " + friendly_bridge_error(d));
                else ok_names.push_back("all lights");
            } else {
                bool bridge_down = false;
                for (auto const& id : targets.ids) {
                    std::string label = hue.light_name(id);
                    if (label.empty()) label = id;
                    if (has_change && !body.contains("on") && !hue.light_is_on(id)) { skipped_off.push_back(label); continue; }
                    if (bridge_down) { failed.push_back(label + ": not attempted, the bridge is not responding"); continue; }
                    res = hue.set_light(id, body);
                    std::string d;
                    if (response_is_error(res, d)) {
                        failed.push_back(label + ": " + friendly_bridge_error(d));
                        // A connection failure or timeout (make_request()'s own "CURL failed")
                        // means the bridge itself isn't answering. Trying every remaining light
                        // would take up to 16 s each and could outlast olli's dead-link timeout
                        // for this tool, so stop after the first.
                        if (d.rfind("CURL failed", 0) == 0) bridge_down = true;
                    } else {
                        ok_names.push_back(label);
                    }
                }
            }

            std::string applied_text;
            for (size_t k = 0; k < applied.size(); ++k) applied_text += (k ? ", " : "") + applied[k];

            if (ok_names.empty() && skipped_off.empty()) {
                std::string detail = failed.empty() ? "nothing was sent" : failed.front();
                std::string err = "Error controlling light '" + target + "': " + detail;
                link.send_result(call_id, err, failure_instruction);
                return "Error: set_hue_light (" + target + ")";
            }

            // set_light() only PUTs the new state to the bridge, it
            // doesn't re-read it back - without this, the live display
            // (and the next list_hue_lights call) would keep showing
            // whatever this light's state was before this command until
            // the next periodic refresh, up to
            // LIGHT_REFRESH_INTERVAL_SECONDS later.
            hue.refresh_lights();

            if (has_flash && flash_count > 1 && !targets.whole_group && targets.ids.size() == 1) {
                std::string resolved_id = targets.ids.front();
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

            std::string summary;
            bool single_or_group = targets.whole_group || (targets.ids.size() == 1 && failed.empty() && skipped_off.empty());
            if (single_or_group) {
                // The bridge's own success response reports "bri" back on its
                // native 0-254 scale, not the 0-100 percentage the model sent -
                // same mismatch bri_to_brightness_percent() already corrects for
                // list_hue_lights' own display. Left as-is here, a model told
                // (via DIRECTOR_NOTE) to report this "without changing the
                // facts/values" would read a requested 30% back as "76%" - seen
                // live. Rewrite every "/bri" leaf in place before it's handed
                // back, same conversion, same rounding.
                try {
                    json res_json = json::parse(res);
                    if (res_json.is_array()) {
                        for (auto& entry : res_json) {
                            if (!entry.is_object() || !entry.contains("success")) continue;
                            json& success = entry["success"];
                            if (!success.is_object()) continue;
                            for (auto& [path, value] : success.items()) {
                                if (path.size() >= 4 && path.compare(path.size() - 4, 4, "/bri") == 0 && value.is_number()) {
                                    value = bri_to_brightness_percent(value.get<int>());
                                }
                            }
                        }
                        res = res_json.dump();
                    }
                } catch (...) {
                    // Malformed/unexpected shape - fall back to the raw
                    // response rather than losing the result entirely.
                }
                summary = "Light command for " + target + " processed. Result: " + res;
            } else {
                auto join = [](const std::vector<std::string>& v) { std::string o; for (size_t k = 0; k < v.size(); ++k) o += (k ? ", " : "") + v[k]; return o; };
                summary = "Light command processed for: " + (ok_names.empty() ? std::string("(none)") : join(ok_names)) + ".";
                if (!skipped_off.empty()) summary += " Not changed because they are off: " + join(skipped_off) + ".";
                if (!failed.empty()) summary += " FAILED: " + join(failed) + ".";
            }
            if (!applied_text.empty()) summary += " Applied: " + applied_text + ".";
            link.send_result(call_id, summary);
            return "Call answered: set_hue_light (" + target + ")";
        }

        link.send_error(call_id, "Unknown tool name: " + name);
        return "Unknown call received: " + name;
    }

    struct RGB { int r = 255, g = 255, b = 255; };

    // The published inverse of the exact "Wide RGB D65" matrix
    // HUE_LIGHT_CLASS::rgb_to_xy() uses forward - same reference (the
    // matrices are a matched pair). Normalized so the brightest channel
    // always hits 255: this is only ever used to show a light's HUE on
    // screen (draw_light_row()'s color swatch below), and the brightness bar
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

    // What a light that is ON is showing, in words: a warm/cool white with its
    // temperature, or a color with its nearest name and hex. A light with no
    // color support says so (it can only be dimmed).
    std::string describe_light_color(const LightState& s)
    {
        if (s.colormode == "ct" && s.ct > 0) return describe_kelvin(mired_to_kelvin(s.ct));
        if ((s.colormode == "xy" || s.colormode == "hs") && s.xy.size() == 2) {
            RGB c = xy_to_rgb(s.xy[0], s.xy[1]);
            char hex[16];
            std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", c.r, c.g, c.b);
            std::string nearest;
            double best = 1e9;
            for (auto const& [pname, pxy] : color_palette()) {
                double d = std::hypot(s.xy[0] - pxy[0], s.xy[1] - pxy[1]);
                if (d < best) { best = d; nearest = pname; }
            }
            return best <= 0.06 ? "color ~" + nearest + " (" + hex + ")" : std::string("color ") + hex;
        }
        return "no color control";
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

    // --- Color swatch ---
    //
    // Raw ANSI truecolor escapes (\033[38;2;r;g;bm...) - what this used to
    // embed directly in a plain std::string - don't work through ncurses:
    // waddstr()/mvwaddstr() don't interpret escape sequences, they just
    // print the ESC byte and whatever follows as literal (non-printing)
    // characters, corrupting ncurses' own idea of where the cursor is.
    // Real color through ncurses means an actual color pair, applied via
    // wattron()/wattroff() around the swatch characters, not baked into the
    // string. Colors/pairs are dynamically allocated the first time a given
    // RGB is seen and cached by packed value - a real bridge has a small,
    // bounded number of lights (so a bounded number of distinct colors),
    // well under COLORS/COLOR_PAIRS on any modern terminal. If that ever
    // were exhausted, new colors just reuse the most recently allocated
    // pair rather than growing without bound or erroring.
    bool color_ready = false;
    std::map<int, short> rgb_to_pair; // packed 0xRRGGBB -> ncurses pair id
    short next_color_slot = 1;        // slot 0 is the terminal's own default palette entry
    short next_pair_slot = 1;         // pair 0 is reserved (default fg/bg)

    // Called once from main(), right after OLLI_DISPLAY brings up ncurses -
    // only set_hue_light's color path (preset/hex) needs this, so a
    // terminal with no color support (color_ready stays false) just means
    // draw_light_row() below falls back to plain block characters in the
    // default color instead of failing.
    void init_color_support()
    {
        if (!has_colors()) return;
        start_color();
        use_default_colors(); // lets a pair's background stay -1 (terminal default) below
        color_ready = can_change_color();
    }

    short pair_for_rgb(const RGB& c)
    {
        if (!color_ready) return 0;

        int key = (c.r << 16) | (c.g << 8) | c.b;
        auto it = rgb_to_pair.find(key);
        if (it != rgb_to_pair.end()) return it->second;

        if (next_pair_slot >= COLOR_PAIRS || next_color_slot >= COLORS) {
            return rgb_to_pair.empty() ? static_cast<short>(0) : rgb_to_pair.rbegin()->second;
        }

        short color_id = next_color_slot++;
        short pair_id = next_pair_slot++;
        // init_extended_color()'s r/g/b are 0-1000, not 0-255.
        init_extended_color(color_id, c.r * 1000 / 255, c.g * 1000 / 255, c.b * 1000 / 255);
        init_extended_pair(pair_id, color_id, -1);
        rgb_to_pair[key] = pair_id;
        return pair_id;
    }

    // Display columns in a string built only from ASCII plus
    // brightness_bar()'s U+2588/U+2591 block glyphs - each of those is 3
    // UTF-8 bytes but exactly 1 terminal column, so a plain byte count
    // (std::string::size()) overcounts by 2 columns per glyph. Counts every
    // byte that isn't a UTF-8 continuation byte (top two bits "10") as one
    // column instead - same "byte count isn't proportional to column count"
    // pitfall ../clock/clock.cpp's BigClock struct tracks its own width to
    // avoid, just via a general helper here instead of a running total kept
    // alongside construction.
    int display_width(const std::string& s)
    {
        int width = 0;
        for (unsigned char c : s) {
            if ((c & 0xC0) != 0x80) ++width;
        }
        return width;
    }

    // One line of the light list below - id, name, on/off, a brightness
    // bar, and (if this light supports color - a plain white/ambiance bulb
    // has no "xy" at all) an actual colored swatch. Draws straight into
    // win at row (not a plain string, unlike every other line in
    // draw_light_list() below) since the swatch's color has to be applied
    // as a real ncurses attribute around just those few characters, not
    // embedded in the text - see the color-swatch comment above.
    void draw_light_row(WINDOW* win, int row, const LightState& state)
    {
        int percent = bri_to_brightness_percent(state.brightness);

        std::stringstream ss;
        ss << " [" << std::setw(3) << std::right << state.id << "] "
           << std::setw(14) << std::left << state.name.substr(0, 14) << " "
           << (state.on ? "ON " : "OFF") << "  "
           << brightness_bar(percent) << " " << std::setw(3) << std::right << percent << "%  ";

        std::string prefix = ss.str();
        mvwaddstr(win, row, 0, prefix.c_str());
        int col = display_width(prefix); // NOT prefix.size() - see display_width()'s comment

        if (state.xy.size() == 2) {
            RGB c = xy_to_rgb(state.xy[0], state.xy[1]);
            short pair = pair_for_rgb(c);
            if (pair > 0) wattron(win, COLOR_PAIR(pair));
            mvwaddstr(win, row, col, "\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88\xE2\x96\x88");
            if (pair > 0) wattroff(win, COLOR_PAIR(pair));
            col += 4;
        } else {
            mvwaddstr(win, row, col, "(no color data)");
            col += 15;
        }

        if (!state.reachable) mvwaddstr(win, row, col, "  *UNREACHABLE*");
    }

    // Draws the bridge summary + light list into display's tool area - the
    // part of the screen genuinely variable in height (a bridge refresh can
    // add/drop a light), same reasoning as ../presence/presence.cpp's
    // draw_people_list().
    void draw_light_list(OLLI_DISPLAY& display)
    {
        std::string bridge_line = "Bridge: " + hue_settings.bridge_ip + " - " +
                                   std::to_string(hue.cached_light_count()) + " light(s) cached";

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

        int row_count = sorted_lights.empty() ? 1 : static_cast<int>(sorted_lights.size());
        display.set_tool_area_height(2 + row_count); // bridge line + blank + rows

        WINDOW* win = display.tool_area();
        werase(win);
        mvwaddstr(win, 0, 0, bridge_line.c_str());

        if (sorted_lights.empty()) {
            mvwaddstr(win, 2, 0, "  (no lights cached yet)");
        } else {
            for (size_t i = 0; i < sorted_lights.size(); ++i) {
                draw_light_row(win, static_cast<int>(2 + i), sorted_lights[i]);
            }
        }

        display.refresh_tool_area();
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
    switch_profile("");

    // One link per server (olli, and subcon if it's running) - same register
    // message on both. See ../PROTOCOL.md's "Two servers" section.
    OLLI_LINK link(host, host_addr, make_register_message());
    OLLI_LINK subcon_link(host, host_addr, make_register_message(), SUBCON_TOOL_PORT);
    OLLI_DISPLAY display(1); // draw_light_list()'s first call corrects this
    init_color_support();    // must run after OLLI_DISPLAY brings up ncurses

    // connection_status/tool_status are the display's two lower fixed lines
    // (see olli_display.hpp) - kept separate since a connection change and
    // a tool event (a call answered) are unrelated facts, same split as
    // ../clock/clock.cpp and ../presence/presence.cpp.
    std::string connection_status = "Not connected to olli at " + host + " - retrying...";
    std::string tool_status;

    auto last_light_refresh = std::chrono::steady_clock::now();

    bool quit = false;
    while (!quit) {
        display.tick(); // picks up a resize, expires any timed-out activity line

        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;
        if (link.fd() >= 0) {
            FD_SET(link.fd(), &read_fds);
            max_fd = link.fd();
        }
        if (subcon_link.fd() >= 0) {
            FD_SET(subcon_link.fd(), &read_fds);
            max_fd = std::max(max_fd, subcon_link.fd());
        }

        int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);

        int key = display.get_key();
        if (key == 'q' || key == 'Q' || key == 3) quit = true; // 3 = Ctrl+C

        bool socket_readable = link.fd() >= 0 && ready > 0 && FD_ISSET(link.fd(), &read_fds);
        bool subcon_readable = subcon_link.fd() >= 0 && ready > 0 && FD_ISSET(subcon_link.fd(), &read_fds);

        if (!quit) {
            link.service(socket_readable);
            subcon_link.service(subcon_readable);

            // Only when NEITHER server is still connected - one dropping
            // must not reset the profile the other's link is relying on.
            if (all_links_dropped(link, subcon_link)) reset_to_default_profile();

            // Each message is answered on the link it arrived on.
            // subcon_link's status() is deliberately never shown as the
            // connection text, only summarized by subcon_link_indicator()
            // below, so a subcon that isn't running stays quiet.
            for (OLLI_LINK* l : {&link, &subcon_link}) {
                json msg;
                while (l->next_message(msg)) {
                    std::string type = msg.value("type", "");
                    if (type == "call") {
                        // A malformed call (a wrongly typed argument) must never take the whole
                        // tool down: answer it with an error instead.
                        try {
                            tool_status = handle_call(*l, msg);
                        } catch (const std::exception& e) {
                            std::string call_id = msg.contains("call_id") && msg["call_id"].is_string() ? msg["call_id"].get<std::string>() : "";
                            l->send_error(call_id, std::string("Could not understand that request: ") + e.what());
                            tool_status = "Error: bad request";
                        }
                    }
                    else if (type == "identity") handle_identity(msg);
                }
            }

            if (!link.status().empty()) connection_status = link.status();
        }

        // Periodic light-cache refresh, independent of whatever happened
        // above this tick - see LIGHT_REFRESH_INTERVAL_SECONDS' comment.
        if (!quit) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_light_refresh).count()
                    >= LIGHT_REFRESH_INTERVAL_SECONDS) {
                last_light_refresh = now;
                hue.refresh_lights();
            }
        }

        if (!quit) {
            draw_light_list(display);

            display.set_connection(connection_status + subcon_link_indicator(subcon_link));
            display.set_profile(current_profile_name.empty() ? "Profile: (shared default)" : "Profile: " + current_profile_name);
            display.set_tool_status(tool_status);

            display.present();
        }
    }

    curl_global_cleanup();
    return 0;
}
