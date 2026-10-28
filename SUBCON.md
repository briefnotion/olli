# Subcon - Olli's background "subconscious"

Subcon is a background thread with its own, separate LLM conversation. While
you aren't talking to Olli it works through a short list of recurring tasks
(check the weather, review past conversations, ...), decides whether anything it
found is actually worth telling you, and hands the worthwhile bits to the real
chat at a sensible moment. It never takes part in your conversation, and your
conversation never sees its working.

Code: `source/subcon_worker.cpp` / `.h` (the thread and its state machine) and
`source/subcon_tasks.h` (the task list and every time-based rule, kept free of
threads and the clock so it can be unit tested). Design history and the
original brainstorm: `IDEAS.md` ("The subconscious").

## What it does, in order

One **cycle** is:

1. **Prioritize.** Subcon is shown the tasks that are *allowed to run right now*
   (see "Limits" below), the time of day, and whether you're home, and decides
   either "run one of these now" or "nothing is worth doing yet, ask me again in
   N minutes" (clamped to 1-60). If no task is allowed at all, the model isn't
   asked: the thread just waits until the soonest one opens up.
2. **Act.** The chosen task is run for real: subcon calls its tools (web search,
   `rag_search`, `check_presence`, `list_hue_lights`, `list_timers`, ...) and
   waits for every call to come back. If it didn't actually call a tool, or a call
   never got an answer, **there is nothing true to report** - the attempt is
   recorded and the cycle ends. (Without this the model happily describes what
   it imagines it would have found.)
3. **Finding.** Subcon reports `{noteworthy, urgency, summary}`. Only
   `noteworthy: true` ever reaches the queue. "Nothing new", "all clear" and
   "unchanged since last time" are recorded in the task's history and go no
   further. It is also told what you've already been told, so the same thing
   isn't repeated. (Before this existed, every cycle queued - and announced - a
   note, including "no unresolved issues detected".)
4. **Deliver**, separately, whenever notes are waiting (see "Delivery").

Every cycle and every delivery decision starts from a **fresh conversation** (only
the persona message is kept), so real tool results don't use up the model's
small context window.

## The tasks

Defined in code, `subcon_task_defs()` in `source/subcon_tasks.h`:

| Task | What it checks | Max per period | Min gap |
|---|---|---|---|
| Weather | `web_search` for Lafayette, Louisiana; noteworthy only for alerts, storms, flooding, extreme heat/cold, or a big change | 3 | 4 h |
| Past conversations | `rag_search` over the conversations collection for unanswered questions / unkept promises | 3 | 4 h |
| Notes | `rag_search` over Notes for deadlines and appointments coming up | 2 | 6 h |
| Lights | `check_presence`, and only if you're away, `list_hue_lights` | 4 | 2 h |
| Timers | `list_timers` for finished timers nobody acknowledged | 2 | 6 h |

**To add, reword or remove a task:** edit `subcon_task_defs()` and rebuild. On
the next start the saved list is merged with the definitions by `key`: a task that
still exists keeps its history and run record but takes the new wording and limits;
a task that no longer exists is moved to `todo_items_retired.json` (with its history)
instead of being deleted. A new `key` starts fresh. Nothing needs hand-editing.

## Limits (how often a task may run)

Each task has a hard cap, enforced in code - never left to the model:

