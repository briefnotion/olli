# TODO

Guiding principle for all of the below: keep code simple, easy to read, and
well compartmentalized. That's the bar for new work here, not just a nice-to-have.
Before writing a new function, check `helper_olli.cpp`/`.h` and
`tools_helper.cpp`/`.h` for an existing one to reuse. When adding a new
function that's frequently used and well-tested, put it in one of those two
files rather than leaving it local to a single caller: `helper_olli` for
general-purpose helpers, `tools_helper` for helpers reused across tools but
not needed elsewhere.

## New tools

- **Text file tool** - let olli create, modify, and delete simple text files
  (e.g. notes), presumably scoped to a dedicated directory under
  `~/olli_files/` rather than anywhere on disk.
- **Tool-help tool** - a way to ask olli what tools it has and what they do,
  loading/displaying that info at runtime. Could be built on top of the text
  file tool above (help text stored as plain files) rather than hardcoded.
- **RAG support** - retrieval-augmented generation over some corpus (notes?
  history? both?). Probably a big task. `nomic-embed-text` is already pulled
  in Ollama, so the embedding side has a natural starting point.
- **Tools rework - mostly done now** (started 2026-08-21). Pulled every
  `TOOL_*` class out of `olla.h`/`olla.cpp` into their own `tools.h`/
  `tools.cpp`; gave every tool the same `configure`/`register_tool`/`check`/
  `monitor_tool` shape via an abstract `TOOL_BASE`. `ollama_system` reaching
  `CLASS_SYSTEM` was resolved 2026-08-24 (a nullable `CLASS_SYSTEM*` threaded
  through `process()` -> `handle_instance_tools()` -> `dispatch_tool_call()`
  -> each tool's `check()`/`monitor_tool()` - null for sidetrack's background
  thread and task-runner automation instances, which have no business
  touching the real one).
  - **Done 2026-08-27: `tools_list` moved off `ollama_system` entirely.**
    Used to be a member, populated once in the constructor. Now every
    `open()`/`send()`/`process()`/`dispatch_tool_call()`/
    `handle_instance_tools()`/`integrate_tool_result()` takes it as a
    reference parameter instead - cascaded into `TOOL_BASE::check()`/
    `monitor_tool()` and every concrete override (`tools.cpp`,
    `remote_tools.cpp`), since several `handle_tool()`s call
    `chat.send()`/`chat.integrate_tool_result()` internally. Each isolated
    instance (main chat, `SIDETRACK_CLASS`'s own member, each task-runner
    automation instance, `consolidate()`'s local client) owns its own real
    `tools_list`, populated via the new free function
    `populate_default_tools()` (olla.h/.cpp) - no more nullable-tools_list
    problem the way a `CLASS_SYSTEM`-owned version would have had (see the
    design discussion this came from if reviving that idea - the real
    blocker was sidetrack/task-runner needing real tools but no safe
    `CLASS_SYSTEM`, not a technical one). Remote tools still only ever
    register onto main chat's `tools_list` (`main.cpp`) - task-runner
    automation instances can see them too now (2026-09-02, see the
    task-runner rewrite entry below); sidetrack still can't, deliberately
    (see its own entry's data-race/single-connection concern).
  - **Done 2026-08-27: `TOOL_PERMISSIONS_CLASS` dropped entirely**, not
    redone - confirmed genuinely dead weight first: every flag was already
    hardcoded `true` (`main.cpp`), and remote tools (the majority of what's
    actually used) never checked it at all. Removed the class
    (`tools_helper.h`), the member on `ollama_system`/`TASK_SIMPLE`, and
    every gate check in `tools.cpp`.
  - **Done 2026-09-02: the task-runner display bug below.**
  - General polish pass over the existing tool set (Hue lights, timers, web
    search, task runner) beyond the structural rework itself.

## Session & model behavior

- **Repeat-bug root cause found and fixed 2026-08-27, awaiting real-world
  confirmation** - full detail in the `olli-time-repeat-bug` memory entry
  (not duplicated here), short version: `SIDETRACK_CHAT_INSTANCE` was
  silently inheriting `LOAD_SAVE_HISTORY_ON_DISK=true` and the real profile
  directory from `chat.PROPS` (`SIDETRACK_CLASS::create()`), and unlike
  every other secondary `ollama_system` instance, never went through the
  `open(Properties)` overload that forces that off. Every completed
  second-guess review tick was auto-saving `SIDETRACK_CHAT_INSTANCE`'s own
  private, in-progress scratch history straight over the shared
  `history.json`, bypassing the actual fold-or-discard decision entirely -
  confirmed directly (a fold correctly skipped in memory still showed up
  duplicated on disk). Fixed by forcing `LOAD_SAVE_HISTORY_ON_DISK=false` on
  that instance (`sidetrack.cpp`). The word-overlap fold guard from the
  2026-08-26 entry (see memory) was also re-implemented - it was never
  actually committed to git, had to be rewritten from scratch - and now
  works correctly since it isn't racing the disk-write bug anymore. Tested
  clean across 3 repro attempts of the original catchphrase-lock pattern.
  `repeat_penalty` (below) is unchanged/still in place, no isolated evidence
  either way on its own contribution. **Not yet confirmed against the
  original real-world symptom** - several past fixes measurably worked for
  what they targeted without ending the actual complaint, so treat this as
  strong-but-unconfirmed until it survives normal use.
- **`repeat_penalty` added 2026-08-26, still in place, still not isolated-
  tested** - `OLLAMA_SYSTEM_PROPERTIES::repeat_penalty` (olla.h, default
  `1.3` vs. Ollama's own bare default of ~1.1 when the option is omitted)
  is sent as `options.repeat_penalty` on every request
  (`ollama_system::send()`, olla.cpp). Added to test a real finding: a raw
  wire-level capture showed the model's *primary* response locking onto a
  short phrase and repeating it verbatim across several unrelated follow-up
  turns, independent of sidetrack's review. Untouched during the 2026-08-27
  fix above, so no data yet on whether it's pulling its own weight
  independent of the disk-write bug fix.
- **Presence tool: fixed 2026-08-27** - both known issues resolved and
  live-tested. (1) `handle_identity()`'s `last_fired_state` reset (found
  2026-08-26, reverted that session, reapplied now) is now guarded on the
  profile actually changing, not fired on every reconnect. (2) Detection
  logic (`combine_states()`, presence.cpp) changed from AND/AND (both
  backends must agree for either HOME or AWAY) to OR/AND: HOME fires as
  soon as *either* Bluetooth or Wi-Fi independently debounces to HOME
  (quick to notice someone's back), AWAY still requires *both* to agree
  (conservative about declaring the house empty - one flaky backend miss
  shouldn't read as "left"). Confirmed live: Bluetooth alone pushed Combined
  to HOME while Wi-Fi was still debouncing, and a later Bluetooth miss
  didn't flip Combined off HOME since Wi-Fi still had it.
  - **Two more real bugs found and fixed 2026-08-28, live-tested against
    the user's own real phone/home setup** (root-caused from `ron`'s
    `debug_full_history.txt`, which showed rapid home/away flapping
    followed by a permanently jammed `check_presence` - see the
    `tool_calls_this_turn` entry below for the second half of that
    symptom). (1) **Debounce was symmetric, should only ever have applied
    to the disconnect side** - `BackendTracker::record()` required
    `home_debounce_hits` consecutive hits before flipping a backend to
    HOME, same as AWAY's miss-count. A real detection can't be a false
    positive (nothing responds to an `l2ping`/answers an ARP probe from
    empty air), so debouncing HOME only delayed genuine arrivals and let
    the two backends disagree long enough to flap. Bluetooth already had
    an ad-hoc single-hit-HOME exception (`bt_tracker.record(bt_present, 1,
    ...)`) with a comment explaining why; Wi-Fi didn't. Fixed by making
    HOME unconditional-on-one-hit for *both* backends (removed
    `home_debounce_hits` from `PresenceSettings` entirely - fully dead
    once both backends work the same way) and leaving AWAY's
    `away_debounce_misses` debounce as the only debounce that exists now,
    still per-backend, still required from both backends independently
    (`combine_states()`, unchanged) before AWAY fires overall. (2)
    **Startup identity race, not occasional - guaranteed on every single
    connect.** `last_poll` starts at `time_point{}` (epoch), so the very
    first poll tick always fires before there's been time for even a
    localhost round trip to olli and back for the "identity" reply - that
    first poll ran under the shared/no-profile settings, reported the
    event as "Someone" (not the real profile name, no configured action)
    with whatever real signal it found, then fired a SECOND, duplicate
    event once identity actually arrived and `profile_changed` reset
    `last_fired_state`. Fixed with a new `identity_received` bool (false
    until `handle_identity()` sets it, cleared again in
    `reset_to_default_profile()` on disconnect) gating the transition-fire
    block in `main()`'s poll loop. Confirmed on a real restart: exactly
    one correctly-attributed event now, no "Someone" duplicate.
- **`tool_calls_this_turn` never reset for a background-pushed event, so it
  eventually jammed every future tool call for the rest of the session -
  found and fixed 2026-08-28.** The other half of the presence-flapping
  symptom above: `dispatch_tool_call()` (`tools.cpp`) caps tool calls at
  `PROPS.max_tool_calls_per_turn` (4) per "turn," where a turn's boundary
  is defined as "since the last `role=="user"` `send()`" (`olla.cpp`).
  That definition only accounted for a real user message or a task-runner's
  next scripted command - it didn't account for a pushed remote-tool
  `event` (`TOOL_REMOTE::monitor_tool()`, `remote_tools.cpp` - a presence
  transition, a timer expiring), which is just as much a fresh,
  externally-initiated topic as either of those, but shared the same
  never-reset counter. An idle session accumulates this count across every
  unrelated background event it ever receives, and once four had gone by
  (trivial for a flapping sensor), every subsequent tool call - including
  a completely unrelated, legitimate `check_presence` - started getting
  silently rejected with "Too many tool calls this turn," for the rest of
  the session, with no recovery short of a real user message. Fixed by
  resetting `tool_calls_this_turn = 0` at the top of the `event` branch in
  `TOOL_REMOTE::monitor_tool()`, before `integrate_tool_result()` runs -
  the original per-event runaway-chain guard (the actual bug it was built
  for - see `tool_calls_this_turn`'s own comment in `olla.h`) is untouched,
  since the model chaining tool call after tool call off its own
  DIRECTOR_NOTE reply to one event still runs through `dispatch_tool_call()`
  and still hits the cap within that single event's chain. Confirmed live
  against `ron`'s real airplane-mode test: zero cap errors on either
  transition.
- **`debug_full_history.txt`'s per-line format standardized 2026-08-28** -
  builds on the instance-labeling capability from 2026-08-27 below.
  `debug_log_message()`/`debug_log_instance_event()` (`helper_olli.cpp`)
  now both funnel through one shared `write_record()`, so every entry -
  message or instance created/closed marker, from any instance - gets the
  identical shape: a `=== <instance_label> / <role-or-EVENT> ===` header, a
  millisecond timestamp, a `Content:` line, and a closing rule
  (`------------------------------------`, matching `history_write()`'s
  own existing rule-line convention in `olla.cpp` rather than inventing a
  second style). Fixes a real readability problem the old one-line format
  had: multi-line content (a DIRECTOR_NOTE, or `sidetrack-consolidate`'s
  own summarization prompt, which quotes older `[role]: content` verbatim)
  used to be visually indistinguishable from a real per-message header on
  the line right above or below it - confirmed as a genuine source of
  confusion while reading a real session's log during the presence-bug
  investigation above.
- **Sidetrack rewrite: a first attempt was made and reverted 2026-08-28 -
  nothing kept, but worth knowing before trying again.** Wrote a
  `SIDETRACK_CLASS_V2` skeleton (`sidetrack.h`/`.cpp`, currently commented
  out) dropping the background-thread design entirely, and started moving
  `COMMS` off `ollama_system` as an owned member toward `main.cpp` owning
  the one real instance instead (in the spirit of `tools_list`'s existing
  move to a reference parameter - see the tools rework entry above) -
  including dropping `COMMS::audio` on the theory that audio should only
  ever be touched by `IO_WORKER_CLASS::thread_main()`, not carried on
  `COMMS` at all. Both changes were left half-wired (`ollama_system`'s
  many internal `comms.*` call sites in `olla.cpp` were never updated to
  match, so it didn't compile) and were reverted back to the pre-attempt
  state (`git restore`) rather than pushed further, after finding enough
  else "messed up" mid-edit to want a clean restart. Whether `COMMS`
  should move off `ollama_system` the way `tools_list` did is still an
  open, reasonable-sounding direction - it just wasn't carried through far
  enough this attempt to know if it's right. Next attempt should decide
  that up front rather than discovering it mid-rewrite: does every
  `ollama_system` method that touches `comms` today (`log()`,
  `write_to_tts()`, `input()`, `send()`'s streaming loop,
  `spawn_background_task()`) take it as a threaded-through reference
  parameter (matches `tools_list`'s precedent exactly, but touches a lot of
  signatures), or does `ollama_system` keep a `COMMS*` pointer member set
  once (mirrors how `COMMS::audio` itself already worked, far smaller
  diff)?
- **Sidetrack is planned for a rewrite - context to carry forward.** Decided
  2026-08-27, during the repeat-bug investigation above. Relevant going in:
  - The disk-write bug and fold-guard fix above are real, confirmed, and
    worth keeping regardless of when/how the rewrite happens - don't
    rediscover them from scratch.
  - A new debug-logging capability landed the same day, built specifically
    to make sessions like this legible: `debug_full_history.txt`
    (`debug_log_message()`/`debug_log_instance_event()`, `helper_olli.h`/
    `.cpp`) now tags every line with which `ollama_system` instance
    produced it (`ollama_system::debug_label`, olla.h - "chat",
    "sidetrack-review", "sidetrack-consolidate", "task-runner:<intent
    phrase>") and brackets each instance's lifetime with an instance
    created/closed marker. Previously every instance's output was
    interleaved and indistinguishable in that file. Use this first when
    investigating sidetrack behavior for the rewrite - it should make it
    much easier to see exactly what the review pass sent/received without
    cross-referencing timestamps or guessing from content alone. **Format
    standardized further 2026-08-28** - see its own entry above
    (`write_record()`, `helper_olli.cpp`) for the ruled-header/timestamp/
    `Content:` shape every entry now shares.
  - Explored giving sidetrack's review real access to the main `tools_list`
    (so it could actually execute a tool call it decides is warranted, not
    just comment) - backed out of before implementing anything. Two real
    problems, not just one: sharing the same `std::vector` across sidetrack's
    background thread and the main thread risks a real data race (a
    `push_back` from a new remote-tool registration could reallocate the
    buffer mid-iteration on the other thread); and remote tools like `hue`
    maintain exactly one blocking, non-multiplexed connection - we already
    saw a single thread issuing two overlapping calls produce a corrupted
    "unexpected response from remote tool" mismatch, so two threads doing
    it would only make that worse, not new but more frequent. Whatever the
    rewrite does here, this needs a real answer, not just wiring the
    reference through.
  - The user found a separate, distinct problem in the second-guess routine
    while testing 2026-08-27 (not detailed here - flagged as needing more
    thought before the rewrite, not written up yet). Ask before assuming
    it's the same class of issue as anything above.
  - Jump instances (`jump_input()`'s "I'm home."/"I'm leaving."/etc.
    phrase-triggered Hue-scene block, olla.cpp) were removed entirely
    2026-08-27, along with `TOOL_PERMISSIONS_CLASS` (see the tools rework
    entry above) - neither was ever actually used. `jump_input()` itself
    stays, now handling only "bye"/"quit"/"Goodbye." exit phrases.
- **Pre-load the model at startup** - right now the model only loads on the
  first message; see if Ollama's keep-alive/preload mechanism (an empty
  `/api/chat` request, or similar) can warm it up during olli's own startup
  instead, so the first real message doesn't eat the load time.
- **Actively save chat history through the session** - make sure history is
  reliably persisted as the session goes, not just on the periodic
  size-changed check in `ollama_system::process()`.
- **Investigate concurrent sessions** - what actually happens to Voca and
  Lira/TTS if one olli session is already running and a second one starts?
  We hit real contention this session (a second instance hung waiting on the
  mic) - worth understanding and documenting properly, maybe guarding
  against it explicitly.
- **Unexplained crash, seen twice now** - same shape both times: normal
  usage, then a long idle stretch (hours, in a detached `screen` session),
  then a brief interaction, then the process is just gone - no error
  visible (the `screen` window itself closes when its process exits, and
  neither time left a core dump - `ulimit -c` is 0 by default here). No
  confirmed root cause. Investigated 2026-08-23 from the surviving
  `history.json`/`chat_log.txt` of the second occurrence: found and fixed
  one real latent bug while looking (`curl_global_init()` was never called
  anywhere - `TOOL_WEB_SEARCH`/`TOOL_HUE`, tools.cpp/tools_helper.cpp, both
  call `curl_easy_init()` directly, which makes libcurl do its own lazy
  global init on first use - libcurl's own docs say that path isn't thread-
  safe. Now called once in `main()` before any thread that could touch curl
  spawns), but couldn't confirm it's actually what caused either crash - the
  timing in both cases doesn't obviously line up with two threads' first
  curl call racing. If it happens a third time: enable core dumps first
  (`ulimit -c unlimited` before launching, or a persistent
  `/etc/security/limits.conf` entry) so there's an actual stack trace to
  work from instead of just the conversation log.
  - **Mitigated 2026-08-23**: `main()` (main.cpp) is now a crash supervisor -
    the actual program body moved to `main_process()`, wrapped in a top-level
    try/catch (converts an uncaught C++ exception into a clean logged return
    instead of an uncatchable `std::terminate()`/`SIGABRT`), and `main()`
    `fork()`+`execv()`s it as a child, restarting on any abnormal exit
    (crashed or non-zero return) rather than a clean one. `execv()`
    specifically, not a plain `fork()` or function call - a genuinely fresh
    process image each time, so a crash caused by memory corruption doesn't
    ride along into the "fresh" restart. `RLIMIT_CORE` is raised once in the
    supervisor (inherited across `execv()`) if it was 0, so a crash finally
    leaves a real core file without needing to remember `ulimit -c
    unlimited` by hand. Crash-loop protection: gives up after 3 restarts
    within 30s rather than spinning forever, with a clear message. Doesn't
    fix the underlying crash (still not diagnosed) - just stops it from
    silently ending the session, and a `--crash-restart` marker makes a
    recovery visible in the chat log (`[System] Recovered from a previous
    crash - starting fresh.`) instead of invisible. A `--debug-crash` flag
    (deliberately segfaults 5s into a real run, undocumented in
    `--help`) exists to test this without waiting for a real crash - tested
    the crash-loop-and-give-up path for real already (this sandbox has no
    real TTY, so ncurses' own fatal exit on a bad `$TERM` triggered it
    naturally: 3 clean restarts, each with a genuinely fresh re-init
    - settings/audio/whisper all reloading from scratch - then a correct
    give-up). The terminal (raw mode / ncurses alt-screen) is NOT restored
    by the supervisor if a crash leaves it in a bad state - known,
    deliberately deprioritized (finding the actual bug matters more).
    Confirmed with the user's own real `--debug-crash` run (2026-08-23):
    crashed, restarted, crashed, restarted, gave up correctly on the 3rd.
    Also found: `RLIMIT_CORE` alone isn't enough on this machine - Ubuntu's
    `apport` (the actual `core_pattern` handler, confirmed active and
    otherwise working) silently drops crashes from any binary that isn't
    from an installed package unless `unpackaged=true` is set, so a locally-
    built binary like `olli` produces no `/var/crash/` entry at all by
    default. Rather than depend on that (system-specific, not something
    olli controls), the supervisor now also writes its own persistent
    `crash_log.txt` in the profile's `olli_files_<name>/` directory directly
    - durable regardless of terminal/screen state or OS crash-reporting
    config, which core dumps and `std::cerr` messages both are not.
- **Found and fixed 2026-09-04: `qwen3:8b` silently refusing to call
  `run_automation_task` for an unfamiliar task name.** After adding a new
  `.task` file (`print test`) to a profile's `scripts/` directory, saying
  "run print test" produced a generic in-persona non-answer instead of the
  automation running - looked at first like a matching bug in
  `TOOL_TASK_RUNNER::handle_tool()`, but `debug_full_history.txt` showed
  the model never even emitted a `[tool_calls: run_automation_task(...)]`
  entry for that phrase, while the exact same session's "run system test"
  produced one immediately. Root cause: `register_tool()`'s schema
  described the tool as being for "a home automation macro" and named
  only the two original task phrases as examples - a task named "print
  test" doesn't sound like home automation, so the model decided up front
  not to try the tool at all, rather than attempting it and getting it
  wrong. Fixed alongside the `TASK_PHRASE`→`TASK_NAME` rename above -
  see that entry (under "Display / OUTPUT_CLASS") for the actual code
  change. Separately, also noticed the model's own canned confused
  response ("you lost the signal, ghost. try again. what do you want?")
  had accumulated 6 verbatim repeats in one profile's `history.json`
  across sessions, and appeared to be reinforcing itself (the model
  reproducing its own past non-answer rather than reasoning fresh) - fixed
  by having `main.cpp` delete the profile's `history.json` at every
  startup (`std::filesystem::remove`, right next to the existing
  `DEBUG_LOG_CLASS::instance().reset()` call for `debug_full_history.txt`,
  same "wipe fresh every launch" reasoning). Deliberate tradeoff, not a
  bug: this profile no longer remembers anything across a restart at all
  - acceptable per the user's own call, since a long-running session still
  gets independent persistence through sidetrack's consolidation path.

## Display / OUTPUT_CLASS

- **Done 2026-09-03: per-instance chat colors, driven by COMMS instead of
  hardcoded in the display function.** `comms.h` gained two `int` fields,
  `INPUT_FROM_LLM_COLOR`/`INPUT_FROM_USER_COLOR` - the ncurses attribute
  value (`COLOR_PAIR(n) | A_DIM`-shaped) each buffer should render with,
  same type `NCURSES_TEXT_PANEL::append()`'s own `attr` parameter already
  takes (user_io.h). Defaults match what was already on screen (grey user
  input via `COLOR_PAIR(1) | A_DIM`, `0`/plain for LLM output) - pulling in
  `<ncursesw/curses.h>` for the macros was a deliberate, flagged tradeoff:
  `comms.h` is included very widely and was previously ncurses-free by
  design, and `PAIR_USER_INPUT_GREY`'s own pair index (1) is now duplicated
  between `comms.h` and `user_io.cpp` rather than shared from one place -
  kept simple on purpose, not revisited.
  - `IO_WORKER_CLASS::exchange()` (io_worker.cpp) copies both fields from
    the real `comms` into its own `comms_buffer` each tick, alongside the
    existing text-buffer relay - plain assignment, not append-then-clear,
    since a color is a current setting, not accumulating content.
  - `display_with_ncurses()` (user_io.cpp) reads `comms.INPUT_FROM_USER_
    COLOR`/`INPUT_FROM_LLM_COLOR` at its two render call sites now, instead
    of a local `user_attr` variable and a bare `0`. `PAIR_USER_INPUT_GREY`
    itself still only exists to `init_pair()` index 1 at ncurses startup -
    nothing renders with it directly anymore.
  - **Distinct colors given to background instances**, so their output
    reads as visually separate from the main chat's white/grey: task-runner
    automations (`TOOL_TASK_RUNNER::handle_tool()`) get bright cyan/yellow
    (pair indices 2/3); delegator sub-agents (`TOOL_DELEGATOR::handle_tool()`)
    get bright magenta/green (pair indices 4/5). Both set on the spawned
    instance's own `instance_comms` right after `spawn_background_task()`,
    same duplicated-index tradeoff as pair 1 above - `user_io.cpp` defines
    and `init_pair()`s all of them, `tools.cpp` references the raw index
    numbers directly (both files already reachable via the same include
    chain, no new dependency).
  - **Delegator's `stream_output` flipped back to `true`** (had been set
    `false` earlier the same session specifically to stop a sub-agent's
    answer from appearing twice - see the TOOL_DELEGATOR entry above).
    With distinct colors now in place, the double appearance actually reads
    as intentional: the live magenta/green stream shows the specialist
    working, the final white/grey narration is the polished answer -
    rather than looking like an accidental repeat, which is what it looked
    like when both were the same color.

- **Done 2026-08-27: right-side tools panel.** `display_with_ncurses()`
  (user_io.cpp) now reserves a fixed-width column on the right (`win_tools`,
  full screen height, hidden below ~43 total columns rather than squeezing
  everything else unreadably thin) listing every currently available tool
  name. `IO_WORKER_CLASS::exchange()` (io_worker.cpp) takes the caller's
  `tools_list` now and copies just the names into its own `tool_names`
  member at the same PROCESSING-wait sync point it already uses for
  `staged` - reuses each tool's own `register_tool()` into a throwaway json
  array rather than adding a separate name-only accessor to `TOOL_BASE`, so
  there's one source of truth for what counts as "available."
- **Done 2026-09-02: task runner (`TOOL_TASK_RUNNER::handle_tool`,
  tools.cpp) display bug, root cause fully resolved.** Originally: running
  `run system test` (2026-08-21) surfaced raw keystrokes (including a
  literal `\r\n` on Enter) echoing straight to the terminal every tick
  while ncurses owned the screen (the local `KEYBOARD_INPUT`'s
  `PROPS.RAW_ECHO` never set to `false`), plus the function's own
  `cout <<` debug prints bypassing the buffer-pull pattern entirely. Both
  are gone now - the whole function was rewritten to a state-machine loop
  driving `comms`/`io_worker.exchange()` like everything else, with no
  local `KEYBOARD_INPUT` or raw `cout` left at all. See the task-runner
  rewrite entry under "Session & model behavior" for full detail.
- **Done 2026-09-04: task runner scripting overhaul - disk-loaded `.task`
  files, `[PAUSE]`/`[ASK]`/`[PRINT]`, dynamic tool description.**
  `TASK_SIMPLE_MANAGER::load_all_task()` (`tools_helper.cpp`) no longer
  hardcodes tasks in C++ - it reads every `*.task` file directly under a
  `scripts_dir` path instead, parsing `NAME:`/`PURPOSE:`/`DIRECTORY:`
  header lines up to a literal `---` separator, then treating every
  remaining non-empty line as one `COMMANDS` entry, verbatim (including
  any `[...]` marker prefix). `TASK_SIMPLE::TASK_PHRASE` renamed to
  `TASK_NAME` throughout (`tools_helper.h`/`.cpp`, `tools.cpp`) to match:
  the file's `NAME:` field is meant to hold the task's bare name with no
  leading verb ("system test", not "run system test") - the earlier
  phrase-based matching required the model to also guess whether to keep
  or drop the verb, which turned out to be a real source of match
  failures (see "Session & model behavior" below).
  - **Loading moved out of the constructor.** `TASK_SIMPLE_MANAGER` used
    to load its (then-hardcoded) list from its own constructor. That
    doesn't work for a real directory path: `TOOL_TASK_RUNNER` (which
    owns a `task_manager` member) gets constructed in
    `populate_default_tools()` (`olla.cpp`), called from `main.cpp`
    *before* `main.cpp` ever sets `chat.PROPS.OLLI_DIRECTORY` - a
    constructor has nothing to read from yet. Fixed by dropping the
    constructor entirely, giving `load_all_task()` a `scripts_dir`
    parameter, and calling it from `TOOL_TASK_RUNNER::configure()`
    (`tools.cpp`) right after `OLLI_DIRECTORY` is set - `configure()` is
    already the existing per-tool hook that runs once `OLLI_DIRECTORY` is
    known (`ollama_system::open()`), so no changes to `main.cpp`'s call
    ordering were needed. Any other tool needing something set up between
    construction and `chat.open()` has the same trap - hook into
    `configure()`, don't rely on a constructor.
  - **Hot reload.** `handle_tool()` also calls `task_manager.load_all_task()`
    fresh at the top of every `run_automation_task` call (not just once at
    startup), and `register_tool()` reloads it too before building the
    tool's JSON schema - an edited/added/removed `.task` file takes effect
    on the very next request, no olli restart needed, whether or not that
    request even mentions automation.
  - **`SCRIPT_STATE`'s switch pulled out of `handle_tool()`** into a free
    function `advance_script_state()` (anonymous namespace, `tools.cpp`),
    taking only `state`/`i`/`current_input`/`found_task`/`instance`/
    `instance_comms`/`tools_list` by reference. Deliberately a free
    function, not a `TOOL_TASK_RUNNER` member - it structurally can't
    reach `chat`/`comms`/`tc_id`/`io_worker`/`task_manager`/
    `OLLI_DIRECTORY` even by accident, so the switch can keep growing new
    command types without `handle_tool()` itself bloating or picking up
    coupling it doesn't need. `SCRIPT_STATE` itself moved to the same
    anonymous namespace (was previously declared inside `handle_tool()`)
    so it can appear in the function's signature.
  - **Markers renamed and one added.** `[[ENTER TO CONTINUE]]`/`[[ASK]]`
    (double-bracket, inconsistent single-word-vs-phrase style) became
    `[PAUSE]`/`[ASK]` (single-bracket, both single keywords). New:
    `[PRINT]<text>` - displays `<text>` verbatim (no separator bar, no
    label, so many `[PRINT]` lines in a row read cleanly with nothing
    between them) and immediately advances to the next command - no LLM
    call, no waiting. Structurally different from `[PAUSE]`/`[ASK]`: those
    hand off to a `WAIT_*` state that increments `i` once the wait
    resolves, but `[PRINT]` has nothing to wait for, so it increments `i`
    itself right inside the `GET_COMMAND` branch instead.
  - **Two display bugs fixed along the way.** `[ASK]`'s `REQUEST:` line
    used to include the literal `"[ASK]"` marker text in what got shown
    on screen (`"REQUEST: " + command`, with `command` still carrying the
    unstripped prefix) - fixed with `command.substr(5)`. Separately, the
    `"INPUT: ..."` display line moved from `GET_COMMAND`'s plain-command
    branch to `EXECUTE_COMMAND` (built from `current_input`, not
    `command`) - it now uniformly shows whatever's actually about to be
    sent to the LLM, whether that's a plain script line or a typed
    `[ASK]` answer, where previously a typed `[ASK]` answer was sent with
    no display at all.
  - **`register_tool()`'s schema stopped hardcoding examples.** It used
    to describe `run_automation_task` as being for "a home automation
    macro" and list the two original task names as its only examples -
    directly caused the model to refuse even attempting the tool for a
    new task whose name didn't resemble either example or "home
    automation" (confirmed via `debug_full_history.txt`: no
    `[tool_calls: run_automation_task(...)]` entry at all for the
    refused attempts, vs. a clean one for a phrase matching an example).
    Description is now generic and explicitly tells the model to attempt
    the call even on an uncertain guess, since a miss is safe; separately,
    `intent_phrase`'s own parameter description is now built at
    `register_tool()` time from the live `task_manager.TASK_LIST`, so the
    model sees the real current task names without those names being
    hardcoded anywhere - deliberately *not* done by dumping the full list
    into the always-present top-level tool description too, since that
    cost would scale with however many tasks exist regardless of
    relevance; instead `handle_tool()`'s "no automation found" error
    result also echoes the same live list, so that cost only applies on
    an actual miss.
  - Sample `.task` files (`system test`, `process resume`, `print test`,
    plus an original creative one, `fitter status`) now live under
    `sample_scripts/` in the repo - copy into a profile's own `scripts/`
    directory to use them; `load_all_task()` only ever reads from a
    profile's `scripts/`, never from `sample_scripts/` directly.
- **Filter tool calls and other non-conversational text out of the chat
  log** - `OUTPUT_CLASS::append_to_chat_log()` (user_io.cpp) just logs
  whatever flows through `chat_response`, same as the screen shows. Seen
  in testing: a malformed tool call streamed as plain content instead of
  landing in the structured `tool_calls` field (`response_buffer += c;`,
  olla.cpp ~line 1317, vs. the proper `tool_calls` handling right below
  it at ~line 1321) - showed up as a literal
  `<tools>{"name": "get_current_time", ...}</tools>` line in the log.
  The log should probably strip that kind of artifact even where the
  on-screen display doesn't bother.
- **Done (2026-08-26)**: keyboard input and screen drawing moved off the
  main thread onto their own `IO_WORKER_CLASS` (`io_worker.h`/`.cpp`) -
  see "What we built today: IO_WORKER_CLASS + COMMS" below.
  `AUDIO_CONTROL_CLASS`'s own coupling (it still owns both Voca and TTS,
  still polls `tts.isSpeaking()` to pause/resume Voca) was deliberately
  left as-is per the note this replaces - the worker just became the sole
  poller of `popVocaEvent()` instead of `main.cpp`, not a relocation of
  `voca`/`tts` themselves.

### What we built earlier: the buffer-pull pattern (superseded 2026-08-26)

**Update 2026-08-26**: the three loose members this section describes
(`response_buffer`/`thinking_buffer`/`log_buffer` directly on
`ollama_system`) got bundled into one `COMMS comms` member instead
(`comms.h`/`.cpp`, gained a fourth buffer `tts_buffer` and an `audio`
pointer along the way) - `OUTPUT_CLASS::get_response()` now takes
`COMMS&`, not `ollama_system&`. The *shape* described below (pull, not
push; producer never knows a consumer exists) is unchanged and still the
reason this works - see "What we built today: IO_WORKER_CLASS + COMMS"
below for what actually changed and why.

The problem this solved: `ollama_system::send()` streams a response on its
own background thread (`chat_thread`), but the code that displays things to
the user (`OUTPUT_CLASS`, living in `CLASS_SYSTEM`/`main.cpp`) has no
reliable way to reach into a `send()` call in progress - especially for
instances with no path back to `CLASS_SYSTEM` at all (sidetrack's
`SIDETRACK_CHAT_INSTANCE`, task-runner automation instances). Threading a
pointer/reference into `send()` itself (or further, into every place
`ollama_system` gets constructed) was the alternative, and would have meant
real coupling in a direction that doesn't exist today.

The shape that solved it instead, entirely on the *producer* side:

1. `ollama_system` grew three plain public `std::string` members -
   `response_buffer`, `thinking_buffer`, `log_buffer` (olla.h) - appended to
   right where the old direct `std::cout`/status-print calls used to be,
   guarded by one shared `output_buffer_mutex` (`inline std::mutex`, same
   `inline`-not-`static` reasoning as `history_mutex` - see the comment next
   to it in olla.h).
2. `ollama_system` never needs to know an `OUTPUT_CLASS` exists. It just
   fills its own buffers, blissfully unaware anything is reading them.
3. `OUTPUT_CLASS::get_response(ollama_system& chat)` (user_io.cpp) is the
   *consumer* side: it reaches in, locks `output_buffer_mutex`, copies each
   buffer's contents out and clears the source, every tick. It's a pull, not
   a push - the caller (`main.cpp`) decides *when* and *which* instances to
   pull from (`chat` itself, each of `chat.background_tasks` via
   `ollama_system::pull_background_output()`, sidetrack's
   `SIDETRACK_CHAT_INSTANCE` via `SIDETRACK_CLASS::pull_output()`).
4. `OUTPUT_CLASS::display()`/`display_with_ncurses()` then just prints
   whatever accumulated and clears it - same check-act-clear shape
   `ollama_system::write_to_tts()` already used for `tts_buffer`, which is
   really the same pattern one step earlier (also cross-thread, just never
   generalized past TTS before now).

The reason this beat a pointer/global: nothing upstream (`ollama_system`,
its tool handlers, its background instances) needs to change when a new
consumer wants to look at its output, and a consumer can choose per-tick
whether a given instance's output should even be visible (e.g. main.cpp
currently pulls sidetrack and background tasks but nothing pulls the
delegator's `sub_agent`, which can't be reached this way at all - it's a
local variable, synchronous, gone before any tick could reach it).

