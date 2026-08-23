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
- **Tools rework - in progress, not done** (started 2026-08-21). Completed so
  far: pulled every `TOOL_*` class out of `olla.h`/`olla.cpp` into their own
  `tools.h`/`tools.cpp`; cleaned out stale/AI-session-artifact comments there;
  gave every tool the same `configure`/`register_tool`/`check`/`monitor_tool`
  shape via an abstract `TOOL_BASE`, replacing the six fixed named members on
  `ollama_system` with `tools_list` (`std::vector<std::unique_ptr<TOOL_BASE>>`)
  so adding a tool no longer touches `ollama_system` itself. Still open:
  - Redo (or drop entirely, undecided) the `TOOL_PERMISSIONS_CLASS` system
    (tools_helper.h) - a bare bool per tool, hand-maintained in parallel with
    `tools_list` now that tools are stored generically.
  - `ollama_system` can't reach `CLASS_SYSTEM` (`system.h`) at all right now -
    `TOOL_TASK_RUNNER`'s dispatcher branch (`handle_instance_tools`,
    tools.cpp) still special-cases toggling `Keyboard_Input_Enabled` by
    checking the tool name directly, marked `TODO` inline, because `check()`'s
    signature has no way to reach `key_input`/`output` otherwise.
  - The task-runner display bug above.
  - General polish pass over the existing tool set (Hue lights, timers, web
    search, task runner) beyond the structural rework itself.

## Session & model behavior

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
- **Revisit bringing Voca and Lira into input/output, using today's buffer
  pattern** - see "What we built today: the buffer-pull pattern" below for
  the mechanism this would reuse. When this first came up (before the
  ncurses work), the conclusion was "don't move `voca` into `KEYBOARD_INPUT`"
  - see `AUDIO_CONTROL_CLASS::adjust_audio_files()` (audio_control.cpp),
  which polls `tts.isSpeaking()` every ~500ms on its own thread and calls
  `voca->pause()`/`resume()` accordingly. That coupling was the real
  blocker, not the callbacks. Worth another look now that the buffer-pull
  pattern exists: it might not remove the AUDIO_CONTROL_CLASS-owns-both
  coupling (Voca and TTS still need to poll each other's state regardless of
  which class holds the pointer), but it could still be the right shape for
  getting Voca's transcript events and TTS's spoken/speaking status
  *visible* through `OUTPUT_CLASS`/`KEYBOARD_INPUT` the same clean way
  `ollama_system`'s response/thinking/log now are, without necessarily
  relocating `voca`/`tts` themselves. Needs real design thought, not a
  quick change - re-read the earlier discussion in full before starting.

### What we built today: the buffer-pull pattern

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

## Voice (Voca)

- **Change the wake word from "voca" to "olli"** - `findWakeWord()`
  (voca.cpp) matches "hey" followed by a "vo"-prefixed word
  (`startsWithVo()`/`kVoPrefixMinLen`, loose - see its comment for why),
  or a bare "voca"/"voka" anywhere. Needs the prefix/bare-word list
  swapped to match "olli" instead (and probably the sleep-trigger side,
  `findSleepTrigger()`, which also keys off a "vo"-prefixed word next to
  a control word - see its own comment for the "hey voca, stop talking"
  disambiguation it does).

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