- **A period is 24 hours, or since you last got home - whichever is more recent.**
  "Got home" means you were away for at least 10 minutes first (a brief presence
  flap doesn't count). Arriving home therefore gives every task a fresh allowance
  *and* pulls the next think-cycle forward to ~30 seconds.
- **Minimum gap** between runs of the same task, regardless of the period, so a
  3-a-day task can't spend its budget in the first ten minutes - and so a presence
  flap can't turn a cap into an unlimited loop.
- An attempt counts from the moment it **starts**, even if it later fails.
- Because arriving home resets the allowance, the practical ceiling over a
  whole day with several trips out is "the minimum gap", not the cap (e.g.
  weather: at most one per 4 hours, so up to 6 in a day if you come and go a lot).
  The cap is a strict per-24-hour limit only on days you stay home.
- The arrival time is not saved: after a restart the period falls back to plain 24
  hours. (Run times *are* saved, so a cap is never lost.)

## Delivery - when you get told

Findings wait in a queue (`queue.json`) until the real chat can say them. Deciding
*when* is the part subcon itself thinks about, bounded by rules it can't override:

**Hard rules (code, `subcon_delivery_gate()`):**
- You must be **home** (`near`), and the system **idle** (not mid-conversation).
- **Quiet hours: nothing is said between 22:00 and 07:00 local**, urgent or not -
  notes just wait until morning.
- An **urgent** note, or any note that has already waited **4 hours**, goes out at
  the next allowed moment *without* asking the model.
- A note not heard within **18 hours** is dropped rather than read out stale.
- The rules are checked again right before anything is announced, because the
  model call in between can take a minute or more.

**Judgment (the model):** otherwise subcon is shown the waiting notes, the time, how
long ago you got home and when it last told you something, and answers either
"deliver now" or "wait N minutes" (5-120). It tends to favour calm moments such as
shortly after you arrive.

Everything waiting is delivered together as one **digest** whose wording is built by
code, not the model, so the facts reach the chat exactly as recorded. The real chat
then narrates it in Olli's own voice (the same path a timer going off uses).

## Files

All in `<profile folder>/subcon/` (for `./olli ron`, `~/olli_files_ron/subcon/`):

| File | Contents |
|---|---|
| `todo_items.json` | each task's state: last checked, recent run times, last 20 results |
| `queue.json` | findings waiting to be told (and recent history of delivered/expired ones) |
| `todo_items_retired.json` | tasks removed from the definitions, with their history (never read back) |
| `*.corrupt` | a file that failed to parse is moved aside, not overwritten |

Writes go to a temp file and are renamed into place, so a crash can't leave a
half-written file (which would otherwise reset every cap).

## Safety

Subcon technically has the whole tool set (a deliberate choice) and is kept
passive by its persona. As a check, every tool it calls is compared with a
read-only list (`is_read_only_tool()`); a call outside it is logged with
`[SAFETY]` and that cycle's finding is thrown away. This is detection, not
prevention - blocking would have to live in the shared tool dispatch.

## Switches and knobs (all in `subcon_worker.cpp` / `subcon_tasks.h`)

| Name | Default | Meaning |
|---|---|---|
| `SUBCON_THINK_CYCLE_ENABLED` | `true` | master switch for starting new cycles. `false` stops the reasoning; already-queued notes are still delivered |
| `SUBCON_FORCE_RUN_FOR_TESTING` | `false` | test only: ignore the model's "nothing worth doing" and run the stalest allowed task |
| `SUBCON_WINDOW_SECONDS` | 24 h | task allowance period |
| `SUBCON_MIN_AWAY_SECONDS` | 10 min | how long you must be away for a return to count |
| `SUBCON_QUIET_HOUR_START` / `_END` | 22 / 7 | no speaking between these local hours |
| `SUBCON_NOTE_MAX_AGE_SECONDS` | 18 h | unheard notes expire |
| `SUBCON_NOTE_MAX_HOLD_SECONDS` | 4 h | after this the model no longer gets a say |
| `SUBCON_WAIT_MINUTES_MIN`/`MAX` | 1 / 60 | bounds on subcon's own "ask me again in" |
| `SUBCON_DELIVERY_WAIT_MINUTES_MIN`/`MAX` | 5 / 120 | same, for delivery |

## Known limitations

- **Presence after a restart.** The presence tool only reports when someone
  *changes* between home and away. After restarting olli, subcon's idea of
  your presence stays `unknown` until the next change, and it fails closed:
  nothing is delivered until then (notes expire after 18 h). Tasks still run.
- **A slow model call delays your own message.** Ollama handles one request at
  a time; if you start talking to Olli while a subcon call is in flight, your
  reply waits for the rest of it (measured: ~22 s against a normal 3-5 s).
  Subcon only *starts* a cycle when the system is idle, so this is the
  window of a minute or two a few times a day, not constant.
- **Announcements are handed to the chat without checking whether the chat is
  mid-reply** (`main.cpp` calls `integrate_tool_result()` directly). Subcon's
  own idle check makes it rare, but it is the same class of race as the
  documented "check `is_processing`" rule.
- **A fetched web page cut in the middle of a multi-byte character** (`tools.cpp`
  truncates to 4000 bytes) can make the next request fail to serialise. Subcon now
  survives and logs it; the underlying truncation is unchanged.
- The model is a small local one and sometimes reasons about recency oddly; it can
  also answer without calling its tool. The "no real tool call -> nothing
  reported" rule is what keeps that from becoming a false finding.
- Shutting down while a tool-result narration is in flight can take as long as that
  model call.

## Testing

- `subcon_tasks.h` has no dependencies beyond `nlohmann/json`, so its rules
  (limits, windows, arrival detection, quiet hours, delivery gate, digest,
  migration of old files) are unit tested with invented timestamps - see the
  tests described in `TODO.md`'s 2026-10-28 subcon entry.
- Live: run `./olli <test profile>` (never the profile you use day to day), set
  `SUBCON_FORCE_RUN_FOR_TESTING` to `true` for a build, and watch
  `debug_full_history.txt` for lines tagged `subcon`. `=== subcon / tool ===`
  entries are real tool results; the `Content:` lines of `subcon / EVENT` entries
  tell the story (`chose:`, `act:`, `finding (...)`, `announcing:`).
- Remember to build with the test flags back to `false` before using a profile
  for real.