### What we built today (2026-08-26): IO_WORKER_CLASS + COMMS

The problem this solved: keyboard reading, voice-event polling, and screen
drawing all lived inline in `main.cpp`'s own loop, interleaved with
chat/model logic (`chat.process()`, `sidetrack.check()`) on the same
thread. `KEYBOARD_INPUT`/`OUTPUT_CLASS` (`user_io.h`/`.cpp`) turned out to
have zero built-in thread-safety of their own once we looked closely - raw
termios manipulation on `STDIN_FILENO`, unlocked plain fields, and ncurses
itself is single-thread-only - so moving them off the main thread meant
picking one thread as their sole, permanent owner rather than sharing
access.

1. **`COMMS` (`comms.h`/`.cpp`)** - bundled `ollama_system`'s four output
   buffers (see the superseded section above) plus a new `audio` pointer
   (replacing a process-wide `g_audio_control` global - each instance's
   `COMMS` points at whichever `AUDIO_CONTROL_CLASS` it should speak
   through, so a future different `COMMS`, e.g. a remote session, doesn't
   have to share the one local speaker) and three input-direction signals:
   `send`/`submitted_line` (a line ready to submit), `stop_requested`
   (abort in-flight generation/speech), `exit_requested` (Ctrl+C).
2. **`IO_WORKER_CLASS` (`io_worker.h`/`.cpp`)** - owns `KEYBOARD_INPUT`/
   `OUTPUT_CLASS` directly as members (moved off `CLASS_SYSTEM`, which no
   longer has them), runs its own background thread. Grew out of a
   generic `WORKER_THREAD_CLASS` skeleton (`templates/worker_thread.h`/
   `.cpp` - copy-and-rename template, not meant to be included directly)
   built earlier the same day as a reusable background-thread-plus-
   main-thread-check-in shape modeled on `SIDETRACK_CLASS`'s own two-
   thread design.
   - `thread_main()` (its own thread): reads the keyboard, polls/merges
     voice events, interrupts `sidetrack`/`audio` directly (it already
     holds references to both), drains `chat`/background-tasks/
     sidetrack's comms into the screen, draws. Stages anything the main
     thread needs to know (a submission, an interrupt, an exit request)
     into a locally-held `COMMS` first.
   - `exchange(COMMS& comms)` (called once per `main.cpp`'s own tick,
     passed `chat.comms`): the *only* thing that crosses the thread
     boundary is `comms` - relays whatever got staged, under a small
     two-flag lock (`INTERUPTED`/`PROCESSING`, adapted from
     `WORKER_THREAD_CLASS`) so a submission never gets read half-written.
   - `main.cpp`'s loop shrank accordingly - `chat.input()`/`jump_input()`
     (olla.cpp) now read `comms.send`/`stop_requested` instead of
     `key_input` directly, since they can't reach `IO_WORKER_CLASS`'s
     members at all anymore.
