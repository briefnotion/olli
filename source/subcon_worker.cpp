#include "subcon_worker.h"

#include "olla.h" // ollama_system
#include "io_worker.h" // IO_WORKER_CLASS - see subcon_io_worker's own comment below

#include <chrono>
#include <thread>
#include <fstream>

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
// same mechanism sidetrack.cpp's DONE-check fix uses) for the "which of
// these is most important" stage below - forces a clean {"chosen_index":
// int, "reasoning": string} instead of free text to parse for an index.
static const json SUBCON_PRIORITIZE_FORMAT = {
    {"type", "object"},
    {"properties", {
        {"chosen_index", {{"type", "integer"}}},
        {"reasoning", {{"type", "string"}}}
    }},
    {"required", json::array({"chosen_index", "reasoning"})}
};

// One past cycle where this item was chosen - the user's own idea (2026-
// 09-26): keep a real history per item, not just a last-checked scalar, so
// there's a record of what was actually decided/planned each time, not
// just when.
struct SUBCON_TODO_RESULT
{
    std::string reasoning; // why it was chosen that time
    std::string plan;      // what it decided to do about it
};

// One candidate for the prioritize stage below - description plus enough
// state to reason about staleness, not just the text alone. cycles_since_
// checked counts completed think-cycles (see thread_main()'s own comment
// on why cycles, not real wall-clock time), not raw ticks - only advances
// once per full prioritize/plan cycle, on every item that wasn't the one
// chosen that cycle.
struct SUBCON_TODO_ITEM
{
    std::string description;
    int cycles_since_checked = 0;
    bool ever_checked = false; // needed so "0 cycles ago" and "never" aren't confused
    std::vector<SUBCON_TODO_RESULT> history;
};

