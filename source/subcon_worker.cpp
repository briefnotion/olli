#include "subcon_worker.h"

#include "olla.h" // ollama_system
#include "io_worker.h" // IO_WORKER_CLASS - see subcon_io_worker's own comment below
#include "subcon_tasks.h" // SUBCON_TODO_ITEM / SUBCON_NOTE and the cap/delivery rules around them

#include <chrono>
#include <thread>
#include <fstream>
#include <algorithm> // std::clamp

// Writes a file so a crash or kill mid-write can't leave it half-written:
// the full text goes to a sibling temp file first, then rename() swaps it
// into place (atomic on the same filesystem). Found in review 2026-10-28:
// truncating and rewriting in place meant an interrupted write left a file
// that then failed to parse, which load treats as a first run - silently
// resetting every task's run cap and losing the history. Returns false if
// anything went wrong (the caller's existing try/catch handles throws).
static bool write_file_atomically(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::path temp_path = path;
    temp_path += ".tmp";
    {
        std::ofstream file(temp_path, std::ios::trunc);
        if (!file) return false;
        file << text;
        file.flush();
        if (!file) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp_path, path, ec);
    return !ec;
}

// A file that exists but won't parse is moved aside (path + ".corrupt")
// instead of being treated as absent and overwritten by the next save - so
// whatever was in it can still be recovered by hand.
static void set_aside_corrupt_file(const std::filesystem::path& path)
{
    std::filesystem::path aside = path;
    aside += ".corrupt";
    std::error_code ec;
    std::filesystem::rename(path, aside, ec);
    DEBUG_LOG_CLASS::instance().log_event("subcon", "couldn't read " + path.filename().string() + " - moved it aside as " + aside.filename().string() + " and starting fresh");
}

// Loads the persisted queue from disk - missing file (first run) or any
// parse trouble is treated the same as "empty queue," not an error; this
// is a nice-to-have record, not something worth failing startup over.
static std::vector<SUBCON_NOTE> load_subcon_queue(const std::filesystem::path& path)
{
    std::vector<SUBCON_NOTE> queue;
    std::ifstream file(path);
    if (!file) return queue;

    try
    {
        json data;
        file >> data;
        queue = data.get<std::vector<SUBCON_NOTE>>();
    }
    catch (const std::exception&)
    {
        // Treated as empty, same as a missing file - but the unreadable
        // file itself is kept aside rather than overwritten by the next save.
        file.close();
        set_aside_corrupt_file(path);
    }

    return queue;
}

static void save_subcon_queue(const std::filesystem::path& path, const std::vector<SUBCON_NOTE>& queue)
{
    // Found live 2026-09-26: three real crashes (SIGABRT in thread_main(),
    // same signature as the 2026-09-24 fix - an uncaught exception forcing
    // thread_main() to unwind with subcon_llm.chat_thread still joinable,
    // which terminates immediately per std::thread's own destructor rule)
    // traced to this line. dump()'s default error_handler_t::strict throws
    // json::type_error on invalid UTF-8 - a real risk here specifically,
    // since note.content is built from live, free-form model output
    // (chosen_reasoning/last_received.response), not a fixed string.
    // error_handler_t::replace avoids throwing at all (swaps any bad bytes
    // for U+FFFD instead); the try/catch is defense-in-depth on top, not a
    // substitute for it - losing one saved note to a write hiccup is a far
    // smaller problem than crashing the whole program over it.
    try
    {
        if (!write_file_atomically(path, json(queue).dump(2, ' ', false, json::error_handler_t::replace)))
            DEBUG_LOG_CLASS::instance().log_event("subcon", "failed to write " + path.filename().string());
    }
    catch (const std::exception& e)
    {
        DEBUG_LOG_CLASS::instance().log_event("subcon", std::string("failed to save queue: ") + e.what());
    }
}

// Structured-output schema (send()'s own response_format parameter, olla.h -
// same mechanism sidetrack.cpp's DONE-check fix uses) for the merged
// prioritize/pace decision below (thread_main()'s own comment on why this
// replaced a plain fixed-timer gate, 2026-09-26) - one call now decides
// both "is anything worth doing right now" and, whichever way that goes,
// what to do about it: should_run_now + chosen_index/reasoning if so,
// wait_minutes if not. All four required (not making the unused pair of
// fields conditional/optional) - simpler for the model to always fill in
// every field than to reliably omit exactly the right two depending on a
// sibling field's value; the unused pair is just ignored in code depending
// on should_run_now.
static const json SUBCON_PRIORITIZE_FORMAT = {
    {"type", "object"},
    {"properties", {
        {"should_run_now", {{"type", "boolean"}}},
        {"chosen_index", {{"type", "integer"}}},
        {"reasoning", {{"type", "string"}}},
        {"wait_minutes", {{"type", "integer"}}}
    }},
    {"required", json::array({"should_run_now", "chosen_index", "reasoning", "wait_minutes"})}
};

// Hard floor/ceiling on the model's own wait_minutes proposal - the same
// lesson this project already learned once (the second-guess safety fix,
// IDEAS.md's own "when to announce" note): AI judgment about real-world
// timing layers on top of a mechanical bound, it doesn't replace one
// entirely. Without this, a proposed 0 could spin the loop tight, or a
// proposed multi-day wait could effectively turn subcon off silently.
constexpr int SUBCON_WAIT_MINUTES_MIN = 1;
constexpr int SUBCON_WAIT_MINUTES_MAX = 60;

// Same UTF-8-safety shape as save_subcon_queue() above (its own comment
// explains why error_handler_t::replace + a try/catch, not just one or the
// other) - reasoning/plan/finding are all live, free-form model output
// same as a queue note's content is.
static void save_subcon_todo_items(const std::filesystem::path& path, const std::vector<SUBCON_TODO_ITEM>& items)
{
    try
    {
        if (!write_file_atomically(path, json(items).dump(2, ' ', false, json::error_handler_t::replace)))
            DEBUG_LOG_CLASS::instance().log_event("subcon", "failed to write " + path.filename().string());
    }
    catch (const std::exception& e)
    {
        DEBUG_LOG_CLASS::instance().log_event("subcon", std::string("failed to save todo items: ") + e.what());
    }
}

// Tasks dropped by reconcile_subcon_tasks() (subcon_tasks.h) are appended
// here rather than discarded - they carry real history (what was found,
// and when), and nothing about retiring a task says that history is
// worthless. Never read back by subcon itself, only there for a person.
static void archive_retired_subcon_tasks(const std::filesystem::path& path, const std::vector<SUBCON_TODO_ITEM>& retired)
{
    if (retired.empty()) return;

    json archive = json::array();
    {
        std::ifstream existing(path);
        if (existing)
        {
            bool readable = true;
            try { existing >> archive; }
            catch (const std::exception&) { readable = false; }
            if (!readable || !archive.is_array())
            {
                existing.close();
                set_aside_corrupt_file(path);
                archive = json::array();
            }
        }
    }
    for (const SUBCON_TODO_ITEM& item : retired) archive.push_back(item);

    try
    {
        if (!write_file_atomically(path, archive.dump(2, ' ', false, json::error_handler_t::replace)))
            DEBUG_LOG_CLASS::instance().log_event("subcon", "failed to write " + path.filename().string());
    }
    catch (const std::exception& e)
    {
        DEBUG_LOG_CLASS::instance().log_event("subcon", std::string("failed to archive retired tasks: ") + e.what());
    }
}

