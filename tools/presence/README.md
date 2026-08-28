# Presence sensor

A home/away detector for olli - see [`presence.cpp`](presence.cpp)'s own
top-of-file comment for the full design (why classic Bluetooth instead of
BLE scanning, why two independent backends, how debouncing/agreement work),
and [`../PROTOCOL.md`](../PROTOCOL.md) for the wire protocol.

## One-time machine setup

Both of these are outside this program's control - Linux/BlueZ permission
model, not a bug here:

1. **Unblock Bluetooth if it's soft-blocked:**
   ```bash
   rfkill unblock bluetooth
   ```
   Check first with `rfkill list bluetooth`.

2. **Pair your phone once** (classic Bluetooth, not BLE - this is what
   gives it a stable, non-randomized MAC address to ping):
   ```bash
   bluetoothctl
   scan on
   # wait for your phone to show up, then Ctrl-C the scan
   pair <mac>
   # your phone will prompt to confirm the pairing code
   trust <mac>
   exit
   ```
   Put that same MAC into `bluetooth_mac` in this tool's settings (see
   below).

3. **Give `l2ping` permission to open a raw Bluetooth socket**, without
   needing to run this whole program as root:
   ```bash
   sudo setcap cap_net_raw+eip $(which l2ping)
   ```
   Without this, every Bluetooth check silently reads as "away" (see
   `check_bluetooth_present()`'s comment - a permission failure and a real
   absence aren't distinguished, matching every other best-effort check in
   this file).

4. **Find your phone's Wi-Fi IP** on your home network - check your
   router's DHCP client list, or reserve one for it there so it doesn't
   change. Put it into `wifi_ip` in settings.

## Settings

Per-profile, at `~/olli_files_<name>/presence_settings.json` (or
`~/olli_files/presence_settings.json` for the shared default) - written out
with placeholder defaults the first time this tool sees a given profile, so
just run it once, then edit the file:

| Field | Meaning |
|---|---|
| `bluetooth_mac` | Your phone's classic Bluetooth MAC (step 2 above). |
| `wifi_ip` | Your phone's home-network IP (step 4 above). Can go stale if iOS's "Rotate Wi-Fi Address" (Settings > Wi-Fi > (i) > Private Wi-Fi Address) is on for your home network and reassigns a new one - either turn that off for this network, reserve the IP for the phone's MAC in your router's DHCP settings, or just fix this field by hand if it ever actually happens. `detection_mode: "both"` means a stale IP just shows as Wi-Fi disagreeing rather than a false trigger - see below. |
| `poll_interval_seconds` | How often both backends get checked. |
| `detection_mode` | `"both"` (default - requires agreement, see above), `"bluetooth"`, or `"wifi"` - the latter two trust that one backend alone, ignoring the other for triggering purposes (it still runs and shows up in the display/log either way). Switch to one of these once you've decided, via `--test`, that a single backend is reliable enough for your situation. |
| `away_debounce_misses` | Consecutive misses one backend needs before *its own* state flips to AWAY. Applies to both backends the same way. There's no equivalent setting for HOME: both backends flip to HOME on a single hit, no debounce - a real response (an l2ping echo, a live ARP entry) can't be a false positive, so waiting for a second one only delays noticing a genuine arrival (real-world testing showed this stalling arrivals for a while whenever a backend flapped hit/miss/hit even while genuinely in range). A miss is the ambiguous case - could be interference, could be a phone that's actually gone - so that side still waits for `away_debounce_misses` consecutive misses before this backend's own state flips, and (per `detection_mode: "both"` above) both backends have to independently reach AWAY before presence overall is considered AWAY. |
| `on_home_action` / `on_away_action` | `{"tool": "...", "arguments": {...}}` - a real registered olli tool call fired on arrival/departure, e.g. `{"tool": "manage_hue_scenes", "arguments": {"action": "load", "name": "repose"}}`. Leave as `{}` for narration only, no action. |

## Recommended first run: `--test`

```bash
./presence --test
```

Runs both backends the same as normal operation, but only logs and
displays what they see - never fires `on_home_action`/`on_away_action`.
Walk away from the house and back, then read
`presence_test_log.txt` (same directory as the settings file above) to see
which backend actually tracked reality, and how much lag each had. Once
you trust the result, drop `--test` for real operation - both backends
still have to agree before anything fires either way (see the file-level
comment in `presence.cpp`).

## Build & run

```bash
make
./presence [host] [--test]   # host defaults to 127.0.0.1
./presence --help
```