// Turns the candidate list into the actual prompt text for the "prioritize"
// stage below - free function, not inlined, purely to keep thread_main()
// itself readable. Includes each item's own staleness now, not just its
// description, so the model has real recency to weigh alongside importance
// (the user's own original "the 3rd looks important... because I hadn't
// done it in a while" example).
static std::string build_prioritize_prompt(const std::vector<SUBCON_TODO_ITEM>& items)
{
    std::string prompt = "Here are some things you could look into right now:\n";
    for (size_t i = 0; i < items.size(); ++i)
    {
        // "0 cycles ago" reads ambiguously - found live 2026-09-26, the
        // model read it as "no prior check has occurred" (confusing it
        // with never_checked) instead of "checked most recently, very
        // fresh." Special-cased to avoid that specific misreading.
        std::string staleness;
        if (!items[i].ever_checked)
            staleness = "never checked";
        else if (items[i].cycles_since_checked == 0)
            staleness = "checked just last cycle - very fresh";
        else
            staleness = "last checked " + std::to_string(items[i].cycles_since_checked) + " cycle(s) ago";
        prompt += std::to_string(i) + ". " + items[i].description + " (" + staleness + ")\n";
    }
    prompt += "Which one is most important to look into right now, and why? Weigh both "
              "how important it is and how overdue it is - the longer it's been since "
              "something was checked (or if it's never been checked at all), the more "
              "it's worth prioritizing; something checked very recently is less urgent "
              "precisely because it was just confirmed. Set chosen_index to the number "
              "of your choice and reasoning to a brief explanation.";
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
// PRIORITIZING/PLANNING each cover one async subcon_llm call (submitted,
// then polled tick by tick, same input()/process() pattern as before) -
// always resolved one way or another via !is_processing, not gated on
// last_received.complete/response.empty() also being true, so a genuine
// network failure can't leave this stuck forever waiting for a condition
// that will never come.
enum class SUBCON_STAGE { IDLE, PRIORITIZING, PLANNING };

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

void SUBCON_WORKER_CLASS::exchange(COMMS& comms)
{
    // try_lock, not a blocking lock_guard - see this class's own header
    // comment for why: a missed copy here is harmless (main_busy_level
    // just keeps last tick's value, refreshed again next tick), so the
    // main thread should never wait on subcon's own background thread for
    // this. If thread_main() happens to hold comms_mutex right now, just
    // skip this tick's copy entirely rather than stalling.
    std::unique_lock<std::mutex> lock(comms_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;

    // Just the busy level for now, not the whole comms - comms_buffer
    // stays unused/stale until subcon actually needs more than this
    // (subcon_worker.h's own comment on both members).
    main_busy_level = comms.busy_count();
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

    // No display to stream into - subcon_comms below is blank/never shown
    // anywhere, so there's nothing for incremental chunks to write to.
    // send() (olla.cpp) only takes the streaming code path at all if either
    // of these is true; with both off it uses the plain non-streaming
    // request instead and still populates last_received.response/.thinking
    // the same way at the end - no difference in the actual result, just
    // skips work with nowhere to go.
    subcon_llm.PROPS.stream_output = false;
    subcon_llm.PROPS.stream_thinking = false;

    // Empty on purpose - subcon has no tools yet (deliberately passive for
    // now, IDEAS.md's own "subconscious" section). Local, not a class
    // member, same reasoning as subcon_llm above: self-contained to this
    // thread, nothing outside thread_main() needs to reach in. open()'s
    // only use of it is looping tool->configure(*this) per entry - a no-op
    // on empty.
    std::vector<std::unique_ptr<TOOL_BASE>> subcon_tools_list;

    // Own persona, not the default (which is written for a general
    // assistant with tool guidance and was leaking through - visible in an
    // earlier live test as an in-character reply that had nothing to do
    // with what subcon actually is). Updated from the original "nothing
    // real to do yet" placeholder - that wording actively contradicted the
    // prioritize/plan prompts now being sent below. Still a placeholder,
    // not the real design (IDEAS.md's own section covers what that should
    // eventually be) - just no longer self-contradictory. Set before
    // open() - that's what seeds the protected opening message with it
    // (same order TOOL_DELEGATOR's own instance uses, tools.cpp).
    subcon_llm.OLLAMA_OPENING =
        "You are Olli's subconscious: a background reasoning process, "
        "separate from the main conversation the user is having with Olli. "
        "You have no tools yet - you can only think, not act. When given a "
        "list of things to consider, pick the one that matters most right "
        "now and explain why; when asked to plan, describe the concrete "
        "steps briefly and plainly.";

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

    // Fake data to play with (user's own explicit request) - not wired to
    // anything real yet (no presence, no real awareness sources). Purely
    // for proving the prioritize/plan reasoning loop itself works before
    // worrying about where real candidates would come from. Descriptions
    // only here - cycles_since_checked/ever_checked/history all start at
    // their own defaults (SUBCON_TODO_ITEM's own declaration).
    std::vector<SUBCON_TODO_ITEM> fake_todo_items = {
        {"Check whether the RAG collections are up to date", 0, false, {}},
        {"See if there's been any change in the weather worth mentioning", 0, false, {}},
        {"Review the last conversation for anything left unresolved", 0, false, {}}
    };

    SUBCON_STAGE stage = SUBCON_STAGE::IDLE;
    int chosen_index = -1;
    std::string chosen_reasoning; // carries PRIORITIZING's own reasoning through to PLANNING's completion, for the queue entry built there

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
            // prompt used (timer ready AND the real system currently idle).
            if (stage == SUBCON_STAGE::IDLE && think_cycle_timer.is_ready() && main_busy_level < 10)
            {
                think_cycle_timer.set(60000); // ms - re-arm for the next cycle

                start_subcon_call(subcon_llm, subcon_comms, build_prioritize_prompt(fake_todo_items), SUBCON_PRIORITIZE_FORMAT);
                stage = SUBCON_STAGE::PRIORITIZING;
                DEBUG_LOG_CLASS::instance().log_event("subcon", "think-cycle started: prioritizing");
            }

            // Drives whatever's currently in flight, if anything - same
            // async submit-and-poll pattern the old one-shot version used,
            // shared across every stage.
            subcon_llm.input(subcon_comms, nullptr);
            subcon_llm.process(subcon_io_worker, nullptr, subcon_tools_list, nullptr, subcon_comms);

            // PRIORITIZING's own call finished (one way or another - see
            // this section's own comment above, SUBCON_STAGE's own
            // declaration, for why this is gated on !is_processing alone,
            // not also requiring complete/a non-empty response).
            if (stage == SUBCON_STAGE::PRIORITIZING && !subcon_llm.is_processing)
            {
                if (!subcon_llm.last_received.complete || subcon_llm.last_received.response.empty())
                {
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "prioritize call failed or produced nothing - abandoning this cycle");
                    stage = SUBCON_STAGE::IDLE;
                }
                else
                {
                    try
                    {
                        json parsed = json::parse(subcon_llm.last_received.response);
                        chosen_index = parsed.at("chosen_index").get<int>();
                        chosen_reasoning = parsed.at("reasoning").get<std::string>();

                        if (chosen_index >= 0 && chosen_index < static_cast<int>(fake_todo_items.size()))
                        {
                            DEBUG_LOG_CLASS::instance().log_event("subcon",
                                "chose: \"" + fake_todo_items[static_cast<size_t>(chosen_index)].description + "\" - " + chosen_reasoning);

                            // Qwen3's own real thinking trace (use_thinking
                            // = true, set above) - generated either way,
                            // just not looked at until now. Not the same as
                            // 'reasoning' above (which is the model
                            // explicitly asked to justify its answer after
                            // the fact) - this is its actual step-by-step
                            // reasoning before landing on that answer.
                            if (!subcon_llm.last_received.thinking.empty())
                                DEBUG_LOG_CLASS::instance().log_event("subcon", "thinking: " + subcon_llm.last_received.thinking);

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
                            DEBUG_LOG_CLASS::instance().log_event("subcon", "chosen_index out of range - abandoning this cycle");
                            stage = SUBCON_STAGE::IDLE;
                        }
                    }
                    catch (const std::exception& e)
                    {
                        DEBUG_LOG_CLASS::instance().log_event("subcon", std::string("prioritize reply wasn't valid structured JSON: ") + e.what());
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
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "plan call failed or produced nothing");
                }
                else
                {
                    DEBUG_LOG_CLASS::instance().log_event("subcon", "plan: " + subcon_llm.last_received.response);

                    // The digest queue itself (IDEAS.md's own section) -
                    // the first real place subcon's own conclusions land
                    // that isn't just a debug-log line. delivered stays
                    // false - nothing reads or clears this yet, since
                    // actual delivery (presence + not-busy, IDEAS.md's own
                    // "when to announce" note) isn't built.
                    SUBCON_NOTE note;
                    note.content = "Decided to look into: \"" + fake_todo_items[static_cast<size_t>(chosen_index)].description +
                                    "\". Reasoning: " + chosen_reasoning +
                                    " Plan: " + subcon_llm.last_received.response;
                    tell_later_queue.push_back(note);
                    save_subcon_queue(subcon_queue_path, tell_later_queue);

                    // Staleness bookkeeping (SUBCON_TODO_ITEM's own
                    // comment) - user's own idea, 2026-09-26: every item
                    // not chosen this cycle gets a little staler, the one
                    // that WAS chosen resets to fresh and keeps a real
                    // record of what happened, not just a timestamp.
                    for (size_t i = 0; i < fake_todo_items.size(); ++i)
                    {
                        if (i == static_cast<size_t>(chosen_index))
                        {
                            fake_todo_items[i].cycles_since_checked = 0;
                            fake_todo_items[i].ever_checked = true;
                            fake_todo_items[i].history.push_back({chosen_reasoning, subcon_llm.last_received.response});
                        }
                        else
                        {
                            fake_todo_items[i].cycles_since_checked++;
                        }
                    }
                }

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
