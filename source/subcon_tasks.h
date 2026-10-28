#ifndef SUBCON_TASKS_H
#define SUBCON_TASKS_H

// Subcon's task model and the plain, time-based rules around it (how often
// a task may run, when a finding is worth surfacing, when a queued note may
// be said out loud). Header-only and free of any ollama_system / thread
// dependency on purpose: every function below takes "now" as a parameter
// instead of reading the clock, so the rules can be unit tested with
// invented timestamps (a real day-long cap can't be tested live in a
// reasonable time). thread_main() (subcon_worker.cpp) is the only place
// that wires these to the real clock and the real model.
//
// The split of responsibility is deliberate, same lesson as the wait-
// minutes clamp and the second-guess safety fix: the model's judgment
// decides *whether something is worth doing or saying right now*, but a
// mechanical rule decides what is even allowed to be asked about - a run
// cap or a quiet hour is never left to the model's own discretion.

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

// ---------------------------------------------------------------------
// Time rules
// ---------------------------------------------------------------------

// A task's run budget is measured over this long, or since the user last
// came home - whichever is more recent (subcon_window_start()).
constexpr int64_t SUBCON_WINDOW_SECONDS = 24 * 60 * 60;

// A "leave and come back" only counts as a new cycle if the user was
// actually gone for this long - presence can flap away/near for a moment
// (a known issue the presence tool itself debounces), and a flap should
// not hand every task a fresh budget.
constexpr int64_t SUBCON_MIN_AWAY_SECONDS = 10 * 60;

// Nothing is said out loud between these local hours (22:00 - 06:59),
// urgent or not. Anything queued waits for morning.
constexpr int SUBCON_QUIET_HOUR_START = 22;
constexpr int SUBCON_QUIET_HOUR_END = 7;

// A queued note nobody heard within this long is dropped, not read out -
// a weather note from yesterday afternoon is worse than no note.
constexpr int64_t SUBCON_NOTE_MAX_AGE_SECONDS = 18 * 60 * 60;

// Past this age the model no longer gets a vote on whether to wait longer:
// the same "judgment on top of a mechanical bound" rule as the wait clamp,
// so a note can't be deferred forever by a model that keeps saying "later."
constexpr int64_t SUBCON_NOTE_MAX_HOLD_SECONDS = 4 * 60 * 60;

// The model's own proposed delay before it's asked again about delivery.
constexpr int SUBCON_DELIVERY_WAIT_MINUTES_MIN = 5;
constexpr int SUBCON_DELIVERY_WAIT_MINUTES_MAX = 120;

// How many past runs each task keeps (the history is for display and
// "what did I tell the user already", not an archive).
constexpr size_t SUBCON_HISTORY_MAX = 20;

inline int subcon_local_hour(int64_t unix_seconds)
{
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
    localtime_r(&t, &local);
    return local.tm_hour;
}

inline bool subcon_is_quiet_hour(int local_hour)
{
    return local_hour >= SUBCON_QUIET_HOUR_START || local_hour < SUBCON_QUIET_HOUR_END;
}

// "Wednesday 3:20 PM" - for prompts that need the model to know the time
// of day (weather in the morning vs. the middle of the night).
inline std::string subcon_describe_local_time(int64_t unix_seconds)
{
    std::time_t t = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
    localtime_r(&t, &local);
    char buffer[64];
    std::strftime(buffer, sizeof(buffer), "%A %I:%M %p", &local);
    return buffer;
}

// ---------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------

// One past cycle where this item was chosen - the user's own idea (2026-
// 09-26): keep a real history per item, not just a last-checked scalar.
struct SUBCON_TODO_RESULT
{
    std::string reasoning; // why it was chosen that time
    std::string plan;      // what the act turn said it did (may be empty)
    std::string finding;   // the outcome, in plain sentences
    int64_t at_unix = 0;   // when this cycle finished (0 = recorded before this was tracked)
    bool noteworthy = false; // whether it was judged worth telling the user
};

