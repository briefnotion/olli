// A home/away presence sensor for olli (see ../PROTOCOL.md) - built from
// ../template/template_tool.cpp's connection plumbing, plus identity
// handling copied from ../clock/clock.cpp's pattern (the template doesn't
// have that yet - see its own note about being due for an update once this
// tool exists as a second real-world example).
//
// Runs TWO independent detection backends every poll interval and compares
// them, rather than trusting either alone:
//   - Bluetooth: pings a paired phone's classic (BR/EDR) MAC via l2ping.
//     Classic Bluetooth's address is stable, unlike BLE advertisements -
//     iOS (and modern Android) randomize the BLE MAC roughly every 15
//     minutes specifically to prevent passive tracking, so scanning for
//     ambient BLE broadcasts was ruled out - see the design discussion this
//     came out of. Requires pairing the phone with this machine once
//     first, and l2ping needs raw-socket privilege - see the README.
//   - Wi-Fi: checks whether the phone's home-network IP shows as reachable
//     in this machine's ARP/neighbor table (forcing a fresh probe first, so
//     a stale cache entry can't lie).
//
// Both run every tick regardless of mode - the only difference --test makes
// (see main()) is whether a debounced HOME/AWAY *transition* actually fires
// the configured action, or just gets logged for comparison. Per-backend
// results are debounced independently (N consecutive hits/misses before
// that backend's own state flips - see BackendTracker::record()), and the
// two backends must independently agree before anything is considered
// settled - see combine_states(). Disagreement isn't an error, it's just
// "not sure yet" - nothing fires until both sides say the same thing.
//
// Settings (MAC/IP to watch, poll interval, debounce counts, what to do on
// arrival/departure) are per-profile, loaded fresh from whichever identity
// olli sends right after registration - see handle_identity() and
// PresenceSettings below. Falls back to the shared, no-profile defaults
// (and clears any loaded settings) on disconnect, same as clock.cpp's
// reset_to_default_profile().
//
// Build: `make` in this directory. Run: `./presence [host] [--test]` - does
// not need olli to already be running. `./presence --help` for usage.

#include <nlohmann/json.hpp>

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <cerrno>
#include <filesystem>

