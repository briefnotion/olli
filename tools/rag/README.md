# RAG system

Retrieval-augmented generation for olli - lets it search a local knowledge
base (notes, imported documentation, saved conversations, or anything else
you put in it) mid-conversation, instead of being limited to what fits in
its immediate context window. Three programs, one shared storage layer:

| Program | Role |
|---|---|
| [`rag_db/`](rag_db) | Shared library - SQLite storage, Ollama embedding client, text chunker. Not a standalone program itself; every binary below links it directly (like [`../olli_link/`](../olli_link) is shared by every other remote tool). |
| [`rag_admin/`](rag_admin) | Standalone, menu-driven maintenance program - create collections, sync their folders into the database, list/delete documents, test searches. The only thing that ever *writes* to the database. |
| [`rag_tool/`](rag_tool) | Remote tool ([`../PROTOCOL.md`](../PROTOCOL.md)) - registers `rag_list_collections`/`rag_search`/`rag_search_documents`/`rag_get_document` with a live olli. Read-only. |

## How it works

**Data model** (SQLite, one file): `collections` (e.g. `notes`, `manuals`,
`conversations` - whatever categories you create) hold `documents` (one
imported file each, storing the *complete original text* verbatim alongside
its source path and a content hash - see "Collection folders and syncing"
below) which hold `chunks` (one embedded slice of that document's text
each). Adding a new "type" of content is just creating a new collection
through `rag_admin` - no schema change, no code change.

**Name matching is deliberately lenient**: collection names (`WHERE
collection ...` on any of the four tools below) and document titles
(`rag_get_document`) both match case-insensitively, and a title also
tolerates a missing/wrong file extension ("misc_notes" matches a document
actually titled "misc_notes.txt"). Found via a real live test - a model
told a collection was named "Notes" kept calling back with "notes" on every
follow-up and failing every time on a plain exact match, even though the
collection was right there. A model recalling a name from earlier context
reliably normalizes it; the lookups need to tolerate that, not the other
way around.

**Embeddings**: Ollama's `/api/embeddings` endpoint, `nomic-embed-text` by
default (already pulled - 768-dim), called directly over libcurl (same
pattern [`../hue/hue.cpp`](../hue/hue.cpp) uses for the Hue bridge's REST
API) - not part of the olli<->tool wire protocol, a separate HTTP call this
code makes on its own. **Text going in for storage and text going out as a
query are embedded differently** - `RAG_EMBEDDER::embed_document()` /
`embed_query()` prefix the text with `"search_document: "`/`"search_query:
"` respectively before embedding, per nomic-embed-text's documented usage.
This isn't cosmetic: dropping the prefixes was tested and measurably hurt
ranking (a chunk containing a query's literal answer scored *below* an
unrelated intro paragraph without them). If this ever changes to a
different embedding model, check whether that model has its own prefix
convention before assuming a bare `embed()` is fine.

**Chunking** ([`rag_db/rag_chunk.hpp`](rag_db/rag_chunk.hpp)): paragraph-
aware, ~150 words per chunk with 20 words of overlap. Also not an arbitrary
choice - the original default (400/40) was tested against a real 340-line
technical document and routinely merged 2-3 unrelated subsections into one
chunk, diluting embeddings enough that the chunk containing a test query's
actual answer ranked 8th out of 9 instead of near the top. Smaller chunks
fixed it. If retrieval quality on some future document looks off, chunk
size is the first thing worth re-checking - see the git history around this
file for the specific before/after scores.

**Search**: brute-force cosine similarity over every chunk (optionally
scoped to one collection), fixed top-5 results, no ANN index. Fine at
personal-scale corpus sizes; revisit only if a real corpus makes this
measurably slow, not preemptively.

## Which database - the profile convention

Both `rag_admin` and `rag_tool` resolve a database path the same way
[`../presence/`](../presence)/[`../clock/`](../clock) resolve their own
per-profile settings - `~/olli_files_<name>/rag.db`, or the shared
`~/olli_files/rag.db` when no profile applies (`profile_db_path()` in
[`rag_db/rag_db.hpp`](rag_db/rag_db.hpp), the one place this mapping is
defined - both programs call it rather than each computing it their own
way).

- **`rag_admin`** takes the profile name directly on the command line -
  there's nothing else it could use, since it never talks to olli.
- **`rag_tool`** normally doesn't need telling - it starts on the shared
  default, then switches the moment olli's `identity` message (see
  [`../PROTOCOL.md`](../PROTOCOL.md)) reveals which profile is actually
  connected, and switches back to the shared default on disconnect (so a
  stale profile's database doesn't linger for whoever, or nothing, connects
  next). An explicit profile name on its command line overrides this
  permanently, ignoring whatever olli says - useful for testing against a
  specific database regardless of which profile happens to be running.

## Collection folders and syncing

Each collection owns a folder on disk -
`~/olli_files_<profile>/collection/<name>/` (`profile_collection_dir()` in
[`rag_db/rag_db.hpp`](rag_db/rag_db.hpp), same profile convention as above)
- created automatically by `rag_admin`'s "Create collection". There's no
"import an arbitrary path from anywhere on disk" option any more: drop
plain-text/markdown files straight into a collection's folder, then run
"Update database" to sync them in.