// A task's fixed definition - what lives in code (subcon_task_defs()
// below), as opposed to SUBCON_TODO_ITEM, which also carries the running
// state that is persisted to disk.
struct SUBCON_TASK_DEF
{
    std::string key;         // stable identity - description can be reworded without losing history
    std::string label;       // short name used when the finding is read out
    std::string description; // what the task is, shown in the prioritize list and the act prompt
    std::string how_to;      // concrete tool hint appended to the act prompt
    int max_runs_per_window; // hard cap, see subcon_window_start(); 0 = unlimited
    int min_gap_minutes;     // minimum spacing between runs, regardless of the window
};

// One candidate for the prioritize stage - the fixed definition plus the
// persisted running state. last_checked_unix_seconds is
// std::chrono::system_clock (real wall time, meaningful across a restart),
// not steady_clock.
struct SUBCON_TODO_ITEM
{
    std::string key;
    std::string label;
    std::string description;
    std::string how_to;
    int max_runs_per_window = 0;
    int min_gap_minutes = 0;

    int64_t last_checked_unix_seconds = 0;
    bool ever_checked = false; // needed so "0 seconds ago" and "never" aren't confused
    std::vector<int64_t> run_times; // when each recent run started (an attempt counts, even a failed one)
    std::vector<SUBCON_TODO_RESULT> history;
};

// The tasks subcon works through. Authoritative here, not in the persisted
// file: reconcile_subcon_tasks() merges these into whatever was saved, so
// editing this list (and rebuilding) is how a task is added, reworded or
// retired - no hand-editing todo_items.json.
//
// Caps are per window (subcon_window_start()): at most max_runs_per_window
// runs in the last 24 hours, or since the user last came home if that was
// more recent. min_gap_minutes spaces them out so a 3-a-day task doesn't
// spend its budget in the first ten minutes, and also bounds the damage
// if presence flaps and hands out a fresh window.
inline const std::vector<SUBCON_TASK_DEF>& subcon_task_defs()
{
    static const std::vector<SUBCON_TASK_DEF> defs = {
        {"weather", "Weather",
         "Check the current weather and today's forecast for Lafayette, Louisiana",
         "Use web_search with a query like \"Lafayette Louisiana weather forecast today\". "
         "Only treat something as noteworthy if it is worth interrupting someone for: a severe "
         "weather alert, storms, flooding, extreme heat or cold, or a big change from last time. "
         "Ordinary conditions are not noteworthy.",
         3, 240},
        {"conversations", "Past conversations",
         "Review recent conversations with the user for anything left unresolved",
         "Use rag_search on the conversations collection (queries about open questions, "
         "unfinished plans, or things promised). Look for something the user asked that was never "
         "answered, or something Olli said it would do and didn't. The logs also hold automated "
         "self-review lines (such as \"needs_correction\" or a \"System Auditor Report\") - ignore "
         "those; only what the user actually said and how Olli answered counts. If everything "
         "was resolved, that is not noteworthy.",
         3, 240},
        {"notes", "Notes",
         "Check the user's notes for anything time-sensitive or coming up soon",
         "Use rag_search on the Notes collection for deadlines, appointments, or dates coming up "
         "soon. Notes with nothing time-sensitive are not noteworthy.",
         2, 360},
        {"lights", "Lights",
         "Check whether any lights were left on while the user is away",
         "Use check_presence first. Only if the user is away, use list_hue_lights and note which "
         "lights are on. If the user is home, nothing is noteworthy.",
         4, 120},
        {"timers", "Timers",
         "Check for timers that finished and were never acknowledged",
         "Use list_timers (read-only). A timer that finished long ago and was never dealt with is "
         "noteworthy; running timers that are not close to done are not.",
         2, 360}
    };
    return defs;
}

inline void to_json(json& j, const SUBCON_TODO_RESULT& result)
{
    j = json{{"reasoning", result.reasoning}, {"plan", result.plan}, {"finding", result.finding},
             {"at_unix", result.at_unix}, {"noteworthy", result.noteworthy}};
}

inline void from_json(const json& j, SUBCON_TODO_RESULT& result)
{
    result.reasoning = j.value("reasoning", "");
    result.plan = j.value("plan", "");
    result.finding = j.value("finding", "");
    result.at_unix = j.value("at_unix", static_cast<int64_t>(0));
    result.noteworthy = j.value("noteworthy", false);
}

