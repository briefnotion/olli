# TODO

Guiding principle for all of the below: keep code simple, easy to read, and
well compartmentalized. That's the bar for new work here, not just a nice-to-have.

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
- **Improve current tools** - a general polish pass over the existing tool
  set (Hue lights, timers, web search, task runner) rather than one specific
  fix.

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

## Display / OUTPUT_CLASS

- **Filter DONE-only responses from display** - sidetrack's second-guess
  review answers with the literal word "DONE" when it has nothing worth
  adding (see the `starts_with(..., "DONE")` check in `sidetrack.cpp`). Now
  that its output is actually wired into `OUTPUT_CLASS::display()`, a
  DONE-only response shows up as visible clutter after a turn instead of
  being silently dropped like it used to be.
- **Add a visible cursor to the ncurses input window** - `display_with_ncurses()`
  calls `curs_set(0)` at init (user_io.cpp), hiding the terminal cursor
  entirely. The input window renders `"> " + LINE` as plain text each tick,
  but there's nothing marking where in that line you actually are - no
  blink, no visible insertion point, so backspace/edit feedback is weaker
  than it should be. Needs `curs_set(1)` (or `2`) plus actually positioning
  it (`wmove`) to the end of the rendered line in `win_input` before that
  window's `wrefresh()`.
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
  to actually fix the underlying issue.
