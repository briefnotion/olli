# TODO

Guiding principle for all of the below: keep code simple, easy to read, and
well compartmentalized. That's the bar for new work here, not just a nice-to-have.

## New tools

- **Text file tool** - let olli create, modify, and delete simple text files
  (e.g. notes), presumably scoped to a dedicated directory under
  `~/olli_files/` rather than anywhere on disk.
- **Tool-help tool** - a way to ask olli what tools it has and what they do,
  loading/displaying that info at runtime. Could be built on top of the text
  file tool above (help text stored as plain files) rather than hardcoded.
- **RAG support** - retrieval-augmented generation over some corpus (notes?
  history? both?). Probably a big task. `nomic-embed-text` is already pulled
  in Ollama, so the embedding side has a natural starting point.
- **Improve current tools** - a general polish pass over the existing tool
  set (Hue lights, timers, web search, task runner) rather than one specific
  fix.

## Session & model behavior

- **Pre-load the model at startup** - right now the model only loads on the
  first message; see if Ollama's keep-alive/preload mechanism (an empty
  `/api/chat` request, or similar) can warm it up during olli's own startup
  instead, so the first real message doesn't eat the load time.
- **Actively save chat history through the session** - make sure history is
  reliably persisted as the session goes, not just on the periodic
  size-changed check in `ollama_system::process()`.
- **Auto-clear context after 30 minutes idle** - reset/expire the chat
  history after a period of inactivity. Also a plausible mitigation for the
  persona-drift issue below - a stale, poisoned context can't outlive 30
  minutes of silence.
- **Investigate concurrent sessions** - what actually happens to Voca and
  Lira/TTS if one olli session is already running and a second one starts?
  We hit real contention this session (a second instance hung waiting on the
  mic) - worth understanding and documenting properly, maybe guarding
  against it explicitly.

## Remote access

- **Expose an API** so a program on another system can talk to olli's
  interface, not just local keyboard/voice/TTS.

## Open questions / carried over

- Consolidation summaries can cement a bad pattern as an established "fact"
  about the assistant (seen firsthand in a "locked door" persona-drift loop
  from a poisoned `history.json` this session). No fix decided - revisit if
  it recurs, and see the 30-minute auto-clear idea above.
- Check `~/source/voca_cpp` against `source/voca.hpp`/`.cpp` for drift -
  flagged when the C++ VOCA port landed, never confirmed either way.
