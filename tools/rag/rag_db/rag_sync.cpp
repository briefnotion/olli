#include "rag_sync.hpp"
#include "rag_chunk.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace {
    std::string trim(const std::string& s)
    {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return "";
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    }

    std::string basename_of(const std::string& path)
    {
        size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    int word_count(const std::string& s)
    {
        std::istringstream stream(s);
        int count = 0;
        std::string word;
        while (stream >> word) count++;
        return count;
    }

    // The three-way sync described in this file's header comment, for one
    // collection: new file -> import, unchanged (same content_hash) ->
    // skip, changed -> delete old document and reimport, document whose
    // source is no longer on disk -> delete.
    //
    // min_words/metadata_for exist only for the special "conversations"
    // sync (see sync_profile_collections() below) - a regular collection
    // leaves both at their defaults (no size filter, plain "{}" metadata).
    void sync_collection(RAG_DB& db, RAG_EMBEDDER& embedder, const RAG_COLLECTION& collection, const std::string& dir,
                          int& imported, int& updated, int& unchanged, int& removed,
                          int min_words = 0,
                          const std::function<std::string(const std::string&)>& metadata_for = nullptr)
    {
        std::vector<RAG_DOCUMENT> existing = db.list_documents(collection.id);
        std::set<std::string> sources_on_disk;

        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;

            std::string path = entry.path().string();
            std::string filename = entry.path().filename().string();
            if (filename.empty() || filename[0] == '.') continue; // skip hidden files

            std::ifstream file(path);
            if (!file) { std::cout << "  " << filename << ": could not open, skipping\n"; continue; }
            std::stringstream buffer;
            buffer << file.rdbuf();
            std::string content = buffer.str();
            if (trim(content).empty()) { std::cout << "  " << filename << ": empty, skipping\n"; continue; }
            if (min_words > 0 && word_count(content) < min_words) {
                std::cout << "  " << filename << ": too short (under " << min_words << " words), skipping\n";
                continue;
            }

            sources_on_disk.insert(path);
            std::string hash = hash_content(content);

            auto existing_doc = std::find_if(existing.begin(), existing.end(),
                [&](const RAG_DOCUMENT& d) { return d.source == path; });

            if (existing_doc != existing.end()) {
                if (existing_doc->content_hash == hash) {
                    std::cout << "  " << filename << ": unchanged, skipping\n";
                    unchanged++;
                    continue;
                }
                std::cout << "  " << filename << ": changed, reimporting\n";
                db.delete_document(existing_doc->id);
                updated++;
            } else {
                std::cout << "  " << filename << ": new, importing\n";
                imported++;
            }

            std::string metadata_json = metadata_for ? metadata_for(filename) : "{}";
            int document_id = db.add_document(collection.id, filename, path, content, hash, metadata_json);
            if (document_id < 0) {
                std::cout << "    failed to create document: " << db.last_error() << "\n";
                continue;
            }

            std::vector<std::string> chunks = chunk_text(content);
            bool ok = true;
            for (size_t i = 0; i < chunks.size(); i++) {
                std::vector<float> embedding = embedder.embed_document(chunks[i]);
                if (embedding.empty()) {
                    std::cout << "    embedding failed on chunk " << i << ": " << embedder.last_error() << " - stopping this file\n";
                    ok = false;
                    break;
                }
                if (!db.add_chunk(document_id, static_cast<int>(i), chunks[i], embedding)) {
                    std::cout << "    failed to store chunk " << i << ": " << db.last_error() << "\n";
                    ok = false;
                    break;
                }
            }
            std::cout << "    " << (ok ? "stored " : "partially stored ") << chunks.size() << " chunk(s)\n";
        }

        for (const auto& d : existing) {
            if (sources_on_disk.count(d.source) == 0) {
                std::cout << "  " << basename_of(d.source) << ": source removed, deleting document\n";
                if (db.delete_document(d.id)) removed++;
            }
        }
    }

    // Chat log filenames look like "260909.1216.chat_log.txt" or
    // "260821.0506.2.chat_log.txt" (an extra numeric disambiguator for a
    // second log started the same minute) - YYMMDD.HHMM, always. Falls
    // back to "{}" (no metadata) on anything that doesn't match, rather
    // than guessing - a filename olli didn't generate itself shouldn't
    // produce fabricated dates.
    std::string chat_log_metadata(const std::string& filename)
    {
        if (filename.size() < 11 || filename[6] != '.') return "{}";
        std::string yymmdd = filename.substr(0, 6);
        std::string hhmm = filename.substr(7, 4);
        bool digits_ok = std::all_of(yymmdd.begin(), yymmdd.end(), [](unsigned char c) { return std::isdigit(c); }) &&
                          std::all_of(hhmm.begin(), hhmm.end(), [](unsigned char c) { return std::isdigit(c); });
        if (!digits_ok) return "{}";

        std::string date = "20" + yymmdd.substr(0, 2) + "-" + yymmdd.substr(2, 2) + "-" + yymmdd.substr(4, 2);
        std::string time = hhmm.substr(0, 2) + ":" + hhmm.substr(2, 2);
        return "{\"log_date\": \"" + date + "\", \"log_time\": \"" + time + "\"}";
    }

    // Chat logs shorter than this are treated as noise (a bare "hi"/"bye"
    // with nothing else) and skipped entirely - see sync_collection()'s
    // min_words.
    constexpr int MIN_CHAT_LOG_WORDS = 30;

    // RAII non-blocking advisory file lock - see sync_profile_collections()'s
    // own header comment for why this exists. Opens (creating if needed)
    // the lock file and tries flock(LOCK_EX | LOCK_NB); held() reports
    // whether that succeeded. The lock releases automatically on
    // destruction - flock()'s lock doesn't outlive the fd it was taken on,
    // so simply closing it is enough, even if the process were to exit
    // abnormally.
    class SYNC_LOCK {
        public:
            explicit SYNC_LOCK(const std::string& path)
            {
                fd = open(path.c_str(), O_CREAT | O_RDWR, 0644);
                if (fd >= 0) held_ = (flock(fd, LOCK_EX | LOCK_NB) == 0);
            }

            ~SYNC_LOCK()
            {
                if (fd >= 0) close(fd);
            }

            SYNC_LOCK(const SYNC_LOCK&) = delete;
            SYNC_LOCK& operator=(const SYNC_LOCK&) = delete;

            bool held() const { return held_; }

        private:
            int fd = -1;
            bool held_ = false;
    };
}

