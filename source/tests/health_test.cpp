// Stand-alone test of source/health.cpp (not part of the olli build). From the source/ folder:
//   g++ -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Werror -pthread -o tests/health_test tests/health_test.cpp health.cpp && ./tests/health_test
#include "../health.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;
static int fails = 0;
static void check(bool ok, const std::string& what)
{
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++fails;
}
static std::string slurp(const fs::path& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main()
{
    using namespace olli_health;

    std::printf("== crash log parsing\n");
    std::tm base{};
    base.tm_year = 2026 - 1900; base.tm_mon = 10; base.tm_mday = 21; base.tm_hour = 12; base.tm_isdst = -1;
    const std::time_t now = std::mktime(&base);
    const std::string log =
        "2026-09-21 08:02:37 - olli was terminated by signal 11 (Segmentation fault) (crash 1/3 within the last 30s)\n"
        "2026-11-07 06:03:37 - olli was exited abnormally (code 1) (crash 1/3 within the last 30s)\n"
        "olli received signal 11\n"
        "  /home/x/olli(+0x1234) [0x55]\n"
        "2026-11-20 23:30:00 - olli was terminated by signal 6 (Aborted) (crash 1/3 within the last 30s)\n"
        "2026-11-20 23:30:05 - 3 crashes within 30s - giving up, not retrying further.\n"
        "garbage line\n"
        "2026-13-99 99:99:99 - olli was nonsense\n";
    CRASH_SUMMARY c = summarize_crash_log(log, now);
    check(c.total == 3, "3 crash lines counted (the giving-up line, backtrace, garbage and the impossible date are not): " + std::to_string(c.total));
    check(c.last_24_hours == 1, "1 in the last 24 hours: " + std::to_string(c.last_24_hours));
    check(c.last_7_days == 1 && c.last_crash_text.find("signal 6") != std::string::npos, "last crash is the signal 6 one: " + c.last_crash_text);
    CRASH_SUMMARY none = summarize_crash_log("", now);
    check(none.total == 0 && none.last_crash_time == 0, "empty log -> nothing");

    std::printf("== formatting\n");
    check(format_duration(45) == "45 s" && format_duration(720) == "12 min" && format_duration(3 * 3600 + 50 * 60) == "3 h 50 min" && format_duration(2 * 86400 + 4 * 3600) == "2 days 4 h" && format_duration(-5) == "0 s", "durations");
    check(format_bytes(512) == "512 B" && format_bytes(87040) == "85.0 KB" && format_bytes(1610612736LL) == "1.5 GB" && format_bytes(-1) == "unknown", "bytes");

    std::printf("== attention\n");
    SNAPSHOT healthy;
    healthy.written_at = now; healthy.started_at = now - 3600;
    healthy.memory_kb = 150000; healthy.threads = 22; healthy.open_files = 65;
    healthy.disk_free_bytes = 100LL * 1024 * 1024 * 1024; healthy.disk_total_bytes = 400LL * 1024 * 1024 * 1024;
    check(attention_items(healthy).empty(), "a healthy snapshot raises nothing");
    check(format_status(healthy).find("Needs attention: nothing.") != std::string::npos, "status says nothing needs attention");
    SNAPSHOT unknown;   // every number unknown (-1): must not raise anything
    check(attention_items(unknown).empty(), "unknown numbers raise nothing");
    SNAPSHOT bad = healthy;
    bad.crashes = c; bad.disk_free_bytes = 2LL * 1024 * 1024 * 1024; bad.memory_kb = 3LL * 1024 * 1024; bad.threads = 150; bad.open_files = 900;
    auto items = attention_items(bad);
    check(items.size() == 5, "all five problems raised: " + std::to_string(items.size()));
    check(format_status(bad).find("Needs attention:\n- olli crashed 1 time") != std::string::npos, "status lists them");
    SNAPSHOT lowpct = healthy; lowpct.disk_total_bytes = 400LL * 1024 * 1024 * 1024; lowpct.disk_free_bytes = 30LL * 1024 * 1024 * 1024;
    check(attention_items(lowpct).size() == 1, "30 GB free of 400 GB (7 %) is low by percentage");


    std::printf("== error counters\n");
    {
        ERROR_TOTALS before = error_totals();
        check(before.last_at == 0 && before.counts[0] == 0, "starts at zero");
        note(ERROR_KIND::TOOL_NOT_AVAILABLE);
        note(ERROR_KIND::TOOL_NOT_AVAILABLE);
        note(ERROR_KIND::SECOND_GUESS_CAP);
        note(static_cast<ERROR_KIND>(99));   // out of range: ignored, no crash
        note(static_cast<ERROR_KIND>(-1));
        ERROR_TOTALS t = error_totals();
        check(t.counts[0] == 2 && t.counts[7] == 1 && t.last_at != 0, "counted per kind, timestamp set");
        // many threads at once: atomic, no lost increments
        std::vector<std::thread> pool;
        for (int i = 0; i < 8; ++i) pool.emplace_back([]() { for (int n = 0; n < 5000; ++n) note(ERROR_KIND::VOICE_CALLBACK_EXCEPTION); });
        for (auto& th : pool) th.join();
        check(error_totals().counts[6] == 40000, "8 threads x 5000 notes = 40000: " + std::to_string(error_totals().counts[6]));

        SNAPSHOT quiet = healthy;   // errors all zero
        check(format_status(quiet).find("Errors since this run started: none.") != std::string::npos, "no errors -> says none");
        SNAPSHOT s1 = healthy;
        s1.errors.counts[static_cast<int>(ERROR_KIND::TOOL_NOT_AVAILABLE)] = 2;   // not an attention item by itself
        check(attention_items(s1).empty(), "tool-not-available alone is not an attention item");
        check(format_status(s1).find("2 tool calls where no connected tool offered the tool") != std::string::npos, "but it is counted in the status");
        SNAPSHOT s2 = healthy;
        s2.errors.counts[static_cast<int>(ERROR_KIND::CHAT_TURN_EXCEPTION)] = 1;
        auto a2 = attention_items(s2);
        check(a2.size() == 1 && a2[0].label == "swallowed errors", "one swallowed error is an attention item");
        SNAPSHOT s3 = healthy;
        s3.errors.counts[static_cast<int>(ERROR_KIND::OLLAMA_REQUEST_FAILED)] = 1;
        s3.errors.counts[static_cast<int>(ERROR_KIND::GEMINI_REQUEST_FAILED)] = 1;
        check(attention_items(s3).empty(), "two failed model requests are not yet");
        s3.errors.counts[static_cast<int>(ERROR_KIND::GEMINI_REQUEST_FAILED)] = 2;
        check(attention_items(s3).size() == 1, "three are");
        SNAPSHOT s4 = healthy;
        s4.errors.counts[static_cast<int>(ERROR_KIND::TOOL_CALL_TIMEOUT)] = 1;
        s4.errors.counts[static_cast<int>(ERROR_KIND::SECOND_GUESS_CAP)] = 1;
        check(attention_items(s4).size() == 2, "a tool timeout and a second-guess cap each raise one");
    }

    std::printf("== the thread (real files, temp folder)\n");
    char templ[] = "/tmp/health_test_XXXXXX";
    const fs::path dir = mkdtemp(templ);
    { std::ofstream(dir / "crash_log.txt") << "2026-11-20 23:30:00 - olli was terminated by signal 11 (Segmentation fault) (crash 1/3 within the last 30s)\n"; }
    // an older events file from a previous run, with its header line
    fs::create_directories(dir / "collection" / "Olli_Health");
    { std::ofstream(dir / "collection" / "Olli_Health" / "events.txt") << "Olli health events (header)\n2026-11-19 10:00:00 - olli started\n"; }
    {
        HEALTH_CLASS health;
        health.thread_start(dir, "testprofile", true, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        note(ERROR_KIND::CHAT_TURN_EXCEPTION);   // arrives while the thread runs
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const std::string status = slurp(dir / "collection" / "Olli_Health" / "status.txt");
        check(status.find("errors silently swallowed during a chat turn") != std::string::npos, "an error noted while running shows in status.txt");
        check(status.find("- errors were silently swallowed ") != std::string::npos, "and raises the attention item");
        check(status.find("Profile: testprofile") != std::string::npos, "status.txt written with the profile");
        check(status.find("This run started after a crash: yes") != std::string::npos, "knows the run followed a crash");
        check(status.find("Threads: ") != std::string::npos && status.find("Threads: unknown") == std::string::npos, "reads its own thread count from /proc");
        check(!fs::exists(dir / "collection" / "Olli_Health" / ".status.txt.tmp"), "no temp file left behind");
        const auto t0 = std::chrono::steady_clock::now();
        health.thread_stop();
        check(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500), "thread_stop is quick");
        health.thread_stop();   // twice is fine
    }
    const std::string events = slurp(dir / "collection" / "Olli_Health" / "events.txt");
    check(events.find("2026-11-19 10:00:00 - olli started") != std::string::npos, "earlier events kept");
    check(events.find("started again after a crash (olli was terminated by signal 11") != std::string::npos, "start event after a crash names the crash");
    check(events.find("olli stopped normally") != std::string::npos, "clean stop is recorded");
    check(events.find("error: 1 more of errors silently swallowed during a chat turn") != std::string::npos, "the error became an event");
    check(events.find("(header)") == std::string::npos && events.find("Olli health events (oldest first") == 0, "the old header line was not carried over as an event");

    std::printf("== unwritable place: no crash, no throw\n");
    {
        HEALTH_CLASS health;
        health.thread_start("/proc/definitely/not/writable", "x", false, 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        health.thread_stop();
        check(true, "survived an unwritable folder");
    }
    {
        HEALTH_CLASS never_started;   // destructor without start
        check(true, "destroying a never-started object is fine");
    }

    fs::remove_all(dir);
    std::printf("\n%s\n", fails == 0 ? "ALL OK" : "SOME FAILED");
    return fails == 0 ? 0 : 1;
}
