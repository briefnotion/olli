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

- **Listening** goes through `Voca` (`voca.hpp`/`voca.cpp`), an offline
  wake-word + speech-to-text engine (whisper.cpp) running on its own capture
  and transcription threads. `AUDIO_CONTROL_CLASS` (`audio_control.h`/`.cpp`)
  owns it; `IO_WORKER_CLASS` (`io_worker.h`/`.cpp` - see [Display](#display))
  polls it once per its own tick, on its own thread, independent of the
  chat/model logic in `main.cpp`.
- **Speaking** goes through `TextToSpeech` (`tts.hpp`/`tts.cpp`), which shells
  out to `espeak-ng` for synthesis and `aplay` for playback on a background
  thread.

`AUDIO_CONTROL_CLASS` coordinates the two directly in memory: it watches
`TextToSpeech::isSpeaking()` and `pause`s Voca while olli is talking, resuming
listening once speech ends.

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

## Building the C++ core

### Dependencies

- A C++17 compiler (GCC or Clang) and **CMake ≥ 3.10**
- **libcurl** development headers — `sudo apt install libcurl4-openssl-dev`
- **[cpp-httplib](https://github.com/yhirose/cpp-httplib)** (header-only)
- **[nlohmann/json](https://github.com/nlohmann/json)** (header-only)
- **ncursesw** (wide-char ncurses), including its **panel** library
  (`libpanelw`) — for the windowed display (see [Display](#display)
  below). Not packaged system-wide here, so it's built from source into
  its own local install prefix, kept separate from olli. Panels come from
  the same build, no extra configure flags needed.

By default the build looks for the two header-only libraries as sibling
checkouts next to this repository:

```
code/
├── olli/            ← this repo
├── cpp-httplib/     ← git clone https://github.com/yhirose/cpp-httplib
├── json/            ← git clone https://github.com/nlohmann/json
└── ncurses-snapshots/  ← see below
```

If you keep them elsewhere (or installed system-wide), point CMake at them:

```bash
cmake -S source -B build \
      -DHTTPLIB_INCLUDE_DIR=/path/to/cpp-httplib \
      -DJSON_INCLUDE_DIR=/path/to/json/include
```

#### Building ncursesw

CMake looks for a *built* ncursesw next to this repo (`../../ncurses-snapshots`,
same sibling convention as above) — it doesn't build ncurses itself, since
ncurses uses autotools, not CMake. One-time setup:

```bash
git clone --depth 1 https://github.com/ThomasDickey/ncurses-snapshots.git
cd ncurses-snapshots
mkdir build && cd build
../configure --prefix="$(pwd)/../install" \
      --without-shared --with-normal --enable-widec \
      --without-debug --without-ada --without-tests \
      --without-manpages --without-progs
make -j
make install
```

Static (`--without-shared`) so the `olli` binary stays self-contained, and
wide-char (`--enable-widec`) so UTF-8 renders correctly. The install prefix
also ends up with its own bundled terminfo database, so nothing about it
depends on what's installed system-wide.

### Compile

```bash
# from the repo root
cmake -S source -B build
cmake --build build -j
```

The resulting `olli` binary lands in `build/`. (The `build/cmak.sh` and
`build/m` scripts are one-line shortcuts for these two steps.)

The project builds with strict warnings-as-errors (`-Wall -Wextra -Wpedantic
-Wconversion … -Werror`); third-party headers are included as `SYSTEM` so their
warnings don't break the build.

---

## Running

olli expects an Ollama server on `localhost:11434` with a **tool-capable**
model pulled. The default is `qwen3:8b`:

```bash
ollama serve            # if not already running
ollama pull qwen3:8b
```

Then start olli:

```bash
./build/olli          # shared settings, ~/olli_files/
./build/olli ron      # ron's own settings, ~/olli_files_ron/ (see Profiles below)
./build/olli --help   # usage, no model/audio/profile init - exits immediately
```

Voice input and output both run in-process, so there's nothing else to start.
You can also just type — speech input is optional; speech output is available
either way.

### Text-to-speech

Speech is synthesized and played by olli itself (`tts.hpp`/`tts.cpp`), which
shells out to `espeak-ng` for synthesis and `aplay` for playback — no Python,
no separate process:

```bash
sudo apt install espeak-ng   # synthesis
sudo apt install alsa-utils  # aplay
```

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

`run_automation_task` matches a spoken intent to a pre-defined command sequence
in `tools_helper.cpp` (`TASK_SIMPLE_MANAGER::load_all_task`). Two ship by
default: `run system test` (a smoke test of several tools) and `run process
resume` (a résumé-vs-job-description writing workflow). Sequences can pause for
input (`[[ASK]]`) or a keypress (`[[ENTER TO CONTINUE]]`).

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
just has one `COMMS comms` member (`comms.h`/`.cpp`), bundling the four
buffers (`response_buffer`, `thinking_buffer`, `tts_buffer`, `log_buffer`)
that tool handlers and the streaming code append to, guarded by one shared
`output_buffer_mutex`. `OUTPUT_CLASS::get_response(COMMS&)` is what reaches
*in* and pulls (and clears) those buffers each tick - a pull, not a push, so
`ollama_system` and its tool handlers never need a pointer back to the
display layer. The same pull happens for sidetrack's background "second
guess" review and for task-runner automation instances, so their output
shows up on screen too, not just the main conversation's.

Keyboard input, voice-event polling, and both `display()` calls above all
happen on `IO_WORKER_CLASS`'s own background thread (`io_worker.h`/`.cpp`),
not `main.cpp`'s - `KEYBOARD_INPUT` and `OUTPUT_CLASS` have no thread-safety
of their own (raw termios manipulation, unlocked fields, and ncurses itself
is single-thread-only), so this worker is their sole owner and sole caller
for its entire lifetime. `comms` is the only thing that crosses the boundary
back to `main.cpp`'s own loop, via `IO_WORKER_CLASS::exchange(COMMS&)`
(called once per `main.cpp` tick) - a submitted line, an interrupt, or an
exit request, relayed under a small two-flag lock. `COMMS::audio`
(`comms.h`) works the same way for reaching `AUDIO_CONTROL_CLASS` - a
pointer on `COMMS` itself rather than a process-wide global, so a future
different `COMMS` (a remote session, say) isn't forced to share the one
local speaker.

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
├── comms.{h,cpp}               COMMS: the four output buffers + audio pointer + input-direction
│                               signals every ollama_system carries - the one thing that crosses
│                               the IO_WORKER_CLASS thread boundary (see Display below).
├── tools.{h,cpp}               TOOL_BASE and every TOOL_* tool implementation (see Tools below).
├── helper_olli.{h,cpp}        Settings (profile loading/saving).
├── user_io.{h,cpp}            KEYBOARD_INPUT (raw-mode input) and OUTPUT_CLASS (all screen output) -
│                               both owned exclusively by IO_WORKER_CLASS, not CLASS_SYSTEM.
├── io_worker.{h,cpp}           IO_WORKER_CLASS: owns keyboard input, voice-event polling, and
│                               screen drawing on its own background thread - see Display below.
├── audio_control.{h,cpp}      Owns TTS and Voca in-process, coordinates the two.
├── tts.{hpp,cpp}              TextToSpeech: in-process synthesis (espeak-ng) + playback (aplay).
├── voca.{hpp,cpp}             Voca: in-process wake-word + speech-to-text (whisper.cpp).
├── sidetrack.{h,cpp}          Background thread: consolidation, "second guess", idle auto-clear.
├── tools_helper.{h,cpp}       HUE_LIGHT_CLASS, task definitions, tool permissions.
├── stringthings.{h,cpp}       General-purpose string utility library.
├── fled_time.{h,cpp}          Timing / frame-pacing helpers used by the background threads.
├── threading.{h,cpp}          Thin std::async thread wrapper.
├── system.h                   Aggregates Settings + audio + user identity + remote-tool listener
│                               into one object (keyboard/display moved to IO_WORKER_CLASS above).
└── CMakeLists.txt             Build definition.
```

### The background "sidetrack" thread

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
