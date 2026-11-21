#include "health.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace
{
    constexpr size_t MAX_EVENTS = 200;
    constexpr size_t MAX_CRASH_LOG_BYTES_READ = 1024 * 1024;   // only the end of the file matters, and it stays small

    // Limits behind attention_items() - judgement calls, kept together so they are easy to change.
    constexpr long long LOW_DISK_BYTES = 5LL * 1024 * 1024 * 1024;   // under 5 GB free...
    constexpr double LOW_DISK_FRACTION = 0.10;                        // ...or under 10 % free
    constexpr long long HIGH_MEMORY_KB = 2LL * 1024 * 1024;           // olli normally sits around 150 MB
    constexpr int MANY_THREADS = 100;                                  // olli normally has about 22
    constexpr int MANY_OPEN_FILES = 500;                               // olli normally has about 65
    constexpr int FAILED_REQUESTS_LIMIT = 3;                           // failed model requests before it is worth a mention

    std::string to_text(long long value) { return std::to_string(value); }

    bool is_digit(char c) { return c >= '0' && c <= '9'; }

    // "YYYY-MM-DD HH:MM:SS - olli was " at the start of a line (what append_crash_log() in main.cpp writes).
    bool parse_crash_line(const std::string& line, std::time_t& when, std::string& text)
    {
        static const std::string MARKER = " - olli was ";
        // 19 characters of timestamp, then the marker.
        if (line.size() < 19 + MARKER.size()) return false;
        const int digit_positions[] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
        for (int p : digit_positions) {
            if (!is_digit(line[static_cast<size_t>(p)])) return false;
        }
        if (line[4] != '-' || line[7] != '-' || line[10] != ' ' || line[13] != ':' || line[16] != ':') return false;
        if (line.compare(19, MARKER.size(), MARKER) != 0) return false;

        auto number = [&](size_t at, size_t length) {
            int value = 0;
            for (size_t i = 0; i < length; ++i) value = value * 10 + (line[at + i] - '0');
            return value;
        };
        // mktime() would quietly turn month 13 or day 99 into some later date - refuse those instead.
        const int month = number(5, 2), day = number(8, 2), hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
        if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return false;
        std::tm parts{};
        parts.tm_year = number(0, 4) - 1900;
        parts.tm_mon = number(5, 2) - 1;
        parts.tm_mday = number(8, 2);
        parts.tm_hour = number(11, 2);
        parts.tm_min = number(14, 2);
        parts.tm_sec = number(17, 2);
        parts.tm_isdst = -1;
        when = std::mktime(&parts);
        if (when == static_cast<std::time_t>(-1)) return false;
        text = line.substr(22);   // after " - ": "olli was terminated by signal 11 (...)"
        // The supervisor appends " (crash 1/3 within the last 30s)" - its crash-loop counter, noise here.
        const size_t counter = text.find(" (crash ");
        if (counter != std::string::npos) text.erase(counter);
        return true;
    }

    long long file_size_or_unknown(const fs::path& path)
    {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        if (ec) return -1;
        return static_cast<long long>(size);
    }

    std::string read_file_tail(const fs::path& path, size_t max_bytes)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return "";
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        if (size <= 0) return "";
        const std::streamoff start = size > static_cast<std::streamoff>(max_bytes) ? size - static_cast<std::streamoff>(max_bytes) : 0;
        file.seekg(start, std::ios::beg);
        std::string text(static_cast<size_t>(size - start), '\0');
        file.read(&text[0], static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<size_t>(file.gcount()));
        return text;
    }

    // Plain-text file written through a temp file and renamed over the real one, so a reader (the MCP file server,
    // olli's own chat) never sees half of it.
    bool write_file_atomically(const fs::path& path, const std::string& text)
    {
        fs::path temp = path.parent_path() / ("." + path.filename().string() + ".tmp");
        {
            std::ofstream file(temp, std::ios::binary | std::ios::trunc);
            if (!file) return false;
            file << text;
            file.flush();
            if (!file) return false;
        }
        std::error_code ec;
        fs::rename(temp, path, ec);
        if (ec) { fs::remove(temp, ec); return false; }
        return true;
    }

    // What this process can read about itself (Linux: /proc/self). Anything unavailable stays -1.
    void read_process_facts(olli_health::SNAPSHOT& snapshot)
    {
#ifdef __linux__
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.compare(0, 6, "VmRSS:") == 0) snapshot.memory_kb = std::atoll(line.c_str() + 6);
            else if (line.compare(0, 8, "Threads:") == 0) snapshot.threads = std::atoi(line.c_str() + 8);
        }
        std::error_code ec;
        int count = 0;
        for (fs::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end; it.increment(ec)) ++count;
        if (!ec && count > 0) snapshot.open_files = count - 1;   // the iterator's own descriptor