inline void to_json(json& j, const SUBCON_TODO_ITEM& item)
{
    j = json{
        {"key", item.key},
        {"label", item.label},
        {"description", item.description},
        {"how_to", item.how_to},
        {"max_runs_per_window", item.max_runs_per_window},
        {"min_gap_minutes", item.min_gap_minutes},
        {"last_checked_unix_seconds", item.last_checked_unix_seconds},
        {"ever_checked", item.ever_checked},
        {"run_times", item.run_times},
        {"history", item.history}
    };
}

inline void from_json(const json& j, SUBCON_TODO_ITEM& item)
{
    // Everything read with .value(), so a file saved by an older version
    // (no key / how_to / caps / run_times) still loads - reconcile_subcon_
    // tasks() is what then decides what to do with an item that has no key.
    item.key = j.value("key", "");
    item.label = j.value("label", "");
    item.description = j.value("description", "");
    item.how_to = j.value("how_to", "");
    item.max_runs_per_window = j.value("max_runs_per_window", 0);
    item.min_gap_minutes = j.value("min_gap_minutes", 0);
    item.last_checked_unix_seconds = j.value("last_checked_unix_seconds", static_cast<int64_t>(0));
    item.ever_checked = j.value("ever_checked", false);
    item.run_times = j.value("run_times", std::vector<int64_t>{});
    item.history = j.value("history", std::vector<SUBCON_TODO_RESULT>{});

    // Real bug, caught live 2026-09-27 on the "ron" profile: a file
    // persisted by the old cycle-counting version of this struct has
    // "ever_checked": true but no last_checked_unix_seconds key at all,
    // which defaults to 0 above - the Unix epoch - and describe_staleness()
    // then reported "497357 hours ago." A real check can never have
    // happened at the epoch, so ever_checked = true with a still-zero
    // timestamp is unambiguously old-format leftover; treat it as never
    // checked rather than guessing a time to backfill.
    if (item.ever_checked && item.last_checked_unix_seconds == 0)
        item.ever_checked = false;
}

inline SUBCON_TODO_ITEM subcon_item_from_def(const SUBCON_TASK_DEF& def)
{
    SUBCON_TODO_ITEM item;
    item.key = def.key;
    item.label = def.label;
    item.description = def.description;
    item.how_to = def.how_to;
    item.max_runs_per_window = def.max_runs_per_window;
    item.min_gap_minutes = def.min_gap_minutes;
    return item;
}

// Brings a loaded task list in line with the definitions in code. For each
// definition: an item with the same key keeps its running state (last
// checked, run times, history) but takes the definition's current wording
// and limits; a definition with no matching item starts fresh. Anything
// loaded that matches no definition - including every item from before
// keys existed - is retired: removed from the live list and returned in
// 'retired' so the caller can archive it rather than silently discarding
// real history.
inline std::vector<SUBCON_TODO_ITEM> reconcile_subcon_tasks(const std::vector<SUBCON_TODO_ITEM>& loaded,
                                                            const std::vector<SUBCON_TASK_DEF>& defs,
                                                            std::vector<SUBCON_TODO_ITEM>& retired)
{
    std::vector<SUBCON_TODO_ITEM> result;
    std::vector<bool> used(loaded.size(), false);

    for (const SUBCON_TASK_DEF& def : defs)
    {
        SUBCON_TODO_ITEM item = subcon_item_from_def(def);
        for (size_t i = 0; i < loaded.size(); ++i)
        {
            if (used[i] || loaded[i].key.empty() || loaded[i].key != def.key) continue;
            used[i] = true;
            item.last_checked_unix_seconds = loaded[i].last_checked_unix_seconds;
            item.ever_checked = loaded[i].ever_checked;
            item.run_times = loaded[i].run_times;
            item.history = loaded[i].history;
            break;
        }
        result.push_back(item);
    }

    for (size_t i = 0; i < loaded.size(); ++i)
        if (!used[i]) retired.push_back(loaded[i]);

    return result;
}

