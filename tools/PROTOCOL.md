# Remote tool protocol

Lets a standalone program (anywhere on the network, localhost for now)
register itself as an olli tool at runtime, without olli ever having
tool-specific code for it. On olli's side this is handled by one general
`TOOL_REMOTE` class (one instance per connection) implementing the same
`TOOL_BASE` interface (`configure`/`register_tool`/`check`/`monitor_tool`,
see `source/tools.h`) as every hardcoded tool - `TOOL_REMOTE` never knows
what program it's proxying for, it just relays whatever JSON that program
registered.

**Writing a new remote tool?** Start from
[`tools/template/`](template/template_tool.cpp) - copy the directory, fill
in the spots marked `CUSTOMIZE`, and all the connection/registration/
heartbeat/reconnect plumbing described below (shared by every tool via
[`tools/olli_link/olli_link.hpp`/`olli_link.cpp`](olli_link/olli_link.hpp))
is already handled. See [`tools/template/README.md`](template/README.md)
for the exact steps, and [`tools/clock/clock.cpp`](clock/clock.cpp) for a
fuller worked example (a real ASCII-art clock) of the tool-specific logic
itself - timers and identity handling.

Status (2026-08-22): All 7 steps of the original plan are done and confirmed
working end-to-end against a real olli session - see git history for
exactly what changed at each step. Summary of what's there:

- **Steps 1-2** - `REMOTE_TOOL_LISTENER` (`source/remote_tools.h`/`.cpp`)
  accepts connections non-blocking, once per `process()` tick.
  `tools/clock/clock.cpp` proves the protocol from the far end.
- **Step 3** - `REMOTE_TOOL_LISTENER::poll()` parses a completed `register`
  handshake and hands off `{fd, tools}`; `main.cpp` wraps that in a
  `TOOL_REMOTE` and calls `ollama_system::register_remote_tool()`.
  `tools_list`'s `register_tool()` calls moved from `open()` (once, at
  startup) to `send()` (every request), so a remote tool joining mid-session
  shows up on the very next request instead of never. Not permission-gated
  (see the class comment in remote_tools.h for why).
- **Step 4** - `TOOL_REMOTE::check()` forwards `{type: call, call_id, name,
  arguments}` and blocks (5s timeout) for the matching `{type: result,
  call_id, result|error}` - `read_line_blocking()` uses `select()`, and
  keeps a persistent `read_buffer` so bytes beyond the current line aren't
  dropped. Along the way, fixed a real latent bug: writing to a closed
  socket raises `SIGPIPE`, whose default disposition kills the whole
  process - `main()` now ignores it globally (`std::signal(SIGPIPE,
  SIG_IGN)`).
- **Step 5** - `TOOL_REMOTE::monitor_tool()` polls (non-blocking) for an
  unsolicited `{type: event, message}` line and feeds it into
  `chat.integrate_tool_result()`, same as `TOOL_TIMER`'s expired-timer
  alert. Known limitation: if an `event` arrives at the exact moment
  `check()` is blocked waiting for a `result`, it gets consumed as the
  (mismatched) result and treated as a protocol error instead of reaching
  `monitor_tool()` - not live for `clock.cpp` (never pushes mid-call), but
  real for anything that might.
- **Step 6** - `TOOL_BASE` gained a fifth method, `is_alive()` - unlike the
  other four, a default (non-pure) virtual (`return true`), since every
  hardcoded tool's answer is permanently true; only `TOOL_REMOTE` overrides
  it. `ollama_system::process()` checks it every tick right after
  `monitor_tool()` and erases anything dead from `tools_list`, logged as
  `[RemoteTools] Removed N disconnected tool(s)`. A failed write or a timed-
  out call both call `mark_dead()` too, not just a cleanly closed
  connection.
- **Step 7** - `tools/clock/clock.cpp` has a real display: a big ASCII-art
  digital clock (block-character digits, "tty-clock" style) plus the date,
  redrawn ~5x/sec via cursor repositioning, not a full clear each frame.
  `'q'`/Ctrl+C quits cleanly via a `RawTerminal` RAII guard. The stub's
  canned test event (used to prove the push path during Steps 5-6) has
  since been removed - `clock.cpp` doesn't fire anything unprompted anymore.

