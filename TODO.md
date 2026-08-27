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
    register onto main chat's `tools_list` (`main.cpp`) - sidetrack/task-
    runner still can't see them, deliberately, for now (see the sidetrack
    rewrite entry below).
  - **Done 2026-08-27: `TOOL_PERMISSIONS_CLASS` dropped entirely**, not
    redone - confirmed genuinely dead weight first: every flag was already
    hardcoded `true` (`main.cpp`), and remote tools (the majority of what's
    actually used) never checked it at all. Removed the class
    (`tools_helper.h`), the member on `ollama_system`/`TASK_SIMPLE`, and
    every gate check in `tools.cpp`.
  - The task-runner display bug above.
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
    phrase>") and brackets each instance's lifetime with `=== instance
    created/closed: <label> ===` markers. Previously every instance's
    output was interleaved and indistinguishable in that file. Use this
    first when investigating sidetrack behavior for the rewrite - it should
    make it much easier to see exactly what the review pass sent/received
    without cross-referencing timestamps or guessing from content alone.
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

## Display / OUTPUT_CLASS

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
- **Task runner (`TOOL_TASK_RUNNER::handle_tool`, tools.cpp) doesn't display
  text correctly during an automation** - running `run system test`
  (2026-08-21) surfaced this. One specific cause is already fixed: its local
  `KEYBOARD_INPUT` never set `PROPS.RAW_ECHO = false`, so raw keystrokes
  (including a literal `\r\n` on Enter) were echoed straight to the terminal
  every tick while ncurses owned the screen, corrupting the display - see
  `KEYBOARD_INPUT_PROPERTIES::RAW_ECHO`'s comment in user_io.h. Still
  outstanding: the function's own `cout <<`/`std::cout <<` debug prints
  (`"PRESS ENTER TO CONTINUE"`, `"REQUEST: ..."`, `"INPUT: ..."`) write
  straight to the terminal too, bypassing the buffer-pull pattern (see below)
  the rest of the codebase uses for exactly this reason - they should route
  through `chat.log()`/`response_buffer` like everything else instead.
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

## Voice (Voca)

- Wake word is "olli" (`findWakeWord()`/`findSleepTrigger()`, voca.cpp) -
  done, no longer "voca".

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