// Where this task's budget starts counting: 24 hours ago, or the user's
// most recent real arrival home if that is later.
inline int64_t subcon_window_start(int64_t now_unix_seconds, int64_t last_arrival_unix_seconds)
{
    return std::max(now_unix_seconds - SUBCON_WINDOW_SECONDS, last_arrival_unix_seconds);
}

inline int subcon_runs_in_window(const SUBCON_TODO_ITEM& item, int64_t window_start_unix_seconds)
{
    int count = 0;
    for (int64_t t : item.run_times)
        if (t >= window_start_unix_seconds) ++count;
    return count;
}

struct SUBCON_ELIGIBILITY
{
    bool eligible = true;
    int runs_in_window = 0;
    int64_t next_eligible_unix = 0; // already <= now when eligible
};

// Whether the task may run right now, and if not, the earliest moment it
// could (ignoring a future arrival, which can only make that sooner). The
// cap and the minimum gap both have to allow it.
inline SUBCON_ELIGIBILITY subcon_check_eligibility(const SUBCON_TODO_ITEM& item, int64_t now_unix_seconds, int64_t last_arrival_unix_seconds)
{
    SUBCON_ELIGIBILITY e;
    int64_t window_start = subcon_window_start(now_unix_seconds, last_arrival_unix_seconds);

    std::vector<int64_t> in_window;
    for (int64_t t : item.run_times)
        if (t >= window_start) in_window.push_back(t);
    std::sort(in_window.begin(), in_window.end());
    e.runs_in_window = static_cast<int>(in_window.size());

    int64_t next = now_unix_seconds;

    if (item.min_gap_minutes > 0 && !item.run_times.empty())
    {
        int64_t last_run = *std::max_element(item.run_times.begin(), item.run_times.end());
        next = std::max(next, last_run + static_cast<int64_t>(item.min_gap_minutes) * 60);
    }

    if (item.max_runs_per_window > 0 && e.runs_in_window >= item.max_runs_per_window)
    {
        // The run that has to age out of the window before another is
        // allowed: counting from the oldest, the (runs - max)th one.
        size_t blocking = static_cast<size_t>(e.runs_in_window - item.max_runs_per_window);
        // +1: a run counts as inside the window while it is at most 24h old
        // (t >= window_start), so it only drops out the second after that.
        next = std::max(next, in_window[blocking] + SUBCON_WINDOW_SECONDS + 1);
    }

    e.next_eligible_unix = next;
    e.eligible = (next <= now_unix_seconds);
    return e;
}

// Same "never checked" / "very fresh" / "N minutes/hours ago" phrasing the
// earlier versions used - and the "0 minutes ago reads as never" special
// case for the very recent.
inline std::string subcon_describe_staleness(const SUBCON_TODO_ITEM& item, int64_t now_unix_seconds)
{
    if (!item.ever_checked) return "never checked";

    int64_t elapsed_seconds = now_unix_seconds - item.last_checked_unix_seconds;
    if (elapsed_seconds < 60) return "checked moments ago - very fresh";

    int64_t elapsed_minutes = elapsed_seconds / 60;
    if (elapsed_minutes < 60)
        return "last checked " + std::to_string(elapsed_minutes) + " minute(s) ago";

    int64_t elapsed_hours = elapsed_minutes / 60;
    return "last checked " + std::to_string(elapsed_hours) + " hour(s) ago";
}

// Records that a run is starting now, and drops run times too old to ever
// matter again (the window is 24h; two days is plenty of margin).
inline void subcon_record_run(SUBCON_TODO_ITEM& item, int64_t now_unix_seconds)
{
    item.run_times.push_back(now_unix_seconds);
    item.run_times.erase(std::remove_if(item.run_times.begin(), item.run_times.end(),
                                        [now_unix_seconds](int64_t t) { return t < now_unix_seconds - 2 * SUBCON_WINDOW_SECONDS; }),
                         item.run_times.end());
}

inline void subcon_append_history(SUBCON_TODO_ITEM& item, const SUBCON_TODO_RESULT& result)
{
    item.history.push_back(result);
    if (item.history.size() > SUBCON_HISTORY_MAX)
        item.history.erase(item.history.begin(), item.history.end() - static_cast<std::ptrdiff_t>(SUBCON_HISTORY_MAX));
}

