# Tools

olli can act in the world through two kinds of tools - both show up to the
model identically (same `tools` array in every request); the difference is
only in where the code that answers a call lives:

- **Internal tools** are compiled directly into olli's core
  (`source/tools.cpp`/`.h`), always available, nothing extra to run.
- **Remote tools** are standalone external programs that connect to olli
  over TCP at runtime - no recompiling or restarting olli required. See
  [Remote tools](#remote-tools) below for how those work and how to write
  your own.

There's no permissions/allowlist mechanism gating which tools are
available - every internal tool always is, and a remote tool is available
exactly when it's connected.

## Internal tools

| Tool | What it does |
|------|--------------|
| `web_search` / `fetch_website_content` | SerpAPI search and page-text extraction (via libcurl), cleaned for the model. Needs a SerpAPI key - see [Configuration](README.md#configuration). |
| `run_automation_task` | Runs a scripted, multi-step macro from a profile's `scripts/*.task` files. Full format in [AUTOMATIONS.md](AUTOMATIONS.md). |
| `set_thinking_mode` | Toggles the model's internal reasoning stream at runtime. |
| `remember_note` | Saves something the user asks Olli to remember (an appointment, a birthday, a fact) as one small text file in `<profile>/collection/Olli_Remembers/`. Save-only: nothing is scheduled or announced. The collection must be created once in `rag_admin` (name `Olli_Remembers`); `rag_tool`'s normal sync then makes the notes searchable within about 10 minutes, and subcon's "Notes and reminders" task reads them. Details below. |
| `quick_note` | Jots down a quick note exactly as the user said it ("make a quick note that the pin code is 2416") as one small text file in `<profile>/collection/Olli_Quick_Notes/`. Just the words and a save time - no title, context or date. Save-only. |
| `consult_expert` | Delegates a sub-question to a different persona, spawned as an isolated background sub-agent that answers and hands its result back. A specialist can itself call `consult_expert` (bounded by a nesting depth limit so it can't just loop asking itself the same thing). |

### remember_note

- **File:** `YYYY-MM-DD_HHMM_<slug>.txt`, with a `Remembered note:` title line, a `Saved:` time and weekday (stamped by code, not the model), an optional `About:` date and time, an optional `Context (olli's wording):` line, then the user's words exactly as given.
- **Context:** an optional one- or two-sentence explanation written by the model (who "he" is, a month the user left out), kept on its own labeled line apart from the user's words.
- **Dates are checked in code.** `about_date` must be a real `YYYY-MM-DD` date, and it is kept only if the user's own words contain the whole date (year, month and day) or a relative one (today, tomorrow, next Tuesday, in 3 days). Otherwise the date is dropped and the note is still saved without it; the result tells Olli so. A malformed date or time is an error and nothing is saved.
- **Known gap:** the second-guess pass can still call this tool and save an unrequested note. See `TODO.md` (2026-11-04).

### quick_note

- **File:** `YYYY-MM-DD_HHMM_<first words>.txt`, containing a `Quick note` line, a `Saved:` time and weekday (stamped by code, not the model), then the user's words exactly as given (minus the request itself, e.g. "make a quick note that").
- **No extras:** no title, `Context` line or `About:` date. Use `remember_note` for anything tied to a day or that should be remembered with context; the tool descriptions tell the model which is which.
- **Collection:** `Olli_Quick_Notes` is a normal RAG collection folder - create the collection in `rag_admin` (as for `Olli_Remembers`); rag_tool's sync then makes the notes searchable within about 10 minutes. Subcon's notes task does not search it.
- **Shared saving code:** the file writing is one small function, `save_simple_note()` in `source/tools.cpp`, meant to be reused by any later tool that only saves text to a collection folder (a journal tool is planned). `remember_note` keeps its own, unchanged.
- **Known gap:** the second-guess pass can still call this tool and save an unrequested note, same as `remember_note`.

## Remote tools

Each remote tool is its own independent process with its own build and its
own lifecycle: it connects to olli's remote-tool listener, registers
whatever it wants to expose exactly the way an internal tool would
(`TOOL_REMOTE`, `source/remote_tools.h`/`.cpp`, never knows or cares what
program it's proxying for), and can run before olli even starts, keep going
if olli isn't reachable, and reconnect automatically once it is. All of
this - listening for new connections, polling each one, matching calls to
results - runs on `TOOL_WORKER_CLASS`'s own background thread
(`source/tool_worker.h`/`.cpp`), independent of whatever the main chat
thread is doing. Full wire protocol in [`tools/PROTOCOL.md`](tools/PROTOCOL.md).

olli listens for these on **port 47601**, loopback-only for now (see
`tools/PROTOCOL.md`'s Scope section).

| Tool | What it does | Docs |
|------|---------------|------|
| Clock | Current time, named countdown timers with optional spoken reminders or real follow-up actions. | [tools/clock/README.md](tools/clock/README.md) |
| Hue lights | On/off, brightness, colour, alerts/flashes, saved local scenes. | [tools/hue/README.md](tools/hue/README.md) |
| Presence | Bluetooth-based home/away detection for one or more phones. | [tools/presence/README.md](tools/presence/README.md) |
| RAG / knowledge base | Search notes, imported documentation, or past conversations mid-chat. | [tools/rag/README.md](tools/rag/README.md) |
| Mail | Watches one Gmail inbox (read-only, hourly) and runs a standing rule you set - tell you, run another tool such as blinking the lights, or nothing - when new mail arrives. | [tools/mail/README.md](tools/mail/README.md) |
| Subcon | Olli's background "subconscious": works through recurring checks (weather, past conversations, notes, lights, timers) with its own LLM and keeps worthwhile findings. Ask olli "any updates?" to hear them. Unlike the others it also acts as a small server, so the other tools hold a second connection to it. | [tools/subcon/README.md](tools/subcon/README.md) |

### Try the example

[`tools/clock/`](tools/clock) is a full worked example - a big ASCII-art
digital clock running in its own terminal that registers `get_clock_time`,
`set_timer`, `check_timer`, `cancel_timer`, and `list_timers` with olli:

```bash
cd tools/clock
make
./clock          # connects to olli on this machine (127.0.0.1)
```

### Write your own

Start from [`tools/template/`](tools/template) rather than from scratch —
copy the directory, fill in the two spots marked `CUSTOMIZE #1` (what your
tool is called and what it does), and everything else — connecting,
registering, heartbeat, automatic reconnect — is already there, working.
See [`tools/template/README.md`](tools/template/README.md) for the exact
steps.
