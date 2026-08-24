# TODO

## Setup automation for a new user/machine (not done - ran out of time)

Getting presence working today is entirely manual - see README.md's
"One-time machine setup" section for the actual steps (unblock Bluetooth,
pair the phone, grant `l2ping` raw-socket capability, find the phone's
Wi-Fi IP). None of it is detected, checked, or prompted for by
`presence.cpp` itself - a machine that's missing a prerequisite just looks
identical to "phone is genuinely away" forever, with nothing on screen
explaining why.

The idea, not yet built: notice the common unfinished-setup states at
startup (or right after `handle_identity()` loads a profile's settings)
and say something useful on screen instead of silently failing checks:

- **Bluetooth radio soft-blocked.** Detectable via `rfkill list bluetooth`
  (shell out and parse, same idea as the check functions already do for
  `l2ping`/`ip neigh`) - print the exact unblock command from README step 1
  if it's blocked, rather than every Bluetooth check just reading "away."
- **`l2ping` missing `cap_net_raw`.** Every `check_bluetooth_present()`
  call currently just silently reads "not present" on a permission
  failure, identical to a real absence - see that function's own comment
  in `presence.cpp`. Detectable ahead of time (attempt one real check at
  startup and look for the specific "Operation not permitted" stderr text,
  or check the capability directly via `getcap`) and worth surfacing loudly
  - this was the single most confusing part of setup this session, and
  we had a real terminal open to debug it interactively; a future user
  won't.
- **`bluetooth_mac` / `wifi_ip` left empty.** `load_settings()` already
  writes the placeholder-defaults file on first run, but never tells
  anyone it did, or that it's now sitting there waiting on real values -
  right now that's only discoverable by opening the settings file directly.

None of this needs to be *fully* automated - actually running
`sudo rfkill unblock`/`sudo setcap` from presence itself is a bigger, more
sensitive step (needs its own separate design/authorization conversation,
not something to sneak in as a side effect of a startup check). Even just
*detecting* these conditions and printing clear guidance in the same place
`status`/`conn_status` already render would meaningfully shorten setup for
whoever's next - the gap is diagnosis, not remediation.

## Known rough edges from real-world testing (2026-08-24)

- **Pairing an iPhone couldn't be done by having this machine scan and
  find the phone** - iOS doesn't reliably answer generic inbound classic-
  Bluetooth inquiry the way the phone's own "Other Devices" scan (looking
  for accessories) does. The workaround that worked: make this machine
  discoverable (`bluetoothctl discoverable on` + `pairable on`) so it shows
  up in the *phone's* own scan list, and pair from the phone's side
  instead. Worth teaching a setup-automation pass to try the first
  approach, notice it's not working, and suggest the second.
- **`wifi_ip` can go stale** if iOS's "Rotate Wi-Fi Address" is on for the
  home network - see README.md's note on that field. mDNS (resolving a
  `.local` hostname fresh each check instead of a fixed IP) was tried as a
  fix and didn't pan out - iOS doesn't reliably answer plain hostname
  queries either, same restrictive-privacy pattern as the Bluetooth
  discovery issue above. No automated recovery exists; the field just
  needs fixing by hand if it ever actually happens (accepted tradeoff, see
  the design discussion this came out of).
- **Real observed latency numbers**, useful context for tuning
  `poll_interval_seconds`/timeouts later: a real `l2ping` hit against a
  locked, idle, paired phone measured ~20-130ms standalone, but ~1-2s
  during actual poll-loop operation (each poll re-establishes the
  Bluetooth connection from scratch, since nothing holds it open between
  checks) - and a real miss (Bluetooth off) measured ~5s despite a 2s `-t`
  flag, which is why `check_bluetooth_present()`/`check_wifi_present()`
  wrap their subprocess calls in a hard `timeout` on top of the tool's own
  flag, not just the flag alone.
