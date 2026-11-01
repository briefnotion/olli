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

`[ECHO_INPUT:on]`/`[ECHO_INPUT:off]` controls the `INPUT: <line>` echo (and the
dashed line above it) that a script prints for every line it sends to the
model, `[IF]` questions included. On by default. Turn it off for a game or any
script where the player should see only the model's replies, not the
instructions that produced them. It also stops that echo being spoken. It does
not touch `[PRINT]` text, `[ASK]` requests, or the model's own reply.

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

## Branching: `[IF]`, `[ELSE]`, `[MAYBE]`, `[ENDIF]`

```
[IF]Is the sky blue on a clear day?
[PRINT]The model said yes.
[ELSE]
[PRINT]The model said no.
[MAYBE]
[PRINT]The model wasn't sure.
[ENDIF]
```

`[IF]<question>` asks the model the question and waits for a one-word answer:
yes, no, or maybe. The lines after `[IF]` are the **yes** branch, `[ELSE]` starts
the **no** branch, and `[MAYBE]` starts the **maybe** branch. `[ELSE]` and
`[MAYBE]` are both optional and can come in either order - a branch that isn't
there is simply skipped, and the script carries on after `[ENDIF]`. Only one
branch runs. `[IF]` blocks can be nested inside any branch.

The question is judged by the model with what it already knows from earlier
lines of the same script (so "Was that answer correct?" refers to the line
before it). The answer is not shown on screen (it used to print `ANSWER: yes`); the
raw one-word reply is not streamed or spoken either, and once the branch is chosen the
question and its answer are taken back out of the script's history, so later
lines don't copy them and the report the main chat gets at the end doesn't list
them. (A side effect: a `[FILE_APPEND]` right after an `[IF]` would append the
raw answer, not the line before it.)

**An `[IF]` right after an `[ASK]` judges what the player typed.**

```
[ASK]What do you do?
[IF]Answer yes if the player went inside the lighthouse. Answer no if ... Answer maybe only if ...
React to what the player just did, in one sentence.
[GOSUB:hall]
[ELSE]
...
[ENDIF]
```

In that case the typed line is *not* sent to the narrator as its own turn. The
player's words are put into the model's history (with no reply), the `[IF]`
question is asked right behind them (with the player's words repeated at the top
of the question, which made short answers like "climb the stairs" judged
correctly far more often), and when the answer is in, the question and its
answer are removed again. So the narrator knows what the player said but has
not reacted to it yet. A branch that carries the story on can start with a
plain line like "React to what the player just did, in one sentence."; a branch
that is an ending just narrates the ending. The same applies after a
`[FILE_IN]` that fell back to asking. An `[ASK]` followed by anything other than
an `[IF]` works as before: the typed line goes to the model and it replies.

Anything else an `[IF]` follows (a plain line, `[PRINT]`, a sub call) still
judges what the narrator just said, as described above.

`[END]` finishes the whole script on the spot, from anywhere, including inside
an `[IF]` branch (so an ending doesn't have to be the last thing in its block).
The script's normal wrap-up still runs afterwards, as if it had run out of
lines.

Every `[IF]` needs an `[ENDIF]`. If one is missing, the script stops with a
`SCRIPT ERROR` line when it reaches that `[IF]` (not at load time, so lines
before it have already run). Branches only ever jump forward, so a script can't
loop.

The model rarely says "maybe" on its own for a question with a plain factual
answer. Word the question to allow it, e.g. "... Answer maybe if experts
disagree."

## Subroutines: `[SUB]`, `[ENDSUB]`, `[GOSUB]`, `[RETURN]`

```
[SUB:door]
[ASK]Type a word:
[IF]Answer yes if the player typed the word open. Answer no if ... Answer maybe only if ...
[PRINT]OPENED
[ELSE]
[PRINT]LOCKED
[MAYBE]
[PRINT]What? Try again.
[GOSUB:door]
[ENDIF]
[ENDSUB]

[GOSUB:door]
[PRINT]main done
```

`[SUB:name]` ... `[ENDSUB]` defines a block of lines that only runs when
something calls it with `[GOSUB:name]`. When the script simply runs into a
`[SUB]`, it skips to after the `[ENDSUB]`, so definitions can sit anywhere at
the top level (usually at the top). `[GOSUB:name]` jumps into the sub and
remembers where it came from; the sub's `[ENDSUB]`, or an early `[RETURN]`,
goes back to the line after the `[GOSUB]`. `[END]` inside a sub still ends the
whole script.

A sub can call other subs, and itself - that is how a room asks its question
again on a "maybe". Subs can't take arguments (there are no variables), can't
be defined inside another sub, and an `[IF]` can't run past the end of the sub
it is in. At most 16 calls can be open at once; a sub that calls itself forever
stops the script with a `SCRIPT ERROR` line. A `[GOSUB]` to a name with no
`[SUB]`, a `[RETURN]`/`[ENDSUB]` with nothing to return to, and a `[SUB]` with
no `[ENDSUB]` are all `SCRIPT ERROR`s, reported when the script reaches them.

After a sub calls itself again and that second attempt finishes, control comes
back through the first attempt too and carries on from the line after its
`[GOSUB]`. Put an `[END]` or `[RETURN]` after a retry call if the lines below
it shouldn't run twice.

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

Sample scripts (`system test`, `process resume`, `print test`, `branch test`, `lighthouse adventure`, `haunted lighthouse`, and an
original creative one, `fitter status`) live under `sample_scripts/` in this
repo - copy whichever ones you want into a profile's own `scripts/`
directory to use them.

`lighthouse_adventure.task` is a small playable example: a text adventure three
choices deep with seven endings, built from `[ASK]` and nested `[IF]` blocks.
Its comments explain how to write `[IF]` questions that a small model answers
reliably (spell out what yes, no and maybe each mean, and keep the question to
the player narrow).

`haunted_lighthouse.task` is the same story rebuilt with subroutines: one `[SUB]`
per room and per ending, no deep nesting, a "maybe" at the door or the stairs
asks again by calling itself, and every ending finishes with `[END]`. It has five
endings (the "maybe" endings of the first version became retries). Keep the two
names unlike each other if you install both - with `lighthouse adventure` and
`lighthouse adventure 2` the model dropped the "2" and ran the wrong script.

One more thing learned from writing it: ask `[IF]` questions in the positive.
"Did the player go down into the cellar?" was answered correctly every time;
"Did the player go up the stairs?" (with "no" meaning the cellar) was not.
