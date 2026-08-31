# Presence sensor

A home/away detector for olli - see [`presence.cpp`](presence.cpp)'s own
top-of-file comment and [`helper_presence.hpp`](helper_presence.hpp)'s for
the full design (why classic Bluetooth instead of BLE scanning, why two
independent backends, how the adaptive polling rate works), and
[`../PROTOCOL.md`](../PROTOCOL.md) for the wire protocol.

## One-time machine setup

Both of these are outside this program's control - Linux/BlueZ permission
model, not a bug here:

1. **Unblock Bluetooth if it's soft-blocked:**
   ```bash
   rfkill unblock bluetooth
   ```
   Check first with `rfkill list bluetooth`.

2. **Pair each phone once** (classic Bluetooth, not BLE - this is what
   gives it a stable, non-randomized MAC address to ping):
   ```bash
   bluetoothctl
   scan on
   # wait for the phone to show up, then Ctrl-C the scan
   pair <mac>
   # the phone will prompt to confirm the pairing code
   trust <mac>
   exit
   ```
   Put that MAC into that person's `bluetooth_mac` in this tool's settings
   (see below).

3. **Give `l2ping` permission to open a raw Bluetooth socket**, without
   needing to run this whole program as root:
   ```bash
   sudo setcap cap_net_raw+eip $(which l2ping)
   ```
   Without this, every Bluetooth check silently reads as "away" (see
   `check_bluetooth_present()`'s comment - a permission failure and a real
   absence aren't distinguished, matching every other best-effort check in
   this file).

4. **Find each phone's Wi-Fi IP** on your home network - check your
   router's DHCP client list, or reserve one for it there so it doesn't
   change. Put it into that person's `wifi_ip` in settings.

## Settings

Per-profile, at `~/olli_files_<name>/presence_settings.json` (or
`~/olli_files/presence_settings.json` for the shared default) - written out
with an empty `people` list the first time this tool sees a given profile,
so just run it once, then edit the file, or configure everything from
olli's chat instead (see "Talking to it through olli" below) once at least
one person's `bluetooth_mac`/`wifi_ip` is filled in by hand.

A profile tracks a *list* of people - useful for a household where more
than one person's phone should be watched independently. Each has their own
identity and their own independent near/away actions:

```json
{
    "people": [
        {
            "name": "ron",
            "bluetooth_mac": "F0:1F:C7:8C:9C:0B",
            "wifi_ip": "192.168.18.10",
            "on_home_action": {"tool": "manage_hue_scenes", "arguments": {"action": "load", "name": "home"}},
            "on_away_action": {"tool": "manage_hue_scenes", "arguments": {"action": "load", "name": "slumber"}}
        },
        {
            "name": "gus",
            "bluetooth_mac": "",
            "wifi_ip": "",
            "on_home_action": {},
            "on_away_action": {}
        }
    ]
}
```

| Field | Meaning |
|---|---|
| `people[].name` | Who this entry is - what shows up in `check_presence`, `get_presence_setup`, and fired "just got home"/"just left" messages, and what `set_presence_action`'s/`register_presence_person`'s `person`/`name` argument matches against. |
| `people[].bluetooth_mac` | That person's phone's classic Bluetooth MAC (step 2 above). |
| `people[].wifi_ip` | That person's phone's home-network IP (step 4 above). Can go stale if iOS's "Rotate Wi-Fi Address" (Settings > Wi-Fi > (i) > Private Wi-Fi Address) is on for your home network and reassigns a new one - either turn that off for this network, reserve the IP for the phone's MAC in your router's DHCP settings, or just fix this field by hand if it ever actually happens. |
| `people[].on_home_action` / `people[].on_away_action` | `{"tool": "...", "arguments": {...}}` - a real registered olli tool call fired on that person's arrival/departure, e.g. `{"tool": "manage_hue_scenes", "arguments": {"action": "load", "name": "repose"}}`. Leave as `{}` for narration only, no action. |

An older single-person settings file (a top-level `bluetooth_mac`/`wifi_ip`/
`on_home_action`/`on_away_action` instead of a `people` list) is migrated
automatically the first time this tool loads it - it becomes a single
`people` entry named after the profile itself, with the original MAC/IP and
actions intact, and the file is rewritten in the new schema right away.

## How detection works

Each person is tracked by two independent backends - Bluetooth (`l2ping`
against their phone's paired MAC) and Wi-Fi (ARP/neighbor-table lookup for
their IP) - see [`helper_presence.hpp`](helper_presence.hpp)'s own comment
for the full design. A person reads as near if *either* backend's most
recent check found them; a single hit is always trusted instantly.

The poll rate adapts rather than staying fixed:

- **Near and settled:** checks every 2 minutes - no urgency, they're here.
- **Away** (however that came to be): checks every 10 seconds - no reason
  to be slow about noticing a return.
- **Near, but just missed a check:** a single miss doesn't immediately
  flip someone away. It starts a 10-second-interval "searching" window for
  up to 2 minutes, actively trying to catch a hit before giving up - a hit
  anywhere in that window cancels the search and goes straight back to
  near. Only once the full 2 minutes passes with no hit does it actually
  declare them away.

The live display (each person's Bluetooth/Wi-Fi line) shows `(searching)`
next to a backend while it's in that confirmation window.

## Talking to it through olli

Four tools are registered once connected:

| Tool | Does |
|---|---|
| `check_presence` | Live near/away reading for every tracked person. |
| `get_presence_setup` | Who's configured and what their near/away actions are - not the live reading. |
| `set_presence_action` | Configures an existing person's `on_home_action`/`on_away_action` from a plain-language request, e.g. "when ron gets home, load the repose scene". |
| `register_presence_person` | Adds a brand new person by name, with an optional `bluetooth_mac`/`wifi_ip` if you already have them handy - otherwise they're added but read as permanently away until one is filled in by hand (the model has no way to know a phone's MAC/IP on its own). |

## Build & run

```bash
make
./presence [host]   # host defaults to 127.0.0.1
./presence --help
```