RAG_SYNC_STATS sync_profile_collections(RAG_DB& db, RAG_EMBEDDER& embedder, const std::string& profile_name)
{
    RAG_SYNC_STATS stats;

    SYNC_LOCK lock(profile_sync_lock_path(profile_name));
    if (!lock.held()) {
        stats.skipped_busy = true;
        return stats;
    }

    // Special auto-synced collection: olli's own chat_logs/ directory for
    // this profile, already tied 1:1 to it - never created via "Create
    // collection", it just exists (get_or_create_collection) the moment
    // there's a chat_logs/ directory to sync against.
    std::string chat_logs_dir = profile_chat_logs_dir(profile_name);
    if (std::filesystem::is_directory(chat_logs_dir)) {
        int conversations_id = db.get_or_create_collection("conversations", "Auto-synced chat history from past conversations with olli.");
        if (conversations_id < 0) {
            std::cout << "\nFailed to set up the conversations collection: " << db.last_error() << "\n";
        } else {
            RAG_COLLECTION conversations{conversations_id, "conversations", ""};
            std::cout << "\n[conversations] " << chat_logs_dir << "\n";
            sync_collection(db, embedder, conversations, chat_logs_dir, stats.imported, stats.updated, stats.unchanged, stats.removed,
                             MIN_CHAT_LOG_WORDS, chat_log_metadata);
        }
    }

    for (const auto& collection : db.list_collections()) {
        if (collection.name == "conversations") continue; // handled above, against chat_logs_dir not collection_dir

        std::string dir = profile_collection_dir(profile_name, collection.name);
        std::cout << "\n[" << collection.name << "] " << dir << "\n";

        if (!std::filesystem::is_directory(dir)) {
            std::cout << "  (folder doesn't exist - skipping)\n";
            continue;
        }

        sync_collection(db, embedder, collection, dir, stats.imported, stats.updated, stats.unchanged, stats.removed);
    }

    return stats;
}
