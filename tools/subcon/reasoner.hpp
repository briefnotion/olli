// SUBCON_REASONER - subcon's reasoning loop and its note queue: the
// prioritize -> act (tool calls) -> finding cycle, ported from the old
// in-olli subcon (source/subcon_worker.cpp's thread_main() stage machine)
// minus everything about pushing a finding to olli (DELIVERING/ANNOUNCING,
// exchange(), COMMS) - delivery is pull-only now, see pull_notes().
//
// Runs entirely on the main thread. tick() advances at most one stage step
// per call and never waits: a model request or a batch of internal tool
// calls is handed to JOB_WORKER and its result comes back through
// on_job_result(); a remote tool call goes through SUBCON_TOOL_SERVER and
// completes through a callback. So a slow model never delays the network
// servicing that shares this thread, and no state here is ever touched by
// another thread (the worker only sees a copied job).
//
// What replaced olli's own tool plumbing: in the old design the model's tool
// calls went through ollama_system::process() and a tool_worker, and a
// result came back via integrate_tool_result() narration - which is why the
// old stage machine needed its "wait for outstanding_tool_call_ids and for a
// second call arriving inside narration" rules. Here the loop is explicit
// and standard: send messages + tools, run EVERY tool call in the reply,
// append the role=tool results, ask again, until a reply has no tool calls
// (capped at max_tool_rounds). Those waits are structural now: the next
// request is never sent until every call of the previous reply has an
// answer, so a late or second call can't be missed.

#pragma once

#include "job_worker.hpp"
#include "subcon_store.hpp"
#include "tool_server.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

// file_only: true for detail that is wanted in the cycle log file but would
// swamp the console/status display (full model thinking, tool result text).
using SUBCON_LOG_FN = std::function<void(const std::string& line, bool file_only)>;

struct REASONER_CONFIG {
    OLLAMA_SETTINGS ollama;
    std::string web_search_api_key;

    // Master switch for starting new cycles (the old SUBCON_THINK_CYCLE_ENABLED).
    // False stops the reasoning; notes already queued can still be pulled.
    bool think_cycle_enabled = true;
    // Test only (the old SUBCON_FORCE_RUN_FOR_TESTING): ignore the model's
    // "nothing worth doing" and run the stalest allowed task.
    bool force_run = false;
    // Test only: treat every task as allowed regardless of cap/min-gap, so a
    // long soak isn't stopped by a 4-hour spacing rule. Never for real use.
    bool ignore_limits = false;

    int first_cycle_delay_seconds = 60; // same as the old think_cycle_timer's initial 60s
    int rethink_seconds = 60;           // gap after a finished or failed cycle (old: 60000ms)
    int max_tool_rounds = 6;            // model<->tool round trips per cycle
};

// What a get_subcon_notes call answers with.
struct SUBCON_PULL_RESULT {
    std::string text;
    std::string special_instruction;
    size_t note_count = 0;
};

class SUBCON_REASONER {
    public:
        SUBCON_REASONER(SUBCON_TOOL_SERVER& server, JOB_WORKER& worker, SUBCON_LOG_FN log, REASONER_CONFIG config);

        // Replaces the settings (model, search key, switches). Called when the
        // profile - and with it its settings file - becomes known; takes
        // effect from the next cycle.
        void set_config(REASONER_CONFIG new_config) { config = std::move(new_config); }

        // Loads (or reloads, on a profile switch) the persisted task list and
        // note queue from state_dir, abandoning any cycle in progress first.
        // Nothing runs until this has been called once.
        void load_state(const std::filesystem::path& state_dir);
        bool state_loaded() const { return loaded; }

        // Advances the machine one step; cheap when nothing is due.
        void tick();

        // A finished JOB_WORKER job (any - results for an abandoned or stale
        // job are recognised by id and dropped).
        void on_job_result(JOB_RESULT& result);

        // The user's presence, "near" or "away" (anything else is ignored by
        // the arrival tracker). Fed by the presence tool's user.presence
        // events and by probe_presence().
        void on_presence(const std::string& value, const std::string& source);

        // If presence is still unknown, asks the presence tool (check_presence)
        // once. The old in-olli subcon only learned presence from transition
        // events, so after a restart it stayed "unknown" until the user next
        // came or went (SUBCON.md, "Known limitations"); a startup probe fixes that.
        void probe_presence_if_unknown();

        // Answers get_subcon_notes. Instant by construction: reads the
        // persisted queue and builds one digest in code - never touches the
        // model or the worker - and marks what it returns as delivered.
        SUBCON_PULL_RESULT pull_notes();

