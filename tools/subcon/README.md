# subcon - olli's background "subconscious", as a standalone tool

Subcon works through a short list of recurring tasks while you aren't talking
to olli (check the weather, review past conversations, look at your notes,
check the lights and timers), decides whether anything it found is actually
worth telling you, and keeps those findings in a queue. Olli **pulls** them
when it asks: the model calls the `get_subcon_notes` tool, and subcon answers
instantly with one digest of whatever is waiting.

It used to be a thread inside olli (`source/subcon_worker.cpp`). The design
and rules - the five tasks, the run caps, the finding gate - are unchanged and
described in [`SUBCON.md`](../../SUBCON.md); this document covers what is new:
it is now its own program, and it is both a **remote tool of olli** and a
**small server for the other tools**.

```
                    port 47601                      port 47603
   olli  <----  [ OLLI_LINK ]  subcon  [ tool server ]  <----  clock / hue / presence / rag
          get_subcon_notes                   |         (each holds a second OLLI_LINK)
                                             +--> Ollama (/api/chat), the web
```

- **As a tool of olli**: one `OLLI_LINK` to olli's port (47601), registering
  one tool, `get_subcon_notes`. The answer is built in code from the persisted
  queue (`reasoner.cpp`, `pull_notes()`) - no model is ever used for delivery,
  so olli is never kept waiting. Everything included is marked delivered. The
  result carries a `special_instruction` so olli relays the facts faithfully in
  its own voice (the same wording idea as the old announcement).
- **As a server for tools**: listens on 127.0.0.1:47603 and speaks olli's own
  protocol ([`../PROTOCOL.md`](../PROTOCOL.md), "Two servers"). The other
  tools register their tools with it, and subcon calls them during think
  cycles (`rag_search`, `list_hue_lights`, `check_presence`, `list_timers`,
  ...). The presence tool's `user.presence` events arriving there drive the
  "arrived home" tracking.
  A tool that sends a message with a wrongly-typed field (a number where text
  belongs) is logged and disconnected; it can't take subcon down.

## Building and running

```bash
cd tools/subcon
make                      # needs libcurl dev files, the json sibling checkout, and the ncursesw build the other tools use
./subcon                  # olli on this machine; learns the profile from olli's identity message
./subcon --help
```

It does not need olli to be running first (it keeps retrying, like every tool),
and runs the same on a terminal (small ncurses status screen via
`OLLI_DISPLAY`) or headless (`nohup ./subcon > subcon.out &`, a tmux pane
with output redirected): when stdout is not a terminal the display is not used
and log lines go to stdout instead. `q` or Ctrl+C / SIGTERM quits.

Start order does not matter; the tools' second links retry every few seconds
until subcon is up. Only one subcon can run (the second fails to bind 47603 and
exits; a second `get_subcon_notes` registration is also refused by the
single-instance lock in `olli_link`). Independently of the ports, one state
folder has one subcon: it takes an exclusive `flock` on
`<profile folder>/subcon/subcon.lock` and holds it for as long as it runs. A
second subcon pointed at the same folder (other ports, or a re-pinned profile)
logs `refusing to run: ... is locked by another subcon (pid N)` and exits with
status 1 before reading or writing anything there. The lock is taken again
whenever the profile changes mid-run (olli switching profiles); if the new
profile's folder is already owned by another subcon, this one exits the same
way. The kernel drops the lock when the process dies, so it can't go stale.

### Command line

