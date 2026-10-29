#include "reasoner.hpp"

#include <algorithm>
#include <cctype>

using json = nlohmann::json;

namespace {
    // Subcon's persona - the old OLLAMA_OPENING (source/subcon_worker.cpp),
    // with one deliberate change: that text said "you have the same tools
    // Olli's main conversation does, including ones that can take real-world
    // action... you must never actually use those", because the old subcon
    // was handed olli's whole tool set and kept passive only by this prompt.
    // This tool only ever offers the read-only allowlist below, so the
    // persona says what is true instead of warning about tools it can't see.
    const char* const SUBCON_PERSONA =
        "You are Olli's subconscious: a background reasoning process, separate from "
        "the main conversation the user is having with Olli. You can only look things "
        "up (web search, checking status, reading data) - you can't control a device, "
        "send anything, or change anything in the outside world. When given a list of "
        "things to consider, pick the one that matters most right now and explain why; "
        "when asked to act, use your tools and report plainly what they showed.";

    // The only tools subcon is ever offered. The old in-olli subcon technically
    // had the whole tool set and used this list only as an after-the-fact
    // audit (is_read_only_tool(): log [SAFETY], discard the finding). Here
    // the filter is applied up front - a write tool is never even shown to
    // the model - and the audit remains as a second line: a call to anything
    // not offered is refused, logged [SAFETY], and its cycle's finding is
    // thrown away. Names are the ones the tools actually register:
    // tools/clock (get_clock_time, check_timer, list_timers), tools/hue
    // (list_hue_lights - set_hue_light and manage_hue_scenes write and are
    // NOT here), tools/presence (check_presence), tools/rag (rag_*), and this
    // tool's own internal ones (web_search, fetch_website_content,
    // get_clock_time).
    bool is_read_only_tool(const std::string& name)
    {
        static const char* const allowed[] = {
            "web_search", "fetch_website_content", "rag_list_collections", "rag_search", "rag_search_documents",
            "rag_get_document", "list_hue_lights", "check_presence", "list_timers", "check_timer", "get_clock_time"
        };
        for (const char* a : allowed)
            if (name == a) return true;
        return false;
    }

    // Tool results go back into an 8192-token context (num_ctx), alongside
    // the thinking-mode answer. 6000 bytes is roughly 1500 tokens: enough for
    // several rag excerpts or a search page, small enough that a few rounds fit.
    constexpr size_t TOOL_RESULT_MAX_BYTES = 6000;

    // Review finding 2026-10-29: a reply with 40 tool calls ran all 40, and
    // 6 rounds x N calls x 6000 bytes can overflow num_ctx - Ollama then
    // silently drops the OLDEST messages, which would be the system persona
    // and the task prompt. Two caps keep the conversation inside it:
    //
    // At most this many calls are executed per model reply; the rest are
    // answered with an error (every call needs an answer or the message
    // pairing breaks). The real tasks need one or two calls.
    constexpr size_t MAX_CALLS_PER_REPLY = 4;

    // And this much role=tool content in a whole cycle. Sized from num_ctx
    // 8192 (the default, ollama_client.hpp): the ~21 tool definitions sent
    // with every act request are ~2500 tokens, the persona + prioritize
    // exchange + act prompt + assistant turns ~1000, the thinking-mode
    // answer needs ~1500 of headroom - leaving roughly 3000 tokens, i.e.
    // ~12000 bytes of results. Once it is used up, later results are
    // replaced by a short error and the cycle goes on to its finding.
    constexpr size_t TOOL_CYCLE_BUDGET_BYTES = 12000;

    // Structured-output schema (Ollama "format") for the merged
    // prioritize/pace decision - see the old subcon_worker.cpp's comment:
    // one call decides both "is anything worth doing right now" and, either
    // way, what to do about it. All four required, unused ones ignored.
    const json PRIORITIZE_FORMAT = {
        {"type", "object"},
        {"properties", {
            {"should_run_now", {{"type", "boolean"}}},
            {"chosen_index", {{"type", "integer"}}},
            {"reasoning", {{"type", "string"}}},
            {"wait_minutes", {{"type", "integer"}}}
        }},
        {"required", json::array({"should_run_now", "chosen_index", "reasoning", "wait_minutes"})}
    };

    // Hard floor/ceiling on the model's own wait_minutes proposal - AI
    // judgment about real-world timing layers on top of a mechanical bound,
    // it doesn't replace one (a proposed 0 could spin the loop tight; a
    // multi-day wait could switch subcon off silently).
    constexpr int WAIT_MINUTES_MIN = 1;
    constexpr int WAIT_MINUTES_MAX = 60;

    // The gate that keeps "nothing new" out of the queue (found 2026-10-28:
    // ron's queue held 141 notes, all "no unresolved issues detected"):
    // only noteworthy:true ever reaches it.
    const json FINDING_FORMAT = {
        {"type", "object"},
        {"properties", {
            {"noteworthy", {{"type", "boolean"}}},
            {"urgency", {{"type", "string"}, {"enum", json::array({"low", "normal", "urgent"})}}},
            {"summary", {{"type", "string"}}}
        }},
        {"required", json::array({"noteworthy", "urgency", "summary"})}
    };

    std::string describe_presence(const std::string& presence)
    {
        if (presence == "near") return "The user is at home.";
        if (presence == "away") return "The user is away from home.";
        return "It isn't known whether the user is home.";
    }

