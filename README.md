# olli

**olli** is a fully local, offline voice assistant. A single C++ process
drives a [Ollama](https://ollama.com) language model, calls real tools (clock,
timers, web search, Philips Hue lights, scripted automations), and both speaks
and listens in-process via local TTS/STT engines. Nothing leaves your machine
except optional web searches and calls to your own Hue bridge - unless you
choose the optional Google Gemini backend, which sends the conversation to
Google (see [AI_BACKENDS.md](AI_BACKENDS.md)).

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

Text isn't handed to `espeak-ng` raw. `display_with_tts()` runs each chunk
of streamed LLM output through `tts_filter()` (`stringthings.cpp`) first,
which turns markdown into something that sounds right instead of being read
symbol by symbol:

| LLM writes | Spoken as |
|---|---|
| `**bold**` / `__bold__` | higher pitch, slightly slower (`<prosody>`) |
| `*italic*` / `_italic_` | espeak-ng's milder `<emphasis>` |
| `## Heading` | the heading text, then a 500ms pause |
| blank line between paragraphs | a 300ms pause |
| `[text](url)` | just the link text |
| `https://www.example.com/path` | just the site name, "example.com" |
| ```` ``` ```` code block | "code block" |
| table | cells separated by pauses, separator rows dropped |
| emoji | dropped |
| `snake_case` | "snake case" |

`espeak-ng` runs with `-m` (SSML input) for the emphasis/pause tags, so
`tts_filter()` also escapes any literal `&`, `<`, `>`. Anything passed to
`TextToSpeech` must go through `tts_filter()` first. The pitch/rate/pause
values are named constants (`kTtsBoldOpen` etc.) at the top of the TTS
section in `stringthings.cpp`.

Because speech is streamed in chunks, a link or code block can arrive split
across two of them. `tts_hold_point()` finds an unfinished construct at the
end of `tts_pending` (open ```` ``` ````, unclosed `**`, half-arrived
`[text](url`, a URL still arriving, a heading line with no newline yet) and
`display_with_tts()` holds that tail back until it completes - or until
nothing new has arrived for 1.5s, so a stray `[` can never stall speech.

### The settings folder (`~/olli_files/`)

Created automatically on first run, and used for persistence rather than
inter-process coordination:

| Path | Purpose |
|------|---------|
| `settings.json` | Your API keys and Hue bridge address (see [Configuration](#configuration)). |
| `history.json` | Persisted chat history, reloaded on start. |
| `history_debug.txt` | Human-readable dump of the current history. |
| `scenes.json` | Locally saved Hue light scenes. |
| `collection/For_Olli/about_the_user.txt` | Plain-English notes about you (favorite color, where you live, ...), written by hand. See [About the user](#about-the-user-history-slot--2) below. |
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

- **AI backend** (`"ai_backend"`): `"ollama"` (default, local) or `"gemini"`
  (Google's API - needs `gemini_key.txt` in the profile folder). Read once at
  startup. See [AI_BACKENDS.md](AI_BACKENDS.md) before using Gemini - it sends
  your conversation to Google.
- **Web search** uses [SerpAPI](https://serpapi.com). Without a key the
  `web_search` / `fetch_website_content` tools will return errors, but the rest
  of the assistant works fine.
- **Hue lights** need your bridge's IP and an
  [application key](https://developers.meethue.com/develop/get-started-2/).

Every internal tool is always available - there's no permissions/allowlist
mechanism gating them. Which remote tools are available is just whatever's
currently connected. The model, default thinking-mode setting, and persona
(`AI_OPENING`) are set in `main.cpp`.

---

## Tools

olli acts in the world through internal tools (compiled into its core) and
remote tools (standalone programs that connect over TCP at runtime, no
rebuild needed - things like the clock, Hue lights, presence detection, and
the RAG knowledge base, plus an [MCP bridge](tools/mcp_bridge/README.md) that lets
olli use tools from any MCP server you choose). Full breakdown of both kinds, what each one does,
and how to write a new remote tool: **[TOOLS.md](TOOLS.md)**.

`run_automation_task`, one of the internal tools, runs scripted multi-step
macros from a profile's own `.task` files - the full format is in
**[AUTOMATIONS.md](AUTOMATIONS.md)**.

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
(cpp-httplib, the same header-only dependency [ollama_system.cpp](source/ollama_system.cpp)
already uses as a client) on its own background thread, listening on
**port 47602** (separate from the [remote tools](TOOLS.md#remote-tools) protocol's
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
├── ai_systems.h               Shared chat data types (ToolCall, Message, ChatResult, ...).
├── ai_root.{h,cpp}            ai_root: the chat engine - history, tool dispatch, send()'s shared
│                               head and tail. Everything else in olli holds an ai_root.
├── ollama_system.{h,cpp}      ai_root backend for a local Ollama server (default).
├── gemini_system.{h,cpp}      ai_root backend for Google's Gemini API (see AI_BACKENDS.md).
├── ai_factory.{h,cpp}         make_ai_system(): picks the backend from the "ai_backend" setting.
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
├── protected_slots.h           Where the protected history slots (-1 persona, -2 about the user, -3 subcon
│                               note) go: levels are IDs, in order, any may be missing.
├── subcon_note.h              Pure parsing of subcon's peek answer + the -3 nudge text (tested on its own).
├── health.{h,cpp}              HEALTH_CLASS: a quiet background thread that writes olli's own status
│                               (crashes on record, memory, threads, disk, what needs attention) to
│                               collection/Olli_Health/status.txt + events.txt once a minute. Isolated
│                               - see "Olli's health report" below.
├── tool_worker.{h,cpp}         TOOL_WORKER_CLASS: owns all remote-tool wire communication (see
│                               Remote tools below) on its own background thread, same
│                               atomics-rendezvous shape as IO_WORKER_CLASS above. Built-in
│                               tools still dispatch inline on main's own thread (tools.cpp) -
│                               this only advertises them alongside the remote ones.
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
├── system.h                   Aggregates Settings + user identity into one object (keyboard/
│                               display/audio moved to IO_WORKER_CLASS, remote-tool listener
│                               moved to TOOL_WORKER_CLASS, both above).
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

### About the user (history slot -2)

Chat history is one ordered list. Entry 0 is the persona (consolidation level
`-1`); any message with a negative level is protected: never consolidated and
kept through the idle context clear. Each negative level is a reserved slot
with its own ID: `-1` is the persona, `-2` what Olli knows about you, `-3`
subcon's quick note (below). The level is an ID, not a position: the slots sit
together at the top of the history in the order -1, -2, -3, but any of them may
be missing (with no `-2` yet, a `-3` simply follows the `-1`, and a `-2` added
later goes in front of it), and one can be removed on its own
(`ai_root::set_protected_slot()` / `clear_protected_slot()`, rules in
`protected_slots.h`).

Slot `-2` holds what Olli knows about you. You write the source by hand in
plain English, in `collection/For_Olli/about_the_user.txt` inside your settings folder
(created empty on first run; being under `collection/` it is also indexed for RAG
search). After the chat has sat unchanged for 5 minutes, a separate chat
instance (`run_about_user()`, `sidetrack.cpp`) condenses the file into one
paragraph, prefixed "What is known about the user, in case it helps:", and puts
it into slot `-2` (replaced if already there, inserted if not). Nothing is sent
or spoken at that moment; the next request just carries it. It runs again after
the next bit of activity plus another 5 idle minutes, so the wording may drift a
little. A blank or missing file, an interrupted call or an empty answer leaves
the current paragraph alone. While the chat is idle it also starts over every 30 minutes (`ABOUT_USER_REPEAT_INTERVAL`), so an edit to the notes file is picked up without any typing - each run is one model call, which Ollama serves one at a time. The paragraph goes to whichever AI backend is
selected, so with `ai_backend: gemini` it goes to Google like the rest of the chat.

### Subcon's quick note (history slot -3)

While subcon has notes waiting for you, slot `-3` tells the model so and nudges it
to deliver them: "Subcon quick note: your background thinking has 2 things waiting
for the user (topics: Timers, Weather), and at least one is urgent... At the first
natural moment - right after you answer what they just asked, or when they greet
you - call get_subcon_notes and tell them what it says." Delivering them (the
existing `get_subcon_notes` pull) uses the notes up, and the slot is removed on the
next check.

How: `run_subcon_note()` (`sidetrack.cpp`) is a stage machine like the other
sidetrack routines (0 arm timer, 1 wait, 2 send the call, 3 wait for the answer, 4 apply,
100 parked). One minute after the chat last changed it calls subcon's read-only
`peek_subcon_notes` tool through the tool worker and reads the count and the topics (the
pure parsing and wording are in `subcon_note.h`), then parks until the chat changes again -
one check per interaction, like second-guess. It also repeats: 30 minutes after parking it starts over, so a note that arrives while nothing is typed still gets its slot (activity restarts it sooner). The -2 paragraph repeats on the same 30-minute interval; second-guess, consolidation and the context clear deliberately do not.
Subcon never pushes anything; a peek marks nothing as told. The slot carries only
topics, never the facts - those reach you only through `get_subcon_notes`, word for
word. No subcon connected: no call is made and any old slot is removed. A slow,
failed or garbled answer changes nothing. A change to the slot is not counted as
activity (second-guess does not re-review, the about-the-user wait does not
restart). Nothing is spoken when the slot changes; it only rides along with the
next request. Ollama re-reads the conversation after a change at the top of the
prompt, so the first reply after a change can be slower (Gemini is unaffected).

### The background "subconscious" (subcon)

Subcon is a standalone remote tool ([`tools/subcon/`](tools/subcon/README.md)),
run like the other tools. It has its own LLM conversation and works through a
short list of recurring tasks while you're not talking to Olli (weather for
Lafayette, past conversations, notes, lights left on while you're away,
forgotten timers), decides whether anything it found is actually worth telling
you, and keeps those findings in a queue. Each task has a hard limit on how
often it may run. Olli **pulls** the findings: ask "any updates?" (or "give me an update", "catch me up") and
the model calls `get_subcon_notes`, which hands over everything waiting as one
digest. Nothing is pushed to you. See [SUBCON.md](SUBCON.md) for the tasks,
limits and files it keeps in `<profile>/subcon/`, and
[tools/subcon/README.md](tools/subcon/README.md) for how it runs and connects.

(Subcon used to be a thread inside olli. That code was removed on 2026-10-31;
it is still in git history, last present at commit `481e697`.)

---

## Olli's health report

Once a minute olli writes a short plain-English report about itself into its own profile folder:

```
~/olli_files_<profile>/collection/Olli_Health/status.txt   how olli is doing right now (rewritten every minute)
~/olli_files_<profile>/collection/Olli_Health/events.txt   notable things with timestamps (the last 200 are kept)
```

`status.txt` holds when this run started (and whether it followed a crash), the crashes on record from `crash_log.txt` (total / last 24 h /
last 7 days / the last one), memory, threads, open files, free disk space, the sizes of `history.json`, the debug log and the crash log, and a
**Needs attention** line that is "nothing" or lists what is wrong (a crash in the last 24 h, under 5 GB or 10 % disk free, over 2 GB memory,
100+ threads, 500+ open files - the limits are constants at the top of `health.cpp`). Its first line carries the time it was written: an old
time means olli is not running. `events.txt` records starts ("started again after a crash (...)"), problems appearing and clearing, and "olli
stopped normally" on a clean exit - after a crash that last line is simply missing.

It lives in `collection/` so the MCP bridge's file server (rooted there) lets olli's own chat and subcon read it through the read-only file
tools; no new tool was needed. RAG does not index it (RAG only imports folders created as collections), so rag_tool shows it as "a folder
with no matching collection". The code (`source/health.{h,cpp}`) reads only what the process can see about itself and writes only into that
folder; every failure inside it is swallowed there.

**Error counters** (added 2026-11-21): eight places in existing code report an error with one line, `olli_health::note(olli_health::ERROR_KIND::...)`
(an atomic increment - no lock, never throws): `tool_worker.cpp` (a call for a tool nothing offers; a call that got no answer in 300 s),
`ollama_system.cpp` (a failed request; the blanket `catch (...)` around a streamed chunk), `gemini_system.cpp` (a failed request, after its
retries), `ai_root.cpp` (the `catch (...)` around a whole chat turn), `io_worker.cpp` (voice input's callback threw), `sidetrack.cpp` (second-guess
hit its 10-round cap). `status.txt` gets an "Errors since this run started" line with the counts, a one-line event per kind appears in `events.txt`
when its count grows, and **Needs attention** is raised for any swallowed error, 3+ failed model requests, a tool-call timeout or a second-guess
cap hit (a "tool not available" on its own is only counted - it happens whenever a tool is stopped). Counts restart at 0 with every olli start.
Test: `source/tests/health_test.cpp` (command at the top of the file).

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
