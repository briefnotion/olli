// RAG_DB - the shared SQLite storage layer for olli's RAG system: schema
// (collections/documents/chunks), CRUD on all three, and brute-force cosine
// search over stored embeddings.
//
// Deliberately storage-only - knows nothing about Ollama, chunking, or the
// olli<->tool wire protocol. See rag_embed.hpp for the embedding client and
// rag_chunk.hpp for the text splitter; rag_admin and rag_tool each link all
// three and add their own logic on top (menu-driven maintenance vs. an
// olli_link-based remote tool, respectively).
//
// One RAG_DB per process, holding one open sqlite3 connection for its
// lifetime - not shared/passed across threads.

#pragma once

#include <optional>
#include <string>
#include <vector>

struct sqlite3;

// Maps an olli profile name to the RAG database path under that profile's
// own settings directory - the same convention `presence`/`clock` already
// use for their own per-profile state: `~/olli_files_<name>/rag.db`, or the
// shared `~/olli_files/rag.db` when profile_name is empty (no profile
// specified, matching olli's own `./olli` with no `[name]` argument).
// Doesn't create the directory - every profile directory this could resolve
// to is expected to already exist (created by olli itself the first time
// that profile is used), and RAG_DB's own constructor already creates the
// database *file* (and schema) inside it if that part is missing.
std::string profile_db_path(const std::string& profile_name);

// Same convention, for where a collection's *source files* live on disk:
// `~/olli_files_<profile_name>/collection/<collection_name>/` (or
// `~/olli_files/collection/<collection_name>/` when profile_name is empty).
// Doesn't create the directory itself - rag_admin's do_create_collection()
// does that, since creating it is a deliberate user action, not a side
// effect of merely computing where it would go.
std::string profile_collection_dir(const std::string& profile_name, const std::string& collection_name);

// A fast, non-cryptographic content hash (FNV-1a, 64-bit, hex string) -
// purely for "has this file changed since it was imported", not security.
// Used to give a re-run of rag_admin's "Update database" a cheap way to
// tell an unchanged file (skip it) from a genuinely edited one (delete the
// old document and reimport) without re-embedding everything every time.
std::string hash_content(const std::string& content);

struct RAG_COLLECTION {
    int id = -1;
    std::string name;
    std::string description;
};

struct RAG_DOCUMENT {
    int id = -1;
    int collection_id = -1;
    std::string title;
    std::string source;
    std::string content; // the full original imported text, verbatim - see add_document()
    std::string content_hash; // hash_content() of content at import time
    std::string metadata; // caller-defined JSON string, not parsed here
    std::string imported_at;
};

struct RAG_SEARCH_RESULT {
    int document_id = -1;
    std::string document_title;
    std::string collection_name;
    int chunk_index = -1;
    std::string chunk_text;
    float score = -1.0f; // cosine similarity, -1..1, higher is better
};

class RAG_DB {
    public:
        // Opens db_path, creating it (and the schema, via CREATE TABLE IF
        // NOT EXISTS) if it doesn't exist yet. Check is_open() before doing
        // anything else - a failure here (bad path, disk full, corrupt file)
        // leaves every other method a safe no-op that returns a sentinel
        // failure value.
        explicit RAG_DB(const std::string& db_path);
        ~RAG_DB();

        RAG_DB(const RAG_DB&) = delete;
        RAG_DB& operator=(const RAG_DB&) = delete;

        bool is_open() const { return db != nullptr; }
        // What went wrong on the most recent failing call - undefined
        // (possibly stale) after a call that succeeded.
        std::string last_error() const { return error; }

        // Collections - the "different types of content" mechanism (e.g.
        // "conversations", "notes", "manuals"). Idempotent: an existing name
        // is returned as-is rather than erroring, so callers can freely
        // call this every time without checking first.
        int get_or_create_collection(const std::string& name, const std::string& description = "");
        std::optional<RAG_COLLECTION> find_collection(const std::string& name) const;
        std::vector<RAG_COLLECTION> list_collections() const;
        bool update_collection_description(int collection_id, const std::string& description);
        // Cascades to every document (and, transitively, every chunk) in
        // this collection (ON DELETE CASCADE) - deliberately DB-only, never
        // touches the collection's folder or files on disk. rag_admin's
        // do_delete_collection() is the one place that decides when this
        // is appropriate to call.
        bool delete_collection(int collection_id);

        // Documents - one importable unit (a note, a manual, a saved
        // conversation) within a collection. content is stored verbatim
        // (not reassembled from chunks later - chunk_text()'s overlap would
        // make that lossy/duplicated) so rag_get_document can hand back
        // the exact original text. metadata_json is stored as-is, free-form
        // (e.g. a conversation's date/participants, a manual's version) -
        // this layer never inspects it.
        int add_document(int collection_id, const std::string& title, const std::string& source, const std::string& content, const std::string& content_hash, const std::string& metadata_json = "{}");
        std::vector<RAG_DOCUMENT> list_documents(int collection_id) const;
        // For the "is this file already imported, and if so has it
        // changed" check a sync (rag_admin's "Update database") needs -
        // source is matched exactly, within one collection.
        std::optional<RAG_DOCUMENT> find_document_by_source(int collection_id, const std::string& source) const;
        // For rag_tool's rag_get_document - title is matched exactly,
        // within one collection (titles aren't required to be unique
        // database-wide, only the source-path uniqueness a sync relies on).
        std::optional<RAG_DOCUMENT> find_document_by_title(int collection_id, const std::string& title) const;
        // Cascades to that document's chunks (ON DELETE CASCADE).
        bool delete_document(int document_id);

        // Chunks - one embedded slice of a document's text, as produced by
        // rag_chunk.hpp's chunk_text() + rag_embed.hpp's RAG_EMBEDDER.
        bool add_chunk(int document_id, int chunk_index, const std::string& chunk_text, const std::vector<float>& embedding);

        // Brute-force cosine similarity over every stored chunk (or just
        // those in one collection, if given) - fine at personal-scale
        // corpus sizes; revisit (e.g. an ANN index) only if a real corpus
        // ever makes this too slow, not preemptively.
        std::vector<RAG_SEARCH_RESULT> search(const std::vector<float>& query_embedding, int top_k, std::optional<int> collection_id = std::nullopt) const;

        // Same ranking as search(), but deduplicated to one result per
        // document (its single best-scoring chunk) instead of up to top_k
        // chunks that might pile up from the same document - for "what do
        // I have on X" browsing across documents rather than "find the
        // specific passage" within them. top_k here counts documents.
        std::vector<RAG_SEARCH_RESULT> search_documents(const std::vector<float>& query_embedding, int top_k, std::optional<int> collection_id = std::nullopt) const;

    private:
        sqlite3* db = nullptr;
        mutable std::string error;

        // Scores every chunk (optionally scoped to one collection) against
        // query_embedding - unsorted, untrimmed. Shared by search() and
        // search_documents() so both stay consistent with exactly the same
        // scoring, differing only in how the raw results get reduced
        // afterward.
        std::vector<RAG_SEARCH_RESULT> score_all_chunks(const std::vector<float>& query_embedding, std::optional<int> collection_id) const;
};
