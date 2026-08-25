# Remote tool template

A starting point for a new olli remote tool - see [`../PROTOCOL.md`](../PROTOCOL.md)
for the wire protocol this implements, and [`../clock/clock.cpp`](../clock/clock.cpp)
for a fuller worked example (a real ASCII-art clock built on this exact
plumbing).

## Layout

Two kinds of file in here, kept deliberately separate:

- **`olli_link.hpp` / `olli_link.cpp`** - the "talk to olli" plumbing:
  connect/reconnect, heartbeat, socket read/write, message framing. Generic,
  tool-agnostic - a new tool doesn't need to touch either file.
- **`template_tool.cpp`** - everything specific to what this tool actually
  does: what it registers, how it answers a call, and (if it has one) its
  display. This is the file you fill in.

Inside `template_tool.cpp`, `olli_processing()` is the one function that
ties the two together, called once per main-loop tick. Its own top half
(marked **CUSTOMIZE #2**) is where you route each message type to the logic
that answers it; its bottom half is just the calls into `OLLI_LINK` that do
the actual talking, unchanged from tool to tool.

## Making a new tool from this

1. Copy the whole directory, named for your tool:
   ```bash
   cp -r tools/template tools/your_tool_name
   cd tools/your_tool_name
   mv template_tool.cpp your_tool_name.cpp
   ```
   Leave `olli_link.hpp`/`olli_link.cpp` named as they are - they're generic
   plumbing, not tool-specific, so there's nothing in them to rename.
2. In `Makefile`, replace every `template_tool` with `your_tool_name` (the
   `.cpp` filename and the build target - `olli_link.cpp` stays as-is). Do
   the same in `.gitignore` (it ignores the compiled binary by name - a
   stale entry there just means the old, wrong binary name gets tracked by
   accident).
3. In `your_tool_name.cpp`, fill in the two spots marked **CUSTOMIZE**:
   - **CUSTOMIZE #1** - `make_register_message()` (what your tool is
     called, what it does - the `description` the model sees - and what
     arguments it takes, the `parameters` JSON-schema) and `handle_call()`
     (what actually happens when the model calls it, and what you send
     back via `link.send_result()`/`link.send_error()`).
   - **CUSTOMIZE #2** - `olli_processing()`'s dispatch: which message
     types you handle and where each one routes to. Only needs a second
     branch if your tool wants more than plain calls - e.g. `identity` (see
     `../clock/clock.cpp`'s `handle_identity()` for a worked example this
     template doesn't include yet).
4. `make`, then `./your_tool_name` - it doesn't need olli to already be
   running (see the reconnect behavior below).

Nothing else needs to change unless your tool genuinely needs different
*connection* behavior, not just different logic - everything in
`olli_link.hpp`/`olli_link.cpp`, and everything past CUSTOMIZE #2 inside
`olli_processing()`, is the same plumbing every remote tool needs.

## What you get for free

- **Connects to olli and stays connected** - retries every few seconds if
  olli isn't running yet, and drops back to retrying (without exiting) if
  olli goes away later. Your tool's own lifecycle is independent of olli's.
- **Heartbeat** - a ping/pong exchange during idle stretches so a *hung*
  (not just crashed) connection gets noticed and cleaned up on both sides.
- **`[host]` argument and `-h`/`--help`** - matching olli's own
  `[name]`/`--help` convention. Defaults to `127.0.0.1`; pass olli's real IP
  to reach it somewhere else on the network.
- **`link.send_event()`** - a ready-to-use `OLLI_LINK` method for the
  unsolicited push path (olli's `TOOL_REMOTE::monitor_tool()` on the
  receiving end) - call it whenever your tool has something to say without
  being asked, e.g. an alarm firing. Not called anywhere by default.
- **A live terminal display** - optional. The template's own is a one-line
  placeholder; see `tools/clock/clock.cpp`'s `render_big_clock()`/
  `redraw_screen()` for a fuller example (a big ASCII-art digit renderer).
  If your tool doesn't need a display at all (a background daemon, say),
  delete `RawTerminal`, `redraw_screen()`, the stdin-watching half of the
  `select()` call in `main()`, and the `redraw_screen()` call at the bottom
  of the loop - nothing else depends on any of it.

## Why a copy per tool, not a shared library

`olli_link.hpp`/`olli_link.cpp` split the plumbing out of the tool-specific
code *within* a tool's own directory, but there's still no shared library
*across* tools - copying this whole directory (all three files) is the
mechanism for starting a new one, and each tool keeps its own standalone
build (see `../PROTOCOL.md`'s "Repo / build layout" section). That's
deliberate: every tool stays fully self-contained, buildable and readable
on its own, with nothing to version or break across tools when one of them
changes. Worth revisiting only if keeping `olli_link.*` in sync by hand
across enough copies becomes a real maintenance burden - not a problem yet
with one tool on this layout (`../clock/` and `../presence/` are still the
older, single-file shape).