#else
        (void)snapshot;
#endif
    }
}

// ---------------------------------------------------------------------------------------------------------------
// pure functions

namespace
{
    // Zero-initialised statics: usable from any thread at any time, including before main() finishes starting things.
    std::atomic<int> g_error_counts[olli_health::ERROR_KIND_COUNT];
    std::atomic<std::time_t> g_last_error_at{0};

    const char* const ERROR_DESCRIPTIONS[olli_health::ERROR_KIND_COUNT] = {
        "tool calls where no connected tool offered the tool",
        "tool calls that got no answer in time",
        "failed requests to Ollama",
        "failed requests to Gemini",
        "errors silently swallowed while reading Ollama's reply",
        "errors silently swallowed during a chat turn",
        "errors in the voice-input callback",
        "times second-guess hit its round limit and was parked"
    };

    int sum_of(const olli_health::ERROR_TOTALS& totals, std::initializer_list<olli_health::ERROR_KIND> kinds)
    {
        int sum = 0;
        for (auto kind : kinds) sum += totals.counts[static_cast<int>(kind)];
        return sum;
    }
}

namespace olli_health
{
    void note(ERROR_KIND kind)
    {
        const int index = static_cast<int>(kind);
        if (index < 0 || index >= ERROR_KIND_COUNT) return;
        g_error_counts[index].fetch_add(1, std::memory_order_relaxed);
        g_last_error_at.store(std::time(nullptr), std::memory_order_relaxed);
    }

    ERROR_TOTALS error_totals()
    {
        ERROR_TOTALS totals;
        for (int i = 0; i < ERROR_KIND_COUNT; ++i) totals.counts[i] = g_error_counts[i].load(std::memory_order_relaxed);
        totals.last_at = g_last_error_at.load(std::memory_order_relaxed);
        return totals;
    }

    const char* error_kind_description(int index)
    {
        if (index < 0 || index >= ERROR_KIND_COUNT) return "unknown";
        return ERROR_DESCRIPTIONS[index];
    }

    CRASH_SUMMARY summarize_crash_log(const std::string& log_text, std::time_t now)
    {
        CRASH_SUMMARY summary;
        std::istringstream lines(log_text);
        std::string line;
        while (std::getline(lines, line)) {
            std::time_t when = 0;
            std::string text;
            if (!parse_crash_line(line, when, text)) continue;
            ++summary.total;
            const double age = std::difftime(now, when);
            if (age < 24.0 * 3600.0) ++summary.last_24_hours;
            if (age < 7.0 * 24.0 * 3600.0) ++summary.last_7_days;
            if (when >= summary.last_crash_time) {
                summary.last_crash_time = when;
                summary.last_crash_text = text;
            }
        }
        return summary;
    }

    std::string format_duration(long long seconds)
    {
        if (seconds < 0) seconds = 0;
        if (seconds < 60) return to_text(seconds) + " s";
        const long long minutes = seconds / 60;
        if (minutes < 60) return to_text(minutes) + " min";
        const long long hours = minutes / 60;
        if (hours < 24) return to_text(hours) + " h " + to_text(minutes % 60) + " min";
        return to_text(hours / 24) + (hours / 24 == 1 ? " day " : " days ") + to_text(hours % 24) + " h";
    }

    std::string format_bytes(long long bytes)
    {
        if (bytes < 0) return "unknown";
        char buffer[48];
        const double value = static_cast<double>(bytes);
        if (bytes < 1024) std::snprintf(buffer, sizeof buffer, "%lld B", bytes);
        else if (bytes < 1024LL * 1024) std::snprintf(buffer, sizeof buffer, "%.1f KB", value / 1024.0);
        else if (bytes < 1024LL * 1024 * 1024) std::snprintf(buffer, sizeof buffer, "%.1f MB", value / (1024.0 * 1024.0));
        else std::snprintf(buffer, sizeof buffer, "%.1f GB", value / (1024.0 * 1024.0 * 1024.0));
        return buffer;
    }

