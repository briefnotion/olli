// rag_admin - standalone, menu-driven maintenance program for olli's RAG
// database: create/edit/delete collections, sync their on-disk folders into
// the database, list documents, and preview every search mode rag_tool
// offers (find-a-passage, survey-a-topic, full-document) without needing
// olli at all. Talks only to rag_db.hpp/rag_embed.hpp/rag_chunk.hpp in
// ../rag_db/ - never to olli itself over the wire protocol (that's
// rag_tool's job, see ../../PROTOCOL.md).
//
// No standalone "delete a document" any more - if its source file still
// exists in the collection's folder, deleting just the database row would
// only have it reimported on the next "Update database" anyway. Delete the
// file (or the whole collection) instead.
//
// Build: `make` in this directory (needs libsqlite3-dev and libcurl, same
// as ../rag_db/'s smoke test). Run: `./rag_admin [profile_name]` - opens
// (creating if needed) that profile's database at ~/olli_files_<name>/
// rag.db, same convention presence/clock already use for their own
// per-profile state (see rag_db.hpp's profile_db_path()). Defaults to the
// shared ~/olli_files/rag.db when no profile is given. Needs Ollama running
// locally with the embedding model pulled (see ../rag_db/rag_embed.hpp for
// the default).
//
// Each collection has its own folder on disk, ~/olli_files_<profile>/
// collection/<name>/ (rag_db.hpp's profile_collection_dir(), created by
// "Create collection" below) - drop plain-text/markdown files in there and
// run "Update database" to sync them in. That sync is a real three-way
// diff against what's already in the database (matched by each document's
// stored source path and content_hash - see rag_db.hpp's hash_content()):
// a new file gets imported, an unchanged one is left alone, a changed one
// is deleted and reimported, and a document whose source file is gone from
// disk is deleted. There's no more "import an arbitrary path from anywhere
// on disk" option - if it's not in a collection's folder, "Update database"
// won't see it.
//
// One exception: "conversations" is a special collection, auto-created
// (never via "Create collection") the moment a profile has a chat_logs/
// directory - see do_update_database(). It syncs straight against that
// existing directory (rag_db.hpp's profile_chat_logs_dir()) rather than a
// collection/ subfolder, skips anything under MIN_CHAT_LOG_WORDS as noise,
// and tags each document with its parsed log_date/log_time in metadata.
// Still no olli-side auto-import - this only runs when "Update database"
// is run by hand.

#include "rag_db.hpp"
#include "rag_embed.hpp"
#include "rag_sync.hpp"

