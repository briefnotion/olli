#include "helper_presence.hpp"

#include <cstdio>
#include <cstdlib>
#include <cctype>

namespace {
    // Bounds each backend's own subprocess call - see presence.cpp's poll
    // loop for why these need to stay short: both checks run synchronously,
    // back to back, on the same thread that also owns the socket/heartbeat/
    // display, so a poll tick blocks everything else for up to roughly the
    // sum of these two. 3s worst case, on a poll interval measured in tens
    // of seconds by default, comfortably clears TOOL_REMOTE's own
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
    // presence.cpp's socket-read loop long enough for olli's own
    // TOOL_REMOTE::check() (5s timeout) to spuriously mark a perfectly
    // healthy connection dead - observed for real, not hypothetical.
    constexpr int BLUETOOTH_PING_TIMEOUT_SECONDS = 2;
    constexpr int WIFI_PING_TIMEOUT_SECONDS = 1;

    // Both check_*_present() functions below take the raw value straight
    // from a JSON settings file and hand it to popen()/system() - a loose
    // allow-list check first (hex digits/colons for a MAC, digits/dots for
    // an IPv4 address) is cheap insurance against a malformed settings file
    // putting shell metacharacters on a command line, even though the file
    // is only ever hand-edited locally, never attacker-supplied over the
    // network.
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

    // Pings a known, paired classic-Bluetooth MAC - see presence.cpp's
    // file-level comment for why classic BT (not BLE scanning) is what's
    // reliable here. Needs the phone paired with this machine at least once
    // first (bluetoothctl pair <mac>), and l2ping needs raw-socket
    // privilege - see README.md. A malformed/empty MAC or a failed ping
    // both just read as "not present" - there's no separate error channel
    // to the caller, matching every other best-effort check here.
    bool check_bluetooth_present(const std::string& mac)
    {
        if (!looks_like_mac(mac)) return false;

        // Wrapped in the coreutils `timeout` command as a hard wall-clock
        // cap, not just l2ping's own -t - a real miss was observed taking
        // ~5s despite -t 2 during testing (likely Bluetooth link-
        // establishment overhead underneath l2ping's own echo timeout,
        // which -t doesn't cover). Without this, a slow poll can stall
        // presence.cpp's socket-read loop long enough for olli's own
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
}

BluetoothBackend::BluetoothBackend(std::string mac, std::chrono::seconds poll_interval, std::chrono::seconds search_poll_interval)
    : address(std::move(mac)), poll_interval(poll_interval), search_poll_interval(search_poll_interval)
{}

void BluetoothBackend::poll_is_near()
{
    auto now = std::chrono::steady_clock::now();
    auto current_interval = (near && !searching_since.has_value()) ? poll_interval : search_poll_interval;
    if (now - last_poll_time < current_interval) return; // not due yet - skip the real check
    last_poll_time = now;

    if (check_bluetooth_present(address)) {
        near = true;
        searching_since.reset(); // hit - confirmed near (or a fresh arrival), back to the normal rate
    } else if (near) {
        // Was near, just missed - start (or continue) searching rather than
        // flipping away on a single miss.
        if (!searching_since.has_value()) searching_since = now;
        if (now - *searching_since >= poll_interval) {
            near = false; // searched the full window, never found them - confirmed away
            searching_since.reset();
        }
    }
    // else: already away and still away - nothing to do, stay at the normal rate.
}

WifiBackend::WifiBackend(std::string ip, std::chrono::seconds poll_interval, std::chrono::seconds search_poll_interval)
    : address(std::move(ip)), poll_interval(poll_interval), search_poll_interval(search_poll_interval)
{}

void WifiBackend::poll_is_near()
{
    auto now = std::chrono::steady_clock::now();
    auto current_interval = (near && !searching_since.has_value()) ? poll_interval : search_poll_interval;
    if (now - last_poll_time < current_interval) return; // not due yet - skip the real check
    last_poll_time = now;

    if (check_wifi_present(address)) {
        near = true;
        searching_since.reset(); // hit - confirmed near (or a fresh arrival), back to the normal rate
    } else if (near) {
        // Was near, just missed - start (or continue) searching rather than
        // flipping away on a single miss.
        if (!searching_since.has_value()) searching_since = now;
        if (now - *searching_since >= poll_interval) {
            near = false; // searched the full window, never found them - confirmed away
            searching_since.reset();
        }
    }
    // else: already away and still away - nothing to do, stay at the normal rate.
}

PersonProfile::PersonProfile(std::string name) : name(std::move(name)) {}

void PersonProfile::add_bluetooth_backend(BluetoothBackend backend)
{
    bluetooth_backends.push_back(std::move(backend));
}

void PersonProfile::add_wifi_backend(WifiBackend backend)
{
    wifi_backends.push_back(std::move(backend));
}

void PersonProfile::poll()
{
    for (BluetoothBackend& backend : bluetooth_backends) {
        backend.poll_is_near();
    }
    for (WifiBackend& backend : wifi_backends) {
        backend.poll_is_near();
    }

    bool new_is_near = false;
    for (BluetoothBackend& backend : bluetooth_backends) {
        if (backend.is_near()) new_is_near = true;
    }
    for (WifiBackend& backend : wifi_backends) {
        if (backend.is_near()) new_is_near = true;
    }

    triggered = (new_is_near != is_near);
    is_near = new_is_near;
}
