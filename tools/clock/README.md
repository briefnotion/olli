# Clock remote tool

A standalone networked clock for olli, built on
[`../olli_link/`](../olli_link)'s connection plumbing and
[`../olli_display/`](../olli_display)'s terminal-drawing helpers - see
[`../PROTOCOL.md`](../PROTOCOL.md) for the wire protocol. Runs as a big
ASCII-art digital display (classic "tty-clock" style) in its own terminal,
independent of olli's lifecycle: it can start before olli does, keeps
ticking while disconnected, and reconnects automatically once olli is
reachable (see `clock.cpp`'s own top-of-file comment).

Registers five tools:

- `get_clock_time` - the current time, formatted with a `strftime` string
  (e.g. `%H:%M:%S`) if given, otherwise a sensible default.
- `set_timer` - starts a named countdown (`label`, `seconds`). Optionally
  takes a `reminder` (spoken in persona when it finishes - narration only)
  and/or `on_expire_tool`/`on_expire_arguments` (another registered tool,
  e.g. `set_hue_light`, actually executed the moment the timer finishes -
  a real action, not just a spoken notification). Timers keep running
  through a disconnect; only the eventual expiry alert waits for a live
  connection to send on.
- `check_timer` - whether a specific named timer has finished yet.
- `cancel_timer` - stops a specific named timer before it finishes, so it
  never goes off and any linked `on_expire_tool` never runs. Like a
  finished timer, a canceled one isn't deleted - it stays queryable via
  `check_timer` (reporting that it was canceled) rather than disappearing.
- `list_timers` - every known timer (running, finished, or canceled) and
  its current status, with no arguments needed.

A timer's label is matched case-insensitively, so "Pasta" and "pasta"
refer to the same timer - the label is still shown back exactly as it was
first given, just not required to match that casing on a later call.

## Using it

Ask olli for the time, or to set a named timer ("set a timer called pasta
for 10 minutes"), optionally with a spoken reminder or a real follow-up
action ("...and turn the kitchen light red when it's done"). Ask "is the
pasta timer done?" to check one directly. Every call re-queries the clock
rather than reusing anything said earlier in the conversation, so olli
never reports a stale time or timer state.

## Build & run

```bash
cd tools/clock
make
./clock          # connects to olli on this machine (127.0.0.1)
./clock <ip>      # connects to olli on another machine
./clock --help
```

Controls: `q` or Ctrl+C to quit (restores the terminal cleanly, and is
exactly what exercises olli's own disconnect handling on the far end).

Doesn't need olli already running - see [`../template/README.md`](../template/README.md)'s
reconnect behavior, unchanged here.
