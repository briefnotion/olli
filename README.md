# olli

**olli** is a fully local, offline voice assistant. A C++ core drives a
[Ollama](https://ollama.com) language model, calls real tools (clock, timers,
web search, Philips Hue lights, scripted automations), and talks to two Python
helper processes that give it *ears* and a *voice*. Nothing leaves your machine
except optional web searches and calls to your own Hue bridge.

```
        speech ──►  VOCA (ears, Python)  ──┐
                                           │  files in ~/olli_files/
   Hue lights ◄──                          ▼
   web search ◄──►   olli  (C++ core + Ollama)   ◄── keyboard
      timers  ◄──                          │
                                           │
        voice ◄──  LIRA (voice, Python) ◄──┘
```

---

## How it works

olli is three cooperating processes that communicate through JSON/text files in
`~/olli_files/`. There is no socket or shared memory between them — each side
polls or watches files, which keeps the pieces decoupled and independently
restartable.

| Process | Language | Role |
|---------|----------|------|
| **olli** | C++ | The "brain". Holds the conversation, calls the model over Ollama's HTTP API, runs tools, persists history, and coordinates the two helpers. |
| **VOCA** (`python/voca.py`) | Python | The "ears". Wake-word detection + speech-to-text with [faster-whisper](https://github.com/SYSTRAN/faster-whisper). Writes transcripts for olli to read. |
| **LIRA** (`python/lira.py`) | Python | The "voice". Watches for text olli wants spoken and renders it with a local TTS engine. |

### The shared folder (`~/olli_files/`)

Everything is coordinated through files here (created automatically on first run):

| Path | Direction | Purpose |
|------|-----------|---------|
| `input/*.txt` | VOCA → olli | Transcribed user speech, one file per utterance. olli consumes and deletes them. |
| `output/output.txt` | olli → LIRA | The next chunk of assistant text to speak. |
| `voca_status.json` | VOCA → olli | VOCA's state (`awake` / `busy` / timestamp). |
| `voca_command.json` | olli → VOCA | Commands to VOCA: `sleep`, `pause`, `listen`. |
| `lira_control.json` | LIRA ↔ olli | `is_speaking` flag (LIRA) and `interrupt` flag (olli). |
| `settings.json` | — | Your API keys and Hue bridge address (see [Configuration](#configuration)). |
| `history.json` | — | Persisted chat history, reloaded on start. |
| `history_debug.txt` | — | Human-readable dump of the current history. |
| `scenes.json` | — | Locally saved Hue light scenes. |

The audio coordination logic (in `audio_control.cpp`) uses these to keep the
microphone from hearing the assistant's own voice: when LIRA starts speaking it
`pause`s VOCA, and resumes listening once speech ends.

---

## Building the C++ core

### Dependencies

- A C++17 compiler (GCC or Clang) and **CMake ≥ 3.10**
- **libcurl** development headers — `sudo apt install libcurl4-openssl-dev`
- **[cpp-httplib](https://github.com/yhirose/cpp-httplib)** (header-only)
- **[nlohmann/json](https://github.com/nlohmann/json)** (header-only)

By default the build looks for the two header-only libraries as sibling
checkouts next to this repository:

```
code/
├── olli/            ← this repo
├── cpp-httplib/     ← git clone https://github.com/yhirose/cpp-httplib
└── json/            ← git clone https://github.com/nlohmann/json
```

If you keep them elsewhere (or installed system-wide), point CMake at them:

```bash
cmake -S source -B build \
      -DHTTPLIB_INCLUDE_DIR=/path/to/cpp-httplib \
      -DJSON_INCLUDE_DIR=/path/to/json/include
```

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

Then start the three processes (each in its own terminal):

```bash
# 1. the brain
./build/olli

# 2. the ears
python python/voca.py

# 3. the voice
python python/lira.py            # or: python python/lira.py pico2wave
```

VOCA and LIRA declare their Python dependencies inline (PEP 723), so if you use
[`uv`](https://github.com/astral-sh/uv) you can simply `uv run python/voca.py`.

You can also run olli on its own and just type — the voice halves are optional.

### Text-to-speech engines (for LIRA)

LIRA auto-detects the first available engine in this order: `pico2wave`,
`festival`, `espeak-ng`, `espeak`. Install at least one, plus a player:

```bash
sudo apt install libttspico-utils   # pico2wave — most natural, recommended
sudo apt install espeak-ng          # fast, robotic
sudo apt install festival           # classic
sudo apt install pulseaudio-utils   # paplay (preferred) / aplay
```

Pass a preferred engine as the first argument, e.g. `python python/lira.py espeak-ng`.

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
| `get_current_time` / `get_current_date` | Reads the system clock with a `strftime` format. |
| `set_timer` / `check_timer` | Named countdown timers with an optional follow-up action; olli announces expiry in-character. |
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

## Voice & keyboard commands

**Wake / sleep (spoken to VOCA):** say *"hey voca"* to wake it; *"stop
listening"* / *"sleep voca"* to sleep it; *"stop talking"* to interrupt speech.
VOCA also auto-sleeps after 5 minutes of silence.

**Jump phrases (typed or spoken)** trigger a scene macro directly, bypassing the
model's tool loop:

| Phrase | Effect |
|--------|--------|
| `I'm home.` / `I'm awake.` / `Lights on.` | Load the *repose* scene |
| `I'm leaving.` | Load the *labor* scene |
| `I'm sleeping.` / `Lights off.` | Load the *slumber* scene |

**Exit:** type `bye`, `quit`, or `Goodbye.` — history is saved on the way out.

---

## Source layout

```
source/
├── main.cpp / main.h          Entry point and the main event loop.
├── olla.{h,cpp}               ollama_system: chat engine, streaming, tool dispatch, all tools.
├── helper_olli.{h,cpp}        Settings, raw-mode keyboard input, VOCA/LIRA control interfaces.
├── audio_control.{h,cpp}      Background thread coordinating VOCA/LIRA via the shared files.
├── sidetrack.{h,cpp}          Background thread: history consolidation + post-turn "second guess".
├── tools_helper.{h,cpp}       HUE_LIGHT_CLASS, timers, task definitions, tool permissions.
├── stringthings.{h,cpp}       General-purpose string utility library.
├── fled_time.{h,cpp}          Timing / frame-pacing helpers used by the background threads.
├── threading.{h,cpp}          Thin std::async thread wrapper.
├── system.h                   Aggregates Settings + keyboard + audio into one object.
└── CMakeLists.txt             Build definition.

python/
├── voca.py                    VOCA — speech-to-text ("ears").
└── lira.py                    LIRA — text-to-speech ("voice").
```

### The background "sidetrack" thread

Two housekeeping routines run off the main thread (`sidetrack.cpp`):

1. **Consolidation** — when the conversation grows past a threshold, older
   messages are summarised into a higher "consolidation level", compressing
   history so long sessions stay within the model's context window.
2. **Second-guess** — after each turn, an "internal monologue" pass reviews the
   answer and, if it finds a genuinely useful addition, speaks a follow-up
   thought; otherwise it stays quiet.

---

## Notes & limitations

- olli targets **Linux** (raw-terminal input, `localtime_r`, PulseAudio/ALSA).
  The settings path has a Windows branch but the audio/input paths are POSIX.
- The three processes are independent — start them in any order; each recreates
  its files as needed.
- olli is a personal/experimental project; expect rough edges. Some source files
  carry commented-out experiments kept as design notes.

## License

MIT — see [LICENSE](LICENSE).
