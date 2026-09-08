// rag_admin - standalone, menu-driven maintenance program for olli's RAG
// database: create collections, import files as documents, list/delete
// documents, and run ad-hoc searches to sanity-check retrieval quality.
// Talks only to rag_db.hpp/rag_embed.hpp/rag_chunk.hpp in ../rag_db/ -
// never to olli itself over the wire protocol (that's rag_tool's job, see
// ../../PROTOCOL.md - a later phase, not this program).
//
// Build: `make` in this directory (needs libsqlite3-dev and libcurl, same
// as ../rag_db/'s smoke test). Run: `./rag_admin [db_path]` - defaults to
// ./rag.db in the current directory if no path is given, created if it
// doesn't exist yet. Needs Ollama running locally with the embedding model
// pulled (see ../rag_db/rag_embed.hpp for the default).
//
// Import is file-based only for now: point it at a plain-text/markdown file
// and it reads, chunks, embeds, and stores the whole thing as one document.
// A "conversations" collection works the same way - save/export the
// conversation you want searchable to a text file first, then import it
// like any other document. No chat_log-specific importer or olli-side
// auto-import exists yet; both were explicitly left out of this phase.

#include "rag_chunk.hpp"
#include "rag_db.hpp"
#include "rag_embed.hpp"

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {
    std::string trim(const std::string& s)
    {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string prompt_line(const std::string& prompt)
    {
        std::cout << prompt;
        std::string line;
        std::getline(std::cin, line);
        return trim(line);
    }

    // Sentinel -1 on anything that isn't a plain integer - every caller
    // here treats that as "cancel", and no valid id/menu-index is ever 0
    // or negative (SQLite AUTOINCREMENT starts at 1, menu numbering starts
    // at 1).
    int prompt_int(const std::string& prompt)
    {
        std::string line = prompt_line(prompt);
        try {
            size_t pos = 0;
            int value = std::stoi(line, &pos);
            return pos == line.size() ? value : -1;
        } catch (const std::exception&) {
            return -1;
        }
    }

    std::string basename_of(const std::string& path)
    {
        size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    // Prints every collection numbered 1..N (display index, not the DB id)
    // and returns them in that same order, so callers can map a user's
    // menu pick back to the real RAG_COLLECTION without assuming ids are
    // contiguous.
    std::vector<RAG_COLLECTION> print_numbered_collections(const RAG_DB& db)
    {
        std::vector<RAG_COLLECTION> collections = db.list_collections();
        if (collections.empty()) {
            std::cout << "(no collections yet)\n";
            return collections;
        }
        for (size_t i = 0; i < collections.size(); i++) {
            std::cout << "  " << (i + 1) << ") " << collections[i].name;
            if (!collections[i].description.empty()) std::cout << " - " << collections[i].description;
            std::cout << "\n";
        }
        return collections;
    }

    // Prompts for a collection by number from the printed list above, or
    // lets the user type a brand new name to create one on the spot.
    // Returns -1 if the user backs out (blank input).
    int choose_or_create_collection(RAG_DB& db)
    {
        std::cout << "\nCollections:\n";
        std::vector<RAG_COLLECTION> collections = print_numbered_collections(db);

        std::string choice = prompt_line("\nPick a number, or type a new collection name (blank to cancel): ");
        if (choice.empty()) return -1;

        try {
            size_t pos = 0;
            int index = std::stoi(choice, &pos);
            if (pos == choice.size() && index >= 1 && index <= static_cast<int>(collections.size())) {
                return collections[static_cast<size_t>(index) - 1].id;
            }
        } catch (const std::exception&) {
            // Not a number - fall through and treat it as a new name.
        }

        std::string description = prompt_line("Description for new collection \"" + choice + "\": ");
        int id = db.get_or_create_collection(choice, description);
        if (id < 0) std::cout << "Failed to create collection: " << db.last_error() << "\n";
        return id;
    }

    void do_list_collections(const RAG_DB& db)
    {
        std::cout << "\n--- Collections ---\n";
        print_numbered_collections(db);
    }

    void do_create_collection(RAG_DB& db)
    {
        std::string name = prompt_line("\nNew collection name: ");
        if (name.empty()) { std::cout << "Cancelled.\n"; return; }

        std::string description = prompt_line("Description: ");
        int id = db.get_or_create_collection(name, description);
        if (id < 0) std::cout << "Failed: " << db.last_error() << "\n";
        else std::cout << "Created collection \"" << name << "\" (id " << id << ").\n";
    }

    void do_import_file(RAG_DB& db, RAG_EMBEDDER& embedder)
    {
        std::string path = prompt_line("\nFile path to import: ");
        if (path.empty()) { std::cout << "Cancelled.\n"; return; }

        std::ifstream file(path);
        if (!file) { std::cout << "Could not open \"" << path << "\".\n"; return; }
        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string content = buffer.str();
        if (trim(content).empty()) { std::cout << "File is empty - nothing to import.\n"; return; }

        int collection_id = choose_or_create_collection(db);
        if (collection_id < 0) { std::cout << "Cancelled.\n"; return; }

        std::string default_title = basename_of(path);
        std::string title = prompt_line("Title [" + default_title + "]: ");
        if (title.empty()) title = default_title;

        int document_id = db.add_document(collection_id, title, path);
        if (document_id < 0) { std::cout << "Failed to create document: " << db.last_error() << "\n"; return; }

        std::vector<std::string> chunks = chunk_text(content);
        std::cout << "Embedding " << chunks.size() << " chunk(s)...\n";

        for (size_t i = 0; i < chunks.size(); i++) {
            std::vector<float> embedding = embedder.embed_document(chunks[i]);
            if (embedding.empty()) {
                std::cout << "Embedding failed on chunk " << i << ": " << embedder.last_error()
                           << " - stopping. Document \"" << title << "\" (id " << document_id
                           << ") is only partially imported - delete it and retry once fixed.\n";
                return;
            }
            if (!db.add_chunk(document_id, static_cast<int>(i), chunks[i], embedding)) {
                std::cout << "Failed to store chunk " << i << ": " << db.last_error() << "\n";
                return;
            }
            std::cout << "  chunk " << (i + 1) << "/" << chunks.size() << " stored\n";
        }

        std::cout << "Imported \"" << title << "\" as document id " << document_id
                   << " (" << chunks.size() << " chunk(s)).\n";
    }

    void do_list_documents(const RAG_DB& db)
    {
        std::cout << "\nCollections:\n";
        std::vector<RAG_COLLECTION> collections = print_numbered_collections(db);
        if (collections.empty()) return;

        int index = prompt_int("\nWhich collection number? ");
        if (index < 1 || index > static_cast<int>(collections.size())) { std::cout << "Cancelled.\n"; return; }

        const RAG_COLLECTION& collection = collections[static_cast<size_t>(index) - 1];
        std::vector<RAG_DOCUMENT> documents = db.list_documents(collection.id);
        if (documents.empty()) { std::cout << "(no documents in this collection)\n"; return; }

        std::cout << "\n--- Documents in \"" << collection.name << "\" ---\n";
        for (const auto& d : documents) {
            std::cout << "  id " << d.id << ": " << d.title << " (source: " << d.source
                       << ", imported " << d.imported_at << ")\n";
        }
    }

    void do_delete_document(RAG_DB& db)
    {
        int document_id = prompt_int("\nDocument id to delete: ");
        if (document_id < 0) { std::cout << "Cancelled.\n"; return; }

        std::string confirm = prompt_line("Delete document id " + std::to_string(document_id) + " and all its chunks? [y/N]: ");
        if (confirm != "y" && confirm != "Y") { std::cout << "Cancelled.\n"; return; }

        if (db.delete_document(document_id)) std::cout << "Deleted.\n";
        else std::cout << "Failed: " << db.last_error() << "\n";
    }

    void do_search(RAG_DB& db, RAG_EMBEDDER& embedder)
    {
        std::string query = prompt_line("\nSearch query: ");
        if (query.empty()) { std::cout << "Cancelled.\n"; return; }

        std::cout << "Restrict to one collection?\n";
        std::vector<RAG_COLLECTION> collections = print_numbered_collections(db);
        std::string choice = prompt_line("Collection number (blank for all): ");

        std::optional<int> collection_id;
        if (!choice.empty()) {
            try {
                size_t pos = 0;
                int index = std::stoi(choice, &pos);
                if (pos == choice.size() && index >= 1 && index <= static_cast<int>(collections.size())) {
                    collection_id = collections[static_cast<size_t>(index) - 1].id;
                }
            } catch (const std::exception&) {
                // Not a number - treat as "all collections".
            }
        }

        std::vector<float> query_embedding = embedder.embed_query(query);
        if (query_embedding.empty()) { std::cout << "Embedding failed: " << embedder.last_error() << "\n"; return; }

        std::vector<RAG_SEARCH_RESULT> results = db.search(query_embedding, 5, collection_id);
        if (results.empty()) { std::cout << "No results.\n"; return; }

        std::cout << "\n--- Results ---\n";
        for (const auto& r : results) {
            std::string snippet = r.chunk_text.substr(0, 200);
            std::cout << "[" << r.score << "] " << r.collection_name << " / " << r.document_title
                       << " (chunk " << r.chunk_index << "):\n    " << snippet
                       << (r.chunk_text.size() > 200 ? "..." : "") << "\n\n";
        }
    }

    void print_menu()
    {
        std::cout << "\n=== olli RAG admin ===\n"
                      "1) List collections\n"
                      "2) Create collection\n"
                      "3) Import a file\n"
                      "4) List documents in a collection\n"
                      "5) Delete a document\n"
                      "6) Search\n"
                      "0) Quit\n";
    }

    void print_usage(const char* argv0)
    {
        std::string prog = argv0;
        size_t slash = prog.find_last_of('/');
        if (slash != std::string::npos) prog = prog.substr(slash + 1);

        std::cout << "Usage: " << prog << " [db_path] [-h|--help]\n\n"
                      "  db_path       Path to the RAG SQLite database. Created if it doesn't\n"
                      "                exist yet. Defaults to ./rag.db\n\n"
                      "  -h, --help    Show this help and exit.\n";
    }
}

int main(int argc, char* argv[])
{
    std::string db_path = "rag.db";

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help") { print_usage(argv[0]); return 0; }
        db_path = arg1;
    }

    RAG_DB db(db_path);
    if (!db.is_open()) {
        std::cerr << "Failed to open database at \"" << db_path << "\": " << db.last_error() << "\n";
        return 1;
    }
    std::cout << "Using RAG database: " << db_path << "\n";

    RAG_EMBEDDER embedder;

    bool quit = false;
    while (!quit) {
        print_menu();
        std::string choice = prompt_line("> ");

        if (choice == "1") do_list_collections(db);
        else if (choice == "2") do_create_collection(db);
        else if (choice == "3") do_import_file(db, embedder);
        else if (choice == "4") do_list_documents(db);
        else if (choice == "5") do_delete_document(db);
        else if (choice == "6") do_search(db, embedder);
        else if (choice == "0" || choice == "q" || choice == "Q") quit = true;
        else std::cout << "Unknown choice.\n";
    }

    return 0;
}
