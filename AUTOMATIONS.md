# Automations (Task Runner)

`run_automation_task` matches a spoken intent to a named script loaded from
disk. Scripts are plain `.task` files under a profile's own
`scripts/` directory (`~/olli_files_<name>/scripts/*.task`,
`TASK_SIMPLE_MANAGER::load_all_task` in `source/tools_helper.cpp`) - reloaded
fresh on every `run_automation_task` call, so adding, editing, or removing a
`.task` file takes effect immediately, no restart needed.

A `.task` file looks like:

```
NAME: system test
PURPOSE: This is a series of a few simple questions to check responses.
DIRECTORY: system_test
---
what time is it?
[ASK]What is a name for a dog?
[PAUSE]
Announce the system test is complete.
```

Three header lines (`NAME:`/`PURPOSE:`/`DIRECTORY:`), a `---` separator, then
one command per line, sent to a background instance in order. A line
starting with `#` is a comment - skipped entirely when the script loads, so
it never runs and never counts as a command. Special line-prefixes:
`[ASK]<text>` pauses and shows `<text>` as a request, then feeds whatever
the user types back in as the next line; `[PAUSE]` just waits for Enter, no
LLM involved; `[PRINT]<text>` displays `<text>` verbatim with no LLM call
and immediately continues (handy for several lines of narration back to
back, with nothing printed between them); `[FILE_IN:name]` reads `name` in
from the task's own files folder (see below) and feeds its contents in as
the next line, same as a typed line would be; `[FILE_APPEND:name]` appends
whatever the *previous* line's response was to `name` in that same folder
(creating it if needed) and continues immediately, no LLM call;
`[KEYBOARD_INPUT:on]`/`[KEYBOARD_INPUT:off]` and `[TTS_OUTPUT:on]`/
`[TTS_OUTPUT:off]` turn typed input and spoken responses on or off for the
rest of the script (or until toggled again) - handy for a run that
shouldn't be interrupted by a stray keystroke, or one that shouldn't talk
over itself. `[ASK]` and `[PAUSE]` each save whatever `[KEYBOARD_INPUT]` is
set to, force it on for themselves (they inherently need a keypress no
matter the current setting), then restore the saved value once done - so a
script only needs one `[KEYBOARD_INPUT:off]` near the top, not one around
every command that needs input. `[TTS_OUTPUT]` has no such auto-restore;
turn it back on explicitly wherever the script should start talking again.

A fourth, optional header line, `DELAY_TOOL_RETURNS: false`, controls when a
remote tool's answer (a clock, a light, anything not answered directly by
the model) actually gets narrated. It defaults to `true` - every such answer
is held until the whole script finishes, then reported all at once, right
before the final completion summary - so a background timer firing midway
through a long script doesn't interrupt whatever command happens to be
running with an out-of-context announcement. If a specific line's answer
matters immediately (e.g. "what time is it?" as an actual question the
script needs answered now, not minutes from now), follow that line with
`[WAIT_FOR_RESULT]` - it pauses the script until that line's own tool call
answers, narrates it right there, then continues; only that one command's
answer jumps the queue, everything else still waits for the end.
`[WAIT_FOR_RESULT]` only makes sense after a line that calls a *remote*
tool (clock/lights/etc.) - a plain question or a built-in tool (like
`web_search`) already answers inline, with nothing to wait for.

Every task gets its own folder under the profile's `files/` directory -
`~/olli_files_<name>/files/<dir>/`, where `<dir>` is the `.task` file's own
`DIRECTORY:` name if it set one, or its `NAME:` otherwise (either way, run
through a sanitizer that strips `/`/`\` and swaps spaces for underscores, so
it can never point outside that folder). `FILE_IN`/`FILE_APPEND`'s own
`name` argument gets the same treatment - both commands only ever read or
write flat filenames confined to that one folder, nothing else on disk is
reachable from a script. Unlike the task's other scratch directory (created
only when `DIRECTORY:` is set, and deleted once the script finishes), this
one is never cleaned up - anything a script writes there via `FILE_APPEND`
stays for as long as you want it to, across runs.

If `FILE_IN` can't find/read `name`, it doesn't abort the script - it falls
back to the same pause-and-prompt behavior as `[ASK]`, so you can paste the
content in by hand and the script continues from there.

Sample scripts (`system test`, `process resume`, `print test`, and an
original creative one, `fitter status`) live under `sample_scripts/` in this
repo - copy whichever ones you want into a profile's own `scripts/`
directory to use them.