    std::string format_time(std::time_t time)
    {
        std::tm local{};
        localtime_r(&time, &local);
        char buffer[32];
        std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M:%S", &local);
        return buffer;
    }

    std::vector<ATTENTION_ITEM> attention_items(const SNAPSHOT& s)
    {
        std::vector<ATTENTION_ITEM> items;

        if (s.crashes.last_24_hours > 0) {
            items.push_back({"recent crash", "olli crashed " + to_text(s.crashes.last_24_hours) + (s.crashes.last_24_hours == 1 ? " time" : " times") +
                             " in the last 24 hours (last: " + format_time(s.crashes.last_crash_time) + ", " + s.crashes.last_crash_text + ")"});
        }
        if (s.disk_free_bytes >= 0 && s.disk_total_bytes > 0) {
            const double fraction = static_cast<double>(s.disk_free_bytes) / static_cast<double>(s.disk_total_bytes);
            if (s.disk_free_bytes < LOW_DISK_BYTES || fraction < LOW_DISK_FRACTION) {
                char percent[16];
                std::snprintf(percent, sizeof percent, "%.0f", fraction * 100.0);
                items.push_back({"low disk space", "disk space is low: " + format_bytes(s.disk_free_bytes) + " free (" + percent + " %)"});
            }
        }
        if (s.memory_kb >= HIGH_MEMORY_KB) {
            items.push_back({"high memory use", "olli is using a lot of memory: " + format_bytes(s.memory_kb * 1024)});
        }
        if (s.threads >= MANY_THREADS) {
            items.push_back({"many threads", "olli has an unusually high number of threads: " + to_text(s.threads)});
        }
        if (s.open_files >= MANY_OPEN_FILES) {
            items.push_back({"many open files", "olli has an unusually high number of open files: " + to_text(s.open_files)});
        }

        // Errors since this run started. Swallowed errors are never expected, so one is enough; a failed model request
        // can be a passing network or server hiccup, so it takes a few.
        const int swallowed = sum_of(s.errors, {ERROR_KIND::OLLAMA_STREAM_EXCEPTION, ERROR_KIND::CHAT_TURN_EXCEPTION, ERROR_KIND::VOICE_CALLBACK_EXCEPTION});
        if (swallowed > 0) {
            items.push_back({"swallowed errors", "errors were silently swallowed " + to_text(swallowed) + (swallowed == 1 ? " time" : " times") + " since olli started"});
        }
        const int failed_requests = sum_of(s.errors, {ERROR_KIND::OLLAMA_REQUEST_FAILED, ERROR_KIND::GEMINI_REQUEST_FAILED});
        if (failed_requests >= FAILED_REQUESTS_LIMIT) {
            items.push_back({"failing model requests", "requests to the AI model failed " + to_text(failed_requests) + " times since olli started"});
        }
        const int timeouts = s.errors.counts[static_cast<int>(ERROR_KIND::TOOL_CALL_TIMEOUT)];
        if (timeouts > 0) {
            items.push_back({"tool call timeouts", to_text(timeouts) + (timeouts == 1 ? " tool call" : " tool calls") + " got no answer in time since olli started"});
        }
        const int capped = s.errors.counts[static_cast<int>(ERROR_KIND::SECOND_GUESS_CAP)];
        if (capped > 0) {
            items.push_back({"second-guess loop", "second-guess hit its round limit " + to_text(capped) + (capped == 1 ? " time" : " times") + " since olli started"});
        }
        return items;
    }

