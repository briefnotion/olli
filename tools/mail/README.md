# Mail - watch one Gmail inbox and run a standing rule

A remote tool for olli ([`../PROTOCOL.md`](../PROTOCOL.md)). It checks one Gmail
inbox, **read-only**, and does one thing when new mail arrives: whatever the user
last told it to do. First version, deliberately small.

## What you can say to olli

One callable tool, `set_mail_action`:

| You say | What happens |
|---|---|
| "when I get new email, tell me *You have mail!*" | olli says it, in character, when new mail arrives |
| "when mail comes in, blink the lights" | olli runs a prepared `set_hue_light` call |
| "when I get mail, blink the lights and tell me" | both |
| "stop telling me about email" / "do nothing" | the rule is cleared |
| "what happens when I get mail?" | olli reads the current rule back |

The rule is **standing**: it stays in force, across restarts, until you change or
clear it (a timer fires once; this does not). Setting a new rule replaces the old
one.

The action is chosen when you set the rule, in the same shape as
`set_timer`'s `on_expire_tool`/`on_expire_arguments`. An email can trigger it but
can never choose or change it.

## What it looks at, and what it never does

Once an hour (`--poll-seconds`) it sends Gmail **one** IMAP command,
`STATUS INBOX (UIDNEXT UIDVALIDITY)`, and compares the answer with last time. "New
mail" means the inbox's next-UID number went up. That is all it ever asks:

- it never opens (selects) the mailbox, never fetches a message, and never sees a
  sender, subject or body, so the announcement cannot contain any of them;
- it has no command that sends, moves, deletes or marks anything;
- TLS only, to `imap.gmail.com:993`, certificate and host name checked; the host
  cannot be changed (the only override is a loopback test port used by the tests);
- an answer larger than 64 KB is abandoned; connection and total timeouts apply;
- the check runs on its own thread, so a slow Gmail cannot stall the link to olli.

If olli is not reachable when mail arrives, the event waits and goes out once
olli is back (once, not repeatedly). A wrong or revoked password is not retried for
an hour, so a bad login cannot turn into a lockout.

## Setup

1. Google account -> Security -> turn on **2-Step Verification**, then create an
   **App password** (Google shows it as four groups of four letters).
2. Start the tool once. It creates `<profile folder>/mail/credentials.json`
   (for `ron`: `~/olli_files_ron/mail/credentials.json`) with empty fields.
3. Fill in the Gmail address and the app password:
   ```json
   {"user": "you@gmail.com", "app_password": "abcd efgh ijkl mnop"}
   ```
4. The file must be readable by you only, or the tool refuses to use it:
   `chmod 600 ~/olli_files_ron/mail/credentials.json`

Until that is done, olli's answer to `set_mail_action` ends with a note saying so.
The first successful check sets a baseline and fires nothing.

## Files (`<profile folder>/mail/`, all owner-only)

| File | Contents |
|---|---|
| `credentials.json` | Gmail address + app password |
| `state.json` | last seen UIDNEXT/UIDVALIDITY, and whether an event is still waiting to go out |
| `rule.json` | the standing rule (absent = do nothing) |

## Build and run

```bash
cd tools/mail
make
./mail              # olli on this machine; [host] for another one
./mail --poll-seconds 600
```

It is installed with the other tools by `build/install_to_home.sh`. Like the other
tools it connects to olli (47601) and to subcon's server (47603), and does not need
either to be running first.

## Not done yet

- Reading what the mail *is* (sender, subject, a snippet) for subcon to judge
  "personal or important". That needs a second, read-only command that fetches
  headers only, and a decision on what may be spoken aloud.
- Tested against a fake IMAP server only. **Not yet run against real Gmail**: the
  one Gmail-specific assumption is that it answers `STATUS INBOX` the standard way.
- Push instead of polling (IMAP IDLE). Gmail's published limit is bandwidth
  (2500 MB/day for IMAP downloads); an hourly STATUS is a few hundred bytes.