// What was already told to the user about this task recently, newest
// last - given to the finding prompt so the same unchanged thing isn't
// reported again every cycle.
inline std::vector<std::string> subcon_recent_told(const SUBCON_TODO_ITEM& item, int64_t now_unix_seconds, size_t max_count)
{
    std::vector<std::string> told;
    for (auto it = item.history.rbegin(); it != item.history.rend() && told.size() < max_count; ++it)
    {
        if (!it->noteworthy || it->finding.empty()) continue;
        if (it->at_unix != 0 && now_unix_seconds - it->at_unix > 2 * SUBCON_WINDOW_SECONDS) break;
        told.push_back(it->finding);
    }
    std::reverse(told.begin(), told.end());
    return told;
}

// ---------------------------------------------------------------------
// Presence: "a cycle between leaving and arriving home"
// ---------------------------------------------------------------------

// Watches the presence value subcon already receives each tick and works
// out the last real arrival home. Not persisted: after a restart the
// window falls back to the plain 24 hours (run_times themselves are
// persisted, so no cap is ever lost, only the early reset).
struct SUBCON_PRESENCE_TRACKER
{
    std::string previous = "unknown";
    int64_t away_since_unix = 0;
    int64_t last_arrival_unix = 0;

    // Returns true on the tick a real arrival is recognised. A value that
    // is not "away" or "near" (e.g. "unknown") is ignored entirely - it
    // must not end an away spell or count as an arrival.
    bool update(const std::string& presence, int64_t now_unix_seconds)
    {
        if (presence != "away" && presence != "near") return false;
        if (presence == previous) return false;

        bool arrived = false;
        if (presence == "away")
        {
            away_since_unix = now_unix_seconds;
        }
        else if (previous == "away" && away_since_unix != 0 &&
                 now_unix_seconds - away_since_unix >= SUBCON_MIN_AWAY_SECONDS)
        {
            last_arrival_unix = now_unix_seconds;
            arrived = true;
        }

        previous = presence;
        return arrived;
    }
};

// ---------------------------------------------------------------------
// The digest queue and delivery
// ---------------------------------------------------------------------

// One thing worth telling the user, waiting for a good moment.
struct SUBCON_NOTE
{
    std::string content;       // the finding, in plain sentences
    std::string label;         // which task it came from (e.g. "Weather")
    std::string urgency = "normal"; // "low" | "normal" | "urgent"
    int64_t created_unix = 0;
    bool delivered = false;
    bool expired = false;      // dropped unheard (SUBCON_NOTE_MAX_AGE_SECONDS) rather than delivered
};

inline void to_json(json& j, const SUBCON_NOTE& note)
{
    j = json{{"content", note.content}, {"label", note.label}, {"urgency", note.urgency},
             {"created_unix", note.created_unix}, {"delivered", note.delivered}, {"expired", note.expired}};
}

inline void from_json(const json& j, SUBCON_NOTE& note)
{
    note.content = j.value("content", "");
    note.label = j.value("label", "");
    note.urgency = j.value("urgency", "normal");
    note.created_unix = j.value("created_unix", static_cast<int64_t>(0));
    note.delivered = j.value("delivered", false);
    note.expired = j.value("expired", false);
}

// Marks stale undelivered notes as expired (and so no longer pending).
// A note with no creation time (saved before it was tracked) counts as
// stale. Returns how many were expired.
inline int subcon_expire_old_notes(std::vector<SUBCON_NOTE>& notes, int64_t now_unix_seconds)
{
    int count = 0;
    for (SUBCON_NOTE& note : notes)
    {
        if (note.delivered) continue;
        if (note.created_unix == 0 || now_unix_seconds - note.created_unix > SUBCON_NOTE_MAX_AGE_SECONDS)
        {
            note.delivered = true;
            note.expired = true;
            ++count;
        }
    }
    return count;
}

inline size_t subcon_count_pending(const std::vector<SUBCON_NOTE>& notes)
{
    size_t n = 0;
    for (const SUBCON_NOTE& note : notes)
        if (!note.delivered) ++n;
    return n;
}

