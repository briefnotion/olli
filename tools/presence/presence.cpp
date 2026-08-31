// A home/away presence sensor for olli (see ../PROTOCOL.md). Detects each
// tracked person via two independent backends - Bluetooth (classic BR/EDR
// MAC via l2ping) and Wi-Fi (ARP/neighbor-table lookup) - see
// helper_presence.hpp for BluetoothBackend/WifiBackend/PersonProfile, the
// classes that actually do the detection and hold each person's state.
//
// A person is "near" if EITHER backend's most recent raw check found them
// present - no debounce, no agreement requirement between the two. (An
// earlier version of this file required both backends to independently
// agree before declaring someone away, with a consecutive-miss counter on
// each side - this rewrite trades that conservatism for simplicity, relying
// on each backend's own poll_interval - see helper_presence.hpp - to avoid
// spamming real checks rather than a debounce window.) PersonProfile::poll()
// (helper_presence.hpp) is what detects a near/away transition and sets
// triggered; run_triggers() below fires the person's configured action
// through olli and clears it.
//
// Settings (which people to track - MAC/IP/actions each - loaded fresh from
// whichever identity olli sends right after registration) live in
// PresenceSettings/PersonSettings below - see handle_identity(). Cleared on
// disconnect, same as clock.cpp's reset_to_default_profile().
//
// Build: `make` in this directory. Run: `./presence [host]` - does
// not need olli to already be running. `./presence --help` for usage.

#include "helper_presence.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <filesystem>
#include <vector>