// Loads the persisted task list and brings it in line with the task
// definitions in code (subcon_task_defs(), subcon_tasks.h) - a missing
// file (first run for this profile) or any parse trouble starts from the
// definitions alone, rather than an empty list, since an empty list would
// leave the reasoning loop with nothing to prioritize over at all -
// unlike the queue above, where empty is a perfectly normal starting
// state. Anything saved that no longer matches a definition (including
// everything saved before tasks had keys) goes to retired_path, not away.
static std::vector<SUBCON_TODO_ITEM> load_subcon_todo_items(const std::filesystem::path& path, const std::filesystem::path& retired_path)
{
    std::vector<SUBCON_TODO_ITEM> loaded;
    {
        std::ifstream file(path);
        if (file)
        {
            try
            {
                json data;
                file >> data;
                loaded = data.get<std::vector<SUBCON_TODO_ITEM>>();
            }
            catch (const std::exception&)
            {
                // Treated as a first run - but kept aside, not overwritten:
                // it holds every task's run times and history.
                loaded.clear();
                file.close();
                set_aside_corrupt_file(path);
            }
        }
    }

    std::vector<SUBCON_TODO_ITEM> retired;
    std::vector<SUBCON_TODO_ITEM> items = reconcile_subcon_tasks(loaded, subcon_task_defs(), retired);
    if (!retired.empty())
    {
        archive_retired_subcon_tasks(retired_path, retired);
        DEBUG_LOG_CLASS::instance().log_event("subcon", "retired " + std::to_string(retired.size()) + " old task(s) to " + retired_path.filename().string());
    }
    return items;
}

// Which tasks the prioritize stage may even be offered right now (cap and
// minimum gap both allow it - subcon_check_eligibility(), subcon_tasks.h),
// as indices into items. eligibility is filled in parallel to items (one
// entry per item, eligible or not) for the caller to read run counts and
// the next-eligible time from.
static std::vector<size_t> eligible_task_indices(const std::vector<SUBCON_TODO_ITEM>& items, int64_t now_unix_seconds,
                                                 int64_t last_arrival_unix_seconds, std::vector<SUBCON_ELIGIBILITY>& eligibility)
{
    std::vector<size_t> indices;
    eligibility.clear();
    for (size_t i = 0; i < items.size(); ++i)
    {
        eligibility.push_back(subcon_check_eligibility(items[i], now_unix_seconds, last_arrival_unix_seconds));
        if (eligibility.back().eligible) indices.push_back(i);
    }
    return indices;
}

static std::string describe_presence(const std::string& presence)
{
    if (presence == "near") return "The user is at home.";
    if (presence == "away") return "The user is away from home.";
    return "It isn't known whether the user is home.";
}

// Turns the candidate list into the actual prompt text for the merged
// prioritize/pace decision below - free function, not inlined, purely to
// keep thread_main() itself readable. Only lists tasks that are allowed to
// run right now (eligible_indices) - a capped task is not offered, rather
// than offered with a note asking the model to please not choose it. The
// numbers shown are each task's own index in items, so chosen_index maps
// straight back without a translation table. Includes each item's own
// staleness and how much of its allowance is used, not just its
// description, so the model has real recency to weigh alongside
// importance (the user's own original "the 3rd looks important... because
// I hadn't done it in a while" example) - and also asks whether now is
// even a good moment to act at all, rather than assuming something must
// always be worth doing the moment it's asked (the user's own idea,
// 2026-09-26/27: "if it seems not enough time has passed, figure out a
// good sleep time, for when i should check again"). The time of day and
// whether the user is home are included because several tasks depend on
// them (weather in the morning, lights while away).
static std::string build_prioritize_prompt(const std::vector<SUBCON_TODO_ITEM>& items, const std::vector<size_t>& eligible_indices,
                                           const std::vector<SUBCON_ELIGIBILITY>& eligibility, int64_t now_unix_seconds,
                                           const std::string& presence)
{
    std::string prompt = "It is " + subcon_describe_local_time(now_unix_seconds) + ". " + describe_presence(presence) + "\n";
    prompt += "Here are some things you could look into right now:\n";
    for (size_t i : eligible_indices)
    {
        prompt += std::to_string(i) + ". " + items[i].description + " (" + subcon_describe_staleness(items[i], now_unix_seconds);
        if (items[i].max_runs_per_window > 0)
            prompt += "; done " + std::to_string(eligibility[i].runs_in_window) + " of " + std::to_string(items[i].max_runs_per_window) + " allowed in this period";
        prompt += ")\n";
    }

    prompt += "First, decide whether now is actually a good moment to look into any of "
              "these, or whether nothing here is urgent enough yet and it'd make more "
              "sense to check back later. Weigh both how important something is and how "
              "overdue it is - the longer it's been since something was checked (or if "
              "it's never been checked at all), the more it's worth prioritizing; "
              "something checked very recently is less urgent precisely because it was "
              "just confirmed. If nothing is worth acting on right now, set should_run_now "
              "to false and wait_minutes to how long you'd actually wait before checking "
              "again - a real judgment call, not a fixed number. If something is worth "
              "doing now, set should_run_now to true, chosen_index to the number of your "
              "choice, and wait_minutes to 0. Always set reasoning to a brief explanation "
              "of your decision either way.";
    return prompt;
}

// The turn that has to carry the real tool call - phrased as an
// instruction to act, not a request for a plan: found 2026-10-28 that
// "what are the concrete steps to actually do it?" got a text description
// of steps and zero tool calls in 0/6 direct trials, where "Do this now
// using your tools" got a real call in 6/6. how_to is the task's own
// concrete hint (which tool, what to look for). Deliberately does NOT
// mention what the task found last time: tried that first ("for
// comparison, last time...") and the model answered "similar to the last
// check" from that text without calling anything. The comparison is
// made in the finding stage instead, once there are fresh tool results to
// compare against (build_finding_prompt()).
static std::string build_act_prompt(const SUBCON_TODO_ITEM& item)
{
    std::string prompt = "Do this now using your tools: \"" + item.description + "\". ";
    if (!item.how_to.empty()) prompt += item.how_to + " ";
    prompt += "Call the tool first - don't answer from memory. Answer in plain sentences, not JSON. Keep it brief.";
    return prompt;
}

// Structured-output schema for the finding stage - replaces the earlier
// free-text "what's the actual result?" turn, which could only ever
// produce text and so queued (and eventually announced) a note after every
// single cycle, including "no unresolved issues detected" (found
// 2026-10-28 in ron's queue: 141 notes, all of that kind). noteworthy is
// the gate: only a true one ever reaches the queue.
static const json SUBCON_FINDING_FORMAT = {
    {"type", "object"},
    {"properties", {
        {"noteworthy", {{"type", "boolean"}}},
        {"urgency", {{"type", "string"}, {"enum", json::array({"low", "normal", "urgent"})}}},
        {"summary", {{"type", "string"}}}
    }},
    {"required", json::array({"noteworthy", "urgency", "summary"})}
};

static std::string build_finding_prompt(const SUBCON_TODO_ITEM& item, int64_t now_unix_seconds, const std::vector<std::string>& already_told)
{
    std::string prompt =
        "Now report what you found. Base it only on what your tool results above actually "
        "showed - do not guess, and do not fill in anything they didn't say. Set noteworthy "
        "to true only if the user would genuinely want to be told: something new, changed, "
        "time-sensitive, or needing attention. Routine, unchanged, all-clear, or empty "
        "results are not noteworthy. If your tools failed or returned nothing useful, set "
        "noteworthy to false and say so in summary. Set urgency to \"urgent\" only for "
        "something that can't wait (such as a severe weather alert), otherwise \"normal\" "
        "or \"low\". Write summary as one to three plain sentences stating the facts, ready "
        "to be read aloud - no JSON, no lists, no markdown.";

    // What this task found last time, if recently - for judging "new or
    // changed" against the fresh results above, not for the model to repeat.
    if (!item.history.empty())
    {
        const SUBCON_TODO_RESULT& last = item.history.back();
        if (!last.finding.empty() && (last.at_unix == 0 || now_unix_seconds - last.at_unix < 2 * SUBCON_WINDOW_SECONDS))
            prompt += " For comparison, the previous check of this found: \"" + last.finding + "\" Judge changes by your results above, not by this.";
    }

    if (!already_told.empty())
    {
        prompt += " The user has already been told:";
        for (const std::string& told : already_told) prompt += " \"" + told + "\"";
        prompt += " Do not report the same thing again unless it has materially changed.";
    }
    return prompt;
}