inline bool subcon_any_pending_urgent(const std::vector<SUBCON_NOTE>& notes)
{
    for (const SUBCON_NOTE& note : notes)
        if (!note.delivered && note.urgency == "urgent") return true;
    return false;
}

// Age of the oldest pending note, 0 if none are pending.
inline int64_t subcon_oldest_pending_age(const std::vector<SUBCON_NOTE>& notes, int64_t now_unix_seconds)
{
    int64_t oldest = 0;
    for (const SUBCON_NOTE& note : notes)
        if (!note.delivered) oldest = std::max(oldest, now_unix_seconds - note.created_unix);
    return oldest;
}

enum class SUBCON_DELIVERY_ACTION
{
    NOTHING,    // not now - conditions aren't met, or a previous "wait" is still running
    ASK_MODEL,  // conditions are fine; let the model judge whether this is a good moment
    DELIVER_NOW // mechanical rule says go, no need to ask
};

struct SUBCON_DELIVERY_INPUT
{
    bool user_near = false;
    int main_busy_level = 0;
    int64_t now_unix_seconds = 0;
    size_t pending_count = 0;
    bool any_urgent = false;
    int64_t oldest_age_seconds = 0;
    int64_t hold_until_unix = 0; // the model's own earlier "wait until" - 0 = none
};

// The mechanical part of delivery. Everything that must never be left to
// the model's judgment lives here: somebody has to be home, the system
// has to be idle, and the dead of night is off limits. Only once those
// hold does a note get a say-so: urgent ones and ones that have waited
// too long go straight out, anything else is a judgment call.
inline SUBCON_DELIVERY_ACTION subcon_delivery_gate(const SUBCON_DELIVERY_INPUT& in)
{
    if (in.pending_count == 0) return SUBCON_DELIVERY_ACTION::NOTHING;
    if (!in.user_near) return SUBCON_DELIVERY_ACTION::NOTHING;
    if (in.main_busy_level >= 10) return SUBCON_DELIVERY_ACTION::NOTHING;
    if (subcon_is_quiet_hour(subcon_local_hour(in.now_unix_seconds))) return SUBCON_DELIVERY_ACTION::NOTHING;

    if (in.any_urgent) return SUBCON_DELIVERY_ACTION::DELIVER_NOW;
    if (in.oldest_age_seconds >= SUBCON_NOTE_MAX_HOLD_SECONDS) return SUBCON_DELIVERY_ACTION::DELIVER_NOW;
    if (in.now_unix_seconds < in.hold_until_unix) return SUBCON_DELIVERY_ACTION::NOTHING;
    return SUBCON_DELIVERY_ACTION::ASK_MODEL;
}

// Everything pending, urgent first then oldest first, as one block of
// text for the real chat to speak - and marks each as delivered. Built
// here, not written by the model, so the facts reach the user exactly as
// recorded. The wording the chat narrates it in is its own persona's.
inline std::string subcon_build_digest(std::vector<SUBCON_NOTE>& notes)
{
    std::vector<SUBCON_NOTE*> pending;
    for (SUBCON_NOTE& note : notes)
        if (!note.delivered) pending.push_back(&note);

    std::stable_sort(pending.begin(), pending.end(), [](const SUBCON_NOTE* a, const SUBCON_NOTE* b)
    {
        bool a_urgent = (a->urgency == "urgent"), b_urgent = (b->urgency == "urgent");
        if (a_urgent != b_urgent) return a_urgent;
        return a->created_unix < b->created_unix;
    });

    std::string digest;
    if (pending.size() > 1)
        digest = "Olli's background thinking turned up " + std::to_string(pending.size()) + " things worth mentioning:\n";

    for (size_t i = 0; i < pending.size(); ++i)
    {
        std::string line = pending[i]->label.empty() ? pending[i]->content : pending[i]->label + ": " + pending[i]->content;
        if (pending.size() > 1) digest += std::to_string(i + 1) + ". ";
        digest += line;
        if (i + 1 < pending.size()) digest += "\n";
        pending[i]->delivered = true;
    }
    return digest;
}

#endif