    std::string format_status(const SNAPSHOT& s)
    {
        std::ostringstream out;
        out << "Olli health status - written " << format_time(s.written_at) << " (rewritten every minute; if this time is old, olli is not running)\n";
        out << "Profile: " << (s.profile_name.empty() ? "(shared default)" : s.profile_name) << "\n";
        out << "Olli started: " << format_time(s.started_at) << " (running for " << format_duration(static_cast<long long>(std::difftime(s.written_at, s.started_at))) << ")\n";
        out << "This run started after a crash: " << (s.started_after_crash ? "yes" : "no") << "\n";

        out << "Crashes on record (crash_log.txt): " << s.crashes.total << " in total, " << s.crashes.last_24_hours << " in the last 24 hours, "
            << s.crashes.last_7_days << " in the last 7 days.";
        if (s.crashes.last_crash_time != 0) out << " Last crash: " << format_time(s.crashes.last_crash_time) << " - " << s.crashes.last_crash_text << ".";
        out << "\n";

        out << "Memory in use: " << (s.memory_kb >= 0 ? format_bytes(s.memory_kb * 1024) : std::string("unknown"))
            << ". Threads: " << (s.threads >= 0 ? to_text(s.threads) : std::string("unknown"))
            << ". Open files: " << (s.open_files >= 0 ? to_text(s.open_files) : std::string("unknown")) << ".\n";

        out << "Disk space free where the profile lives: ";
        if (s.disk_free_bytes >= 0 && s.disk_total_bytes > 0) {
            char percent[16];
            std::snprintf(percent, sizeof percent, "%.0f", 100.0 * static_cast<double>(s.disk_free_bytes) / static_cast<double>(s.disk_total_bytes));
            out << format_bytes(s.disk_free_bytes) << " of " << format_bytes(s.disk_total_bytes) << " (" << percent << " %).\n";
        } else {
            out << "unknown.\n";
        }
        out << "File sizes: history.json " << format_bytes(s.history_bytes) << ", debug_full_history.txt " << format_bytes(s.debug_log_bytes)
            << ", crash_log.txt " << format_bytes(s.crash_log_bytes) << ".\n";

        std::string error_list;
        for (int i = 0; i < ERROR_KIND_COUNT; ++i) {
            if (s.errors.counts[i] <= 0) continue;
            error_list += (error_list.empty() ? "" : "; ") + to_text(s.errors.counts[i]) + " " + error_kind_description(i);
        }
        if (error_list.empty()) out << "Errors since this run started: none.\n";
        else out << "Errors since this run started: " << error_list << ". Last one at " << format_time(s.errors.last_at) << ".\n";

        const auto items = attention_items(s);
        if (items.empty()) {
            out << "Needs attention: nothing.\n";
        } else {
            out << "Needs attention:\n";
            for (const auto& item : items) out << "- " << item.text << "\n";
        }
        return out.str();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// the thread

HEALTH_CLASS::~HEALTH_CLASS()
{
    // A destructor that joins: if main_process() unwinds through an exception, the thread must still be stopped
    // before this object goes away (a joinable std::thread destroyed = std::terminate).
    stop_requested = true;
    if (worker.joinable()) worker.join();
}

void HEALTH_CLASS::thread_start(const fs::path& profile_dir_in, const std::string& profile_name_in, bool started_after_crash_in, int update_seconds_in)
{
    if (running) return;
    profile_dir = profile_dir_in;
    health_dir = profile_dir_in / "collection" / "Olli_Health";
    profile_name = profile_name_in;
    started_after_crash = started_after_crash_in;
    update_seconds = update_seconds_in > 0 ? update_seconds_in : 60;
    started_at = std::time(nullptr);
    stop_requested = false;
    running = true;
    try {
        worker = std::thread([this]() { thread_main(); });
    } catch (...) {
        running = false;   // could not start a thread: olli carries on without a health report
    }
}

void HEALTH_CLASS::thread_stop()
{
    if (!running) return;
    stop_requested = true;
    if (worker.joinable()) worker.join();
    running = false;
    // The thread is gone, so nothing else touches events now.
    try {
        add_event("olli stopped normally");
        write_events_file();
    } catch (...) {}
}

void HEALTH_CLASS::thread_main()
{
    try {
        std::error_code ec;
        fs::create_directories(health_dir, ec);

        // Carry the earlier events over so the list survives restarts.
        {
            std::ifstream existing(health_dir / "events.txt");
            std::string line;
            while (std::getline(existing, line)) {
                if (!line.empty() && is_digit(line[0])) events.push_back(line);   // skips the header line
            }
            if (events.size() > MAX_EVENTS) events.erase(events.begin(), events.end() - static_cast<std::ptrdiff_t>(MAX_EVENTS));
        }

        if (started_after_crash) {
            const olli_health::CRASH_SUMMARY crashes = olli_health::summarize_crash_log(read_file_tail(profile_dir / "crash_log.txt", MAX_CRASH_LOG_BYTES_READ), std::time(nullptr));
            add_event("olli started again after a crash" + (crashes.last_crash_time != 0 ? " (" + crashes.last_crash_text + ")" : std::string()));
        } else {
            add_event("olli started");
        }
        write_events_file();
    } catch (...) {}

    auto next_write = std::chrono::steady_clock::now();
    while (!stop_requested) {
        if (std::chrono::steady_clock::now() >= next_write) {
            try { write_status_file(); } catch (...) {}
            next_write = std::chrono::steady_clock::now() + std::chrono::seconds(update_seconds);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void HEALTH_CLASS::write_status_file()
{
    olli_health::SNAPSHOT snapshot;
    snapshot.profile_name = profile_name;
    snapshot.written_at = std::time(nullptr);
    snapshot.started_at = started_at;
    snapshot.started_after_crash = started_after_crash;
    snapshot.crashes = olli_health::summarize_crash_log(read_file_tail(profile_dir / "crash_log.txt", MAX_CRASH_LOG_BYTES_READ), snapshot.written_at);
    read_process_facts(snapshot);
    snapshot.errors = olli_health::error_totals();

    // An error counted since the last look becomes an event (with the running total), so "what happened" survives
    // the next rewrite of status.txt. At most once a minute per kind, however many errors there were.
    bool errors_noted = false;
    for (int i = 0; i < olli_health::ERROR_KIND_COUNT; ++i) {
        const int now_count = snapshot.errors.counts[i];
        if (now_count > errors_seen[i]) {
            add_event("error: " + to_text(now_count - errors_seen[i]) + " more of " + olli_health::error_kind_description(i) + " (" + to_text(now_count) + " since olli started)");
            errors_seen[i] = now_count;
            errors_noted = true;
        }
    }

    std::error_code ec;
    const fs::space_info space = fs::space(profile_dir, ec);
    if (!ec) {
        snapshot.disk_free_bytes = static_cast<long long>(space.available);
        snapshot.disk_total_bytes = static_cast<long long>(space.capacity);
    }
    snapshot.history_bytes = file_size_or_unknown(profile_dir / "history.json");
    snapshot.debug_log_bytes = file_size_or_unknown(profile_dir / "debug_full_history.txt");
    snapshot.crash_log_bytes = file_size_or_unknown(profile_dir / "crash_log.txt");

    // Things that start or stop needing attention become events, so "what happened" survives past the next rewrite.
    const auto items = olli_health::attention_items(snapshot);
    bool events_changed = false;
    for (const auto& item : items) {
        if (std::find(known_attention.begin(), known_attention.end(), item.label) == known_attention.end()) {
            add_event("needs attention: " + item.text);
            events_changed = true;
        }
    }
    for (const auto& label : known_attention) {
        const bool still = std::any_of(items.begin(), items.end(), [&](const olli_health::ATTENTION_ITEM& item) { return item.label == label; });
        if (!still) {
            add_event("no longer a problem: " + label);
            events_changed = true;
        }
    }
    known_attention.clear();
    for (const auto& item : items) known_attention.push_back(item.label);
    if (events_changed || errors_noted) write_events_file();

    fs::create_directories(health_dir, ec);   // someone may have deleted the folder while olli ran
    write_file_atomically(health_dir / "status.txt", olli_health::format_status(snapshot));
}

void HEALTH_CLASS::add_event(const std::string& text)
{
    events.push_back(olli_health::format_time(std::time(nullptr)) + " - " + text);
    if (events.size() > MAX_EVENTS) events.erase(events.begin(), events.end() - static_cast<std::ptrdiff_t>(MAX_EVENTS));
}

void HEALTH_CLASS::write_events_file()
{
    std::error_code ec;
    fs::create_directories(health_dir, ec);
    std::string text = "Olli health events (oldest first, newest last; the most recent " + std::to_string(MAX_EVENTS) + " are kept)\n";
    for (const auto& event : events) text += event + "\n";
    write_file_atomically(health_dir / "events.txt", text);
}