Remaining open items from the steps above: the event/call interleaving gap
(Step 4/5), and the fact remote tools only register with the main chat
instance, not background task-runner/jump instances (the Scope section
below).

### Heartbeat + reconnect (in progress, 2026-08-22)

Added after the 7 original steps, to answer two related gaps: neither side
had a way to notice a *hung* (not crashed - a cleanly closed socket is
already detected) peer during an otherwise idle stretch, and `clock.cpp`
could only ever be started *after* olli was already listening, with no way
to recover if olli restarted.

- New message types: `ping`/`pong` (see Message shapes below). Both sides
  track time-since-last-received (any message counts, not just these two)
  and time-since-last-sent; idle 5s -> send a ping, nothing received for
  15s -> presume dead (`TOOL_REMOTE::mark_dead()` on olli's side).
- `clock.cpp`'s `main()` is now a persistent outer loop rather than
  "connect once or exit": not connected -> retry every 3s while still
  redrawing the display -> connected -> normal operation, heartbeats
  included -> back to "not connected, retrying" the moment a read fails or
  the dead-timeout trips. The display never blocks on any connection state.
- Worth being explicit about what this does and doesn't add: on loopback, a
  *crashed* peer (killed process) gets its socket closed by the OS almost
  immediately regardless of a heartbeat - the real value here is catching a
  *hung*, not-yet-crashed peer (which a clean-close event never would), and
  it's what makes a genuinely remote (not just localhost) deployment
  viable later, per the Scope note about "anywhere on the network, localhost
  for now."

### Identity broadcast (2026-08-24)

- New message type: `identity` (see Message shapes below), olli -> tool,
  sent once right after registration completes - `main.cpp` calls
  `TOOL_REMOTE::send_identity()` (`source/remote_tools.h`/`.cpp`)
  immediately after constructing the `TOOL_REMOTE`, using `CLASS_SYSTEM`'s
  new `USER_IDENTITY` (`source/helper_olli.h`/`system.h`) - see the
  architecture discussion this came out of for why that struct exists and
  lives there.
- Not tied to any lifecycle beyond registration itself: a reconnect
  re-registers from scratch (see the heartbeat/reconnect note above), so it
  naturally lands here again with no separate trigger needed. There's no
  "user changed mid-session" case to handle either - the active profile is
  fixed for olli's whole process lifetime (chosen once at startup, see
  `main.cpp`'s `[name]` argument), so `identity` is genuinely sent-once per
  connection, not something that can go stale while still connected.
- Deliberately sends the actual identity fields over the wire (`name`/
  `full_name`/`about`), not just enough for the tool to go read
  `~/olli_files_<name>/` itself - that directory is on *olli's* machine,
  and per the Scope note above this protocol targets "anywhere on the
  network" as the long-term goal, not just loopback. A same-machine tool
  that wants to keep real per-user settings on disk is still free to use
  `name` as a lookup key into its own directory convention (see
  `tools/clock/clock.cpp`'s `handle_identity()` for a commented-out sketch
  of exactly that) - the wire message just doesn't *require* filesystem
  co-location to be useful.
- `tools/clock/clock.cpp` is the reference implementation: `handle_identity()`
  records the fields and returns a status string for the display (same
  convention as `handle_call()`); `reset_to_default_profile()`, called from
  both places the connection drops (a clean close and a heartbeat timeout),
  clears them back out so a stale identity from whoever was just connected
  never lingers for whoever (or nothing) connects next. Clock itself has no
  real per-user settings to load or revert, so both functions carry a
  commented-out sketch of what a tool that *does* have some (a Hue-lights
  remote tool's per-user scenes, say) would do there instead - copy the
  shape, not the emptiness, when building the next one.

### Remote host support (2026-08-22)

`clock.cpp` now takes an optional `[host]` argument (default `127.0.0.1`)
and `-h`/`--help`, mirroring olli's own `[name]`/`--help` convention in
`main.cpp`. This exposed a real gap the loopback-only testing so far hadn't:
a plain blocking `connect()` to an *unreachable* remote address (wrong IP,
firewalled, host down with packets silently dropped) can hang for the
platform's full TCP timeout (commonly 20-30+ seconds) - fine on loopback,
where a refusal is near-instant, but would have frozen the whole display
once a real remote host was in play. `try_connect()` now does a non-blocking
connect with a `CONNECT_TIMEOUT_SECONDS` (2s) cap via `select()` + checking
`SO_ERROR`, same shape as every other socket wait in this codebase. Traffic
volume itself is unaffected by host - during idle silence it's still
~32 bytes every 5 seconds (the ping/pong pair) regardless of whether that's
staying on loopback or actually crossing a network; the only behavior
difference for a real remote target is that reconnect attempts now fail
within a bounded 2s instead of either near-instant (loopback) or
platform-timeout-slow (remote, previously).

## Scope (for this first pass)

A remote tool registers with olli's **main chat instance only** - not
background task-runner instances, not jump-phrase instances. Each of those
builds its own `tools_list` via its own `open()` call; teaching them about a
live socket registry is a bigger coordination question this first pass
doesn't need to answer. Revisit if a real need for it shows up.

## Transport

- Plain TCP. Olli listens on port **47601** ("olli's remote-tool port" -
  distinct from Ollama's own 11434, which olli is a *client* to, not a
  server on).
- The listening socket's only job is `accept()`ing new connections, checked
  non-blocking once per `ollama_system::process()` tick - same tick-based
  poll shape already used for Voca transcripts, TTS state, and the
  response/thinking/log output buffers, not a blocking `accept()` call that
  could stall the whole program.
- The moment a connection is accepted, it becomes a separate, persistent,
  two-way socket dedicated to that one program - the listening socket goes
  straight back to waiting for the next connection, unaffected. A second
  remote tool connecting later gets its own independent channel; neither
  program's traffic touches the other's.
- One JSON object per line (newline-delimited JSON) in both directions.
  Chosen over length-prefixed framing for simplicity - easy to read/write by
  hand (`nc localhost 47601`), no binary header to get right.

## Message shapes

Six message types, distinguished by `"type"`.

### `register` (tool -> olli, sent once, right after connecting)

```json
{"type": "register", "tools": [
    {
        "name": "get_clock_time",
        "description": "Returns the current time from a networked clock.",
        "parameters": {
            "type": "object",
            "properties": {
                "format": {"type": "string", "description": "strftime format string"}
            },
            "required": ["format"]
        }
    }
]}
```

- `tools` is an array, not a single object, so one program can register more
  than one callable name (mirrors `TOOL_GET_CURRENT_TIME` registering both
  `get_current_time` and `get_current_date` from one class).
- Each entry needs both `description` (prose, for the model to know *when*
  to reach for it) and `parameters` (a JSON-schema, for the model to know
  *how* to call it - what arguments to construct). Prose alone isn't enough;
  this is the same shape every hardcoded tool's `register_tool()` already
  builds via `add_tool()` (see `source/tools.cpp`), just supplied externally
  instead of written in C++.
- On olli's side, `TOOL_REMOTE::register_tool()` does nothing but re-emit
  this array into the `tools` JSON sent to Ollama - no tool-specific code.

### `identity` (olli -> tool, sent once, right after registration completes)

```json
{"type": "identity", "name": "ron", "full_name": "", "about": ""}
```

- Who's running olli right now - `name` mirrors the profile name that
  selected `~/olli_files_<name>/` (empty for the shared/no-profile default),
  `full_name`/`about` are whatever's set on `CLASS_SYSTEM`'s `USER_IDENTITY`
  (`source/helper_olli.h`), also empty if unset. All three are always
  present in the message, even when empty - a tool can rely on the keys
  existing rather than needing `.value()`-style fallbacks for missing ones.
- One-way, not a `call` - there's no matching `result` to send back.
- See the "Identity broadcast" status note above for the full design
  reasoning (why it's sent fields-and-all instead of just enough to look up
  a local file, why there's no "identity changed" case to handle, etc.) and
  `tools/clock/clock.cpp`'s `handle_identity()`/`reset_to_default_profile()`
  for the reference implementation.

### `call` (olli -> tool, when the model invokes one of its registered names)

```json
{"type": "call", "call_id": "abc123", "name": "get_clock_time", "arguments": {"format": "%H:%M:%S"}}
```

- `call_id` echoes the tool_call id Ollama assigned (`ToolCall::id`,
  `source/olla.h`), so the eventual `result` can be matched back to the
  right pending call.

### `result` (tool -> olli, in response to a `call`)

```json
{"type": "result", "call_id": "abc123", "result": "14:32:07"}
```

or, on failure:

```json
{"type": "result", "call_id": "abc123", "error": "clock hardware not responding"}
```

- `result` is a plain string, matching how every hardcoded tool already
  works - `handle_tool()` hands `send_tool_result()` a plain formatted
  string, never JSON (see the "JSON in, plain string out of the tool"
  discussion this spec came out of). `TOOL_REMOTE` doesn't change that
  convention, it just receives the string over a socket instead of computing
  it in-process.

### `event` (tool -> olli, unsolicited - not a response to any `call`)

```json
{"type": "event", "message": "Alarm: wake up!"}
```

- `TOOL_REMOTE::monitor_tool()` polls its socket each tick; if an `event`
  line is waiting, `message` gets forwarded into `chat.integrate_tool_result()`
  unprompted, asking the model to narrate/acknowledge it in persona - e.g.
  `tools/clock/clock.cpp`'s `set_timer` noticing an expired timer.

- Optional `action` field - a real tool call for olli to execute itself,
  separate from (and in addition to) `message`'s narration:

  ```json
  {"type": "event", "message": "...", "action": {"tool": "set_hue_light", "arguments": {"light_id": "all", "on": false}}}
  ```

  `action.tool`/`action.arguments` are handed to the exact same dispatch
  `ollama_system::handle_instance_tools()` uses for a call the model issued
  itself (matched against `tools_list` by name) - so this works for *any*
  registered tool, built-in or remote, with no tool-specific code anywhere
  in this path. Queued via `pending_tool_calls` (`source/olla.h`) rather
  than injected into the model's own `last_received.tool_calls`, since the
  latter gets reset at the top of every `send()` call and could silently
  drop a queued action if a new turn started first.

  This exists because `message`/narration alone was never reliable for
  "when the timer goes off, actually do X" - it depended on the model
  correctly inferring and re-issuing a real action from a text hint at the
  right moment, with less context than it had when the timer was first set.
  `set_timer`'s `on_expire_tool`/`on_expire_arguments` params (see
  `tools/clock/clock.cpp`) are what let the model pre-author this action
  once, up front, instead. Still subject to the same
  `PROPS.max_tool_calls_per_turn` cap every tool call is (see olla.h) - one
  action barely dents that budget, and one uniform rule for every
  execution, regardless of origin, is simpler to reason about than a
  separate exemption.

### `ping` / `pong` (either direction - heartbeat, see the status note above)

```json
{"type": "ping"}
```
```json
{"type": "pong"}
```

- Sent by whichever side has gone 5s without sending anything else. The
  other side replies with `pong` immediately on receiving a `ping` - either
  message counts as proof of life either way, so a side that's actively
  sending calls/results/events never needs to also send pings; this only
  fires during an otherwise-idle stretch.
- No fields beyond `type` - there's nothing else either side needs to say.

## Repo / build layout

- `tools/<name>/` per remote tool (e.g. `tools/clock/`), each with its own
  build (own `CMakeLists.txt` or plain Makefile) - never folded into the
  main `olli` CMake target. Keeps each tool's build/versioning independent
  of olli's own (much heavier) build, even though the source lives in the
  same repo for now. Splitting a subdirectory out into its own repo later,
  if one ever needs a genuinely separate release cadence, is a clean move
  from here - not a decision this locks in.
- Shared `tools/olli_link/` library (2026-09-01) - the connect/reconnect/
  heartbeat/framing plumbing (`OLLI_LINK`, originally split out within
  `tools/template/` on 2026-08-25) now lives once, at `tools/`-level, not
  copied per tool. Every tool's `Makefile` points at it via
  `-I../olli_link` and builds `../olli_link/olli_link.cpp` alongside its
  own `.cpp` - see `tools/template/README.md`'s "Layout" section for the
  boundary between this shared file and each tool's own tailored logic
  (what it registers, how it answers a call). `tools/clock/` and
  `tools/presence/` used to hand-roll the identical plumbing inline before
  this - keeping that many copies in sync by hand is exactly what forced
  the consolidation; `tools/clock/clock.cpp` is still the fullest worked
  example of the *tool-specific* logic (timers, identity handling), just no
  longer of the connection plumbing itself. Each tool's build otherwise
  stays fully self-contained - own `Makefile`, own binary - only this one
  piece is shared.
