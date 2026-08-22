# Remote tool template

A starting point for a new olli remote tool - see [`../PROTOCOL.md`](../PROTOCOL.md)
for the wire protocol this implements, and [`../clock/clock.cpp`](../clock/clock.cpp)
for a fuller worked example (a real ASCII-art clock built on this exact
plumbing).

## Making a new tool from this

1. Copy the whole directory, named for your tool:
   ```bash
   cp -r tools/template tools/your_tool_name
   cd tools/your_tool_name
   mv template_tool.cpp your_tool_name.cpp
   ```
2. In `Makefile`, replace every `template_tool` with `your_tool_name` (the
   `.cpp` filename and the build target). Do the same in `.gitignore` (it
   ignores the compiled binary by name - a stale entry there just means the
   old, wrong binary name gets tracked by accident).
3. In `your_tool_name.cpp`, fill in the two spots marked **CUSTOMIZE #1**:
   - `make_register_message()` - what your tool is called, what it does
     (the `description` the model sees), and what arguments it takes (the
     `parameters` JSON-schema).
   - `handle_call()` - what actually happens when the model calls it, and
     what you send back.
4. `make`, then `./your_tool_name` - it doesn't need olli to already be
   running (see the reconnect behavior below).

Nothing else needs to change unless your tool genuinely needs different
*connection* behavior, not just different logic - everything past
"End of CUSTOMIZE #1" in the file is the same plumbing every remote tool
needs.

## What you get for free

- **Connects to olli and stays connected** - retries every few seconds if
  olli isn't running yet, and drops back to retrying (without exiting) if
  olli goes away later. Your tool's own lifecycle is independent of olli's.
- **Heartbeat** - a ping/pong exchange during idle stretches so a *hung*
  (not just crashed) connection gets noticed and cleaned up on both sides.
- **`[host]` argument and `-h`/`--help`** - matching olli's own
  `[name]`/`--help` convention. Defaults to `127.0.0.1`; pass olli's real IP
  to reach it somewhere else on the network.
- **`send_event()`** - a ready-to-use helper for the unsolicited push path
  (olli's `TOOL_REMOTE::monitor_tool()` on the receiving end) - call it
  whenever your tool has something to say without being asked, e.g. an
  alarm firing. Not called anywhere by default.
- **A live terminal display** - optional. The template's own is a one-line
  placeholder; see `tools/clock/clock.cpp`'s `render_big_clock()`/
  `redraw_screen()` for a fuller example (a big ASCII-art digit renderer).
  If your tool doesn't need a display at all (a background daemon, say),
  delete `RawTerminal`, `redraw_screen()`, the stdin-watching half of the
  `select()` call in `main()`, and the `redraw_screen()` call at the bottom
  of the loop - nothing else depends on any of it.

## Why one file each, not a shared library

Every remote tool is a single, standalone `.cpp` file with its own build -
no shared library between them yet (see `../PROTOCOL.md`'s "Repo / build
layout" section). That's deliberate: each tool stays fully self-contained
and readable end to end, and copying this template is the whole mechanism
for starting a new one. Worth revisiting only if the duplicated plumbing
becomes a real maintenance burden across enough tools - not a problem yet
with two.