        // For the display.
        std::string stage_description() const;
        std::string presence() const { return presence_value; }
        size_t pending_note_count() const { return subcon_count_pending(queue); }
        std::string last_finding() const { return last_finding_text; }

        // Abandons a cycle in progress (profile switch, shutdown): cancels its
        // outstanding tool calls, aborts its job, and returns to IDLE.
        void abandon_cycle(const std::string& why);

    private:
        enum class STAGE { IDLE, PRIORITIZING, ACTING_MODEL, ACTING_TOOLS, FINDING };

        // One model-requested tool call and its eventual answer.
        struct CALL_SLOT {
            std::string tool_name;
            nlohmann::json arguments;
            bool dispatched = false;   // an offered tool actually ran (counts as "a real tool was used")
            bool plain = false;        // a short instruction to the model, sent as-is rather than wrapped as a tool failure
            bool done = false;
            bool ok = false;
            std::string text;
            std::string server_call_id; // remote calls only, for abandon()
        };

        using steady = std::chrono::steady_clock;

        void log(const std::string& line, bool file_only = false) { log_fn(line, file_only); }
        int64_t unix_now() const;
        void schedule_next_cycle(int seconds);

        void start_cycle(int64_t now_unix);
        bool submit_chat(const nlohmann::json& tools, const nlohmann::json& format);
        void handle_prioritized(const OLLAMA_REPLY& reply);
        void begin_act();
        void handle_act_reply(const OLLAMA_REPLY& reply);
        void dispatch_calls(const std::vector<OLLAMA_TOOL_CALL>& calls);
        void maybe_finish_tool_round();
        void finish_act();
        void handle_finding(const OLLAMA_REPLY& reply);
        void end_cycle_without_finding(const std::string& history_text);
        void record_attempt_result(const std::string& finding, bool noteworthy);
        void save_items();
        void save_queue();

        nlohmann::json build_offered_tools(bool log_it);
        // missing_tools: the required tools that kept an otherwise-allowed
        // task out of the returned list (see subcon_missing_tools()).
        std::vector<size_t> eligible_indices(int64_t now_unix, std::vector<SUBCON_ELIGIBILITY>& eligibility, std::set<std::string>& missing_tools) const;
        std::set<std::string> available_tool_names() const;
        std::string build_prioritize_prompt(const std::vector<size_t>& eligible, const std::vector<SUBCON_ELIGIBILITY>& eligibility, int64_t now_unix) const;
        std::string build_act_prompt(const SUBCON_TODO_ITEM& item) const;
        std::string build_finding_prompt(const SUBCON_TODO_ITEM& item, int64_t now_unix) const;

        // May a new cycle start right now? The seam where a "the user is
        // busy talking to olli" signal belongs when one exists (the old
        // subcon gated on main_busy_level < 10, COMMS::busy_count()). By
        // design (the user, 2026-10-29: "no gating for now") it always says
        // yes - see the definition's comment.
        bool may_start_cycle() const;

        SUBCON_TOOL_SERVER& server;
        JOB_WORKER& worker;
        SUBCON_LOG_FN log_fn;
        REASONER_CONFIG config;

        bool loaded = false;
        std::filesystem::path items_path, retired_path, queue_path;
        std::vector<SUBCON_TODO_ITEM> items;
        std::vector<SUBCON_NOTE> queue;

        std::string presence_value = "unknown";
        SUBCON_PRESENCE_TRACKER presence_tracker;
        bool presence_probe_inflight = false;

        // The missing-tools set last written to the log, so a task list
        // waiting on a tool that isn't connected logs once, not every minute.
        std::set<std::string> last_logged_missing_tools;

        STAGE stage = STAGE::IDLE;
        steady::time_point next_cycle_at;
        steady::time_point next_housekeeping_at;

        // Bumped whenever a cycle ends or is abandoned. Callbacks and jobs
        // from an earlier cycle see a different token and do nothing.
        uint64_t cycle_token = 0;
        uint64_t next_job_id = 1;
        uint64_t expected_job_id = 0; // 0: no job outstanding

        // --- per-cycle state ---
        nlohmann::json conversation = nlohmann::json::array();
        size_t chosen_index = 0;
        std::string chosen_reasoning;
        std::string chosen_plan;
        bool cycle_unsafe = false;
        bool nudged_about_fake_output = false;
        int rounds = 0;
        int tools_dispatched = 0;
        int tools_ok = 0;
        size_t tool_bytes_used = 0; // role=tool content added to this cycle's conversation so far
        std::vector<std::string> tool_names_used;
        std::set<std::string> offered_names;
        std::vector<CALL_SLOT> slots;
        bool internal_batch_pending = false;
        std::vector<size_t> internal_slot_indices;

        std::string last_finding_text;
};