#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {
    constexpr int REMOTE_TOOL_PORT = 47601;

    // Heartbeat/reconnect timing - see ../PROTOCOL.md. Kept in step with
    // TOOL_REMOTE's own PING_INTERVAL_SECONDS/DEAD_TIMEOUT_SECONDS
    // (source/remote_tools.h) even though nothing enforces the two staying
    // equal - either side can independently notice a timeout regardless of
    // what the other's numbers are.
    constexpr int RECONNECT_INTERVAL_SECONDS = 3;
    constexpr int PING_INTERVAL_SECONDS = 5;
    constexpr int DEAD_TIMEOUT_SECONDS = 15;

    // Caps how long one connection attempt can take - see try_connect()
    // below for why this matters once host isn't loopback.
    constexpr int CONNECT_TIMEOUT_SECONDS = 2;

    void send_line(int fd, const std::string& line)
    {
        std::string with_newline = line + "\n";
        ssize_t written = write(fd, with_newline.data(), with_newline.size());
        (void)written;
    }

    // =====================================================================
    // Per-profile settings - loaded fresh on every "identity" message
    // (handle_identity() below), written out with placeholder defaults the
    // first time a given profile has none yet, same bootstrap convention as
    // olli's own Settings::load_settings() (source/helper_olli.cpp).
    // =====================================================================

    // One tracked person within a profile - a household can hold several
    // (a profile isn't necessarily just its own olli user - see the
    // file-level comment). Each has their own phone identity and their own
    // independent home/away action.
    struct PersonSettings {
        std::string name;
        std::string bluetooth_mac = "";
        std::string wifi_ip = "";

        // {"tool": "...", "arguments": {...}} - same shape as olli's own
        // ToolCall (source/olla.h) and exactly what set_timer's
        // on_expire_tool/on_expire_arguments already do (tools/clock/clock.cpp)
        // for the identical reason: a real registered tool call, not text
        // the model has to correctly infer and re-issue on its own. Empty
        // object means "narrate only, no action" - e.g. manage_hue_scenes
        // loading a saved scene by name.
        json on_home_action = json::object();
        json on_away_action = json::object();
    };

    struct PresenceSettings {
        std::vector<PersonSettings> people;
    };

    fs::path get_home_dir()
    {
        const char* home = std::getenv("HOME");
        return (home != nullptr) ? fs::path(home) : fs::current_path();
    }

    // Mirrors Settings::get_settings_path()'s logic (olli's own
    // source/helper_olli.cpp) - same ~/olli_files_<name>/ (or shared
    // ~/olli_files/ for an empty name) convention, so this tool's settings
    // live right alongside olli's own for that profile.
    fs::path settings_path_for(const std::string& profile_name)
    {
        std::string dir_name = profile_name.empty() ? "olli_files" : "olli_files_" + profile_name;
        return get_home_dir() / dir_name / "presence_settings.json";
    }

    void save_settings(const fs::path& path, const PresenceSettings& s)
    {
        json j;

        json people_json = json::array();
        for (const PersonSettings& p : s.people) {
            people_json.push_back({
                {"name", p.name},
                {"bluetooth_mac", p.bluetooth_mac},
                {"wifi_ip", p.wifi_ip},
                {"on_home_action", p.on_home_action},
                {"on_away_action", p.on_away_action}
            });
        }
        j["people"] = people_json;

        std::ofstream file(path);
        if (file.is_open()) file << j.dump(4);
    }

    // Loads settings for one profile, bootstrapping a placeholder file (same
    // as olli's own settings.json on first run) if none exists yet.
    //
    // Also migrates the old single-person schema (a profile's file used to
    // hold one bluetooth_mac/wifi_ip/on_home_action/on_away_action directly
    // at the top level, before multi-person support existed) into a single
    // PersonSettings entry named after the profile itself, then rewrites the
    // file in the new schema immediately - so an existing real settings file
    // (real MAC/IP, real configured actions) survives the upgrade as that
    // person's entry rather than silently vanishing because the old
    // top-level keys are no longer read.
    PresenceSettings load_settings(const std::string& profile_name)
    {
        fs::path path = settings_path_for(profile_name);
        PresenceSettings s;

        if (!fs::exists(path)) {
            fs::create_directories(path.parent_path());
            save_settings(path, s); // writes the defaults above as a starting point to edit
            return s;
        }

        try {
            std::ifstream file(path);
            json j;
            file >> j;

            bool needs_resave = false;

            if (j.contains("people") && j["people"].is_array()) {
                for (const json& pj : j["people"]) {
                    std::string name = pj.value("name", "");
                    if (name.empty()) continue; // malformed hand-edit - skip, don't fail the whole load

                    PersonSettings p;
                    p.name = name;
                    p.bluetooth_mac = pj.value("bluetooth_mac", p.bluetooth_mac);
                    p.wifi_ip = pj.value("wifi_ip", p.wifi_ip);
                    p.on_home_action = pj.value("on_home_action", p.on_home_action);
                    p.on_away_action = pj.value("on_away_action", p.on_away_action);
                    s.people.push_back(p);
                }
            } else if (j.contains("bluetooth_mac") || j.contains("wifi_ip") ||
                       j.contains("on_home_action") || j.contains("on_away_action")) {
                PersonSettings p;
                p.name = profile_name.empty() ? "default" : profile_name;
                p.bluetooth_mac = j.value("bluetooth_mac", p.bluetooth_mac);
                p.wifi_ip = j.value("wifi_ip", p.wifi_ip);
                p.on_home_action = j.value("on_home_action", p.on_home_action);
                p.on_away_action = j.value("on_away_action", p.on_away_action);
                s.people.push_back(p);
                needs_resave = true;
            }

            if (needs_resave) save_settings(path, s);
        } catch (const std::exception&) {
            // Malformed file - fall back to defaults rather than crash;
            // s already holds them.
        }

        return s;
    }

    // Detection backends (check_bluetooth_present()/check_wifi_present())
    // and BluetoothBackend/WifiBackend/PersonProfile now live in
    // helper_presence.hpp/.cpp - see that header's own top comment for the
    // design.

    // Push-only path - see PersonSettings::on_home_action's comment for why
    // this carries a real action, not just narration text.
    void fire_transition_event(int fd, const std::string& message, const json& action)
    {
        json event_msg = {{"type", "event"}, {"message", message}};
        if (action.is_object() && !action.empty()) {
            event_msg["action"] = {
                {"tool", action.value("tool", "")},
                {"arguments", action.value("arguments", json::object())}
            };
        }
        send_line(fd, event_msg.dump());
    }

    std::vector<PersonProfile> people;

    // Builds people fresh from settings.people (already-parsed/migrated
    // JSON - see load_settings()) - one PersonProfile per entry, with one
    // BluetoothBackend and one WifiBackend each.
    void load_person_profiles();

    void poll_all_people()
    {
        for (PersonProfile& profile : people) {
            profile.poll();
        }
    }

    // Steps through every person; for each one whose trigger is on, fires
    // their configured action (on_near_action if they just arrived,
    // on_away_action if they just left) through olli, then clears the
    // trigger so it doesn't fire again next call.
    void run_triggers(int fd)
    {
        for (PersonProfile& profile : people) {
            if (!profile.triggered) continue;

            std::string message = profile.name + (profile.is_near ? " just got home." : " just left.");
            fire_transition_event(fd, message, profile.is_near ? profile.on_near_action : profile.on_away_action);

            profile.triggered = false;
        }
    }

    // =====================================================================
    // Identity - see ../clock/clock.cpp's handle_identity()/
    // reset_to_default_profile() for the pattern this mirrors, and
    // ../PROTOCOL.md's "identity" message. A fresh identity means a fresh
    // settings file and a fresh set of PersonProfiles.
    // =====================================================================

    std::string current_profile_name;
    PresenceSettings settings;

    std::string handle_identity(const json& msg)
    {
        current_profile_name = msg.value("name", "");
        settings = load_settings(current_profile_name);
        load_person_profiles();

        if (current_profile_name.empty()) {
            return "Identified: olli's shared default (no profile)";
        }
        return "Identified: " + current_profile_name + " - settings loaded from " +
               settings_path_for(current_profile_name).string();
    }

    void reset_to_default_profile()
    {
        current_profile_name.clear();
        settings = PresenceSettings{};
        people.clear();
    }

    void load_person_profiles()
    {
        people.clear();
        for (const PersonSettings& person_settings : settings.people) {
            PersonProfile profile(person_settings.name);
            profile.add_bluetooth_backend(BluetoothBackend(person_settings.bluetooth_mac));
            profile.add_wifi_backend(WifiBackend(person_settings.wifi_ip));
            profile.on_near_action = person_settings.on_home_action;
            profile.on_away_action = person_settings.on_away_action;
            people.push_back(std::move(profile));
        }
    }

    // =====================================================================
    // Registration + calls. Three queryable tools alongside the push-only
    // event/action path (run_triggers() above):
    //   - check_presence: "is anyone home right now" - the live reading.
    //   - get_presence_setup: "what's configured" - not the live reading,
    //     so a user can ask olli what will happen without needing to look
    //     at this program's own terminal at all.
    //   - set_presence_action: lets the model configure on_near_action/
    //     on_away_action itself from a plain-language request ("when ron
    //     gets home, load the repose scene").
    // =====================================================================

    // One-line summary of a {"tool": ..., "arguments": {...}} action, or a
    // fixed placeholder for an unconfigured (empty object) one - shared by
    // redraw_screen() and get_presence_setup's result below, so the
    // terminal display and what olli can be asked always agree.
    std::string describe_action(const json& action)
    {
        if (!action.is_object() || action.empty()) return "(none configured - narration only)";
        std::string tool = action.value("tool", "");
        if (tool.empty()) return "(none configured - narration only)";
        return tool + "(" + action.value("arguments", json::object()).dump() + ")";
    }

    std::string describe_presence()
    {
        if (people.empty()) {
            return "No people configured for this profile yet - see get_presence_setup.";
        }

        std::stringstream ss;
        ss << "Presence:";
        for (const PersonProfile& profile : people) {
            ss << "\n- " << profile.name << ": " << (profile.is_near ? "NEAR" : "AWAY");
        }
        return ss.str();
    }

    std::string describe_setup()
    {
        std::stringstream ss;
        ss << "Presence setup for " << (current_profile_name.empty() ? "shared default" : current_profile_name) << ".";

        if (people.empty()) {
            ss << " No people configured yet.";
            return ss.str();
        }

        for (const PersonProfile& profile : people) {
            ss << "\n- " << profile.name << ": on near: " << describe_action(profile.on_near_action)
               << ". On away: " << describe_action(profile.on_away_action) << ".";
        }
        return ss.str();
    }

    // Handles set_presence_action - see its registered description below.
    // Updates both settings.people (persisted immediately via
    // save_settings(), same file handle_identity() loaded from, so the
    // change survives a restart/reconnect) and the matching live
    // PersonProfile in people (so it takes effect on the very next poll,
    // not just after a reconnect).
    std::string handle_set_presence_action(const json& args)
    {
        std::string trigger = args.value("trigger", "");
        if (trigger != "near" && trigger != "away") {
            return "Error: 'trigger' must be 'near' or 'away'.";
        }

        std::string person_name = args.value("person", "");
        PersonSettings* target_settings = nullptr;
        for (PersonSettings& p : settings.people) {
            if (p.name == person_name) {
                target_settings = &p;
                break;
            }
        }
        if (target_settings == nullptr) {
            std::string configured = "(none configured)";
            if (!settings.people.empty()) {
                configured.clear();
                for (size_t i = 0; i < settings.people.size(); ++i) {
                    if (i > 0) configured += ", ";
                    configured += settings.people[i].name;
                }
            }
            return "Error: no person named '" + person_name + "' configured for this profile. Configured: " +
                   configured + ".";
        }

        std::string tool = args.value("tool", "");
        json new_action = json::object();
        if (!tool.empty()) {
            new_action = {{"tool", tool}, {"arguments", args.value("arguments", json::object())}};
        }

        json& action_target = (trigger == "near") ? target_settings->on_home_action : target_settings->on_away_action;
        action_target = new_action;
        save_settings(settings_path_for(current_profile_name), settings);

        for (PersonProfile& profile : people) {
            if (profile.name == person_name) {
                (trigger == "near" ? profile.on_near_action : profile.on_away_action) = new_action;
                break;
            }
        }

        return "Presence '" + trigger + "' action for " + person_name + " set to " + describe_action(new_action) + ".";
    }

    // Handles register_presence_person - see its registered description
    // below. Adds a brand new PersonSettings entry (persisted immediately,
    // same as handle_set_presence_action()) and a matching live
    // PersonProfile, so the new person shows up in check_presence/
    // get_presence_setup and starts polling right away - no reconnect
    // needed. Mirrors load_person_profiles()'s own construction of a
    // PersonProfile from a PersonSettings entry.
    std::string handle_register_presence_person(const json& args)
    {
        std::string person_name = args.value("name", "");
        if (person_name.empty()) {
            return "Error: 'name' is required.";
        }

        for (const PersonSettings& p : settings.people) {
            if (p.name == person_name) {
                return "Error: a person named '" + person_name + "' is already configured for this profile. "
                       "Use set_presence_action to change their actions instead.";
            }
        }

        PersonSettings new_person;
        new_person.name = person_name;
        new_person.bluetooth_mac = args.value("bluetooth_mac", "");
        new_person.wifi_ip = args.value("wifi_ip", "");
        settings.people.push_back(new_person);
        save_settings(settings_path_for(current_profile_name), settings);

        PersonProfile profile(new_person.name);
        profile.add_bluetooth_backend(BluetoothBackend(new_person.bluetooth_mac));
        profile.add_wifi_backend(WifiBackend(new_person.wifi_ip));
        people.push_back(std::move(profile));

        std::string result = "Registered " + person_name + " for presence tracking.";
        if (new_person.bluetooth_mac.empty() && new_person.wifi_ip.empty()) {
            result += " No bluetooth_mac or wifi_ip given, so they'll show as away until at least one is set - "
                      "edit " + settings_path_for(current_profile_name).string() + " directly to add it.";
        }
        return result;
    }

    json make_register_message()
    {
        return {
            {"type", "register"},
            {"tools", json::array({
                {
                    {"name", "check_presence"},
                    {"description", "Reports whether each person configured for this household is currently "
                                     "detected as near (home) or away. Always execute this tool call for every "
                                     "request, even if you believe you already know the answer."},
                    {"parameters", {{"type", "object"}}}
                },
                {
                    {"name", "get_presence_setup"},
                    {"description", "Reports which people are tracked for this profile and what will happen "
                                     "(if anything) when each one arrives or leaves. Use this when the user asks "
                                     "what's set up, not whether someone's currently home."},
                    {"parameters", {{"type", "object"}}}
                },
                {
                    {"name", "set_presence_action"},
                    {"description", "Configures what happens when a specific configured person arrives or "
                                     "leaves. Call this whenever the user asks to set up, change, or clear what "
                                     "happens on someone's arrival or departure - e.g. 'when ron gets home, load "
                                     "the repose scene' or 'stop doing anything when gus leaves'. 'person' must "
                                     "match a name already configured (see get_presence_setup). To set a real "
                                     "action (not just narration), construct 'tool'/'arguments' exactly as you "
                                     "would to call that other tool directly - e.g. tool='manage_hue_scenes', "
                                     "arguments={'action': 'load', 'name': 'repose'}. Omit 'tool' (or leave it "
                                     "empty) to clear an existing action back to narration-only."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"person", {{"type", "string"},
                                        {"description", "Which configured person this applies to - must match a "
                                                         "name already configured for this profile (see "
                                                         "get_presence_setup)."}}},
                            {"trigger", {{"type", "string"}, {"enum", {"near", "away"}},
                                         {"description", "Which event this configures - 'near' fires on "
                                                          "arrival, 'away' fires on departure."}}},
                            {"tool", {{"type", "string"},
                                      {"description", "Name of another registered olli tool to call on this trigger. Omit/empty to clear."}}},
                            {"arguments", {{"type", "object"},
                                           {"description", "Arguments for that tool, same shape as calling it directly."}}}
                        }},
                        {"required", json::array({"person", "trigger"})}
                    }}
                },
                {
                    {"name", "register_presence_person"},
                    {"description", "Adds a brand new person to presence tracking for this profile - use this "
                                     "when the user wants to start tracking someone who isn't configured yet "
                                     "(check get_presence_setup first to see who's already configured). "
                                     "Requires their phone's classic Bluetooth MAC and/or Wi-Fi IP address - you "
                                     "have no way to know these yourself, so ask the user for them. The MAC "
                                     "comes from pairing the phone with this machine first (bluetoothctl); the "
                                     "IP comes from the home router's DHCP client list - see this tool's "
                                     "README.md for the full one-time setup. Registering with neither value is "
                                     "allowed but leaves that person showing as permanently away until at least "
                                     "one is filled in by hand-editing the settings file. Fails if a person with "
                                     "that name is already configured - use set_presence_action to change an "
                                     "existing person's actions instead."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"name", {{"type", "string"},
                                      {"description", "Name for this person - what they'll be called in "
                                                       "check_presence/set_presence_action."}}},
                            {"bluetooth_mac", {{"type", "string"},
                                               {"description", "Their phone's classic Bluetooth MAC address "
                                                                "(e.g. 'F0:1F:C7:8C:9C:0B'), if known."}}},
                            {"wifi_ip", {{"type", "string"},
                                        {"description", "Their phone's home-network IP address, if known."}}}
                        }},
                        {"required", json::array({"name"})}
                    }}
                }
            })}
        };
    }

    std::string handle_call(int fd, const json& msg)
    {
        std::string call_id = msg.value("call_id", "");
        std::string name = msg.value("name", "");
        json args = msg.value("arguments", json::object());

        json result_msg;
        std::string status;
        if (name == "check_presence") {
            result_msg = {{"type", "result"}, {"call_id", call_id}, {"result", describe_presence()}};
            status = "Call answered: " + name;
        } else if (name == "get_presence_setup") {
            result_msg = {{"type", "result"}, {"call_id", call_id}, {"result", describe_setup()}};
            status = "Call answered: " + name;
        } else if (name == "set_presence_action") {
            result_msg = {{"type", "result"}, {"call_id", call_id}, {"result", handle_set_presence_action(args)}};
            status = "Call answered: " + name;
        } else if (name == "register_presence_person") {
            result_msg = {{"type", "result"}, {"call_id", call_id}, {"result", handle_register_presence_person(args)}};
            status = "Call answered: " + name;
        } else {
            result_msg = {
                {"type", "result"},
                {"call_id", call_id},
                {"error", "Unknown tool name: " + name}
            };
            status = "Unknown call received: " + name;
        }
        send_line(fd, result_msg.dump());
        return status;
    }

    // --- Terminal handling (see ../template/template_tool.cpp's own copy
    // of this for the full explanation - unchanged here) ---

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
                std::cout << "\033[?25l" << std::flush; // hide cursor
            }

            ~RawTerminal()
            {
                std::cout << "\033[?25h" << std::flush; // show cursor again
                if (active) tcsetattr(STDIN_FILENO, TCSANOW, &old_termios);
            }

            RawTerminal(const RawTerminal&) = delete;
            RawTerminal& operator=(const RawTerminal&) = delete;

        private:
            termios old_termios{};
            bool active = false;
    };

    // Tracks how many lines the previous frame drew, so a frame with fewer
    // lines (e.g. a profile switch to a household with fewer people) still
    // blanks the leftover lines below it instead of leaving stale text on
    // screen - see redraw_screen() below.
    int last_frame_line_count = 0;

    void redraw_screen(const std::string& status, const std::string& conn_status)
    {
        std::vector<std::string> lines;
        lines.push_back(conn_status);
        lines.push_back(current_profile_name.empty()
                    ? "Profile: (shared default)" : "Profile: " + current_profile_name);

        if (people.empty()) {
            lines.push_back("  No people loaded.");
        } else {
            for (const PersonProfile& profile : people) {
                lines.push_back("Person: " + profile.name + " - " + (profile.is_near ? "NEAR" : "AWAY"));
                for (const BluetoothBackend& backend : profile.bluetooth_backends) {
                    lines.push_back(std::string("  Bluetooth: ") + (backend.is_near() ? "NEAR" : "AWAY") +
                                     (backend.is_searching() ? " (searching)" : ""));
                }
                for (const WifiBackend& backend : profile.wifi_backends) {
                    lines.push_back(std::string("  Wi-Fi:     ") + (backend.is_near() ? "NEAR" : "AWAY") +
                                     (backend.is_searching() ? " (searching)" : ""));
                }
                lines.push_back("  On near: " + describe_action(profile.on_near_action));
                lines.push_back("  On away: " + describe_action(profile.on_away_action));
            }
        }
        lines.push_back(status);

        std::cout << "\033[H";
        size_t total = std::max(lines.size(), static_cast<size_t>(last_frame_line_count));
        for (size_t i = 0; i < total; ++i) {
            std::cout << "\033[2K";
            if (i < lines.size()) std::cout << lines[i];
            std::cout << "\n";
        }
        last_frame_line_count = static_cast<int>(lines.size());
        std::cout << std::flush;
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

    // Same bounded, non-blocking connect as ../template/template_tool.cpp's
    // try_connect() - see its own comment for the full reasoning.
    int try_connect(const in_addr& host_addr)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;

        int flags = fcntl(fd, F_GETFL, 0);
        if (flags != -1) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(REMOTE_TOOL_PORT));
        addr.sin_addr = host_addr;

        int rc = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

        if (rc < 0 && errno == EINPROGRESS) {
            timeval tv{};
            tv.tv_sec = CONNECT_TIMEOUT_SECONDS;
            tv.tv_usec = 0;

            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(fd, &write_fds);

            int ready = select(fd + 1, nullptr, &write_fds, nullptr, &tv);
            if (ready <= 0) {
                close(fd);
                return -1;
            }

            int so_error = 0;
            socklen_t len = sizeof(so_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len) < 0 || so_error != 0) {
                close(fd);
                return -1;
            }
        } else if (rc < 0) {
            close(fd);
            return -1;
        }

        if (flags != -1) fcntl(fd, F_SETFL, flags);
        return fd;
    }

    bool extract_line(std::string& buffer, std::string& out)
    {
        auto newline_pos = buffer.find('\n');
        if (newline_pos == std::string::npos) return false;

        out = buffer.substr(0, newline_pos);
        buffer.erase(0, newline_pos + 1);
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
    }
}

