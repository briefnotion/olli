# Hue remote tool

Philips Hue light control for olli, built on
[`../template/`](../template/template_tool.cpp)'s `OLLI_LINK` plumbing -
see [`../PROTOCOL.md`](../PROTOCOL.md) for the wire protocol. Ported out of
olli's core (formerly `TOOL_HUE`/`HUE_LIGHT_CLASS`,
`source/tools.cpp`/`.h`/`tools_helper.h`/`.cpp`) - see `hue.cpp`'s own
header comment for exactly what carried over unchanged and what didn't.

Registers three tools, same names/arguments the core version always used
(nothing else that references them, e.g.
[`../presence/presence.cpp`](../presence/presence.cpp)'s `on_home_action`/
`on_away_action`, needed to change):

- `set_hue_light` - on/off, brightness, color (hex or a named preset),
  alert/flash, transition time. `light_id: "all"` targets every light via
  Hue's group 0.
- `list_hue_lights` - current on/off/brightness/reachability for every
  light.
- `manage_hue_scenes` - save/load/remove/list named local scenes (snapshots
  of every light's current state).

## Setup

Needs a Hue bridge IP and an API key already paired against it (this tool
doesn't do the pairing/link-button flow itself - if you don't have a key
yet, generate one first: press the bridge's physical link button, then
`curl -X POST http://<bridge-ip>/api -d '{"devicetype":"olli"}'` within the
following ~30 seconds).

Credentials are per-profile, at `~/olli_files_<name>/hue_settings.json`
(or `~/olli_files/hue_settings.json` for the shared default) - same
`bridge_ip`/`api_key` fields, hand-editable. **First run for a profile
that doesn't have this file yet migrates it automatically** from that
profile's old `settings.json` (`tool_hue_lights_bridge_ip`/
`tool_hue_lights_apiKey`) if real values are already there from when Hue
support lived in olli's core - nothing to re-enter if you were already
using it. Scenes live at `~/olli_files_<name>/scenes.json`, same file/
format as before - no migration needed there at all.

## Build & run

```bash
cd tools/hue
make
./hue [host]
```

Needs libcurl (`-lcurl`) and pthreads (`-pthread`) - see `Makefile`; every
other remote tool so far has needed neither. Doesn't need olli already
running - see `../template/README.md`'s reconnect behavior, unchanged
here.
