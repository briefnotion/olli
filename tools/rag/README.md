# RAG system

Retrieval-augmented generation for olli - lets it search a local knowledge
base (notes, imported documentation, saved conversations, or anything else
you put in it) mid-conversation, instead of being limited to what fits in
its immediate context window. Three programs, one shared storage layer:

| Program | Role |
|---|---|
| [`rag_db/`](rag_db) | Shared library - SQLite storage, Ollama embedding client, text chunker. Not a standalone program itself; every binary below links it directly (like [`../olli_link/`](../olli_link) is shared by every other remote tool). |
| [`rag_admin/`](rag_admin) | Standalone, menu-driven maintenance program - create collections, import files, list/delete documents, test searches. The only thing that ever *writes* to the database. |
| [`rag_tool/`](rag_tool) | Remote tool ([`../PROTOCOL.md`](../PROTOCOL.md)) - registers `rag_list_collections`/`rag_search` with a live olli. Read-only. |

## How it works

**Data model** (SQLite, one file): `collections` (e.g. `notes`, `manuals`,
`conversations` - whatever categories you create) hold `documents` (one
imported file each) which hold `chunks` (one embedded slice of that
document's text each). Adding a new "type" of content is just creating a
new collection through `rag_admin` - no schema change, no code change.

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

## rag_admin - maintenance

```bash
cd rag_admin
make
./rag_admin [db_path]   # db_path defaults to ./rag.db - see the warning below
```

**Always pass `db_path` explicitly** unless you deliberately want a
throwaway database in the current directory - it's an easy mistake to make
(happened during initial testing) to run this from an arbitrary directory
with no argument, silently write into a `./rag.db` nobody else reads, and
then wonder why `rag_tool` can't find what you just imported. The two
programs don't automatically agree on a path; you have to point them at the
same file yourself.

Menu: list/create collections, import a file (prompts for which collection
- pick a number or type a new name to create one on the spot - and a title,
defaulting to the filename), list/delete documents, and a search option to
sanity-check retrieval without needing olli at all.

Import is file-based only for now - point it at a plain-text/markdown file.
No paste-text-in-the-menu option, no chat_log-aware importer. A
"conversations" collection works the same as any other: export/save the
conversation to a text file first, then import it like a note.

## rag_tool - talking to it through olli

```bash
cd rag_tool
make
./rag_tool [host] [db_path]
# host defaults to 127.0.0.1 (olli on this machine)
# db_path defaults to ~/olli_files_claude/rag.db
```

Same reconnect/heartbeat/registration plumbing as every other remote tool
(see [`../PROTOCOL.md`](../PROTOCOL.md)) - it's built from
[`../template/template_tool.cpp`](../template/template_tool.cpp). Because
it's a compiled binary, **restart it after pulling in any rag_db/rag_embed/
rag_chunk change** - the database itself is re-read fresh on every query
(no restart needed for new imports), but code changes need a rebuild +
restart to take effect in an already-running process.

| Tool | Does |
|---|---|
| `rag_list_collections` | Lists every collection with its description - lets the model discover what's actually in the database before guessing a name. |
| `rag_search` | `{query, collection?}` - embeds the query, searches (optionally scoped to one named collection), returns the top 5 chunks with their source document/collection. No model-settable result count - fixed at 5 to keep the tool's surface simple; revisit if that ever turns out to matter. |

## Not built yet (deliberately out of scope so far)

- No importer that understands olli's own `chat_log` file format - the
  "conversations" collection is manual-export-then-import, same as
  everything else.
- No auto-import - olli doesn't push its own conversations into the RAG
  store as it goes. Would mean touching olli's own source, not just this
  directory.
- No per-profile identity-driven database switching - `rag_tool`'s db path
  is a fixed default/CLI argument, not derived from the `identity` message
  every other stateful remote tool convention (see
  [`../clock/clock.cpp`](../clock/clock.cpp)'s `handle_identity()`) would
  suggest. Worth revisiting if this is ever used by more than one profile.
- No document editing - delete and re-import instead.