// Structured-output schema for the delivery decision - the "when is a good
// moment to tell the user" judgment (the user's own 2026-10-28 idea: a
// subcon task of its own). Bounded by subcon_delivery_gate() before it is
// ever asked (presence, idle, quiet hours, urgent / overdue notes) and
// clamped after (SUBCON_DELIVERY_WAIT_MINUTES_*), same
// judgment-on-top-of-a-mechanical-bound shape as the prioritize wait.
static const json SUBCON_DELIVERY_FORMAT = {
    {"type", "object"},
    {"properties", {
        {"deliver_now", {{"type", "boolean"}}},
        {"wait_minutes", {{"type", "integer"}}},
        {"reasoning", {{"type", "string"}}}
    }},
    {"required", json::array({"deliver_now", "wait_minutes", "reasoning"})}
};

static std::string build_delivery_prompt(const std::vector<SUBCON_NOTE>& notes, int64_t now_unix_seconds,
                                         int64_t last_arrival_unix_seconds, int64_t last_delivery_unix_seconds)
{
    std::string prompt = "It is " + subcon_describe_local_time(now_unix_seconds) + ". The user is at home and nothing else is going on.";
    if (last_arrival_unix_seconds != 0 && now_unix_seconds - last_arrival_unix_seconds < SUBCON_WINDOW_SECONDS)
        prompt += " They got home " + std::to_string((now_unix_seconds - last_arrival_unix_seconds) / 60) + " minute(s) ago.";
    if (last_delivery_unix_seconds != 0)
        prompt += " You last told them something " + std::to_string((now_unix_seconds - last_delivery_unix_seconds) / 60) + " minute(s) ago.";

    prompt += "\nThese are waiting to be told to the user:\n";
    for (const SUBCON_NOTE& note : notes)
    {
        if (note.delivered) continue;
        prompt += "- (" + note.urgency + ", found " + std::to_string((now_unix_seconds - note.created_unix) / 60) + " minute(s) ago) " +
                  (note.label.empty() ? "" : note.label + ": ") + note.content + "\n";
    }

    prompt += "Decide whether this is a good moment to tell them, or whether to wait a while. "
              "A good moment is a calm one - shortly after they get home, or a quiet point in "
              "the day - and anything time-sensitive should not wait. If it can wait and you "
              "just told them something, holding off a bit is fine. If this is a good moment, "
              "set deliver_now to true and wait_minutes to 0. Otherwise set deliver_now to "
              "false and wait_minutes to how long you'd hold off before reconsidering. Always "
              "set reasoning to a brief explanation.";
    return prompt;
}

// Same direct chat_thread-spawn shape as sidetrack.cpp's own
// start_second_guess_call() - needed specifically because ollama_system::
// input() (the ENTER_PRESSED-driven submission path used for the PLANNING
// stage below, which just wants free text) has no way to pass a
// response_format through to send() at all. Used for every structured
// stage (PRIORITIZING, RESULTING, DELIVERING). comms captured by reference, same as
// start_second_guess_call()'s own - safe for the same reason that one is:
// nothing outside this instance's own chat_thread and thread_main()'s own
// polling ever touches subcon_comms concurrently.
static void start_subcon_call(ollama_system& instance, COMMS& comms, const std::string& prompt, const json& response_format)
{
    instance.status.interrupt_signal = false;
    instance.is_processing = true;
    if (instance.chat_thread.joinable()) instance.chat_thread.join();
    instance.chat_thread = std::thread([&instance, &comms, prompt, response_format]()
    {
        {
            std::lock_guard<std::mutex> lock(output_buffer_mutex);
            comms.INPUT_FROM_USER.set(prompt);
        }
        // An uncaught exception on a std::thread is std::terminate for the
        // whole program - log it and let the stage resolve as a failed call
        // (empty response) instead.
        try { instance.send(nullptr, comms, "user", response_format); }
        catch (const std::exception& e)
        {
            DEBUG_LOG_CLASS::instance().log_event("subcon", std::string("model call threw: ") + e.what());
        }
        instance.is_processing = false;
    });
}

// The "talking to itself" reasoning loop below (thread_main()'s own while
// loop) - a real, if tiny, multi-stage conversation with itself, entirely
// self-contained to this thread, no wiring anywhere else yet. IDLE waits
// for the same timer/busy gate the old one-shot test prompt used;
// PRIORITIZING/PLANNING/RESULTING each cover one async subcon_llm call
// (submitted, then polled tick by tick, same input()/process() pattern as
// before) - always resolved one way or another via !is_processing, not
// gated on last_received.complete/response.empty() also being true, so a
// genuine network failure can't leave this stuck forever waiting for a
// condition that will never come.
//
// RESULTING added 2026-09-26 - PLANNING alone only ever produced a
// procedure ("here's how you'd check this"), never an actual outcome
// ("here's what you found") - not something sayable to a user, and the
// whole point of the digest queue is eventually surfacing something
// sayable. Kept as its own stage rather than folding into PLANNING, per
// the user's own call: keeps "how would you do it" and "what did you find"
// as two distinct, separately-visible things in history, not one blended
// answer.
//
// ANNOUNCING added 2026-09-27 - the user's own "special" case from a
// pseudocode sketch of this whole state machine (something happened, need
// to communicate a big thought to the upper system, then clean up). Unlike
// the other three, it involves no subcon_llm call at all - the text to say
// was already decided, this stage just stages it into pending_announcement
// for exchange() to carry out and immediately resolves, same tick. Since
// 2026-10-28 that text is a digest of every pending note at once
// (subcon_build_digest()), not one note per firing.
//
// 2026-10-28 changes to the cycle (the user's "make real subcon tasks"
// project): PLANNING is now the act turn, the one that has to make a real
// tool call, and its completion also waits for any remote tool result to
// come back; RESULTING is a structured-output finding (noteworthy /
// urgency / summary) so a quiet "all clear" never reaches the queue; and
// DELIVERING is a new structured call where the model judges whether this
// is a good moment to tell the user what's queued. Every cycle (and every
// delivery decision) starts from a fresh conversation, so the small context
// window isn't spent carrying earlier cycles' tool results around.
enum class SUBCON_STAGE { IDLE, PRIORITIZING, PLANNING, RESULTING, DELIVERING, ANNOUNCING };

// The only tools subcon is expected to call. It technically has the whole
// tool set (the user's own 2026-09-27 call) and is kept passive only by
// its persona prompt - this list is the audit that makes a slip visible:
// any call outside it is logged loudly and its finding is thrown away
// rather than queued (the action itself can't be undone from here). A
// real structural block would have to live in the tool dispatch, which is
// outside subcon's own scope.
static bool is_read_only_tool(const std::string& name)
{
    static const char* const allowed[] = {
        "web_search", "fetch_website_content", "rag_list_collections", "rag_search", "rag_search_documents",
        "rag_get_document", "list_hue_lights", "check_presence", "list_timers", "check_timer", "get_clock_time"
    };
    for (const char* a : allowed)
        if (name == a) return true;
    return false;
}

