# Hue remote tool

Philips Hue light control for olli, built on
[`../template/`](../template/template_tool.cpp)'s `OLLI_LINK` plumbing -
see [`../PROTOCOL.md`](../PROTOCOL.md) for the wire protocol. Ported out of
olli's core (formerly `TOOL_HUE`/`HUE_LIGHT_CLASS`,
`source/tools.cpp`/`.h`/`tools_helper.h`/`.cpp`) - see `hue.cpp`'s own
header comment for exactly what carried over unchanged and what didn't.

Registers four tools. The first three keep the names and arguments the core
version always used (nothing that references them, e.g.
[`../presence/presence.cpp`](../presence/presence.cpp)'s `on_home_action`/
`on_away_action`, needed to change); everything added since is optional.

- `set_hue_light` - on/off, brightness, color (hex or a named preset),
  alert/flash, transition time. `light_id: "all"` targets every light via
  Hue's group 0. Added 2026-10-30:
  - **Several lights**: `light_id` can be `"Computer, Sink"`, `"Computer and
    Sink"` or a list; `"all"` with `except: "Sofa"` means everything but the
    sofa. An unknown name is an error that lists the real lights, and
    nothing is sent. A light whose own name contains "and" still works.
  - **Relative brightness**: `brightness_change` in percentage points (`-20` =
    a bit dimmer). Applied per light, and lights that are off are left alone
    and reported (a bare relative change never switches a light on). Absolute
    `brightness` wins if both are given.
  - **True whites**: `white` (candle 2000K, warm 2700K, neutral 3500K, cool
    4500K, daylight 6500K) or an exact `kelvin` (2000-6500). A color or preset
    wins if both are given. A white-only lamp that can't do it is reported as
    a failure in plain words ("this light does not support color temperature
    ..."), also when the bridge accepted the other fields of the same request.
  - **Fade**: `transition_ms` as before (`0` = instant, for "right now"); if it
    is left out the saved default fade applies, so "turn on all the lights"
    can fade in over 10 seconds. A bare alert/flash is never faded. The reply
    says which fade was used ("Applied: ..., fade 10.0 s (saved default)").
- `set_hue_default_fade` - `seconds` (0 = instant) or `reset: true` (back to
  the bridge's own quick ~0.4 s). Saved as `default_transition_ms` in
  `hue_settings.json` (`-1` = not set; hand-editable too) and also used when a
  saved scene is loaded. Applies to presence's home/away light actions as well.
- `list_hue_lights` - every light with on/off, brightness, color (nearest
  name and hex) or warm/cool white (kelvin), effect/alert, whether it is
  reachable, and its type (so white-only lamps are visible); then which
  **saved scene the lights match right now** and which scene olli **last
  loaded** ("changed since" when the lights no longer match), the default
  fade, and the saved scene names. `detail: "full"` adds light models and
  firmware, the bridge's name/versions, its rooms and groups, and the names
  of the Hue app's own scenes (which one is *active* isn't available from
  the v1 API). Read-only, so subcon and anything else can use it for the
  fuller picture; `set_hue_default_fade` is not offered to subcon.
- `manage_hue_scenes` - save/load/remove/list named local scenes (snapshots
  of every light's current state). A scene's off lights are sent just
  `on:false` (the bridge rejects brightness/color for a light that is already
  off, which used to make loading an all-off scene look like a partial
  failure). Loading a scene records it in `hue_state.json` (next to
  `scenes.json`, whose format is unchanged).

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