| Option | Meaning |
|---|---|
| `[host]` | where olli is (default `127.0.0.1`) |
| `--olli-port N` / `--listen-port N` | 47601 / 47603; for tests on other ports |
| `--profile NAME` / `--dir PATH` | pin the profile (`~/olli_files_NAME/`, or this folder) instead of waiting for olli's identity |
| `--user-name NAME` | the name tools are told in `identity` (default: `--profile`'s, else olli's own message) |
| `--model`, `--ollama-host`, `--ollama-port` | override the settings file; defaults are olli's (`qwen3:8b`, `localhost:11434`) |
| `--no-think-cycle` | the old `SUBCON_THINK_CYCLE_ENABLED = false`: no new cycles, queued notes still pullable |
| `--force-run` | the old `SUBCON_FORCE_RUN_FOR_TESTING`: ignore the model's "nothing worth doing", run the stalest allowed task |
| `--test-ignore-limits` | **test only**: ignore per-task caps and minimum gaps (so a soak isn't throttled to a handful of cycles) |
| `--first-cycle-delay S` | seconds before the first cycle (default 60) |

Think cycle **on** and force **off** are the defaults.

### Files

Everything lives under `<profile folder>/subcon/` - for `ron`,
`~/olli_files_ron/subcon/`, **the same folder and the same formats the in-olli
subcon used**, so an existing profile's history and run caps carry over
unchanged. (Which also means the old in-olli subcon and this tool must not both
be running against one profile - they would both write these files. The old one
lives inside olli and does not take the `subcon.lock` described above, so it
**must be retired first**: stop olli's own subcon from running before this tool
is started on that profile.)

| File | Contents |
|---|---|
| `todo_items.json` | each task's state: last checked, recent run times, last 20 results |
| `queue.json` | findings waiting to be told, plus a record of delivered/expired ones (pruned after 14 days - new; the old queue never was) |
| `todo_items_retired.json` | tasks removed from the definitions, with their history |
| `*.corrupt` | a file that failed to parse is moved aside, not overwritten |
| `subcon.lock` | the single-instance lock (holds the owner's pid); safe to delete only while no subcon is running |
| `subcon_log.txt` | plain-text record of cycles: what was chosen, every tool call and result, findings, the model's thinking (rotated at 5 MB to `.old`) |

Writes go to a temp file and are renamed into place, so a crash can't leave a
half-written file.

Settings (optional, never created for you): `<profile folder>/subcon_settings.json`,
same per-tool convention as `hue_settings.json`:

```json
{
  "ollama_host": "localhost", "ollama_port": 11434, "model": "qwen3:8b",
  "num_ctx": 8192, "repeat_penalty": 1.3, "keep_alive_seconds": -1, "think": true,
  "web_search_api_key": "...",
  "max_tool_rounds": 6, "call_timeout_seconds": 30, "rethink_seconds": 60
}
```

Every key is optional. `web_search_api_key` falls back to the profile's own
`settings.json` (`tool_web_search_apiKey`, where olli keeps the serpapi key),
so an existing profile works with no setup. Command-line options win over the file.

## How a cycle works

Same stages as `SUBCON.md`: **prioritize** (structured output: run one now, or
"ask me again in N minutes", clamped 1-60) -> **act** -> **finding**
(structured: `noteworthy` / `urgency` / `summary`; only `noteworthy: true`
reaches the queue). The first think cycle starts a minute after startup; after
each cycle (or failure) the next is a minute later unless the model asked to wait.

The act stage is now a standard Ollama tool loop that this tool owns: send the
conversation and the tools array, run **every** tool call in the reply (remote
ones through the server, internal ones on the worker thread), append the
`role: tool` results, ask again - until a reply has no tool calls (at most 6
rounds). A remote call that is never answered times out (30 s) with an error
result; so a stuck tool can't hang a cycle. (While a call is in flight and
within that 30 s, the connection is not dropped for missing heartbeats - the
tools are single-threaded and can't answer pings while busy running one; a tool
that hangs outright is dropped when the call times out.)

Two caps keep a cycle inside the model's context window (`num_ctx`, 8192 by
default), found in review 2026-10-29: **at most 4 tool calls are executed per
model reply** (any further ones are answered with the error "too many tool
calls in one step; call at most 4." so every call still has its reply), and
**about 12000 bytes of tool results per cycle** (each result is also cut to
6000). Once that budget is used up further results are replaced by "result
omitted: size budget used up." and the cycle goes straight on to its finding.
The budget is a constant in `reasoner.cpp`, sized for the default `num_ctx`:
if you lower `num_ctx` in the settings file, lower it too.

**Tasks wait for their tools.** Each task names the tools it cannot do
without (`required_tools` in `subcon_task_defs()`: weather needs `web_search`,
conversations and notes need `rag_search`, lights need `check_presence`,
timers need `list_timers`). A task whose tools aren't currently available
- the same set the model would be offered, so a connected-but-write-only or
disconnected tool doesn't count - is not listed in the prioritize prompt, does
not use up any of its allowance, and is not recorded as a run. The log says
which tools are missing, once per change. If every task that is otherwise
allowed is waiting for a tool, the cycle does nothing (no model call) and looks
again a minute later. The requirement lives only in code, not in
`todo_items.json`, so existing state files load unchanged. If no real tool ran, or every
call failed, there is nothing true to report and the cycle ends without a
finding. A model that writes a fake `<tool_response>` itself is told once to
make the real call and, if it does it again, the cycle is discarded.

**Tools offered.** Whatever the connected tools registered plus subcon's own
internal `web_search`, `fetch_website_content` and `get_clock_time` (copied from
`source/tools.cpp`, not shared - `tools/` never shares headers with `source/`) -
**filtered to a read-only allowlist** (`reasoner.cpp`, `is_read_only_tool()`).
Write tools (`set_hue_light`, `manage_hue_scenes`, `set_timer`,
`cancel_timer`, `set_presence_action`...) are never shown to the model, and a
call to anything not offered is refused, logged `[SAFETY]`, and that cycle's
finding discarded. The allowlist already matched the tools' actual registered
names; nothing needed adding.

## How the old in-olli subcon maps to this

| Old (`source/`) | Now |
|---|---|
| thread in olli, own `ollama_system`, tool_worker | its own process; `JOB_WORKER` thread (model + web calls only), `SUBCON_TOOL_SERVER` for tools |
| prioritize / PLANNING / RESULTING / DELIVERING / ANNOUNCING stages | prioritize / act (tool loop) / finding; **no delivery stages** |
| `exchange()` carrying an announcement to `main.cpp`, then `integrate_tool_result()` | olli calls `get_subcon_notes`; the digest is the tool result |
| delivery gate: must be home, system idle, quiet hours 22:00-07:00, model decides when, 4 h hold limit | **none** (decided 2026-10-29: "no gating for now"). Notes still expire after 18 h and are ordered urgent-first. A busy signal belongs in `SUBCON_REASONER::may_start_cycle()` (a commented, empty seam) |
| `main_busy_level < 10` gate on starting a cycle | same seam; always true for now |
| presence from `COMMS::user.presence` (olli) | the presence tool's `user.presence` events on its link to subcon, **plus** a `check_presence` call at startup when presence is still unknown (the old one failed closed forever after a restart) |
| full tool access, audited after the fact | read-only allowlist filtered up front, audit kept as a second line |
| `outstanding_tool_call_ids`, "wait for a second call arriving inside narration" | structural: the next request is never sent until every call of the previous reply has an answer |
| 4000-byte fetch truncation could split a UTF-8 character | fixed in this copy (`truncate_utf8()`) |
| `SUBCON_THINK_CYCLE_ENABLED`, `SUBCON_FORCE_RUN_FOR_TESTING` constants | `--no-think-cycle`, `--force-run` |

The task list, caps, minimum gaps, 24-hour-or-since-arrival window, finding
gate and prompts are ported unchanged (`subcon_tasks.h` is a copy of
`source/subcon_tasks.h`, still header-only and unit-testable; the delivery gate,
quiet hours and hold rules were removed from the copy because nothing uses them).
To add, reword or retire a task, edit `subcon_task_defs()` and rebuild; on the
next start saved state is merged by `key` and retired tasks go to
`todo_items_retired.json`.

## Threads and state

Everything runs on the main thread (one `select()` loop over olli's link, the
tool connections and a wake pipe) except `JOB_WORKER`, which runs one blocking
job at a time: an Ollama request or a batch of internal tool calls. The handoff
is by value, there is no shared state, and the only cross-thread flags are two
atomics that tell libcurl to give up - see the comment at the top of
`job_worker.hpp`. Shutdown aborts any call in flight and joins the thread.

## Not done / known limits

- Ollama serves one request at a time; a subcon call in flight makes olli's own
  reply wait for it (SUBCON.md, "Known limitations"). With no busy signal yet,
  nothing prevents a cycle starting while you are talking to olli.
- `get_subcon_notes` marks notes delivered when it answers; if olli's narration
  of them then fails they are not offered again.
