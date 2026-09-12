# olli

**olli** is a fully local, offline voice assistant. A single C++ process
drives a [Ollama](https://ollama.com) language model, calls real tools (clock,
timers, web search, Philips Hue lights, scripted automations), and both speaks
and listens in-process via local TTS/STT engines. Nothing leaves your machine
except optional web searches and calls to your own Hue bridge.

```
        speech ──►  VOCA (ears, in-process)  ──┐
                                                │
   Hue lights ◄──                              ▼
   web search ◄──►      olli  (C++ core + Ollama)   ◄── keyboard
      timers  ◄──                              │
                                                ▼
                                 voice  (in-process: espeak-ng + aplay)
```

---

## How it works

olli is a single process. Both halves of voice — listening and speaking —
happen directly in-process, no sockets, files, or second process involved:

- **Listening** goes through `Voca`, an offline wake-word + speech-to-text
  engine (whisper.cpp) running on its own capture and transcription threads.
- **Speaking** goes through `TextToSpeech`, which shells out to `espeak-ng`
  for synthesis and `aplay` for playback on a background thread.

Both classes are declared and defined entirely inside `io_worker.h`/`.cpp`
now (moved there 2026-08-28 - there used to be a separate `AUDIO_CONTROL_CLASS`
wrapper in its own `audio_control.h`/`.cpp`, with `Voca`/`TextToSpeech` each
in their own files too) - `IO_WORKER_CLASS` (see [Display](#display)) is the
*only* thing that ever touches either, as its own private `tts`/`voca`
members. Both are constructed right before `thread_main()` sets `RUN = true`
and torn down right after its `while(RUN)` loop exits, not for
`IO_WORKER_CLASS`'s whole lifetime - so they only exist while the worker
thread is actually running. `IO_WORKER_CLASS::speak()`/`stop_speaking()` are
the only way anything outside `io_worker.cpp` reaches either.

`IO_WORKER_CLASS`'s own `thread_main()` coordinates the two directly, once
per tick, on its own thread: it watches `TextToSpeech::isSpeaking()` and
`pause`s Voca while olli is talking, resuming listening once speech ends
(this used to run on a separate, dedicated `AUDIO_CONTROL_CLASS` thread of
its own - folded into `IO_WORKER_CLASS`'s existing ~20ms tick instead, since
nothing about it needs its own thread).

### The settings folder (`~/olli_files/`)

Created automatically on first run, and used for persistence rather than
inter-process coordination:

| Path | Purpose |
|------|---------|
| `settings.json` | Your API keys and Hue bridge address (see [Configuration](#configuration)). |
| `history.json` | Persisted chat history, reloaded on start. |
| `history_debug.txt` | Human-readable dump of the current history. |
| `scenes.json` | Locally saved Hue light scenes. |
| `models/` | Whisper model file(s) for Voca's speech-to-text. Always shared — see below. |
| `chat_log.txt` | Live, flat-text transcript of the current session (see [Chat log](#chat-log) below). |
| `chat_logs/` | Archived, timestamped past sessions' transcripts. |

#### Profiles

Passing a name on the command line (`./build/olli ron`) points everything
above at `~/olli_files_ron/` instead of the shared `~/olli_files/`, so each
person gets their own settings, history, and scenes. The first time a named
profile runs, it's seeded by copying `~/olli_files/` (if one exists) rather
than starting empty. `models/` is the one exception — it's never copied per
profile and is always read from the shared `~/olli_files/models/`, since the
whisper model file is large and has no reason to differ per person.

---

## Building & running

See [BUILD_AND_INSTALL.md](BUILD_AND_INSTALL.md) for the full guide —
manual build-from-source steps, or an option to have Claude Code do the
whole install and adapt it to your machine.

---

## Configuration

On first run olli writes `~/olli_files/settings.json` with placeholder values.
Edit it to enable the optional tools:

```json
{
    "tool_web_search_apiKey": "<your serpapi.com key>",
    "tool_hue_lights_apiKey": "<your Hue bridge application key>",
    "tool_hue_lights_bridge_ip": "192.168.1.x"
}
```

- **Web search** uses [SerpAPI](https://serpapi.com). Without a key the
  `web_search` / `fetch_website_content` tools will return errors, but the rest
  of the assistant works fine.
- **Hue lights** need your bridge's IP and an
  [application key](https://developers.meethue.com/develop/get-started-2/).

Which tools are available is set in `main.cpp` (`chat.TOOL_PERMISSIONS.*`), as
is the model, thinking mode, and persona (`OLLAMA_OPENING`).

---

## Tools

| Tool | What it does |
|------|--------------|
| `set_hue_light` | Turns lights on/off, sets brightness, colour (preset, hex, or xy), alerts/flashes. `light_id: "all"` targets every light. |
| `list_hue_lights` | Reports the current state of every connected light. |
| `manage_hue_scenes` | Save / load / remove / list local light scenes (stored in `scenes.json`). |
| `set_thinking_mode` | Toggles the model's internal reasoning stream at runtime. |
| `web_search` / `fetch_website_content` | SerpAPI search and page-text extraction (via libcurl), cleaned for the model. |
| `run_automation_task` | Runs a scripted, multi-step macro (see below). |

### Automations (Task Runner)

`run_automation_task` matches a spoken intent to a named script loaded from
disk. Scripts are plain `.task` files under a profile's own
`scripts/` directory (`~/olli_files_<name>/scripts/*.task`,
`TASK_SIMPLE_MANAGER::load_all_task` in `tools_helper.cpp`) - reloaded fresh
on every `run_automation_task` call, so adding, editing, or removing a
`.task` file takes effect immediately, no restart needed.

A `.task` file looks like:

```
NAME: system test
PURPOSE: This is a series of a few simple questions to check responses.
DIRECTORY: system_test
---
what time is it?
[ASK]What is a name for a dog?
[PAUSE]
Announce the system test is complete.
```

Three header lines (`NAME:`/`PURPOSE:`/`DIRECTORY:`), a `---` separator, then
one command per line, sent to a background instance in order. A line
starting with `#` is a comment - skipped entirely when the script loads, so
it never runs and never counts as a command. Special line-prefixes:
`[ASK]<text>` pauses and shows `<text>` as a request, then feeds whatever
the user types back in as the next line; `[PAUSE]` just waits for Enter, no
LLM involved; `[PRINT]<text>` displays `<text>` verbatim with no LLM call
and immediately continues (handy for several lines of narration back to
back, with nothing printed between them); `[FILE_IN:name]` reads `name` in
from the task's own files folder (see below) and feeds its contents in as
the next line, same as a typed line would be; `[FILE_APPEND:name]` appends
whatever the *previous* line's response was to `name` in that same folder
(creating it if needed) and continues immediately, no LLM call;
`[KEYBOARD_INPUT:on]`/`[KEYBOARD_INPUT:off]` and `[TTS_OUTPUT:on]`/
`[TTS_OUTPUT:off]` turn typed input and spoken responses on or off for the
rest of the script (or until toggled again) - handy for a run that
shouldn't be interrupted by a stray keystroke, or one that shouldn't talk
over itself. `[ASK]` and `[PAUSE]` each save whatever `[KEYBOARD_INPUT]` is
set to, force it on for themselves (they inherently need a keypress no
matter the current setting), then restore the saved value once done - so a
script only needs one `[KEYBOARD_INPUT:off]` near the top, not one around
every command that needs input. `[TTS_OUTPUT]` has no such auto-restore;
turn it back on explicitly wherever the script should start talking again.

Every task gets its own folder under the profile's `files/` directory -
`~/olli_files_<name>/files/<dir>/`, where `<dir>` is the `.task` file's own
`DIRECTORY:` name if it set one, or its `NAME:` otherwise (either way, run
through a sanitizer that strips `/`/`\` and swaps spaces for underscores, so
it can never point outside that folder). `FILE_IN`/`FILE_APPEND`'s own
`name` argument gets the same treatment - both commands only ever read or
write flat filenames confined to that one folder, nothing else on disk is
reachable from a script. Unlike the task's other scratch directory (created
only when `DIRECTORY:` is set, and deleted once the script finishes), this
one is never cleaned up - anything a script writes there via `FILE_APPEND`
stays for as long as you want it to, across runs.

If `FILE_IN` can't find/read `name`, it doesn't abort the script - it falls
back to the same pause-and-prompt behavior as `[ASK]`, so you can paste the
content in by hand and the script continues from there.

Sample scripts (`system test`, `process resume`, `print test`, and an
original creative one, `fitter status`) live under `sample_scripts/` in this
repo - copy whichever ones you want into a profile's own `scripts/`
directory to use them.

---

## Remote tools

Beyond the built-in tools above, olli can load tools from **standalone
external programs** at runtime — no recompiling or restarting olli required.
Each one is its own independent process with its own build and its own
lifecycle: it connects to olli over a small TCP protocol, registers whatever
it wants to expose exactly the way a built-in tool would (`TOOL_REMOTE`,
`source/remote_tools.h`/`.cpp`, never knows or cares what program it's
proxying for), and can run before olli even starts, keep going if olli isn't
reachable, and reconnect automatically once it is. Full wire protocol in
[`tools/PROTOCOL.md`](tools/PROTOCOL.md).

olli listens for these on **port 47601**, loopback-only for now (see
`tools/PROTOCOL.md`'s Scope section).

### Try the example

[`tools/clock/`](tools/clock) is a full worked example — a big ASCII-art
digital clock running in its own terminal that registers `get_clock_time`,
`set_timer`, and `check_timer` with olli (named countdown timers with an
optional follow-up action; olli announces expiry in-character):

```bash
cd tools/clock
make
./clock          # connects to olli on this machine (127.0.0.1)
./clock <ip>      # connects to olli on another machine
./clock --help
```

### Write your own

Start from [`tools/template/`](tools/template) rather than from scratch —
copy the directory, fill in the two spots marked `CUSTOMIZE #1` (what your
tool is called and what it does), and everything else — connecting,
registering, heartbeat, automatic reconnect — is already there, working.
See [`tools/template/README.md`](tools/template/README.md) for the exact
steps.

---

## Voice & keyboard commands

**Wake / sleep (spoken to Voca):** say *"hey olli"* to wake it; *"stop
listening"* / *"sleep olli"* to sleep it; *"stop talking"* to interrupt speech.
Voca also auto-sleeps after 5 minutes of silence.

**Jump phrases (typed or spoken)** trigger a scene macro directly, bypassing the
model's tool loop:

| Phrase | Effect |
|--------|--------|
| `I'm home.` / `I'm awake.` / `Lights on.` | Load the *repose* scene |
| `I'm leaving.` | Load the *labor* scene |
| `I'm sleeping.` / `Lights off.` | Load the *slumber* scene |

**Exit:** type `bye`, `quit`, or `Goodbye.`, or press **Ctrl+C** — history is
saved on the way out either way. (Raw-mode input disables the terminal's own
signal generation, so Ctrl+C is handled explicitly rather than arriving as a
real `SIGINT` — see `KEYBOARD_INPUT::EXIT_REQUESTED` in `user_io.h`.)

**Web links:** whenever `web_search`/`fetch_website_content` (see Tools below)
surface a link, a short `[Links: [1] title, ...]` notice appears under the
response - press **Ctrl+L** to open the full list as real, clickable links.
Links aren't rendered inline in the chat transcript itself: ncurses'
own text-drawing routines strip out the escape codes a clickable terminal
link needs, showing garbled text instead, so Ctrl+L's popup briefly drops
out of ncurses to print real links directly, then returns.

---

## Display

All screen output goes through one `OUTPUT_CLASS` instance (`user_io.h`/`.cpp`),
which sorts everything into four buckets: `system_message` (status/tool
activity), `user_input` (an echo of what was typed/said), `chat_response`, and
`chat_thinking` (the model's reasoning stream, when enabled).

Two ways to render those buckets, chosen once at startup by the `USE_NCURSES`
constant in `main.cpp`:

- **`display_with_ncurses()`** (the default) — a windowed layout: a 3-line
  scrolling system-message strip, a scrolling chat transcript (what you typed,
  dimmed grey, above the assistant's replies in the default foreground color),
  and an input line showing what you're typing live with a reverse-video block
  marking the cursor position. While the model is reasoning, a small bordered
  box floats over the upper-right corner of the transcript instead of
  displacing it, and lingers for ~2 seconds after reasoning ends before
  closing. `win_chat` and that floating box are the only two windows that ever
  overlap, so they're the only two backed by ncurses' **panel** library
  (`PANEL`/`update_panels()`) rather than plain `wrefresh()` — panels track
  which window is stacked on top and correctly repaint whatever a closed
  panel was covering, which a window's own refresh can't do on its own (it
  only knows about writes to its own buffer). Handles terminal resizes
  (`SIGWINCH` → `resizeterm()`, re-laying out every window without losing the
  transcript's scrollback).
- **`display()`** — the original plain scrolling behavior: everything printed
  straight to the terminal in order, no windows, no color. Kept as a fallback
  in case ncurses ever needs to be ruled out.

Flip `USE_NCURSES` to `false` and rebuild to switch to the plain version;
there's no runtime toggle.

### How this stays decoupled from the chat engine

`ollama_system` (the chat engine) never talks to `OUTPUT_CLASS` directly - it
just has one `COMMS comms` member (`comms.h`/`.cpp`), bundling the three
output-direction buffers (`INPUT_FROM_LLM`, `INPUT_FROM_THINKING`,
`INPUT_FROM_SYSTEM` - renamed 2026-08-28 from `response_buffer`/
`thinking_buffer`/`log_buffer`; a fourth, `tts_buffer`, is gone entirely, see
below) that tool handlers and the streaming code append to, guarded by one
shared `output_buffer_mutex`. `OUTPUT_CLASS::get_response(COMMS&)` is what
reaches *in* and pulls (and clears) those buffers each tick - a pull, not a
push, so `ollama_system` and its tool handlers never need a pointer back to
the display layer. The same pull happens for task-runner automation
instances, so their output shows up on screen too, not just the main
conversation's (sidetrack's own output pull is currently disconnected -
sidetrack is being reworked, see [Source layout](#source-layout)).

Keyboard input, voice-event polling, TTS/STT, and both `display()` calls
above all happen on `IO_WORKER_CLASS`'s own background thread
(`io_worker.h`/`.cpp`), not `main.cpp`'s - `KEYBOARD_INPUT` and `OUTPUT_CLASS`
have no thread-safety of their own (raw termios manipulation, unlocked
fields, and ncurses itself is single-thread-only), so this worker is their
sole owner and sole caller for its entire lifetime. `IO_WORKER_CLASS` also
keeps two private `COMMS` instances of its own - `comms_buffer` and
`comms_buffer_audio` - each a same-shaped independent copy of
`INPUT_FROM_LLM`/`INPUT_FROM_THINKING`/`INPUT_FROM_SYSTEM`, fanned out from
one drain of the real `comms` so the screen (`comms_buffer`, drained by
`get_response()` on the worker's own thread) and TTS (`comms_buffer_audio`,
chunked by speaking-idle rather than punctuation/length - see
[How it works](#how-it-works)) never race each other for the same source
text. `comms` is the only thing that crosses the boundary back to
`main.cpp`'s own loop, via `IO_WORKER_CLASS::exchange(ollama_system&,
COMMS&, tools_list)` (called once per `main.cpp` tick) - relayed both
directions under a small two-flag lock (`INTERUPTED`/`PROCESSING`):
`INPUT_FROM_LLM`/`INPUT_FROM_THINKING`/`INPUT_FROM_SYSTEM` flow real-`comms`
→ `comms_buffer`(`_audio`); `ENTER_PRESSED`/`INPUT_FROM_USER`/`INTERRUPTED`/
`IS_TYPING`/`EXIT_REQUESTED` (a submission, an interrupt, an exit request -
renamed 2026-08-28 from `send`/`submitted_line`/`stop_requested`/
`exit_requested`, matching `KEYBOARD_INPUT`'s own field names) flow the
other way. `COMMS::audio` is gone (there's no `AUDIO_CONTROL_CLASS` to point
at anymore - see [How it works](#how-it-works)) - reaching TTS now goes
through `comms_stt_tts` inside `IO_WORKER_CLASS` instead of a pointer
on `COMMS` itself, which also means background tasks and sidetrack (neither
routed through `exchange()`) currently have no speech output at all.

`IO_WORKER_CLASS::thread_main()`'s own tick actually treats keyboard/
ncurses, STT/TTS, and the web interface below as three parallel channels,
each with its own `COMMS` copy (`comms_keyboard`/`comms_stt_tts`/
`comms_web`) snapshotted from `comms_buffer` at the start of a tick -
whichever channel has new input hands its own copy back to `comms_buffer`
as one unit, then all three get re-synced before their own
display/speak/push step drains them. `COMMS` gained a real `operator=`
to make that whole-struct round-tripping possible at all (its
`close_chat_log_requested` field is a `std::atomic<bool>`, which
otherwise implicitly deletes it - the new operator copies every other
field and leaves that one alone).

---

## Web interface

Beyond the local terminal, olli can also be driven from a browser on the
same LAN - a second, independent way in, running alongside (not instead
of) the ncurses/keyboard session, not replacing it. `WEB_SERVER_CLASS`
(`source/web_server.h`/`.cpp`) owns a small embedded HTTP server
(cpp-httplib, the same header-only dependency [olla.cpp](source/olla.cpp)
already uses as a client) on its own background thread, listening on
**port 47602** (separate from the [remote tools](#remote-tools) protocol's
47601) and bound to every network interface rather than loopback-only,
since the whole point is reaching it from another machine. No
authentication - trusted home LAN only, same trust boundary as remote
tools.

Open `http://<olli-host>:47602/` in any browser to get a simple chat
page: type a message and hit Enter/Send, and it joins the same
conversation as the local session, responses streaming back live
(Server-Sent Events) - typed/spoken input from *any* channel (keyboard,
speech, or the web page itself) shows up in all of them, not just the one
it came from. A dedicated strip at the top shows system messages, same as
ncurses' own `win_system`. A right-side panel lists whatever tools are
currently registered. A small floating box in the upper-right corner
shows the model's reasoning while it's thinking, mirroring
`display_with_ncurses()`'s own floating thinking box (see
[Display](#display) above) - it closes the moment the real reply starts,
not on a thinking-side timeout, lingering a couple seconds first rather
than vanishing mid-read. Links render as real clickable `<a>` tags -
simpler than ncurses' own notice-plus-popup dance, since a browser
doesn't have ncurses' escape-sequence-stripping problem. A reply from a
background task instance (task-runner, the delegator) shows up in the
same color the terminal already uses for it (cyan/yellow/magenta/green).

Starting to type interrupts TTS/an in-flight response, the same way
typing at the local terminal does - not a live per-keystroke sync, just
one lightweight ping the moment a fresh line starts. A running `.task`
script's "press enter to continue" works from the page too (an empty
submission sends a bare newline, matching the terminal's own keyboard
input), and `[KEYBOARD_INPUT:off]` both disables and visibly dims the
page's input box, mirroring the terminal's own dimmed input line.

Submissions are line-at-a-time (type, then send), not a live per-keystroke
mirror of what's being typed the way the local terminal's input line
shows - that would need the page posting on every keystroke instead of
just on submit, real added scope not yet built. Also not yet built:
authentication (deliberately out of scope for now), and real multi-tab
support - the output/tools-panel queues are single shared buffers rather
than per-connection, built for the expected case of one browser tab
watching at a time.

If a browser on another machine can't reach it, check the *host*
machine's firewall before assuming something's wrong with olli itself -
a listening-but-unreachable port (confirm with `ss -tlnp | grep 47602`)
timing out rather than refusing the connection is the telltale sign
(`sudo ufw allow 47602/tcp` on Ubuntu).

---

## Chat log

Independent of `history.json` (which is structured, periodically rewritten
whole, and fed back into the model), `chat_log.txt` is a flat, human-readable,
append-only transcript — `OUTPUT_CLASS::append_to_chat_log()` (`user_io.cpp`)
writes it right where `user_input`/`chat_response` get shown, so it always
matches exactly what appeared on screen. Theatrical-script style, labeled only
on a speaker change so streamed replies don't repeat the label every chunk:

```
Ron: what's the weather like?

Olli: I don't have a weather tool yet, sorry.
```

The speaker label for you is whatever name you gave at startup (`./build/olli
ron`, or typed at the "What is your name?" prompt), capitalized — falls back
to plain "You" for the shared/no-name profile.

`chat_log.txt` gets **archived**, not appended to forever: `close_chat_log()`
moves it into `chat_logs/<YYMMDD.HHMM>.chat_log.txt` and lets the next message
start a fresh file. This happens at clean program exit, and whenever
sidetrack's idle auto-clear wipes history (see [the background sidetrack
thread](#the-background-sidetrack-thread) below) — both moments a
conversation is considered "over."

---

## Source layout

```
source/
├── main.cpp / main.h          Entry point and the main event loop.
├── olla.{h,cpp}               ollama_system: chat engine, streaming, history, tool dispatch.
├── comms.{h,cpp}               COMMS: the three output buffers + input-direction signals every
│                               ollama_system carries - the one thing that crosses the
│                               IO_WORKER_CLASS thread boundary (see Display below).
├── tools.{h,cpp}               TOOL_BASE and every TOOL_* tool implementation (see Tools below).
├── helper_olli.{h,cpp}        Settings (profile loading/saving).
├── user_io.{h,cpp}            KEYBOARD_INPUT (raw-mode input) and OUTPUT_CLASS (all screen output) -
│                               both owned exclusively by IO_WORKER_CLASS, not CLASS_SYSTEM.
├── io_worker.{h,cpp}           IO_WORKER_CLASS: owns keyboard input, voice-event polling, screen
│                               drawing, and TTS/STT (TextToSpeech/Voca, declared and defined
│                               directly in this file - see How it works above) on its own
│                               background thread - see Display below. AUDIO_CONTROL_CLASS/
│                               audio_control.{h,cpp}/tts.{hpp,cpp}/voca.{hpp,cpp} are gone
│                               (2026-08-28) - folded in here entirely.
├── web_server.{h,cpp}          WEB_SERVER_CLASS: the browser-driven web interface - see
│                               Web interface below.
├── sidetrack.{h,cpp}          Background thread: consolidation, "second guess", idle auto-clear.
│                               Being reworked (2026-08-28) - disconnected from IO_WORKER_CLASS
│                               and commented out in main.cpp in the meantime; the notes below
│                               still describe its pre-rework design.
├── tools_helper.{h,cpp}       HUE_LIGHT_CLASS, task definitions, tool permissions.
├── stringthings.{h,cpp}       General-purpose string utility library.
├── fled_time.{h,cpp}          Timing / frame-pacing helpers used by the background threads.
├── threading.{h,cpp}          Thin std::async thread wrapper.
├── system.h                   Aggregates Settings + user identity + remote-tool listener into
│                               one object (keyboard/display/audio moved to IO_WORKER_CLASS above).
└── CMakeLists.txt             Build definition.
```

### The background "sidetrack" thread

**Currently disconnected (2026-08-28)** - sidetrack is being reworked; every
call into it is commented out in `main.cpp` for now, so none of the three
routines below actually run. Left in place as a description of its
pre-rework design, for whenever the rewrite lands.

Three housekeeping routines run off the main thread (`sidetrack.cpp`):

1. **Consolidation** — when the conversation grows past a threshold, older
   messages are summarised into a higher "consolidation level", compressing
   history so long sessions stay within the model's context window. The
   assistant's foundational persona/instructions message is tagged level
   `-1` and is never summarised.
2. **Second-guess** — after each turn, an "internal monologue" pass reviews the
   answer and, if it finds a genuinely useful addition, speaks a follow-up
   thought; otherwise its "nothing to add" reply is routed into the thinking
   window (see [Display](#display)) instead of the transcript, so it doesn't
   show up as clutter.
3. **Idle auto-clear** — after 30 minutes with no user activity, the
   conversation history is wiped back down to just the protected,
   `-1`-tagged persona message. Skipped if there's any activity in flight
   when the timer fires. A stale or "poisoned" context can't outlive 30
   minutes of silence.

---

## Notes & limitations

- olli targets **Linux** (raw-terminal input, `localtime_r`, PulseAudio/ALSA).
  The settings path has a Windows branch but the audio/input paths are POSIX.
- The ncurses display needs a real, recognized `$TERM` — ncurses' `initscr()`
  exits the whole process immediately if it can't identify the terminal type
  (e.g. `$TERM=dumb` or unset in some non-interactive/piped contexts). A
  normal interactive terminal is unaffected; if you hit this, either run in
  one, or flip `USE_NCURSES` to `false` in `main.cpp` and rebuild.
- olli is a personal/experimental project; expect rough edges. Some source files
  carry commented-out experiments kept as design notes.

## License

GPL-3.0 — see [LICENSE](LICENSE).
