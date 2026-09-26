# Future ideas

Not started, not designed in code-level detail, not committed to. A place
to keep real brainstorming from getting lost between sessions - see
`TODO.md` for actual bugs/fixes and `README.md`/`TOOLS.md`/`AUTOMATIONS.md`
for what's actually built. Captured 2026-09-23, after a long "what should
olli become" conversation once the night's bug-fixing was done.

## The subconscious - the big one

**The idea**: a second, genuinely independent background process - not
just another `sidetrack.cpp` routine - whose whole job is to notice
things while you're not around, decide what's worth remembering, and
bring it up naturally once you're back. The seed scenario that led here:
walking in the door, olli already has the lights on, and instead of
dumping everything at once, offers the most important thing first ("you
got an email about a job offer"), lets you defer things ("I'll look at it
later"), and only goes further if you ask ("anything else?").

### Why not build it as another sidetrack routine

Sidetrack's existing routines (`run_second_guess`, `run_consolidation`,
`run_clear_context`) are all state machines that only get to advance when
the *main* thread's own loop ticks and calls `SIDETRACK_CLASS::check()`.
That means they inherit every stall the main thread has - and tonight's
own bug-fixing found the main thread genuinely does stall, more than
once: `TOOL_TASK_RUNNER::handle_tool()` blocks it synchronously for
however long a whole task-runner script takes (used to be up to 5
minutes on a stuck `[WAIT_FOR_RESULT]`, still up to 30s worst case even
after tonight's fix); `integrate_tool_result()`'s own `send()` call
blocks it for the length of a whole model reply. A "subconscious" riding
on sidetrack's own tick would go quiet exactly during the stretches it's
most likely needed to be thinking.

**Proposed shape instead**: a real, dedicated worker thread, modeled on
`IO_WORKER_CLASS`/`TOOL_WORKER_CLASS` (`thread_start()`/`thread_main()`
on its own thread, started once at program startup) - but unlike those
two, which are pure I/O shuttles with no reasoning of their own, this one
would own and drive its own `ollama_system` instance, thinking
independently of any user turn, on its own schedule, regardless of what
the main thread is doing. Would need a real name better than "the
subconscious worker" - kept as a placeholder.

### Deliberately passive toolset - a safety principle, not an afterthought

The subconscious should get its *own* tool set, structurally incapable of
taking real-world action - can read email, can't send/delete; can listen
in on STT, can't act on what it hears; can check a calendar, can't create
events. This isn't a style preference - it's the preventive version of a
real bug found and fixed tonight (see `TODO.md`'s 2026-09-23 second-guess
entry): an autonomous background process with real write-capable tools
silently took a real action (turned off physical lights) contradicting
what the user had just decided. That got fixed reactively, after the
fact, with prompt narrowing and forced visibility. For the subconscious,
the same protection can be structural instead: if "turn something off" is
never a tool it's handed, no prompt drift can ever talk it into doing so.
Hold this as a hard rule specifically for this thing.

### The awareness/event layer

Not really a new plumbing concept - more "tools" that emit `TOOL_EVENT`s,
same as clock's timers already do (and same as tonight's `origin_id`
"birth certificate" mechanism was built for - an event with no direct
question behind it, just something worth knowing). What's actually new is
the *sources*: audio classification beyond raw transcription (a sound at
the door vs. the neighbor's hedge trimmer vs. actual conversation - STT/
Voca already exists, this is triage on top of it, not a new pipeline),
power-state monitoring (loss/restore), mail arrival, room-level presence
(finer-grained than today's home/away). Each is roughly "one more
read-only remote tool," same shape as everything already built - not a
new kind of problem individually.

### The digest queue

Where the subconscious's own conclusions live until surfaced - not a
flat list, needs at least: unacknowledged, deferred ("I'll look at it
later" - don't re-mention now, don't lose it either), and maybe a
"resurface eventually" state for things like a job-offer email that
deserve a gentle nudge if never revisited. Surfaced pull-style once
presence signals you're back - one thing at a time, ranked by actual
importance, more only if asked ("anything else?") - not a wall of text
the moment you walk in.

### "When to announce" as just another reasoning task, not a separate mechanism

Idea from 2026-09-26, once the prioritize/plan reasoning loop was actually
built and live-verified (see `TODO.md`'s own entry): instead of a rigid
external gate deciding when it's okay to surface what's in the digest
queue, make "is now a good moment to bring up the tell-later list" one
more candidate item in the same prioritize/plan loop subcon already uses
for everything else - reasoned about the same way, using whatever context
it has (how many things are queued, how important they are, presence,
time of day), not a hand-coded rule.

One deliberate refinement on top, though: this should layer on top of the
existing hard mechanical busy-gate (`main_busy_level < 10`), not replace
it. The busy-gate stays a non-negotiable floor governing whether subcon
even runs a think-cycle at all; "is this a good moment to speak up"
becomes something it reasons about *within* that already-safe window, not
instead of having one. Same reasoning as the second-guess safety fix
(`TODO.md`'s 2026-09-23 entry) - leaving a real-world-facing decision
entirely to the model's own judgment, with no structural backstop, is a
failure mode this project has already hit once.

### The hard parts (there are two, and they're different kinds of hard)

1. **Curation** - deciding what's worth remembering and how urgently, at
   all layers (is this email worth noting at all? is this sound worth
   investigating? is a birthday worth a proactive mention or only if
   asked?). Fetching data is easy across every source above; judging its
   importance is hard everywhere, and it's a prompting/triage problem,
   not an architecture one.
2. **Initiative** ("gets bored, tries to figure out other things to
   do") - everything else above is *reactive*, triggered by something
   arriving. Self-initiated checking - going to look something up with
   nothing external prompting it - is a different problem. Probably not
   "let it decide when it's bored" so much as something more mechanical
   underneath: each checkable thing (birthdays, show times, email) gets
   its own refresh cadence, and "boredom" is really just walking the list
   of things overdue for a check, dressed up in character - same idle-
   timer instinct `run_consolidation()` already uses, just per-source
   instead of for the whole conversation.

### What tonight's work already supports, for free

- **Sharing `tool_worker`**: a permanent background instance is just one
  more thing with its own `outstanding_tool_call_ids`/
  `owned_tool_call_ids` (the 2026-09-22/23 correlation fix) - the
  infrastructure doesn't care whether an instance is transient (a script)
  or lives for the whole program.
- **Reading the real chat's history** (so it doesn't re-queue the same
  birthday five times) needs the same `history_mutex` discipline already
  established everywhere else that touches `main_instance.history`.
- **Its own notes/queue state** is genuinely new and would deserve its
  own small, explicitly-justified lock if it needs one - same spirit as
  `tool_worker`'s own mutex exception (raised openly, not slipped in),
  not a default to reach for casually.

### The one real practical risk

A second, continuous stream of real model inference, sharing the same
Ollama server (and, on this machine, the same single GPU) as the live
conversation. Left unpaced, it competes with the real conversation for
GPU time exactly when it's least wanted. Needs to think only when things
are genuinely idle, not continuously - a pacing problem to solve
deliberately, not an afterthought.

### A useful parallel, not a design requirement

Neither an Ollama-hosted model nor Claude has any native "idle
cognition" - both are purely reactive, no standing thread computing
between invocations, regardless of model generation. What this
conversation's own environment (Claude Code) does to *fake* continuity -
background subagents that keep running after being handed a task, memory
files persisting facts across sessions, scheduled wakeups re-invoking
with accumulated context - is architecturally the same trick as what's
being proposed here: a separate process, its own reasoning loop, writes
conclusions somewhere durable, gets read back in later. The scaffolding
is the whole idea; a "smarter model" alone wouldn't get you this on its
own.

**Explicitly not started** - park until current testing/bug-fixing work
is done, then design in real detail before writing any code (matching
this project's own "full design agreement before coding a new feature"
practice).

## Smaller ideas from the same conversation

New remote tools that'd fit the existing pattern:
- A **weather tool** as its own remote tool instead of routing through
  `web_search` - faster, more reliable, no scraping fragility (round-4
  testing hit a real case of `web_search` returning LA weather for an NYC
  query).
- A **lightweight notes/todo tool**, distinct from RAG - RAG's for
  searching longer content later; this would be simpler, faster,
  "add milk to the list" / "what's on my list," no embedding pipeline.
- **Recurring reminders**, not just clock's one-shot timers - "remind me
  every Monday to take the trash out."
- **System/network health** - CPU temp, disk space, "is the server still
  up" - useful given how much is already running on this machine.
- **Media control**, if there's something like mpd worth hooking into.

Quality-of-life on what's already built:
- Browse `.task` scripts and RAG collections **from inside olli itself**
  - "what automations do I have" - instead of needing to go look at the
    filesystem.
- A **dry-run mode for task-runner scripts** - step through a script's
  commands without dispatching real tool calls. Would have made tonight's
  debugging noticeably faster.
- Generalize the **"always visible, never silent" rule from tonight's
  second-guess fix** into a house style for anything autonomous, not a
  one-off patch just for that routine.

Architecture items already identified, just not done:
- **Finish Phase 2 properly** (see `TODO.md`'s 2026-09-22 entry) - the
  event-ownership plumbing (`origin_id`) is already built and verified
  correct, just disabled pending a real debugger session to close the
  `pending_tool_calls`/`last_received` race it exposed.
- **Per-profile remote-tool ports** - the real fix to the multi-instance
  port collision tonight's session only mitigated (a loud notice instead
  of silence). Would also make testing noticeably less annoying - no more
  a test profile and a real profile fighting over one port.

More speculative:
- A **"catch me up"** feature - if you've been away a while, olli
  summarizes what happened instead of you scrolling back through
  transcripts.
- Tying **presence** into more automations now that it exists as a real
  tool - `welcome_home`/`leaving_home` are already a foundation.

## Conditional branching in `.task` scripts (2026-09-24)

`[IF <question>]` / `[ELSE]` / `[MAYBE]` / `[ENDIF]` - a real branch
structure for task-runner scripts, sent to the LLM as a question and
routed on the answer instead of always running the same fixed sequence.

A genuine three-way branch, not just true/false - `MAYBE` is its own
branch, not an error/fallback path, for whenever the honest answer to
the condition isn't a clean yes or no. Maps directly onto the structured-
output mechanism added the same night (`sidetrack.cpp`'s DONE-check
fix, `TODO.md`) - an `enum` field (`"yes"`/`"no"`/`"maybe"`) instead of a
plain boolean, so the model reports genuine uncertainty as a real answer
rather than being forced to pick a side.

Not designed further yet - parked until ready to work through it
properly (parser changes to `TOOL_TASK_RUNNER::handle_tool()`/
`tools_task_script.cpp`, how nesting works, what happens to a call
already in flight when a branch is skipped, etc.).