int main(int argc, char* argv[])
{
    // Writing to a socket right as olli closes its end raises SIGPIPE,
    // whose default disposition kills this whole process - ignoring it
    // makes write() (send_line(), above) just return -1 (EPIPE) instead,
    // same as olli's own core does for the same reason (see
    // source/main.cpp's std::signal(SIGPIPE, SIG_IGN) call), and what
    // tools/template/olli_link.cpp now does too. Set here, as early as
    // possible, before the first connection attempt.
    std::signal(SIGPIPE, SIG_IGN);

    std::string host = "127.0.0.1";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else {
            host = arg;
        }
    }

    in_addr host_addr{};
    if (inet_pton(AF_INET, host.c_str(), &host_addr) != 1) {
        std::cerr << "Not a valid IPv4 address: " << host << "\n\n";
        print_usage(argv[0]);
        return 1;
    }

    // people stays empty until an "identity" message actually arrives (see
    // handle_identity()) - deliberately not loading the shared/no-profile
    // settings here at startup the way this file used to, since that meant
    // presence started polling real hardware before olli had even
    // connected, under whatever the shared file happened to hold.
    const json register_msg = make_register_message();

    RawTerminal raw_terminal;
    std::cout << "\033[2J";

    // A file at EOF (stdin redirected from /dev/null, or genuinely closed -
    // e.g. this program ever run unattended, with no controlling terminal)
    // is always "ready to read" as far as select() is concerned, since
    // reading it returns immediately (0 bytes) rather than blocking. If
    // STDIN_FILENO were unconditionally watched below, that would make
    // select()'s 200ms timeout never actually apply - the loop would spin
    // as fast as the CPU allows instead of pacing itself, hammering the
    // socket/display/poll logic at full speed. Watching it only when it's a
    // real terminal sidesteps that entirely: with nothing in read_fds but a
    // (possibly absent) socket, select() genuinely blocks for the timeout,
    // same as intended. There's no 'q'-to-quit to watch for anyway without
    // a real terminal for someone to press it on.
    bool has_real_terminal = isatty(STDIN_FILENO) != 0;

    int fd = -1;
    std::string status = "Not connected to olli at " + host + " - retrying...";
    std::string conn_status = status;
    std::string read_buffer;
    auto last_sent = std::chrono::steady_clock::now();
    auto last_received = std::chrono::steady_clock::now();
    auto last_connect_attempt = std::chrono::steady_clock::time_point{};

    bool quit = false;
    while (!quit) {
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
        if (fd >= 0) {
            FD_SET(fd, &read_fds);
            max_fd = std::max(fd, max_fd);
        }

        int ready = select(max_fd + 1, &read_fds, nullptr, nullptr, &tv);

        if (ready > 0 && FD_ISSET(STDIN_FILENO, &read_fds)) {
            char c = 0;
            if (read(STDIN_FILENO, &c, 1) > 0) {
                if (c == 'q' || c == 'Q' || c == 3) quit = true;
            }
        }

        auto now = std::chrono::steady_clock::now();

        if (!quit && fd < 0) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_connect_attempt).count()
                    >= RECONNECT_INTERVAL_SECONDS) {
                last_connect_attempt = now;
                fd = try_connect(host_addr);
                if (fd >= 0) {
                    send_line(fd, register_msg.dump());
                    conn_status = "Registered with olli at " + host + ". Waiting for identity/calls...";
                    last_sent = last_received = now;
                    read_buffer.clear();
                } else {
                    conn_status = "Not connected to olli at " + host + " - retrying...";
                }
            }
        }
        else if (!quit && fd >= 0 && ready > 0 && FD_ISSET(fd, &read_fds)) {
            char buf[4096];
            ssize_t n = read(fd, buf, sizeof(buf));

            if (n <= 0) {
                close(fd);
                fd = -1;
                reset_to_default_profile();
                conn_status = "Disconnected from olli at " + host + " - retrying...";
            } else {
                read_buffer.append(buf, static_cast<size_t>(n));

                std::string line;
                while (extract_line(read_buffer, line)) {
                    if (line.empty()) continue;

                    last_received = std::chrono::steady_clock::now();
                    try {
                        json msg = json::parse(line);
                        std::string type = msg.value("type", "");
                        if (type == "call") {
                            status = handle_call(fd, msg);
                            // handle_call() sends its result via send_line()
                            // internally but never touched last_sent, so a
                            // call answered right after an idle stretch
                            // could still have the heartbeat block below
                            // queue a ping right behind it. Not just
                            // cosmetic: olli's own TOOL_REMOTE::check()
                            // (source/remote_tools.cpp) reads exactly one
                            // line as "the" answer to its call - if a ping
                            // beat the real result onto the wire, it would
                            // misread the ping as an unexpected response and
                            // orphan the real result to desync a later,
                            // unrelated call. See ../PROTOCOL.md's ping/pong
                            // section ("either message counts as proof of
                            // life") and tools/template/olli_link.cpp's
                            // OLLI_LINK::send_result() for where this same
                            // bug was first found and fixed.
                            last_sent = std::chrono::steady_clock::now();
                        } else if (type == "ping") {
                            send_line(fd, json{{"type", "pong"}}.dump());
                            last_sent = std::chrono::steady_clock::now();
                        } else if (type == "identity") {
                            conn_status = handle_identity(msg);
                        }
                    } catch (const std::exception&) {
                        status = "Bad JSON from olli.";
                    }
                }
            }
        }

        // Heartbeat - see ../PROTOCOL.md.
        if (!quit && fd >= 0) {
            now = std::chrono::steady_clock::now();

            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_sent).count() >= PING_INTERVAL_SECONDS) {
                send_line(fd, json{{"type", "ping"}}.dump());
                last_sent = now;
            }

            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_received).count() >= DEAD_TIMEOUT_SECONDS) {
                close(fd);
                fd = -1;
                reset_to_default_profile();
                conn_status = "Connection to olli at " + host + " timed out - retrying...";
            }
        }

        // Poll every tracked person and fire whatever just transitioned -
        // safe to call every tick regardless of connection state: people is
        // empty until an identity arrives (see handle_identity()), and each
        // backend rate-limits its own real network check via poll_interval
        // (helper_presence.hpp) - so this costs nothing extra when there's
        // nothing to do.
        if (!quit) {
            poll_all_people();
            run_triggers(fd);
            status = "Polled " + std::to_string(people.size()) + " people.";
        }

        if (!quit) redraw_screen(status, conn_status);
    }

    if (fd >= 0) close(fd);
    return 0;
}