3. Two correctness bugs found and fixed along the way, worth remembering
   if this area gets touched again: (a) `Keyboard_Input_Enabled`
   (toggled by `TOOL_TASK_RUNNER` automations, threaded through
   `process()`/`handle_instance_tools()`/`dispatch_tool_call()`) was a
   plain `bool&` read/written on two different threads once keyboard
   reading moved to the worker - now `std::atomic<bool>&`. (b)
   `ollama_system::input()`'s submission branch has to clear
   `comms.stop_requested` too, not just `comms.send` - a real Enter
   keypress sets both `INTERRUPTED` and `ENTER_PRESSED` together
   (`KEYBOARD_INPUT::keyboard_input()`, user_io.cpp), and if only `send`
   gets cleared, a stale `stop_requested` from that same keypress can
   fire on a *later* tick once `is_processing` becomes true - aborting
   the response that keypress itself just started.

### What we built today (2026-08-28): AUDIO_CONTROL_CLASS folded into IO_WORKER_CLASS, COMMS renamed, sidetrack disconnected

The trigger: `AUDIO_CONTROL_CLASS`'s own coupling, called out but deliberately
left alone in the 2026-08-26 entry above ("the worker just became the sole
poller of `popVocaEvent()`... not a relocation of `voca`/`tts` themselves").
This time it actually moved.

1. **`AUDIO_CONTROL_CLASS` is gone.** `TextToSpeech`/`Voca` (formerly
   `tts.hpp`/`.cpp`, `voca.hpp`/`.cpp`) are declared and defined directly in
   `io_worker.h`/`.cpp` now, as `IO_WORKER_CLASS`'s own private `tts`/`voca`
   members - nothing outside that one file touches either. Both are
   constructed right before `thread_main()` sets `RUN = true` and torn down
   right after its `while(RUN)` loop exits, not for the object's whole
   lifetime - Voca's whisper-model load/mic-thread startup and TextToSpeech's
   own worker-thread startup all happen on the worker's background thread as
   a result, not blocking whoever calls `thread_start()`. The old
   `AUDIO_CONTROL_CLASS` background thread (polling `tts.isSpeaking()` to
   pause/resume Voca, `adjust_audio_files()`) is gone too - that check now
   just rides `thread_main()`'s existing ~20ms tick instead of its own
   500ms one.
2. **`COMMS` renamed** (`comms.h`): `response_buffer`/`thinking_buffer`/
   `log_buffer` → `INPUT_FROM_LLM`/`INPUT_FROM_THINKING`/`INPUT_FROM_SYSTEM`;
   `send`/`submitted_line`/`stop_requested`/`exit_requested` →
   `ENTER_PRESSED`/`INPUT_FROM_USER`/`INTERRUPTED`/`EXIT_REQUESTED`, matching
   `KEYBOARD_INPUT`'s own field names one-for-one since these are relayed
   straight from it; gained `IS_TYPING` (not yet consumed anywhere). `audio`
   and `tts_buffer` are gone from `COMMS` entirely - no more per-instance
   pointer to a speaker.
3. **`IO_WORKER_CLASS` gained two private `COMMS` copies** - `comms_buffer`
   (renamed from `staged`) and a new `comms_buffer_audio`. `exchange()` fans
   one drain of the real `comms` out into both (`+=`, then a single clear on
   the source) so the screen (`comms_buffer`, drained by `get_response()` on
   the worker's own thread) and TTS (`comms_buffer_audio`) never race each
   other for the same `INPUT_FROM_LLM`/`THINKING`/`SYSTEM` text.
4. **TTS chunking moved out of `ollama_system::write_to_tts()`** (deleted,
   along with the `comms.tts_buffer` it drained and its punctuation/length/
   generation-finished heuristic) **into `IO_WORKER_CLASS::thread_main()`**,
   gated on `tts->isSpeaking()` instead: once idle, flush whatever's
   accumulated in `comms_buffer_audio.INPUT_FROM_LLM` to `speakAsync()` and
   clear it. That field doubles as its own accumulator (nothing else reads
   it), so no separate buffer was needed - a `tts_buffer`/`stt_buffer` pair
   was tried first and undone once this became clear.
5. **`display_with_ncurses()` takes `(input_from_user_echo, comms,
   tool_names)`** now, not `(key_input, tool_names)` - `input_from_user_echo`
   is a new `IO_WORKER_CLASS` member, a live per-tick mirror of
   `key_input.LINE` for showing what's currently being typed; `comms` (passed
   `comms_buffer`) is threaded through but not yet read inside the function.
6. **Voice transcripts write straight into `comms_buffer.INPUT_FROM_USER`/
   `ENTER_PRESSED`** now, instead of staging through `key_input.LINE`/
   `ENTER_PRESSED` first the way typed input still does - STT lives inside
   this worker's own `thread_main()`, so there's no reason to detour through
   `key_input` for it.
7. **Sidetrack disconnected from `IO_WORKER_CLASS` entirely** -
   `thread_start()`/`thread_main()` no longer take a `SIDETRACK_CLASS&` at
   all - and every sidetrack call in `main.cpp` is commented out, ahead of
   the rewrite below. `SIDETRACK_CLASS sidetrack;` itself is still declared
   there (harmless - non-trivial constructor, no unused-variable warning).
8. **Known gaps accepted for now, not fixed**:
   - Background tasks (`spawn_background_task()`) and sidetrack have no TTS
     path at all - `COMMS::audio` is gone, and `comms_buffer_audio` only
     exists inside `IO_WORKER_CLASS`, fed only by the main chat's own
     `exchange()` call.
   - Sidetrack's own generated text no longer reaches the screen (its
     `pull_output()`'s only caller, in `thread_main()`, was removed).
     **Done 2026-09-04 (found already resolved during a later audit)**:
     resolved as a side effect of `run_second_guess()` (sidetrack.cpp,
     2026-08-30) deliberately reusing the main chat's own real `comms`
     instead of an isolated one - its thinking/response text now reaches
     the screen (and TTS) for free via the existing `exchange()`/display
     path, same as the main chat's own output, with no dedicated wiring
     needed. See `run_second_guess()`'s own entry below for the reasoning.
   - `IO_WORKER_CLASS::exchange()` still takes an `ollama_system& chat_ref`
     parameter as a placeholder, purely because `TOOL_BASE::register_tool
     (ollama_system&, json&)` (every `TOOL_*` class, `tools.h`/
     `remote_tools.h`) hard-requires one for building the tools-panel's
     `tool_names` - untangling that is part of the move below.
     **Done 2026-09-04 (found already resolved during a later audit)**:
     `IO_WORKER_CLASS::exchange()` (io_worker.cpp) now takes only
     `(COMMS&, std::vector<std::unique_ptr<TOOL_BASE>>&)` - the
     `ollama_system& chat_ref` placeholder is gone, exactly as the
     COMMS-ownership-move entry below (Done 2026-08-29) describes.
   - Everything above got to a clean `-Wall -Wextra -Werror` build (verified
     2026-08-28) - but only static analysis, not real usage, beyond a quick
     smoke-test session that surfaced and fixed an unrelated pre-existing
     issue (two orphaned `presence` remote-tool client processes, killed;
     see `olli_presence_flapping_fix.md`-style notes if that recurs).

### Sidetrack rewrite + COMMS ownership move

Two-part plan, in order:

1. **Done 2026-08-29: moved `COMMS comms` off `ollama_system` entirely,
   onto `main_process()` (`main.cpp`) instead.** Every `ollama_system`
   method that used to touch `this->comms` internally (`send()`,
   `process()`, `input()`, `jump_input()`, `integrate_tool_result()`,
   `handle_instance_tools()`, `dispatch_tool_call()`) now takes a `COMMS&`
   parameter instead, threaded all the way down through `TOOL_BASE::
   check()`/`monitor_tool()` and every concrete override (`tools.cpp`,
   `remote_tools.cpp`) - same pattern `tools_list` already used. `send()`
   dropped its old `user_input` string parameter entirely; it now reads
   `comms.INPUT_FROM_USER` instead, so every caller sets that field first
   (`comms.INPUT_FROM_USER = prompt; send(tools_list, comms, role);`).
   `background_tasks` changed from `vector<unique_ptr<ollama_system>>` to
   `vector<pair<unique_ptr<ollama_system>, unique_ptr<COMMS>>>` so each
   background task-runner automation instance gets its own real,
   persistent `COMMS` too (not a throwaway) - `unique_ptr<COMMS>`
   specifically, not a bare `COMMS`, both because `COMMS` holds a
   `std::atomic` member (no copy/move constructor, can't live by value in
   a vector) and because `spawn_background_task()` hands out a `COMMS&`
   that a reallocating vector would otherwise dangle. Found and fixed one
   new real race along the way: `ollama_system::input()`'s `chat_thread`
   has to write the submitted text into the shared `comms.INPUT_FROM_USER`
   right before calling `send()` (since `send()` no longer takes it as a
   parameter) - now locked under `output_buffer_mutex` on both sides
   (`olla.cpp` and `IO_WORKER_CLASS::exchange()`'s own relay, io_worker.cpp),
   since a real second writer (`exchange()`, main thread) touches the same
   field. `IO_WORKER_CLASS::exchange()` also dropped its `ollama_system&
   chat_ref` placeholder entirely - `register_tool()`'s `ollama_system&`
   parameter turned out to be unused by every real implementation, so the
   tools-panel's name list now reads each tool's own `TOOL_BASE::
   tool_functions` (populated by `register_tool()` itself) instead of
   re-deriving names by calling `register_tool()` a second time just for
   display.
   - **One deliberately deferred gap: `ollama_system::log()` is currently
     a no-op stub** (`olla.cpp`) - `comms.log()` no longer exists (COMMS::
     log() was removed, replaced everywhere else by a direct
     `comms.INPUT_FROM_SYSTEM +=` append), and threading `COMMS&` through
     every one of `log()`'s many call sites (`tools.cpp`, `remote_tools.cpp`,
     `main.cpp`, `olla.cpp` itself) was set aside rather than done as part
     of this move. Revisit next time system messages are being worked on -
     same pattern as `integrate_tool_result()`: `void log(COMMS& comms,
     const std::string& text) { std::lock_guard<std::mutex> lock
     (output_buffer_mutex); comms.INPUT_FROM_SYSTEM += text; }`, locked
     since `log()` could in principle be called from `chat_thread` (inside
     `send()`) as well as the main thread.
2. **Done 2026-09-04 (found already resolved during a later audit, not
   fixed in response to this entry): sidetrack rewritten from scratch**
   against the real COMMS-ownership shape, rather than retrofitting the
   pre-rework design described in [the background sidetrack thread section
   of README.md](README.md). `sidetrack.h`/`.cpp` are no longer wrapped in
   `#if 0` - `SIDETRACK_CLASS` is a real, working class (no background
   thread; `check()` is called once per tick from `main.cpp`'s main loop,
   non-blocking except for the deliberately-deferred synchronous
   consolidation call noted below), with `run_consolidation()`/
   `run_second_guess()`/`run_clear_context()`/`persistent_time_checks()`
   all implemented - see the two Done entries just below (2026-08-30) for
   the detail on the two biggest pieces. Its screen output and TTS path
   both got re-wired as part of that same work: `run_second_guess()`
   deliberately reuses the main chat's own real `comms` rather than an
   isolated one (see its own entry below), so sidetrack's thinking/response
   text now reaches both the screen and TTS for free via
   `IO_WORKER_CLASS::exchange()`'s normal drain of that shared `comms` -
   no separate `IO_WORKER_CLASS::speak()` wiring needed after all.
3. **Done 2026-08-30: `run_consolidation()` (sidetrack.cpp) implemented and
   working**, tested against a real Ollama server via a standalone harness
   (`../olli_consolidation_test/`, outside the real build - see its own
   `build.sh`/`test_consolidation.cpp`; deliberately avoids linking
   whisper/ncurses/portaudio, only needs curl+pthread, since only 4 unused
   `user_io.cpp` symbols are ever referenced by this code path -
   `stub_ui.cpp` no-ops them). Bucket-by-`consolidation_level`, then per
   level: once it holds more than `keep_count`
   (`PROPS.consolitation_starts_starts_at`, now 30) messages and the
   overflow is at least `trigger_count` (`PROPS.consolitation_sizes`, now
   12), the entire overflow gets replayed onto a dedicated throwaway
   `ollama_system` (`SIDETRACK_CHAT_INSTANCE`, model/host/port only - no
   thinking/streaming/disk-saving/tools) and squashed into one summary
   message via a single `system`-role trigger prompt, promoted to the next
   level up - cascades through multiple levels in one `run_consolidation()`
   call if a promotion pushes the next level over its own threshold too.
   `ollama_system::replace_history()` added (same in-memory-only,
   caller-calls-save_history()-separately pattern as `clear_history()`/
   `clear_history_keep_protected()`) for installing the rebuilt vector back
   into `main_instance.history` at the end.
   - **Real bug found and fixed while testing**: the overflow slice can't
     just be cut at a raw count - level 0's raw messages aren't a fixed
     2-message-per-turn shape once a tool call is involved (`user`,
     `assistant`, `tool`, DIRECTOR_NOTE `system`, `assistant` - 5 messages,
     not 2), so a slice ending mid-exchange leaves a dangling unanswered
     `user`/`tool` message right next to the trigger prompt - confirmed
     this made the model narrate the dangling question instead of
     summarizing anything, or (worse, more silently) confidently "answer"
     just that trailing question while dropping every other topic in the
     batch. An even-count rounding rule was tried first and wasn't enough
     (parity shifts after any odd-length tool exchange) - fixed for real by
     shrinking the slice until it ends on a completed `assistant` turn,
     content-based rather than count-based (level 0 only; levels above 0
     are always self-contained `system`-role summaries with no such
     pairing to worry about).
   - **`tool`/DIRECTOR_NOTE messages get flattened, not filtered, when
     replayed for summarization** - deliberately not dropped from what
     gets consolidated (that would repeat the exact "erasing tool-call
     evidence" bug from 16ab453, just via consolidation exclusion instead
     of early deletion), but a bare `tool`-role message has no meaning to
     the API without the preceding `assistant` tool_calls entry that isn't
     being replayed, so it's relabeled to `user` with a `"[Tool result]:
     "` text marker instead. DIRECTOR_NOTE (`system`-role, matched by its
     literal `"[DIRECTOR_NOTE]"` prefix) is dropped entirely rather than
     also flattened - it's redundant with the `tool` message right before
     it (`integrate_tool_result()` embeds the exact same raw result
     verbatim). Any other `system`-role message at level 0+ is our own
     past promoted summary, not a DIRECTOR_NOTE - replayed unchanged.
   - **Known quality gap, not yet fixed**: when a squashed batch spans
     several unrelated topics, the resulting summary tends to only cover
     the *last* topic in the batch, silently dropping the others, rather
     than covering the whole thing - seen with both a 14-message and a
     19-message multi-topic batch in testing. Graceful, not corrupting
     (only ever affects already-aged-out material, the newest `keep_count`
     messages always stay at full fidelity regardless) but does undermine
     consolidation's actual point if it's this lossy - likely needs a
     stronger trigger prompt (e.g. explicitly asking it to cover every
     topic present, not just summarize) rather than a code change. Revisit
     before relying on long-lived consolidated history for anything that
     matters.
   - **Done 2026-08-30: wired into `check()`** - `run_consolidation()` is
     now actually called every tick, no longer just reachable via the
     test-only `force_consolidation()` hook (sidetrack.h). It still runs
     synchronously, blocking, directly on the main thread whenever it
     triggers - the class comment in sidetrack.h says `check()` is meant to
     stay non-blocking, which this doesn't honor - but deliberately left
     that way for now: idle-gated, fast in practice (~8-11s for a large
     stress-test batch), so not worth the added complexity of threading it
     at this point. Revisit if it ever proves disruptive in real use.
