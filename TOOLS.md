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
| `consult_expert` | Delegates a sub-question to a different persona, spawned as an isolated background sub-agent that answers and hands its result back. A specialist can itself call `consult_expert` (bounded by a nesting depth limit so it can't just loop asking itself the same thing). |

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