That sync is a real three-way diff, per collection, between what's in the
folder and what's in the database - matched by each document's stored
`source` path and a `content_hash` (`hash_content()` in `rag_db.hpp` - a
fast FNV-1a hash, not cryptographic, purely for "did this change"):

- File on disk, no matching document → **imported** (new).
- File on disk, matching document, same hash → **skipped** (unchanged -
  not re-embedded, that's the whole point of hashing it).
- File on disk, matching document, different hash → the old document (and
  its chunks) is **deleted and reimported** fresh.
- Document whose source file is no longer on disk → **deleted**.

So "Update database" is always safe to run repeatedly - a second run with
nothing changed does nothing. A "conversations" collection works the same
as any other: export/save the conversation you want searchable into its
folder, then sync.

## rag_admin - maintenance

```bash
cd rag_admin
make
./rag_admin [profile_name]   # e.g. "ron" for ~/olli_files_ron/rag.db
                              # omit for the shared ~/olli_files/rag.db
```

Menu:

```
1) List collections
2) Create collection
3) Edit collection description
4) Delete collection
5) Update database (sync collection folders)
6) List documents in a collection
7) Search (find a passage)
8) Search documents (survey a topic)
9) View a document's full content
0) Quit
```

"Delete collection" removes the collection (and every document/chunk in
it, via the same cascading delete a single document already used) from
the *database only* - it never touches the collection's folder or files.
Deleting one, then recreating it and running "Update database" again,
brings everything straight back from what's still on disk.

There's deliberately no standalone "delete a document" any more - if its
source file is still in the collection's folder, deleting just the
database row would only get it reimported on the next "Update database"
anyway (nothing on disk changed, so the sync sees it as new again). Delete
the file (or the whole collection) instead.

Options 7-9 mirror `rag_tool`'s three read tools exactly (same
`search()`/`search_documents()` calls, same fixed top-5/top-10), so you
can preview what olli would actually get back without needing a live
connection at all.

No chat_log-aware importer, no filtering by file type (anything readable as
plain text in a collection's folder gets imported - dropping something
that isn't plain text in there is on you for now).

## rag_tool - talking to it through olli

```bash
cd rag_tool
make
./rag_tool [host] [profile_name]
# host defaults to 127.0.0.1 (olli on this machine)
# profile_name, if given, pins the database and ignores olli's identity -
# see "Which database" above. Normally left unset.
```

Same reconnect/heartbeat/registration plumbing as every other remote tool
(see [`../PROTOCOL.md`](../PROTOCOL.md)) - it's built from
[`../template/template_tool.cpp`](../template/template_tool.cpp). Because
it's a compiled binary, **restart it after pulling in any rag_db/rag_embed/
rag_chunk change** - the database itself is re-read fresh on every query
and correctly follows an identity switch without a restart, but *code*
changes need a rebuild + restart to take effect in an already-running
process.

| Tool | Does |
|---|---|
| `rag_list_collections` | Lists every collection with its description - lets the model discover what's actually in the database before guessing a name. |
| `rag_search` | `{query, collection?}` - embeds the query, searches (optionally scoped to one named collection), returns the top 5 *chunks* with their source document/collection - "find the specific passage." Several results can come from the same document. |
| `rag_search_documents` | `{query, collection?}` - same search, but deduplicated to one result per *document* (its single best-scoring chunk) instead of up to 5 chunks that might pile up from the same one - "what do I have on this topic," a broader survey (top 10) across documents rather than passages within them. |
| `rag_get_document` | `{title, collection}` (both required) - fetches one document's complete original content verbatim, once something's been identified by title/collection from either search above. Neither search tool returns full documents on its own; this is the only way to get one. |

None of the four take a model-settable result count - fixed at 5/10 to
keep each tool's surface simple; revisit if that ever turns out to matter.

`rag_search`/`rag_search_documents`'s descriptions explicitly call out
that they also cover help/documentation about olli's own tools (if a
`help`-style collection has been imported), and that a request for help
on a tool by name shouldn't be treated as a request to actually call
that tool - added after a real live test asked for "help on the clock
tool" and got the current time back, because nothing hinted the model
toward searching instead of just pattern-matching "clock" to the clock
tool. Worth knowing this is a nudge in the tool description, not a
guarantee - the model is still making a judgment call between two
plausible readings each time.

This four-tool split (list → search-for-a-passage → search-for-documents →
fetch-one-in-full) came out of watching a real "I know it's in my notes
somewhere but I don't remember which file" session live - see git history
around this README for that conversation if the reasoning ever needs
revisiting.

## Not built yet (deliberately out of scope so far)

- No importer that understands olli's own `chat_log` file format - the
  "conversations" collection is manual-export-then-import, same as
  everything else.
- No auto-import - olli doesn't push its own conversations into the RAG
  store as it goes. Would mean touching olli's own source, not just this
  directory.
- No in-app document editing - edit the file in its collection folder and
  run "Update database" instead; that's a real edit+resync, not a manual
  delete-and-reimport.
- No search pagination/exclusion ("show me the next 5, not these") -
  deliberately skipped in favor of `rag_search_documents`, which surveys
  everything relevant in one call instead of needing repeated re-searches.
  Revisit only if a real corpus grows past what one `rag_search_documents`
  call can usefully list.