#include <filesystem>
#include <iostream>
#include <optional>
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

    void do_list_collections(const RAG_DB& db)
    {
        std::cout << "\n--- Collections ---\n";
        print_numbered_collections(db);
    }

    void do_create_collection(RAG_DB& db, const std::string& profile_name)
    {
        std::string name = prompt_line("\nNew collection name: ");
        if (name.empty()) { std::cout << "Cancelled.\n"; return; }

        std::string description = prompt_line("Description: ");
        int id = db.get_or_create_collection(name, description);
        if (id < 0) { std::cout << "Failed: " << db.last_error() << "\n"; return; }

        std::string dir = profile_collection_dir(profile_name, name);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            std::cout << "Created collection \"" << name << "\" (id " << id
                       << "), but failed to create its folder " << dir << ": " << ec.message() << "\n";
            return;
        }

        std::cout << "Created collection \"" << name << "\" (id " << id << ").\n"
                   << "Drop files into " << dir << " and run \"Update database\" to import them.\n";
    }

    void do_edit_collection_description(RAG_DB& db)
    {
        std::cout << "\nCollections:\n";
        std::vector<RAG_COLLECTION> collections = print_numbered_collections(db);
        if (collections.empty()) return;

        int index = prompt_int("\nWhich collection number? ");
        if (index < 1 || index > static_cast<int>(collections.size())) { std::cout << "Cancelled.\n"; return; }
        const RAG_COLLECTION& collection = collections[static_cast<size_t>(index) - 1];

        std::string new_description = prompt_line("New description for \"" + collection.name + "\" [" + collection.description + "]: ");
        if (new_description.empty()) { std::cout << "Cancelled (left unchanged).\n"; return; }

        if (db.update_collection_description(collection.id, new_description)) std::cout << "Updated.\n";
        else std::cout << "Failed: " << db.last_error() << "\n";
    }

    void do_delete_collection(RAG_DB& db, const std::string& profile_name)
    {
        std::cout << "\nCollections:\n";
        std::vector<RAG_COLLECTION> collections = print_numbered_collections(db);
        if (collections.empty()) return;

        int index = prompt_int("\nWhich collection number to delete? ");
        if (index < 1 || index > static_cast<int>(collections.size())) { std::cout << "Cancelled.\n"; return; }
        const RAG_COLLECTION& collection = collections[static_cast<size_t>(index) - 1];

        int document_count = static_cast<int>(db.list_documents(collection.id).size());
        std::string dir = profile_collection_dir(profile_name, collection.name);

        std::string confirm = prompt_line("Delete collection \"" + collection.name + "\" and its " +
            std::to_string(document_count) + " document(s) from the database? Files in " + dir +
            " will NOT be touched. [y/N]: ");
        if (confirm != "y" && confirm != "Y") { std::cout << "Cancelled.\n"; return; }

        if (db.delete_collection(collection.id)) {
            std::cout << "Deleted collection \"" << collection.name << "\" from the database.\n"
                       << "Its folder (" << dir << ") and files were left untouched.\n";
        } else {
            std::cout << "Failed: " << db.last_error() << "\n";
        }
    }

    // The actual sync logic (the three-way diff per collection, plus the
    // special "conversations" collection) moved to ../rag_db/rag_sync.hpp
    // (2026-09-12) so rag_tool can run the exact same code on its own
    // timer instead of drifting from a second copy - see that file for
    // what it does. This just prints the result the way "Update database"
    // always has.
    void do_update_database(RAG_DB& db, RAG_EMBEDDER& embedder, const std::string& profile_name)
    {
        RAG_SYNC_STATS stats = sync_profile_collections(db, embedder, profile_name);

        if (stats.skipped_busy) {
            std::cout << "\nSync already in progress (rag_tool?) - try again in a moment.\n";
            return;
        }

        std::cout << "\nDone: " << stats.imported << " imported, " << stats.updated << " updated, "
                   << stats.unchanged << " unchanged, " << stats.removed << " removed.\n";
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

    // Prints every collection and prompts for one to restrict a search to,
    // shared by do_search/do_search_documents. Blank input, or anything
    // that doesn't parse as a listed number, means "all collections."
    std::optional<int> prompt_optional_collection(const RAG_DB& db)
    {
        std::cout << "Restrict to one collection?\n";
        std::vector<RAG_COLLECTION> collections = print_numbered_collections(db);
        std::string choice = prompt_line("Collection number (blank for all): ");

        if (!choice.empty()) {
            try {
                size_t pos = 0;
                int index = std::stoi(choice, &pos);
                if (pos == choice.size() && index >= 1 && index <= static_cast<int>(collections.size())) {
                    return collections[static_cast<size_t>(index) - 1].id;
                }
            } catch (const std::exception&) {
                // Not a number - treat as "all collections".
            }
        }
        return std::nullopt;
    }

    void print_search_results(const std::vector<RAG_SEARCH_RESULT>& results)
    {
        if (results.empty()) { std::cout << "No results.\n"; return; }

        std::cout << "\n--- Results ---\n";
        for (const auto& r : results) {
            std::string snippet = r.chunk_text.substr(0, 200);
            std::cout << "[" << r.score << "] " << r.collection_name << " / " << r.document_title
                       << " (chunk " << r.chunk_index << "):\n    " << snippet
                       << (r.chunk_text.size() > 200 ? "..." : "") << "\n\n";
        }
    }

    void do_search(RAG_DB& db, RAG_EMBEDDER& embedder)
    {
        std::string query = prompt_line("\nSearch query: ");
        if (query.empty()) { std::cout << "Cancelled.\n"; return; }

        std::optional<int> collection_id = prompt_optional_collection(db);

        std::vector<float> query_embedding = embedder.embed_query(query);
        if (query_embedding.empty()) { std::cout << "Embedding failed: " << embedder.last_error() << "\n"; return; }

        // Fixed top-5 chunks - see rag_tool's own rag_search for why (one
        // less parameter to get wrong).
        print_search_results(db.search(query_embedding, 5, collection_id));
    }

    void do_search_documents(RAG_DB& db, RAG_EMBEDDER& embedder)
    {
        std::string query = prompt_line("\nSearch query: ");
        if (query.empty()) { std::cout << "Cancelled.\n"; return; }

        std::optional<int> collection_id = prompt_optional_collection(db);

        std::vector<float> query_embedding = embedder.embed_query(query);
        if (query_embedding.empty()) { std::cout << "Embedding failed: " << embedder.last_error() << "\n"; return; }

        // Deduplicated to one (best) result per document, top-10 - mirrors
        // rag_tool's rag_search_documents exactly, so this menu option
        // previews the same behavior olli would actually get.
        print_search_results(db.search_documents(query_embedding, 10, collection_id));
    }

    void do_view_document(const RAG_DB& db)
    {
        std::cout << "\nCollections:\n";
        std::vector<RAG_COLLECTION> collections = print_numbered_collections(db);
        if (collections.empty()) return;

        int index = prompt_int("\nWhich collection number? ");
        if (index < 1 || index > static_cast<int>(collections.size())) { std::cout << "Cancelled.\n"; return; }
        const RAG_COLLECTION& collection = collections[static_cast<size_t>(index) - 1];

        std::vector<RAG_DOCUMENT> documents = db.list_documents(collection.id);
        if (documents.empty()) { std::cout << "(no documents in this collection)\n"; return; }

        std::cout << "\nDocuments in \"" << collection.name << "\":\n";
        for (size_t i = 0; i < documents.size(); i++) {
            std::cout << "  " << (i + 1) << ") " << documents[i].title << "\n";
        }

        int doc_index = prompt_int("\nWhich document number? ");
        if (doc_index < 1 || doc_index > static_cast<int>(documents.size())) { std::cout << "Cancelled.\n"; return; }
        const RAG_DOCUMENT& doc = documents[static_cast<size_t>(doc_index) - 1];

        std::cout << "\n--- " << collection.name << " / " << doc.title << " ---\n" << doc.content << "\n";
    }

    void print_menu()
    {
        std::cout << "\n=== olli RAG admin ===\n"
                      "1) List collections\n"
                      "2) Create collection\n"
                      "3) Edit collection description\n"
                      "4) Delete collection\n"
                      "5) Update database (sync collection folders)\n"
                      "6) List documents in a collection\n"
                      "7) Search (find a passage)\n"
                      "8) Search documents (survey a topic)\n"
                      "9) View a document's full content\n"
                      "0) Quit\n";
    }

    void print_usage(const char* argv0)
    {
        std::string prog = argv0;
        size_t slash = prog.find_last_of('/');
        if (slash != std::string::npos) prog = prog.substr(slash + 1);

        std::cout << "Usage: " << prog << " [profile_name] [-h|--help]\n\n"
                      "  profile_name  Which olli profile's RAG database to open, e.g. \"ron\"\n"
                      "                for ~/olli_files_ron/rag.db. Defaults to the shared\n"
                      "                ~/olli_files/rag.db when omitted. Created (file and\n"
                      "                schema) if it doesn't exist yet - the profile directory\n"
                      "                itself must already exist (olli creates it).\n\n"
                      "  -h, --help    Show this help and exit.\n";
    }
}

