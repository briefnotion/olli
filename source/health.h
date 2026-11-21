#ifndef HEALTH_H
#define HEALTH_H

// Olli's quiet self-report: a small background thread that, once a minute, writes a plain-English status of how
// olli itself is doing into the profile's collection folder, and keeps a short rolling list of notable events.
//
//   ~/olli_files_<profile>/collection/Olli_Health/status.txt   - the current picture, rewritten every minute
//   ~/olli_files_<profile>/collection/Olli_Health/events.txt   - notable things with timestamps, last 200 lines
//
// Why in collection/: the MCP bridge's file server is rooted there, so olli's own chat and subcon (through the
// bridge's read-only file tools) can read these files with no new tool and no new code. RAG does NOT index it: RAG
// only imports folders that were created as collections, so this folder is just reported as "a folder with no
// matching collection" in rag_tool's sync status line.
//
// Isolation (deliberate): this module reads only things it can see by itself (the process's own /proc entries, the
// profile folder, crash_log.txt) and writes only into its own folder. It shares no state with any other part of olli,
// holds no lock anyone else uses, and every failure inside it is swallowed inside it - a broken health file must
// never cost the user a conversation. Nothing in the rest of olli calls into it except main.cpp, which starts and
// stops the thread.
//
// Phase 1: facts olli can read about itself. Phase 2 (added 2026-11-21): counters of errors seen since this run
// started. Existing code reports an error with ONE line, olli_health::note(olli_health::ERROR_KIND::...) - an atomic
// increment, no lock, no allocation, never throws - and the thread above turns the totals into status lines, events
// and attention items.
//
// The free functions in namespace olli_health are pure (no files, no clock unless a time is passed in) so they can be
// tested on their own - see source/tests/health_test.cpp.

#include <atomic>
#include <ctime>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace olli_health
{
    // The places in olli that report an error to this module (one note() call each, in existing code). Order matters:
    // it is the index into the counters and into error_kind_description().
    enum class ERROR_KIND : int
    {
        TOOL_NOT_AVAILABLE = 0,        // tool_worker: a call for a tool nothing connected offers (answered with an error after 3 s)
        TOOL_CALL_TIMEOUT,             // tool_worker: a call nobody answered within CALL_TIMEOUT_SECONDS
        OLLAMA_REQUEST_FAILED,         // ollama_system: the request to Ollama failed (streaming or not)
        GEMINI_REQUEST_FAILED,         // gemini_system: the request to Gemini failed after its retries
        OLLAMA_STREAM_EXCEPTION,       // ollama_system: the blanket catch(...) around one streamed chunk
        CHAT_TURN_EXCEPTION,           // ai_root: the blanket catch(...) around a whole chat turn
        VOICE_CALLBACK_EXCEPTION,      // io_worker: voice input's callback threw
        SECOND_GUESS_CAP               // sidetrack: second-guess reached its round limit and was parked
    };
    constexpr int ERROR_KIND_COUNT = 8;

    // Thread-safe from any thread: an atomic increment and a timestamp. Never throws, never blocks, never allocates.
    void note(ERROR_KIND kind);

    struct ERROR_TOTALS
    {
        int counts[ERROR_KIND_COUNT] = {};
        std::time_t last_at = 0;       // when the most recent error of any kind was noted (0 = none)
    };
    ERROR_TOTALS error_totals();       // totals since this process started

    // "tool calls where no connected tool offered the tool", ... - reads after a number: "3 tool calls where ...".
    const char* error_kind_description(int index);

    // What crash_log.txt says. The log is written by olli's supervisor (the parent process that restarts the
    // crashed child), so a crash never shows up as a counter inside the child - it has to be read from the file.
    struct CRASH_SUMMARY
    {
        int total = 0;
        int last_24_hours = 0;
        int last_7_days = 0;
        std::time_t last_crash_time = 0;     // 0 = none
        std::string last_crash_text;          // e.g. "olli was terminated by signal 11 (Segmentation fault)"
    };

    // Counts the lines the supervisor writes ("YYYY-MM-DD HH:MM:SS - olli was ..."); backtrace lines, the "giving up"
    // line and anything else in the file are ignored. 'now' is passed in so this stays testable.
    CRASH_SUMMARY summarize_crash_log(const std::string& log_text, std::time_t now);

    struct SNAPSHOT
    {
        std::string profile_name;            // "" = the shared default profile
        std::time_t written_at = 0;
        std::time_t started_at = 0;
        bool started_after_crash = false;
        CRASH_SUMMARY crashes;
        long long memory_kb = -1;            // -1 = unknown (every number below)
        int threads = -1;
        int open_files = -1;
        long long disk_free_bytes = -1;
        long long disk_total_bytes = -1;
        long long history_bytes = -1;
        long long debug_log_bytes = -1;
        long long crash_log_bytes = -1;
        ERROR_TOTALS errors;
    };

    // One thing worth a human's attention. 'label' is stable (used to notice when it appears and clears); 'text' is
    // the sentence, which may contain changing numbers.
    struct ATTENTION_ITEM
    {
        std::string label;
        std::string text;
    };

    // The judgement calls, in one place: a crash in the last 24 h, low disk space, a lot of memory, a lot of threads,
    // a lot of open files. Unknown numbers (-1) never raise anything.
    std::vector<ATTENTION_ITEM> attention_items(const SNAPSHOT& snapshot);

    // The status.txt text.
    std::string format_status(const SNAPSHOT& snapshot);

    std::string format_duration(long long seconds);      // "45 s", "12 min", "3 h 50 min", "2 days 4 h"
    std::string format_bytes(long long bytes);           // "512 B", "85.0 KB", "1.5 GB"
    std::string format_time(std::time_t time);           // local "YYYY-MM-DD HH:MM:SS"
}

class HEALTH_CLASS
{
    public:
        HEALTH_CLASS() = default;
        ~HEALTH_CLASS();
        HEALTH_CLASS(const HEALTH_CLASS&) = delete;
        HEALTH_CLASS& operator=(const HEALTH_CLASS&) = delete;

        // profile_dir: the profile's folder (~/olli_files_<name>). started_after_crash: main_process()'s
        // crash_restart flag. Writes the first status right away, then every update_seconds.
        void thread_start(const std::filesystem::path& profile_dir, const std::string& profile_name, bool started_after_crash, int update_seconds = 60);

        // Writes "olli stopped normally" to events.txt and joins the thread. Safe to call twice, or without a start.
        // Only a clean shutdown reaches this - after a crash that line is simply missing, which is the point.
        void thread_stop();

    private:
        void thread_main();
        void write_status_file();
        void add_event(const std::string& text);
        void write_events_file();

        std::thread worker;
        std::atomic<bool> stop_requested{false};
        bool running = false;

        // Set before the thread starts, read-only afterwards (except events/known_attention, which only the thread
        // touches - and thread_stop() once the thread is joined).
        std::filesystem::path profile_dir;
        std::filesystem::path health_dir;
        std::string profile_name;
        bool started_after_crash = false;
        int update_seconds = 60;
        std::time_t started_at = 0;

        std::vector<std::string> events;           // newest last, at most MAX_EVENTS
        std::vector<std::string> known_attention;  // labels currently raised
        int errors_seen[olli_health::ERROR_KIND_COUNT] = {};   // totals already turned into events
};

#endif // HEALTH_H
