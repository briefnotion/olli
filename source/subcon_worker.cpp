#include "subcon_worker.h"

#include "olla.h" // ollama_system
#include "io_worker.h" // IO_WORKER_CLASS - see subcon_io_worker's own comment below

#include <chrono>
#include <thread>
#include <fstream>
#include <algorithm> // std::clamp

// The digest queue's own entry (IDEAS.md's "The digest queue" section) -
// deliberately minimal for now, just enough to give subcon's own
// conclusions somewhere real to land instead of a debug-log line that
// disappears on the next test run. No timestamp/priority/deferred-vs-
// resurface state yet - those are real design questions for once actual
// delivery (presence + not-busy, IDEAS.md's own "when to announce" note)
// is being built, not before.
struct SUBCON_NOTE
{
    std::string content;
    bool delivered = false;
};

static void to_json(json& j, const SUBCON_NOTE& note)
{
    j = json{{"content", note.content}, {"delivered", note.delivered}};
}

static void from_json(const json& j, SUBCON_NOTE& note)
{
    note.content = j.value("content", "");
    note.delivered = j.value("delivered", false);
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
    catch (const std::exception&) { /* treat as empty, same as a missing file */ }

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
        std::ofstream file(path);
        if (!file) return;
        file << json(queue).dump(2, ' ', false, json::error_handler_t::replace);
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

// One past cycle where this item was chosen - the user's own idea (2026-
// 09-26): keep a real history per item, not just a last-checked scalar, so
// there's a record of what was actually decided/planned each time, not
// just when.
struct SUBCON_TODO_RESULT
{
    std::string reasoning; // why it was chosen that time
    std::string plan;      // what it decided to do about it
    std::string finding;   // what it "found" after (pretend) carrying out plan -
                            // the actual outcome, not another how-to. Deliberately
                            // fabricated right now, same as plan already is - subcon
                            // has no tools and no real data behind any of the fake
                            // items, so this can't be grounded in anything real yet.
                            // Kept anyway (the user's own call, 2026-09-26): "let's
                            // learn to fail before we succeed" - see what a toolless
                            // system actually produces when pushed for a concrete
                            // result, rather than solving that honestly up front.
};

// One candidate for the prioritize stage below - description plus enough
// state to reason about staleness, not just the text alone. Real wall-clock
// time now (2026-09-26), not a cycle count - cycles stopped being a
// meaningful unit once the model itself started choosing how long to wait
// between checks (thread_main()'s own comment on the merged prioritize/
// pace decision) - a fixed-length "cycle" no longer exists to count.
// last_checked_unix_seconds is std::chrono::system_clock (real wall time,
// meaningful across a restart), not steady_clock (monotonic but not tied to
// an actual calendar time, the wrong choice for something persisted to
// disk and read back after a restart).
struct SUBCON_TODO_ITEM
{
    std::string description;
    int64_t last_checked_unix_seconds = 0;
    bool ever_checked = false; // needed so "0 seconds ago" and "never" aren't confused
    std::vector<SUBCON_TODO_RESULT> history;
};

static void to_json(json& j, const SUBCON_TODO_RESULT& result)
{
    j = json{{"reasoning", result.reasoning}, {"plan", result.plan}, {"finding", result.finding}};
}

static void from_json(const json& j, SUBCON_TODO_RESULT& result)
{
    result.reasoning = j.value("reasoning", "");
    result.plan = j.value("plan", "");
    result.finding = j.value("finding", "");
}

static void to_json(json& j, const SUBCON_TODO_ITEM& item)
{
    j = json{
        {"description", item.description},
        {"last_checked_unix_seconds", item.last_checked_unix_seconds},
        {"ever_checked", item.ever_checked},
        {"history", item.history}
    };
}

static void from_json(const json& j, SUBCON_TODO_ITEM& item)
{
    item.description = j.value("description", "");
    item.last_checked_unix_seconds = j.value("last_checked_unix_seconds", static_cast<int64_t>(0));
    item.ever_checked = j.value("ever_checked", false);
    item.history = j.value("history", std::vector<SUBCON_TODO_RESULT>{});

    // Real bug, caught live 2026-09-27 on the "ron" profile: a file
    // persisted by the previous, cycle-counting version of this struct has
    // "ever_checked": true but no last_checked_unix_seconds key at all
    // (that field didn't exist yet), which defaults to 0 above - the Unix
    // epoch. describe_staleness() then reported "497357 hours ago"
    // (~56 years) instead of recognizing this as stale, pre-migration data.
    // Since a real check can never have happened at the epoch, ever_checked
    // = true with a still-zero timestamp is unambiguously old-format
    // leftover, not a real "checked at second 0" - safe to treat as never
    // checked (matches ever_checked's own declared purpose) rather than
    // guessing at a real time to backfill.
    if (item.ever_checked && item.last_checked_unix_seconds == 0)
        item.ever_checked = false;
}

// Loads the persisted todo items from disk - falls back to seed_items (the
// hardcoded fake list, thread_main()'s own declaration) on a missing file
// (first run for this profile) or any parse trouble, rather than an empty
// list, since an empty list would leave the reasoning loop with nothing to
// prioritize over at all - unlike the queue above, where empty is a
// perfectly normal starting state.
static std::vector<SUBCON_TODO_ITEM> load_subcon_todo_items(const std::filesystem::path& path, const std::vector<SUBCON_TODO_ITEM>& seed_items)
{
    std::ifstream file(path);
    if (!file) return seed_items;

    try
    {
        json data;
        file >> data;
        return data.get<std::vector<SUBCON_TODO_ITEM>>();
    }
    catch (const std::exception&) { return seed_items; /* treat as first run */ }
}

// Same UTF-8-safety shape as save_subcon_queue() above (its own comment
// explains why error_handler_t::replace + a try/catch, not just one or the
// other) - reasoning/plan/finding are all live, free-form model output same
// as a queue note's content is.
static void save_subcon_todo_items(const std::filesystem::path& path, const std::vector<SUBCON_TODO_ITEM>& items)
{
    try
    {
        std::ofstream file(path);
        if (!file) return;
        file << json(items).dump(2, ' ', false, json::error_handler_t::replace);
    }
    catch (const std::exception& e)
    {
        DEBUG_LOG_CLASS::instance().log_event("subcon", std::string("failed to save todo items: ") + e.what());
    }
}

// Real elapsed time since last_checked_unix_seconds, in the same "never
// checked" / "very fresh" / "N minutes/hours ago" phrasing the old
// cycle-counting version used - same reasoning for the "just now" special
// case as that version's own "0 cycles ago" fix: an unqualified "0 minutes
// ago" risks reading as "never" rather than "extremely recent."
static std::string describe_staleness(const SUBCON_TODO_ITEM& item, int64_t now_unix_seconds)
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

// Turns the candidate list into the actual prompt text for the merged
// prioritize/pace decision below - free function, not inlined, purely to
// keep thread_main() itself readable. Includes each item's own staleness
// now, not just its description, so the model has real recency to weigh
// alongside importance (the user's own original "the 3rd looks important...
// because I hadn't done it in a while" example) - and now also asks
// whether now is even a good moment to act at all, rather than assuming
// something must always be worth doing the moment it's asked (the user's
// own idea, 2026-09-26/27: "if it seems not enough time has passed, figure
// out a good sleep time, for when i should check again").
static std::string build_prioritize_prompt(const std::vector<SUBCON_TODO_ITEM>& items, int64_t now_unix_seconds)
{
    std::string prompt = "Here are some things you could look into right now:\n";
    for (size_t i = 0; i < items.size(); ++i)
        prompt += std::to_string(i) + ". " + items[i].description + " (" + describe_staleness(items[i], now_unix_seconds) + ")\n";

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

// Same direct chat_thread-spawn shape as sidetrack.cpp's own
// start_second_guess_call() - needed specifically because ollama_system::
// input() (the ENTER_PRESSED-driven submission path used for the PLANNING
// stage below, which just wants free text) has no way to pass a
// response_format through to send() at all. Only used for the
// PRIORITIZING stage. comms captured by reference, same as
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
        instance.send(nullptr, comms, "user", response_format);
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
// was already decided (whichever SUBCON_NOTE the delivery-readiness check
// picked), this stage just stages it into pending_announcement for
// exchange() to carry out and immediately resolves, same tick.
enum class SUBCON_STAGE { IDLE, PRIORITIZING, PLANNING, RESULTING, ANNOUNCING };

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

    // Seed data for a fresh profile only (load_subcon_todo_items() below
    // falls back to this on a missing/unparseable file) - once persisted,
    // fake_todo_items itself is loaded from disk, not rebuilt from this
    // literal every time. Aggregate-initialized positionally against
    // SUBCON_TODO_ITEM's own declared order (description,
    // last_checked_unix_seconds, ever_checked, history) - 0/false/{} is
    // "never checked" regardless of which type last_checked_unix_seconds is.
    //
    // Refreshed 2026-09-27, now that subcon has full tool access
    // (populate_default_tools() + tool_worker, above) - chosen deliberately
    // for having a real, checkable answer through something subcon can
    // actually reach, not just plausible-sounding fake topics anymore:
    //   - lights: list_hue_lights (read-only, real device state)
    //   - weather: web_search (real search results)
    //   - last conversation: rag_search against the real, auto-synced
    //     "conversations" collection (tools/rag/rag_db/rag_sync.cpp) - not
    //     a dead end like it looked at first; olli already indexes its own
    //     past chat_logs/ there.
    //   - notes: rag_search against the real "Notes" collection - closest
    //     of all five to the original subconscious brainstorm's own seed
    //     scenario (IDEAS.md: noticing a job-offer email worth mentioning),
    //     just notes instead of email.
    //   - timers: clock's own list_timers/check_timer (read-only).
    //
    // "Check whether the RAG collections are up to date" (the original
    // first item) dropped, not just replaced - the user's own call: it
    // "never failed," nothing about it ever produces a real, discriminating
    // answer, unlike the five below.
    std::vector<SUBCON_TODO_ITEM> seed_todo_items = {
        {"Check whether any lights were left on that probably shouldn't be", 0, false, {}},
        {"See if there's been any change in the weather worth mentioning", 0, false, {}},
        {"Review the last conversation for anything left unresolved", 0, false, {}},
        {"Check my notes for anything time-sensitive or coming up soon", 0, false, {}},
        {"Check if any timers are currently running or about to expire", 0, false, {}}
    };

    // Persisted to disk now (2026-09-26) - same directory as queue.json,
    // same load/save shape. Closes a real gap: before this, a restart wiped
    // every item's staleness/ever_checked/history back to fresh, as if no
    // cycle had ever run. Also the actual point of this change - the user
    // wanted a way to see these outside of a temporary debug-log hack.
    std::filesystem::path subcon_todo_items_path = subcon_llm.PROPS.OLLI_DIRECTORY / "todo_items.json";
    std::vector<SUBCON_TODO_ITEM> fake_todo_items = load_subcon_todo_items(subcon_todo_items_path, seed_todo_items);

    // Off by default (2026-09-26) - the whole reasoning loop above still
    // only has fake_todo_items to work with, so leaving this on burns real
    // GPU/LLM time every 60s producing fabricated findings about made-up
    // topics into whatever profile happens to be running - fine for a
    // deliberate test session, not something to leave running unattended
    // in real day-to-day use. Flip true for testing; everything else in
    // this thread (exchange()'s busy/presence copy, the delivery-readiness
    // check) keeps running either way - only the "start a new think-cycle"
    // gate below is affected.
    constexpr bool SUBCON_THINK_CYCLE_ENABLED = false;

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
    std::string chosen_reasoning; // carries PRIORITIZING's own reasoning through to RESULTING's completion, for the queue entry/history built there
    std::string chosen_plan;      // carries PLANNING's own plan through to RESULTING's completion, same reasoning
    int announcing_note_index = -1; // which tell_later_queue entry ANNOUNCING is currently staging, set by the delivery-readiness check below

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

            // Start a new think-cycle - same gate the old one-shot test
            // prompt used (timer ready AND the real system currently idle),
            // plus the enable switch declared above. No re-arm here anymore
            // (2026-09-26/27) - think_cycle_timer's own next interval is now
            // a real decision the model makes as part of this call's own
            // result (see the completion branch below), not a fixed
            // constant set blindly at dispatch time.
            if (SUBCON_THINK_CYCLE_ENABLED && stage == SUBCON_STAGE::IDLE && think_cycle_timer.is_ready() && main_busy_level < 10)
            {
                int64_t now_unix_seconds = static_cast<int64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
                start_subcon_call(subcon_llm, subcon_comms, build_prioritize_prompt(fake_todo_items, now_unix_seconds), SUBCON_PRIORITIZE_FORMAT);
                stage = SUBCON_STAGE::PRIORITIZING;
                DEBUG_LOG_CLASS::instance().log_event("subcon", "think-cycle started: prioritizing");
            }

            // Delivery-readiness check - independent of the reasoning-cycle
            // stages (a previously-queued note can become ready to surface
            // while a new reasoning cycle is or isn't running), but only
            // actually acts when stage == IDLE and nothing's already
            // staged - doesn't interrupt an in-flight PRIORITIZING/PLANNING/
            // RESULTING call (the user's own pseudocode's "special" case
            // does say "interrupt whatever was going on" for something
            // urgent, but presence/idle timing isn't that - a genuine
            // interrupt-mid-call is real future scope, not this). "near"
            // matches presence.cpp's own vocabulary (profile.is_near) - see
            // comms.h/olla.cpp's own PART 5 for where this value actually
            // comes from. Picks at most one note per firing (the oldest
            // undelivered one), not the whole backlog at once - a cheap way
            // to get "one thing at a time" (IDEAS.md's own digest-queue
            // section) without new pacing machinery, given this timer only
            // fires once per delivery_check_timer.set() interval above.
            if (delivery_check_timer.is_ready())
            {
                delivery_check_timer.set(30000); // ms - re-arm for the next check

                if (stage == SUBCON_STAGE::IDLE && pending_announcement.empty() &&
                    user_presence == "near" && main_busy_level < 10)
                {
                    for (size_t i = 0; i < tell_later_queue.size(); ++i)
                    {
                        if (tell_later_queue[i].delivered) continue;

                        announcing_note_index = static_cast<int>(i);
                        stage = SUBCON_STAGE::ANNOUNCING;
                        break;
                    }
                }
            }

            // ANNOUNCING itself - synchronous, no subcon_llm call, resolves
            // in the same tick it's entered. Stages the note's own content
            // into pending_announcement for exchange() to carry out to
            // main.cpp (subcon_worker.h's own comment on why this is a
            // plain string, not a COMMS field), marks it delivered, and
            // "cleans up" (the user's own pseudocode wording) by clearing
            // announcing_note_index and returning to IDLE.
            if (stage == SUBCON_STAGE::ANNOUNCING)
            {
                pending_announcement = tell_later_queue[static_cast<size_t>(announcing_note_index)].content;
                tell_later_queue[static_cast<size_t>(announcing_note_index)].delivered = true;
                save_subcon_queue(subcon_queue_path, tell_later_queue);
                DEBUG_LOG_CLASS::instance().log_event("subcon", "announcing: \"" + pending_announcement + "\"");

                announcing_note_index = -1;
                stage = SUBCON_STAGE::IDLE;
            }

            // Drives whatever's currently in flight, if anything - same
            // async submit-and-poll pattern the old one-shot version used,
            // shared across every stage.
            subcon_llm.input(subcon_comms, tool_worker);
            subcon_llm.process(subcon_io_worker, nullptr, subcon_tools_list, tool_worker, subcon_comms);

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

                        // TEMP-SMOKETEST - see SUBCON_FORCE_RUN_FOR_TESTING's
                        // own comment above. Logs the real, unmodified
                        // decision first, so the actual pacing behavior
                        // stays visible even while this forces past it.
                        if (SUBCON_FORCE_RUN_FOR_TESTING && !should_run_now)
                        {
                            DEBUG_LOG_CLASS::instance().log_event("subcon",
                                "[TEMP-SMOKETEST] would have stayed idle (" + chosen_reasoning +
                                ", proposed " + std::to_string(wait_minutes) + " min) - forcing a run anyway");
                            should_run_now = true;
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
                        else if (chosen_index >= 0 && chosen_index < static_cast<int>(fake_todo_items.size()))
                        {
                            DEBUG_LOG_CLASS::instance().log_event("subcon",
                                "chose: \"" + fake_todo_items[static_cast<size_t>(chosen_index)].description + "\" - " + chosen_reasoning);

                            // No response_format this time (plain input(),
                            // not start_subcon_call()) - but found live
                            // that without being told otherwise, the model
                            // just repeated the previous turn's own JSON
                            // shape verbatim instead of writing a real
                            // plan, apparently mimicking the immediately
                            // preceding turn's own style even though
                            // nothing constrains this one to JSON at all.
                            // Explicit "plain sentences, not JSON" fixes it.
                            subcon_comms.INPUT_FROM_USER.set(
                                "Let's do this: \"" + fake_todo_items[static_cast<size_t>(chosen_index)].description +
                                "\". What are the concrete steps to actually do it? Answer in plain "
                                "sentences, not JSON. Keep it brief.");
                            subcon_comms.ENTER_PRESSED = true;
                            stage = SUBCON_STAGE::PLANNING;
                        }
                        else
                        {
                            DEBUG_LOG_CLASS::instance().log_event("subcon", "should_run_now was true but chosen_index out of range - checking again shortly");
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
            // PLANNING's own call finished - same "resolved one way or
            // another" reasoning as PRIORITIZING above.
            else if (stage == SUBCON_STAGE::PLANNING && !subcon_llm.is_processing)
            {
                if (!subcon_llm.last_received.complete || subcon_llm.last_received.response.empty())
                {
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "plan call failed or produced nothing - abandoning this cycle");
                    stage = SUBCON_STAGE::IDLE;
                }
                else
                {
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "plan: " + subcon_llm.last_received.response);
                    chosen_plan = subcon_llm.last_received.response;

                    // Ask for the actual outcome, not another how-to - see
                    // SUBCON_TODO_RESULT's own comment on finding for why
                    // this is its own stage rather than folded into plan
                    // above. Deliberately open/neutral wording, not nudged
                    // toward honesty about having no real way to know - the
                    // point right now is observing what a toolless system
                    // actually produces when pushed for a result, not
                    // solving that up front (user's own call, 2026-09-26).
                    subcon_comms.INPUT_FROM_USER.set(
                        "You just did that. What's the actual result or finding? Answer in plain "
                        "sentences, not JSON. Keep it brief.");
                    subcon_comms.ENTER_PRESSED = true;
                    stage = SUBCON_STAGE::RESULTING;
                }

                subcon_llm.last_received.response.clear();
            }
            // RESULTING's own call finished - same "resolved one way or
            // another" reasoning as the stages above. This is where the
            // digest queue entry and history both actually get built now -
            // moved here from PLANNING's own completion (pre-2026-09-26)
            // once a real finding existed to build them from instead of
            // just a procedure.
            else if (stage == SUBCON_STAGE::RESULTING && !subcon_llm.is_processing)
            {
                if (!subcon_llm.last_received.complete || subcon_llm.last_received.response.empty())
                {
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "result call failed or produced nothing - abandoning this cycle");
                }
                else
                {
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "finding: " + subcon_llm.last_received.response);

                    // The digest queue itself (IDEAS.md's own section) -
                    // the first real place subcon's own conclusions land
                    // that isn't just a debug-log line. delivered stays
                    // false until the delivery-readiness gate above marks
                    // it. Built from the finding now, not the plan - a
                    // finding is what's actually sayable to a user ("I
                    // looked - nothing was unresolved"), a procedure never
                    // was.
                    SUBCON_NOTE note;
                    note.content = "Regarding \"" + fake_todo_items[static_cast<size_t>(chosen_index)].description +
                                    "\": " + subcon_llm.last_received.response;
                    tell_later_queue.push_back(note);
                    save_subcon_queue(subcon_queue_path, tell_later_queue);

                    // Staleness bookkeeping (SUBCON_TODO_ITEM's own
                    // comment) - user's own idea, 2026-09-26, now tracked in
                    // real elapsed time rather than a cycle count (2026-09-
                    // 27): only the chosen item needs touching - its own
                    // last_checked_unix_seconds moves to now and it keeps a
                    // real record of what happened via history. Every other
                    // item needs no update at all anymore - its own
                    // staleness is just "now minus its own last_checked_
                    // unix_seconds," computed fresh wherever it's needed
                    // (describe_staleness()) rather than incremented by hand
                    // every cycle.
                    fake_todo_items[static_cast<size_t>(chosen_index)].last_checked_unix_seconds =
                        static_cast<int64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
                    fake_todo_items[static_cast<size_t>(chosen_index)].ever_checked = true;
                    fake_todo_items[static_cast<size_t>(chosen_index)].history.push_back({chosen_reasoning, chosen_plan, subcon_llm.last_received.response});

                    save_subcon_todo_items(subcon_todo_items_path, fake_todo_items);
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
