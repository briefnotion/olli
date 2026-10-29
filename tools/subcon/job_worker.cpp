#include "job_worker.hpp"

#include <unistd.h>

JOB_WORKER::JOB_WORKER(int wake_fd_) : wake_fd(wake_fd_)
{
    thread = std::thread([this]() { run(); });
}

JOB_WORKER::~JOB_WORKER()
{
    stop();
}

bool JOB_WORKER::submit(JOB job)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (pending.has_value() || running || finished.has_value() || stopping.load()) return false;
        pending = std::move(job);
        // A fresh job starts un-aborted. Cleared here, under the mutex and
        // before the worker can see the job, not when run() picks it up:
        // found in review 2026-10-29 that clearing at dequeue lost an
        // abort_current() arriving between the two, so a job abandoned
        // right after it was submitted ran to completion. (stop() holds the
        // mutex while it sets 'stopping', and the check above refuses once
        // it is set, so this can't undo a shutdown abort.)
        abort_any.store(false);
    }
    wake.notify_one();
    return true;
}

bool JOB_WORKER::take_result(JOB_RESULT& out)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!finished.has_value()) return false;
    out = std::move(*finished);
    finished.reset();
    return true;
}

bool JOB_WORKER::busy()
{
    std::lock_guard<std::mutex> lock(mutex);
    return pending.has_value() || running || finished.has_value();
}

void JOB_WORKER::abort_current()
{
    abort_any.store(true);
}

void JOB_WORKER::stop()
{
    {
        // Under the mutex so the worker can't test its wait predicate, see
        // "not stopping", and then block just after our notify (a lost wakeup).
        std::lock_guard<std::mutex> lock(mutex);
        stopping.store(true);
        abort_any.store(true);
    }
    wake.notify_all();
    if (thread.joinable()) thread.join();
}

void JOB_WORKER::run()
{
    while (true) {
        JOB job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [this]() { return pending.has_value() || stopping.load(); });
            if (stopping.load()) return;
            job = std::move(*pending);
            pending.reset();
            running = true;
        }

        JOB_RESULT result;
        result.kind = job.kind;
        result.id = job.id;

        // An uncaught exception on a std::thread is std::terminate for the
        // whole process (the failure mode behind the old subcon's crashes):
        // anything thrown here becomes a failed result instead.
        try {
            if (job.kind == JOB::CHAT) {
                result.reply = ollama_chat(job.settings, job.messages, job.tools, job.format, abort_any);
            } else {
                for (const JOB::CALL& call : job.calls)
                    result.tool_results.push_back(run_internal_tool(call.name, call.arguments, job.api_key, abort_any));
            }
        } catch (const std::exception& e) {
            result.reply.ok = false;
            result.reply.error = std::string("worker threw: ") + e.what();
            result.tool_results.assign(job.calls.size(), INTERNAL_TOOL_RESULT{false, std::string("Error: worker threw: ") + e.what()});
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            finished = std::move(result);
            running = false;
        }
        char byte = 1;
        ssize_t written = write(wake_fd, &byte, 1);
        (void)written; // a full pipe just means the main loop is already due to wake
    }
}