    // What an error result looks like to the model. The old subcon's
    // web_search path told the model plainly "This attempt did NOT succeed -
    // it is an error, not real data" (the 2026-09-16 failure-framing fix) so
    // it explained failures instead of narrating raw curl text as an answer;
    // here the same framing is on the tool message itself.
    std::string frame_failure(const std::string& text)
    {
        return "This attempt did NOT succeed - it is an error, not real data. Do not present it as a real "
               "answer or fill in what it might have said. Details: " + text;
    }

    std::string short_text(const std::string& text, size_t max_bytes)
    {
        std::string one_line = text;
        for (char& c : one_line) if (c == '\n' || c == '\r') c = ' ';
        return truncate_utf8(one_line, max_bytes);
    }

    // check_presence's text is "Presence:\n- ron: NEAR" (tools/presence).
    // Single tracked person per profile is the documented assumption
    // (PROTOCOL.md, `var` event note); with several, anyone near counts as home.
    std::string parse_presence_text(const std::string& text)
    {
        bool near = text.find("NEAR") != std::string::npos;
        bool away = text.find("AWAY") != std::string::npos;
        if (near) return "near";
        if (away) return "away";
        return "";
    }
}

SUBCON_REASONER::SUBCON_REASONER(SUBCON_TOOL_SERVER& server_, JOB_WORKER& worker_, SUBCON_LOG_FN log_, REASONER_CONFIG config_)
    : server(server_), worker(worker_), log_fn(std::move(log_)), config(std::move(config_))
{
    next_cycle_at = steady::now() + std::chrono::seconds(config.first_cycle_delay_seconds);
    next_housekeeping_at = steady::now();
}

