# AI backends - Ollama (default) or Google Gemini

olli can be answered by a local Ollama model (the default, fully offline) or by
Google's Gemini API. Which one is a single setting, read once at startup.

## Choosing one

In the profile's `settings.json` (`~/olli_files_<profile>/settings.json`):

```json
{
    "ai_backend": "ollama"
}
```

| value | what answers | needs |
|---|---|---|
| `"ollama"` (default, and what a missing key means) | a local Ollama server, model `qwen3:8b` | Ollama running |
| `"gemini"` | Google's Gemini API, model `gemini-3.5-flash-lite` | a Gemini API key and internet |

An unrecognised value prints a warning and falls back to `ollama`. olli rewrites
`settings.json` when it exits, so **edit it while olli is stopped** or the edit is
overwritten. Changing the backend needs a restart.

### Gemini setup

1. Make an API key in Google AI Studio (aistudio.google.com) and make sure the
   Generative Language API is enabled for its project.
2. Save it, for you only, in the profile folder - never in the repo:
   `~/olli_files_<profile>/gemini_key.txt` (one line), or set `GEMINI_API_KEY`.
3. Set `"ai_backend": "gemini"` and start olli.

The main chat, sidetrack's review/consolidation passes and background task or
delegate instances all use the same backend. Subcon is a separate program with its
own Ollama client and is **not** affected.

### What leaves your machine with Gemini

Everything the model sees: the persona, the whole conversation history, tool
definitions and tool results (light names and states, presence, clock, notes and
RAG excerpts, web results). The Hue bridge address and key, and the SerpAPI key,
are used only inside their tools and are not put in the conversation. Only the
Gemini key itself goes to Google, as a request header. **On Google's free tier,
your prompts may be used to improve their products.** With `ollama`, nothing
leaves the machine except optional web searches and calls to your own Hue bridge.

## How it is built

```
ai_systems.h        shared data types: ToolCall, Message, Confidence, ChatResult, ...
ai_root.h/.cpp      ai_root: everything that doesn't depend on WHICH AI answers - history,
                    tool dispatch, result claiming, saving/loading, threads, and the head
                    and tail of send(). The rest of olli only ever holds an ai_root.
ollama_system.h/.cpp   backend: Ollama's /api/chat (NDJSON stream, logprobs, keep_alive, format)
gemini_system.h/.cpp   backend: Gemini's streamGenerateContent (SSE over libcurl)
ai_factory.h/.cpp   make_ai_system("ollama" | "gemini") -> the right backend
```

`ai_root::send()` is the same for every backend: shared head (reset state, record
the user's message), then one virtual call, `backend_generate()`, then shared tail
(record the reply, confidence, yes/no/maybe answer, debug log). A backend only
provides the middle.

### Adding another backend (OpenAI, Claude, ...)

1. Add `<name>_system.h/.cpp` deriving from `ai_root` and implement:
   - `backend_generate()` - build your API's request from `history` (under
     `history_mutex`) and the tool defs, make the call, stream text into
     `comms.INPUT_FROM_LLM` / thinking into `comms.INPUT_FROM_THINKING` while
     `PROPS.stream_output` / `stream_thinking` allow it, poll
     `status.interrupt_signal`, fill `last_received.tool_calls` / `.complete`
     (and `.response_tokens` if the API has token probabilities), and return the
     full reply and thinking text. The full contract is the comment above
     `backend_generate()` in `ai_root.h`.
   - `make_sibling()` - a fresh, empty instance of the same backend.
   - `unload_model()` - optional; the default does nothing.
2. Add the file to `source/CMakeLists.txt`.
3. Add one branch to `make_ai_system()` in `ai_factory.cpp`.

A backend must not contain history, consolidation, tool-dispatch or
result-claiming code. If it starts to, that code belongs in `ai_root`.
`ToolCall::signature` is opaque, backend-owned data that is saved in history and
echoed back unchanged (Gemini uses it for its `thoughtSignature`).

## How Ollama behaviour maps to Gemini

| olli / Ollama | Gemini |
|---|---|
| protected `system` messages (persona, review instructions) | `systemInstruction` |
| other `system` messages (DIRECTOR_NOTE, summaries) | `user` turns (no mid-conversation system role) |
| `assistant` / `user` | `model` / `user`; neighbouring same-role turns are merged |
| tool call + its `tool` result | `functionCall` in a model turn, `functionResponse` in the very next user turn. A call with no result yet is still sent with a placeholder answer ("no result has come back for this call yet - do not request it again"); a result whose call is gone is sent as text |
| tool schemas | `functionDeclarations`, only Gemini's schema subset; a tool with no parameters omits `parameters` |
| `think` on / off | `includeThoughts` / `thinkingLevel: minimal` |
| `response_format` (yes/no/maybe, DONE check) | `responseMimeType` + `responseSchema`; tools left out of those calls |
| streamed `thinking` / `content` | parts flagged `thought: true` / ordinary text parts |
| `num_ctx`, `repeat_penalty`, `keep_alive`, host/port | not used |
| logprobs / confidence | not available, so `Confidence` stays empty |

**Thought signatures.** Gemini 3 returns an opaque `thoughtSignature` with every tool
call and answers HTTP 400 if the call is sent back without it. It is kept on
`ToolCall::signature`, saved in `history.json` (only when set), and echoed back. A call
that never had one (old history, system-injected) gets Google's documented placeholder
`skip_thought_signature_validator`.

## Limits and known issues

- **One fixed model, `gemini-3.5-flash-lite`**, set in `gemini_system`'s constructor.
  Free-tier quotas (Cloud console, 2026-11-06): the full flash models allow 5 requests a
  minute and only **20 a day**; lite allows **15 a minute** and 500 a day. olli makes 2-3
  calls per message, so use lite. Model names change (`gemini-2.5-flash` was already closed
  to new accounts); edit the constructor when Google moves on.
- **Retries:** HTTP 429/500/503 and network errors are retried on the same model after 3 s,
  8 s and 15 s, only while nothing has been shown yet.
- **Streaming is choppier** than Ollama's: sentence-sized chunks, not word by word.
- **Second-guess loops on Gemini lite (open, deliberately left as is).** Seen live on `ron`
  (2026-11-07): the review said "needs correction" for 23 of 43 replies, mostly false alarms;
  the follow-up pass answers by calling a harmless tool (`get_clock_time`); that result
  triggers another review, and so on up to the chain limit (10). One question caused 7
  reviews in 20 seconds, which exhausts the 15 requests/minute free limit (HTTP 429) and
  sometimes leaks "(Took action on review: get_clock_time.)" into a reply. Options if it needs
  fixing: a setting that turns second-guess off for Gemini, don't re-review a follow-up that
  only ran a read-only tool, or a stricter review question. Not seen with Ollama.
- Settings fields that only apply to Ollama (`host`, `port`, `num_ctx`, `keep_alive_seconds`,
  `repeat_penalty`) still live in the shared `OLLAMA_SYSTEM_PROPERTIES` and are ignored by
  Gemini.

## Tested

- 2026-11-07, `ollama` on a scratch profile: streamed reply with confidence line, structured
  second-guess, built-in tool call, clean exit.
- 2026-11-07, `gemini` on a scratch profile and on `ron`: chat, clock/presence/Hue/RAG/subcon
  tools, structured second-guess, a `consult_expert` delegation (background instance via
  `make_sibling()`), clean exit, no crashes.
- Not tested: a task-runner or delegation script on the Ollama backend after the split;
  interrupting mid-reply on Gemini; consolidation on Gemini with a long history.
