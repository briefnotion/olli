// Phase 1 smoke test for rag_db - not a real program, just proves the
// schema/chunk/embed/search round-trip works end to end (real SQLite file,
// real Ollama call) before rag_admin (Phase 2) or rag_tool (Phase 3) get
// built on top of these same files.
//
// Run: `make test` in this directory - needs Ollama running locally with
// nomic-embed-text pulled (`ollama pull nomic-embed-text`).

#include "rag_chunk.hpp"
#include "rag_db.hpp"
#include "rag_embed.hpp"

#include <cstdio>
#include <iostream>

int main()
{
    const char* db_path = "/tmp/rag_db_test.sqlite3";
    std::remove(db_path);

    RAG_DB db(db_path);
    if (!db.is_open()) {
        std::cerr << "FAIL: could not open DB: " << db.last_error() << "\n";
        return 1;
    }
    std::cout << "OK: DB opened and schema created.\n";

    int collection_id = db.get_or_create_collection("notes", "Test notes collection");
    if (collection_id < 0) {
        std::cerr << "FAIL: could not create collection: " << db.last_error() << "\n";
        return 1;
    }
    std::cout << "OK: collection id " << collection_id << ".\n";

    std::string sample_text =
        "Olli is a personal AI assistant that runs locally via Ollama.\n\n"
        "The RAG system lets olli search through notes, conversations, and "
        "documentation to answer questions using stored knowledge instead of "
        "only what fits in its immediate context window.\n\n"
        "Chunks are embedded using a local embedding model, so nothing about "
        "this system depends on an external API.";

    int document_id = db.add_document(collection_id, "Test note about RAG", "manual entry");
    if (document_id < 0) {
        std::cerr << "FAIL: could not add document: " << db.last_error() << "\n";
        return 1;
    }
    std::cout << "OK: document id " << document_id << ".\n";

    std::vector<std::string> chunks = chunk_text(sample_text, 50, 10);
    std::cout << "OK: split into " << chunks.size() << " chunk(s).\n";

    RAG_EMBEDDER embedder;
    for (size_t i = 0; i < chunks.size(); i++) {
        std::vector<float> embedding = embedder.embed_document(chunks[i]);
        if (embedding.empty()) {
            std::cerr << "FAIL: embedding failed: " << embedder.last_error() << "\n";
            return 1;
        }
        if (!db.add_chunk(document_id, static_cast<int>(i), chunks[i], embedding)) {
            std::cerr << "FAIL: could not store chunk: " << db.last_error() << "\n";
            return 1;
        }
        std::cout << "OK: chunk " << i << " stored (" << embedding.size() << "-dim embedding).\n";
    }

    std::string query = "How does olli search stored knowledge?";
    std::vector<float> query_embedding = embedder.embed_query(query);
    if (query_embedding.empty()) {
        std::cerr << "FAIL: query embedding failed: " << embedder.last_error() << "\n";
        return 1;
    }

    std::vector<RAG_SEARCH_RESULT> results = db.search(query_embedding, 3);
    std::cout << "\nSearch results for: \"" << query << "\"\n";
    for (const auto& r : results) {
        std::cout << "  [score " << r.score << "] " << r.collection_name << " / " << r.document_title
                   << " (chunk " << r.chunk_index << "): " << r.chunk_text.substr(0, 70) << "...\n";
    }

    if (results.empty()) {
        std::cerr << "FAIL: search returned no results.\n";
        return 1;
    }

    std::cout << "\nPASS: rag_db round-trip works end to end.\n";
    return 0;
}