int64_t SUBCON_REASONER::unix_now() const
{
    return static_cast<int64_t>(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

void SUBCON_REASONER::schedule_next_cycle(int seconds)
{
    next_cycle_at = steady::now() + std::chrono::seconds(seconds);
}

void SUBCON_REASONER::save_items()
{
    subcon_save_todo_items(items_path, items, [this](const std::string& note) { log(note); });
}

void SUBCON_REASONER::save_queue()
{
    subcon_save_queue(queue_path, queue, [this](const std::string& note) { log(note); });
}

void SUBCON_REASONER::load_state(const std::filesystem::path& state_dir)
{
    abandon_cycle("state is being (re)loaded");

    std::error_code ec;
    std::filesystem::create_directories(state_dir, ec);
    items_path = state_dir / "todo_items.json";
    retired_path = state_dir / "todo_items_retired.json";
    queue_path = state_dir / "queue.json";

    auto note = [this](const std::string& text) { log(text); };
    queue = subcon_load_queue(queue_path, note);
    items = subcon_load_todo_items(items_path, retired_path, note);
    // Persisted straight away so a retired task isn't archived a second time
    // on the next start (same as the old subcon).
    save_items();

    // A reload (profile switch) also restarts the arrival tracking - it
    // described the previous profile's user. A first load keeps whatever
    // presence is already known: a tool can register and be probed before
    // olli's identity (and so the profile) arrives.
    if (loaded) {
        presence_tracker = SUBCON_PRESENCE_TRACKER{};
        presence_value = "unknown";
    }
    loaded = true;
    last_logged_missing_tools.clear(); // a new state dir logs its own situation afresh
    schedule_next_cycle(config.first_cycle_delay_seconds);
    log("state loaded from " + state_dir.string() + ": " + std::to_string(items.size()) + " task(s), " +
        std::to_string(queue.size()) + " note(s) on record, " + std::to_string(subcon_count_pending(queue)) + " pending");
}

bool SUBCON_REASONER::may_start_cycle() const
{
    // SEAM - deliberately empty (the user, 2026-10-29: "no gating for now;
    // a busy signal may come later"). The old in-olli subcon refused to
    // start a cycle while olli's chat was busy (main_busy_level < 10), since
    // Ollama serves one request at a time and a subcon call made the user's
    // own reply wait (measured ~22s vs 3-5s, SUBCON.md). A standalone tool
    // has no view of olli's busy state yet; when one exists (a busy value
    // pushed to this tool), the check belongs here.
    return true;
}

// ---------------------------------------------------------------------
// Presence
// ---------------------------------------------------------------------

void SUBCON_REASONER::on_presence(const std::string& value, const std::string& source)
{
    if (value != "near" && value != "away") return;
    if (value != presence_value) log("presence: " + value + " (" + source + ")");
    presence_value = value;

    // "A cycle between leaving and arriving home": a real arrival (away for
    // a while first, SUBCON_MIN_AWAY_SECONDS) restarts every task's
    // allowance, and the next cycle is pulled in so what just became
    // allowed is looked at soon rather than at the end of whatever wait was
    // in progress.
    if (presence_tracker.update(value, unix_now())) {
        log("user arrived home - task allowances start a new period");
        auto soon = steady::now() + std::chrono::seconds(30);
        if (next_cycle_at > soon) next_cycle_at = soon;
    }
}

void SUBCON_REASONER::probe_presence_if_unknown()
{
    if (presence_value != "unknown" || presence_probe_inflight || !server.has_tool("check_presence")) return;

    presence_probe_inflight = true;
    std::string id = server.call("check_presence", json::object(), [this](const SERVER_CALL_RESULT& result) {
        presence_probe_inflight = false;
        if (!result.ok) { log("presence probe failed: " + short_text(result.text, 200)); return; }
        std::string value = parse_presence_text(result.text);
        if (value.empty()) log("presence probe: couldn't read an answer from: " + short_text(result.text, 200));
        else on_presence(value, "check_presence at startup");
    });
    if (id.empty()) presence_probe_inflight = false;
}

// ---------------------------------------------------------------------
// Pull delivery
// ---------------------------------------------------------------------

SUBCON_PULL_RESULT SUBCON_REASONER::pull_notes()
{
    SUBCON_PULL_RESULT out;
    bool changed = false;

    int64_t now = unix_now();
    // A note nobody heard within 18 hours is dropped rather than read out
    // stale ("a weather note from yesterday afternoon is worse than no note").
    int expired = subcon_expire_old_notes(queue, now);
    if (expired > 0) { log("dropped " + std::to_string(expired) + " note(s) that went unheard too long"); changed = true; }

    // No delivery gating here - not presence, quiet hours, olli's busy
    // state, or a model's opinion on timing. The user asked for none for now
    // (2026-10-29); olli asking IS the "good moment". (The seam for a busy
    // signal is may_start_cycle(), which governs when subcon thinks, not
    // when it answers a direct question.)
    out.note_count = subcon_count_pending(queue);
    if (out.note_count > 0) {
        out.text = subcon_build_digest(queue); // marks every included note delivered
        // The wording olli narrates this in is its own persona's. Same
        // faithful-relay intent as the old DIRECTOR_NOTE announcement
        // (source/olla.cpp integrate_tool_result): report the real facts,
        // change none of them.
        out.special_instruction =
            "These are things your own background thinking noticed earlier, while the user was busy - findings "
            "you worked out yourself, recorded as written in the data above. Tell the user about them in your own voice, "
            "naturally and briefly, as things you noticed or have been thinking about. State every fact exactly "
            "as given - do not add, change or guess at anything, and do not mention tools or checks being run. "
            "Begin speaking now:";
        log("pull: handing over " + std::to_string(out.note_count) + " note(s)");
        changed = true;
    } else {
        out.text = "Nothing on my mind right now.";
        out.special_instruction =
            "Your background thinking has nothing to report right now. Tell the user briefly, in your own voice, "
            "that nothing is on your mind at the moment. Do not invent anything. Begin speaking now:";
        log("pull: olli asked, nothing to hand over"); // so a pull olli made is visible in the log either way
    }

    if (subcon_prune_old_notes(queue, now) > 0) changed = true;
    if (changed) save_queue();
    return out;
}

// ---------------------------------------------------------------------
// Prompts (ported unchanged from the old subcon_worker.cpp - see there for
// the dated findings behind each wording)
// ---------------------------------------------------------------------

// The tool names the model would be offered right now: the read-only
// allowlist applied to subcon's internal tools plus whatever connected tools
// have registered - the same rule as build_offered_tools(), by name only. If
// one of them changes, change both.
std::set<std::string> SUBCON_REASONER::available_tool_names() const
{
    std::set<std::string> names;
    for (const json& def : internal_tool_defs()) {
        std::string name = def["name"].get<std::string>();
        if (is_read_only_tool(name)) names.insert(name);
    }
    for (const json& def : server.tool_defs()) {
        std::string name = def["name"].get<std::string>();
        if (is_read_only_tool(name)) names.insert(name);
    }
    return names;
}

// A task that is allowed by its cap and gap but whose required tools aren't
// available (e.g. rag_search before the rag tool has connected) is left out
// altogether: not offered, so it can't be chosen, and so never recorded as a
// run or counted toward its cap. The tools it was missing are reported in
// missing_tools.
std::vector<size_t> SUBCON_REASONER::eligible_indices(int64_t now_unix, std::vector<SUBCON_ELIGIBILITY>& eligibility, std::set<std::string>& missing_tools) const
{
    std::vector<size_t> indices;
    eligibility.clear();
    missing_tools.clear();
    std::set<std::string> available = available_tool_names();
    for (size_t i = 0; i < items.size(); ++i) {
        eligibility.push_back(subcon_check_eligibility(items[i], now_unix, presence_tracker.last_arrival_unix));
        if (config.ignore_limits) eligibility.back().eligible = true;
        if (!eligibility.back().eligible) continue;

        std::vector<std::string> missing = subcon_missing_tools(items[i].key, available);
        if (!missing.empty()) { missing_tools.insert(missing.begin(), missing.end()); continue; }
        indices.push_back(i);
    }
    return indices;
}

// Only lists tasks allowed to run right now - a capped task is not offered,
// rather than offered with a note asking the model please not to choose it.
// The numbers shown are each task's own index in items, so chosen_index maps
// straight back. Includes staleness and how much allowance is used so the
// model has real recency to weigh, and asks whether now is a good moment at
// all (the user's own idea: "if it seems not enough time has passed, figure
// out a good sleep time").
std::string SUBCON_REASONER::build_prioritize_prompt(const std::vector<size_t>& eligible, const std::vector<SUBCON_ELIGIBILITY>& eligibility, int64_t now_unix) const
{
    std::string prompt = "It is " + subcon_describe_local_time(now_unix) + ". " + describe_presence(presence_value) + "\n";
    prompt += "Here are some things you could look into right now:\n";
    for (size_t i : eligible) {
        prompt += std::to_string(i) + ". " + items[i].description + " (" + subcon_describe_staleness(items[i], now_unix);
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

// The turn that has to carry the real tool call - phrased as an instruction
// to act, not a request for a plan: found 2026-10-28 that "what are the
// concrete steps to actually do it?" got a text description of steps and
// zero tool calls in 0/6 direct trials, where "Do this now using your tools"
// got a real call in 6/6. Deliberately does NOT mention what the task found
// last time ("for comparison, last time..." made the model answer "similar
// to the last check" without calling anything) - the comparison is made in
// the finding stage, once there are fresh results to compare against.
std::string SUBCON_REASONER::build_act_prompt(const SUBCON_TODO_ITEM& item) const
{
    std::string prompt = "Do this now using your tools: \"" + item.description + "\". ";
    if (!item.how_to.empty()) prompt += item.how_to + " ";
    prompt += "Call the tool first - don't answer from memory. Answer in plain sentences, not JSON. Keep it brief.";
    return prompt;
}

std::string SUBCON_REASONER::build_finding_prompt(const SUBCON_TODO_ITEM& item, int64_t now_unix) const
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
    if (!item.history.empty()) {
        const SUBCON_TODO_RESULT& last = item.history.back();
        if (!last.finding.empty() && (last.at_unix == 0 || now_unix - last.at_unix < 2 * SUBCON_WINDOW_SECONDS))
            prompt += " For comparison, the previous check of this found: \"" + last.finding + "\" Judge changes by your results above, not by this.";
    }

    std::vector<std::string> already_told = subcon_recent_told(item, now_unix, 3);
    if (!already_told.empty()) {
        prompt += " The user has already been told:";
        for (const std::string& told : already_told) prompt += " \"" + told + "\"";
        prompt += " Do not report the same thing again unless it has materially changed.";
    }
    return prompt;
}

// ---------------------------------------------------------------------
// The stage machine
// ---------------------------------------------------------------------

std::string SUBCON_REASONER::stage_description() const
{
    switch (stage) {
        case STAGE::IDLE: {
            if (!loaded) return "waiting to learn the profile";
            if (!config.think_cycle_enabled) return "idle (think cycle off)";
            auto wait = std::chrono::duration_cast<std::chrono::seconds>(next_cycle_at - steady::now()).count();
            return wait > 0 ? "idle - next look in " + std::to_string(wait / 60) + "m" + std::to_string(wait % 60) + "s" : "idle - due to look";
        }
        case STAGE::PRIORITIZING: return "choosing what to look into";
        case STAGE::ACTING_MODEL: return "working on: " + items[chosen_index].label + " (asking the model, round " + std::to_string(rounds + 1) + ")";
        case STAGE::ACTING_TOOLS: return "working on: " + items[chosen_index].label + " (waiting for tools, round " + std::to_string(rounds + 1) + ")";
        case STAGE::FINDING: return "working on: " + items[chosen_index].label + " (writing up the finding)";
    }
    return "";
}

void SUBCON_REASONER::abandon_cycle(const std::string& why)
{
    if (stage == STAGE::IDLE && expected_job_id == 0 && slots.empty()) return;

    log("abandoning the cycle in progress: " + why);
    ++cycle_token; // any callback still in flight for this cycle becomes a no-op
    for (const CALL_SLOT& slot : slots)
        if (!slot.done && !slot.server_call_id.empty()) server.abandon(slot.server_call_id);
    slots.clear();
    internal_slot_indices.clear();
    internal_batch_pending = false;
    if (expected_job_id != 0) worker.abort_current(); // its result will arrive stale and be dropped by id
    expected_job_id = 0;
    stage = STAGE::IDLE;
    schedule_next_cycle(config.rethink_seconds);
}

void SUBCON_REASONER::tick()
{
    if (!loaded) return;

    auto now = steady::now();

    // Housekeeping: the queue file reflects expiry even if nobody pulls.
    if (now >= next_housekeeping_at) {
        next_housekeeping_at = now + std::chrono::seconds(60);
        int64_t unix = unix_now();
        int expired = subcon_expire_old_notes(queue, unix);
        int pruned = subcon_prune_old_notes(queue, unix);
        if (expired > 0) log("dropped " + std::to_string(expired) + " note(s) that went unheard too long");
        if (expired > 0 || pruned > 0) save_queue();
    }

    if (stage != STAGE::IDLE) return; // everything else happens in on_job_result()/callbacks
    if (!config.think_cycle_enabled || now < next_cycle_at || worker.busy() || !may_start_cycle()) return;

    start_cycle(unix_now());
}

bool SUBCON_REASONER::submit_chat(const json& tools, const json& format)
{
    JOB job;
    job.kind = JOB::CHAT;
    job.id = next_job_id++;
    job.settings = config.ollama;
    job.messages = conversation;
    job.tools = tools;
    job.format = format;
    if (!worker.submit(std::move(job))) return false;
    expected_job_id = next_job_id - 1;
    return true;
}

void SUBCON_REASONER::start_cycle(int64_t now_unix)
{
    // If presence is unknown (a restart, or the presence tool arrived late)
    // ask now rather than only at registration - cheap, and the answer
    // lands in time for the next cycle's prompt and arrival tracking.
    probe_presence_if_unknown();

    std::vector<SUBCON_ELIGIBILITY> eligibility;
    std::set<std::string> missing_tools;
    std::vector<size_t> eligible = eligible_indices(now_unix, eligibility, missing_tools);

    // Say which tools are missing, but only when that changes - this runs
    // every minute while a tool stays unconnected.
    if (missing_tools != last_logged_missing_tools) {
        last_logged_missing_tools = missing_tools;
        if (missing_tools.empty()) {
            log("the tools some tasks need are all available again");
        } else {
            std::string names;
            for (const std::string& n : missing_tools) names += (names.empty() ? "" : ", ") + n;
            log("tasks that need " + names + " are being skipped until " + (missing_tools.size() > 1 ? "they are" : "it is") + " available");
        }
    }

    if (eligible.empty() && !missing_tools.empty()) {
        // Everything that is otherwise allowed is waiting on a tool: nothing
        // to ask the model about. Look again soon, with no model call.
        schedule_next_cycle(config.rethink_seconds);
        return;
    }

    if (eligible.empty()) {
        int64_t soonest = eligibility.front().next_eligible_unix;
        for (const SUBCON_ELIGIBILITY& e : eligibility) soonest = std::min(soonest, e.next_eligible_unix);

        // Capped at the usual maximum wait, not the full time until the
        // soonest one opens: coming home reopens everything sooner than the
        // clock would, and a re-check with nothing eligible costs no model call.
        int minutes = std::clamp(static_cast<int>((soonest - now_unix + 59) / 60), WAIT_MINUTES_MIN, WAIT_MINUTES_MAX);
        log("every task is at its limit right now - checking again in " + std::to_string(minutes) + " minute(s)");
        schedule_next_cycle(minutes * 60);
        return;
    }

    // Fresh conversation for every cycle - only the persona survives, so a
    // previous cycle's tool results never use up the small context window.
    conversation = json::array({{{"role", "system"}, {"content", SUBCON_PERSONA}}});
    conversation.push_back({{"role", "user"}, {"content", build_prioritize_prompt(eligible, eligibility, now_unix)}});

    if (!submit_chat(nullptr, PRIORITIZE_FORMAT)) { schedule_next_cycle(config.rethink_seconds); return; }
    stage = STAGE::PRIORITIZING;
    log("think-cycle started: prioritizing");
}

void SUBCON_REASONER::on_job_result(JOB_RESULT& result)
{
    if (result.id != expected_job_id || expected_job_id == 0) {
        log("dropping a stale job result (cycle was abandoned)", true);
        return;
    }
    expected_job_id = 0;

    switch (stage) {
        case STAGE::PRIORITIZING: handle_prioritized(result.reply); break;
        case STAGE::ACTING_MODEL: handle_act_reply(result.reply); break;
        case STAGE::FINDING: handle_finding(result.reply); break;
        case STAGE::ACTING_TOOLS: {
            // The batch of internal tool calls finished.
            internal_batch_pending = false;
            for (size_t k = 0; k < internal_slot_indices.size(); ++k) {
                CALL_SLOT& slot = slots[internal_slot_indices[k]];
                slot.done = true;
                if (k < result.tool_results.size()) { slot.ok = result.tool_results[k].ok; slot.text = result.tool_results[k].text; }
                else { slot.ok = false; slot.text = "Error: the tool batch returned no result."; }
            }
            internal_slot_indices.clear();
            maybe_finish_tool_round();
            break;
        }
        case STAGE::IDLE: break;
    }
}

void SUBCON_REASONER::handle_prioritized(const OLLAMA_REPLY& reply)
{
    if (!reply.ok || reply.content.empty()) {
        // A failure isn't a real "wait" decision the model made - just retry
        // soon rather than either spinning tight or going quiet indefinitely.
        log("prioritize call failed or produced nothing (" + (reply.ok ? std::string("empty reply") : reply.error) + ") - checking again shortly");
        stage = STAGE::IDLE;
        schedule_next_cycle(config.rethink_seconds);
        return;
    }

    try {
        json parsed = json::parse(reply.content);
        bool should_run_now = parsed.at("should_run_now").get<bool>();
        int chosen = parsed.at("chosen_index").get<int>();
        chosen_reasoning = parsed.at("reasoning").get<std::string>();
        int wait_minutes = parsed.at("wait_minutes").get<int>();

        // Recomputed, not remembered from when the call was sent - the answer
        // is checked against what is allowed now, and a model that names a
        // task it wasn't offered (a capped one) is treated the same as an
        // out-of-range index.
        int64_t now_unix = unix_now();
        std::vector<SUBCON_ELIGIBILITY> eligibility;
        std::set<std::string> missing_tools;
        std::vector<size_t> eligible = eligible_indices(now_unix, eligibility, missing_tools);

        // Qwen3's own thinking trace - its actual step-by-step reasoning
        // before landing on the answer (not the same as 'reasoning' above,
        // which is the model asked to justify itself afterwards).
        if (!reply.thinking.empty()) log("thinking: " + reply.thinking, true);

        // Test switch, see REASONER_CONFIG::force_run. Logs the real,
        // unmodified decision first so the actual pacing stays visible.
        if (config.force_run && !should_run_now && !eligible.empty()) {
            log("[FORCE-RUN] would have stayed idle (" + chosen_reasoning + ", proposed " + std::to_string(wait_minutes) + " min) - forcing a run anyway");
            should_run_now = true;

            // When the model decides to wait it has no reason to fill in a
            // valid chosen_index, so pick the stalest eligible task instead -
            // never-checked first, otherwise the oldest last_checked (the
            // 2026-10-28 fix: before it, forced runs produced no cycles).
            if (chosen < 0 || std::find(eligible.begin(), eligible.end(), static_cast<size_t>(chosen)) == eligible.end()) {
                size_t stalest = eligible.front();
                for (size_t i : eligible) {
                    const SUBCON_TODO_ITEM& a = items[i];
                    const SUBCON_TODO_ITEM& b = items[stalest];
                    bool older = (!a.ever_checked && b.ever_checked) ||
                                 (a.ever_checked == b.ever_checked && a.last_checked_unix_seconds < b.last_checked_unix_seconds);
                    if (older) stalest = i;
                }
                chosen = static_cast<int>(stalest);
            }
        }

        if (!should_run_now) {
            // The model itself decides nothing's worth acting on right now,
            // and how long to wait, instead of a fixed timer always finding
            // something to do. Clamped regardless of what it proposed.
            int clamped = std::clamp(wait_minutes, WAIT_MINUTES_MIN, WAIT_MINUTES_MAX);
            log("staying idle - " + chosen_reasoning + " (checking again in " + std::to_string(clamped) + " minute(s), proposed " + std::to_string(wait_minutes) + ")");
            stage = STAGE::IDLE;
            schedule_next_cycle(clamped * 60);
        } else if (chosen >= 0 && std::find(eligible.begin(), eligible.end(), static_cast<size_t>(chosen)) != eligible.end()) {
            chosen_index = static_cast<size_t>(chosen);
            SUBCON_TODO_ITEM& task = items[chosen_index];
            log("chose: \"" + task.description + "\" - " + chosen_reasoning);

            // The attempt counts toward the task's allowance from the moment
            // it starts, not when it finishes - a cycle that fails halfway
            // must not be free to retry in a loop.
            subcon_record_run(task, now_unix);
            save_items();

            // The prioritize answer stays in the conversation (as in the old
            // flow): the act prompt carries "plain sentences, not JSON"
            // because the model otherwise mimics the previous turn's JSON.
            conversation.push_back({{"role", "assistant"}, {"content", reply.content}});
            conversation.push_back({{"role", "user"}, {"content", build_act_prompt(task)}});
            begin_act();
        } else {
            log("should_run_now was true but chosen_index isn't an allowed task - checking again shortly");
            stage = STAGE::IDLE;
            schedule_next_cycle(config.rethink_seconds);
        }
    } catch (const std::exception& e) {
        log(std::string("prioritize reply wasn't valid structured JSON: ") + e.what());
        stage = STAGE::IDLE;
        schedule_next_cycle(config.rethink_seconds);
    }
}

json SUBCON_REASONER::build_offered_tools(bool log_it)
{
    // Everything registered by connected tools plus subcon's own internal
    // tools, narrowed to the read-only allowlist. Internal tools win a name
    // clash (a connected clock also offers get_clock_time).
    json plain = internal_tool_defs();
    for (const json& def : server.tool_defs()) {
        if (!is_internal_tool(def["name"].get<std::string>())) plain.push_back(def);
    }

    json offered = json::array();
    offered_names.clear();
    std::string hidden;
    for (const json& def : plain) {
        std::string name = def["name"].get<std::string>();
        if (!is_read_only_tool(name)) { hidden += (hidden.empty() ? "" : ", ") + name; continue; }
        offered_names.insert(name);
        offered.push_back({{"type", "function"}, {"function", def}});
    }

    std::string names;
    for (const std::string& n : offered_names) names += (names.empty() ? "" : ", ") + n;
    if (log_it) log("tools offered: " + names + (hidden.empty() ? "" : " | withheld (not read-only): " + hidden));
    return offered;
}

void SUBCON_REASONER::begin_act()
{
    rounds = 0;
    tools_dispatched = 0;
    tools_ok = 0;
    tool_bytes_used = 0;
    tool_names_used.clear();
    cycle_unsafe = false;
    nudged_about_fake_output = false;
    chosen_plan.clear();
    slots.clear();

    json tools = build_offered_tools(true);
    if (!submit_chat(tools, nullptr)) {
        end_cycle_without_finding("(the model could not be asked - nothing was checked)");
        return;
    }
    stage = STAGE::ACTING_MODEL;
}

void SUBCON_REASONER::handle_act_reply(const OLLAMA_REPLY& reply)
{
    if (!reply.ok) {
        log("act call failed: " + reply.error);
        end_cycle_without_finding("(the model call failed - nothing was checked)");
        return;
    }

    // The model writing a tool's output itself instead of calling the tool.
    // Found in the first live run of this tool (2026-10-29): after a real
    // check_presence call (the user was away) it never called list_hue_lights
    // and instead wrote "<tool_response> Hue Lights: - Living Room Light: ON
    // ..." in its answer - invented bulbs - and the finding built on that was
    // queued as noteworthy. The old in-olli flow had the same weakness (its
    // "undispatched second call -> invented hue bulbs" bug, SUBCON.md notes).
    // One corrective nudge (the fake turn is NOT kept in the conversation);
    // if it happens again nothing true can be reported, so the cycle ends.
    {
        std::string lowered = reply.content;
        for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowered.find("<tool_response") != std::string::npos || lowered.find("<tool_call") != std::string::npos) {
            if (nudged_about_fake_output || rounds >= config.max_tool_rounds) {
                log("the model wrote tool output itself again instead of calling the tool - nothing trustworthy to report");
                end_cycle_without_finding("(the model wrote tool output itself instead of calling the tool - nothing was reported)");
                return;
            }
            nudged_about_fake_output = true;
            ++rounds;
            log("the model wrote tool output itself instead of calling the tool - asking it to make the real call");
            conversation.push_back({{"role", "user"}, {"content",
                "That was not a real tool result - you wrote it yourself. Never write tool output. If you still need "
                "information, make the real tool call now; if you already have everything, just state what the real "
                "tool results above showed."}});
            if (!submit_chat(build_offered_tools(false), nullptr)) end_cycle_without_finding("(the model could not be asked - nothing was reported)");
            return;
        }
    }

    // Echo the assistant's message back as the conversation's next turn,
    // minus its thinking trace (it would spend the small context window and
    // isn't needed to continue).
    json assistant = reply.message;
    assistant.erase("thinking");
    if (!assistant.contains("content") || !assistant["content"].is_string()) assistant["content"] = reply.content;
    conversation.push_back(assistant);

    if (!reply.content.empty()) chosen_plan = reply.content;
    if (!reply.thinking.empty()) log("thinking: " + reply.thinking, true);

    if (reply.tool_calls.empty()) { finish_act(); return; }

    if (rounds >= config.max_tool_rounds) {
        // Not an error worth throwing the cycle away for - whatever the
        // earlier rounds found is still real. Drop the unanswerable last
        // turn (a tool call with no result would leave the conversation
        // malformed) and write up what there is.
        log("tool round limit (" + std::to_string(config.max_tool_rounds) + ") reached - not running further calls, writing up what was found");
        conversation.erase(conversation.size() - 1);
        finish_act();
        return;
    }

    dispatch_calls(reply.tool_calls);
}

void SUBCON_REASONER::dispatch_calls(const std::vector<OLLAMA_TOOL_CALL>& calls)
{
    slots.clear();
    internal_slot_indices.clear();
    internal_batch_pending = false;
    stage = STAGE::ACTING_TOOLS;

    JOB job;
    job.kind = JOB::TOOLS;
    job.api_key = config.web_search_api_key;
    size_t executed = 0; // calls actually run from this reply, against MAX_CALLS_PER_REPLY

    for (const OLLAMA_TOOL_CALL& call : calls) {
        tool_names_used.push_back(call.name);
        slots.push_back({});
        size_t index = slots.size() - 1;
        slots[index].tool_name = call.name;
        slots[index].arguments = call.arguments;
        log("tool call: " + call.name + " " + short_text(call.arguments.dump(-1, ' ', false, json::error_handler_t::replace), 300));

        if (!offered_names.count(call.name)) {
            // Not offered. Either a tool outside the read-only list (the
            // old audit's [SAFETY] case - refused, and this cycle's finding
            // is discarded) or a read-only one nobody currently provides
            // (rag not connected): the model invented it either way, but
            // only the first is a safety event.
            if (!is_read_only_tool(call.name)) {
                cycle_unsafe = true;
                log("[SAFETY] subcon asked for a tool outside its read-only set: " + call.name + " - refused; this cycle's finding will be discarded");
            }
            slots[index].done = true;
            slots[index].text = "Error: there is no tool named '" + call.name + "' available to you.";
            continue;
        }

        if (executed >= MAX_CALLS_PER_REPLY) {
            // Still answered, not dropped: every call in the assistant
            // message needs its own role=tool reply or the pairing breaks.
            slots[index].done = true;
            slots[index].plain = true;
            slots[index].text = "Error: too many tool calls in one step; call at most " + std::to_string(MAX_CALLS_PER_REPLY) + ".";
            continue;
        }
        ++executed;

        slots[index].dispatched = true;
        if (is_internal_tool(call.name)) {
            job.calls.push_back({call.name, call.arguments});
            internal_slot_indices.push_back(index);
        } else {
            uint64_t token = cycle_token;
            std::string id = server.call(call.name, call.arguments, [this, token, index](const SERVER_CALL_RESULT& result) {
                // A callback from a cycle that has since been abandoned (the
                // call was abandon()ed, but a completion may already have
                // been queued) must not write into the new cycle's slots.
                if (token != cycle_token || stage != STAGE::ACTING_TOOLS || index >= slots.size()) return;
                slots[index].done = true;
                slots[index].ok = result.ok;
                slots[index].text = result.text;
                maybe_finish_tool_round();
            });
            if (id.empty()) {
                slots[index].done = true;
                slots[index].text = "Error: the tool '" + call.name + "' is not connected right now.";
            } else {
                slots[index].server_call_id = id;
            }
        }
    }

    if (!job.calls.empty()) {
        job.id = next_job_id++;
        if (worker.submit(std::move(job))) {
            expected_job_id = next_job_id - 1;
            internal_batch_pending = true;
        } else {
            for (size_t index : internal_slot_indices) { slots[index].done = true; slots[index].text = "Error: could not run the tool right now."; }
            internal_slot_indices.clear();
        }
    }

    maybe_finish_tool_round();
}

void SUBCON_REASONER::maybe_finish_tool_round()
{
    if (stage != STAGE::ACTING_TOOLS || internal_batch_pending) return;
    for (const CALL_SLOT& slot : slots)
        if (!slot.done) return;

    // Every call of this reply has an answer (a result, a tool-reported
    // error, a timeout, or a disconnect error - the server completes every
    // call one way or another, so this can't wait forever). Append them in
    // the order the model asked, then ask again.
    for (const CALL_SLOT& slot : slots) {
        // Each result is cut to what is left of the cycle's budget; once
        // none is left it is replaced outright (the model is still told the
        // call was answered, just not with data).
        std::string content;
        if (tool_bytes_used >= TOOL_CYCLE_BUDGET_BYTES) {
            content = "Error: result omitted: size budget used up.";
        } else {
            size_t room = std::min(TOOL_RESULT_MAX_BYTES, TOOL_CYCLE_BUDGET_BYTES - tool_bytes_used);
            std::string cut = truncate_utf8(slot.text, room);
            content = (slot.ok || slot.plain) ? cut : frame_failure(cut);
        }
        tool_bytes_used += content.size();
        conversation.push_back({{"role", "tool"}, {"tool_name", slot.tool_name}, {"content", content}});

        if (slot.dispatched) {
            ++tools_dispatched;
            if (slot.ok) ++tools_ok;
        }
        log("tool result (" + slot.tool_name + (slot.ok ? "" : ", FAILED") + "): " + short_text(slot.text, 300));
        log("tool result full text (" + slot.tool_name + "): " + slot.text, true);
    }
    slots.clear();
    ++rounds;

    if (tool_bytes_used >= TOOL_CYCLE_BUDGET_BYTES) {
        // No room for more results, so another round could only be answered
        // with "omitted": write up what there is. (The pairing is intact -
        // every call above has its reply.)
        log("tool result size budget (" + std::to_string(TOOL_CYCLE_BUDGET_BYTES) + " bytes) used up - not asking for further calls, writing up what was found");
        finish_act();
        return;
    }

    json tools = build_offered_tools(false);
    if (!submit_chat(tools, nullptr)) {
        end_cycle_without_finding("(the model could not be asked - the tools ran but no write-up was made)");
        return;
    }
    stage = STAGE::ACTING_MODEL;
}

void SUBCON_REASONER::finish_act()
{
    if (!chosen_plan.empty()) log("act: " + chosen_plan);

    // The finding is only asked for if a real tool was actually used.
    // Without one the model can only be describing what it imagines it would
    // have found (the fabrication the 2026-09-26 work kept running into), so
    // there is nothing true to report - the attempt is recorded as such and
    // the cycle ends.
    if (tools_dispatched == 0) {
        log("act turn made no real tool call - nothing was actually checked, recording the attempt and moving on");
        end_cycle_without_finding("(no tool was used - nothing was actually checked)");
        return;
    }
    // New with the explicit loop: if every call that ran failed there is
    // nothing true to ask a finding about either. (The old flow left this to
    // the finding prompt's "if your tools failed, say so" instruction, which
    // is still there for the mixed case.)
    if (tools_ok == 0) {
        log("every tool call failed - nothing trustworthy to report, recording the attempt and moving on");
        end_cycle_without_finding("(every tool call failed - nothing was reported)");
        return;
    }

    conversation.push_back({{"role", "user"}, {"content", build_finding_prompt(items[chosen_index], unix_now())}});
    if (!submit_chat(nullptr, FINDING_FORMAT)) {
        end_cycle_without_finding("(the model could not be asked for a finding)");
        return;
    }
    stage = STAGE::FINDING;
}

void SUBCON_REASONER::record_attempt_result(const std::string& finding, bool noteworthy)
{
    SUBCON_TODO_ITEM& task = items[chosen_index];
    SUBCON_TODO_RESULT result;
    result.reasoning = chosen_reasoning;
    result.plan = chosen_plan;
    result.finding = finding;
    result.at_unix = unix_now();
    result.noteworthy = noteworthy;
    subcon_append_history(task, result);
    save_items();
}

void SUBCON_REASONER::end_cycle_without_finding(const std::string& history_text)
{
    record_attempt_result(history_text, false);
    ++cycle_token;
    stage = STAGE::IDLE;
    schedule_next_cycle(config.rethink_seconds);
}

void SUBCON_REASONER::handle_finding(const OLLAMA_REPLY& reply)
{
    SUBCON_TODO_ITEM& task = items[chosen_index];

    if (!reply.ok || reply.content.empty()) {
        log("finding call failed or produced nothing - abandoning this cycle");
    } else {
        try {
            json parsed = json::parse(reply.content);
            bool noteworthy = parsed.at("noteworthy").get<bool>();
            std::string urgency = parsed.at("urgency").get<std::string>();
            std::string summary = parsed.at("summary").get<std::string>();

            // A tool outside the read-only set means whatever the model
            // "found" came from a side effect, not a look-up - never queued,
            // whatever it says about itself.
            bool queue_it = noteworthy && !summary.empty() && !cycle_unsafe;
            log(std::string(queue_it ? "finding (worth telling): " : "finding (not queued): ") + summary);

            int64_t now_unix = unix_now();
            if (queue_it) {
                SUBCON_NOTE note;
                note.content = summary;
                note.label = task.label;
                note.urgency = (urgency == "urgent" || urgency == "low") ? urgency : "normal";
                note.created_unix = now_unix;
                queue.push_back(note);
                save_queue();
                last_finding_text = task.label + ": " + summary;
            }

            // Staleness bookkeeping: only the chosen item needs touching -
            // its last_checked moves to now and history keeps what happened.
            task.last_checked_unix_seconds = now_unix;
            task.ever_checked = true;
            record_attempt_result(summary, queue_it);
        } catch (const std::exception& e) {
            log(std::string("finding reply wasn't valid structured JSON: ") + e.what());
        }
    }

    ++cycle_token;
    stage = STAGE::IDLE;
    // Whether this cycle succeeded or failed, ask again reasonably soon -
    // the model decides next time whether anything is worth acting on again
    // or proposes a longer wait; this is only the gap until that next ask.
    schedule_next_cycle(config.rethink_seconds);
}
