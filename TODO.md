# TODO

Guiding principle for all of the below: keep code simple, easy to read, and
well compartmentalized. That's the bar for new work here, not just a nice-to-have.
Before writing a new function, check `helper_olli.cpp`/`.h` and
`tools_helper.cpp`/`.h` for an existing one to reuse. When adding a new
function that's frequently used and well-tested, put it in one of those two
files rather than leaving it local to a single caller: `helper_olli` for
general-purpose helpers, `tools_helper` for helpers reused across tools but
not needed elsewhere.

## New tools

- **Text file tool** - let olli create, modify, and delete simple text files
  (e.g. notes), presumably scoped to a dedicated directory under
  `~/olli_files/` rather than anywhere on disk.
- **Tool-help tool** - a way to ask olli what tools it has and what they do,
  loading/displaying that info at runtime. Could be built on top of the text
  file tool above (help text stored as plain files) rather than hardcoded.
- **RAG support - first working version done (2026-09-08).** Three programs
  under [`tools/rag/`](tools/rag) (see [`tools/rag/README.md`](tools/rag/README.md)
  for the full design): `rag_db`, a shared SQLite storage/embedding/chunking
  library; `rag_admin`, a standalone menu-driven maintenance program;
  `rag_tool`, a remote tool ([`tools/PROTOCOL.md`](tools/PROTOCOL.md))
  registering `rag_list_collections`/`rag_search` with a live olli.
  Embeddings via Ollama's already-pulled `nomic-embed-text`. Verified
  end-to-end against a real running olli session, not just compiled - that
  live testing caught and fixed two retrieval-quality problems worth
  knowing about before touching this code: `nomic-embed-text` needs its
  `search_document:`/`search_query:` prefix convention (dropping it
  measurably hurt ranking), and the original 400-word chunk size was
  diluting embeddings on jargon-dense documents by merging multiple
  subsections into one chunk (dropped to 150 words). Not yet built: a
  chat_log-aware importer (the "conversations" collection is
  manual-export-then-import for now) and olli auto-importing its own
  conversations.
  - **Done 2026-09-09: profile-aware database + folder-based sync.**
    `rag_tool` now follows olli's `identity` message the way
    `clock.cpp`/`presence` follow it for their own per-profile state -
    starts on the shared `~/olli_files/rag.db`, switches to
    `~/olli_files_<name>/rag.db` on `identity`, switches back on
    disconnect; an explicit CLI argument still overrides this permanently
    for testing. `rag_admin` takes a bare profile name the same way. Bigger
    change: `rag_admin` no longer imports an arbitrary file path - each
    collection now owns a folder (`~/olli_files_<profile>/collection/<name>/`,
    `profile_collection_dir()` in `rag_db.hpp`), and "Update database" does
    a real three-way sync against it (new file -> import, unchanged content
    -> skip via a stored `content_hash`, changed content -> delete and
    reimport, source file gone -> delete the document) - safe to run
    repeatedly. Verified all four cases live.
  - **Done 2026-09-09: fixed a real truncation bug, added
    rag_search_documents/rag_get_document.** Live testing with real notes
    (calipers/M2-screws-style "I know it's in there somewhere" scenario)
    found `rag_tool`'s `handle_search()` was truncating every chunk to 300
    characters before handing it to the model - a 150-word chunk runs
    600-900+ characters, so the model was routinely told "not found" for
    content that had, in fact, been retrieved correctly, just cut off
    before the relevant part. Fixed by sending full chunk text (chunks are
    already bounded by `chunk_text()`, so no new cap needed). Also added
    two tools once that scenario was thought through fully:
    `rag_search_documents` (same ranking as `rag_search`, deduplicated to
    one result per *document* instead of per chunk - "what do I have on
    this topic" survey, not "find the passage") and `rag_get_document`
    (fetch one document's complete original content by title+collection,
    once identified from either search). Needed a new `documents.content`
    column storing the full original text verbatim - reconstructing it
    from overlapping chunks would have been lossy/duplicated at chunk
    boundaries. Deliberately did *not* build search pagination/exclusion
    ("show me the next 5") - `rag_search_documents` covers the same need
    by surveying everything relevant in one call instead. Verified all of
    this against a scripted fake-olli (not just a live model, which can't
    be trusted to pick exact test arguments) - registration, both search
    tools' ranking/dedup, full-document fetch, and both error paths all
    confirmed.
  - **Done 2026-09-09: case/extension-tolerant name matching.** A real
    live test showed `rag_list_collections` telling the model a collection
    was named "Notes," then every follow-up call using "notes" instead and
    failing a plain case-sensitive match, every time, even though the
    collection was right there. `collections.name` and `documents.title`
    are now `COLLATE NOCASE`; `find_document_by_title()` also falls back
    to comparing both sides with any file extension stripped, since a
    model asked for a document by name tends to drop the extension it was
    shown too ("misc_notes" for a document titled "misc_notes.txt").
    Deliberately didn't rename stored titles to drop extensions outright -
    that would risk two same-name-different-extension files colliding.
    Verified against a real scripted rag_tool connection (not a live
    model - needed a test harness robust to a stray already-running
    `clock` tool racing for the same port, which is what falsely looked
    like a failure on the first two attempts).
  - **Done 2026-09-09: rag_admin menu caught up with everything above.**
    Removed "Delete a document" - now actively counterproductive under the
    folder-sync model (deleting just the DB row while its source file is
    still on disk just gets it reimported on the next "Update database").
    Added "Delete collection" (DB-only via the same cascading delete a
    document already used, folder/files deliberately left untouched -
    verified live: deleted, confirmed files survived, recreated + resynced,
    got all 6 documents back exactly), "Edit collection description", and
    two menu options mirroring rag_tool's newer search_documents/
    get_document so both can be sanity-checked without olli.
  - **Done 2026-09-09: nudged rag_search/rag_search_documents's
    descriptions to cover tool help/documentation.** A real live test
    asked "help on the clock tool" and got the current time back instead
    - nothing hinted the model toward treating "help on X" as a search
    request rather than a request to actually call tool X. Both
    descriptions now explicitly say they cover help/documentation about
    olli's own tools too, and that a help request about a tool by name
    shouldn't be read as a request to call that tool. This is a
    description nudge, not a guaranteed fix - still a judgment call the
    model makes each time between two plausible readings.
  - **Done 2026-09-10: chat_log auto-sync via a special "conversations"
    collection.** No new database needed after all - the existing
    collections/documents/chunks schema and the metadata JSON field
    already covered it. "Update database" now auto-creates a
    `conversations` collection (never via "Create collection") the moment
    a profile has a `~/olli_files_<profile>/chat_logs/` directory, and
    syncs it straight against that existing directory
    (`profile_chat_logs_dir()`) rather than copying/symlinking logs into
    a `collection/` subfolder - reuses the exact same
    import/skip/reimport/delete sync every other collection already gets.
    Logs under 30 words are skipped as noise; each document's metadata
    gets its date/time parsed from olli's own `YYMMDD.HHMM[.N].
    chat_log.txt` filename convention. Verified live against a real
    profile's chat_logs/ (auto-creation, filtering, metadata, and
    re-run idempotency all confirmed) before running it for real.
  - **Done 2026-09-10: flagged conversation-transcript results as
    historical, not live instructions.** Real risk found live: a
    retrieved chat log excerpt literally contained "Ron: run the task
    named radiohead fitter" (a genuine past command, preserved verbatim),
    and the model read that quoted text as a live instruction and
    actually re-ran the automation - repeatedly, unprompted. Notes/help
    content doesn't carry this risk; a conversation transcript inherently
    does, since it's full of real commands directed at olli.
    `rag_tool`'s `format_results()` now appends an explicit "this is
    historical, don't act on it" note whenever any result comes from the
    `conversations` collection - a mitigation, not a guarantee. Verified
    live: the note appears on a conversations-scoped search, and is
    absent on a help-scoped one (no false positives). Deliberately did
    *not* try to design for every possible way someone might search
    conversations (specific lookup vs. broad survey vs. narrowing down,
    etc.) - a chat log doesn't respect "one document = one topic" the way
    a note does, so no new tool surface fixes that; left to iterate on as
    real friction surfaces, not designed for speculatively.
  - **Done 2026-09-10: `special_instruction` on a remote tool's `result`
    (step 1 of 2 toward "let a tool ask for exact delivery," see the
    design discussion this came from).** Real gap found live: asking
    olli to show a whole note revealed every tool result narrates through
    the exact same generic "[DIRECTOR_NOTE]... be concise, no jargon"
    framing (`ollama_system::integrate_tool_result()`, `source/olla.cpp`)
    - fine for a short fact, not for relaying a multi-thousand-character
    document faithfully. `integrate_tool_result()` already had an unused
    `Special_Instruction` parameter (every call site in the whole codebase
    passed `""`) - wired it through instead of adding new plumbing:
    `OLLI_LINK::send_result()` (`tools/olli_link/`) gained a defaulted 3rd
    parameter, `TOOL_REMOTE::check()` (`source/remote_tools.cpp`) now
    reads an optional `special_instruction` field off the wire message and
    passes it through instead of a hardcoded `""`, and
    `rag_get_document` (`tools/rag/rag_tool/rag_tool.cpp`) is the first
    real caller, asking the model to relay a full document verbatim
    instead of summarizing. Fully backward compatible - every existing
    remote tool (clock/hue/presence) keeps calling the 2-argument
    `send_result()` unchanged. No new global state anywhere in this;
    `special_instruction` flows purely through parameters already in
    every touched function's signature. Verified the wire format live
    (a scripted connection confirmed the field lands correctly); the
    receiving side (does olli's `integrate_tool_result()` actually
    narrate differently) needs a live chat turn to confirm, not just a
    scripted one.
    - **Still to do (step 2, not built)**: a genuine exact-data side
      channel - `special_instruction` only steers narration, it can't
      guarantee a huge result survives unmodified, and it can't help at
      all for a non-text result (e.g. a future screen-grab tool). The
      plan is to generalize `COMMS::WEB_LINKS`'s existing bypass-
      narration pattern (`source/comms.h`/`user_io.cpp`) to any remote
      tool, not just web search - a bigger, cross-cutting change
      (`source/remote_tools.cpp`, `tools/PROTOCOL.md`, wherever
      `WEB_LINKS` gets displayed), to be designed properly in its own
      pass rather than folded into this one.
  - **Done 2026-09-10: title matching also treats `_`/`-` as a space.**
    Found live, right after the case/extension fix above shipped: a model
    asked for "misc notes" (space) when the real title was
    "misc_notes.txt" (underscore), and failed the existing fallback since
    it only handled case and extension. `find_document_by_title()`'s
    fallback now normalizes `_`/`-` to a space too, alongside the
    existing case-fold and extension-strip. Verified live (the exact
    failing case now resolves correctly).
  - **Step 2, part 1 done 2026-09-10: `COMMS::WEB_LINKS` generalized into
    `COMMS::TOOL_ATTACHMENTS`.** The plumbing half of the exact-data side
    channel from the note above - see the dated addendum on the original
    `COMMS::WEB_LINKS` + Ctrl+L popup entry (Display / OUTPUT_CLASS
    section) for exactly what changed. Deliberately scoped
    to "upstream" only, agreed explicitly before starting: `comms.h`
    (the new `TOOL_ATTACHMENT{type, label, content}` struct),
    `tools.cpp` (`TOOL_WEB_SEARCH` now pushes `type="link"`),
    `io_worker.cpp` (`exchange()`'s drain), plus the handful of
    mechanical (behavior-unchanged) renames in `user_io.h`/`.cpp` that
    were unavoidable once the field's type changed at the source. Not
    done yet, deliberately deferred as their own separate steps: (a)
    `remote_tools.cpp`/`tools/PROTOCOL.md` wiring so a remote tool like
    `rag_tool` can actually populate `TOOL_ATTACHMENTS` at all (nothing
    but `TOOL_WEB_SEARCH` can reach it yet), and (b) the actual downstream
    display work - type-branching so a "document" entry doesn't render as
    a broken link, plus a real text viewer for it. Both need designing,
    not just coding, before either lands.
  - **Step 2, part 2 done 2026-09-10: `remote_tools.cpp`/`tools/PROTOCOL.md`
    wiring, so a remote tool can actually reach `TOOL_ATTACHMENTS`.** New
    optional `attachment` object on the wire `result` message (`type`/
    `label`/`content`, same shape as `TOOL_ATTACHMENT`) - `TOOL_REMOTE::
    check()` parses it and pushes straight into `comms.TOOL_ATTACHMENTS`
    (same `output_buffer_mutex` lock `TOOL_WEB_SEARCH`'s own writes
    already use). `tools/olli_link/`'s `send_result()` gained a matching
    4th defaulted parameter (`OLLI_ATTACHMENT` - its own type, not shared
    with `comms.h`, since `tools/` deliberately doesn't share headers
    with `source/`); every existing 2/3-arg call site is unaffected.
    `rag_get_document` (`tools/rag/rag_tool/rag_tool.cpp`) is the first
    real producer - sets `type="document"` with the full document text.
    Verified live end-to-end: connected a real `rag_tool` to a disposable
    test `olli` instance, drove an actual chat turn (via `tmux send-keys`,
    no user needed), and confirmed a `rag_get_document` call completes
    normally with no crash through the new parsing code - can't yet
    confirm the attachment is *displayed* correctly, since that's still
    the deferred step, but the plumbing itself doesn't break anything.
    Along the way, found (but didn't fix, unrelated to this change): a
    ~19-second gap between two tool calls in the same turn (real model
    thinking time) exceeded the 15-second heartbeat timeout while olli's
    own thread was busy generating, and the remote tool got marked dead
    before the second call could even be attempted - a pre-existing
    heartbeat/reconnect characteristic that'd affect any remote tool
    under a long enough gap between calls, not something introduced here.
  - **Step 3 done 2026-09-10: the actual display - numbered selection in
    the Ctrl+L popup, and a new `DOCUMENT_VIEWER` class for non-link
    attachments.** The last piece of the exact-data side channel from
    Step 1/2 above. `OUTPUT_CLASS::show_web_links_panel()`
    (`source/user_io.cpp`) now lets you type a number then Enter (or a
    blank Enter, or `q` + Enter, to just dismiss) to pick one listed
    attachment: a `"link"` entry prints its raw URL (the numbered list
    itself only ever shows the clickable label - `make_clickable_link()`'s
    OSC 8 escape hides the URL inside the escape sequence, never visible
    as plain text otherwise); anything else opens the new
    `DOCUMENT_VIEWER` (`source/document_viewer.h`/`.cpp`) on it - a
    scrollable, line-numbered, word-wrapped full-screen text viewer
    adapted from delmanel's own standalone `file_watch` program
    (`../../file_watcher`), stripped of its live-file-reload machinery
    since the content here is a fixed string already in memory, not a
    watched file on disk. Navigation: Up/Down/PageUp/PageDown/Home/End, a
    typed number + Enter to jump to that line, a blank Enter or `q` +
    Enter to quit.
    - **Two real bugs found live, both confined to `document_viewer.cpp`
      once understood:**
      1. The first version copied `file_watch`'s own `initscr()`/
         `endwin()` lifecycle, since that's how `file_watch` (a genuinely
         standalone program) legitimately owns its own screen. olli
         already has exactly one curses screen alive for its whole
         process; a *second* `initscr()` call while the first is still
         alive silently corrupts the shared `SP`/`stdscr`/`curscr`
         globals - confirmed live: it left a stray row on screen that not
         even a full window-resize rebuild (recreating every window from
         scratch) could repair. Fixed by never calling `initscr()`/
         `endwin()` here at all - the caller
         (`show_web_links_panel()`) already suspends/resumes the one real
         screen via `def_prog_mode()`/`endwin()`/`reset_prog_mode()`, and
         `DOCUMENT_VIEWER` just draws into it.
      2. Removing that alone didn't fully fix it: the viewer's own
         `cbreak()`/`noecho()`/`keypad()`/`timeout()` calls (made to
         configure curses' `getch()`) left `KEYBOARD_INPUT`'s own separate
         raw, non-blocking terminal reader broken afterward - confirmed
         live: the chat input box stopped echoing/submitting typed
         characters entirely once the viewer had been opened and closed
         once, even though its own navigation worked perfectly the whole
         time it was open. Fixed by never touching terminal modes here
         either - the viewer now reads stdin the exact same raw way
         `KEYBOARD_INPUT` already does (manual byte-level parsing of
         arrow/Home/End/PageUp/PageDown escape sequences, plus its own
         saved-and-restored `SIGWINCH` handler for resize, since
         `KEY_RESIZE` only ever arrives through curses' own `getch()`,
         which this file no longer calls), and only ever uses curses for
         drawing.
    - **A third, smaller issue found live, fixed separately.** Right
      after returning from either the viewer or a plain link view, one
      screen row (the chat/input separator, or the tools-panel border)
      could be left showing stale content indefinitely - traced to
      `ncurses_draw_focus_indicators()` (`user_io.cpp`) drawing those
      separators directly onto `stdscr` rather than into any of the four
      real windows (`win_system`/`win_chat`/`win_input`/`win_tools`),
      which are the only things `show_web_links_panel()`'s existing
      `clearok(curscr, TRUE)` actually guaranteed would get freshly
      recomposited (they already do, every tick, regardless). Fixed by
      calling `ncurses_draw_focus_indicators()` again right there on
      return, instead of leaving it to happen on its own later (which it
      always eventually did - e.g. the next real chat message resizing
      the input box - just not "now").
    - `DOCUMENT_VIEWER::show()` also takes the `WINDOW*` to draw into as
      an explicit parameter (always `stdscr` in practice) rather than
      reaching for the global implicitly, per this project's own
      no-hidden-globals convention for this refactor - raised as a direct
      question ("couldn't you just inherit the ncurses stream via the
      function call?") after the class was already working, and applied
      as a pure mechanical follow-up with no behavior change.
    - Verified live throughout on a disposable test `olli` + `rag_tool`
      instance (never the real session) - both bugs reproduced and
      confirmed fixed, full navigation, both attachment types, and a
      repeat pass scrolled to the very last line before quitting to
      confirm the stray-row fix holds under the same conditions that
      first exposed it.
  - **Done 2026-09-12: `rag_tool` now syncs on its own, every 10 minutes,
    instead of needing `rag_admin` run by hand.** The three-way sync
    (`sync_collection()`, `do_update_database()`, the special
    `conversations` handling, `chat_log_metadata()`, `MIN_CHAT_LOG_WORDS`)
    moved out of `rag_admin.cpp` into a new shared
    `tools/rag/rag_db/rag_sync.hpp`/`.cpp` - the same location (and
    Makefile-list-of-`.cpp`-files convention) `rag_db`/`rag_embed`/
    `rag_chunk` already share between both programs, so both `rag_admin`
    and `rag_tool`'s Makefiles just gained one more line each rather than
    needing a new build target. Public surface is one function,
    `sync_profile_collections(db, embedder, profile_name)`, returning a
    `RAG_SYNC_STATS` (imported/updated/unchanged/removed counts, plus a
    `skipped_busy` flag - see below) instead of printing anything itself,
    so each caller decides how to show it: `rag_admin`'s "Update database"
    is now a thin wrapper that prints the exact same "Done: X imported..."
    line it always did (verified byte-for-byte identical output);
    `rag_tool` folds a short summary into its existing one-line status
    display instead (due for a real redesign of its own, separately - not
    attempted here).
    - **Concurrency**: two separate processes can't share a `std::mutex`,
      so `sync_profile_collections()` takes a non-blocking `flock()` on a
      new `.rag_sync.lock` file next to `rag.db` (`profile_sync_lock_path()`,
      a new sibling to `rag_db.hpp`'s existing `profile_db_path()`/
      `profile_collection_dir()`/`profile_chat_logs_dir()`) for the whole
      call - whichever program loses just skips that round (`skipped_busy`)
      rather than waiting, since `rag_tool` in particular needs to stay
      responsive to the wire protocol regardless. Verified live: held the
      lock externally, confirmed `rag_admin` correctly reported "Sync
      already in progress" and left the database untouched; released it,
      confirmed a normal sync then worked.
    - **A real bug found live, not just in review**: the very first
      automatic sync could fire against the *wrong* profile.
      `OLLI_LINK::is_connected()` goes true the instant the TCP socket
      connects, but olli's own `identity` message (how `rag_tool` learns
      which profile it's actually talking to) arrives as a separate, later
      application message - the first eligible tick could (and, live,
      did) fire before that arrived, syncing the shared default
      `~/olli_files/` instead of the real profile's folder. No data
      damage in the live case (the one file present in the wrong
      profile's `chat_logs/` was too short to import), but a real risk in
      general. Fixed with a new `identity_received` flag, separate from
      `is_connected()` - the periodic sync now also waits for that (or
      `explicit_profile`, which already knows its profile for good from
      the command line and never needs to wait). Reset to false on
      disconnect, since a reconnect might be a different olli/profile.
      Re-tested clean afterward - correct profile every time.
    - `rag_tool` also now tracks the actual profile *name* alongside its
      existing `db_path` tracking (`switch_database()`'s signature grew a
      `std::string& profile_name` it keeps in sync with `db_path` at the
      same two call sites, `identity`/disconnect) - folder-based sync
      needs the name itself, not just the resulting database path.
    - Fires on the first eligible tick (immediately once connected and
      the profile's known, not after waiting a full 10 minutes), per
      design discussion; only while actually connected, since a
      disconnected `rag_tool` syncing in the background wasn't asked for.
  - **Done 2026-09-16: sync now warns about a collection folder with no
    matching collection row.** Real gap found live: `sync_profile_
    collections()` only ever iterated existing DB collections against
    their own folders - never the reverse. A folder dropped under
    `collection/` without running "Create collection" first was
    completely invisible to sync: no error, no warning, its files just
    silently never got imported. Confirmed live against a test profile
    whose `Notes`/`help` folders sat fully populated on disk but unsynced
    - a RAG query confidently reported "no info" on content that was
    right there in the file. New `profile_collection_root_dir()`
    (`rag_db.hpp`/`.cpp`, mirrors the existing `profile_chat_logs_dir()`)
    plus `RAG_SYNC_STATS::orphan_folders` (`rag_sync.hpp`) -
    `sync_profile_collections()` now also scans the profile's `collection/`
    root and reports any subfolder with no matching collection
    (case-insensitively, via `db.find_collection()`, so it doesn't create
    or touch anything - deciding to create a collection stays a deliberate
    manual action). `rag_admin`'s "Update database" and `rag_tool`'s
    periodic sync status line both surface it. Verified live: created a
    throwaway orphan folder, confirmed the warning named it and only it,
    then removed the test folder.
  - **Done 2026-09-16: filtered out low-information chunks that were
    outranking real content, plus a "Force resync" to apply the change to
    already-imported data.** Real live finding, on `ron`'s actual
    database, not a synthetic test: `rag_search` for "automotive" ranked a
    `conversations` chunk at 0.62 - pure repeated boilerplate ("that's a
    bad taste in the matrix... something's fked up with that remote end."
    repeated 4x, a leftover from an earlier remote-tool-reconnect failure
    loop) - *ahead* of `car_maintenance.txt`'s genuine, on-topic content
    at 0.56. New `is_low_information_chunk()` (`rag_chunk.hpp`/`.cpp`)
    flags a chunk whose distinct-word ratio falls below 0.45 (calibrated
    against real data: legitimate notes/conversation chunks measured
    0.58-0.94, the offending chunk measured 0.26), with a 20-word floor so
    a short chunk isn't falsely flagged on ratio noise. Wired into
    `sync_collection()` (`rag_sync.cpp`) before embedding, so a flagged
    chunk is never even stored.
    - **Doesn't help already-imported data on its own** - the normal
      unchanged-content-hash skip means an existing document never gets
      re-chunked just because the chunking *logic* changed, only when its
      source file does. New `force` parameter threaded through
      `sync_collection()`/`sync_profile_collections()` (default `false`,
      so `rag_tool`'s automatic background sync is entirely unaffected)
      bypasses that skip - re-chunks and re-embeds every document
      regardless. New `rag_admin` menu option "10) Force resync (rebuild
      everything, even unchanged)", with a confirmation prompt since it's
      a bigger operation than a normal sync.
    - Verified on a **disposable copy** of `ron`'s real database first
      (never production directly): confirmed the specific offending chunk
      was gone, and that a specific query ("tire rotation schedule") now
      correctly lands `car_maintenance.txt` at the top, clearly relevant.
      Applied live afterward via the new menu option, then reconfirmed
      directly against the database (not just trusting the tool's own
      report): `conversations` 952 -> 901 chunks, the specific offending
      document down to 2 chunks from 5.
    - **Only closes the clear-cut anomaly, not the deeper volume-imbalance
      effect** - `conversations` still has ~75x `Notes`' chunk count
      (901 vs. 12), so a vague single-word query ("automotive" alone,
      with no specific phrase to match) still leans toward `conversations`
      purely on volume, even with the anomaly gone; a specific phrase
      ("tire rotation schedule") now works cleanly. A second option was
      discussed and deliberately deferred, not built: collection-aware
      tie-breaking at query time (prefer `Notes`/`help` over
      `conversations` when scores are close).
- **Tools rework - mostly done now** (started 2026-08-21). Pulled every
  `TOOL_*` class out of `olla.h`/`olla.cpp` into their own `tools.h`/
  `tools.cpp`; gave every tool the same `configure`/`register_tool`/`check`/
  `monitor_tool` shape via an abstract `TOOL_BASE`. `ollama_system` reaching
  `CLASS_SYSTEM` was resolved 2026-08-24 (a nullable `CLASS_SYSTEM*` threaded
  through `process()` -> `handle_instance_tools()` -> `dispatch_tool_call()`
  -> each tool's `check()`/`monitor_tool()` - null for sidetrack's background
  thread and task-runner automation instances, which have no business
  touching the real one).
  - **Done 2026-08-27: `tools_list` moved off `ollama_system` entirely.**
    Used to be a member, populated once in the constructor. Now every
    `open()`/`send()`/`process()`/`dispatch_tool_call()`/
    `handle_instance_tools()`/`integrate_tool_result()` takes it as a
    reference parameter instead - cascaded into `TOOL_BASE::check()`/
    `monitor_tool()` and every concrete override (`tools.cpp`,
    `remote_tools.cpp`), since several `handle_tool()`s call
    `chat.send()`/`chat.integrate_tool_result()` internally. Each isolated
    instance (main chat, `SIDETRACK_CLASS`'s own member, each task-runner
    automation instance, `consolidate()`'s local client) owns its own real
    `tools_list`, populated via the new free function
    `populate_default_tools()` (olla.h/.cpp) - no more nullable-tools_list
    problem the way a `CLASS_SYSTEM`-owned version would have had (see the
    design discussion this came from if reviving that idea - the real
    blocker was sidetrack/task-runner needing real tools but no safe
    `CLASS_SYSTEM`, not a technical one). Remote tools still only ever
    register onto main chat's `tools_list` (`main.cpp`) - task-runner
    automation instances can see them too now (2026-09-02, see the
    task-runner rewrite entry below); sidetrack still can't, deliberately
    (see its own entry's data-race/single-connection concern).
  - **Done 2026-08-27: `TOOL_PERMISSIONS_CLASS` dropped entirely**, not
    redone - confirmed genuinely dead weight first: every flag was already
    hardcoded `true` (`main.cpp`), and remote tools (the majority of what's
    actually used) never checked it at all. Removed the class
    (`tools_helper.h`), the member on `ollama_system`/`TASK_SIMPLE`, and
    every gate check in `tools.cpp`.
  - **Done 2026-09-02: the task-runner display bug below.**
  - General polish pass over the existing tool set (Hue lights, timers, web
    search, task runner) beyond the structural rework itself.
  - **Done 2026-09-15: remote-tool communication moved to its own thread,
    `TOOL_WORKER_CLASS` (`source/tool_worker.h`/`.cpp`).** The real bug this
    fixed wasn't a crash - remote-tool results/events arriving while main's
    own thread was busy (streaming a response, blocked in a task-runner
    script) used to just sit unpolled until the next `process()` tick got
    around to them, since the old dispatch/monitor loop
    (`ollama_system::dispatch_tool_call()`/`TOOL_REMOTE::monitor_tool()`,
    `tools.cpp`/`remote_tools.cpp`) only ever ran inline on that same
    thread. Modeled directly on `IO_WORKER_CLASS` (`io_worker.h`/`.cpp`):
    its own background thread, the same INTERUPTED/PROCESSING atomics
    rendezvous for cross-thread handoff (no mutex over the worker's own
    state - `set_identity()`/`put_pending_call()`/`get_pending_result()`/
    `get_pending_event()`/`get_registered_tool_defs()` each do their own
    short wait-then-touch, mirroring `exchange()`'s shape), and no global
    state - every function takes what it needs as a parameter, same as the
    rest of this rework.
    - **`TOOL_REMOTE` gained a non-blocking `poll_communications()`**
      (`remote_tools.h`/`.cpp`), driven purely by `pending_calls`/
      `pending_results`/`pending_events` vectors with no `chat`/`comms`
      dependency at all - added alongside the old blocking `check()`/
      `monitor_tool()` rather than replacing them (both still compile and
      still work; nothing currently calls them outside sidetrack's own
      historical path, kept rather than deleted per this project's usual
      caution around code real remote tools - clock/hue/presence/rag -
      still depend on being correct).
    - **One unified call-timeout** (`CALL_TIMEOUT_SECONDS = 300`,
      `call_deadlines` tracked by call_id) covers both a call nothing ever
      claims and a call claimed but abandoned - previously two different
      failure shapes with no single mechanism catching both.
    - **One unified tool-advertisement registry**
      (`registered_tool_defs`/`manual_tool_defs`), rebuilt every tick from
      whatever's actually connected plus the built-ins - `send()`
      (`olla.cpp`) now reads only from `tool_worker->get_registered_tool_defs()`
      instead of separately walking `tools_list`. Built-ins are seeded once
      via `register_local_tools()`, reusing each real tool instance's own
      `register_tool()` (no schema duplicated anywhere).
    - **Built-ins (`TOOL_SET_THINKING_MODE`/`TOOL_WEB_SEARCH`/
      `TOOL_DELEGATOR`/`TOOL_TASK_RUNNER`) still dispatch inline on main's
      own thread**, through the same `tools_list`+`check()`-loop dispatcher
      as before - deliberately not folded into tool_worker's queue, since
      they have real reasons to run on main's thread (e.g. `TOOL_DELEGATOR`/
      `TOOL_TASK_RUNNER` spawning their own sub-agent `ollama_system`
      instances). `dispatch_tool_call()` tries the built-in loop first,
      falling through to `tool_worker->put_pending_call()` only if nothing
      claimed it. This required threading a nullable `TOOL_WORKER_CLASS*`
      all the way into `TOOL_BASE`'s own virtual interface
      (`check()`/`monitor_tool()`, not just `dispatch_tool_call()`'s level)
      - needed once a running task-runner script's own sub-instance can
      itself call another task/delegator, which needs real access to
      `tool_worker` too, not `nullptr` (same nullable-pointer convention
      `CLASS_SYSTEM*` already uses, and the same kind of deliberate
      exception to it sidetrack's second-guess review already has).
    - **`tools_list_mutex` removed entirely** (was guarding 3 sites,
      see the 2026-09-05 race-crash entry below) - now genuinely dead, since
      tool_worker's own rendezvous is what actually prevents concurrent
      access to shared tool state, not a mutex over `tools_list` itself.
    - Live-verified end-to-end against real profiles/real remote tools
      (clock, hue, presence, rag) before this landed - registration,
      non-blocking servicing, timeout, event routing, and built-in dispatch
      all confirmed via `debug_full_history.txt`, not just a clean compile.
    - **Known, deliberately deferred, not designed yet**: `pending_events`/
      `pending_results` have no correlation to "which conversation" is
      waiting on them, so whichever `ollama_system::process()` call reaches
      them first (often a blocked sub-agent's own fast inner loop) drains
      them - a pre-existing characteristic (the old `monitor_tool()` had the
      same race), not a regression. Plan is script-defined behavior for an
      out-of-script tool event/result, not yet designed.
  - **Done 2026-09-16: `TOOL_WEB_SEARCH` curl timeout tuning + honest
    failure framing.** Real, live-caught bug: `web_search`/
    `fetch_website_content` return "Error: libcurl failed (...)" whenever
    curl's own timeout fires - confirmed from `debug_full_history.txt`, a
    real SF-weather search timed out at exactly 10.02s against
    serpapi.com, matching the old hardcoded `CURLOPT_TIMEOUT, 10L` to the
    millisecond. Considered (and rejected) making the tool a remote tool
    instead, on the theory async would fix it - it wouldn't: curl's own
    clock runs the same either way, moving the wait to another thread
    doesn't make serpapi answer faster, and it would've been a whole new
    subsystem (own process, own PROTOCOL.md registration, its own "is it
    even running" failure mode) to fix a value that just needed tuning.
    - **`perform_actual_search()`/`fetch_url_content()`'s duplicated curl
      setup collapsed into one new private helper, `curl_get()`**
      (`tools.h`/`.cpp`) - timeout raised from 10s/15s to 20s/25s, a new
      separate `CURLOPT_CONNECTTIMEOUT` (5s) so a slow DNS/TLS handshake
      can't eat the whole budget before the real request starts, one
      retry gated specifically on `CURLE_OPERATION_TIMEDOUT` (a bad
      URL/host-not-found won't succeed on a blind retry, so those still
      fail immediately), and `CURLOPT_ERRORBUFFER` for the specific
      failure detail beyond `curl_easy_strerror()`'s generic category.
    - **Honest failure framing, found from a different live gap**:
      `integrate_tool_result()`'s default `[DIRECTOR_NOTE]` framing
      ("report this real result... without changing the facts") reads
      identically whether the tool succeeded or not, so a raw curl error
      used to get narrated with the same instructions as a real answer.
      `perform_actual_search()`/`fetch_url_content()` now return
      `std::pair<bool, std::string>` instead of a bare string, so
      `handle_tool()` can tell success from failure directly rather than
      guessing from an "Error:" prefix, and passes a distinct
      `Special_Instruction` on failure telling the model plainly it's not
      real data and to explain what went wrong in its own words - no
      error-code translation table needed, the model handles that fine on
      its own once it knows it's looking at a failure. Reuses
      `integrate_tool_result()`'s existing `Special_Instruction` parameter
      (same mechanism `rag_get_document` already uses for its own
      verbatim-relay case) rather than touching the shared function itself
      - every other tool is unaffected.
    - Verified live, twice: a non-routable IP (`http://10.255.255.1`) hit
      the real timeout+retry path and got "That URL didn't respond. The
      connection timed out, so it's either down or blocked." - exactly the
      intended honest framing. A fake domain
      (`this-domain-does-not-exist-asdkjh123.com`) turned out to test
      something else entirely - the model declined to even call the tool
      ("I don't have permission to visit external sites without a valid
      reason"), a hallucinated restriction that doesn't exist anywhere in
      the code, not something this fix touches or could fix.
  - **Done 2026-09-17: a persistent keyboard-state bug, and a real per-line
    delay mechanism for task-runner scripts, both found via live use.**
    - **Enter-keypress silently dropped in a fast burst - real bug, not a
      tmux artifact.** `KEYBOARD_INPUT::keyboard_input()` (`user_io.cpp`)
      used to track "was the last processed byte also Enter-class" as a
      class member (`last_char_was_enter`), meant only to stop a genuine
      `\r\n` pair (two bytes, one keypress) from double-submitting. Bug:
      being a class member, it persisted across completely unrelated
      keypress events, not just within one physical Enter's own byte pair
      - submitting a real message (ending in Enter) left it `true`, and the
      *next*, entirely separate Enter (e.g. a `[PAUSE]` prompt's "press
      Enter to continue") got silently swallowed, needing a second press to
      register. Fixed by making it a local variable inside
      `keyboard_input()`, reset fresh every call - a genuine `\r\n` pair
      still arrives and drains together in one call, but a later, distinct
      keypress no longer inherits stale state from an unrelated earlier one.
    - **`TASK_SIMPLE::delay_tool_returns` (default `true`) + a new
      `[WAIT_FOR_RESULT]` script command**, `tools_helper.h`/`.cpp` and
      `TOOL_TASK_RUNNER::handle_tool()`'s own while loop (`tools.cpp`) -
      see the entry above (2026-09-16, tool_worker rewrite) for the
      out-of-script routing race this is the first real fix for. While a
      script runs with `delay_tool_returns` set, its own
      `instance.process()` calls stop draining `tool_worker`'s pending
      results/events entirely (drained manually into local
      `held_results`/`held_events` instead, right before that same call, so
      nothing is lost - just not narrated live through the script's own
      throwaway context/persona) - everything held gets replayed on
      `instance` once the script's own command list finishes, right before
      its final report. A held event's follow-up action goes onto `chat`'s
      own `pending_tool_calls`, not `instance`'s - confirmed via
      `process()`'s own PART 2 comment (`olla.cpp`) that a background task
      instance gets `tool_worker` forced to `nullptr` on every tick once
      it's marked complete, so anything still sitting in `instance`'s own
      queue by then could never actually reach a real tool.
      `[WAIT_FOR_RESULT]` is the opt-in escape hatch for a specific line
      that needs its answer *now* instead of at the end (new
      `SCRIPT_STATE::WAIT_TOOL_RESULT`, `tools_task_script.h`/`.cpp` -
      `advance_script_state()`'s own case for it is a no-op, resolved
      entirely in `handle_tool()`'s loop, so `tools_task_script.*` never
      needs to know `held_results` exists at all).
      - **First implementation attempt reintroduced the exact CPU-pegging
        bug this session's own tool_worker rewrite had already hit once**
        (see the 2026-09-14/15 entry) - `TOOL_TASK_RUNNER::handle_tool()`'s
        while loop turned out to have *no pacing of its own at all*, ever;
        it was silently leaning on `tool_worker`'s own atomics-rendezvous
        wait (inside `instance.process()`'s old draining call) as an
        accidental throttle. Skipping that draining call removed the only
        thing keeping the loop from spinning as fast as the CPU allowed -
        confirmed live (one core pegged at 100% the whole time a script
        ran). Fixed with a plain, explicit `sleep_for(20ms)` at the bottom
        of the loop, matching `tool_worker`'s/`io_worker`'s own cadence -
        first case found this session of two *different* features
        independently uncovering the same "this loop has always
        implicitly depended on someone else's side effect for its own
        pacing" fragility.
      - **`[WAIT_FOR_RESULT]`'s own first two implementations were both
        wrong, both found and fixed via live testing against the exact
        race they were meant to solve** (a 30-second timer set earlier in
        `system_test.task`, deliberately timed to still be running when
        the script reaches "what time is it?"): (1) naively taking
        `held_results.front()` grabbed an *earlier*, unrelated command's
        own still-unclaimed result (e.g. "turn off all the lights"'
        confirmation) instead of the clock's answer - fixed by capturing
        how many results were already queued before the wait started
        (`wait_baseline_results`) and only accepting something arriving
        *past* that point, leaving the stale backlog untouched for the
        normal end-of-script replay. (2) That baseline was still captured
        too late - at the moment `[WAIT_FOR_RESULT]` itself is reached,
        which can be several ticks after the triggering command's own
        dispatch - so a fast-answering tool's real result (a local clock,
        answering in about a second) could already be sitting in the queue
        by the time the marker line was reached, getting wrongly folded
        into "already there" instead of recognized as the answer. Fixed by
        capturing the baseline at the one tick `EXECUTE_COMMAND` actually
        runs (inherently transient, always moving straight to
        `WAIT_RESPONSE`) - the exact instant the triggering command's own
        request goes out, not whenever the script happens to reach the
        marker afterward. Separately, checking `held_events` in this same
        wait was *also* wrong even before that timing fix - a completely
        unrelated background event (that same 30-second timer's own
        expiry, unprompted, mid-wait) satisfied the wait instead of the
        clock's real answer, confirmed live. An event is by definition
        unsolicited, never a direct response to anything a script line
        asked for, so `[WAIT_FOR_RESULT]` now only ever watches
        `held_results` - an event landing during the wait just joins the
        normal backlog instead.
      - **Considered and explicitly rejected**: matching a held result to
        its triggering command by the call's own id, instead of by timing/
        position - more robust in principle (sidesteps every race above by
        construction), but the id isn't visible from outside
        `ollama_system::process()`/`dispatch_tool_call()` without touching
        them, which would have broken this feature's own scope (confined
        entirely to `tools.cpp`/`tools_task_script.cpp`, deliberately, per
        the isolation `tools_task_script.*`'s own functions already have).
        Chose to keep the isolation and accept the more intricate
        timing-based correlation instead - a real, acknowledged trade-off,
        not an oversight. Revisit if more edge cases turn up.
    - Both applied to `system_test.task` and live-tested against real
      `ron`/`claude` profile runs, not just compiled.

## Session & model behavior

- **Model repeatedly fails to make use of its own successful tool results
  within the same conversation - observed 2026-09-10, not investigated,
  no fix attempted.** Three independent live instances via `tools/rag/`
  testing (see its TODO entry above for the full detail): (1) a
  `rag_search_documents` call returning 10 real results, one genuinely
  relevant but ranked #4 - the model reported only 6 of the 10, silently
  dropping that one and 3 others; (2) a `rag_search` result that
  genuinely contained the answer (ranked #3 of 5) - the model's answer
  opened with "nothin' on that in the notes" anyway; (3) `rag_get_document`
  successfully returning a real document's full content *twice* in one
  session - the model's final summary a few turns later claimed "no
  [x] info in that file," ignoring both successful retrievals. In all
  three, the *data* was correct and delivered correctly - the model just
  didn't reliably use or report what it already had.
  - Leading hypothesis, not confirmed: `sidetrack.cpp`'s background
    `sidetrack-second-guess`/`sidetrack-consolidate` passes (seen live in
    `debug_full_history.txt`) review and compact history - if one ran
    between a successful retrieval and a later summary, it may be
    compacting away the detail that a specific call succeeded while
    keeping nearby failed attempts. Checkable but not yet checked -
    would mean reading `sidetrack.cpp`'s actual consolidation logic, not
    a quick look.
  - Alternative, equally plausible: plain LLM unreliability synthesizing
    across a long conversation with several tool calls and real errors
    interspersed - not fixable by engineering anything, a model
    limitation, not a bug.
  - Deliberately not chased further for now - it hasn't broken anything
    `tools/rag/` built (every result was still fetched/stored/searched
    correctly), it's broken the model's *summary* of what was built. If
    pursued, treat as its own separate investigation into
    `sidetrack.cpp`, not part of the RAG work.
  - **A sharper, related instance found live 2026-09-16** - not "failing
    to use" a result, but confabulating an entirely fake one. Asked to
    search notes for automotive content, the model got back 10 real
    `rag_search_documents` results (mostly irrelevant `conversations`
    noise - see the RAG entry's low-information-chunk fix above) and,
    instead of relaying them, replied with a "# Tools Used Summary"
    listing `check_presence`/`set_hue_light`/`manage_hue_scenes`/
    `consult_expert` - **none of which were called that turn**. Every one
    of those was mentioned somewhere *inside* the retrieved historical
    chat excerpts (an old conversation about lights/presence/scenes, an
    old "why is the sky blue" joke). Not the already-known "acted on a
    quoted command" risk (the 2026-09-10 mitigation, `rag_tool.cpp`'s
    "historical, don't act on it" note, is working as intended here -
    nothing got *executed*) - a different failure mode: it mistook quoted
    history for a report of its own current actions and answered a
    fabricated question instead of the real one. `integrate_tool_result()`
    's `[DIRECTOR_NOTE]` prompt (`olla.cpp:191-205`) and `rag_tool`'s
    historical-content note (`rag_tool.cpp:213-225`) both say "don't act
    on this" but neither says "this isn't a report of what you just did."
    Likely compounded by the same volume issue as the RAG ranking bug
    above - a wall of transcript-shaped, tool-action-sounding text is
    exactly the setup for this. **Deferred deliberately**: agreed to fix
    the RAG ranking issue first (less noise reaching the model in the
    first place) and re-evaluate whether this still reproduces before
    deciding whether the historical-content note also needs hardening
    (e.g. explicitly disclaiming "not a report of your own actions") -
    not yet revisited.
- **Repeat-bug root cause found and fixed 2026-08-27, awaiting real-world
  confirmation** - full detail in the `olli-time-repeat-bug` memory entry
  (not duplicated here), short version: `SIDETRACK_CHAT_INSTANCE` was
  silently inheriting `LOAD_SAVE_HISTORY_ON_DISK=true` and the real profile
  directory from `chat.PROPS` (`SIDETRACK_CLASS::create()`), and unlike
  every other secondary `ollama_system` instance, never went through the
  `open(Properties)` overload that forces that off. Every completed
  second-guess review tick was auto-saving `SIDETRACK_CHAT_INSTANCE`'s own
  private, in-progress scratch history straight over the shared
  `history.json`, bypassing the actual fold-or-discard decision entirely -
  confirmed directly (a fold correctly skipped in memory still showed up
  duplicated on disk). Fixed by forcing `LOAD_SAVE_HISTORY_ON_DISK=false` on
  that instance (`sidetrack.cpp`). The word-overlap fold guard from the
  2026-08-26 entry (see memory) was also re-implemented - it was never
  actually committed to git, had to be rewritten from scratch - and now
  works correctly since it isn't racing the disk-write bug anymore. Tested
  clean across 3 repro attempts of the original catchphrase-lock pattern.
  `repeat_penalty` (below) is unchanged/still in place, no isolated evidence
  either way on its own contribution. **Not yet confirmed against the
  original real-world symptom** - several past fixes measurably worked for
  what they targeted without ending the actual complaint, so treat this as
  strong-but-unconfirmed until it survives normal use.
- **`repeat_penalty` added 2026-08-26, still in place, still not isolated-
  tested** - `OLLAMA_SYSTEM_PROPERTIES::repeat_penalty` (olla.h, default
  `1.3` vs. Ollama's own bare default of ~1.1 when the option is omitted)
  is sent as `options.repeat_penalty` on every request
  (`ollama_system::send()`, olla.cpp). Added to test a real finding: a raw
  wire-level capture showed the model's *primary* response locking onto a
  short phrase and repeating it verbatim across several unrelated follow-up
  turns, independent of sidetrack's review. Untouched during the 2026-08-27
  fix above, so no data yet on whether it's pulling its own weight
  independent of the disk-write bug fix.
- **Presence tool: fixed 2026-08-27** - both known issues resolved and
  live-tested. (1) `handle_identity()`'s `last_fired_state` reset (found
  2026-08-26, reverted that session, reapplied now) is now guarded on the
  profile actually changing, not fired on every reconnect. (2) Detection
  logic (`combine_states()`, presence.cpp) changed from AND/AND (both
  backends must agree for either HOME or AWAY) to OR/AND: HOME fires as
  soon as *either* Bluetooth or Wi-Fi independently debounces to HOME
  (quick to notice someone's back), AWAY still requires *both* to agree
  (conservative about declaring the house empty - one flaky backend miss
  shouldn't read as "left"). Confirmed live: Bluetooth alone pushed Combined
  to HOME while Wi-Fi was still debouncing, and a later Bluetooth miss
  didn't flip Combined off HOME since Wi-Fi still had it.
  - **Two more real bugs found and fixed 2026-08-28, live-tested against
    the user's own real phone/home setup** (root-caused from `ron`'s
    `debug_full_history.txt`, which showed rapid home/away flapping
    followed by a permanently jammed `check_presence` - see the
    `tool_calls_this_turn` entry below for the second half of that
    symptom). (1) **Debounce was symmetric, should only ever have applied
    to the disconnect side** - `BackendTracker::record()` required
    `home_debounce_hits` consecutive hits before flipping a backend to
    HOME, same as AWAY's miss-count. A real detection can't be a false
    positive (nothing responds to an `l2ping`/answers an ARP probe from
    empty air), so debouncing HOME only delayed genuine arrivals and let
    the two backends disagree long enough to flap. Bluetooth already had
    an ad-hoc single-hit-HOME exception (`bt_tracker.record(bt_present, 1,
    ...)`) with a comment explaining why; Wi-Fi didn't. Fixed by making
    HOME unconditional-on-one-hit for *both* backends (removed
    `home_debounce_hits` from `PresenceSettings` entirely - fully dead
    once both backends work the same way) and leaving AWAY's
    `away_debounce_misses` debounce as the only debounce that exists now,
    still per-backend, still required from both backends independently
    (`combine_states()`, unchanged) before AWAY fires overall. (2)
    **Startup identity race, not occasional - guaranteed on every single
    connect.** `last_poll` starts at `time_point{}` (epoch), so the very
    first poll tick always fires before there's been time for even a
    localhost round trip to olli and back for the "identity" reply - that
    first poll ran under the shared/no-profile settings, reported the
    event as "Someone" (not the real profile name, no configured action)
    with whatever real signal it found, then fired a SECOND, duplicate
    event once identity actually arrived and `profile_changed` reset
    `last_fired_state`. Fixed with a new `identity_received` bool (false
    until `handle_identity()` sets it, cleared again in
    `reset_to_default_profile()` on disconnect) gating the transition-fire
    block in `main()`'s poll loop. Confirmed on a real restart: exactly
    one correctly-attributed event now, no "Someone" duplicate.
- **`tool_calls_this_turn` never reset for a background-pushed event, so it
  eventually jammed every future tool call for the rest of the session -
  found and fixed 2026-08-28.** The other half of the presence-flapping
  symptom above: `dispatch_tool_call()` (`tools.cpp`) caps tool calls at
  `PROPS.max_tool_calls_per_turn` (4) per "turn," where a turn's boundary
  is defined as "since the last `role=="user"` `send()`" (`olla.cpp`).
  That definition only accounted for a real user message or a task-runner's
  next scripted command - it didn't account for a pushed remote-tool
  `event` (`TOOL_REMOTE::monitor_tool()`, `remote_tools.cpp` - a presence
  transition, a timer expiring), which is just as much a fresh,
  externally-initiated topic as either of those, but shared the same
  never-reset counter. An idle session accumulates this count across every
  unrelated background event it ever receives, and once four had gone by
  (trivial for a flapping sensor), every subsequent tool call - including
  a completely unrelated, legitimate `check_presence` - started getting
  silently rejected with "Too many tool calls this turn," for the rest of
  the session, with no recovery short of a real user message. Fixed by
  resetting `tool_calls_this_turn = 0` at the top of the `event` branch in
  `TOOL_REMOTE::monitor_tool()`, before `integrate_tool_result()` runs -
  the original per-event runaway-chain guard (the actual bug it was built
  for - see `tool_calls_this_turn`'s own comment in `olla.h`) is untouched,
  since the model chaining tool call after tool call off its own
  DIRECTOR_NOTE reply to one event still runs through `dispatch_tool_call()`
  and still hits the cap within that single event's chain. Confirmed live
  against `ron`'s real airplane-mode test: zero cap errors on either
  transition.
- **`debug_full_history.txt`'s per-line format standardized 2026-08-28** -
  builds on the instance-labeling capability from 2026-08-27 below.
  `debug_log_message()`/`debug_log_instance_event()` (`helper_olli.cpp`)
  now both funnel through one shared `write_record()`, so every entry -
  message or instance created/closed marker, from any instance - gets the
  identical shape: a `=== <instance_label> / <role-or-EVENT> ===` header, a
  millisecond timestamp, a `Content:` line, and a closing rule
  (`------------------------------------`, matching `history_write()`'s
  own existing rule-line convention in `olla.cpp` rather than inventing a
  second style). Fixes a real readability problem the old one-line format
  had: multi-line content (a DIRECTOR_NOTE, or `sidetrack-consolidate`'s
  own summarization prompt, which quotes older `[role]: content` verbatim)
  used to be visually indistinguishable from a real per-message header on
  the line right above or below it - confirmed as a genuine source of
  confusion while reading a real session's log during the presence-bug
  investigation above.
- **Sidetrack rewrite: a first attempt was made and reverted 2026-08-28 -
  nothing kept, but worth knowing before trying again.** Wrote a
  `SIDETRACK_CLASS_V2` skeleton (`sidetrack.h`/`.cpp`, currently commented
  out) dropping the background-thread design entirely, and started moving
  `COMMS` off `ollama_system` as an owned member toward `main.cpp` owning
  the one real instance instead (in the spirit of `tools_list`'s existing
  move to a reference parameter - see the tools rework entry above) -
  including dropping `COMMS::audio` on the theory that audio should only
  ever be touched by `IO_WORKER_CLASS::thread_main()`, not carried on
  `COMMS` at all. Both changes were left half-wired (`ollama_system`'s
  many internal `comms.*` call sites in `olla.cpp` were never updated to
  match, so it didn't compile) and were reverted back to the pre-attempt
  state (`git restore`) rather than pushed further, after finding enough
  else "messed up" mid-edit to want a clean restart. Whether `COMMS`
  should move off `ollama_system` the way `tools_list` did is still an
  open, reasonable-sounding direction - it just wasn't carried through far
  enough this attempt to know if it's right. Next attempt should decide
  that up front rather than discovering it mid-rewrite: does every
  `ollama_system` method that touches `comms` today (`log()`,
  `write_to_tts()`, `input()`, `send()`'s streaming loop,
  `spawn_background_task()`) take it as a threaded-through reference
  parameter (matches `tools_list`'s precedent exactly, but touches a lot of
  signatures), or does `ollama_system` keep a `COMMS*` pointer member set
  once (mirrors how `COMMS::audio` itself already worked, far smaller
  diff)?
- **Sidetrack is planned for a rewrite - context to carry forward.** Decided
  2026-08-27, during the repeat-bug investigation above. Relevant going in:
  - The disk-write bug and fold-guard fix above are real, confirmed, and
    worth keeping regardless of when/how the rewrite happens - don't
    rediscover them from scratch.
  - A new debug-logging capability landed the same day, built specifically
    to make sessions like this legible: `debug_full_history.txt`
    (`debug_log_message()`/`debug_log_instance_event()`, `helper_olli.h`/
    `.cpp`) now tags every line with which `ollama_system` instance
    produced it (`ollama_system::debug_label`, olla.h - "chat",
    "sidetrack-review", "sidetrack-consolidate", "task-runner:<intent
    phrase>") and brackets each instance's lifetime with an instance
    created/closed marker. Previously every instance's output was
    interleaved and indistinguishable in that file. Use this first when
    investigating sidetrack behavior for the rewrite - it should make it
    much easier to see exactly what the review pass sent/received without
    cross-referencing timestamps or guessing from content alone. **Format
    standardized further 2026-08-28** - see its own entry above
    (`write_record()`, `helper_olli.cpp`) for the ruled-header/timestamp/
    `Content:` shape every entry now shares.
  - Explored giving sidetrack's review real access to the main `tools_list`
    (so it could actually execute a tool call it decides is warranted, not
    just comment) - backed out of before implementing anything. Two real
    problems, not just one: sharing the same `std::vector` across sidetrack's
    background thread and the main thread risks a real data race (a
    `push_back` from a new remote-tool registration could reallocate the
    buffer mid-iteration on the other thread); and remote tools like `hue`
    maintain exactly one blocking, non-multiplexed connection - we already
    saw a single thread issuing two overlapping calls produce a corrupted
    "unexpected response from remote tool" mismatch, so two threads doing
    it would only make that worse, not new but more frequent. Whatever the
    rewrite does here, this needs a real answer, not just wiring the
    reference through.
  - The user found a separate, distinct problem in the second-guess routine
    while testing 2026-08-27 (not detailed here - flagged as needing more
    thought before the rewrite, not written up yet). Ask before assuming
    it's the same class of issue as anything above.
  - Jump instances (`jump_input()`'s "I'm home."/"I'm leaving."/etc.
    phrase-triggered Hue-scene block, olla.cpp) were removed entirely
    2026-08-27, along with `TOOL_PERMISSIONS_CLASS` (see the tools rework
    entry above) - neither was ever actually used. `jump_input()` itself
    stays, now handling only "bye"/"quit"/"Goodbye." exit phrases.
- **Pre-load the model at startup** - right now the model only loads on the
  first message; see if Ollama's keep-alive/preload mechanism (an empty
  `/api/chat` request, or similar) can warm it up during olli's own startup
  instead, so the first real message doesn't eat the load time.
- **Actively save chat history through the session** - make sure history is
  reliably persisted as the session goes, not just on the periodic
  size-changed check in `ollama_system::process()`.
- **Investigate concurrent sessions** - what actually happens to Voca and
  Lira/TTS if one olli session is already running and a second one starts?
  We hit real contention this session (a second instance hung waiting on the
  mic) - worth understanding and documenting properly, maybe guarding
  against it explicitly.
- **Unexplained crash, seen twice now** - same shape both times: normal
  usage, then a long idle stretch (hours, in a detached `screen` session),
  then a brief interaction, then the process is just gone - no error
  visible (the `screen` window itself closes when its process exits, and
  neither time left a core dump - `ulimit -c` is 0 by default here). No
  confirmed root cause. Investigated 2026-08-23 from the surviving
  `history.json`/`chat_log.txt` of the second occurrence: found and fixed
  one real latent bug while looking (`curl_global_init()` was never called
  anywhere - `TOOL_WEB_SEARCH`/`TOOL_HUE`, tools.cpp/tools_helper.cpp, both
  call `curl_easy_init()` directly, which makes libcurl do its own lazy
  global init on first use - libcurl's own docs say that path isn't thread-
  safe. Now called once in `main()` before any thread that could touch curl
  spawns), but couldn't confirm it's actually what caused either crash - the
  timing in both cases doesn't obviously line up with two threads' first
  curl call racing. If it happens a third time: enable core dumps first
  (`ulimit -c unlimited` before launching, or a persistent
  `/etc/security/limits.conf` entry) so there's an actual stack trace to
  work from instead of just the conversation log.
  - **Mitigated 2026-08-23**: `main()` (main.cpp) is now a crash supervisor -
    the actual program body moved to `main_process()`, wrapped in a top-level
    try/catch (converts an uncaught C++ exception into a clean logged return
    instead of an uncatchable `std::terminate()`/`SIGABRT`), and `main()`
    `fork()`+`execv()`s it as a child, restarting on any abnormal exit
    (crashed or non-zero return) rather than a clean one. `execv()`
    specifically, not a plain `fork()` or function call - a genuinely fresh
    process image each time, so a crash caused by memory corruption doesn't
    ride along into the "fresh" restart. `RLIMIT_CORE` is raised once in the
    supervisor (inherited across `execv()`) if it was 0, so a crash finally
    leaves a real core file without needing to remember `ulimit -c
    unlimited` by hand. Crash-loop protection: gives up after 3 restarts
    within 30s rather than spinning forever, with a clear message. Doesn't
    fix the underlying crash (still not diagnosed) - just stops it from
    silently ending the session, and a `--crash-restart` marker makes a
    recovery visible in the chat log (`[System] Recovered from a previous
    crash - starting fresh.`) instead of invisible. A `--debug-crash` flag
    (deliberately segfaults 5s into a real run, undocumented in
    `--help`) exists to test this without waiting for a real crash - tested
    the crash-loop-and-give-up path for real already (this sandbox has no
    real TTY, so ncurses' own fatal exit on a bad `$TERM` triggered it
    naturally: 3 clean restarts, each with a genuinely fresh re-init
    - settings/audio/whisper all reloading from scratch - then a correct
    give-up). The terminal (raw mode / ncurses alt-screen) is NOT restored
    by the supervisor if a crash leaves it in a bad state - known,
    deliberately deprioritized (finding the actual bug matters more).
    Confirmed with the user's own real `--debug-crash` run (2026-08-23):
    crashed, restarted, crashed, restarted, gave up correctly on the 3rd.
    Also found: `RLIMIT_CORE` alone isn't enough on this machine - Ubuntu's
    `apport` (the actual `core_pattern` handler, confirmed active and
    otherwise working) silently drops crashes from any binary that isn't
    from an installed package unless `unpackaged=true` is set, so a locally-
    built binary like `olli` produces no `/var/crash/` entry at all by
    default. Rather than depend on that (system-specific, not something
    olli controls), the supervisor now also writes its own persistent
    `crash_log.txt` in the profile's `olli_files_<name>/` directory directly
    - durable regardless of terminal/screen state or OS crash-reporting
    config, which core dumps and `std::cerr` messages both are not.
  - **Still unexplained as of 2026-09-17** - kept recurring this week
    (`ron`'s `crash_log.txt`: 3x on 2026-09-13, 2026-09-16 17:12,
    2026-09-17 00:47, and twice back-to-back at 01:29:39, which is what
    actually took olli down overnight - it sat dead until manually
    restarted ~2.5 hours later since the third auto-restart attempt never
    got logged as a further crash, so something outside olli itself must
    have relaunched it). All of these are `SIGABRT` (signal 6), not
    `SIGSEGV` - consistent with glibc detecting heap corruption
    (double-free / invalid pointer) or a hard assert, not a slow leak.
    Still no stack trace, since apport's unpackaged-binary gap (above)
    was never actually worked around.
  - **Backtraces added 2026-09-17**: `main()` now installs a real signal
    handler (`crash_signal_handler`/`install_crash_handler`, main.cpp) for
    `SIGSEGV`/`SIGABRT`/`SIGFPE`/`SIGILL`/`SIGBUS`, registered fresh on
    every `execv()`'d restart same as `SIGPIPE`'s `SIG_IGN` just above it.
    On a crash it writes a `backtrace_symbols_fd()` dump straight into
    `crash_log.txt` via a raw fd opened ahead of time in
    `install_crash_handler()` - no `std::ofstream`/`std::string` work
    inside the handler itself, since the heap or libc's own locks could be
    in a corrupted state exactly when this fires (see `man 7
    signal-safety`). `backtrace()` is warmed up once at startup so its
    first real (lazy-allocating) call isn't the one happening mid-signal.
    After writing the backtrace it restores the signal's default
    disposition and re-raises, so `main()`'s supervisor still sees a real
    `WIFSIGNALED()`/`WTERMSIG()` termination and its own crash-loop
    logging/give-up behavior is unchanged. `source/CMakeLists.txt` also
    now adds `-g` (debug info, zero runtime cost) and `-rdynamic` (so
    `backtrace_symbols_fd()` can resolve olli's own function names, not
    just bare addresses) to the non-MSVC build. Tested with the existing
    `--debug-crash` hook run inside a real tmux pty (the sandbox's fake
    `$TERM` masks it, same gotcha noted in the 2026-08-23 entry above):
    all 3 crash-loop attempts produced a correct backtrace pointing at
    `main_process()`, and the give-up-after-3 behavior was unaffected.
    Installed to `~/olli` via `install_to_home.sh`; the already-running
    `ron`/`claude` sessions won't have this until their next restart
    (crash or manual) picks up the new binary. Function names in the
    trace are still C++-mangled (e.g. `_Z12main_process...`) - pipe a
    line through `c++filt` to read it; not demangled in the handler
    itself since `abi::__cxa_demangle()` allocates, which is exactly what
    the handler otherwise avoids. Root cause of the `SIGABRT`s themselves
    is still not diagnosed - this only means the *next* one leaves real
    evidence instead of just a signal name.
  - **Root cause found and fixed 2026-09-22, for a separate `SIGSEGV`
    (not the `SIGABRT`s above) - `TOOL_WORKER_CLASS`'s own
    `INTERUPTED`/`PROCESSING` rendezvous had a real gap.** The new
    backtrace handler (above) caught two identical real crashes, 4 days
    apart, both inside an `nlohmann::json` copy constructor reached from
    `TOOL_WORKER_CLASS::thread_main()`'s own `registered_tool_defs =
    manual_tool_defs;` (`tool_worker.cpp`) - a classic torn read: one
    thread mutating the same `json`/`std::map`/`std::vector` storage
    while another copy-constructed from it. A second, independent agent
    (no access to the first one's hypothesis) investigated the same two
    backtraces from scratch and reached the identical root cause, which
    is what gave real confidence before touching anything.
    - **Why the original rendezvous (see the tool_worker + internal
      tools restoration entry, 2026-09-14/15 above) wasn't actually
      enough**: it only ever excluded `thread_main()` from *one* caller
      at a time - `PROCESSING` is exclusively set by `thread_main()`
      itself, never by a caller, and `INTERUPTED` is a single shared
      flag any finishing caller clears unconditionally, with no
      awareness of whether a *different* caller thread is still
      mid-access. Traced as genuinely reachable, not theoretical: the
      main loop's own tick, a task-runner script's own spawned
      `chat_thread` (`tools.cpp`), and `TOOL_DELEGATOR`'s own spawned
      `chat_thread` all call `tool_worker`'s accessors independently -
      two of those overlapping is what actually crashed it live.
    - **Fix: `state_mutex` (a real `std::mutex`), replacing the
      `INTERUPTED`/`PROCESSING` atomics pair entirely** - every accessor
      (`put_pending_call()`, `get_pending_result()`,
      `get_pending_event()`, `get_registered_tool_defs()`,
      `add_manual_tool_def()`, `set_identity()`) and `thread_main()`'s
      own tick now just take `state_mutex` for as long as they touch
      shared state, instead of doing the busy-wait dance. A deliberate,
      explicitly-discussed exception to "no program-wide mutexes" (the
      original constraint for this whole rewrite, see the 2026-09-14/15
      entry) - raised openly rather than slipped past it: the
      alternative (a lock-free atomic-swap/RCU-style publish for the
      `json` snapshot, a real lock-free MPMC queue for the FIFOs) avoids
      blocking entirely but is meaningfully more new, hand-rolled
      complexity in exactly the area that just produced a subtle
      concurrency bug, for critical sections that are short and
      low-contention (not a hot path) - blocking costs nothing
      measurable here, and a mutex is far easier to verify correct by
      inspection. User's own call, made with that tradeoff in front of
      them.
    - Verified live: clean build under `-Wall -Wextra -Wconversion`
      etc., a real interactive session with tool registration/dispatch/
      streaming all confirmed working, including deliberately exercising
      the exact concurrent-access shape that used to race (a
      task-runner script's own instance running while the main chat and
      sidetrack's second-guess pass are also active) - no deadlock, no
      hang, no crash.
    - **Not yet deployed to `~/olli`** as of this entry - confirm
      `install_to_home.sh` has actually been run before assuming a live
      session has this fix.
    - **Separate, pre-existing bug found incidentally while testing
      shutdown - root-caused and fixed 2026-09-22.** A clean `bye`/`quit`
      exit could abort, confirmed reproducing on the *old*, pre-mutex
      binary too, so unrelated to the mutex fix above - just surfaced by
      the same testing pass. Two independent sources of the identical
      `std::terminate`/`SIGABRT` crash (same backtrace signature both
      times), both the same structural gap: `ollama_system::
      request_exit()` (`olla.cpp`) already does exactly the right thing
      - interrupt any in-flight response, then join `chat_thread` - but
      it was only ever called on the *main* `chat` object
      (`main.cpp`). Two other things own their own `ollama_system`
      (and thus their own `chat_thread`) and got no such treatment:
      1. **`chat.background_tasks`** - each task-runner automation or
         `consult_expert` delegation is its own instance. Normally
         `process()`'s PART 2 joins a finished one's thread each tick,
         but once `bye` sets `running = false`, the main loop never
         calls `chat.process()` again, so that cleanup simply stops
         running. Anything still mid-response at that exact moment sat
         there, still joinable, until `chat` itself was destroyed at the
         end of `main_process()`.
      2. **`sidetrack`'s own `SIDETRACK_CHAT_INSTANCE`** (`sidetrack.h`)
         - same shape: its own `chat_thread`, spawned per second-guess/
         consolidate cycle, never explicitly joined at shutdown
         (`sidetrack.thread_stop()` was commented out in `main.cpp` - a
         stale leftover from when sidetrack had a dedicated thread of
         its own, which it hasn't since the 2026-08-29 rewrite; removed
         as dead code in this same change). If a review pass happened to
         still be streaming when `bye` landed - which can happen at
         essentially any moment, since second-guess runs automatically,
         unprompted, after nearly every turn - same crash.
      - **Fix**: two small new public methods, each just looping the
        already-correct `request_exit()` over what they own -
        `ollama_system::shutdown_background_tasks()` (`olla.h`/`.cpp`)
        and `SIDETRACK_CLASS::shutdown()` (`sidetrack.h`/`.cpp`). Both
        called from `main.cpp`'s shutdown sequence, *before*
        `io_worker.thread_stop()`/`tool_worker.thread_stop()` - so
        anything still mid-flight gets torn down while its dependencies
        are still alive, not after. No new state, no new locking -
        `request_exit()`'s own existing logic (already used correctly
        for the main `chat` instance) is just reused per-instance.
      - **Verified live**: ~9 rapid "chat, then immediately `bye`"
        attempts across several fresh sessions, deliberately timed to
        land while a sidetrack review pass was confirmed actively
        streaming (polled `debug_full_history.txt` for "review started"
        before sending `bye`) - every attempt exited cleanly (`chat /
        EVENT: instance closed`, no new `crash_log.txt` entry), where
        the identical timing reliably crashed before this fix.
- **Found and fixed 2026-09-22: `tool_worker`'s shared result/event queue
  had no correlation to which conversation was actually waiting - three
  independent live bugs, all one root cause.** Found via a 90-minute
  exploratory testing round (following the pattern from the 2026-09-16/17
  rounds): `cancel_timer` silently producing no result under rapid-fire
  input, a task-runner's "turn off all the lights" line firing its action
  but narrating it as the answer to a later, unrelated `[ASK]` prompt, and
  a task-runner's own timer event leaking into the main chat after the
  script that set it had already closed - three different-looking
  symptoms, traced back to one thing: `TOOL_WORKER_CLASS`'s
  `pending_results`/`pending_events` (`tool_worker.h`) are a single shared
  queue, and every `ollama_system` instance that shares this one worker
  (main chat, a task-runner's own instance, a delegate's own instance)
  used to just claim whichever result was oldest, with nothing recording
  which instance actually dispatched which call.
  - **Phase 1 - results, fixed and shipped**: `TOOL_RESULT` already
    carried `call_id` (`remote_tools.h`) - the data needed for correct
    correlation already existed, it just wasn't used. Fix: `ollama_system`
    gained `outstanding_tool_call_ids` (`olla.h`) - ids this instance has
    dispatched but not yet claimed a result for, pushed in
    `dispatch_tool_call()` (`tools.cpp`) right before `put_pending_call()`.
    `TOOL_WORKER_CLASS::get_pending_result()` gained an id-aware overload
    (`tool_worker.h`/`.cpp`) that claims one specific call's result
    instead of blindly popping the front; `process()`'s own PART 5
    (`olla.cpp`) and the task-runner's own `DELAY_TOOL_RETURNS` pre-drain
    loop (`tools.cpp`, `TOOL_TASK_RUNNER::handle_tool()`) both switched to
    it, so an instance only ever claims its own. No global state, no new
    mutex - `outstanding_tool_call_ids` is a plain per-instance member;
    the existing `state_mutex` (2026-09-22 entry above) already covers
    the new id-aware accessor the same way it covers every other one.
    - **Leak found by review, fixed same day**: claiming-by-id meant a
      result nobody ever asks for again just sits in `pending_results`
      forever - and a background task/delegate instance can finish (its
      own script doesn't wait for every call it fired) while one of its
      calls is still outstanding, about to be destroyed with nobody left
      to claim its eventual result. Fixed with
      `TOOL_WORKER_CLASS::abandon_call(call_id)` - marks an id so
      `thread_main()`'s existing per-tick pass drops a matching result
      instead of keeping it, called right before `background_tasks.erase()`
      (`olla.cpp` PART 2) for every id still outstanding on the instance
      about to be destroyed. Bounded: every dispatched call already gets a
      `call_deadlines` entry and is guaranteed *some* eventual
      `pending_results` entry (a real answer or the existing
      `CALL_TIMEOUT_SECONDS` timeout's own synthesized one), so an
      abandoned id can't wait longer than that before being cleaned up.
    - Two independent code-review passes (fresh subagents, no access to
      each other's findings) confirmed: no globals, no new mutexes beyond
      the one sanctioned exception, no signature changes to `process()`/
      `dispatch_tool_call()`/`thread_main()` - only new methods/members
      added, exactly the "functions only call variables passed through
      their own signatures" scope this was built to.
    - **Verified live**: re-ran the exact `cancel_timer`/rapid-fire-input
      repro and `system_test.task`'s "turn off all the lights" line
      end-to-end against the fixed binary - both confirmed correct every
      time, no more stray content masquerading as a different call's
      answer.
  - **Phase 2 - events, data plumbing shipped, behavior change disabled
    pending further investigation.** An event (a timer expiring) has no
    `call_id` at all, unlike a result - nothing asked for it. Design
    (agreed with the user before building): reuse the *originating* call's
    id as the event's own "birth certificate" when one exists (a timer
    remembers the `call_id` of the `set_timer` call that created it,
    stamps it on the eventual expiry event); a reserved sentinel id,
    `EVENT_NO_ORIGIN_ID = "no_origin_call"`
    (`tools/olli_link/olli_link.hpp`, mirrored independently in
    `source/remote_tools.h` - the two builds don't share headers, see
    `tools/PROTOCOL.md`'s "Repo / build layout") for an event with no
    originating call at all (ambient - `presence`). One uniform lookup
    either way, no missing-field special case.
    - Wire protocol: `event` messages gained `origin_id`
      (`tools/PROTOCOL.md`). `OLLI_LINK::send_event()` gained a defaulted
      third parameter, so `presence.cpp`'s existing call site needed zero
      changes - the default *is* the correct value for it.
      `tools/clock/clock.cpp`'s `ActiveTimer` gained `origin_call_id`, set
      from `set_timer`'s own `call_id`, stamped onto the expiry event.
    - `ollama_system` gained `owned_tool_call_ids` (`olla.h`) - every call
      id an instance has *ever* dispatched, for its whole life, unlike
      `outstanding_tool_call_ids` above (never pruned - a remote tool's
      standing state can outlive the immediate call by a long time, same
      "never pruned" tradeoff `active_timers` itself already accepts,
      `clock.cpp`).
    - All of the above is fully wired, code-reviewed, and confirmed
      correct - `EVENT_NO_ORIGIN_ID` verified to match byte-for-byte
      between its two independent copies, `origin_call_id` confirmed
      always populated, both draining sites confirmed to check the
      correct instance's own list.
    - **What's NOT enabled**: actually *using* `owned_tool_call_ids` to
      change how an unowned event gets narrated (`process()`'s PART 5,
      `olla.cpp`; the task-runner's own end-of-script replay,
      `tools.cpp`). A "this wasn't triggered by anything you asked"
      `Special_Instruction` reliably provoked the model into issuing a
      follow-up tool call instead of replying in plain text, far more
      often than the default framing ever did - and that collided, live,
      reproduced 3 of 4 attempts, with a **pre-existing** race: a timer's
      `on_expire_tool` action dispatches through the separate
      `pending_tool_calls` queue (`handle_instance_tools()`, `tools.cpp`),
      while the model's own reactive tool call goes through
      `last_received.tool_calls` - and `pending_tool_calls`' own comment
      in `olla.h` already documented the underlying gap ("`last_received`
      gets reset at the top of every `send()` call, so anything sitting
      in it could be silently dropped if a new turn started first").
      Worse than a dropped call: `integrate_tool_result()`'s own call to
      `send()` is fully synchronous, blocking whichever thread is running
      `process()` on the HTTP round-trip to Ollama - when this went wrong
      live, the *entire program* stopped ticking for minutes, not just
      one narration. Root cause not confirmed - no `ptrace`/`gdb` access
      in the sandbox this was investigated in, only `/proc`-level thread
      state (consistently `futex_do_wait`, not conclusive on its own).
      Reverted to the plain default framing (identical to what's run all
      session without incident) rather than ship something that can
      freeze the whole program. Whoever picks this back up: the `is_own`
      lookup itself (`std::find` against `owned_tool_call_ids`) is
      trivial and already proven correct - the fix needed is on the
      `pending_tool_calls`/`last_received` race and/or making
      `integrate_tool_result()`'s `send()` non-blocking, not in the
      origin_id plumbing itself. A real debugger (not blocked by sandbox
      `ptrace` restrictions) would likely resolve this quickly.
  - **Also fixed same session, unrelated root cause**: `set_hue_light`'s
    own result string embedded the Hue bridge's raw native 0-254
    brightness value verbatim (`tools/hue/hue.cpp`) - the *input*-side
    percent-to-raw conversion was already fixed (an earlier session), but
    nothing converted the *output* back, so a model told to report a
    result "without changing the facts" read a requested 30% back as
    "76%". Fixed by rewriting every `/bri` leaf in the bridge's own
    success-response JSON, in place, using the same
    `bri_to_brightness_percent()` `list_hue_lights` already had. Verified
    live: requested 30% -> bridge reports 30 -> narrated correctly.
- **Found and fixed 2026-09-23: `sidetrack-second-guess` could silently
  override a decision the user had already explicitly made, with a real
  physical side effect and zero trace on screen.** Found via a 90-minute
  exploratory testing round: given a conversation ending on the user's own
  contradictory-but-final decision ("leave the lights as they are"), the
  main chat correctly did nothing - but the second-guess review pass
  (`SIDETRACK_CLASS::run_second_guess()`, `sidetrack.cpp`) then
  autonomously called `set_hue_light(on:false)` on its own judgment,
  physically turning off all the lights, and the code only ever committed
  the model's own *spoken text* to history - when the text came back
  empty (which it did here), the whole thing was logged as "empty answer -
  nothing committed" with no indication anywhere a real user would see
  that a real action had just fired.
  - **Root cause is architectural, not a typo**: `SIDETRACK_CHAT_INSTANCE`
    is deliberately handed the REAL `tool_worker` and the REAL main chat's
    own `comms` (unlike consolidation's own throwaway instance, which gets
    neither) - a tool call it decides to make has genuine real-world
    effects, same as anything the main chat itself dispatches. That's
    intentional (see the design discussion below), not itself the bug.
  - **The actual purpose, per the user**: second-guess exists to combine
    the *speed* of a non-thinking fast reply with the *quality* of a
    thinking one, without paying thinking mode's latency on every turn -
    reply fast, then reconsider in the background and correct if wrong
    ("the answer is blue... wait, green is better"), including actually
    following through on something the fast reply claimed to do but
    didn't. This is a recognized pattern in LLM-agent literature (similar
    to published "Self-Refine"/"Reflexion" techniques) - the user arrived
    at it independently. The *implementation* had drifted wider than that
    scope, though: its actual prompt ("more needed to be done or said?")
    invited it to independently decide *new* things should happen, not
    just correct/complete what it already said - which is what let it
    reach past correcting its own answer into overriding the user's own
    separate decision.
  - **Fix, two parts, agreed with the user before building**:
    1. **Narrower prompts** (three strings in `run_second_guess()`,
       stages 2 and 4) - rescoped to "was your reply accurate, did you
       follow through on anything you claimed - correct it if not, but
       never introduce a new action, and never override a decision the
       user already explicitly made." A wording mitigation, not a hard
       guarantee (still relies on the model following it) - live-tested,
       the exact repro no longer reproduces, though the model still
       occasionally re-issues a redundant (harmless in that case) call;
       what changed is it now explains the inconsistency in the visible
       chat instead of staying silent about it.
    2. **Structural fix for the silence itself**: new
       `SIDETRACK_CLASS::second_guess_actions_taken` (`sidetrack.h`) -
       captures the names of any tool calls actually dispatched, at the
       one tick `last_received.tool_calls` is still populated before
       `handle_instance_tools()` clears it to dispatch (by the time the
       review reaches its own "commit to history" step, that list is
       already empty - the old code had nothing left to report by then).
       Whenever a review pass ends with real action taken but no spoken
       text, a plain fallback note ("(Took action on review:
       set_hue_light.)") now gets committed to the real, visible
       conversation instead of nothing - across all 3 places the pass can
       end. **First review pass on this found a real double-counting
       bug**: the capture condition was missing an `!is_processing` check,
       so it could re-append the same call name across multiple polling
       ticks before the real dispatch actually cleared `tool_calls`
       (`last_received.complete` flips true slightly before
       `is_processing` flips false, inside `send()`, `olla.cpp`) - fixed
       by adding that check to both capture sites, re-reviewed clean.
  - **Second, separate bug found live while re-testing the first fix**:
    `SIDETRACK_CLASS::check()` calls `run_consolidation()` then
    `run_second_guess()` every tick, unconditionally, with zero
    coordination between them - both operate on the exact same shared
    `SIDETRACK_CHAT_INSTANCE` (`history`/`PROPS`/`debug_label`/its own
    `chat_thread`). Second-guess's own review call is asynchronous (spawns
    a real background thread, `run_second_guess()` just polls across many
    ticks while it's in flight); nothing stopped consolidation's own
    synchronous pass (which clears/reconfigures that same instance) from
    *starting* while second-guess's background thread was still actively
    running - confirmed live: a second-guess follow-up call ended up
    dispatched under consolidation's clobbered state/label, firing yet
    another real `set_hue_light` call against already-corrupted context.
    - **Fix**: `run_consolidation()`'s own stage-1-to-2 transition now
      also requires `second_guess_stage == 100` (second-guess's fully-idle
      resting state), not just its own idle timer being ready. The user's
      own idea, chosen over an earlier draft gated on
      `SIDETRACK_CHAT_INSTANCE.is_processing` instead - `is_processing`
      alone leaves a real gap (the moment between second-guess's own two
      calls, where it reads false but the review cycle isn't actually
      done), which `second_guess_stage == 100` closes completely, since it
      only reaches 100 once the *whole* cycle - including any follow-up
      call and its own commit-to-history step - is finished.
      `run_clear_context()` was checked and confirmed to only ever touch
      `main_instance` (the real chat), never `SIDETRACK_CHAT_INSTANCE` -
      not part of this risk at all.
    - **No new mutex** - both routines' own stage variables are only ever
      touched by the single main thread (confirmed by tracing every
      access site); the only actual background thread involved
      (`start_second_guess_call()`'s own spawned `chat_thread`) never
      touches `second_guess_stage` at all. Independently re-verified:
      `run_consolidation()`'s own stage-2 body is one straight-line
      synchronous call with no yield points, so by the time
      `run_second_guess()` even runs on a given tick, consolidation has
      either already fully finished or hasn't started - no partial-
      completion window exists to hit.
  - Both fixes independently code-reviewed (two separate passes, fresh
    agents) and live-tested against the original repro plus a normal
    idle-timer consolidation pass, confirming no regression.
- **Found and fixed 2026-09-23: nothing stopped two copies of the same
  remote tool, or two concurrent `olli` processes, from silently stepping
  on each other.** Same 90-minute testing round: a remote-tool listener
  bind failure left `clock`/`hue` retrying forever with zero error
  anywhere - traced to the user's own concurrent `ron` profile session
  already holding the fixed, hardcoded remote-tool port
  (`REMOTE_TOOL_LISTENER::PORT = 47601`, `remote_tools.h`) - every olli
  process on a machine shares this one port, with no per-profile
  distinction at all.
  - **Two separate problems, two separate fixes, both the user's own
    proposed shape**:
    1. **Two instances of the *same* remote tool** (e.g. two `./clock`
       processes) - fixed with a non-blocking `flock()`-based single-
       instance lock in the shared `tools/olli_link/` plumbing every
       remote tool links against, keyed off the tool's own first
       registered function name (already passed into `OLLI_LINK`'s
       constructor via `register_message` - no new parameter, no changes
       needed to any individual tool's own `.cpp`/`Makefile` at all,
       confirmed by rebuilding all 4 tools - `clock`/`hue`/`presence`/
       `rag_tool` - with zero edits on their end). A duplicate instance
       keeps running (its own local display still works) but `service()`
       never calls `try_connect()` for it - `sock_fd` stays -1 forever.
       The OS releases the lock automatically on exit however the process
       ends (clean, crash, `kill -9`), so there's no stale-lock case to
       handle. Live-tested: started two `./clock` processes against a
       live olli - only the first opened a real socket and served a real
       `get_clock_time` request end-to-end; the second had no socket at
       all and reported "Another instance of this tool is already
       running - not connecting." once, not spammed every tick.
    2. **Two concurrent `olli` processes** (different profiles, or the
       same one twice) - the user's own call: don't try to prevent this
       or add per-profile ports (a bigger redesign), just make the
       *already-graceful* degradation loud instead of silent.
       `REMOTE_TOOL_LISTENER::bind_failed()` (`remote_tools.h`) exposes
       what was already tracked internally; `TOOL_WORKER_CLASS::
       thread_main()` (`tool_worker.cpp`) logs one clear event right after
       construction if it's true. Still not fatal - the losing instance
       just runs on built-in tools alone for its whole session, exactly
       as before - but now says so once, instead of a user only ever
       seeing "Not connected to olli - retrying..." forever on the tool's
       own display with no explanation why. Live-tested: two concurrent
       `olli` processes (different profiles) - the second logged the
       notice once and kept working correctly on built-in tools only
       (confirmed with a real non-tool question).
  - Both independently code-reviewed; the lock fix flagged one cosmetic,
    non-blocking note (treats any non-EWOULDBLOCK `flock()` failure the
    same as "genuine twin," not distinguishing a rare unrelated system
    error) - not acted on, not considered worth the complexity.
- **Found and fixed 2026-09-04: `qwen3:8b` silently refusing to call
  `run_automation_task` for an unfamiliar task name.** After adding a new
  `.task` file (`print test`) to a profile's `scripts/` directory, saying
  "run print test" produced a generic in-persona non-answer instead of the
  automation running - looked at first like a matching bug in
  `TOOL_TASK_RUNNER::handle_tool()`, but `debug_full_history.txt` showed
  the model never even emitted a `[tool_calls: run_automation_task(...)]`
  entry for that phrase, while the exact same session's "run system test"
  produced one immediately. Root cause: `register_tool()`'s schema
  described the tool as being for "a home automation macro" and named
  only the two original task phrases as examples - a task named "print
  test" doesn't sound like home automation, so the model decided up front
  not to try the tool at all, rather than attempting it and getting it
  wrong. Fixed alongside the `TASK_PHRASE`→`TASK_NAME` rename above -
  see that entry (under "Display / OUTPUT_CLASS") for the actual code
  change. Separately, also noticed the model's own canned confused
  response ("you lost the signal, ghost. try again. what do you want?")
  had accumulated 6 verbatim repeats in one profile's `history.json`
  across sessions, and appeared to be reinforcing itself (the model
  reproducing its own past non-answer rather than reasoning fresh) - fixed
  by having `main.cpp` delete the profile's `history.json` at every
  startup (`std::filesystem::remove`, right next to the existing
  `DEBUG_LOG_CLASS::instance().reset()` call for `debug_full_history.txt`,
  same "wipe fresh every launch" reasoning). Deliberate tradeoff, not a
  bug: this profile no longer remembers anything across a restart at all
  - acceptable per the user's own call, since a long-running session still
  gets independent persistence through sidetrack's consolidation path.

## Display / OUTPUT_CLASS

- **Done 2026-09-03: per-instance chat colors, driven by COMMS instead of
  hardcoded in the display function.** `comms.h` gained two `int` fields,
  `INPUT_FROM_LLM_COLOR`/`INPUT_FROM_USER_COLOR` - the ncurses attribute
  value (`COLOR_PAIR(n) | A_DIM`-shaped) each buffer should render with,
  same type `NCURSES_TEXT_PANEL::append()`'s own `attr` parameter already
  takes (user_io.h). Defaults match what was already on screen (grey user
  input via `COLOR_PAIR(1) | A_DIM`, `0`/plain for LLM output) - pulling in
  `<ncursesw/curses.h>` for the macros was a deliberate, flagged tradeoff:
  `comms.h` is included very widely and was previously ncurses-free by
  design, and `PAIR_USER_INPUT_GREY`'s own pair index (1) is now duplicated
  between `comms.h` and `user_io.cpp` rather than shared from one place -
  kept simple on purpose, not revisited.
  - `IO_WORKER_CLASS::exchange()` (io_worker.cpp) copies both fields from
    the real `comms` into its own `comms_buffer` each tick, alongside the
    existing text-buffer relay - plain assignment, not append-then-clear,
    since a color is a current setting, not accumulating content.
  - `display_with_ncurses()` (user_io.cpp) reads `comms.INPUT_FROM_USER_
    COLOR`/`INPUT_FROM_LLM_COLOR` at its two render call sites now, instead
    of a local `user_attr` variable and a bare `0`. `PAIR_USER_INPUT_GREY`
    itself still only exists to `init_pair()` index 1 at ncurses startup -
    nothing renders with it directly anymore.
  - **Distinct colors given to background instances**, so their output
    reads as visually separate from the main chat's white/grey: task-runner
    automations (`TOOL_TASK_RUNNER::handle_tool()`) get bright cyan/yellow
    (pair indices 2/3); delegator sub-agents (`TOOL_DELEGATOR::handle_tool()`)
    get bright magenta/green (pair indices 4/5). Both set on the spawned
    instance's own `instance_comms` right after `spawn_background_task()`,
    same duplicated-index tradeoff as pair 1 above - `user_io.cpp` defines
    and `init_pair()`s all of them, `tools.cpp` references the raw index
    numbers directly (both files already reachable via the same include
    chain, no new dependency).
  - **Delegator's `stream_output` flipped back to `true`** (had been set
    `false` earlier the same session specifically to stop a sub-agent's
    answer from appearing twice - see the TOOL_DELEGATOR entry above).
    With distinct colors now in place, the double appearance actually reads
    as intentional: the live magenta/green stream shows the specialist
    working, the final white/grey narration is the polished answer -
    rather than looking like an accidental repeat, which is what it looked
    like when both were the same color.

- **Done 2026-08-27: right-side tools panel.** `display_with_ncurses()`
  (user_io.cpp) now reserves a fixed-width column on the right (`win_tools`,
  full screen height, hidden below ~43 total columns rather than squeezing
  everything else unreadably thin) listing every currently available tool
  name. `IO_WORKER_CLASS::exchange()` (io_worker.cpp) takes the caller's
  `tools_list` now and copies just the names into its own `tool_names`
  member at the same PROCESSING-wait sync point it already uses for
  `staged` - reuses each tool's own `register_tool()` into a throwaway json
  array rather than adding a separate name-only accessor to `TOOL_BASE`, so
  there's one source of truth for what counts as "available."
- **Done 2026-09-02: task runner (`TOOL_TASK_RUNNER::handle_tool`,
  tools.cpp) display bug, root cause fully resolved.** Originally: running
  `run system test` (2026-08-21) surfaced raw keystrokes (including a
  literal `\r\n` on Enter) echoing straight to the terminal every tick
  while ncurses owned the screen (the local `KEYBOARD_INPUT`'s
  `PROPS.RAW_ECHO` never set to `false`), plus the function's own
  `cout <<` debug prints bypassing the buffer-pull pattern entirely. Both
  are gone now - the whole function was rewritten to a state-machine loop
  driving `comms`/`io_worker.exchange()` like everything else, with no
  local `KEYBOARD_INPUT` or raw `cout` left at all. See the task-runner
  rewrite entry under "Session & model behavior" for full detail.
- **Done 2026-09-04: task runner scripting overhaul - disk-loaded `.task`
  files, `[PAUSE]`/`[ASK]`/`[PRINT]`, dynamic tool description.**
  `TASK_SIMPLE_MANAGER::load_all_task()` (`tools_helper.cpp`) no longer
  hardcodes tasks in C++ - it reads every `*.task` file directly under a
  `scripts_dir` path instead, parsing `NAME:`/`PURPOSE:`/`DIRECTORY:`
  header lines up to a literal `---` separator, then treating every
  remaining non-empty line as one `COMMANDS` entry, verbatim (including
  any `[...]` marker prefix). `TASK_SIMPLE::TASK_PHRASE` renamed to
  `TASK_NAME` throughout (`tools_helper.h`/`.cpp`, `tools.cpp`) to match:
  the file's `NAME:` field is meant to hold the task's bare name with no
  leading verb ("system test", not "run system test") - the earlier
  phrase-based matching required the model to also guess whether to keep
  or drop the verb, which turned out to be a real source of match
  failures (see "Session & model behavior" below).
  - **Loading moved out of the constructor.** `TASK_SIMPLE_MANAGER` used
    to load its (then-hardcoded) list from its own constructor. That
    doesn't work for a real directory path: `TOOL_TASK_RUNNER` (which
    owns a `task_manager` member) gets constructed in
    `populate_default_tools()` (`olla.cpp`), called from `main.cpp`
    *before* `main.cpp` ever sets `chat.PROPS.OLLI_DIRECTORY` - a
    constructor has nothing to read from yet. Fixed by dropping the
    constructor entirely, giving `load_all_task()` a `scripts_dir`
    parameter, and calling it from `TOOL_TASK_RUNNER::configure()`
    (`tools.cpp`) right after `OLLI_DIRECTORY` is set - `configure()` is
    already the existing per-tool hook that runs once `OLLI_DIRECTORY` is
    known (`ollama_system::open()`), so no changes to `main.cpp`'s call
    ordering were needed. Any other tool needing something set up between
    construction and `chat.open()` has the same trap - hook into
    `configure()`, don't rely on a constructor.
  - **Hot reload.** `handle_tool()` also calls `task_manager.load_all_task()`
    fresh at the top of every `run_automation_task` call (not just once at
    startup), and `register_tool()` reloads it too before building the
    tool's JSON schema - an edited/added/removed `.task` file takes effect
    on the very next request, no olli restart needed, whether or not that
    request even mentions automation.
  - **`SCRIPT_STATE`'s switch pulled out of `handle_tool()`** into a free
    function `advance_script_state()` (anonymous namespace, `tools.cpp`),
    taking only `state`/`i`/`current_input`/`found_task`/`instance`/
    `instance_comms`/`tools_list` by reference. Deliberately a free
    function, not a `TOOL_TASK_RUNNER` member - it structurally can't
    reach `chat`/`comms`/`tc_id`/`io_worker`/`task_manager`/
    `OLLI_DIRECTORY` even by accident, so the switch can keep growing new
    command types without `handle_tool()` itself bloating or picking up
    coupling it doesn't need. `SCRIPT_STATE` itself moved to the same
    anonymous namespace (was previously declared inside `handle_tool()`)
    so it can appear in the function's signature.
  - **Markers renamed and one added.** `[[ENTER TO CONTINUE]]`/`[[ASK]]`
    (double-bracket, inconsistent single-word-vs-phrase style) became
    `[PAUSE]`/`[ASK]` (single-bracket, both single keywords). New:
    `[PRINT]<text>` - displays `<text>` verbatim (no separator bar, no
    label, so many `[PRINT]` lines in a row read cleanly with nothing
    between them) and immediately advances to the next command - no LLM
    call, no waiting. Structurally different from `[PAUSE]`/`[ASK]`: those
    hand off to a `WAIT_*` state that increments `i` once the wait
    resolves, but `[PRINT]` has nothing to wait for, so it increments `i`
    itself right inside the `GET_COMMAND` branch instead.
  - **Two display bugs fixed along the way.** `[ASK]`'s `REQUEST:` line
    used to include the literal `"[ASK]"` marker text in what got shown
    on screen (`"REQUEST: " + command`, with `command` still carrying the
    unstripped prefix) - fixed with `command.substr(5)`. Separately, the
    `"INPUT: ..."` display line moved from `GET_COMMAND`'s plain-command
    branch to `EXECUTE_COMMAND` (built from `current_input`, not
    `command`) - it now uniformly shows whatever's actually about to be
    sent to the LLM, whether that's a plain script line or a typed
    `[ASK]` answer, where previously a typed `[ASK]` answer was sent with
    no display at all.
  - **`register_tool()`'s schema stopped hardcoding examples.** It used
    to describe `run_automation_task` as being for "a home automation
    macro" and list the two original task names as its only examples -
    directly caused the model to refuse even attempting the tool for a
    new task whose name didn't resemble either example or "home
    automation" (confirmed via `debug_full_history.txt`: no
    `[tool_calls: run_automation_task(...)]` entry at all for the
    refused attempts, vs. a clean one for a phrase matching an example).
    Description is now generic and explicitly tells the model to attempt
    the call even on an uncertain guess, since a miss is safe; separately,
    `intent_phrase`'s own parameter description is now built at
    `register_tool()` time from the live `task_manager.TASK_LIST`, so the
    model sees the real current task names without those names being
    hardcoded anywhere - deliberately *not* done by dumping the full list
    into the always-present top-level tool description too, since that
    cost would scale with however many tasks exist regardless of
    relevance; instead `handle_tool()`'s "no automation found" error
    result also echoes the same live list, so that cost only applies on
    an actual miss.
  - Sample `.task` files (`system test`, `process resume`, `print test`,
    plus an original creative one, `fitter status`) now live under
    `sample_scripts/` in the repo - copy into a profile's own `scripts/`
    directory to use them; `load_all_task()` only ever reads from a
    profile's `scripts/`, never from `sample_scripts/` directly.
- **Filter tool calls and other non-conversational text out of the chat
  log** - `OUTPUT_CLASS::append_to_chat_log()` (user_io.cpp) just logs
  whatever flows through `chat_response`, same as the screen shows. Seen
  in testing: a malformed tool call streamed as plain content instead of
  landing in the structured `tool_calls` field (`response_buffer += c;`,
  olla.cpp ~line 1317, vs. the proper `tool_calls` handling right below
  it at ~line 1321) - showed up as a literal
  `<tools>{"name": "get_current_time", ...}</tools>` line in the log.
  The log should probably strip that kind of artifact even where the
  on-screen display doesn't bother.
- **Done (2026-08-26)**: keyboard input and screen drawing moved off the
  main thread onto their own `IO_WORKER_CLASS` (`io_worker.h`/`.cpp`) -
  see "What we built today: IO_WORKER_CLASS + COMMS" below.
  `AUDIO_CONTROL_CLASS`'s own coupling (it still owns both Voca and TTS,
  still polls `tts.isSpeaking()` to pause/resume Voca) was deliberately
  left as-is per the note this replaces - the worker just became the sole
  poller of `popVocaEvent()` instead of `main.cpp`, not a relocation of
  `voca`/`tts` themselves.

### What we built earlier: the buffer-pull pattern (superseded 2026-08-26)

**Update 2026-08-26**: the three loose members this section describes
(`response_buffer`/`thinking_buffer`/`log_buffer` directly on
`ollama_system`) got bundled into one `COMMS comms` member instead
(`comms.h`/`.cpp`, gained a fourth buffer `tts_buffer` and an `audio`
pointer along the way) - `OUTPUT_CLASS::get_response()` now takes
`COMMS&`, not `ollama_system&`. The *shape* described below (pull, not
push; producer never knows a consumer exists) is unchanged and still the
reason this works - see "What we built today: IO_WORKER_CLASS + COMMS"
below for what actually changed and why.

The problem this solved: `ollama_system::send()` streams a response on its
own background thread (`chat_thread`), but the code that displays things to
the user (`OUTPUT_CLASS`, living in `CLASS_SYSTEM`/`main.cpp`) has no
reliable way to reach into a `send()` call in progress - especially for
instances with no path back to `CLASS_SYSTEM` at all (sidetrack's
`SIDETRACK_CHAT_INSTANCE`, task-runner automation instances). Threading a
pointer/reference into `send()` itself (or further, into every place
`ollama_system` gets constructed) was the alternative, and would have meant
real coupling in a direction that doesn't exist today.

The shape that solved it instead, entirely on the *producer* side:

1. `ollama_system` grew three plain public `std::string` members -
   `response_buffer`, `thinking_buffer`, `log_buffer` (olla.h) - appended to
   right where the old direct `std::cout`/status-print calls used to be,
   guarded by one shared `output_buffer_mutex` (`inline std::mutex`, same
   `inline`-not-`static` reasoning as `history_mutex` - see the comment next
   to it in olla.h).
2. `ollama_system` never needs to know an `OUTPUT_CLASS` exists. It just
   fills its own buffers, blissfully unaware anything is reading them.
3. `OUTPUT_CLASS::get_response(ollama_system& chat)` (user_io.cpp) is the
   *consumer* side: it reaches in, locks `output_buffer_mutex`, copies each
   buffer's contents out and clears the source, every tick. It's a pull, not
   a push - the caller (`main.cpp`) decides *when* and *which* instances to
   pull from (`chat` itself, each of `chat.background_tasks` via
   `ollama_system::pull_background_output()`, sidetrack's
   `SIDETRACK_CHAT_INSTANCE` via `SIDETRACK_CLASS::pull_output()`).
4. `OUTPUT_CLASS::display()`/`display_with_ncurses()` then just prints
   whatever accumulated and clears it - same check-act-clear shape
   `ollama_system::write_to_tts()` already used for `tts_buffer`, which is
   really the same pattern one step earlier (also cross-thread, just never
   generalized past TTS before now).

The reason this beat a pointer/global: nothing upstream (`ollama_system`,
its tool handlers, its background instances) needs to change when a new
consumer wants to look at its output, and a consumer can choose per-tick
whether a given instance's output should even be visible (e.g. main.cpp
currently pulls sidetrack and background tasks but nothing pulls the
delegator's `sub_agent`, which can't be reached this way at all - it's a
local variable, synchronous, gone before any tick could reach it).

### What we built today (2026-08-26): IO_WORKER_CLASS + COMMS

The problem this solved: keyboard reading, voice-event polling, and screen
drawing all lived inline in `main.cpp`'s own loop, interleaved with
chat/model logic (`chat.process()`, `sidetrack.check()`) on the same
thread. `KEYBOARD_INPUT`/`OUTPUT_CLASS` (`user_io.h`/`.cpp`) turned out to
have zero built-in thread-safety of their own once we looked closely - raw
termios manipulation on `STDIN_FILENO`, unlocked plain fields, and ncurses
itself is single-thread-only - so moving them off the main thread meant
picking one thread as their sole, permanent owner rather than sharing
access.

1. **`COMMS` (`comms.h`/`.cpp`)** - bundled `ollama_system`'s four output
   buffers (see the superseded section above) plus a new `audio` pointer
   (replacing a process-wide `g_audio_control` global - each instance's
   `COMMS` points at whichever `AUDIO_CONTROL_CLASS` it should speak
   through, so a future different `COMMS`, e.g. a remote session, doesn't
   have to share the one local speaker) and three input-direction signals:
   `send`/`submitted_line` (a line ready to submit), `stop_requested`
   (abort in-flight generation/speech), `exit_requested` (Ctrl+C).
2. **`IO_WORKER_CLASS` (`io_worker.h`/`.cpp`)** - owns `KEYBOARD_INPUT`/
   `OUTPUT_CLASS` directly as members (moved off `CLASS_SYSTEM`, which no
   longer has them), runs its own background thread. Grew out of a
   generic `WORKER_THREAD_CLASS` skeleton (`templates/worker_thread.h`/
   `.cpp` - copy-and-rename template, not meant to be included directly)
   built earlier the same day as a reusable background-thread-plus-
   main-thread-check-in shape modeled on `SIDETRACK_CLASS`'s own two-
   thread design.
   - `thread_main()` (its own thread): reads the keyboard, polls/merges
     voice events, interrupts `sidetrack`/`audio` directly (it already
     holds references to both), drains `chat`/background-tasks/
     sidetrack's comms into the screen, draws. Stages anything the main
     thread needs to know (a submission, an interrupt, an exit request)
     into a locally-held `COMMS` first.
   - `exchange(COMMS& comms)` (called once per `main.cpp`'s own tick,
     passed `chat.comms`): the *only* thing that crosses the thread
     boundary is `comms` - relays whatever got staged, under a small
     two-flag lock (`INTERUPTED`/`PROCESSING`, adapted from
     `WORKER_THREAD_CLASS`) so a submission never gets read half-written.
   - `main.cpp`'s loop shrank accordingly - `chat.input()`/`jump_input()`
     (olla.cpp) now read `comms.send`/`stop_requested` instead of
     `key_input` directly, since they can't reach `IO_WORKER_CLASS`'s
     members at all anymore.
3. Two correctness bugs found and fixed along the way, worth remembering
   if this area gets touched again: (a) `Keyboard_Input_Enabled`
   (toggled by `TOOL_TASK_RUNNER` automations, threaded through
   `process()`/`handle_instance_tools()`/`dispatch_tool_call()`) was a
   plain `bool&` read/written on two different threads once keyboard
   reading moved to the worker - now `std::atomic<bool>&`. (b)
   `ollama_system::input()`'s submission branch has to clear
   `comms.stop_requested` too, not just `comms.send` - a real Enter
   keypress sets both `INTERRUPTED` and `ENTER_PRESSED` together
   (`KEYBOARD_INPUT::keyboard_input()`, user_io.cpp), and if only `send`
   gets cleared, a stale `stop_requested` from that same keypress can
   fire on a *later* tick once `is_processing` becomes true - aborting
   the response that keypress itself just started.

### What we built today (2026-08-28): AUDIO_CONTROL_CLASS folded into IO_WORKER_CLASS, COMMS renamed, sidetrack disconnected

The trigger: `AUDIO_CONTROL_CLASS`'s own coupling, called out but deliberately
left alone in the 2026-08-26 entry above ("the worker just became the sole
poller of `popVocaEvent()`... not a relocation of `voca`/`tts` themselves").
This time it actually moved.

1. **`AUDIO_CONTROL_CLASS` is gone.** `TextToSpeech`/`Voca` (formerly
   `tts.hpp`/`.cpp`, `voca.hpp`/`.cpp`) are declared and defined directly in
   `io_worker.h`/`.cpp` now, as `IO_WORKER_CLASS`'s own private `tts`/`voca`
   members - nothing outside that one file touches either. Both are
   constructed right before `thread_main()` sets `RUN = true` and torn down
   right after its `while(RUN)` loop exits, not for the object's whole
   lifetime - Voca's whisper-model load/mic-thread startup and TextToSpeech's
   own worker-thread startup all happen on the worker's background thread as
   a result, not blocking whoever calls `thread_start()`. The old
   `AUDIO_CONTROL_CLASS` background thread (polling `tts.isSpeaking()` to
   pause/resume Voca, `adjust_audio_files()`) is gone too - that check now
   just rides `thread_main()`'s existing ~20ms tick instead of its own
   500ms one.
2. **`COMMS` renamed** (`comms.h`): `response_buffer`/`thinking_buffer`/
   `log_buffer` → `INPUT_FROM_LLM`/`INPUT_FROM_THINKING`/`INPUT_FROM_SYSTEM`;
   `send`/`submitted_line`/`stop_requested`/`exit_requested` →
   `ENTER_PRESSED`/`INPUT_FROM_USER`/`INTERRUPTED`/`EXIT_REQUESTED`, matching
   `KEYBOARD_INPUT`'s own field names one-for-one since these are relayed
   straight from it; gained `IS_TYPING` (not yet consumed anywhere). `audio`
   and `tts_buffer` are gone from `COMMS` entirely - no more per-instance
   pointer to a speaker.
3. **`IO_WORKER_CLASS` gained two private `COMMS` copies** - `comms_buffer`
   (renamed from `staged`) and a new `comms_buffer_audio`. `exchange()` fans
   one drain of the real `comms` out into both (`+=`, then a single clear on
   the source) so the screen (`comms_buffer`, drained by `get_response()` on
   the worker's own thread) and TTS (`comms_buffer_audio`) never race each
   other for the same `INPUT_FROM_LLM`/`THINKING`/`SYSTEM` text.
4. **TTS chunking moved out of `ollama_system::write_to_tts()`** (deleted,
   along with the `comms.tts_buffer` it drained and its punctuation/length/
   generation-finished heuristic) **into `IO_WORKER_CLASS::thread_main()`**,
   gated on `tts->isSpeaking()` instead: once idle, flush whatever's
   accumulated in `comms_buffer_audio.INPUT_FROM_LLM` to `speakAsync()` and
   clear it. That field doubles as its own accumulator (nothing else reads
   it), so no separate buffer was needed - a `tts_buffer`/`stt_buffer` pair
   was tried first and undone once this became clear.
5. **`display_with_ncurses()` takes `(input_from_user_echo, comms,
   tool_names)`** now, not `(key_input, tool_names)` - `input_from_user_echo`
   is a new `IO_WORKER_CLASS` member, a live per-tick mirror of
   `key_input.LINE` for showing what's currently being typed; `comms` (passed
   `comms_buffer`) is threaded through but not yet read inside the function.
6. **Voice transcripts write straight into `comms_buffer.INPUT_FROM_USER`/
   `ENTER_PRESSED`** now, instead of staging through `key_input.LINE`/
   `ENTER_PRESSED` first the way typed input still does - STT lives inside
   this worker's own `thread_main()`, so there's no reason to detour through
   `key_input` for it.
7. **Sidetrack disconnected from `IO_WORKER_CLASS` entirely** -
   `thread_start()`/`thread_main()` no longer take a `SIDETRACK_CLASS&` at
   all - and every sidetrack call in `main.cpp` is commented out, ahead of
   the rewrite below. `SIDETRACK_CLASS sidetrack;` itself is still declared
   there (harmless - non-trivial constructor, no unused-variable warning).
8. **Known gaps accepted for now, not fixed**:
   - Background tasks (`spawn_background_task()`) and sidetrack have no TTS
     path at all - `COMMS::audio` is gone, and `comms_buffer_audio` only
     exists inside `IO_WORKER_CLASS`, fed only by the main chat's own
     `exchange()` call.
   - Sidetrack's own generated text no longer reaches the screen (its
     `pull_output()`'s only caller, in `thread_main()`, was removed).
     **Done 2026-09-04 (found already resolved during a later audit)**:
     resolved as a side effect of `run_second_guess()` (sidetrack.cpp,
     2026-08-30) deliberately reusing the main chat's own real `comms`
     instead of an isolated one - its thinking/response text now reaches
     the screen (and TTS) for free via the existing `exchange()`/display
     path, same as the main chat's own output, with no dedicated wiring
     needed. See `run_second_guess()`'s own entry below for the reasoning.
   - `IO_WORKER_CLASS::exchange()` still takes an `ollama_system& chat_ref`
     parameter as a placeholder, purely because `TOOL_BASE::register_tool
     (ollama_system&, json&)` (every `TOOL_*` class, `tools.h`/
     `remote_tools.h`) hard-requires one for building the tools-panel's
     `tool_names` - untangling that is part of the move below.
     **Done 2026-09-04 (found already resolved during a later audit)**:
     `IO_WORKER_CLASS::exchange()` (io_worker.cpp) now takes only
     `(COMMS&, std::vector<std::unique_ptr<TOOL_BASE>>&)` - the
     `ollama_system& chat_ref` placeholder is gone, exactly as the
     COMMS-ownership-move entry below (Done 2026-08-29) describes.
   - Everything above got to a clean `-Wall -Wextra -Werror` build (verified
     2026-08-28) - but only static analysis, not real usage, beyond a quick
     smoke-test session that surfaced and fixed an unrelated pre-existing
     issue (two orphaned `presence` remote-tool client processes, killed;
     see `olli_presence_flapping_fix.md`-style notes if that recurs).

### Sidetrack rewrite + COMMS ownership move

Two-part plan, in order:

1. **Done 2026-08-29: moved `COMMS comms` off `ollama_system` entirely,
   onto `main_process()` (`main.cpp`) instead.** Every `ollama_system`
   method that used to touch `this->comms` internally (`send()`,
   `process()`, `input()`, `jump_input()`, `integrate_tool_result()`,
   `handle_instance_tools()`, `dispatch_tool_call()`) now takes a `COMMS&`
   parameter instead, threaded all the way down through `TOOL_BASE::
   check()`/`monitor_tool()` and every concrete override (`tools.cpp`,
   `remote_tools.cpp`) - same pattern `tools_list` already used. `send()`
   dropped its old `user_input` string parameter entirely; it now reads
   `comms.INPUT_FROM_USER` instead, so every caller sets that field first
   (`comms.INPUT_FROM_USER = prompt; send(tools_list, comms, role);`).
   `background_tasks` changed from `vector<unique_ptr<ollama_system>>` to
   `vector<pair<unique_ptr<ollama_system>, unique_ptr<COMMS>>>` so each
   background task-runner automation instance gets its own real,
   persistent `COMMS` too (not a throwaway) - `unique_ptr<COMMS>`
   specifically, not a bare `COMMS`, both because `COMMS` holds a
   `std::atomic` member (no copy/move constructor, can't live by value in
   a vector) and because `spawn_background_task()` hands out a `COMMS&`
   that a reallocating vector would otherwise dangle. Found and fixed one
   new real race along the way: `ollama_system::input()`'s `chat_thread`
   has to write the submitted text into the shared `comms.INPUT_FROM_USER`
   right before calling `send()` (since `send()` no longer takes it as a
   parameter) - now locked under `output_buffer_mutex` on both sides
   (`olla.cpp` and `IO_WORKER_CLASS::exchange()`'s own relay, io_worker.cpp),
   since a real second writer (`exchange()`, main thread) touches the same
   field. `IO_WORKER_CLASS::exchange()` also dropped its `ollama_system&
   chat_ref` placeholder entirely - `register_tool()`'s `ollama_system&`
   parameter turned out to be unused by every real implementation, so the
   tools-panel's name list now reads each tool's own `TOOL_BASE::
   tool_functions` (populated by `register_tool()` itself) instead of
   re-deriving names by calling `register_tool()` a second time just for
   display.
   - **One deliberately deferred gap: `ollama_system::log()` is currently
     a no-op stub** (`olla.cpp`) - `comms.log()` no longer exists (COMMS::
     log() was removed, replaced everywhere else by a direct
     `comms.INPUT_FROM_SYSTEM +=` append), and threading `COMMS&` through
     every one of `log()`'s many call sites (`tools.cpp`, `remote_tools.cpp`,
     `main.cpp`, `olla.cpp` itself) was set aside rather than done as part
     of this move. Revisit next time system messages are being worked on -
     same pattern as `integrate_tool_result()`: `void log(COMMS& comms,
     const std::string& text) { std::lock_guard<std::mutex> lock
     (output_buffer_mutex); comms.INPUT_FROM_SYSTEM += text; }`, locked
     since `log()` could in principle be called from `chat_thread` (inside
     `send()`) as well as the main thread.
2. **Done 2026-09-04 (found already resolved during a later audit, not
   fixed in response to this entry): sidetrack rewritten from scratch**
   against the real COMMS-ownership shape, rather than retrofitting the
   pre-rework design described in [the background sidetrack thread section
   of README.md](README.md). `sidetrack.h`/`.cpp` are no longer wrapped in
   `#if 0` - `SIDETRACK_CLASS` is a real, working class (no background
   thread; `check()` is called once per tick from `main.cpp`'s main loop,
   non-blocking except for the deliberately-deferred synchronous
   consolidation call noted below), with `run_consolidation()`/
   `run_second_guess()`/`run_clear_context()`/`persistent_time_checks()`
   all implemented - see the two Done entries just below (2026-08-30) for
   the detail on the two biggest pieces. Its screen output and TTS path
   both got re-wired as part of that same work: `run_second_guess()`
   deliberately reuses the main chat's own real `comms` rather than an
   isolated one (see its own entry below), so sidetrack's thinking/response
   text now reaches both the screen and TTS for free via
   `IO_WORKER_CLASS::exchange()`'s normal drain of that shared `comms` -
   no separate `IO_WORKER_CLASS::speak()` wiring needed after all.
3. **Done 2026-08-30: `run_consolidation()` (sidetrack.cpp) implemented and
   working**, tested against a real Ollama server via a standalone harness
   (`../olli_consolidation_test/`, outside the real build - see its own
   `build.sh`/`test_consolidation.cpp`; deliberately avoids linking
   whisper/ncurses/portaudio, only needs curl+pthread, since only 4 unused
   `user_io.cpp` symbols are ever referenced by this code path -
   `stub_ui.cpp` no-ops them). Bucket-by-`consolidation_level`, then per
   level: once it holds more than `keep_count`
   (`PROPS.consolitation_starts_starts_at`, now 30) messages and the
   overflow is at least `trigger_count` (`PROPS.consolitation_sizes`, now
   12), the entire overflow gets replayed onto a dedicated throwaway
   `ollama_system` (`SIDETRACK_CHAT_INSTANCE`, model/host/port only - no
   thinking/streaming/disk-saving/tools) and squashed into one summary
   message via a single `system`-role trigger prompt, promoted to the next
   level up - cascades through multiple levels in one `run_consolidation()`
   call if a promotion pushes the next level over its own threshold too.
   `ollama_system::replace_history()` added (same in-memory-only,
   caller-calls-save_history()-separately pattern as `clear_history()`/
   `clear_history_keep_protected()`) for installing the rebuilt vector back
   into `main_instance.history` at the end.
   - **Real bug found and fixed while testing**: the overflow slice can't
     just be cut at a raw count - level 0's raw messages aren't a fixed
     2-message-per-turn shape once a tool call is involved (`user`,
     `assistant`, `tool`, DIRECTOR_NOTE `system`, `assistant` - 5 messages,
     not 2), so a slice ending mid-exchange leaves a dangling unanswered
     `user`/`tool` message right next to the trigger prompt - confirmed
     this made the model narrate the dangling question instead of
     summarizing anything, or (worse, more silently) confidently "answer"
     just that trailing question while dropping every other topic in the
     batch. An even-count rounding rule was tried first and wasn't enough
     (parity shifts after any odd-length tool exchange) - fixed for real by
     shrinking the slice until it ends on a completed `assistant` turn,
     content-based rather than count-based (level 0 only; levels above 0
     are always self-contained `system`-role summaries with no such
     pairing to worry about).
   - **`tool`/DIRECTOR_NOTE messages get flattened, not filtered, when
     replayed for summarization** - deliberately not dropped from what
     gets consolidated (that would repeat the exact "erasing tool-call
     evidence" bug from 16ab453, just via consolidation exclusion instead
     of early deletion), but a bare `tool`-role message has no meaning to
     the API without the preceding `assistant` tool_calls entry that isn't
     being replayed, so it's relabeled to `user` with a `"[Tool result]:
     "` text marker instead. DIRECTOR_NOTE (`system`-role, matched by its
     literal `"[DIRECTOR_NOTE]"` prefix) is dropped entirely rather than
     also flattened - it's redundant with the `tool` message right before
     it (`integrate_tool_result()` embeds the exact same raw result
     verbatim). Any other `system`-role message at level 0+ is our own
     past promoted summary, not a DIRECTOR_NOTE - replayed unchanged.
   - **Known quality gap, not yet fixed**: when a squashed batch spans
     several unrelated topics, the resulting summary tends to only cover
     the *last* topic in the batch, silently dropping the others, rather
     than covering the whole thing - seen with both a 14-message and a
     19-message multi-topic batch in testing. Graceful, not corrupting
     (only ever affects already-aged-out material, the newest `keep_count`
     messages always stay at full fidelity regardless) but does undermine
     consolidation's actual point if it's this lossy - likely needs a
     stronger trigger prompt (e.g. explicitly asking it to cover every
     topic present, not just summarize) rather than a code change. Revisit
     before relying on long-lived consolidated history for anything that
     matters.
   - **Done 2026-08-30: wired into `check()`** - `run_consolidation()` is
     now actually called every tick, no longer just reachable via the
     test-only `force_consolidation()` hook (sidetrack.h). It still runs
     synchronously, blocking, directly on the main thread whenever it
     triggers - the class comment in sidetrack.h says `check()` is meant to
     stay non-blocking, which this doesn't honor - but deliberately left
     that way for now: idle-gated, fast in practice (~8-11s for a large
     stress-test batch), so not worth the added complexity of threading it
     at this point. Revisit if it ever proves disruptive in real use.
4. **Done 2026-08-30: `run_second_guess()` (sidetrack.cpp) implemented and
   wired into `check()`.** After a real assistant reply lands, waits
   `SECOND_GUESS_WAIT_TIME` (2s), then asks a thinking-mode
   `SIDETRACK_CHAT_INSTANCE` (real `tools_list`/`CLASS_SYSTEM*` passed in,
   so it can actually dispatch a tool, not just talk about one) "More
   needed to be done or said? Respond DONE if not." If not `DONE`, asks a
   follow-up "Go ahead - say or do what needs to happen." and commits
   whatever comes back as a new `assistant` message onto `main_instance.
   history` - marked with `"..."` if interrupted mid-answer rather than
   discarded. Uses `SIDETRACK_CHAT_INSTANCE`'s own `chat_thread`/
   `is_processing` (same async mechanism `ollama_system::input()` already
   uses for the main chat) so `run_second_guess()` can poll tick-by-tick
   instead of blocking; each "waiting" stage calls `handle_instance_tools()`
   every tick (same shape `process()`'s own background-task handling uses)
   so a tool call gets genuinely dispatched and narrated, not just
   requested.
   - **Deliberately reuses the main chat's own `comms`, not an isolated
     one** - initially built with a separate `second_guess_comms` (plus
     matching plumbing through `check()`, `main.cpp`, `io_worker.cpp`) to
     dodge a narrow collision risk on `comms.INPUT_FROM_USER`/`INTERRUPTED`
     between a real user submission and second-guess's own internal
     prompts, but reverted at the user's explicit direction: "comms is the
     backbone of the io that gets to and from the user... the only reason
     to circumvent it is if the user shouldn't be seeing it" - see
     [[olli-collaboration-style]]. Accepted the collision risk; bonus from
     reverting: thinking/response text reaches the screen for free via the
     existing `exchange()`/display path, no extra wiring needed.
   - **`comms.INTERRUPTED` is read-only everywhere in this function, never
     cleared** - it's the real, shared main-chat comms now, and the actual
     owner of clearing it is `ollama_system::input()` (gated on
     `is_processing`, olla.cpp) - clearing it a second time here could race
     with that and swallow a real interrupt to the main chat.
   - **Two independent streaming gates added to make DONE-suppression
     possible**: `OLLAMA_SYSTEM_PROPERTIES::stream_thinking` (new, alongside
     `stream_output`, both default `true`) - `send()` (olla.cpp) now uses
     Ollama's streaming API whenever *either* flag is on, and gates
     `comms.INPUT_FROM_LLM`/`INPUT_FROM_THINKING` independently inside the
     streaming callback, so a call can show its thinking live while
     suppressing its plain-text content (or vice versa) - needed because
     the DONE-check call's literal `"DONE"` answer was otherwise leaking
     onto the screen the same as any real content would. Learned along the
     way: this can only be prevented by never writing it to `comms` in the
     first place (`stream_output = false` for that one call) - clipping it
     out afterward doesn't work, since streaming writes happen live, chunk
     by chunk, during the call itself, well before any post-completion
     check could intervene.
   - **Known latent bug found, not yet confirmed as the cause of anything
     real**: `comms.INTERRUPTED` can get stuck `true` forever in a narrow
     case - `IO_WORKER_CLASS::thread_main()`'s voice-event handling
     (io_worker.cpp) sets `key_input.INTERRUPTED = true` unconditionally
     whenever a non-status voice event is popped, even if
     `voca_event.text` is empty (no transcript, e.g. some wake/listening
     event) - `ENTER_PRESSED` only gets set alongside it when there IS
     text. `comms.INTERRUPTED` only ever gets cleared by
     `ollama_system::input()`, gated on either `is_processing` or
     `ENTER_PRESSED` being true - if neither happens to be true at that
     exact moment, nothing ever clears it again. Always a latent
     possibility, harmless before since nothing else ever read the flag -
     now that `run_second_guess()`'s abort-before-starting check does,
     stuck-true here would permanently block it from ever leaving stage
     0/1. Suspected during one test session ("it's not going into second
     guess at all") but the user found it working again on a recheck, so
     unconfirmed as the actual cause - worth a real fix (e.g. only ever
     set `key_input.INTERRUPTED` alongside a real `ENTER_PRESSED`,
     matching how a real keypress already pairs them) if it recurs.
   - **Confirmed and fixed 2026-09-03.** This was the actual cause of a
     real, reproduced bug: interrupting mid-thinking-block during
     second-guess left `comms.INTERRUPTED` stuck `true` forever (exactly
     the narrow case above - a bare interrupt, no `ENTER_PRESSED`, while
     `chat.is_processing` was false since second-guess only ever runs once
     the main turn has already finished). Fixed in `sidetrack.cpp` at both
     read sites (`poll_second_guess_call()` and the abort-before-starting
     check in `run_second_guess()`) - each now clears `comms.INTERRUPTED`
     itself right after consuming it, safe because `main.cpp`'s loop always
     runs `chat.input()` before `sidetrack.check()` each tick, so main
     chat already had first claim on the flag that same tick if it needed
     it. A second, related bug found in the same investigation:
     `poll_second_guess_call()`'s own "done" check required
     `last_received.complete`, which is permanently `false` after an
     interrupt - stalled stage 3/5 forever on any interrupt mid-call
     (stage 4/6's own "handle an interrupted result gracefully" branches
     existed but were unreachable as a result). Fixed by dropping
     `complete` from that check entirely - `is_processing` false and
     `tool_calls` empty is enough to mean "this call is over," clean or
     not.
   - **Done 2026-09-16: the review instance never actually saw what it was
     reviewing - found and fixed.** `run_second_guess()`'s `stage == 2`
     block calls `SIDETRACK_CHAT_INSTANCE.clear_history()` and pushes only
     one system line ("You are reviewing your own last response to the
     user...") before asking "More needed to be done or said?" - no copy
     of the actual user question or olli's actual last response ever made
     it into that instance's own context, anywhere. Confirmed by grepping
     the whole file for any such copy - none exists in `run_second_guess()`
     (`run_consolidation()`, same file, does this correctly for its own
     purpose - `working_history = main_instance.history;` - second_guess
     just never did the equivalent). The review model was judging
     completeness of a response it had never been shown, which explains a
     full afternoon of live-found evidence from an exploratory test
     session the same day: a `check_timer` call for a label ("example")
     never mentioned anywhere, `set_hue_light` dimming all lights to 0%
     completely unprompted, an unprompted `check_presence`, an unprompted
     `set_thinking_mode` - all real remote/built-in tool calls (which is
     working as designed, see the 2026-08-30 entry above - the *access*
     was never the bug), fired with no basis in the actual conversation
     because there *was* no actual conversation in view.
     - **Fix**: after the `task_note` push, lock `history_mutex`, walk
       back from the end of `main_instance.history` to (and including) the
       last `"user"` message, and copy that whole exchange - the real
       question, any tool activity in between, the actual reply - into
       `SIDETRACK_CHAT_INSTANCE.history` before asking "More needed to be
       done or said?" (same idea as `run_consolidation()`'s own copy,
       just scoped to one turn instead of the whole conversation).
       ~15 lines, contained entirely to the `stage == 2` block.
     - **Verified live afterward**: 6 separate review cycles across a
       fresh real session, zero fabricated actions - every one either
       resolved `DONE` correctly or was cleanly interrupted by real
       activity. Caveat: none of those 6 cycles actually took the
       "not done -> do something" branch with the fix in place, so the fix
       to the DONE/not-done *judgment* is confirmed; the fix to the
       *action itself staying grounded once triggered* isn't independently
       confirmed yet - worth watching for over time, not fully closed out.
     - **Side effect found the same day, worth watching, not yet acted
       on**: now that the review genuinely has real content in front of
       it, the DONE-check call itself sometimes fully elaborates a
       hallucinated, out-of-persona continuation instead of answering
       cleanly with "DONE" - one real instance: fabricated additional "car
       maintenance tips... from reliable sources" not present in the real
       notes, continuing a real list's numbering as if extending it.
       Stayed private this time (that call has `stream_output = false` by
       design - confirmed via the `sidetrack-second-guess` debug tag
       rather than `chat`, meaning it never reached the screen), but it's
       the same fuzzy `starts_with(answer, "DONE")` check already known to
       be imprecise (see the markdown-wrapper-stripping fix above), just
       demonstrated in a more extreme form - full hallucinated content
       instead of just a decorated "DONE". Not yet investigated further.

### Task-runner rewrite (2026-09-02): state machine, live streaming, real tool access, leak fix

`TOOL_TASK_RUNNER::handle_tool()` (tools.cpp) rewritten from three separate
nested blocking `while` loops (one each for a plain command, `[[ENTER TO
CONTINUE]]`, and `[[ASK]]`, each spinning its own `io_worker.exchange()`
calls) into one flat `while` loop over an explicit `SCRIPT_STATE` enum
(`GET_COMMAND`/`EXECUTE_COMMAND`/`WAIT_RESPONSE`/`WAIT_ENTER`/`WAIT_ASK`/
`DONE`) - `instance.process()` and `io_worker.exchange()` each run exactly
once per tick regardless of state, mirroring the shape of `main.cpp`'s own
loop and `process()`'s existing PART 2 background-task poll. Several real,
previously-latent bugs found and fixed along the way:

- **`IO_WORKER_CLASS&` threaded through the tool-dispatch chain.** Added to
  `TOOL_BASE::check()` (and all four overrides: `TOOL_SET_THINKING_MODE`,
  `TOOL_WEB_SEARCH`, `TOOL_TASK_RUNNER`, `TOOL_REMOTE`), `dispatch_tool_call()`,
  `handle_instance_tools()`, `process()` (olla.h/.cpp, tools.h/.cpp,
  remote_tools.h/.cpp), and `SIDETRACK_CLASS::check()`/`run_second_guess()`/
  the file-local `poll_second_guess_call()` (sidetrack.h/.cpp), since
  sidetrack also calls `handle_instance_tools()` directly. `main.cpp` passes
  the real `io_worker` at both call sites (`chat.process(...)`,
  `sidetrack.check(...)`).
- **`Keyboard_Input_Enabled`/`disable_keyboard` special-case removed from
  `dispatch_tool_call()` entirely** (was: a hardcoded `tc.name ==
  "run_automation_task"` string check toggling a `std::atomic<bool>&`
  threaded through `process()`/`handle_instance_tools()`/
  `dispatch_tool_call()` purely for this one tool - flagged as a stopgap by
  its own TODO comment). No longer needed now that `TOOL_TASK_RUNNER` holds
  `io_worker` directly. Removing it also let `SIDETRACK_CLASS`'s own
  `second_guess_keyboard_enabled` - a deliberate fake placeholder so
  sidetrack's review instance could never touch the real keyboard state via
  the old mechanism - be deleted too, since sidetrack now gets the real
  `io_worker` reference instead (harmless today since nothing yet toggles
  keyboard-enabled state from `handle_tool()` - see the deferred item
  below).
- **Live streaming actually works now - two separate fixes needed.**
  (1) `EXECUTE_COMMAND` used to call `instance.send()` directly/
  synchronously - a blocking HTTP call, so the loop's own
  `io_worker.exchange()` couldn't run *during* a request, only after it
  fully returned. Now spawns `instance.chat_thread` (same pattern
  `ollama_system::input()` and `sidetrack.cpp`'s own
  `start_second_guess_call()` already use), so `WAIT_RESPONSE` can poll
  `!instance.is_processing` while `io_worker.exchange()` keeps flushing
  each tick. (2) Separately, `instance.PROPS.stream_output` was hardcoded
  `false` (present since before this rewrite too) - since that's the
  specific gate on whether streamed chunks reach `comms.INPUT_FROM_LLM` at
  all (`send()`'s streaming callback, olla.cpp), the model's actual answers
  never reached the screen regardless of fix (1) - only a trailing `"\n"`
  did. `stream_thinking`/`use_thinking` staying default-`true` meant
  reasoning text streamed live the whole time, masking this - easy to
  mistake for "streaming already works." Flipped `stream_output` to `true`.
  Confirmed live post-fix (weather-in-New-York answer visibly appeared on
  screen, previously invisible).
- **Automation instance now shares the caller's real `tools_list`, not a
  separately-built one.** Previously built its own via
  `populate_default_tools()` (thinking-mode/web-search/task-runner only) -
  since `TOOL_REMOTE` instances only ever get added to `main.cpp`'s own
  `tools_list` dynamically as devices register, and are `unique_ptr`-owned
  (can't exist in two vectors at once), the automation instance could never
  see or control any real connected device, and `TOOL_REMOTE::monitor_tool()`'s
  keep-alive ping never ran for it either. Confirmed live before the fix:
  "turn off all the lights" got a generic "I can't control smart devices
  directly" answer instead of an actual tool call. `instance.open()`/
  `send()`/`process()` all use the real `tools_list` now - also resolves a
  lifetime concern the old comment was reasoning about (the local
  `automation_tools_list` outliving its own scope via `instance` sitting in
  `chat.background_tasks` after `handle_tool()` returns; the real
  `tools_list` is already the same long-lived reference PART 2's cleanup
  pass uses).
- **`instance_comms` (from `spawn_background_task()`) now actually used** -
  was created but every `send()`/`process()` call used the *main* chat's
  own `comms` instead, so the automation's own I/O and the main chat's own
  were the same buffers. Now isolated: everything inside the loop uses
  `instance_comms`; only the pre-loop status line and the post-loop
  `chat.send_tool_result()`/`integrate_tool_result()` calls (intentionally
  reporting back to the real conversation) still use the real `comms`.
- **`chat.background_tasks` leak fixed.** `WAIT_RESPONSE` resets
  `instance.last_received.complete = false` after every command (including
  the last one, to distinguish a finished command from a still-in-flight
  one) - and nothing ever set it back to `true` afterward, so
  `ollama_system::process()`'s PART 2 (`is_finished = !is_processing &&
  last_received.complete && ...`) could never recognize a finished
  automation instance as done, and its `unique_ptr<ollama_system>`+`COMMS`
  just accumulated in `background_tasks` forever - present in the original
  pre-rewrite code too (same unconditional reset after every command),
  despite an inline comment claiming "the very next completion check below
  erases it." Fixed by explicitly setting `complete = true` once the whole
  script is done, right before returning - also clears
  `last_received.response` at the same time, otherwise PART 2's own
  separate "if the task produced a response, relay it" check would fire a
  second, redundant narration of the same completion
  `integrate_tool_result()` already reports.
- Live-tested end to end against `run system test` (streaming, `[[ASK]]`,
  `[[ENTER TO CONTINUE]]`, real tool dispatch, all confirmed via
  `debug_full_history.txt` timestamps). `run process resume` not yet
  exercised with the new loop.

**Deferred, not fixed:**
- No keyboard-enable/interrupt handling during an automation -
  `io_worker.key_input.PROPS.ENABLED` management was deliberately left out
  for now (manual workaround: don't type until a script step is actually
  asking). Two known consequences: a keystroke typed before `WAIT_ENTER`/
  `WAIT_ASK` sits stale in `instance_comms` and gets misread as the answer
  to whatever prompt is reached next; and `comms.INTERRUPTED` is never
  checked anywhere in the state machine, so there's currently no way to
  abort a running automation once started.
- Whether sidetrack's second-guess review should be suppressed after an
  automation-driven reply - confirmed it currently reviews them like any
  other assistant turn (`run_second_guess()`'s trigger is role-based, not
  source-based, so it can't tell), decided to defer the actual design call
  rather than fix it blind.

### TOOL_DELEGATOR revived (2026-09-03): consult_expert, recursion depth cap, two open issues found in testing

Was a commented-out design sketch (`tools.h`/`.cpp`, see git history for the
pre-revival version) - predates the COMMS refactor entirely, so its old
`register_tool(json&)`/`handle_tool(ollama_system&, ...)` signatures didn't
match anything callable, and it was pulled out of the `TOOL_BASE` hierarchy
as a result. Brought back in line with `TOOL_TASK_RUNNER`'s current shape:
spawns a sub-agent via `chat.spawn_background_task()`, seeds its persona via
`OLLAMA_OPENING` + `open(tools_list, chat.PROPS)` instead of hand-pushing a
system `Message`, drives it with the same `chat_thread`+polling-loop pattern
as everything else, and relays its answer via the modern 4-arg
`integrate_tool_result()`. Registered in `populate_default_tools()`
(`olla.cpp`) - live as `consult_expert` for the model to call like any other
tool. The sub-agent shares the caller's own `tools_list`, deliberately (not
the original's empty one - development had stopped before that mattered) -
it can actually act under its persona's judgment, not just talk about it.

- **`stream_output` ordering bug, fixed.** `open(tools_list, Properties)`
  does `PROPS = Properties` first thing (`olla.cpp`), so anything set on
  `instance.PROPS` *before* that call gets silently overwritten by
  `chat.PROPS`'s own value. Set `stream_output = false` *after* `open()`
  instead - deliberately off, since `integrate_tool_result()` always
  narrates the raw result back to the user afterward anyway, so streaming
  the sub-agent's own generation live just showed the same content twice
  (confirmed live: a poem streamed once raw, then pasted again verbatim
  inside the main persona's own reply - `[DIRECTOR_NOTE]`'s "report...
  without changing the facts/values it contains" wording taken literally
  for creative content). `TOOL_TASK_RUNNER::handle_tool()` sets
  `stream_output` before its own `open()` call too - likely the same silent
  no-op there, not fixed here, out of this task's scope.
- **Recursion depth cap added** (`delegation_depth` member, max 3,
  `tools.h`/`.cpp`). The sub-agent's shared `tools_list` includes
  `TOOL_DELEGATOR` itself, so a specialist can call `consult_expert` on
  itself - a *different* persona chaining in for a sub-problem is
  legitimate (presumably the original intent), but nothing stopped a
  persona from just re-asking itself the identical question with no new
  information. Confirmed live: one "design a mood for movie night" request
  recursed 6 levels deep before finally doing any real work, each hop a
  full blocking network round-trip. Reentrant guard, safe as a plain `int`
  (not atomic) since the whole chain runs synchronously nested on one
  thread - `handle_tool()`'s own wait loop blocks until its sub-agent
  finishes before anything else touches the object.
- **Open, not fixed - empty-response bug.** After a successful
  `set_hue_light` call deep in a recursive chain, `instance.last_received.
  response` came back empty even though real work had just completed,
  propagating "The expert subroutine failed to return a response." back up
  through every nesting level - the lights had actually changed, but the
  user was told they hadn't. Not yet isolated whether this needs deep
  recursion to trigger, or can happen on a single ordinary delegation that
  includes a tool call - the depth cap above stops the recursion but wasn't
  confirmed to fix this specific symptom (the very next test run avoided it
  via a different path: once blocked by the depth cap, the model just
  fabricated a plausible-sounding text answer instead of calling the tool,
  so the empty-response path never got exercised again either way).
- **Open, not fixed - remote-tool heartbeat starves during any tool-result
  narration, badly so during a deep delegation chain.**
  `TOOL_REMOTE::monitor_tool()` (`remote_tools.cpp`) is what pings a
  connected remote tool (e.g. `hue`) to keep it alive
  (`PING_INTERVAL_SECONDS=5`, `DEAD_TIMEOUT_SECONDS=15`, `remote_tools.h`) -
  called once per `process()` tick for every tool, which is fine during an
  ordinary wait loop. The actual gap: `integrate_tool_result()`'s own
  `this->send(tools_list, comms, "system")` (`olla.cpp`) is the one `send()`
  call site in the whole codebase that isn't wrapped in a background
  `std::thread` the way every other one is (`sidetrack.cpp`'s
  `start_second_guess_call()`, `TOOL_TASK_RUNNER`/`TOOL_DELEGATOR`'s own
  initial calls) - it blocks the calling thread for the entire network
  round-trip (observed regularly taking ~15s on its own, right at
  `DEAD_TIMEOUT_SECONDS`), during which nothing else runs, `monitor_tool()`
  included. Not delegator-specific - a single ordinary tool call is already
  marginal - but delegation's nesting stacks several of these blocking
  windows on top of each other, making it far more likely to actually trip.
  Confirmed live: the hue remote-tool connection went down mid-test during
  a long recursive chain.
  - Considered and set aside as too risky for now: threading
    `integrate_tool_result()`'s `send()` call itself, reusing the existing
    `chat_thread`/`is_processing` pair. Would likely "just work" for the
    main chat's own top-level flow (`process()` PART 3, `olla.cpp`, already
    tolerates `chat_thread` finishing on a later tick) but risks a real
    race in every nested wait loop that already polls that same pair for
    its *own* completion (`poll_second_guess_call()`, `TOOL_TASK_RUNNER`,
    `TOOL_DELEGATOR`) - a loop could sample `is_processing` in the brief
    gap between the tool-call-generating `send()` finishing and the
    narration `send()` being launched, and conclude "done" before the
    narration ever ran. Same shape as the empty-response bug above - not
    confirmed to be the same bug, but suspicious.
  - Preferred direction instead, deferred as a real cross-file change
    (touches `tools/PROTOCOL.md` and every remote tool's own heartbeat
    side, not just olli's): before a known-long blocking stretch, send
    connected remote tools an explicit "standing by, don't go away" signal
    instead of relying purely on the regular ping cadence - the remote side
    would need to extend its own dead-timeout expectation on receiving it
    rather than just resetting on real ping/pong traffic.

- **Separate remote-tool bug found and fixed the same session (2026-09-03):
  event/result race in `TOOL_REMOTE::check()` (`remote_tools.cpp`) -
  unrelated to the heartbeat-starvation issue directly above despite living
  in the same class.** `check()` and `monitor_tool()` both read off the
  same per-connection socket - `check()` blocks waiting specifically for
  the `"result"` matching its own call's `call_id`, `monitor_tool()`
  non-blocking-polls each tick for anything else (`"ping"`/`"event"`).
  Previously, if the remote tool pushed an unsolicited event (e.g.
  `tools/clock/clock.cpp` noticing a timer expired) at the exact moment
  `check()` was mid-wait, `check()`'s blocking read consumed that line
  first, saw it wasn't a matching `"result"`, and reported a garbled
  `"Error: unexpected response from remote tool."` - the event itself was
  silently lost rather than ever reaching `monitor_tool()`'s own handling.
  Confirmed live: a `check_timer` call failing this exact way right after a
  `[TIMER EXPIRED]` push landed moments earlier in `run system test`
  testing.
  - **Not a wrong-tool-selection issue** - `is_mine`'s check at the top of
    `check()` already confirms `tc.name` belongs to this specific
    connection before any of this runs, and a genuinely invalid request
    (e.g. a timer label that doesn't exist) comes back as a clean, real
    `"error"` result from the remote tool, handled by a separate branch -
    neither of those reach this code path at all.
  - **Fixed by turning `check()`'s single read into a small loop**, bounded
    by the same overall 5-second budget (shrinking each pass via
    `read_line_blocking()`'s own timeout parameter): a `"ping"` gets
    replied to and waiting continues; an `"event"` is handled inline (same
    narration/`tool_calls_this_turn`-reset/queued-action logic
    `monitor_tool()`'s own event branch already has, duplicated rather than
    extracted into a shared helper - deliberately kept to this one function,
    not touching `monitor_tool()` or the header) and waiting continues;
    anything else (malformed, a pong, a mismatched `call_id`) is logged via
    `DEBUG_LOG_CLASS` and waiting continues; only the real budget running
    out (or a broken write mid-loop) now ends in failure.
  - **Not empirically confirmed against a live collision** - correct by
    construction (any non-matching line is now handled rather than
    misread, before the loop can ever fall through to giving up), but the
    one test run after this landed didn't actually land an event during an
    in-flight call, so it didn't exercise the new code path either way.
    Considered adding a dedicated debug-log line inside the new
    `ping`/`event` branches specifically (so a future real collision would
    leave unambiguous proof it was caught, unlike today where only the
    fallback branch logs anything) - deferred, not done.

### Live crash caught and fixed: unsynchronized `tools_list`; build version display; task-runner file I/O (2026-09-05)

- **`tools_list_mutex` superseded 2026-09-15** - removed entirely as part of
  the `TOOL_WORKER_CLASS` rewrite (see the Tools rework section above). The
  race this guarded against structurally can't happen anymore: remote-tool
  state now lives in `tool_worker`'s own thread-local `tools_list`, reachable
  from outside only through its INTERUPTED/PROCESSING rendezvous, not a
  shared vector multiple threads could iterate/erase/push_back on at once.
  Root-cause analysis below is still accurate history, just describing a
  problem that no longer exists in the current design.
- **The crash.** A real, reproducible SIGSEGV, caught live with `gdb -p`
  attached to a running `ron` session (no core dump otherwise - apport
  skips unpackaged binaries). Backtrace landed in `ollama_system::send()`
  (`olla.cpp`), `tool->register_tool(*this, tools)`, with the loop's current
  `tool` showing `get() = 0x0` - a null `unique_ptr<TOOL_BASE>` mid-iteration
  over `tools_list`.
  - **Root cause: an unsynchronized data race on the one real `tools_list`**
    (`main_process()`'s local, `main.cpp`, passed by reference everywhere -
    `ollama_system::open/process/send`, `dispatch_tool_call`,
    `TOOL_DELEGATOR`/`TOOL_TASK_RUNNER::handle_tool`). Any spawned
    background instance (`consult_expert`, `run_automation_task`) drives its
    own `chat_thread` calling `send()`, which rebuilds the tool schema by
    iterating `tools_list` on that thread - concurrently with the *main*
    thread's own `process()` (PART 5, same file), which prunes any
    `TOOL_REMOTE` that just reported `!is_alive()` via
    `tools_list.erase(std::remove_if(...))`. A `unique_ptr` vector's erase
    moves elements (unique_ptr is move-only), so a concurrent iterator on
    another thread can land on a moved-from (now-null) element mid-shift -
    exactly the null dereference caught.
  - **Why it fires reliably, not just on a rare real disconnect:**
    `TOOL_REMOTE::check()` (the blocking call whenever the model actually
    calls hue/clock/presence) budgets up to 5 blocking seconds
    (`remote_tools.cpp`), during which `monitor_tool()` never runs for any
    *other* remote tool sharing `tools_list` - there's no dedicated
    heartbeat thread, ping/pong bookkeeping is entirely opportunistic,
    riding on whatever tick rate the currently-active call chain happens to
    produce. Chain a few tool calls (exactly what a multi-step task or a
    `consult_expert` detour does) and another tool's `last_received` can go
    stale past `DEAD_TIMEOUT_SECONDS` (15s, `remote_tools.h`) from pure
    starvation, triggering a `mark_dead()` → `erase()` on a connection that
    was never actually down - at the same moment a background `send()` is
    likely to be mid-flight. Self-inflicted timing, not a rare coincidence.
  - **Fix: `tools_list_mutex`** (new `inline std::mutex`, `olla.h`, same
    'inline'-not-'static' reasoning as `history_mutex` right above it - one
    shared instance across translation units, not one per). Guards exactly
    three sites: `send()`'s `register_tool()` loop, `process()`'s `erase()`
    (not the `monitor_tool()` loop immediately above it - deliberately left
    unlocked, since `monitor_tool()` can recurse back into `send()` on the
    same thread via a pushed event → `integrate_tool_result()` → `send()`,
    which would self-deadlock against a lock held across that whole loop),
    and `main.cpp`'s `push_back()` when a new remote tool connects (a
    `push_back` can reallocate the whole vector, same hazard as `erase`).
    Everywhere else `tools_list` gets touched (`dispatch_tool_call`'s
    lookup loop, `open()`'s one-time `configure()` pass, the display
    thread's tool-name list) only ever runs on the main thread, never
    overlapping a live `chat_thread` by construction, so left alone.
  - Confirmed fixed by reattaching gdb (`handle SIGPIPE nostop noprint
    pass` this time - the first attempt's `continue` stopped on the
    program's own deliberately-ignored `SIGPIPE` instead of the real crash)
    and reproducing the same trigger again with the fix in place: no
    second crash.

- **Build version, shown in the tools panel.** `OLLI_GIT_VERSION` (new
  `source/version.h`, generated by `GenerateVersionHeader.cmake` - a
  `configure_file()` call run via an `ALWAYS`-built custom target
  (`olli_version`, `CMakeLists.txt`), not `execute_process()` directly in
  the normal configure step, since that alone would only refresh on a
  fresh `cmake ..` and go stale after a plain `cmake --build .`) is
  `git describe --always --dirty` - content-addressed, not time-addressed:
  two people building the identical commit get the identical string, unlike
  a `__DATE__`/`__TIME__` stamp (also added, printed once at
  `main_process()`'s very start, kept alongside this rather than removed -
  cheap, harmless, and may prove useful for something else later).
  `ncurses_render_panel()` (`user_io.cpp`/`.h`) gained an optional `footer`
  parameter - claims `win`'s own last row, shrinking the scrollable
  viewport by one, so it can never collide with scrolled content. `win_tools`
  passes `"v." + OLLI_GIT_VERSION` (`TOOLS_PANEL_VERSION_FOOTER`); `tools_w`
  in `ncurses_layout()` widens past its 22-column base if that string
  wouldn't otherwise fit.

- **Task-runner file I/O: `[FILE_IN:name]` / `[FILE_APPEND:name]`.** New
  `read_file()`/`write_file()` (`helper_olli.h`/`.cpp` - the latter takes a
  trailing `bool append` so one function covers both overwrite and append,
  rather than two near-identical ones). Every task gets its own folder,
  `OLLI_DIRECTORY/files/<dir>` (`files_dir`, `TOOL_TASK_RUNNER::handle_tool()`) -
  a plain local, separate from `working_dir` right above it and from
  `working_dir`'s own lifetime: `working_dir` still gets `remove_all()`'d
  once the script finishes (unchanged), but `files_dir` is never cleaned up
  - anything `FILE_APPEND` writes there is meant to persist indefinitely.
  `<dir>` is `TASK_DIRECTORY` if the `.task` file set one, else `TASK_NAME`,
  either way passed through a new free function, `sanitize_path_segment()`
  (anonymous namespace, `tools.cpp` - a free function rather than a
  `TOOL_TASK_RUNNER` private method, since `advance_script_state()` needs it
  too and can't reach a class's private members): strips `/`/`\` outright so
  the result can never create or escape into a subdirectory, rejects a bare
  `".."`/`"."`/empty result, spaces → underscores. The same sanitizer runs
  on `FILE_IN`/`FILE_APPEND`'s own filename argument, so a script can only
  ever read or write flat filenames confined to its own folder - no nesting,
  no absolute paths, no `~`, no traversal out.
  - `[FILE_IN:name]`, on success, reads the file and feeds its content in as
    `current_input` (straight to `EXECUTE_COMMAND`, same as any plain
    line - no new wait-state needed, since unlike `[ASK]` the path is
    already known from the script text). On a missing/unreadable file, it
    doesn't abort - it falls back to the exact `WAIT_ASK` state `[ASK]`
    already uses (pause, show a request, take whatever gets typed/pasted as
    `current_input`, then continue the script normally), rather than ending
    the whole automation over one missing file.
  - `[FILE_APPEND:name]` appends `instance.last_received.response` (the
    previous line's output) to the file, creating `files_dir` first if
    needed, and continues immediately - same "no wait state" shape as
    `[PRINT]`. Deliberately plain concatenation for now, no separator
    between entries - confirmed in live testing that repeated runs blend
    together unreadably in the same file without one; left as-is per "keep
    it simple, complexity can always be added" rather than deciding a
    separator format unprompted.
  - `sample_scripts/resume_process.task` updated to use both: `[FILE_IN:resume.txt]`/
    `[FILE_IN:job_description.txt]` in place of the old
    `Ask the user for X.` + `[ASK]` pairs, `[FILE_APPEND:evaluation.txt]`/
    `[FILE_APPEND:cover_letter.txt]` added after each generation step. Live
    end-to-end test (real resume/job-description files, no restart needed
    between `.task` edits since it's reloaded fresh every call) surfaced two
    real content-quality issues worth a separate pass sometime: the model
    conflated three distinct, unrelated resume facts into one fabricated
    metric ("cut latency by 30%" attributed to the wrong project/company),
    and it manufactured a skill gap ("haven't used CMake") directly
    contradicting the resume's own Skills line - both propagated from the
    evaluation step into the cover-letter step once stated once, since it's
    one continuous conversation. Not an olli bug, but worth knowing this
    class of task is prone to it.

### Task-runner readability split; `#` comments; `[KEYBOARD_INPUT]`/`[TTS_OUTPUT]` toggles with auto save/restore (2026-09-07)

- **`tools_task_script.h`/`.cpp` split off from `tools.cpp`.** Purely a
  readability move, no behavior change: `advance_script_state()` and every
  `command_*()` function it dispatches to (`command_pause`, `command_ask`,
  `command_print`, `command_file_in`, `command_file_append`,
  `command_keyboard_input`, `command_tts_output`, `command_plain`,
  `command_get_command`, `command_wait_enter`, `command_wait_ask`,
  `command_execute_command`, `command_wait_response`) moved out to their own
  header/source pair as free functions (not `TOOL_TASK_RUNNER` methods -
  each takes exactly the references it needs, deliberately isolated from
  `chat`/`comms`/`task_manager`/`OLLI_DIRECTORY`), so the state machine's
  cases can be read and followed on their own instead of scrolling through
  `TOOL_TASK_RUNNER::handle_tool()`'s other ~1000 lines. `CMakeLists.txt`
  gained the new `.cpp` in `add_executable(olli ...)`.

- **`#` comment lines.** A parse-time filter in
  `TASK_SIMPLE_MANAGER::load_all_task()` (`tools_helper.cpp`): any line
  starting with `#` is skipped before it's ever pushed into `COMMANDS`, so a
  running script has no idea the comment ever existed - `advance_script_state()`
  needed no changes at all.

- **`[KEYBOARD_INPUT:on/off]` / `[TTS_OUTPUT:on/off]`.** New `COMMS` fields
  `ENABLE_KEYBOARD_INPUT`/`ENABLE_TTS_OUTPUT` (`comms.h`, both default
  `true`), set by the two new `command_keyboard_input()`/`command_tts_output()`
  functions parsing a plain `"on"`/`"off"` value off the command string.
  Deliberately **not** gated inside `IO_WORKER_CLASS::exchange()`
  (`io_worker.cpp`) - that function stays a complete, policy-free pass
  through in both directions; the two flags are just plain-copied from
  `comms` to `comms_buffer` there, same shape as the existing color-field
  copies right above them. The actual gating lives at the point each thing
  is produced/captured instead:
  - `thread_main()` syncs `key_input.PROPS.CHAT_INPUT_ENABLED` from
    `comms_buffer.ENABLE_KEYBOARD_INPUT` every tick, right before calling
    `keyboard_input()`. New plain `bool CHAT_INPUT_ENABLED = true` on
    `KEYBOARD_INPUT_PROPERTIES` (`user_io.h` - not atomic, unlike the
    existing `ENABLED`, since only the worker thread ever touches it).
    Inside `keyboard_input()` (`user_io.cpp`), every content-building branch
    (Enter, Ctrl+K/N/O, backspace, Alt+Enter, plain chars) is gated behind
    `PROPS.CHAT_INPUT_ENABLED`, and `INTERRUPTED = true` sits *nested inside*
    those same four guards rather than firing unconditionally - a full
    lockout by design, so a stray keypress while keyboard is off can't
    break a running script's cycle (confirmed: this was the one point of
    miscommunication while building it - first pass let `INTERRUPTED` fire
    regardless of the flag, corrected after clarifying the intent was total
    lockout, not just suppressing typed content). Ctrl+C/Tab/ESC-CSI parsing
    stay always-active regardless of the flag.
  - `ncurses_update_input_box()` (`user_io.h`/`.cpp`) takes a new
    `bool input_enabled` parameter and dims the input box under `A_DIM` when
    false, so a disabled keyboard is visually obvious ("ghosted") rather
    than silently inert. Called with `comms.ENABLE_KEYBOARD_INPUT` from
    `display_with_ncurses()`.
  - `thread_main()`'s TTS step checks `comms_buffer.ENABLE_TTS_OUTPUT`
    before calling `speakAsync()` - the text buffer itself is still always
    cleared either way, only the speaking is skipped.
  - **Auto save/restore around `[ASK]`/`[PAUSE]`.** Both inherently need a
    human to press a key regardless of the current keyboard setting, so
    rather than requiring a script to manually toggle `[KEYBOARD_INPUT]` on
    around every input-requiring command, `command_pause()`/`command_ask()`
    save the current `ENABLE_KEYBOARD_INPUT` value into a new
    `bool& keyboard_was_enabled` (threaded through `advance_script_state()`
    and every `command_*()` signature down from `TOOL_TASK_RUNNER::
    handle_tool()`'s local), force it on for themselves, and
    `command_wait_enter()`/`command_wait_ask()` restore the saved value once
    the wait completes. `command_file_in()`'s own `WAIT_ASK` fallback (when
    the file can't be read) does the same. Net effect: a script only needs
    one `[KEYBOARD_INPUT:off]` near the top; `[TTS_OUTPUT]` has no such
    auto-restore, since nothing about `[ASK]`/`[PAUSE]` inherently needs
    audio.
  - `sample_scripts/system_test.task` updated to demonstrate both: keyboard
    off for the whole run, TTS off for just the first section, `#` comments
    explaining why. Live end-to-end test confirmed working.

### Web-search links: suppressed a stray `std::cout`, then a real clickable-link popup (2026-09-07)

- **Bug: `TOOL_WEB_SEARCH` bypassing the comms channel.**
  `perform_actual_search()` (`tools.cpp`) had a leftover
  `std::cout << "[System] Result Found: " << make_clickable(link, title)`
  for each organic search result - a raw write straight to the terminal
  while ncurses owns the screen, invisible to `COMMS`/`OUTPUT_CLASS`
  entirely, corrupting the display. Removed outright: the same title/
  link/snippet already flows back to the model through the function's own
  `summary` return value, so nothing was lost. `make_clickable()` (the
  OSC 8 escape-sequence formatter it called) became dead code as a result
  and was removed too - a fresh copy was written locally in `user_io.cpp`
  for the popup below instead of sharing the old one, so that file doesn't
  need to depend on `tools.h`.

- **Investigated: can a link render as clickable straight in the ncurses
  chat panel?** A standalone test program (outside the repo) confirmed
  no - `waddstr()`/`addstr()` sanitizes unprintable control bytes
  (including the OSC 8 sequence's own ESC bytes) into visible caret
  notation instead of passing them to the terminal, so an inline link
  just shows as garbled escape text, worse the longer the URL. Plain bare
  URLs (relying on the terminal's own auto-linking of a `https://...`
  substring) were also ruled out for the same reason a raw
  `perform_actual_search` URL from a real API is long (query strings,
  session tokens) - inline, it breaks up the response text badly. The
  same standalone test then confirmed a fix: dropping out of curses mode
  entirely (`def_prog_mode()`+`endwin()`) and writing the OSC 8 sequence
  with a raw `std::cout` *does* render as a real, working clickable link
  (confirmed live - clicked it, it opened the page) - ncurses just never
  gets a chance to sanitize bytes it never sees.

- **The feature: `COMMS::WEB_LINKS` + Ctrl+L popup.** New
  `std::vector<std::pair<std::string, std::string>> WEB_LINKS` on `COMMS`
  (title, url pairs) - accumulate-then-drain, same shape as
  `INPUT_FROM_LLM`: `TOOL_WEB_SEARCH::perform_actual_search()`/
  `fetch_url_content()` (now taking `COMMS&`) push directly from the raw
  search/fetch result under `output_buffer_mutex`, not parsed back out of
  the model's own rewritten response text (which may paraphrase or drop
  them). Relayed on by `IO_WORKER_CLASS::exchange()` exactly like every
  other field there - a plain pass-through, no gating. The model's own
  tool description no longer tells it to emit a `CLICKABLE_LINK(url,
  text)` marker (the old, never-actually-converted format) - it's told to
  just refer to a source by name instead, since the real link is already
  shown separately.
  - `OUTPUT_CLASS::get_response()` drains `comms.WEB_LINKS` into a new
    `web_links` member - unlike the four string buckets it also drains,
    this one is never cleared afterward, since it needs to still be there
    whenever the popup opens, not just the tick it arrived on.
  - `display_with_ncurses()` appends a short `"[Links: [1] title, [2]
    title - Ctrl+L to open]"` notice right after the response text that
    surfaced them, in a new dedicated color (`PAIR_WEB_LINKS_NOTICE`,
    blue) so it reads as a UI hint rather than part of the assistant's
    answer. `web_links_shown_count` tracks how many entries have already
    been announced, so a link is never mentioned twice across ticks.
  - New `KEYBOARD_INPUT::SHOW_LINKS_REQUESTED`, set by Ctrl+L (byte 12,
    previously unused) the same read-and-cleared-by-caller way `Tab`/Page
    Up/Down already work. `IO_WORKER_CLASS::thread_main()` acts on it
    right after `get_response()` (so the popup always sees whatever just
    arrived that tick), calling the new
    `OUTPUT_CLASS::show_web_links_panel()`: drops out of curses mode,
    lists every `web_links` entry as a real OSC 8 link via raw
    `std::cout`, polls the same raw non-blocking stdin read the rest of
    input handling uses until a key is pressed, then resumes curses and
    forces a full redraw (`clearok(curscr, TRUE)`) to recover from
    whatever the raw prints did to the physical screen. Confirmed working
    live, colored notice included.
  - **Generalized 2026-09-10** into `COMMS::TOOL_ATTACHMENTS`
    (`vector<TOOL_ATTACHMENT>`, a `{type, label, content}` struct instead
    of a plain pair) - see "Exact-data side channel" under `tools/rag`'s
    entry (New tools, above) for why. `TOOL_WEB_SEARCH` now pushes
    `type="link"` entries; every relay/rendering spot named above
    (`exchange()`, `get_response()`, the notice, `show_web_links_panel()`)
    got the matching mechanical rename, with zero behavior change - still
    unconditionally renders every entry as a link, since nothing but web
    search populates it yet. The "document" type existing but not yet
    rendered as anything but a link, if one ever reaches display before
    that catches up, is the known gap to close before wiring an actual
    document producer through - see the RAG entry's own note on this.
  - **Display step done 2026-09-10** - closes the gap noted just above.
    See the RAG entry's own "Step 3" note (New tools, above) for the full
    detail: numbered selection in `show_web_links_panel()`, and a new
    `DOCUMENT_VIEWER` class (`source/document_viewer.h`/`.cpp`) for
    non-link attachments, including two real screen-corruption bugs found
    and fixed while building it.

### `thread_main()` reworked into three parallel per-channel comms copies; `COMMS` gained `operator=` (2026-09-10)

- **Why:** built as the prerequisite for a browser-driven interface (see
  "Remote access" below) - `IO_WORKER_CLASS::thread_main()`'s tick used to
  drain/display `comms_buffer` directly for keyboard/ncurses and a second
  `comms_buffer_audio` copy for TTS, each handled by its own bespoke
  logic. Adding a third channel (web) on that same shape meant
  generalizing the pattern first rather than bolting a special case onto
  it - went through several rounds of design correction before landing
  here (see this session's own conversation for the false starts: an
  accumulate-into-`comms_stt_tts` version that didn't match the intended
  shape at all, then a version that cleared `comms_buffer` too early and
  would have wiped what it had just handed the channels before display
  even ran).
- **The new shape:** three per-channel `COMMS` copies - `comms_keyboard`,
  `comms_stt_tts` (STT input + TTS output, one shared channel covering
  both directions of voice, replacing the old TTS-only
  `comms_buffer_audio`), `comms_web` - each snapshotted from
  `comms_buffer` (`comms_X = comms_buffer`) at the start of a tick. Each
  channel's own input source (`key_input.ENTER_PRESSED`,
  `popVocaEvent()`, `WEB_SERVER_CLASS::poll_input()`) writes into its own
  copy and hands the WHOLE copy back to `comms_buffer` as one unit
  (`comms_buffer = comms_X`) rather than a field-by-field merge - "comms
  is just a holding place," not something needing per-field merge logic
  at every call site. A second re-sync right before the display/speak/
  push step gives every channel the authoritative post-merge state, and
  only *then* is `comms_buffer`'s own screen/speech-direction copy
  cleared (`INPUT_FROM_LLM`/`THINKING`/`SYSTEM`/`TOOL_ATTACHMENTS`) -
  clearing any earlier would wipe what the first snapshot just gave the
  channels before they'd had a chance to use it. If `exchange()` hasn't
  picked up a still-pending submission yet when a new one arrives (same
  tick or a different channel racing it), it's appended onto
  `INPUT_FROM_USER` with a `\n` rather than dropped or overwritten - a
  deliberate behavior change from the old "don't stomp" guard, since two
  rapid submissions now both survive instead of the second one vanishing.
- **`display_with_tts()` is a new dedicated method**
  (`io_worker.h`/`.cpp`), replacing the old inline accumulate-and-speak
  block that read from `comms_buffer_audio` - now reads/clears
  `comms_stt_tts.INPUT_FROM_LLM` each tick into its own `tts_pending`
  accumulator (`comms_stt_tts` itself gets wholesale-overwritten every
  tick, so anything not yet spoken has to live somewhere that survives
  that independently), speaking once `tts` is actually idle, same pacing
  as before.
- **`COMMS` gained a real `operator=`** (`comms.h`) - needed because
  `std::atomic<bool> close_chat_log_requested` implicitly deletes it
  otherwise (a `-Werror=shadow`-style compiler error caught this
  immediately on the first build attempt). The new operator copies every
  other field and deliberately leaves that one alone - a standalone
  main-thread -> io-thread signal, never part of the round-trip.
- **`exchange()` simplified** - no longer fans a second copy
  (`comms_buffer_audio`) directly from the real `comms`; `thread_main()`
  now does that copying itself, locally, once per tick, before draining
  `comms_buffer`. Kept as a pure subtraction from `exchange()`, not a
  rewrite of it - the caller-facing contract (`comms` in, `comms_buffer`
  out) didn't change.
- Verified: full local session testing (keyboard, TTS speech, olli's own
  responses) confirmed working identically to before the rewrite - this
  was a pure internal restructuring, no user-visible behavior change on
  its own. Stale comments left behind in `main.cpp`/`olla.h`/`olla.cpp`
  still describing the old `comms_buffer_audio`/`exchange()`-fans-it-
  directly shape were cleaned up in the same session once noticed.

## Voice (Voca)

- Wake word is "olli" (`findWakeWord()`/`findSleepTrigger()`, now in
  `io_worker.cpp` - see "What we built today (2026-08-28)" below) - done, no
  longer "voca".
- **Interrupt-burst spam right after a fresh session starts - root-caused
  2026-09-16, fix deferred.** Confirmed live in a real `debug_full_history.txt`:
  interrupting olli (a real spoken interrupt phrase) while it's speaking -
  seen specifically while `sidetrack-second-guess`'s own TTS/thinking is
  active, early in a session - produced **11 separate
  `"interrupted mid-response - stopping"` log lines in ~2.2 seconds**
  (120-500ms apart), instead of one clean interrupt. Confirmed by the user
  to reproduce reliably as "the first interrupt of a session," then never
  again for the rest of that same session.
  - **Root cause, traced through real code, not the `comms.INTERRUPTED`-
    stuck-true bug this looks like at first glance (that one's the
    opposite failure - stuck permanently `true`; already fixed 2026-09-03,
    see "Sidetrack rewrite" above) and not `io_worker.cpp`'s `onInterrupt`
    callback pushing an empty-text `VOCA_EVENT` either (`io_worker.cpp:
    967-973`) - that's deliberate design for a pure "stop now" signal, not
    a bug.** The real chain: `busy_` (`Voca`, `io_worker.h:200`) means "TTS
    is currently speaking," toggled by `adjust_audio_files()`
    (`io_worker.cpp:809-831`) via `voca->pause()`/`resume()` purely so Voca
    doesn't transcribe olli's own voice coming out of the speakers.
    `handleTranscript()`'s interrupt branch (`io_worker.cpp:719-724`) fires
    `onInterrupt` on *any* transcript segment that matches an interrupt
    phrase while `busy_` is true - with no debounce. If TTS audio takes a
    little while to actually finish draining out of the audio device after
    `stop_speaking()` is called (buffered samples still playing), the mic
    keeps picking up audio during that drain window, whisper keeps
    producing new segments, and each one that still matches (or mishears
    olli's own trailing audio) re-fires `onInterrupt` again - a burst, not
    one clean signal. Matches the observed pattern: happens during a real
    interrupt while TTS is speaking, stops once TTS audio actually finishes
    draining and `resume()` clears `busy_`.
  - **Not yet explained: why only the *first* interrupt of a session.** If
    the re-fire were purely about drain-window timing, it should reproduce
    on every interrupt, not just the first. Leading guess, unconfirmed -
    one-time startup latency in the audio pipeline on `TextToSpeech`'s very
    first utterance (device/stream init, first-call buffering) that later
    calls don't pay. `TextToSpeech`'s own `stop()`/`isSpeaking()` haven't
    been read yet to confirm.
  - **Proposed fix, agreed but deliberately not yet implemented (deferred
    at the user's request, 2026-09-16):** a simple debounce, not a chase
    of the exact timing cause - a new bool (e.g.
    `interrupt_already_signaled_`) on `Voca`, set the first time
    `handleTranscript()`'s interrupt branch fires `onInterrupt` during one
    busy period, checked before firing again, cleared in `resume()` so the
    next real busy period starts fresh. Contained to `io_worker.h`/`.cpp`,
    fixes the burst regardless of which subsystem is actually slow to
    drain.

## Remote access

- **Done 2026-09-10: a browser-driven web interface**, alongside (not
  instead of) the local terminal - answers the "expose an API" item this
  entry used to be. New `WEB_SERVER_CLASS` (`source/web_server.h`/`.cpp`)
  owns a `cpp-httplib` `httplib::Server` and its own accept/serve thread
  (unavoidable - sockets can't be polled synchronously inside
  `thread_main()`'s ~20ms tick, same reasoning Voca's capture/transcribe
  threads and `TextToSpeech`'s own worker thread already rely on).
  Listens on **port 47602** (separate from the remote-tools protocol's
  47601, see `tools/PROTOCOL.md`), bound `0.0.0.0` rather than
  loopback-only, since the whole point is LAN reach - no authentication,
  trusted home LAN only, same trust boundary as remote tools. Built on
  top of the `comms_web` channel added to `IO_WORKER_CLASS::thread_main()`'s
  per-channel rework (see "Display / OUTPUT_CLASS" above) -
  `display_with_web()`'s signature deliberately matches
  `display_with_ncurses()`'s in full (not just what phase 1 actually
  uses) so growing toward feature parity later doesn't need the call
  site touched again.
  - **The page**: one self-contained HTML/JS/CSS string (`GET /`, no
    external assets, no build step, no framework) - a scrolling
    transcript, an input box, `EventSource` against `GET /events`
    (Server-Sent Events) for live updates, `fetch()` `POST /input` on
    send. Streamed chat text accumulates into one growing element per
    turn rather than one `<div>` per chunk - the first version got this
    wrong (a `<div>` is block-level, so every streamed chunk forced its
    own line) and needed a live-tested fix.
  - **Thinking display matches ncurses' own floating box behavior**
    (`display_with_ncurses()`'s `win_thinking`, `user_io.cpp`) rather
    than inlining into the transcript, after an initial version that put
    it inline got corrected: a separate floating box in the upper-right
    corner, closes the moment the real reply starts (not on a
    thinking-side timeout - same signal ncurses uses,
    `in_thinking_block`/`chat_response` in `user_io.cpp`), lingering ~2s
    first (`THINKING_BOX_LINGER_MS` mirrored in JS) before disappearing.
    A first pass of this also had a scroll bug - scrolling the wrong
    (non-scrollable) inner element instead of the actual scrollable
    container - caught live and fixed.
  - **Tools panel**: a right-side list of currently-registered tools,
    kept as current STATE rather than an append-only stream -
    `WEB_SERVER_CLASS::push_tool_names()` only broadcasts on an actual
    change, but a brand-new `/events` connection always gets the current
    list once regardless (forced via `tool_names_dirty_`), so a
    freshly-opened tab isn't stuck waiting for the next change to see it
    - confirmed live with back-to-back connections.
  - **Links render as real clickable `<a>` tags** - simpler than
    ncurses' own `[Links: ...]` notice + Ctrl+L popup dance, since a
    browser doesn't have ncurses' OSC-8-escape-stripping problem (see the
    web-search links entry, Display section above).
  - **Deliberately deferred for now** (flagged as later work, not
    forgotten): live-typing echo (`input_from_user_echo` - would need the
    page posting on every keystroke, not just on submit, real added
    scope rather than just filling in an existing parameter); scroll/
    focus-cycle parity (a browser already scrolls its own transcript
    natively, so `scroll_request`/`focus_cycle_requested` may not even
    need a real equivalent); auth (explicitly out of scope - trusted home
    LAN only); real multi-tab support (the output/tools-panel queues are
    single shared buffers, not per-connection - built for the expected
    one-viewer-at-a-time case, a second simultaneous tab can occasionally
    race the first for the same pending chunk).
  - Verified live end-to-end: connected from a separate Windows machine
    on the LAN (after finding/fixing a `ufw` firewall block on the olli
    host - a connection timing out rather than being refused was the
    tell), full round-trip chat confirmed working, thinking box and tools
    panel both confirmed against a real running session, not just
    compiled.
  - **Done 2026-09-12: cross-channel echo, interrupt parity, per-instance
    colors, keyboard-disable parity.** A real bug found via live use: a
    line typed/spoken via keyboard or STT never showed up in the web
    transcript, and (less obviously) STT input never showed up in the
    *terminal's own* transcript either - `output.user_input` (the bucket
    `display_with_ncurses()` reads for "what the user typed/said") was
    only ever written to by keyboard's own merge step, despite a comment
    on it already saying "echo... once, uniformly for typed and voice
    input." Fixed by adding the same echo to STT's and web's merge steps
    too, plus a new `WEB_SERVER_CLASS::push_user_message()` (a `user` SSE
    event) relaying keyboard/STT submissions into the browser - never the
    other direction, since the browser already echoes its own submission
    instantly, client-side, on `send()`.
    - **Interrupt parity**: at the terminal, just starting to type stops
      TTS/generation (`KEYBOARD_INPUT::keyboard_input()`'s "any keystroke
      sets `INTERRUPTED`", `user_io.cpp`). The web page can't mirror that
      literally without live-syncing every keystroke (the live-typing
      echo already ruled out as unnecessary), so it does the same thing
      at a coarser grain instead: one `POST /interrupt` ping the moment
      the input box goes from empty to non-empty, polled once per tick
      (`WEB_SERVER_CLASS::poll_interrupt()`) and OR'd into the same
      interrupt-handling step the keyboard's own flag already goes
      through.
    - **Per-instance chat colors, relayed to the web page.**
      `COMMS::INPUT_FROM_LLM_COLOR`/`INPUT_FROM_USER_COLOR` (raw ncurses
      attributes, set per-instance in `tools.cpp` - task-runner cyan/
      yellow, delegator magenta/green) were already flowing into
      `comms_web` for free, just never read there. New
      `ncurses_attr_to_css_color()` (`web_server.cpp`) decodes the pair
      number + bold/dim bits via pure bit-math macros rather than calling
      ncurses' own `pair_content()` at runtime (which needs ncurses
      actually initialized - would break running headless/web-only) -
      hardcoded against the same 5 pairs `user_io.cpp` registers, a small,
      stable duplication rather than a runtime dependency. `llm`/`user`
      SSE events now carry `{text, color}` instead of a bare string, and
      the reply now renders as one `<span>` per chunk (inline, so it
      still flows as one line) instead of one accumulating text node, so
      a color change mid-reply doesn't break anything.
    - **Bare Enter now works from the web page** - a running `.task`
      script's "press enter to continue" (`command_wait_enter()`,
      `tools_task_script.cpp`) only checks `ENTER_PRESSED`, never the
      submitted text, but the page's own `send()` refused to fire at all
      on an empty box. Now submits `"\n"` instead, matching
      `keyboard_input()`'s own behavior exactly rather than inventing a
      different convention.
    - **Keyboard-disable parity, functional and visual.** A running
      `.task` script's `[KEYBOARD_INPUT:off]` (`COMMS::
      ENABLE_KEYBOARD_INPUT`) already made the keyboard channel silently
      discard typing rather than queue it for later; the web channel
      checked nothing at all. Now gated the same way (still drains the
      pending submission so it doesn't pile up, just doesn't act on it).
      New `push_keyboard_enabled()` (same current-state broadcast shape
      as `push_tool_names()`) also dims the page's own input box/send
      button via the native `disabled` attribute when this is off,
      mirroring `ncurses_update_input_box()`'s own dimming - `disabled`
      also means the box can't receive focus/keystrokes at all while off,
      so no separate guard was needed to block `send()`/the interrupt-
      ping from firing during that window.
    - A UI-only follow-up along the way: the thinking box was widened
      (its left edge moved from a fixed width to `left: 30%`, right edge
      unchanged) after live feedback that the original was too narrow.

## Open questions / carried over

- Consolidation summaries can cement a bad pattern as an established "fact"
  about the assistant (seen firsthand in a "locked door" persona-drift loop
  from a poisoned `history.json` this session). No fix decided - revisit if
  it recurs. The 30-minute idle auto-clear (`SIDETRACK_CLASS` ROUTINE 3 in
  `sidetrack.cpp`/`.h`) is now in place as a plausible mitigation - a stale,
  poisoned context can't outlive 30 minutes of silence - but isn't confirmed
  to actually fix the underlying issue. `OLLAMA_OPENING` (olla.h) also
  dropped the "snarky" persona for a cyberpunk-lingo one (2026-08-21) since
  snark was suspected to correlate with getting stuck in this kind of
  recursive-response loop as context grew large - also unconfirmed, revisit
  together if it recurs. A concrete instance of the same shape caught
  2026-08-23 in a real `history.json`: sidetrack's second-guess review
  (`SECOND_GUESS_PROCESSING_STAGE`, `sidetrack.cpp`) kept re-raising the
  exact same stale point ("only group 0 was confirmed" after a Hue command)
  after four consecutive unrelated turns - it's shown the whole
  conversation but only told to review "the turn that just ended," with
  nothing marking where that boundary is or telling it not to repeat a
  point it already made in an earlier note. Added an explicit
  don't-repeat-yourself instruction to that prompt as a mitigation - not a
  structural fix (the model still has to notice its own prior notes and
  self-censor), so revisit if it still recurs.
  - **Another concrete instance, 2026-09-23**: second-guess generated pure
    nonsense ("DUN. You got what you paid for." -> "DUN. You got your DUN.
    Stay sharp, kid.") and committed it to history on an essentially empty,
    freshly-restarted conversation - the very first thing visible on
    screen, with zero real user interaction yet to have "reviewed." The
    stray "DUN" tic then measurably persisted into later, real, unrelated
    replies. The 2026-09-23 second-guess fix (narrower prompt, real
    actions always visible - same section, "Session & model behavior")
    targets the *autonomous-action* half of second-guess's problems, not
    this *fabricating text from nothing* half - unconfirmed whether the
    narrower prompt happens to help here too. Revisit/re-test
    specifically for this if it recurs.
- Same shape of problem as the item above, but from plain persisted history,
  not a consolidation summary - seen concretely while developing the
  remote-tools feature (2026-08-22): early testing recorded "remote tool
  call round-trip not implemented yet" in raw history while `TOOL_REMOTE`'s
  round trip genuinely didn't work yet (Step 3). After it actually got
  implemented (Step 4) and worked when tested standalone, the model still
  insisted the clock didn't work in a session that had loaded the old
  `history.json` - only went away after deleting it and restarting fresh.
  A tool's own past failure getting cemented as a permanent fact is the same
  underlying issue as the consolidation-drift case above, just without
  consolidation involved - whatever fix eventually gets decided there should
  probably account for plain history too, not just summaries.
- **Found 2026-09-23, not fixed: a `[WAIT_FOR_RESULT]` task-runner script
  step has no timeout of its own.** Found via the same 90-minute testing
  round, alongside (and likely triggered by) the remote-tool listener bind
  failure above - a `qa_wait_test.task` run hit its own `[WAIT_FOR_RESULT]`
  step waiting on a `set_timer` call that could never return (nothing was
  actually connected to answer it), and the wait blocked not just that
  task-runner instance but the entire `chat` channel - neither `bye` nor a
  plain typed message produced any new activity for over 3.5 minutes,
  needing a forced kill. Distinct from the already-known/mitigated
  `pending_tool_calls`/`last_received` race (2026-09-22 entries above) -
  this is specifically about `[WAIT_FOR_RESULT]`'s own wait loop
  (`tools.cpp`, `TOOL_TASK_RUNNER::handle_tool()`) never giving up on a
  call that simply never comes back. Not investigated or fixed - the
  listener-bind fix above makes the *specific trigger* (silently missing
  remote tools) far less likely to happen by accident, but doesn't touch
  the underlying gap: a genuinely connected tool that hangs for any other
  reason would still freeze the whole program the same way.
- **Found 2026-09-23, not fixed: raw, unnarrated DIRECTOR_NOTE text leaking
  verbatim into the visible chat.** Same testing round, one occurrence: at
  the exact seam the already-known-and-mitigated Phase 2 event-framing race
  lives in (timer expiry + a concurrent user message), the model's
  response to the `[TIMER EXPIRED]` DIRECTOR_NOTE was the raw DIRECTOR_NOTE
  text itself, shown directly to the user instead of a real narrated reply.
  Not a hang, milder than the disabled Phase 2 framing change's own failure
  mode, but a real, live-caught symptom of the same underlying fragility -
  not investigated further.
- **Found 2026-09-23, not reproduced: one real crash with no backtrace -
  root cause of the *missing backtrace* found and fixed same day; the
  underlying crash itself is still unreproduced.** `crash_log.txt`: "olli
  was exited abnormally (code 134)" (SIGABRT) right as a hard
  `consult_expert` question was sent - unlike the two 2026-09-22 crashes in
  the same file, this one captured no backtrace, despite the crash handler
  (2026-09-22 entry, above) being installed and working for those two.
  Re-sending the identical message afterward did not reproduce it.
  - **Why the backtrace itself went missing, found via a fast read of
    `crash_signal_handler()` (`main.cpp`)**: its re-entrancy guard
    (`g_in_crash_handler`, a plain bool) was meant to stop a fault
    recursing inside its OWN handler (e.g. `backtrace()` itself faulting),
    but being a process-wide signal handler with no thread awareness, it
    also silently ate a second, genuinely DIFFERENT thread's own crash if
    one fired while the first was still mid-write - that thread's own
    handler invocation would see the guard already set and immediately
    `_exit()` with zero output. If that instant exit won the race against
    the first thread's careful write-then-raise sequence, the whole
    process vanishes with no trace at all, matching exactly what's in the
    log (the two 2026-09-22 crashes both have full backtraces; this one
    has none, just the supervisor's own summary line).
  - **Fix**: the guard now compares thread ids (`gettid()` via the raw
    `syscall(SYS_gettid)`, not glibc's own wrapper - async-signal-safe,
    and doesn't assume a glibc version that has it built in) instead of a
    plain bool - only bails out for true same-thread self-recursion; a
    genuinely different thread's crash still gets to write its own
    backtrace (the two can interleave in the log if that happens, which
    is still strictly better than one vanishing entirely).
  - **Verified no regression**: re-ran the existing `--debug-crash` hook
    (deliberately segfaults 5s in, real tmux pty per the original
    2026-09-22 gotcha) through all 3 of the supervisor's own crash-restart
    attempts - every one produced a complete, correct backtrace exactly as
    before, and the give-up-after-3 behavior was unaffected.
  - **What this does NOT explain**: why the underlying abort() happened in
    the first place - only why its evidence went missing. The two
    2026-09-22 crashes were separately diagnosed and fixed (the shutdown
    thread-join gap, same section above); this one could be a fresh
    instance of a similar "exception escapes a thread" class of bug, or
    something else entirely - still not reproducible, so still not
    actionable beyond this. If it recurs, it should at least leave a real
    backtrace to work from now.
- **Found and fixed 2026-09-24: `set_hue_light` returning a genuine
  network hiccup as a flat, un-retried failure, plus `manage_hue_scenes`
  having no way to report a bridge failure honestly at all.** Found live,
  outside any testing round - the user asked to turn the lights on, olli
  said it timed out, and a little later a repeated request worked fine.
  - **Root cause**: `HUE_LIGHT_CLASS::make_request()` (`tools/hue/hue.cpp`)
    - the one function every Hue API call funnels through - had a flat 5s
    `CURLOPT_TIMEOUT` covering everything, including a group "all lights"
    command, which can genuinely take the bridge a moment longer than a
    single light, with no retry at all on a plain transient timeout. Same
    class of bug `TOOL_WEB_SEARCH::curl_get()` (`source/tools.cpp`) was
    already fixed for (2026-09-16 entry, below) - this tool just hadn't
    gotten the same treatment.
  - **Fix**: `make_request()` now matches `curl_get()`'s own shape exactly
    - one retry, but only on `CURLE_OPERATION_TIMEDOUT` specifically (a
    bad URL/connection-refused/host-not-found would just fail identically
    again, so retrying those doubles the wait for nothing), overall
    timeout raised 5s -> 8s, plus its own separate 3s
    `CURLOPT_CONNECTTIMEOUT` (the bridge is a local LAN device - a
    connection that hasn't even opened within a few seconds is genuinely
    unreachable, not just slow to answer, and shouldn't eat the whole
    request budget finding that out).
  - **Separately, while checking this: `set_hue_light`/`list_hue_lights`
    already had an honest-failure signal for the model
    (`failure_instruction`, mirroring `TOOL_WEB_SEARCH`'s own
    `Special_Instruction` wording exactly - "this is not real data, tell
    the user plainly it failed"), but `manage_hue_scenes`'s save/load
    actions didn't.** Not as broken as first suspected on a quick look -
    `save_scene()`/`load_scene()` already produced reasonably readable
    failure text (not raw curl noise), they just had no way to tell
    `handle_call()`'s dispatch "this one's actually a failure" vs. an
    ordinary success, so the same honesty framing never got attached.
    Fixed by changing both to return `{ok, message}` (matching
    `TOOL_WEB_SEARCH::curl_get()`'s own established `std::pair<bool,
    std::string>` convention exactly), with `handle_call()`'s dispatch now
    attaching `failure_instruction` whenever `ok` is false.
    `remove_scene()` deliberately left as a plain string - it's purely
    local (a map erase + a disk write), no bridge call at all, nothing to
    ever be dishonest about.
  - **Noticed but not touched**: `refresh_lights()`'s own error-detection
    only explicitly checks one of the two error shapes
    `response_is_error()` normally distinguishes (the array-shaped one,
    not the object-shaped `{"error": "CURL failed: ..."}` a real curl
    failure actually produces) - still correctly returns failure either
    way, just via an exception fallback instead of a clean check. Not
    actually broken, just not as tidy as it could be - flagged, not fixed,
    per this project's own scope-discipline convention.
- **Built 2026-09-24: `SUBCON_WORKER_CLASS` (`source/subcon_worker.h`/
  `.cpp`) - a skeleton dedicated-thread worker for "the subconscious" (see
  `IDEAS.md`'s own section), with a real, live-verified LLM round trip
  proven working end to end. No real reasoning/design behind it yet -
  purely the plumbing, built incrementally and smoke-tested at every step.**
  - **Modeled on `TOOL_WORKER_CLASS`, not `IO_WORKER_CLASS`** - chosen
    deliberately: `IO_WORKER_CLASS`'s single combined `exchange()` exists
    because keyboard/audio genuinely need a full `COMMS` snapshot every
    tick; this worker doesn't need that shape, and it explicitly isn't
    built sidetrack-style either - a real dedicated thread, not a tick-
    based state machine riding the main thread's own loop (and inheriting
    every stall that has, e.g. `TOOL_TASK_RUNNER::handle_tool()`'s
    synchronous blocking).
  - **`thread_main()` made private on all three worker classes**
    (`SUBCON_WORKER_CLASS`, `TOOL_WORKER_CLASS`, `IO_WORKER_CLASS`) - it was
    only ever invoked internally via `thread_start()`'s own lambda, which
    has the same access as any other member function; moved for
    correctness on all three at once rather than leaving the two existing
    classes' own pre-existing (arguably wrong) public declarations as-is.
  - **Its own `ollama_system` (`subcon_llm`), fully isolated**: own
    `debug_label` ("subcon", tagging its lines in the one shared
    `debug_full_history.txt` - `DEBUG_LOG_CLASS` is a global singleton, one
    log file for the whole process, not per-instance-directory-controlled),
    own `OLLI_DIRECTORY` (`.../subcon`, created by `open()`'s own
    unconditional `create_directories()` calls - confirmed on disk via a
    live run), `LOAD_SAVE_HISTORY_ON_DISK = false` (never touches the real
    `history.json` - the exact trap `SIDETRACK_CLASS::run_consolidation()`
    already had to avoid), `use_thinking = true` (chat itself runs with
    this off), and `stream_output`/`stream_thinking` both off (nothing
    ever displays this instance's output, so there's nothing for
    incremental chunks to write to - `send()` just takes the plain
    non-streaming path instead, same end result). `PROPS` starts as a
    wholesale copy of chat's own (so model/host/port always track chat's
    automatically) with these overrides applied on top, not cherry-picked
    field by field.
  - **Its own persona** (`OLLAMA_OPENING`, set before `open()` so it seeds
    the protected opening message, same order `TOOL_DELEGATOR`'s own
    instance uses) - added after a live test showed the *default* persona
    (written for a general assistant with tool guidance) bleeding through
    as an in-character reply with nothing to do with what subcon actually
    is. Re-tested after the fix: plain, on-topic replies.
  - **Deliberately passive for now**: empty `tools_list`, self-contained to
    `thread_main()` - no tools at all yet, matching the safety principle
    from the `IDEAS.md` brainstorm (a background process that can only
    think out loud is a very different risk than one that can also act).
  - **A real `input()`/`process()`/`send()` round trip, proven live**: a
    one-shot test prompt (fires once via a `TIMED_IS_READY_SIMPLE` delay
    and a `bool` latch - an earlier version re-armed the timer every 60s,
    caught and fixed before pushing since a repeating, unremovable nag was
    exactly the failure mode being designed against) goes through
    `subcon_llm.input()`/`.process()` the same way real user input does,
    and the real response gets logged via `DEBUG_LOG_CLASS` once it
    arrives - confirmed with real model output in the log (~3-9s round
    trip), confirmed it does not repeat.
  - **`process()`/`handle_instance_tools()` need an `IO_WORKER_CLASS&` (a
    reference, not nullable)** even though subcon has nothing to do with
    the real one - resolved with a real but never-started, fully self-
    contained `IO_WORKER_CLASS subcon_io_worker` local to `thread_main()`,
    safe because that reference is only ever actually touched inside
    `dispatch_tool_call()`, which can't fire with an empty `tools_list`.
  - **No `close()` exists anywhere on `ollama_system`** - confirmed via
    grep; what other instances call "instance closed" is just a
    `DEBUG_LOG_CLASS` log tag, not a real teardown call. Left alone for
    subcon for now (undecided, not forgotten).
  - **Verified after every single increment**, not just at the end - each
    step (thread lifecycle, the privacy fix, the local `ollama_system`,
    the `PROPS` copy chain, the tools list, `open()`, `COMMS`, the
    `input()`/`process()` loop, the persona, the one-shot fix) got its own
    clean rebuild (`-Wall -Wextra -Wpedantic -Wconversion -Werror`) and a
    real `tmux`-driven start/`bye`/exit check against `crash_log.txt`
    before moving to the next piece.
  - **Not yet built**: any actual reasoning/scheduling logic - what
    triggers a real thought, a digest queue, tying into the awareness/
    event layer. All still exactly as scoped in `IDEAS.md`; this entry
    only covers the plumbing now proven to work.
- **Found and fixed 2026-09-24: `sidetrack-second-guess`'s own DONE-check
  never once recognized a real "DONE" answer, causing a live, active
  self-chaining review loop on the user's own real `ron` profile
  (bounded by `SECOND_GUESS_MAX_CHAIN`, but wasteful and polluted history
  with junk "corrections" while it ran).** Caught live, not in testing -
  the user noticed second-guess "isn't picking up done."
  - **Root cause**: stage 4's `starts_with(answer, "DONE")` check
    (`sidetrack.cpp`) assumed the model would lead with the marker. Real
    replies never did - every one looked like a short explanation
    followed by the marker at the *end* ("...nothing to correct.\n\n
    DONE"), which a leading-only check can never match. Every DONE-check
    this session read as "not done," which sent it to the "go ahead and
    correct something" stage, which committed a new assistant message to
    real history - which, by design (self-chaining off its own follow-up
    is intentional, capped by `SECOND_GUESS_MAX_CHAIN = 10`), immediately
    triggered another review of itself. Confirmed live: 10 straight
    cycles, none of them a real correction.
  - **First pass, since superseded**: added a matching trailing-DONE
    check (strip trailing whitespace/periods/markdown emphasis, check the
    end instead of the start) - fixed the immediate bug, verified live
    (single review cycle, correctly recognized `**DONE**` on its own
    trailing line).
  - **Real fix, replacing the trailing check same day**: Ollama supports
    structured outputs (a JSON Schema passed in the request's `format`
    field, constraining the model's own token generation server-side
    rather than hoping a phrase lands somewhere findable in free text) -
    installed Ollama here is 0.34.1, comfortably past the version that
    added this (~0.5.0, December 2024). `ollama_system::send()`
    (`olla.h`/`.cpp`) gained an optional `response_format` parameter
    (default-empty JSON, so every existing call site is unaffected).
    `sidetrack.cpp`'s DONE-check call now passes a schema forcing
    `{"needs_correction": bool}`; stage 4 just reads that field directly
    (`json::parse` + `.at("needs_correction").get<bool>()`, wrapped in a
    try/catch defaulting to `true` - i.e. "needs a look" - on a parse
    failure, the safer direction given tonight's incident, still bounded
    by `SECOND_GUESS_MAX_CHAIN` regardless). All leading/trailing text-
    matching removed - nothing left to miss. Verified live: the model's
    raw reply is now literally `{"needs_correction":false}`, recognized
    correctly in a single cycle.
  - **Scope note**: the "needs_correction: true" (real-correction) path
    through stages 5/6 was not itself touched by this change and wasn't
    separately live-tested - confirmed correct by inspection, not by a
    forced repro.
  - **Same mechanism now available project-wide** for any place currently
    parsing a yes/no (or similar) answer out of free text - flagged as a
    likely fit for a future `.task`-script conditional-branching feature
    (`IDEAS.md`'s "Conditional branching" section, not started).
- **Built 2026-09-24: `COMMS::busy`, the first real piece of subcon's
  "how busy is the system" input** (see `IDEAS.md`'s "The subconscious"
  section) - a plain `int busy` member (`comms.h`) plus three free
  functions in `comms.cpp`: `comms_busy_inc()` (capped at 1024),
  `comms_busy_dec()`, and `comms_busy()` (currently `busy < 10` - true
  while quiet, false once enough recent activity has pushed the count up;
  deliberately not `!= 0`, since checking busy from inside an `exchange()`-
  style tick would otherwise make it nearly impossible to ever see a clean
  zero). Free functions rather than members - COMMS is more a plain data
  definition than a class with real behavior of its own (its own existing
  hand-written `operator=` is the same spirit). A fancier fully-self-
  contained design (a wrapper type auto-incrementing on field assignment,
  no external calls needed anywhere) was discussed and deliberately not
  built - real risk of silently breaking on `COMMS`'s own existing copy
  sites (`operator=`, `IO_WORKER_CLASS::thread_main()`'s per-channel
  relay) without careful handling; kept simple instead.
  - **Wiring so far, deliberately light**: `comms_busy_dec()` once per
    main-loop tick (`main.cpp`). `comms_busy_inc()` in every guarded,
    something-actually-happened branch of `IO_WORKER_CLASS::exchange()`
    (`io_worker.cpp`) - both directions (`INPUT_FROM_LLM`,
    `TOOL_ATTACHMENTS`, `INPUT_FROM_THINKING`, `INPUT_FROM_SYSTEM` out;
    `ENTER_PRESSED`, `INPUT_FROM_USER`, `INTERRUPTED`, `IS_TYPING`,
    `EXIT_REQUESTED` in) - skipping the two unconditional settings-copies
    (color/enable flags, not real events) and `close_chat_log_requested`
    (rare enough not worth it for a "light" first pass).
  - **A real, live-verified finding, not just theory**: watched real
    values in `debug_full_history.txt` during a live test - `busy` went
    0→2→1→0 right as a message was submitted (multiple different signals
    landing close together), then sat flat at 0 through the entire
    multi-second streamed response that followed. Root cause: right now
    `comms_busy_inc()` fires at most once per tick per field, and
    `comms_busy_dec()` fires once per tick unconditionally - during
    steady single-chunk-per-tick streaming, the same tick's own +1 and -1
    cancel out exactly, invisible to change-only logging. Not a bug (the
    code does exactly what's written), but a real gap between what's
    built and "reflects ongoing activity, not just bursts" - open
    question, not yet resolved, revisit once there's an actual reader for
    `is_busy()`/`comms_busy()` that cares about the distinction.
- **Found and fixed 2026-09-24: a real, live SIGABRT in
  `SUBCON_WORKER_CLASS::thread_main()`** (`ollama_system::~ollama_system()`
  per `addr2line` on the crash address) - caught live during the same
  session that built the busy-tracking work above, on a `claude`-profile
  test instance.
  - **Root cause**: `subcon_worker.cpp`'s own test-prompt response check
    read and cleared `subcon_llm.last_received.response` gated only on
    `last_received.complete`, missing a `!subcon_llm.is_processing` guard.
    `send()` (`olla.cpp`) sets `last_received.complete = true` near its
    own tail end, but that runs on the `chat_thread` it spawns -
    `is_processing` only flips false slightly later, once that thread's
    own lambda finishes its next line. Reading/clearing a `std::string`
    in that narrow window is an unsynchronized race against whatever
    `chat_thread` is still doing - no happens-before relationship, real
    undefined behavior, not just a stale read. Exact same missing-guard
    shape as `sidetrack.cpp`'s own action-capture race, already found and
    fixed the day before (2026-09-23 entry, above) - same lesson, not
    applied when this new code was written.
  - **Diagnosis path**: `addr2line` on the crash address (from
    `crash_log.txt`'s own backtrace, the crash-handler thread-race fix
    from 2026-09-23 doing its job) pointed at `ollama_system`'s implicit
    destructor - consistent with corrupted state from the race finally
    surfacing wherever something next got destroyed, not necessarily
    where the actual corruption happened. A live repro attempt under gdb
    (`gdb -p` attach is blocked in this sandbox; running fresh under gdb
    directly, bypassing the fork/exec supervisor wrapper via
    `./olli --supervised-child claude` as the direct target, does work)
    didn't reproduce the race within several minutes of waiting - gdb's
    own overhead very plausibly shifts the timing enough to avoid it.
    Root cause was pinned down by code reading against the already-
    documented sidetrack precedent, not by a forced live repro.
  - **Fix**: added the missing `!subcon_llm.is_processing` guard,
    matching `sidetrack.cpp`'s own established pattern exactly. Verified
    live afterward (one-shot test prompt fired and logged correctly, a
    real chat message was also exercised, clean shutdown, no new crash
    entries) - but honestly caveated: since this is a timing-dependent
    race that never reliably reproduced under controlled testing either,
    a clean test run doesn't *prove* it's gone the way a deterministic
    bug's fix could be proven. Confidence comes from the diagnosis
    matching an already-confirmed bug class in this same codebase, not
    from forcing the original failure and watching it not happen.
- **Built 2026-09-24/25: `COMMS_STRING` - `COMMS`'s 4 text fields
  (`INPUT_FROM_LLM`/`INPUT_FROM_THINKING`/`INPUT_FROM_SYSTEM`/
  `INPUT_FROM_USER`) are now a real, encapsulated type instead of plain
  `std::string`, with self-contained busy tracking built in.** A genuine
  rewrite, not an addition - the flat `int busy` + free-function design
  from the previous entry is fully retired.
  - **Design journey**: started from "make the 4 fields private, add
    `add_to_X()`/`drain_X()` accessor methods" - surveyed every real call
    site first (~50 across 8 files) and found the actual access patterns
    needed 5 shapes, not 2 (`add_to`/`set`/`peek`/`clear`/`drain`).
    Considered and rejected a generic free-function alternative
    (`add_to(comms_buffer.X, drain(comms.X))`) - it can't cleanly reach
    `busy`, which lives on the owning object, not the string, so it would
    have needed the fields to stay public anyway. Landed on the user's own
    proposed design instead: wrap each field in its own small class
    (`COMMS_STRING`) owning both its data AND its own busy counter
    together - no back-pointer to any parent `COMMS` needed at all, which
    is what makes this safe where an earlier auto-tracking wrapper idea
    (rejected the same session, see previous entry) wasn't: an ordinary
    copy of a `COMMS_STRING` just correctly copies both its string and its
    own count, no special-casing needed on any of `COMMS`'s existing copy
    sites (`operator=`, `IO_WORKER_CLASS::thread_main()`'s per-channel
    relay).
  - **The sweep**: converted every real call site in `olla.cpp`,
    `io_worker.cpp`, `user_io.cpp`, `tools.cpp`, `tools_task_script.cpp`,
    `sidetrack.cpp`, `subcon_worker.cpp`, `main.cpp` - couldn't be
    separated from adding the class itself, since the new type has no
    implicit `std::string` conversion, so nothing compiles until every
    site is converted together. Caught one real behavior bug of the sweep
    itself before it was ever tested: a `.set(source.drain())` conversion
    that would have unconditionally overwritten unconsumed input with an
    empty string whenever the source happened to be empty that tick -
    `set()` replaces rather than appends, so (unlike `add_to()`) it isn't
    a safe no-op on empty input; restored the original `!empty()` guard
    around that one call site. Clean build with `-Werror` on the first
    attempt after the full sweep - a strong signal every site was
    converted correctly, since any missed one would have failed to
    compile.
  - **Deliberately dropped, per explicit direction**: the 5 non-string
    signals the old flat counter also tracked (`TOOL_ATTACHMENTS`,
    `ENTER_PRESSED`, `INTERRUPTED`, `IS_TYPING`, `EXIT_REQUESTED`) don't
    fit `COMMS_STRING` and aren't tracked by anything else now -
    `COMMS::busy_count()` is just the sum of the 4 text fields' own
    counts. Explicitly not a mistake: "the entire idea is up in the air...
    lets just do what's simple and right for now."
  - **A real, live-verified finding, twice over**: first pass (`+1` per
    event, matching the old design's own magnitude) showed the SAME
    fields watched via a live test *never* moved at all, even across a
    full conversation with tool calls, streaming, and second-guess all
    firing - worse than the previous (flat-counter) version's own already-
    known gap. Root cause, found by re-reading `exchange()`: each field's
    own counter can only be incremented once per main-loop tick, and
    `COMMS::busy_count_dec()` also fires once per tick in that same
    iteration - so a lone `+1` is *always* cancelled by that same tick's
    own `-1` before anything can ever read it, deterministically, not by
    bad luck. Separately, `INPUT_FROM_USER`'s own counter (as read on the
    real `comms`) specifically never moved at all under any circumstance,
    because `exchange()`'s input-direction block drains `comms_buffer`'s
    own copy, not `comms`'s - the real `comms.INPUT_FROM_USER` only ever
    receives via `set()`, which didn't touch `busy_counter` at the time.
  - **Fix, all user-directed**: `busy_count_inc()`'s own step raised from
    `+1` to `+10` (still capped at 1024) - leaves a real `+9` residue
    surviving the same tick's `-1`, decaying over the next several ticks
    instead of vanishing instantly. `add_to()`/`set()`/`clear()` now also
    count as activity, not just `drain()` - `set()` is what
    `INPUT_FROM_USER`'s real content actually arrives through, closing
    that gap directly. One correctness nuance handled deliberately:
    `add_to()`/`clear()` safely guard the underlying operation itself on
    `!empty()` (appending/clearing nothing is already a no-op either way),
    but `set()`'s assignment is unconditional - only the busy-count
    increment is conditional - since `set("")` has to actually clear the
    field, not silently leave stale data in place.
  - **Re-verified live after the fix**: real, clean, meaningful numbers
    this time - `USER` climbed to 9 then drained to 0, then a second burst
    to 28 draining cleanly afterward; `LLM` climbed past 550 during a
    streamed response and drained afterward; `THINKING` climbed all the
    way to 1023 (effectively the cap) during second-guess's own thinking
    stream (which deliberately reuses the *same* `comms` as the main chat,
    a documented existing tradeoff, `sidetrack.cpp`) before draining back
    down cleanly once that stream stopped.
  - **Side effect worth remembering later, not fixed now**: with `+10`
    instead of `+1`, hitting the 1024 cap during any genuinely fast/
    sustained burst is now the normal case, not an edge case - and a full
    drain from the cap still takes ~20s (1024 ticks x 20ms) regardless of
    increment size, so "pinned at max for several seconds" is expected
    behavior now, not a bug.
  - **Not separately live-tested**: the web/voice channels
    (`display_with_web()`/`display_with_tts()`, the keyboard/voice/web
    multi-channel input paths in `IO_WORKER_CLASS::thread_main()`) got the
    same mechanical conversions as everything else, verified by clean
    compile and matching each site's original semantics exactly, but
    weren't separately exercised live (would need a browser session and
    mic input).
- **Fixed 2026-09-25: `refresh_lights()`'s error-detection gap, flagged but
  not fixed in the 2026-09-24 hue timeout entry.** `HUE_LIGHT_CLASS::
  refresh_lights()` (`tools/hue/hue.cpp`) only explicitly checked the
  array-shaped bridge error (`response_is_error()`'s own already-correct
  pattern, right above it, checks both shapes) - `make_request()` itself
  returns the *object*-shaped kind on a real curl failure
  (`{"error": "CURL failed: ..."}`). That case fell through the explicit
  check, hit the light-parsing loop, threw a `json::type_error` trying to
  treat the error string as a light object, and got caught by the blanket
  `catch (...) { return false; }` - correct result, wrong path. Added the
  missing `data.is_object() && data.contains("error")` check, mirroring
  `response_is_error()` exactly. Verified with a small isolated test (the
  exact JSON logic, no real bridge involved) against all three real
  shapes - object-error, array-error, and real light data - each behaving
  correctly. Restarted on the live `ron` profile and confirmed working by
  the user.
- **Built 2026-09-25: `SUBCON_WORKER_CLASS::exchange()` - subcon can now
  see the real chat's own `COMMS` (specifically `COMMS::busy_count()`,
  same-day entry above) from its own thread, safely.** The next real step
  once `busy_count()` had something genuine to show - wiring it so subcon
  can actually use it, not just watch it in isolation.
  - **Modeled directly on `IO_WORKER_CLASS::exchange()`** (`io_worker.h`/
    `.cpp`) at the user's own explicit direction ("we can hand over a
    copy of comms via exchange. just like we do in io worker") - same
    `INTERUPTED`/`PROCESSING` atomic-bool handshake: `exchange()` (main
    thread) sets `INTERUPTED`, waits for `PROCESSING` to clear (confirming
    `thread_main()` isn't mid-tick), only then safely copies into
    `comms_buffer`; `thread_main()`'s own `while(RUN)` loop now sits out a
    tick entirely if `INTERUPTED` is set, otherwise wraps its whole tick in
    `PROCESSING.store(true)`/`store(false)`. One-way only (`comms_buffer =
    comms;`) - subcon has nothing to relay back into the real conversation
    yet, so it's simpler than `IO_WORKER_CLASS`'s own two-direction
    version, but the same real synchronization requirement applies
    regardless of direction.
  - **A deliberate change from the original skeleton's own stated plan**:
    `subcon_worker.h`'s class comment used to explicitly argue against this
    shape ("IO_WORKER_CLASS's single combined exchange() exists because
    keyboard/audio genuinely need a full COMMS snapshot... this worker
    isn't going to need that shape") - updated now that subcon actually has
    a first real reason to need one.
  - **`main.cpp`** calls `subcon_worker.exchange(comms);` once per tick,
    right alongside the existing `io_worker.exchange(comms, &tool_worker);`.
  - **Verified live, not just compiled**: a temporary log inside
    `thread_main()` (removed before pushing) confirmed `comms_buffer.
    busy_count()` - subcon's own private copy, read from its own thread -
    genuinely tracked the real chat's own busy activity in real time
    (climbing to 9 during a real exchange, draining cleanly afterward),
    proving the cross-thread handoff actually works, not just that it
    compiles. Clean shutdown confirmed afterward too - no deadlock risk
    from the new synchronization.
  - **Not yet built (2026-09-25 entry below picks this up)**: anything
    that actually *uses* `comms_buffer.busy_count()` to make a decision -
    subcon still just ran its old one-shot test prompt on a fixed timer,
    unconditionally.
- **Built 2026-09-25: `TOOL_WORKER_CLASS::get_pending_result()`/
  `get_pending_event()` converted from a blocking `lock_guard` to a
  non-blocking `try_lock`-based `unique_lock`, plus `subcon_worker.h`'s
  own `comms_mutex` designed the same way from the start - both grew out
  of a live design conversation comparing `state_mutex` against `IO_WORKER_
  CLASS`'s own `INTERUPTED`/`PROCESSING` scheme.**
  - **The underlying question**: for any given `state_mutex`-guarded
    function, is it ever actually safe to skip the work entirely on lock
    contention, or does skipping it lose something real? Landed on a clear
    rule: safe only for operations that are already polled repeatedly with
    "nothing yet" as an expected, harmless outcome - `get_pending_result()`/
    `get_pending_event()` fit that exactly (called every tick until they
    succeed, and a contention-`false` is behaviorally identical to a
    genuine "not ready yet" `false`). `put_pending_call()`/`abandon_call()`/
    `set_identity()`/`add_manual_tool_def()` don't - each fires exactly
    once with nothing else ever giving it a second chance, so skipping any
    of them on contention would silently lose real state (a dropped tool
    call, a leaked pending result, a stale identity) - `lock_guard` (block
    until safe) stays correct for those.
  - **`get_pending_result()`/`get_pending_event()`**: both now use
    `std::unique_lock<std::mutex> lock(state_mutex, std::try_to_lock);
    if (!lock.owns_lock()) return false;` - still real RAII (auto-unlocks
    if it did acquire the lock), just non-blocking on the attempt itself.
    Verified live with a genuine remote-tool round trip (`set_timer`,
    the `clock` tool) - both the immediate `put_pending_call()`/
    `get_pending_result()` reply and the later on-expire `TOOL_EVENT`
    (`get_pending_event()`) worked correctly with real data, not just a
    clean compile.
  - **`subcon_worker`'s own `exchange()`/`comms_mutex` redesigned the same
    way, from a different starting point**: it had just been built
    (previous entry, same day) using `IO_WORKER_CLASS`'s own `INTERUPTED`/
    `PROCESSING` atomic-bool handshake, matching that class exactly. After
    the `state_mutex` comparison, replaced it with a real `std::mutex` -
    correct regardless of caller count (removes the same single-caller
    fragility `TOOL_WORKER_CLASS` had to fix once already, `2026-09-22`
    entry above) - with `exchange()` itself using `try_lock` (same
    reasoning as `get_pending_result()`: a missed snapshot copy is
    harmless, refreshed again next tick) so the *main* thread specifically
    never blocks waiting on subcon's own background thread.
    `thread_main()`'s own side keeps a plain blocking `lock_guard` - fine
    for subcon's own thread to wait briefly on its own mutex, since
    nothing else is waiting on it. Verified live again after the swap -
    same real, fluctuating `busy_count()` values coming through correctly,
    clean shutdown confirmed with the new mutex in place.
- **Built 2026-09-25: subcon's one-shot test prompt now actually gated on
  `comms_buffer.busy_count() < 10`, not just the 60s timer** - the first
  real use of the busy signal to make a decision, closing out the
  "not yet built" item from the entry above.
  - `!test_prompt_sent && test_prompt_timer.is_ready() && comms_buffer.
    busy_count() < 10` - the timer being ready is now necessary but not
    sufficient; it also has to be a genuinely quiet moment. `10` matches
    the threshold the retired `comms_busy()` free function used (same-day
    entry above), not a re-derived value.
  - **Verified live, both branches**: kept the real chat continuously busy
    (two back-to-back real messages) spanning well past the 60s mark -
    confirmed the prompt did *not* fire during that entire window, despite
    the timer being ready. Then let things go quiet - confirmed it fired
    correctly shortly after, once `busy_count()` actually dropped below
    10.
- **Simplified 2026-09-25: `subcon_worker`'s `exchange()` no longer copies
  the whole `COMMS` snapshot - just the one `int` subcon actually uses.**
  User-directed simplification, same day as the entries above: `comms_
  buffer` (the full `COMMS` snapshot member) is kept declared for later,
  but `exchange()` doesn't populate it anymore; a new plain `int
  main_busy_level` member gets `comms.busy_count()` copied into it instead.
  `thread_main()`'s own gate now reads `main_busy_level` directly instead
  of `comms_buffer.busy_count()`. Same `comms_mutex`/`try_lock` protection
  as before, just guarding a smaller, cheaper copy. Verified live again
  with real, correctly-tracking values, clean shutdown confirmed.