// How many real tool results subcon_llm's conversation holds, and the name
// of every tool the model asked for. The conversation is cleared at the
// start of each cycle, so this is exactly "this cycle." Counted from
// history rather than tracked as calls are dispatched so a built-in tool
// (answered synchronously) and a remote one (answered later) are treated
// the same.
static int count_subcon_tool_results(ollama_system& instance, std::vector<std::string>& tool_names)
{
    std::lock_guard<std::mutex> lock(history_mutex);
    int count = 0;
    for (const Message& message : instance.history)
    {
        if (message.role == "tool") ++count;
        for (const ToolCall& tc : message.tool_calls) tool_names.push_back(tc.name);
    }
    return count;
}

void SUBCON_WORKER_CLASS::thread_start()
{
    THREAD_CONTROL.create(1000);
    THREAD_CONTROL.start_render_thread([this]() { thread_main(); });
}

void SUBCON_WORKER_CLASS::thread_stop()
{
    RUN = false;
    THREAD_CONTROL.wait_for_thread_to_finish();
}

std::string SUBCON_WORKER_CLASS::exchange(COMMS& comms)
{
    // try_lock, not a blocking lock_guard - see this class's own header
    // comment for why: a missed copy here is harmless (main_busy_level
    // just keeps last tick's value, refreshed again next tick), so the
    // main thread should never wait on subcon's own background thread for
    // this. If thread_main() happens to hold comms_mutex right now, just
    // skip this tick's copy entirely rather than stalling - an
    // undrained pending_announcement just waits one more tick too.
    std::unique_lock<std::mutex> lock(comms_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return "";

    // Just the busy level and presence for now, not the whole comms -
    // comms_buffer stays unused/stale until subcon actually needs more than
    // this (subcon_worker.h's own comment on both members).
    main_busy_level = comms.busy_count();
    user_presence = comms.user.presence;

    // Drain, not peek - once handed out, this copy's job is done. Empty
    // string (the common case) means nothing to announce this tick.
    std::string announcement = pending_announcement;
    pending_announcement.clear();
    return announcement;
}

void SUBCON_WORKER_CLASS::thread_main()
{
    // Local, not a class member - same reasoning TOOL_WORKER_CLASS::
    // thread_main() already has for its own tools_list/remote_tools
    // (tool_worker.cpp): only this thread ever touches it, and nothing
    // outside thread_main() needs to reach in directly. This is the
    // subconscious's own LLM.
    ollama_system subcon_llm;

    // Short human-readable tag for debug_full_history.txt (debug_label's
    // own comment, olla.h) - a member of ollama_system itself, not PROPS,
    // so it's set here directly rather than carried over by the PROPS copy
    // below.
    subcon_llm.debug_label = "subcon";

    // One-way copy from PROPS (set once by main.cpp, before thread_start()
    // - see that class member's own comment, subcon_worker.h) into this
    // instance's own PROPS - same model/host/port as the real chat. Just a
    // stepping stone, not canon: real overrides on top of it below.
    subcon_llm.PROPS = PROPS;

    // Never persist this instance's own history to disk - a wholesale
    // PROPS copy above would otherwise carry over the real
    // LOAD_SAVE_HISTORY_ON_DISK=true and OLLI_DIRECTORY, silently letting
    // this instance overwrite the real history.json with its own - the
    // exact trap SIDETRACK_CLASS::run_consolidation() already had to avoid
    // (sidetrack.cpp).
    subcon_llm.PROPS.LOAD_SAVE_HISTORY_ON_DISK = false;

    // Its own subdirectory, separate from the profile's own top-level one -
    // keeps whatever it eventually needs of its own cleanly separated
    // rather than cluttering the main directory. Not yet acted on - nothing
    // calls subcon_llm.open() yet, so nothing's created on disk until that
    // lands.
    subcon_llm.PROPS.OLLI_DIRECTORY = PROPS.OLLI_DIRECTORY / "subcon";

    // Chat itself runs with use_thinking = false (main.cpp) - the
    // subconscious wants its own reasoning regardless of that, so this is
    // forced true rather than inherited from the PROPS copy above.
    subcon_llm.PROPS.use_thinking = true;

    // 2026-09-27, alongside full tool access (subcon_tools_list below) -
    // see olla.h's own comment on drains_events for the full reasoning.
    // Sharing tool_worker (this->tool_worker, set once by main.cpp before
    // thread_start()) is safe for dispatching/receiving subcon's own calls
    // (id-based, already proven), but ambient events (a timer firing,
    // presence changing) have no such correlation - without this, subcon
    // would compete with the real chat to drain them, risking silently
    // absorbing one that was meant to reach the user.
    subcon_llm.drains_events = false;

    // No display to stream into - subcon_comms below is blank/never shown
    // anywhere, so there's nothing for incremental chunks to write to.
    // send() (olla.cpp) only takes the streaming code path at all if either
    // of these is true; with both off it uses the plain non-streaming
    // request instead and still populates last_received.response/.thinking
    // the same way at the end - no difference in the actual result, just
    // skips work with nowhere to go.
    subcon_llm.PROPS.stream_output = false;
    subcon_llm.PROPS.stream_thinking = false;

    // Full tool access as of 2026-09-27 - was TOOL_WEB_SEARCH alone, before
    // that empty/deliberately passive (IDEAS.md's own "subconscious"
    // section, "deliberately passive toolset" principle). The user's own
    // explicit, informed choice over a curated read-only subset - see
    // TODO.md's own entry for the full reasoning: this relies on the
    // prompt (OLLAMA_OPENING below) instructing passivity rather than
    // structurally preventing action, a real, deliberate departure from
    // the original passive-toolset principle, accepted because subcon's
    // own prompts are always developer-authored and narrow (never reacting
    // to open-ended real user conversation the way the second-guess
    // routine that principle was originally reacting to does).
    // populate_default_tools() (olla.cpp) - the same 4 built-ins chat/
    // sidetrack get, not a hand-picked subset anymore. Local, not a class
    // member, same reasoning as subcon_llm above.
    std::vector<std::unique_ptr<TOOL_BASE>> subcon_tools_list;
    populate_default_tools(subcon_tools_list);

    // Own persona, not the default (which is written for a general
    // assistant with tool guidance and was leaking through - visible in an
    // earlier live test as an in-character reply that had nothing to do
    // with what subcon actually is). Updated again for full tool access
    // (2026-09-27) - explicitly instructed to stay observational despite
    // technically having the same real-world-action tools the main chat
    // does, since nothing structural prevents it anymore. Set before
    // open() - that's what seeds the protected opening message with it
    // (same order TOOL_DELEGATOR's own instance uses, tools.cpp).
    subcon_llm.OLLAMA_OPENING =
        "You are Olli's subconscious: a background reasoning process, "
        "separate from the main conversation the user is having with Olli. "
        "You have the same tools Olli's main conversation does, including "
        "ones that can take real-world action (controlling devices, "
        "running automations). You must never actually use those - only "
        "look things up (web search, checking status, reading data). Never "
        "control a device, send anything, or change anything in the "
        "outside world, even if it seems helpful or is close to something "
        "you were asked to look into. When given a list of things to "
        "consider, pick the one that matters most right now and explain "
        "why; when asked to plan, describe the concrete steps briefly and "
        "plainly.";

    // One-arg overload, not the (tools_list, Properties) one - that one
    // does PROPS = Properties first thing (olla.cpp), which would stomp the
    // overrides just set above.
    subcon_llm.open(subcon_tools_list);

    // The digest queue's own file - subcon_llm.PROPS.OLLI_DIRECTORY already
    // is the "subcon" subdirectory (set above), and open() just created it
    // on disk if it didn't already exist, so this path is guaranteed valid
    // right here. Loaded once, saved again every time a new note is added
    // below - deliberately not read back from disk on every tick, since
    // nothing outside this thread ever touches this file.
    std::filesystem::path subcon_queue_path = subcon_llm.PROPS.OLLI_DIRECTORY / "queue.json";
    std::vector<SUBCON_NOTE> tell_later_queue = load_subcon_queue(subcon_queue_path);

    // Every real ollama_system call (send(), process(), integrate_tool_
    // result()) takes a COMMS&. Local, not a class member, same self-
    // contained reasoning as subcon_llm/subcon_tools_list above - and
    // blank on purpose, same as SIDETRACK_CLASS::run_consolidation()'s own
    // blank_comms (sidetrack.cpp): nothing here speaks/displays or calls a
    // tool yet, so there's nothing for it to carry.
    COMMS subcon_comms;

    // process()/handle_instance_tools() take IO_WORKER_CLASS& (a reference,
    // not a pointer - unlike system/tool_worker below, there's no nullptr
    // option), but only ever actually touch it inside dispatch_tool_call(),
    // which only runs when a tool call exists to dispatch - structurally
    // impossible here since subcon_tools_list is empty. So this is a real
    // object only to satisfy the signature, never started (thread_start()
    // never called on it) and never the real main.cpp io_worker - fully
    // self-contained to this thread, same as everything else here.
    IO_WORKER_CLASS subcon_io_worker;

    // Placeholder delay between think-cycles, not a considered design
    // choice yet - same as the old sleep_for placeholder this loop used to
    // just sit in.
    TIMED_IS_READY_SIMPLE think_cycle_timer;
    think_cycle_timer.set(60000); // ms

    // Own timer, deliberately separate from think_cycle_timer above - this
    // gates "is now an okay moment to consider surfacing something already
    // queued," a different question from "is it time to reason about
    // something new," even though they happen to share the same
    // presence/busy inputs right now. A shorter interval than the think
    // cycle's own 60s - checking readiness is cheap (no LLM call), unlike
    // starting a new reasoning cycle.
    TIMED_IS_READY_SIMPLE delivery_check_timer;
    delivery_check_timer.set(30000); // ms

    // The tasks themselves are defined in code, subcon_task_defs()
    // (subcon_tasks.h), and merged into whatever was persisted last run -
    // see load_subcon_todo_items(). State (last checked, run times,
    // history) survives for tasks that still exist; a task removed from
    // the definitions is archived to todo_items_retired.json instead of
    // lingering in the live list (which is also what retired the earlier
    // placeholder topics on 2026-10-28). Persisted straight away so a
    // retired task isn't archived a second time on the next start.
    std::filesystem::path subcon_todo_items_path = subcon_llm.PROPS.OLLI_DIRECTORY / "todo_items.json";
    std::filesystem::path subcon_retired_items_path = subcon_llm.PROPS.OLLI_DIRECTORY / "todo_items_retired.json";
    std::vector<SUBCON_TODO_ITEM> task_items = load_subcon_todo_items(subcon_todo_items_path, subcon_retired_items_path);
    save_subcon_todo_items(subcon_todo_items_path, task_items);

    // On (the user's call, 2026-10-28): subcon now has real tasks, each with
    // a hard run cap and spacing (subcon_task_defs(), subcon_tasks.h), a
    // finding gate that keeps "nothing to report" out of the queue, and
    // delivery rules (presence, idle, quiet hours) - so leaving it running
    // costs a handful of model calls a day, not one every minute. Before
    // that it was off by default (2026-09-26) because the loop only had
    // made-up topics to work with and burned GPU on fabricated findings.
    // Flip false to turn the whole reasoning cycle off; everything else in
    // this thread (exchange()'s busy/presence copy, the delivery-readiness
    // check for notes already queued) keeps running either way - only the
    // "start a new think-cycle" gate below is affected.
    constexpr bool SUBCON_THINK_CYCLE_ENABLED = true;

    // TEMP-SMOKETEST (2026-09-27): forces every prioritize decision into a
    // real PLAN/RESULT cycle regardless of what should_run_now actually
    // says, purely to get more real tool-call opportunities to observe
    // faster - the self-paced wait logic itself is working exactly as
    // intended (real, sensible waits, not the concern here) and is
    // deliberately left completely untouched otherwise. The model's own
    // original decision is still parsed and logged before being
    // overridden, so the real pacing behavior stays visible underneath the
    // override. Revert to false when done testing.
    constexpr bool SUBCON_FORCE_RUN_FOR_TESTING = false;

    SUBCON_STAGE stage = SUBCON_STAGE::IDLE;
    int chosen_index = -1;
    std::string chosen_reasoning; // carries PRIORITIZING's own reasoning through to RESULTING's completion, for the history entry built there
    std::string chosen_plan;      // the act turn's own reply (may be empty if it only made tool calls), same reasoning
    bool cycle_unsafe = false;    // set when the act turn called a tool outside is_read_only_tool() - its finding is then discarded
    SUBCON_PRESENCE_TRACKER presence_tracker; // works out the last real arrival home from user_presence - see subcon_tasks.h
    int64_t last_delivery_unix = 0;       // when something was last handed to the real chat
    int64_t delivery_hold_until_unix = 0; // the model's own "ask me again after this" for the delivery decision

    RUN = true;
    while (RUN)
    {
        // Plain blocking lock - see this class's own header comment for
        // why comms_mutex (not IO_WORKER_CLASS's own INTERUPTED/PROCESSING
        // scheme) is used here: a real mutex needs no separate "please
        // wait" flag, and it's fine for this thread to block briefly on
        // its own mutex since nothing else is waiting on it (exchange(),
        // main.cpp, uses try_lock precisely so it never has to).
        {
            std::lock_guard<std::mutex> lock(comms_mutex);

            int64_t now_unix_seconds = static_cast<int64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));

            // The mechanical delivery rules against the situation right now.
            // A lambda (not inlined) because it is asked twice: once to
            // decide whether to even start a delivery decision, and again
            // just before anything is actually announced - the model call
            // in between can take a minute or more, long enough for the
            // user to leave, start talking to Olli, or for it to turn 22:00.
            auto current_delivery_action = [&]()
            {
                SUBCON_DELIVERY_INPUT gate_input;
                gate_input.user_near = (user_presence == "near");
                gate_input.main_busy_level = main_busy_level;
                gate_input.now_unix_seconds = now_unix_seconds;
                gate_input.pending_count = subcon_count_pending(tell_later_queue);
                gate_input.any_urgent = subcon_any_pending_urgent(tell_later_queue);
                gate_input.oldest_age_seconds = subcon_oldest_pending_age(tell_later_queue, now_unix_seconds);
                gate_input.hold_until_unix = delivery_hold_until_unix;
                return subcon_delivery_gate(gate_input);
            };

            // "A cycle between leaving and arriving home" (the user's own
            // definition of a day for task allowances, along with a plain
            // 24 hours) - presence_tracker decides what counts as a real
            // arrival (away for a while first, SUBCON_MIN_AWAY_SECONDS). On
            // one, subcon_window_start() restarts every task's allowance,
            // and the think timer is pulled in so what just became
            // allowed is looked at soon rather than at the end of whatever
            // wait was in progress.
            if (presence_tracker.update(user_presence, now_unix_seconds))
            {
                DEBUG_LOG_CLASS::instance().log_event("subcon", "user arrived home - task allowances start a new period");
                think_cycle_timer.set(30000); // ms
            }

            // Start a new think-cycle - same gate the old one-shot test
            // prompt used (timer ready AND the real system currently idle),
            // plus the enable switch declared above. No re-arm on dispatch
            // (2026-09-26/27) - think_cycle_timer's own next interval is a
            // real decision the model makes as part of this call's own
            // result (see the completion branch below), not a fixed
            // constant set blindly at dispatch time. What is new
            // (2026-10-28): only tasks that are currently allowed to run
            // (cap and spacing, subcon_check_eligibility()) are offered,
            // and if none are, the model isn't asked at all - the timer is
            // simply set for when the soonest one opens up.
            if (SUBCON_THINK_CYCLE_ENABLED && stage == SUBCON_STAGE::IDLE && think_cycle_timer.is_ready() && main_busy_level < 10)
            {
                std::vector<SUBCON_ELIGIBILITY> eligibility;
                std::vector<size_t> eligible = eligible_task_indices(task_items, now_unix_seconds, presence_tracker.last_arrival_unix, eligibility);

                if (eligible.empty())
                {
                    int64_t soonest = eligibility.front().next_eligible_unix;
                    for (const SUBCON_ELIGIBILITY& e : eligibility) soonest = std::min(soonest, e.next_eligible_unix);

                    // Capped at the usual maximum wait, not the full time
                    // until the soonest one opens: coming home reopens
                    // everything sooner than the clock would, and a
                    // re-check with nothing eligible costs no model call.
                    int minutes = std::clamp(static_cast<int>((soonest - now_unix_seconds + 59) / 60), SUBCON_WAIT_MINUTES_MIN, SUBCON_WAIT_MINUTES_MAX);
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "every task is at its limit right now - checking again in " + std::to_string(minutes) + " minute(s)");
                    think_cycle_timer.set(minutes * 60000);
                }
                else
                {
                    // Fresh conversation for every cycle - keeps only the
                    // protected persona message. With real tool results
                    // now landing in the conversation, carrying earlier
                    // cycles along would spend the small context window
                    // (num_ctx 8192) on things that no longer matter.
                    subcon_llm.clear_history_keep_protected();
                    start_subcon_call(subcon_llm, subcon_comms,
                                      build_prioritize_prompt(task_items, eligible, eligibility, now_unix_seconds, user_presence),
                                      SUBCON_PRIORITIZE_FORMAT);
                    stage = SUBCON_STAGE::PRIORITIZING;
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "think-cycle started: prioritizing");
                }
            }

            // Delivery-readiness check - independent of the reasoning-cycle
            // stages (a previously-queued note can become ready to surface
            // while a new reasoning cycle is or isn't running), but only
            // acts when stage == IDLE and nothing's already staged -
            // doesn't interrupt an in-flight call (the user's own
            // pseudocode's "special" case does say "interrupt whatever was
            // going on" for something urgent, but presence/idle timing
            // isn't that - a genuine interrupt-mid-call is real future
            // scope, not this). "near" matches presence.cpp's own
            // vocabulary (profile.is_near) - see comms.h/olla.cpp's own
            // PART 5 for where this value actually comes from.
            //
            // 2026-10-28, the user's own idea: when to say it is itself
            // something subcon thinks about. subcon_delivery_gate()
            // (subcon_tasks.h) holds the mechanical rules - somebody must
            // be home, the system idle, and not the dead of night, with
            // urgent or long-waiting notes going straight out - and only
            // when none of those decide it is the model asked
            // (DELIVERING). Everything pending goes out together as one
            // digest, not one note per firing.
            if (delivery_check_timer.is_ready())
            {
                delivery_check_timer.set(30000); // ms - re-arm for the next check

                if (stage == SUBCON_STAGE::IDLE && pending_announcement.empty())
                {
                    int expired = subcon_expire_old_notes(tell_later_queue, now_unix_seconds);
                    if (expired > 0)
                    {
                        DEBUG_LOG_CLASS::instance().log_event("subcon", "dropped " + std::to_string(expired) + " note(s) that went unheard too long");
                        save_subcon_queue(subcon_queue_path, tell_later_queue);
                    }

                    SUBCON_DELIVERY_ACTION action = current_delivery_action();
                    if (action == SUBCON_DELIVERY_ACTION::DELIVER_NOW)
                    {
                        DEBUG_LOG_CLASS::instance().log_event("subcon", "delivering without asking - an urgent note, or one that has waited too long");
                        stage = SUBCON_STAGE::ANNOUNCING;
                    }
                    else if (action == SUBCON_DELIVERY_ACTION::ASK_MODEL)
                    {
                        subcon_llm.clear_history_keep_protected();
                        start_subcon_call(subcon_llm, subcon_comms,
                                          build_delivery_prompt(tell_later_queue, now_unix_seconds, presence_tracker.last_arrival_unix, last_delivery_unix),
                                          SUBCON_DELIVERY_FORMAT);
                        stage = SUBCON_STAGE::DELIVERING;
                        DEBUG_LOG_CLASS::instance().log_event("subcon", "deciding whether now is a good moment to deliver");
                    }
                }
            }

            // ANNOUNCING itself - synchronous, no subcon_llm call, resolves
            // in the same tick it's entered. Builds one digest of every
            // pending note into pending_announcement for exchange() to
            // carry out to main.cpp (subcon_worker.h's own comment on why
            // this is a plain string, not a COMMS field), which marks
            // them all delivered, and returns to IDLE.
            if (stage == SUBCON_STAGE::ANNOUNCING)
            {
                // Re-checked now, not trusted from when this was decided -
                // see current_delivery_action()'s own comment. If the
                // situation changed, the notes simply stay queued and the
                // regular delivery check picks them up again.
                std::string digest;
                if (current_delivery_action() == SUBCON_DELIVERY_ACTION::NOTHING)
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "conditions changed while deciding - not announcing now");
                else
                    digest = subcon_build_digest(tell_later_queue);

                if (!digest.empty())
                {
                    pending_announcement = digest;
                    save_subcon_queue(subcon_queue_path, tell_later_queue);
                    last_delivery_unix = now_unix_seconds;
                    delivery_hold_until_unix = 0;
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "announcing: \"" + pending_announcement + "\"");
                }
                stage = SUBCON_STAGE::IDLE;
            }

            // Drives whatever's currently in flight, if anything - same
            // async submit-and-poll pattern the old one-shot version used,
            // shared across every stage.
            // process() can run a synchronous model call itself (narrating
            // a tool result), and an exception out of one used to unwind
            // thread_main() entirely - subcon silently stopped for the rest
            // of the session (found in review 2026-10-28: a cut multi-byte
            // character in a fetched page). Logged and carried on instead;
            // the stage's own completion check then sees whatever state was
            // left.
            try
            {
                subcon_llm.input(subcon_comms, tool_worker);
                subcon_llm.process(subcon_io_worker, nullptr, subcon_tools_list, tool_worker, subcon_comms);
            }
            catch (const std::exception& e)
            {
                DEBUG_LOG_CLASS::instance().log_event("subcon", std::string("model/tool processing threw: ") + e.what());
                subcon_llm.is_processing = false;
            }

            // PRIORITIZING's own call finished (one way or another - see
            // this section's own comment above, SUBCON_STAGE's own
            // declaration, for why this is gated on !is_processing alone,
            // not also requiring complete/a non-empty response).
            if (stage == SUBCON_STAGE::PRIORITIZING && !subcon_llm.is_processing)
            {
                if (!subcon_llm.last_received.complete || subcon_llm.last_received.response.empty())
                {
                    // A failure isn't a real "wait" decision the model made -
                    // just retry soon rather than either spinning tight or
                    // going quiet indefinitely.
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "prioritize call failed or produced nothing - checking again shortly");
                    think_cycle_timer.set(60000); // ms
                    stage = SUBCON_STAGE::IDLE;
                }
                else
                {
                    try
                    {
                        json parsed = json::parse(subcon_llm.last_received.response);
                        bool should_run_now = parsed.at("should_run_now").get<bool>();
                        chosen_index = parsed.at("chosen_index").get<int>();
                        chosen_reasoning = parsed.at("reasoning").get<std::string>();
                        int wait_minutes = parsed.at("wait_minutes").get<int>();

                        // Recomputed, not remembered from when the call was
                        // sent - the answer is checked against what is
                        // allowed now, and a model that names a task it
                        // wasn't offered (a capped one) is treated the same
                        // as an out-of-range index.
                        std::vector<SUBCON_ELIGIBILITY> eligibility;
                        std::vector<size_t> eligible = eligible_task_indices(task_items, now_unix_seconds, presence_tracker.last_arrival_unix, eligibility);

                        // TEMP-SMOKETEST - see SUBCON_FORCE_RUN_FOR_TESTING's
                        // own comment above. Logs the real, unmodified
                        // decision first, so the actual pacing behavior
                        // stays visible even while this forces past it.
                        if (SUBCON_FORCE_RUN_FOR_TESTING && !should_run_now && !eligible.empty())
                        {
                            DEBUG_LOG_CLASS::instance().log_event("subcon",
                                "[TEMP-SMOKETEST] would have stayed idle (" + chosen_reasoning +
                                ", proposed " + std::to_string(wait_minutes) + " min) - forcing a run anyway");
                            should_run_now = true;

                            // When the model decides to wait it has no reason
                            // to fill in a valid chosen_index (the prompt only
                            // asks for one when it's running something), so a
                            // forced run used to bounce straight off the range
                            // check below and no cycle ever completed (found
                            // 2026-10-28: 7 forced runs, 0 completed). Pick
                            // the stalest eligible task instead - never-
                            // checked first, otherwise the oldest last_checked.
                            if (std::find(eligible.begin(), eligible.end(), static_cast<size_t>(std::max(chosen_index, 0))) == eligible.end() || chosen_index < 0)
                            {
                                size_t stalest = eligible.front();
                                for (size_t i : eligible)
                                {
                                    const SUBCON_TODO_ITEM& a = task_items[i];
                                    const SUBCON_TODO_ITEM& b = task_items[stalest];
                                    bool older = (!a.ever_checked && b.ever_checked) ||
                                                 (a.ever_checked == b.ever_checked && a.last_checked_unix_seconds < b.last_checked_unix_seconds);
                                    if (older) stalest = i;
                                }
                                chosen_index = static_cast<int>(stalest);
                            }
                        }

                        // Qwen3's own real thinking trace (use_thinking =
                        // true, set above) - generated either way, just not
                        // looked at until now. Not the same as 'reasoning'
                        // above (which is the model explicitly asked to
                        // justify its answer after the fact) - this is its
                        // actual step-by-step reasoning before landing on
                        // that answer. Logged regardless of which branch
                        // below is taken.
                        if (!subcon_llm.last_received.thinking.empty())
                            DEBUG_LOG_CLASS::instance().log_event("subcon", "thinking: " + subcon_llm.last_received.thinking);

                        if (!should_run_now)
                        {
                            // The actual point of this whole change - the
                            // model itself decides nothing's worth acting on
                            // right now, and how long to wait, instead of a
                            // fixed timer always finding *something* to do
                            // every single tick. Clamped regardless of what
                            // it proposed - SUBCON_WAIT_MINUTES_MIN/MAX's
                            // own comment explains why a hard bound stays
                            // even though this is otherwise the model's call.
                            int clamped_minutes = std::clamp(wait_minutes, SUBCON_WAIT_MINUTES_MIN, SUBCON_WAIT_MINUTES_MAX);
                            DEBUG_LOG_CLASS::instance().log_event("subcon",
                                "staying idle - " + chosen_reasoning + " (checking again in " +
                                std::to_string(clamped_minutes) + " minute(s), proposed " + std::to_string(wait_minutes) + ")");
                            think_cycle_timer.set(clamped_minutes * 60000);
                            stage = SUBCON_STAGE::IDLE;
                        }
                        else if (chosen_index >= 0 && std::find(eligible.begin(), eligible.end(), static_cast<size_t>(chosen_index)) != eligible.end())
                        {
                            SUBCON_TODO_ITEM& chosen = task_items[static_cast<size_t>(chosen_index)];
                            DEBUG_LOG_CLASS::instance().log_event("subcon", "chose: \"" + chosen.description + "\" - " + chosen_reasoning);

                            // The attempt counts toward the task's allowance
                            // from the moment it starts, not when it
                            // finishes - a cycle that fails halfway must not
                            // be free to retry in a loop.
                            subcon_record_run(chosen, now_unix_seconds);
                            save_subcon_todo_items(subcon_todo_items_path, task_items);

                            // No response_format this time (plain input(),
                            // not start_subcon_call()) - the act turn has to
                            // be free to answer with tool calls. Found live
                            // that without being told otherwise, the model
                            // just repeated the previous turn's own JSON
                            // shape verbatim instead of writing a real
                            // answer, apparently mimicking the immediately
                            // preceding turn's own style even though
                            // nothing constrains this one to JSON at all -
                            // build_act_prompt() carries the explicit
                            // "plain sentences, not JSON" for that.
                            subcon_comms.INPUT_FROM_USER.set(build_act_prompt(chosen));
                            subcon_comms.ENTER_PRESSED = true;
                            stage = SUBCON_STAGE::PLANNING;
                        }
                        else
                        {
                            DEBUG_LOG_CLASS::instance().log_event("subcon", "should_run_now was true but chosen_index isn't an allowed task - checking again shortly");
                            think_cycle_timer.set(60000); // ms
                            stage = SUBCON_STAGE::IDLE;
                        }
                    }
                    catch (const std::exception& e)
                    {
                        DEBUG_LOG_CLASS::instance().log_event("subcon", std::string("prioritize reply wasn't valid structured JSON: ") + e.what());
                        think_cycle_timer.set(60000); // ms
                        stage = SUBCON_STAGE::IDLE;
                    }
                }

                subcon_llm.last_received.response.clear();
            }
            // PLANNING's own call (the act turn) finished - same "resolved one way or
            // another" reasoning as PRIORITIZING above, plus: not while a
            // tool call is still unfinished. That means both ends of one:
            // a call the model just asked for that process() hasn't
            // dispatched yet (last_received.tool_calls - a call can arrive
            // inside the narration of an earlier one, a tick after
            // is_processing has already gone false; found live 2026-10-28
            // when the finding was requested with list_hue_lights asked
            // for but never run, and the model invented the bulbs), and a
            // dispatched remote call whose result hasn't come back yet
            // (outstanding_tool_call_ids - process() PART 5 claims it on a
            // later tick and narrates it into last_received.response).
            // Without that wait a tool-only reply would also look like
            // "produced nothing."
            //
            // The finding is only asked for if a real tool was actually
            // used. Without one the model can only be describing what it
            // imagines it would have found (the fabrication the 2026-09-26
            // work kept running into), so there is nothing true to report -
            // the attempt is recorded as such and the cycle ends.
            else if (stage == SUBCON_STAGE::PLANNING && !subcon_llm.is_processing &&
                     subcon_llm.last_received.tool_calls.empty() &&
                     subcon_llm.outstanding_tool_call_ids.empty())
            {
                SUBCON_TODO_ITEM& chosen = task_items[static_cast<size_t>(chosen_index)];
                chosen_plan = subcon_llm.last_received.response;
                if (!chosen_plan.empty())
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "act: " + chosen_plan);

                std::vector<std::string> tool_names;
                int tools_used = count_subcon_tool_results(subcon_llm, tool_names);
                cycle_unsafe = false;
                for (const std::string& name : tool_names)
                {
                    if (is_read_only_tool(name)) continue;
                    cycle_unsafe = true;
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "[SAFETY] subcon called a tool outside its read-only set: " + name + " - this cycle's finding will be discarded");
                }

                // Every call the model asked for needs a result in the
                // conversation (a capped or failed call still gets an
                // error result) - a call without one means any "finding"
                // would be built on data the model never saw. Backstop
                // for the wait above, not a replacement for it.
                bool every_call_answered = tool_names.size() <= static_cast<size_t>(tools_used);

                if (tools_used == 0 || !every_call_answered)
                {
                    DEBUG_LOG_CLASS::instance().log_event("subcon", tools_used == 0
                        ? "act turn made no real tool call - nothing was actually checked, recording the attempt and moving on"
                        : "a tool call never got a result - nothing trustworthy to report, recording the attempt and moving on");
                    SUBCON_TODO_RESULT result;
                    result.reasoning = chosen_reasoning;
                    result.plan = chosen_plan;
                    result.finding = tools_used == 0 ? "(no tool was used - nothing was actually checked)" : "(a tool call never got a result - nothing was reported)";
                    result.at_unix = now_unix_seconds;
                    subcon_append_history(chosen, result);
                    save_subcon_todo_items(subcon_todo_items_path, task_items);

                    think_cycle_timer.set(60000); // ms
                    stage = SUBCON_STAGE::IDLE;
                }
                else
                {
                    start_subcon_call(subcon_llm, subcon_comms,
                                      build_finding_prompt(chosen, now_unix_seconds, subcon_recent_told(chosen, now_unix_seconds, 3)),
                                      SUBCON_FINDING_FORMAT);
                    stage = SUBCON_STAGE::RESULTING;
                }

                subcon_llm.last_received.response.clear();
            }
            // RESULTING's own call finished - same "resolved one way or
            // another" reasoning as the stages above. This is where the
            // digest queue entry and history both actually get built - and
            // where a finding is held back unless it's noteworthy (see
            // SUBCON_FINDING_FORMAT's own comment).
            else if (stage == SUBCON_STAGE::RESULTING && !subcon_llm.is_processing)
            {
                SUBCON_TODO_ITEM& chosen = task_items[static_cast<size_t>(chosen_index)];

                if (!subcon_llm.last_received.complete || subcon_llm.last_received.response.empty())
                {
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "finding call failed or produced nothing - abandoning this cycle");
                }
                else
                {
                    try
                    {
                        json parsed = json::parse(subcon_llm.last_received.response);
                        bool noteworthy = parsed.at("noteworthy").get<bool>();
                        std::string urgency = parsed.at("urgency").get<std::string>();
                        std::string summary = parsed.at("summary").get<std::string>();

                        // A tool outside the read-only set means whatever the
                        // model "found" came from a side effect, not a look-up
                        // - never queued, whatever it says about itself.
                        bool queue_it = noteworthy && !summary.empty() && !cycle_unsafe;
                        DEBUG_LOG_CLASS::instance().log_event("subcon",
                            std::string(queue_it ? "finding (worth telling): " : "finding (not queued): ") + summary);

                        if (queue_it)
                        {
                            SUBCON_NOTE note;
                            note.content = summary;
                            note.label = chosen.label;
                            note.urgency = (urgency == "urgent" || urgency == "low") ? urgency : "normal";
                            note.created_unix = now_unix_seconds;
                            tell_later_queue.push_back(note);
                            save_subcon_queue(subcon_queue_path, tell_later_queue);
                        }

                        // Staleness bookkeeping (SUBCON_TODO_ITEM's own
                        // comment) - user's own idea, 2026-09-26, tracked in
                        // real elapsed time rather than a cycle count (2026-
                        // 09-27): only the chosen item needs touching - its
                        // own last_checked_unix_seconds moves to now and it
                        // keeps a real record of what happened via history.
                        chosen.last_checked_unix_seconds = now_unix_seconds;
                        chosen.ever_checked = true;
                        SUBCON_TODO_RESULT result;
                        result.reasoning = chosen_reasoning;
                        result.plan = chosen_plan;
                        result.finding = summary;
                        result.at_unix = now_unix_seconds;
                        result.noteworthy = queue_it;
                        subcon_append_history(chosen, result);
                        save_subcon_todo_items(subcon_todo_items_path, task_items);
                    }
                    catch (const std::exception& e)
                    {
                        DEBUG_LOG_CLASS::instance().log_event("subcon", std::string("finding reply wasn't valid structured JSON: ") + e.what());
                    }
                }

                // Whether this cycle succeeded or failed, ask again
                // reasonably soon - the model gets to decide for itself,
                // next time it's asked, whether anything (including
                // whatever it just did) is worth acting on again or whether
                // to propose a longer wait; this is just the gap until that
                // next ask happens, not a decision about pacing itself.
                think_cycle_timer.set(60000); // ms
                subcon_llm.last_received.response.clear();
                stage = SUBCON_STAGE::IDLE;
            }
            // DELIVERING's own call finished - the model's say on whether
            // this is a good moment (see the delivery-readiness check
            // above for the mechanical rules around it). A failure or an
            // unreadable reply holds off a while rather than delivering
            // blind; SUBCON_NOTE_MAX_HOLD_SECONDS still guarantees a note
            // is never held forever.
            else if (stage == SUBCON_STAGE::DELIVERING && !subcon_llm.is_processing)
            {
                stage = SUBCON_STAGE::IDLE;
                delivery_hold_until_unix = now_unix_seconds + 10 * 60;

                if (!subcon_llm.last_received.complete || subcon_llm.last_received.response.empty())
                {
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "delivery decision failed or produced nothing - holding off 10 minutes");
                }
                else
                {
                    try
                    {
                        json parsed = json::parse(subcon_llm.last_received.response);
                        bool deliver_now = parsed.at("deliver_now").get<bool>();
                        int wait_minutes = parsed.at("wait_minutes").get<int>();
                        std::string reasoning = parsed.at("reasoning").get<std::string>();

                        if (deliver_now)
                        {
                            DEBUG_LOG_CLASS::instance().log_event("subcon", "good moment to deliver - " + reasoning);
                            delivery_hold_until_unix = 0; // the ANNOUNCING re-check must not mistake the failure-default hold above for a wait
                            stage = SUBCON_STAGE::ANNOUNCING;
                        }
                        else
                        {
                            int clamped_minutes = std::clamp(wait_minutes, SUBCON_DELIVERY_WAIT_MINUTES_MIN, SUBCON_DELIVERY_WAIT_MINUTES_MAX);
                            delivery_hold_until_unix = now_unix_seconds + static_cast<int64_t>(clamped_minutes) * 60;
                            DEBUG_LOG_CLASS::instance().log_event("subcon",
                                "holding off on delivery - " + reasoning + " (asking again in " + std::to_string(clamped_minutes) +
                                " minute(s), proposed " + std::to_string(wait_minutes) + ")");
                        }
                    }
                    catch (const std::exception& e)
                    {
                        DEBUG_LOG_CLASS::instance().log_event("subcon", std::string("delivery reply wasn't valid structured JSON: ") + e.what() + " - holding off 10 minutes");
                    }
                }

                subcon_llm.last_received.response.clear();
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Found live 2026-09-26: a real SIGABRT (same signature as the 2026-
    // 09-24 fix - std::thread::~thread() called on a still-joinable
    // thread during subcon_llm's own destruction, which terminates
    // unconditionally per the standard) happening right around shutdown.
    // Root cause: the while(RUN) loop above only ever joins subcon_llm's
    // own chat_thread from inside input()'s/process()'s own polling - if
    // thread_stop() flips RUN false while a call is still in flight
    // (is_processing still true), the loop exits on its very next check
    // without ever getting the chance to see that finish and join it,
    // leaving chat_thread joinable right as subcon_llm goes out of scope
    // below. Same exact bug class, same exact fix, as the 2026-09-22
    // shutdown thread-join fix (TODO.md) already applied to the main
    // chat/background tasks/sidetrack's own instance - subcon_llm never
    // got that treatment since it didn't exist yet at the time.
    // request_exit() (olla.cpp) sets running=false, interrupts anything
    // in flight, and joins chat_thread if it's still joinable - exactly
    // what's needed here, even though subcon_llm's own `running` field
    // isn't otherwise used for anything (this class has its own RUN).
    subcon_llm.request_exit();
}