4. **Done 2026-08-30: `run_second_guess()` (sidetrack.cpp) implemented and
   wired into `check()`.** After a real assistant reply lands, waits
   `SECOND_GUESS_WAIT_TIME` (2s), then asks a thinking-mode
   `SIDETRACK_CHAT_INSTANCE` (real `tools_list`/`CLASS_SYSTEM*` passed in,
   so it can actually dispatch a tool, not just talk about one) "More
   needed to be done or said? Respond DONE if not." If not `DONE`, asks a
   follow-up "Go ahead - say or do what needs to happen." and commits
   whatever comes back as a new `assistant` message onto `main_instance.
   history` - marked with `"..."` if interrupted mid-answer rather than
   discarded. Uses `SIDETRACK_CHAT_INSTANCE`'s own `chat_thread`/
   `is_processing` (same async mechanism `ollama_system::input()` already
   uses for the main chat) so `run_second_guess()` can poll tick-by-tick
   instead of blocking; each "waiting" stage calls `handle_instance_tools()`
   every tick (same shape `process()`'s own background-task handling uses)
   so a tool call gets genuinely dispatched and narrated, not just
   requested.
   - **Deliberately reuses the main chat's own `comms`, not an isolated
     one** - initially built with a separate `second_guess_comms` (plus
     matching plumbing through `check()`, `main.cpp`, `io_worker.cpp`) to
     dodge a narrow collision risk on `comms.INPUT_FROM_USER`/`INTERRUPTED`
     between a real user submission and second-guess's own internal
     prompts, but reverted at the user's explicit direction: "comms is the
     backbone of the io that gets to and from the user... the only reason
     to circumvent it is if the user shouldn't be seeing it" - see
     [[olli-collaboration-style]]. Accepted the collision risk; bonus from
     reverting: thinking/response text reaches the screen for free via the
     existing `exchange()`/display path, no extra wiring needed.
   - **`comms.INTERRUPTED` is read-only everywhere in this function, never
     cleared** - it's the real, shared main-chat comms now, and the actual
     owner of clearing it is `ollama_system::input()` (gated on
     `is_processing`, olla.cpp) - clearing it a second time here could race
     with that and swallow a real interrupt to the main chat.
   - **Two independent streaming gates added to make DONE-suppression
     possible**: `OLLAMA_SYSTEM_PROPERTIES::stream_thinking` (new, alongside
     `stream_output`, both default `true`) - `send()` (olla.cpp) now uses
     Ollama's streaming API whenever *either* flag is on, and gates
     `comms.INPUT_FROM_LLM`/`INPUT_FROM_THINKING` independently inside the
     streaming callback, so a call can show its thinking live while
     suppressing its plain-text content (or vice versa) - needed because
     the DONE-check call's literal `"DONE"` answer was otherwise leaking
     onto the screen the same as any real content would. Learned along the
     way: this can only be prevented by never writing it to `comms` in the
     first place (`stream_output = false` for that one call) - clipping it
     out afterward doesn't work, since streaming writes happen live, chunk
     by chunk, during the call itself, well before any post-completion
     check could intervene.
   - **Known latent bug found, not yet confirmed as the cause of anything
     real**: `comms.INTERRUPTED` can get stuck `true` forever in a narrow
     case - `IO_WORKER_CLASS::thread_main()`'s voice-event handling
     (io_worker.cpp) sets `key_input.INTERRUPTED = true` unconditionally
     whenever a non-status voice event is popped, even if
     `voca_event.text` is empty (no transcript, e.g. some wake/listening
     event) - `ENTER_PRESSED` only gets set alongside it when there IS
     text. `comms.INTERRUPTED` only ever gets cleared by
     `ollama_system::input()`, gated on either `is_processing` or
     `ENTER_PRESSED` being true - if neither happens to be true at that
     exact moment, nothing ever clears it again. Always a latent
     possibility, harmless before since nothing else ever read the flag -
     now that `run_second_guess()`'s abort-before-starting check does,
     stuck-true here would permanently block it from ever leaving stage
     0/1. Suspected during one test session ("it's not going into second
     guess at all") but the user found it working again on a recheck, so
     unconfirmed as the actual cause - worth a real fix (e.g. only ever
     set `key_input.INTERRUPTED` alongside a real `ENTER_PRESSED`,
     matching how a real keypress already pairs them) if it recurs.
   - **Confirmed and fixed 2026-09-03.** This was the actual cause of a
     real, reproduced bug: interrupting mid-thinking-block during
     second-guess left `comms.INTERRUPTED` stuck `true` forever (exactly
     the narrow case above - a bare interrupt, no `ENTER_PRESSED`, while
     `chat.is_processing` was false since second-guess only ever runs once
     the main turn has already finished). Fixed in `sidetrack.cpp` at both
     read sites (`poll_second_guess_call()` and the abort-before-starting
     check in `run_second_guess()`) - each now clears `comms.INTERRUPTED`
     itself right after consuming it, safe because `main.cpp`'s loop always
     runs `chat.input()` before `sidetrack.check()` each tick, so main
     chat already had first claim on the flag that same tick if it needed
     it. A second, related bug found in the same investigation:
     `poll_second_guess_call()`'s own "done" check required
     `last_received.complete`, which is permanently `false` after an
     interrupt - stalled stage 3/5 forever on any interrupt mid-call
     (stage 4/6's own "handle an interrupted result gracefully" branches
     existed but were unreachable as a result). Fixed by dropping
     `complete` from that check entirely - `is_processing` false and
     `tool_calls` empty is enough to mean "this call is over," clean or
     not.

### Task-runner rewrite (2026-09-02): state machine, live streaming, real tool access, leak fix

`TOOL_TASK_RUNNER::handle_tool()` (tools.cpp) rewritten from three separate
nested blocking `while` loops (one each for a plain command, `[[ENTER TO
CONTINUE]]`, and `[[ASK]]`, each spinning its own `io_worker.exchange()`
calls) into one flat `while` loop over an explicit `SCRIPT_STATE` enum
(`GET_COMMAND`/`EXECUTE_COMMAND`/`WAIT_RESPONSE`/`WAIT_ENTER`/`WAIT_ASK`/
`DONE`) - `instance.process()` and `io_worker.exchange()` each run exactly
once per tick regardless of state, mirroring the shape of `main.cpp`'s own
loop and `process()`'s existing PART 2 background-task poll. Several real,
previously-latent bugs found and fixed along the way:

- **`IO_WORKER_CLASS&` threaded through the tool-dispatch chain.** Added to
  `TOOL_BASE::check()` (and all four overrides: `TOOL_SET_THINKING_MODE`,
  `TOOL_WEB_SEARCH`, `TOOL_TASK_RUNNER`, `TOOL_REMOTE`), `dispatch_tool_call()`,
  `handle_instance_tools()`, `process()` (olla.h/.cpp, tools.h/.cpp,
  remote_tools.h/.cpp), and `SIDETRACK_CLASS::check()`/`run_second_guess()`/
  the file-local `poll_second_guess_call()` (sidetrack.h/.cpp), since
  sidetrack also calls `handle_instance_tools()` directly. `main.cpp` passes
  the real `io_worker` at both call sites (`chat.process(...)`,
  `sidetrack.check(...)`).
- **`Keyboard_Input_Enabled`/`disable_keyboard` special-case removed from
  `dispatch_tool_call()` entirely** (was: a hardcoded `tc.name ==
  "run_automation_task"` string check toggling a `std::atomic<bool>&`
  threaded through `process()`/`handle_instance_tools()`/
  `dispatch_tool_call()` purely for this one tool - flagged as a stopgap by
  its own TODO comment). No longer needed now that `TOOL_TASK_RUNNER` holds
  `io_worker` directly. Removing it also let `SIDETRACK_CLASS`'s own
  `second_guess_keyboard_enabled` - a deliberate fake placeholder so
  sidetrack's review instance could never touch the real keyboard state via
  the old mechanism - be deleted too, since sidetrack now gets the real
  `io_worker` reference instead (harmless today since nothing yet toggles
  keyboard-enabled state from `handle_tool()` - see the deferred item
  below).
- **Live streaming actually works now - two separate fixes needed.**
  (1) `EXECUTE_COMMAND` used to call `instance.send()` directly/
  synchronously - a blocking HTTP call, so the loop's own
  `io_worker.exchange()` couldn't run *during* a request, only after it
  fully returned. Now spawns `instance.chat_thread` (same pattern
  `ollama_system::input()` and `sidetrack.cpp`'s own
  `start_second_guess_call()` already use), so `WAIT_RESPONSE` can poll
  `!instance.is_processing` while `io_worker.exchange()` keeps flushing
  each tick. (2) Separately, `instance.PROPS.stream_output` was hardcoded
  `false` (present since before this rewrite too) - since that's the
  specific gate on whether streamed chunks reach `comms.INPUT_FROM_LLM` at
  all (`send()`'s streaming callback, olla.cpp), the model's actual answers
  never reached the screen regardless of fix (1) - only a trailing `"\n"`
  did. `stream_thinking`/`use_thinking` staying default-`true` meant
  reasoning text streamed live the whole time, masking this - easy to
  mistake for "streaming already works." Flipped `stream_output` to `true`.
  Confirmed live post-fix (weather-in-New-York answer visibly appeared on
  screen, previously invisible).
- **Automation instance now shares the caller's real `tools_list`, not a
  separately-built one.** Previously built its own via
  `populate_default_tools()` (thinking-mode/web-search/task-runner only) -
  since `TOOL_REMOTE` instances only ever get added to `main.cpp`'s own
  `tools_list` dynamically as devices register, and are `unique_ptr`-owned
  (can't exist in two vectors at once), the automation instance could never
  see or control any real connected device, and `TOOL_REMOTE::monitor_tool()`'s
  keep-alive ping never ran for it either. Confirmed live before the fix:
  "turn off all the lights" got a generic "I can't control smart devices
  directly" answer instead of an actual tool call. `instance.open()`/
  `send()`/`process()` all use the real `tools_list` now - also resolves a
  lifetime concern the old comment was reasoning about (the local
  `automation_tools_list` outliving its own scope via `instance` sitting in
  `chat.background_tasks` after `handle_tool()` returns; the real
  `tools_list` is already the same long-lived reference PART 2's cleanup
  pass uses).
- **`instance_comms` (from `spawn_background_task()`) now actually used** -
  was created but every `send()`/`process()` call used the *main* chat's
  own `comms` instead, so the automation's own I/O and the main chat's own
  were the same buffers. Now isolated: everything inside the loop uses
  `instance_comms`; only the pre-loop status line and the post-loop
  `chat.send_tool_result()`/`integrate_tool_result()` calls (intentionally
  reporting back to the real conversation) still use the real `comms`.
- **`chat.background_tasks` leak fixed.** `WAIT_RESPONSE` resets
  `instance.last_received.complete = false` after every command (including
  the last one, to distinguish a finished command from a still-in-flight
  one) - and nothing ever set it back to `true` afterward, so
  `ollama_system::process()`'s PART 2 (`is_finished = !is_processing &&
  last_received.complete && ...`) could never recognize a finished
  automation instance as done, and its `unique_ptr<ollama_system>`+`COMMS`
  just accumulated in `background_tasks` forever - present in the original
  pre-rewrite code too (same unconditional reset after every command),
  despite an inline comment claiming "the very next completion check below
  erases it." Fixed by explicitly setting `complete = true` once the whole
  script is done, right before returning - also clears
  `last_received.response` at the same time, otherwise PART 2's own
  separate "if the task produced a response, relay it" check would fire a
  second, redundant narration of the same completion
  `integrate_tool_result()` already reports.
- Live-tested end to end against `run system test` (streaming, `[[ASK]]`,
  `[[ENTER TO CONTINUE]]`, real tool dispatch, all confirmed via
  `debug_full_history.txt` timestamps). `run process resume` not yet
  exercised with the new loop.

**Deferred, not fixed:**
- No keyboard-enable/interrupt handling during an automation -
  `io_worker.key_input.PROPS.ENABLED` management was deliberately left out
  for now (manual workaround: don't type until a script step is actually
  asking). Two known consequences: a keystroke typed before `WAIT_ENTER`/
  `WAIT_ASK` sits stale in `instance_comms` and gets misread as the answer
  to whatever prompt is reached next; and `comms.INTERRUPTED` is never
  checked anywhere in the state machine, so there's currently no way to
  abort a running automation once started.
- Whether sidetrack's second-guess review should be suppressed after an
  automation-driven reply - confirmed it currently reviews them like any
  other assistant turn (`run_second_guess()`'s trigger is role-based, not
  source-based, so it can't tell), decided to defer the actual design call
  rather than fix it blind.

### TOOL_DELEGATOR revived (2026-09-03): consult_expert, recursion depth cap, two open issues found in testing

Was a commented-out design sketch (`tools.h`/`.cpp`, see git history for the
pre-revival version) - predates the COMMS refactor entirely, so its old
`register_tool(json&)`/`handle_tool(ollama_system&, ...)` signatures didn't
match anything callable, and it was pulled out of the `TOOL_BASE` hierarchy
as a result. Brought back in line with `TOOL_TASK_RUNNER`'s current shape:
spawns a sub-agent via `chat.spawn_background_task()`, seeds its persona via
`OLLAMA_OPENING` + `open(tools_list, chat.PROPS)` instead of hand-pushing a
system `Message`, drives it with the same `chat_thread`+polling-loop pattern
as everything else, and relays its answer via the modern 4-arg
`integrate_tool_result()`. Registered in `populate_default_tools()`
(`olla.cpp`) - live as `consult_expert` for the model to call like any other
tool. The sub-agent shares the caller's own `tools_list`, deliberately (not
the original's empty one - development had stopped before that mattered) -
it can actually act under its persona's judgment, not just talk about it.

- **`stream_output` ordering bug, fixed.** `open(tools_list, Properties)`
  does `PROPS = Properties` first thing (`olla.cpp`), so anything set on
  `instance.PROPS` *before* that call gets silently overwritten by
  `chat.PROPS`'s own value. Set `stream_output = false` *after* `open()`
  instead - deliberately off, since `integrate_tool_result()` always
  narrates the raw result back to the user afterward anyway, so streaming
  the sub-agent's own generation live just showed the same content twice
  (confirmed live: a poem streamed once raw, then pasted again verbatim
  inside the main persona's own reply - `[DIRECTOR_NOTE]`'s "report...
  without changing the facts/values it contains" wording taken literally
  for creative content). `TOOL_TASK_RUNNER::handle_tool()` sets
  `stream_output` before its own `open()` call too - likely the same silent
  no-op there, not fixed here, out of this task's scope.
- **Recursion depth cap added** (`delegation_depth` member, max 3,
  `tools.h`/`.cpp`). The sub-agent's shared `tools_list` includes
  `TOOL_DELEGATOR` itself, so a specialist can call `consult_expert` on
  itself - a *different* persona chaining in for a sub-problem is
  legitimate (presumably the original intent), but nothing stopped a
  persona from just re-asking itself the identical question with no new
  information. Confirmed live: one "design a mood for movie night" request
  recursed 6 levels deep before finally doing any real work, each hop a
  full blocking network round-trip. Reentrant guard, safe as a plain `int`
  (not atomic) since the whole chain runs synchronously nested on one
  thread - `handle_tool()`'s own wait loop blocks until its sub-agent
  finishes before anything else touches the object.
- **Open, not fixed - empty-response bug.** After a successful
  `set_hue_light` call deep in a recursive chain, `instance.last_received.
  response` came back empty even though real work had just completed,
  propagating "The expert subroutine failed to return a response." back up
  through every nesting level - the lights had actually changed, but the
  user was told they hadn't. Not yet isolated whether this needs deep
  recursion to trigger, or can happen on a single ordinary delegation that
  includes a tool call - the depth cap above stops the recursion but wasn't
  confirmed to fix this specific symptom (the very next test run avoided it
  via a different path: once blocked by the depth cap, the model just
  fabricated a plausible-sounding text answer instead of calling the tool,
  so the empty-response path never got exercised again either way).
- **Open, not fixed - remote-tool heartbeat starves during any tool-result
  narration, badly so during a deep delegation chain.**
  `TOOL_REMOTE::monitor_tool()` (`remote_tools.cpp`) is what pings a
  connected remote tool (e.g. `hue`) to keep it alive
  (`PING_INTERVAL_SECONDS=5`, `DEAD_TIMEOUT_SECONDS=15`, `remote_tools.h`) -
  called once per `process()` tick for every tool, which is fine during an
  ordinary wait loop. The actual gap: `integrate_tool_result()`'s own
  `this->send(tools_list, comms, "system")` (`olla.cpp`) is the one `send()`
  call site in the whole codebase that isn't wrapped in a background
  `std::thread` the way every other one is (`sidetrack.cpp`'s
  `start_second_guess_call()`, `TOOL_TASK_RUNNER`/`TOOL_DELEGATOR`'s own
  initial calls) - it blocks the calling thread for the entire network
  round-trip (observed regularly taking ~15s on its own, right at
  `DEAD_TIMEOUT_SECONDS`), during which nothing else runs, `monitor_tool()`
  included. Not delegator-specific - a single ordinary tool call is already
  marginal - but delegation's nesting stacks several of these blocking
  windows on top of each other, making it far more likely to actually trip.
  Confirmed live: the hue remote-tool connection went down mid-test during
  a long recursive chain.
  - Considered and set aside as too risky for now: threading
    `integrate_tool_result()`'s `send()` call itself, reusing the existing
    `chat_thread`/`is_processing` pair. Would likely "just work" for the
    main chat's own top-level flow (`process()` PART 3, `olla.cpp`, already
    tolerates `chat_thread` finishing on a later tick) but risks a real
    race in every nested wait loop that already polls that same pair for
    its *own* completion (`poll_second_guess_call()`, `TOOL_TASK_RUNNER`,
    `TOOL_DELEGATOR`) - a loop could sample `is_processing` in the brief
    gap between the tool-call-generating `send()` finishing and the
    narration `send()` being launched, and conclude "done" before the
    narration ever ran. Same shape as the empty-response bug above - not
    confirmed to be the same bug, but suspicious.
  - Preferred direction instead, deferred as a real cross-file change
    (touches `tools/PROTOCOL.md` and every remote tool's own heartbeat
    side, not just olli's): before a known-long blocking stretch, send
    connected remote tools an explicit "standing by, don't go away" signal
    instead of relying purely on the regular ping cadence - the remote side
    would need to extend its own dead-timeout expectation on receiving it
    rather than just resetting on real ping/pong traffic.

- **Separate remote-tool bug found and fixed the same session (2026-09-03):
  event/result race in `TOOL_REMOTE::check()` (`remote_tools.cpp`) -
  unrelated to the heartbeat-starvation issue directly above despite living
  in the same class.** `check()` and `monitor_tool()` both read off the
  same per-connection socket - `check()` blocks waiting specifically for
  the `"result"` matching its own call's `call_id`, `monitor_tool()`
  non-blocking-polls each tick for anything else (`"ping"`/`"event"`).
  Previously, if the remote tool pushed an unsolicited event (e.g.
  `tools/clock/clock.cpp` noticing a timer expired) at the exact moment
  `check()` was mid-wait, `check()`'s blocking read consumed that line
  first, saw it wasn't a matching `"result"`, and reported a garbled
  `"Error: unexpected response from remote tool."` - the event itself was
  silently lost rather than ever reaching `monitor_tool()`'s own handling.
  Confirmed live: a `check_timer` call failing this exact way right after a
  `[TIMER EXPIRED]` push landed moments earlier in `run system test`
  testing.
  - **Not a wrong-tool-selection issue** - `is_mine`'s check at the top of
    `check()` already confirms `tc.name` belongs to this specific
    connection before any of this runs, and a genuinely invalid request
    (e.g. a timer label that doesn't exist) comes back as a clean, real
    `"error"` result from the remote tool, handled by a separate branch -
    neither of those reach this code path at all.
  - **Fixed by turning `check()`'s single read into a small loop**, bounded
    by the same overall 5-second budget (shrinking each pass via
    `read_line_blocking()`'s own timeout parameter): a `"ping"` gets
    replied to and waiting continues; an `"event"` is handled inline (same
    narration/`tool_calls_this_turn`-reset/queued-action logic
    `monitor_tool()`'s own event branch already has, duplicated rather than
    extracted into a shared helper - deliberately kept to this one function,
    not touching `monitor_tool()` or the header) and waiting continues;
    anything else (malformed, a pong, a mismatched `call_id`) is logged via
    `DEBUG_LOG_CLASS` and waiting continues; only the real budget running
    out (or a broken write mid-loop) now ends in failure.
  - **Not empirically confirmed against a live collision** - correct by
    construction (any non-matching line is now handled rather than
    misread, before the loop can ever fall through to giving up), but the
    one test run after this landed didn't actually land an event during an
    in-flight call, so it didn't exercise the new code path either way.
    Considered adding a dedicated debug-log line inside the new
    `ping`/`event` branches specifically (so a future real collision would
    leave unambiguous proof it was caught, unlike today where only the
    fallback branch logs anything) - deferred, not done.

## Voice (Voca)

- Wake word is "olli" (`findWakeWord()`/`findSleepTrigger()`, now in
  `io_worker.cpp` - see "What we built today (2026-08-28)" below) - done, no
  longer "voca".

## Remote access

- **Expose an API** so a program on another system can talk to olli's
  interface, not just local keyboard/voice/TTS.

## Open questions / carried over

- Consolidation summaries can cement a bad pattern as an established "fact"
  about the assistant (seen firsthand in a "locked door" persona-drift loop
  from a poisoned `history.json` this session). No fix decided - revisit if
  it recurs. The 30-minute idle auto-clear (`SIDETRACK_CLASS` ROUTINE 3 in
  `sidetrack.cpp`/`.h`) is now in place as a plausible mitigation - a stale,
  poisoned context can't outlive 30 minutes of silence - but isn't confirmed
  to actually fix the underlying issue. `OLLAMA_OPENING` (olla.h) also
  dropped the "snarky" persona for a cyberpunk-lingo one (2026-08-21) since
  snark was suspected to correlate with getting stuck in this kind of
  recursive-response loop as context grew large - also unconfirmed, revisit
  together if it recurs. A concrete instance of the same shape caught
  2026-08-23 in a real `history.json`: sidetrack's second-guess review
  (`SECOND_GUESS_PROCESSING_STAGE`, `sidetrack.cpp`) kept re-raising the
  exact same stale point ("only group 0 was confirmed" after a Hue command)
  after four consecutive unrelated turns - it's shown the whole
  conversation but only told to review "the turn that just ended," with
  nothing marking where that boundary is or telling it not to repeat a
  point it already made in an earlier note. Added an explicit
  don't-repeat-yourself instruction to that prompt as a mitigation - not a
  structural fix (the model still has to notice its own prior notes and
  self-censor), so revisit if it still recurs.
- Same shape of problem as the item above, but from plain persisted history,
  not a consolidation summary - seen concretely while developing the
  remote-tools feature (2026-08-22): early testing recorded "remote tool
  call round-trip not implemented yet" in raw history while `TOOL_REMOTE`'s
  round trip genuinely didn't work yet (Step 3). After it actually got
  implemented (Step 4) and worked when tested standalone, the model still
  insisted the clock didn't work in a session that had loaded the old
  `history.json` - only went away after deleting it and restarting fresh.
  A tool's own past failure getting cemented as a permanent fact is the same
  underlying issue as the consolidation-drift case above, just without
  consolidation involved - whatever fix eventually gets decided there should
  probably account for plain history too, not just summaries.
