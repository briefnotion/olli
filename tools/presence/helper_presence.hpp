// Detection backends and supporting classes for olli's presence sensor -
// see ../PROTOCOL.md and presence.cpp's own top-of-file comment for the
// full design (why classic Bluetooth instead of BLE scanning, why two
// independent backends, how debouncing/agreement work).
//
// BluetoothBackend/WifiBackend each own their own identity and hold the raw
// result of their own most recent check, rate-limited by poll_interval - no
// debouncing of their own (see their own comment). Debounce/trigger-on-change
// lives one level up, in PersonProfile::poll() - see its own comment.
// PersonProfile holds a name plus every backend watching for that person.
// This is what presence.cpp's main() loop actually polls - see
// poll_all_people()/run_triggers() there.

#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <chrono>
#include <optional>

// =============================================================================
// Self-contained per-backend trackers - what presence.cpp's main() loop
// actually polls (see poll_all_people()/run_triggers() there). Each holds
// its own identity and the raw result of its own most recent check - no
// debounce, no threshold, nothing time-based beyond poll_interval (which
// only rate-limits how often the real check runs - see below - it doesn't
// affect what near means). The idea being that adding a new way to detect
// someone's home later (a router API, an NFC tag, whatever) means writing
// one more class shaped like these two, not touching a shared free-function
// debounce path. The raw checks themselves (check_bluetooth_present()/
// check_wifi_present()) are an implementation detail of helper_presence.cpp
// now - nothing outside it calls them directly anymore.
//
// Each has exactly two entry points: poll_is_near() is safe to call as
// often as you like (e.g. every main-loop tick) - it only actually runs the
// real check (a subprocess call that can take up to a couple seconds - see
// helper_presence.cpp) once the current interval has passed since the last
// one; in between it's a no-op. is_near() reads back the cached raw result
// without checking anything or touching the clock.
//
// The interval isn't always poll_interval, though. The slow, default rate
// only applies while near is true and settled - anywhere else, checks run
// at the faster search_poll_interval (10s) instead:
//   - While away (near is false): no reason to be slow about noticing a
//     return, so it just keeps checking at the fast rate all the time.
//   - Right after a miss while near was true: same fast rate, but for a
//     different reason - a single miss doesn't immediately flip near
//     false. It starts a "searching" window (searching_since) that speeds
//     checks up for up to poll_interval (the same 2 minutes as the default
//     rate) from that first miss, actively trying to catch a hit before
//     giving up. Any hit cancels the search and goes straight back to
//     near=true at the normal slow rate; if the whole window elapses with
//     no hit, near finally flips false (and stays on the fast rate, per
//     the away case above).
// A hit is always trusted instantly regardless of which rate found it.
// =============================================================================

class BluetoothBackend {
    public:
        explicit BluetoothBackend(std::string mac,
                                   std::chrono::seconds poll_interval = std::chrono::seconds(120),
                                   std::chrono::seconds search_poll_interval = std::chrono::seconds(10));

        void poll_is_near();
        bool is_near() const { return near; }
        bool is_searching() const { return searching_since.has_value(); }

    private:
        std::string address;
        std::chrono::seconds poll_interval;
        std::chrono::seconds search_poll_interval;

        bool near = false;
        std::chrono::steady_clock::time_point last_poll_time{}; // epoch - so the first poll_is_near() call always actually checks
        std::optional<std::chrono::steady_clock::time_point> searching_since; // set while confirming a possible departure; unset = normal rate
};

class WifiBackend {
    public:
        explicit WifiBackend(std::string ip,
                              std::chrono::seconds poll_interval = std::chrono::seconds(120),
                              std::chrono::seconds search_poll_interval = std::chrono::seconds(10));

        void poll_is_near();
        bool is_near() const { return near; }
        bool is_searching() const { return searching_since.has_value(); }

    private:
        std::string address;
        std::chrono::seconds poll_interval;
        std::chrono::seconds search_poll_interval;

        bool near = false;
        std::chrono::steady_clock::time_point last_poll_time{}; // epoch - so the first poll_is_near() call always actually checks
        std::optional<std::chrono::steady_clock::time_point> searching_since; // set while confirming a possible departure; unset = normal rate
};

// One tracked person - a name plus every backend (Bluetooth, Wi-Fi, and
// whatever else gets added later) watching for them.
class PersonProfile {
    public:
        explicit PersonProfile(std::string name);

        void add_bluetooth_backend(BluetoothBackend backend);
        void add_wifi_backend(WifiBackend backend);

        // Polls every backend, then recomputes is_near as "any backend
        // near" (raw - see BluetoothBackend/WifiBackend's own comment, no
        // debounce happens down there anymore). triggered is set true only
        // on the poll where is_near actually changed value from what it was
        // before this call - false otherwise, including every poll where
        // nothing changed. This is the one place a transition is detected.
        void poll();

        std::string name;
        bool is_near = false;
        bool triggered = false;

        // {"tool": "...", "arguments": {...}} - same shape as PersonSettings'
        // on_home_action/on_away_action (presence.cpp). Empty object means
        // "narrate only, no action".
        nlohmann::json on_near_action = nlohmann::json::object();
        nlohmann::json on_away_action = nlohmann::json::object();

        std::vector<BluetoothBackend> bluetooth_backends;
        std::vector<WifiBackend> wifi_backends;
};
