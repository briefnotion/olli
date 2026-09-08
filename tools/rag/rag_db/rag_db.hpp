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

        // Documents - one importable unit (a note, a manual, a saved
        // conversation) within a collection. metadata_json is stored as-is,
        // free-form (e.g. a conversation's date/participants, a manual's
        // version) - this layer never inspects it.
        int add_document(int collection_id, const std::string& title, const std::string& source = "", const std::string& metadata_json = "{}");
        std::vector<RAG_DOCUMENT> list_documents(int collection_id) const;
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

    private:
        sqlite3* db = nullptr;
        mutable std::string error;
};
