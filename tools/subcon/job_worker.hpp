// JOB_WORKER - the one worker thread behind subcon's blocking work: a model
// request to Ollama, or a batch of internal tool calls (web search, page
// fetch). Both can take many seconds; the main thread must keep servicing
// the network meanwhile (olli's pull, the tools' heartbeats), so they live
// here.
//
// The handoff is deliberately the smallest thing that is correct, because
// the old in-olli subcon's worst bugs were all shared-state races between
// its thread and the chat's (TODO.md: the 2026-09-22 SIGSEGV, 2026-09-24
// SIGABRT, the response-read-without-is_processing-guard crash):
//
//   * The worker thread touches NOTHING but its own job and result. A job is
//     handed over BY VALUE (moved in under the mutex); the result comes back
//     the same way. No pointer or reference into the reasoner's state ever
//     crosses the boundary, so there is nothing for the worker to read
//     unguarded.
//   * One job at a time. submit() refuses while a previous job is pending,
//     running, or finished-but-not-yet-taken, so there is never a second
//     writer of the result slot.
//   * The only cross-thread flags are two std::atomic<bool> (abort_any,
//     stopping): abort_any is read by libcurl's progress callback so a long
//     request can be cut short; they carry no data.
//   * The worker tells the main loop a result is ready by writing one byte
//     to a pipe the main loop's select() already watches - no polling
//     latency and no shared flag to read racily.
//
// Shutdown (stop()): sets 'stopping' (which also aborts anything in
// flight), wakes the thread and JOINS it, so the std::thread is never
// destroyed joinable (the SIGABRT that hit the old subcon_llm). With the
// abort polled about once a second, a model call mid-flight delays shutdown
// by well under two seconds.

#pragma once

#include "internal_tools.hpp"
#include "ollama_client.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

struct JOB {
    enum Kind { CHAT, TOOLS } kind = CHAT;
    uint64_t id = 0;

    // CHAT
    OLLAMA_SETTINGS settings;
    nlohmann::json messages;
    nlohmann::json tools;   // null/empty: none offered
    nlohmann::json format;  // null/empty: free text

    // TOOLS
    struct CALL { std::string name; nlohmann::json arguments; };
    std::vector<CALL> calls;
    std::string api_key;
};

struct JOB_RESULT {
    JOB::Kind kind = JOB::CHAT;
    uint64_t id = 0;
    OLLAMA_REPLY reply;                          // CHAT
    std::vector<INTERNAL_TOOL_RESULT> tool_results; // TOOLS, one per call, same order
};

class JOB_WORKER {
    public:
        // wake_fd: write end of a (non-blocking) pipe; one byte is written
        // each time a result becomes available.
        explicit JOB_WORKER(int wake_fd);
        ~JOB_WORKER();

        JOB_WORKER(const JOB_WORKER&) = delete;
        JOB_WORKER& operator=(const JOB_WORKER&) = delete;

        // False if a job is already in the pipeline (see header comment).
        bool submit(JOB job);

        // Main thread: takes the finished result, if there is one.
        bool take_result(JOB_RESULT& out);

        bool busy();

        // Cuts the in-flight job short (its result still arrives, as a
        // cancelled failure, and must be taken). For abandoning a cycle.
        void abort_current();

        // Aborts, joins. Safe to call more than once.
        void stop();

    private:
        void run();

        int wake_fd;
        std::thread thread;
        std::mutex mutex;
        std::condition_variable wake;
        std::optional<JOB> pending;
        bool running = false;
        std::optional<JOB_RESULT> finished;

        // abort_any is what libcurl's progress callback polls: set by
        // abort_current() or stop(), cleared by submit() when it accepts the
        // next job (never by the worker - see there). 'stopping' is only written/tested under the mutex.
        std::atomic<bool> abort_any{false};
        std::atomic<bool> stopping{false};
};