#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/ioctl.h>
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

    // Bounds each backend's own subprocess call - see the poll loop in
    // main() for why these need to stay short: both checks run
    // synchronously, back to back, on the same thread that also owns the
    // socket/heartbeat/display, so a poll tick blocks everything else for
    // up to roughly the sum of these two. 3s worst case, on a poll interval
    // measured in tens of seconds by default, comfortably clears
    // PING_INTERVAL_SECONDS without tripping DEAD_TIMEOUT_SECONDS - the
    // same "bounded synchronous block is fine" tradeoff olli's own
    // TOOL_WEB_SEARCH (libcurl) and TOOL_REMOTE::check() (5s) already make.
    //
    // These values alone are NOT a reliable ceiling, though - real testing
    // showed l2ping -t 2 taking ~5s on an actual miss (Bluetooth link-
    // establishment overhead the -t flag doesn't cover). check_bluetooth_
    // present()/check_wifi_present() wrap their subprocess calls in the
    // coreutils `timeout` command using these same values as a hard
    // wall-clock cap on top, since a stalled poll tick can otherwise starve
    // this program's socket-read loop long enough for olli's own
    // TOOL_REMOTE::check() (5s timeout) to spuriously mark a perfectly
    // healthy connection dead - observed for real, not hypothetical.
    constexpr int BLUETOOTH_PING_TIMEOUT_SECONDS = 2;
    constexpr int WIFI_PING_TIMEOUT_SECONDS = 1;

    void send_line(int fd, const std::string& line)
    {
        std::string with_newline = line + "\n";
        ssize_t written = write(fd, with_newline.data(), with_newline.size());
        (void)written;
    }

    // =====================================================================
    // Per-profile settings - see the class-level comment above for the
    // fields. Loaded fresh on every "identity" message (handle_identity()
    // below), written out with placeholder defaults the first time a given
    // profile has none yet, same bootstrap convention as olli's own
    // Settings::load_settings() (source/helper_olli.cpp).
    // =====================================================================

    // Which backend(s) combine_states() (further down) actually requires
    // before considering presence "settled" - see its own comment for what
    // each mode means. BOTH is the default and the reason this tool has two
    // backends at all (see the file-level comment); BLUETOOTH/WIFI exist for
    // when you've already decided one alone is good enough for your
    // situation and want faster, simpler single-backend triggering instead.
    // Both backends still run and get displayed/logged every poll
    // regardless of this setting - it only changes what counts as agreement.
    enum class DetectionMode { BLUETOOTH, WIFI, BOTH };

    DetectionMode parse_detection_mode(const std::string& s)
    {
        if (s == "bluetooth") return DetectionMode::BLUETOOTH;
        if (s == "wifi") return DetectionMode::WIFI;
        return DetectionMode::BOTH; // also the fallback for an unrecognized value
    }

    std::string detection_mode_name(DetectionMode m)
    {
        switch (m) {
            case DetectionMode::BLUETOOTH: return "bluetooth";
            case DetectionMode::WIFI: return "wifi";
            default: return "both";
        }
    }

    struct PresenceSettings {
        std::string bluetooth_mac = "";
        std::string wifi_ip = "";
        int poll_interval_seconds = 30;
        DetectionMode detection_mode = DetectionMode::BOTH;

        // How many consecutive hits/misses one backend needs before ITS OWN
        // debounced state flips - see BackendTracker::record(). Independent
        // of the other backend; combine_states() is what requires
        // agreement between the two.
        int home_debounce_hits = 2;
        int away_debounce_misses = 2;

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
        j["bluetooth_mac"] = s.bluetooth_mac;
        j["wifi_ip"] = s.wifi_ip;
        j["poll_interval_seconds"] = s.poll_interval_seconds;
        j["detection_mode"] = detection_mode_name(s.detection_mode);
        j["home_debounce_hits"] = s.home_debounce_hits;
        j["away_debounce_misses"] = s.away_debounce_misses;
        j["on_home_action"] = s.on_home_action;
        j["on_away_action"] = s.on_away_action;

        std::ofstream file(path);
        if (file.is_open()) file << j.dump(4);
    }

    // Loads settings for one profile, bootstrapping a placeholder file (same
    // as olli's own settings.json on first run) if none exists yet.
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
            s.bluetooth_mac = j.value("bluetooth_mac", s.bluetooth_mac);
            s.wifi_ip = j.value("wifi_ip", s.wifi_ip);
            s.poll_interval_seconds = j.value("poll_interval_seconds", s.poll_interval_seconds);
            s.detection_mode = parse_detection_mode(j.value("detection_mode", detection_mode_name(s.detection_mode)));
            s.home_debounce_hits = j.value("home_debounce_hits", s.home_debounce_hits);
            s.away_debounce_misses = j.value("away_debounce_misses", s.away_debounce_misses);
            s.on_home_action = j.value("on_home_action", s.on_home_action);
            s.on_away_action = j.value("on_away_action", s.on_away_action);
        } catch (const std::exception&) {
            // Malformed file - fall back to defaults rather than crash;
            // s already holds them.
        }

        return s;
    }

    // =====================================================================
    // Detection backends. Both take the raw value straight from a JSON
    // settings file and hand it to popen()/system() - a loose allow-list
    // check first (hex digits/colons for a MAC, digits/dots for an IPv4
    // address) is cheap insurance against a malformed settings file putting
    // shell metacharacters on a command line, even though the file is only
    // ever hand-edited locally, never attacker-supplied over the network.
    // =====================================================================

    bool looks_like_mac(const std::string& s)
    {
        if (s.empty()) return false;
        for (char c : s) {
            if (!std::isxdigit(static_cast<unsigned char>(c)) && c != ':') return false;
        }
        return true;
    }

    bool looks_like_ipv4(const std::string& s)
    {
        if (s.empty()) return false;
        for (char c : s) {
            if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
        }
        return true;
    }

    // Pings a known, paired classic-Bluetooth MAC - see the file-level
    // comment for why classic BT (not BLE scanning) is what's reliable
    // here. Needs the phone paired with this machine at least once first
    // (bluetoothctl pair <mac>), and l2ping needs raw-socket privilege - see
    // README.md. A malformed/empty MAC or a failed ping both just read as
    // "not present" - there's no separate error channel to the caller,
    // matching every other best-effort check in this file.
    bool check_bluetooth_present(const std::string& mac)
    {
        if (!looks_like_mac(mac)) return false;

        // Wrapped in the coreutils `timeout` command as a hard wall-clock
        // cap, not just l2ping's own -t - a real miss was observed taking
        // ~5s despite -t 2 during testing (likely Bluetooth link-
        // establishment overhead underneath l2ping's own echo timeout,
        // which -t doesn't cover). Without this, a slow poll can stall this
        // program's socket-read loop long enough for olli's own
        // TOOL_REMOTE::check() (5s timeout, source/remote_tools.h) to give
        // up and mark the connection dead while this program is still
        // alive and well, just late - seen for real, not hypothetical.
        std::string cmd = "timeout " + std::to_string(BLUETOOTH_PING_TIMEOUT_SECONDS) +
                           " l2ping -c 1 -t " + std::to_string(BLUETOOTH_PING_TIMEOUT_SECONDS) +
                           " " + mac + " >/dev/null 2>&1";
        int rc = std::system(cmd.c_str());
        return rc == 0;
    }

    // Forces a fresh ARP/neighbor-table probe (a stale cached entry could
    // otherwise say REACHABLE long after the phone actually left), then
    // checks the kernel's own neighbor state for it. REACHABLE is the
    // normal "just confirmed" state; STALE/DELAY are included too since
    // they mean the kernel saw it recently and just hasn't finished
    // re-confirming yet, which can otherwise race the ping above by a
    // moment - simpler than parsing ping's own output for this purpose.
    bool check_wifi_present(const std::string& ip)
    {
        if (!looks_like_ipv4(ip)) return false;

        // Same hard-cap reasoning as check_bluetooth_present() above -
        // ping's own -W is a per-reply wait, not a guaranteed total
        // runtime ceiling either.
        std::string ping_cmd = "timeout " + std::to_string(WIFI_PING_TIMEOUT_SECONDS) +
                                " ping -c 1 -W " + std::to_string(WIFI_PING_TIMEOUT_SECONDS) +
                                " " + ip + " >/dev/null 2>&1";
        std::system(ping_cmd.c_str());

        std::string neigh_cmd = "ip neigh show " + ip + " 2>/dev/null";
        FILE* pipe = popen(neigh_cmd.c_str(), "r");
        if (pipe == nullptr) return false;

        std::string output;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), pipe) != nullptr) output += buf;
        pclose(pipe);

        return output.find("REACHABLE") != std::string::npos ||
               output.find("STALE") != std::string::npos ||
               output.find("DELAY") != std::string::npos;
    }

    // =====================================================================
    // Debounce + agreement. Each backend tracks its own consecutive
    // hit/miss streak and only flips ITS OWN state once one streak clears
    // the configured threshold - a single flaky miss (an iPhone slow to
    // answer a Bluetooth ping, a momentarily stale ARP entry) can't flip
    // anything by itself. combine_states() is the separate "both backends
    // must agree" gate on top of that.
    // =====================================================================

    enum class PresenceState { UNKNOWN, HOME, AWAY };

    std::string state_name(PresenceState s)
    {
        switch (s) {
            case PresenceState::HOME: return "HOME";
            case PresenceState::AWAY: return "AWAY";
            default: return "UNKNOWN";
        }
    }

    struct BackendTracker {
        PresenceState state = PresenceState::UNKNOWN;
        int consecutive_hits = 0;
        int consecutive_misses = 0;
        bool last_result = false;
        bool has_checked = false;
        std::chrono::steady_clock::time_point last_check{};
        std::chrono::milliseconds last_latency{0};

        void record(bool present, int home_debounce_hits, int away_debounce_misses,
                    std::chrono::milliseconds latency)
        {
            has_checked = true;
            last_result = present;
            last_latency = latency;
            last_check = std::chrono::steady_clock::now();

            if (present) {
                consecutive_hits++;
                consecutive_misses = 0;
                if (consecutive_hits >= home_debounce_hits) state = PresenceState::HOME;
            } else {
                consecutive_misses++;
                consecutive_hits = 0;
                if (consecutive_misses >= away_debounce_misses) state = PresenceState::AWAY;
            }
        }

        // Reset on every identity change (handle_identity() below) - a
        // fresh profile's debounce history shouldn't inherit whatever the
        // previous user's streaks happened to be.
        void reset()
        {
            *this = BackendTracker{};
        }
    };

    // mode == BLUETOOTH/WIFI: trust that one backend's own debounced state
    // outright, ignoring the other (which still runs and displays/logs
    // normally - see the file-level comment - just doesn't gate anything).
    //
    // mode == BOTH (the default): only HOME if both backends have
    // independently debounced to HOME, only AWAY if both have independently
    // debounced to AWAY. Anything else (either still UNKNOWN, or the two
    // disagreeing) is "not settled yet" - not an error, just not something
    // to act on. See the design discussion this came out of for why
    // disagreement doesn't get resolved any other way (e.g. majority/
    // tiebreak) - two backends can't outvote each other, they either agree
    // or nothing happens.
    PresenceState combine_states(const BackendTracker& bt, const BackendTracker& wifi, DetectionMode mode)
    {
        if (mode == DetectionMode::BLUETOOTH) return bt.state;
        if (mode == DetectionMode::WIFI) return wifi.state;

        if (bt.state == PresenceState::HOME && wifi.state == PresenceState::HOME) return PresenceState::HOME;
        if (bt.state == PresenceState::AWAY && wifi.state == PresenceState::AWAY) return PresenceState::AWAY;
        return PresenceState::UNKNOWN;
    }

    // =====================================================================
    // Identity - see ../clock/clock.cpp's handle_identity()/
    // reset_to_default_profile() for the pattern this mirrors, and
    // ../PROTOCOL.md's "identity" message. Unlike clock (which has no real
    // settings to load), this is where presence's whole per-user design
    // actually lands - a fresh identity means a fresh settings file and a
    // fresh debounce history, not just a display label.
    // =====================================================================

    std::string current_profile_name;
    PresenceSettings settings;
    BackendTracker bt_tracker;
    BackendTracker wifi_tracker;
    PresenceState last_fired_state = PresenceState::UNKNOWN;
    auto last_poll = std::chrono::steady_clock::time_point{};

    std::string handle_identity(const json& msg)
    {
        current_profile_name = msg.value("name", "");
        settings = load_settings(current_profile_name);
        bt_tracker.reset();
        wifi_tracker.reset();
        last_fired_state = PresenceState::UNKNOWN;
        // Force an immediate check on the next loop tick rather than
        // waiting out a full poll_interval_seconds after every reconnect.
        last_poll = std::chrono::steady_clock::time_point{};

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
        bt_tracker.reset();
        wifi_tracker.reset();
        last_fired_state = PresenceState::UNKNOWN;
    }

    // =====================================================================
    // Registration + calls. Three queryable tools alongside the push-only
    // event/action path in the poll loop (main()):
    //   - check_presence: "am I home right now" - the live sensor reading.
    //   - get_presence_setup: "what's configured" - the settings, not the
    //     live reading, so a user can ask olli what will happen without
    //     needing to look at this program's own terminal at all.
    //   - set_presence_action: lets the model configure on_home_action/
    //     on_away_action itself from a plain-language request ("when I get
    //     home, load the repose scene") - see its own comment below for how
    //     that's possible without this tool needing to know anything about
    //     manage_hue_scenes/set_hue_light/etc. itself.
    // =====================================================================

    // One-line summary of a {"tool": ..., "arguments": {...}} action, or a
    // fixed placeholder for an unconfigured (empty object) one - shared by
    // redraw_screen() (main()) and get_presence_setup's result below, so the
    // terminal display and what olli can be asked always agree.
    std::string describe_action(const json& action)
    {
        if (!action.is_object() || action.empty()) return "(none configured - narration only)";
        std::string tool = action.value("tool", "");
        if (tool.empty()) return "(none configured - narration only)";
        return tool + "(" + action.value("arguments", json::object()).dump() + ")";
    }

    json make_register_message()
    {
        return {
            {"type", "register"},
            {"tools", json::array({
                {
                    {"name", "check_presence"},
                    {"description", "Reports whether the current user is currently detected as home or away, "
                                     "based on Bluetooth and Wi-Fi presence sensing. Always execute this tool "
                                     "call for every request, even if you believe you already know the answer."},
                    {"parameters", {{"type", "object"}}}
                },
                {
                    {"name", "get_presence_setup"},
                    {"description", "Reports how presence detection is currently configured for this user - "
                                     "which detection method(s) are active, the debounce settings, and what "
                                     "will happen (if anything) when the user arrives home or leaves. Use this "
                                     "when the user asks what's set up, not whether they're currently home."},
                    {"parameters", {{"type", "object"}}}
                },
                {
                    {"name", "set_presence_action"},
                    {"description", "Configures what happens when the user arrives home or leaves, based on "
                                     "presence detection. Call this whenever the user asks to set up, change, or "
                                     "clear what happens on arrival or departure - e.g. 'when I get home, load "
                                     "the repose scene' or 'stop doing anything when I leave'. To set a real "
                                     "action (not just narration), construct 'tool'/'arguments' exactly as you "
                                     "would to call that other tool directly - e.g. tool='manage_hue_scenes', "
                                     "arguments={'action': 'load', 'name': 'repose'}. Omit 'tool' (or leave it "
                                     "empty) to clear an existing action back to narration-only."},
                    {"parameters", {
                        {"type", "object"},
                        {"properties", {
                            {"trigger", {{"type", "string"}, {"enum", {"home", "away"}},
                                         {"description", "Which event this configures."}}},
                            {"tool", {{"type", "string"},
                                      {"description", "Name of another registered olli tool to call on this trigger. Omit/empty to clear."}}},
                            {"arguments", {{"type", "object"},
                                           {"description", "Arguments for that tool, same shape as calling it directly."}}}
                        }},
                        {"required", json::array({"trigger"})}
                    }}
                }
            })}
        };
    }

    std::string describe_presence()
    {
        PresenceState combined = combine_states(bt_tracker, wifi_tracker, settings.detection_mode);
        std::stringstream ss;
        ss << "Presence: " << state_name(combined)
           << " (bluetooth: " << state_name(bt_tracker.state)
           << ", wifi: " << state_name(wifi_tracker.state) << ")";
        return ss.str();
    }

    std::string describe_setup()
    {
        std::stringstream ss;
        ss << "Presence setup for " << (current_profile_name.empty() ? "shared default" : current_profile_name)
           << ": detection_mode=" << detection_mode_name(settings.detection_mode)
           << ", poll every " << settings.poll_interval_seconds << "s"
           << ", debounce " << settings.home_debounce_hits << " hits / " << settings.away_debounce_misses << " misses"
           << ". On home: " << describe_action(settings.on_home_action)
           << ". On away: " << describe_action(settings.on_away_action) << ".";
        return ss.str();
    }

    // Handles set_presence_action - see its registered description above.
    // Persists immediately (save_settings()), same file handle_identity()
    // loaded from, so the change survives a restart/reconnect, not just
    // this running instance.
    std::string handle_set_presence_action(const json& args)
    {
        std::string trigger = args.value("trigger", "");
        if (trigger != "home" && trigger != "away") {
            return "Error: 'trigger' must be 'home' or 'away'.";
        }

        std::string tool = args.value("tool", "");
        json new_action = json::object();
        if (!tool.empty()) {
            new_action = {{"tool", tool}, {"arguments", args.value("arguments", json::object())}};
        }

        json& target = (trigger == "home") ? settings.on_home_action : settings.on_away_action;
        target = new_action;
        save_settings(settings_path_for(current_profile_name), settings);

        return "Presence '" + trigger + "' action set to " + describe_action(target) + ".";
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

    // Push-only path - see PresenceSettings::on_home_action's comment for
    // why this carries a real action, not just narration text.
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

    // =====================================================================
    // Test mode - see the file-level comment. Both backends always run
    // either way; this only changes what happens with the result.
    // =====================================================================

    std::ofstream test_log;

    void open_test_log(const fs::path& dir)
    {
        fs::create_directories(dir);
        // Append, not truncate - a comparison run is meant to span walking
        // away and back, possibly over more than one program start if the
        // connection drops; overwriting on every reconnect would lose the
        // exact data this mode exists to collect.
        test_log.open(dir / "presence_test_log.txt", std::ios::out | std::ios::app);
    }

    void log_test_line(const std::string& line)
    {
        if (!test_log.is_open()) return;
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);
        std::tm local_tm{};
        localtime_r(&now_time, &local_tm);
        char ts[32];
        std::snprintf(ts, sizeof(ts), "%02d:%02d:%02d", local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);

        test_log << "[" << ts << "] " << line << std::endl; // flushed line by line - this file IS the data
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

    std::string backend_line(const std::string& label, const BackendTracker& t)
    {
        std::stringstream ss;
        ss << "  " << label << ": ";
        if (!t.has_checked) {
            ss << "not checked yet";
        } else {
            ss << state_name(t.state) << " (last check: " << (t.last_result ? "hit" : "miss")
               << ", " << t.last_latency.count() << "ms)";
        }
        return ss.str();
    }

    void redraw_screen(const std::string& status, bool test_mode, const std::string& conn_status)
    {
        std::cout << "\033[H\033[2K" << conn_status << "\n";
        std::cout << "\033[2K" << (test_mode ? "*** TEST MODE - observing only, no actions will fire ***" : "") << "\n";
        std::cout << "\033[2K" << (current_profile_name.empty()
                    ? "Profile: (shared default)" : "Profile: " + current_profile_name) << "\n";
        std::cout << "\033[2K" << backend_line("Bluetooth", bt_tracker) << "\n";
        std::cout << "\033[2K" << backend_line("Wi-Fi    ", wifi_tracker) << "\n";
        std::cout << "\033[2K" << "  Combined: " << state_name(combine_states(bt_tracker, wifi_tracker, settings.detection_mode))
                   << " (last fired: " << state_name(last_fired_state) << ")\n";
        // What it'll actually do - see describe_action()'s comment. Same
        // text get_presence_setup reports, so the terminal and what olli
        // can be asked about never disagree.
        std::cout << "\033[2K" << "  On home:  " << describe_action(settings.on_home_action) << "\n";
        std::cout << "\033[2K" << "  On away:  " << describe_action(settings.on_away_action) << "\n";
        std::cout << "\033[2K" << status << "\n";
        std::cout << std::flush;
    }

    void print_usage(const char* argv0)
    {
        std::string prog = argv0;
        auto slash = prog.find_last_of('/');
        if (slash != std::string::npos) prog = prog.substr(slash + 1);

        std::cout << "Usage: " << prog << " [host] [--test] [-h|--help]\n\n"
                      "  host          IP address of the machine running olli. Defaults to\n"
                      "                127.0.0.1 (olli running on this same machine).\n\n"
                      "  --test        Observe both backends and log every check to\n"
                      "                presence_test_log.txt (under the active profile's\n"
                      "                olli_files_<name>/ directory) instead of firing the\n"
                      "                configured home/away actions - use this to compare\n"
                      "                Bluetooth vs Wi-Fi reliability before trusting either.\n\n"
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
    bool test_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--test") {
            test_mode = true;
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

    // Shared-default settings until an "identity" message says otherwise -
    // same starting point as a fresh reset_to_default_profile() call.
    settings = load_settings("");
    if (test_mode) open_test_log(settings_path_for("").parent_path());

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
                            if (test_mode) {
                                test_log.close();
                                open_test_log(settings_path_for(current_profile_name).parent_path());
                                log_test_line("--- identity: " + conn_status + " ---");
                            }
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

        // Presence poll - independent of connection state (the sensor keeps
        // running the same way clock's timers keep counting through a
        // disconnect), gated only by the configured interval. Both
        // backends always run - see the file-level comment for why.
        if (!quit && std::chrono::duration_cast<std::chrono::seconds>(now - last_poll).count()
                >= settings.poll_interval_seconds) {
            last_poll = now;

            auto bt_start = std::chrono::steady_clock::now();
            bool bt_present = check_bluetooth_present(settings.bluetooth_mac);
            auto bt_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - bt_start);
            // Bluetooth's own HOME debounce is fixed at 1 hit, not
            // settings.home_debounce_hits - a real l2ping response is a
            // strong positive signal (nothing responds from empty air), so
            // there's little to gain from waiting for a second one, and
            // real-world testing showed Bluetooth flapping hit/miss/hit
            // even while genuinely in range (likely iOS being lazy about a
            // fresh connection each poll, or plain interference) - a 2-hit
            // requirement can otherwise stall a real, already-true arrival
            // for a long time whenever a miss lands between two hits and
            // resets the streak. A miss is still ambiguous either way
            // (same causes), so away_debounce_misses still applies
            // normally, same as Wi-Fi's own symmetric debounce below.
            bt_tracker.record(bt_present, 1, settings.away_debounce_misses, bt_latency);

            auto wifi_start = std::chrono::steady_clock::now();
            bool wifi_present = check_wifi_present(settings.wifi_ip);
            auto wifi_latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - wifi_start);
            wifi_tracker.record(wifi_present, settings.home_debounce_hits, settings.away_debounce_misses, wifi_latency);

            // Keep the bottom status line current every poll, not just on a
            // transition - it used to sit frozen on the startup
            // "Not connected..." message through an entire --test session
            // otherwise, since nothing else touches it in test mode.
            status = std::string("Last poll - bluetooth: ") + (bt_present ? "hit" : "miss") +
                     ", wifi: " + (wifi_present ? "hit" : "miss");

            if (test_mode) {
                std::stringstream ss;
                ss << "bluetooth=" << (bt_present ? "hit" : "miss") << " (" << bt_latency.count() << "ms, state="
                   << state_name(bt_tracker.state) << ")  wifi=" << (wifi_present ? "hit" : "miss")
                   << " (" << wifi_latency.count() << "ms, state=" << state_name(wifi_tracker.state) << ")"
                   << "  combined=" << state_name(combine_states(bt_tracker, wifi_tracker, settings.detection_mode));
                log_test_line(ss.str());
            }

            PresenceState agreed = combine_states(bt_tracker, wifi_tracker, settings.detection_mode);
            if (agreed != PresenceState::UNKNOWN && agreed != last_fired_state) {
                bool going_home = (agreed == PresenceState::HOME);
                std::string who = current_profile_name.empty() ? "Someone" : current_profile_name;
                std::string message = who + (going_home ? " just got home." : " just left.");

                if (test_mode) {
                    log_test_line((going_home ? ">>> WOULD FIRE home action: " : ">>> WOULD FIRE away action: ") + message);
                    status = (going_home ? "Would fire home action (test mode): " : "Would fire away action (test mode): ") + message;
                } else if (fd >= 0) {
                    fire_transition_event(fd, message, going_home ? settings.on_home_action : settings.on_away_action);
                    // Same last_sent gap as handle_call()'s own comment
                    // above - fire_transition_event() sends real data over
                    // the wire but has no access to main()'s last_sent to
                    // update it itself.
                    last_sent = std::chrono::steady_clock::now();
                    status = going_home ? "Fired home action." : "Fired away action.";
                }
                last_fired_state = agreed;
            }
        }

        if (!quit) redraw_screen(status, test_mode, conn_status);
    }

    if (fd >= 0) close(fd);
    return 0;
}