int main(int argc, char* argv[])
{
    std::string profile_name;

    if (argc > 1) {
        std::string arg1 = argv[1];
        if (arg1 == "-h" || arg1 == "--help") { print_usage(argv[0]); return 0; }
        profile_name = arg1;
    }

    std::string db_path = profile_db_path(profile_name);
    RAG_DB db(db_path);
    if (!db.is_open()) {
        std::cerr << "Failed to open database at \"" << db_path << "\": " << db.last_error() << "\n";
        return 1;
    }
    std::cout << "Using RAG database for profile \"" << (profile_name.empty() ? "(shared, no profile)" : profile_name)
               << "\": " << db_path << "\n";

    RAG_EMBEDDER embedder;

    bool quit = false;
    while (!quit) {
        print_menu();
        std::string choice = prompt_line("> ");

        if (choice == "1") do_list_collections(db);
        else if (choice == "2") do_create_collection(db, profile_name);
        else if (choice == "3") do_edit_collection_description(db);
        else if (choice == "4") do_delete_collection(db, profile_name);
        else if (choice == "5") do_update_database(db, embedder, profile_name);
        else if (choice == "6") do_list_documents(db);
        else if (choice == "7") do_search(db, embedder);
        else if (choice == "8") do_search_documents(db, embedder);
        else if (choice == "9") do_view_document(db);
        else if (choice == "0" || choice == "q" || choice == "Q") quit = true;
        else std::cout << "Unknown choice.\n";
    }

    return 0;
}
