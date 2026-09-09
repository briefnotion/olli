#include "rag_db.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
    // collections.name is COLLATE NOCASE (case-insensitive comparisons and
    // uniqueness, applied automatically to every WHERE/UNIQUE check
    // against this column - no query-level changes needed elsewhere) -
    // found via a real live test: rag_list_collections told the model a
    // collection named "Notes" existed, and it kept calling back with
    // "notes" (lowercase) on every follow-up, failing a plain case-
    // sensitive match every time even though the collection was right
    // there. Models don't reliably preserve exact case when recalling a
    // name from earlier context; the schema shouldn't require that they do.
    constexpr const char* SCHEMA_SQL = R"SQL(
        PRAGMA foreign_keys = ON;

        CREATE TABLE IF NOT EXISTS collections (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL COLLATE NOCASE UNIQUE,
            description TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS documents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE,
            title TEXT NOT NULL COLLATE NOCASE,
            source TEXT NOT NULL DEFAULT '',
            content TEXT NOT NULL DEFAULT '',
            content_hash TEXT NOT NULL DEFAULT '',
            metadata TEXT NOT NULL DEFAULT '{}',
            imported_at TEXT NOT NULL DEFAULT (datetime('now'))
        );

        CREATE TABLE IF NOT EXISTS chunks (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            document_id INTEGER NOT NULL REFERENCES documents(id) ON DELETE CASCADE,
            chunk_index INTEGER NOT NULL,
            chunk_text TEXT NOT NULL,
            embedding BLOB NOT NULL,
            embedding_dim INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_documents_collection ON documents(collection_id);
        CREATE INDEX IF NOT EXISTS idx_chunks_document ON chunks(document_id);
    )SQL";

    // Strips a trailing ".ext" from a filename-like title, if present -
    // see find_document_by_title()'s fallback for why.
    std::string strip_extension(const std::string& title)
    {
        size_t dot = title.find_last_of('.');
        return (dot == std::string::npos || dot == 0) ? title : title.substr(0, dot);
    }

    std::string ascii_lowercase(const std::string& s)
    {
        std::string result = s;
        for (char& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return result;
    }

    float cosine_similarity(const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.empty() || a.size() != b.size()) return -1.0f;

        double dot = 0.0, norm_a = 0.0, norm_b = 0.0;
        for (size_t i = 0; i < a.size(); i++) {
            dot += static_cast<double>(a[i]) * b[i];
            norm_a += static_cast<double>(a[i]) * a[i];
            norm_b += static_cast<double>(b[i]) * b[i];
        }
        if (norm_a == 0.0 || norm_b == 0.0) return -1.0f;
        return static_cast<float>(dot / (std::sqrt(norm_a) * std::sqrt(norm_b)));
    }

    std::string text_column(sqlite3_stmt* stmt, int col)
    {
        const unsigned char* text = sqlite3_column_text(stmt, col);
        return text ? reinterpret_cast<const char*>(text) : "";
    }
}

namespace {
    std::string profile_dir(const std::string& profile_name)
    {
        const char* home = std::getenv("HOME");
        std::string home_dir = home ? home : ".";
        std::string suffix = profile_name.empty() ? "" : ("_" + profile_name);
        return home_dir + "/olli_files" + suffix;
    }
}

std::string profile_db_path(const std::string& profile_name)
{
    return profile_dir(profile_name) + "/rag.db";
}

std::string profile_collection_dir(const std::string& profile_name, const std::string& collection_name)
{
    return profile_dir(profile_name) + "/collection/" + collection_name;
}

std::string hash_content(const std::string& content)
{
    // FNV-1a, 64-bit - see rag_db.hpp's comment on hash_content() for why a
    // simple non-cryptographic hash is the right tool here.
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (unsigned char c : content) {
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }

    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(hex);
}

RAG_DB::RAG_DB(const std::string& db_path)
{
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        error = db ? sqlite3_errmsg(db) : "sqlite3_open failed";
        if (db) { sqlite3_close(db); db = nullptr; }
        return;
    }

    char* errmsg = nullptr;
    if (sqlite3_exec(db, SCHEMA_SQL, nullptr, nullptr, &errmsg) != SQLITE_OK) {
        error = errmsg ? errmsg : "schema init failed";
        sqlite3_free(errmsg);
        sqlite3_close(db);
        db = nullptr;
    }
}

RAG_DB::~RAG_DB()
{
    if (db) sqlite3_close(db);
}

int RAG_DB::get_or_create_collection(const std::string& name, const std::string& description)
{
    if (!db) return -1;

    if (auto existing = find_collection(name)) return existing->id;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO collections (name, description) VALUES (?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, description.c_str(), -1, SQLITE_TRANSIENT);

    int result_id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        result_id = static_cast<int>(sqlite3_last_insert_rowid(db));
    } else {
        error = sqlite3_errmsg(db);
    }
    sqlite3_finalize(stmt);
    return result_id;
}

std::optional<RAG_COLLECTION> RAG_DB::find_collection(const std::string& name) const
{
    if (!db) return std::nullopt;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, description FROM collections WHERE name = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return std::nullopt;
    }
    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<RAG_COLLECTION> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        RAG_COLLECTION c;
        c.id = sqlite3_column_int(stmt, 0);
        c.name = text_column(stmt, 1);
        c.description = text_column(stmt, 2);
        result = c;
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<RAG_COLLECTION> RAG_DB::list_collections() const
{
    std::vector<RAG_COLLECTION> results;
    if (!db) return results;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, name, description FROM collections ORDER BY name;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return results;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RAG_COLLECTION c;
        c.id = sqlite3_column_int(stmt, 0);
        c.name = text_column(stmt, 1);
        c.description = text_column(stmt, 2);
        results.push_back(c);
    }
    sqlite3_finalize(stmt);
    return results;
}

bool RAG_DB::update_collection_description(int collection_id, const std::string& description)
{
    if (!db) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE collections SET description = ? WHERE id = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_text(stmt, 1, description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, collection_id);

    bool step_ok = sqlite3_step(stmt) == SQLITE_DONE;
    bool ok = step_ok && sqlite3_changes(db) > 0;
    if (!step_ok) error = sqlite3_errmsg(db);
    else if (!ok) error = "No collection with that id";
    sqlite3_finalize(stmt);
    return ok;
}

bool RAG_DB::delete_collection(int collection_id)
{
    if (!db) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM collections WHERE id = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int(stmt, 1, collection_id);

    bool step_ok = sqlite3_step(stmt) == SQLITE_DONE;
    bool ok = step_ok && sqlite3_changes(db) > 0;
    if (!step_ok) error = sqlite3_errmsg(db);
    else if (!ok) error = "No collection with that id";
    sqlite3_finalize(stmt);
    return ok;
}

int RAG_DB::add_document(int collection_id, const std::string& title, const std::string& source, const std::string& content, const std::string& content_hash, const std::string& metadata_json)
{
    if (!db) return -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO documents (collection_id, title, source, content, content_hash, metadata) VALUES (?, ?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, collection_id);
    sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, metadata_json.c_str(), -1, SQLITE_TRANSIENT);

    int result_id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        result_id = static_cast<int>(sqlite3_last_insert_rowid(db));
    } else {
        error = sqlite3_errmsg(db);
    }
    sqlite3_finalize(stmt);
    return result_id;
}

namespace {
    RAG_DOCUMENT document_from_row(sqlite3_stmt* stmt)
    {
        RAG_DOCUMENT d;
        d.id = sqlite3_column_int(stmt, 0);
        d.collection_id = sqlite3_column_int(stmt, 1);
        d.title = text_column(stmt, 2);
        d.source = text_column(stmt, 3);
        d.content = text_column(stmt, 4);
        d.content_hash = text_column(stmt, 5);
        d.metadata = text_column(stmt, 6);
        d.imported_at = text_column(stmt, 7);
        return d;
    }

    constexpr const char* DOCUMENT_COLUMNS = "id, collection_id, title, source, content, content_hash, metadata, imported_at";
}

std::vector<RAG_DOCUMENT> RAG_DB::list_documents(int collection_id) const
{
    std::vector<RAG_DOCUMENT> results;
    if (!db) return results;

    sqlite3_stmt* stmt = nullptr;
    std::string sql = std::string("SELECT ") + DOCUMENT_COLUMNS + " FROM documents WHERE collection_id = ? ORDER BY id;";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return results;
    }
    sqlite3_bind_int(stmt, 1, collection_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) results.push_back(document_from_row(stmt));
    sqlite3_finalize(stmt);
    return results;
}

std::optional<RAG_DOCUMENT> RAG_DB::find_document_by_source(int collection_id, const std::string& source) const
{
    if (!db) return std::nullopt;

    sqlite3_stmt* stmt = nullptr;
    std::string sql = std::string("SELECT ") + DOCUMENT_COLUMNS + " FROM documents WHERE collection_id = ? AND source = ?;";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return std::nullopt;
    }
    sqlite3_bind_int(stmt, 1, collection_id);
    sqlite3_bind_text(stmt, 2, source.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<RAG_DOCUMENT> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = document_from_row(stmt);
    sqlite3_finalize(stmt);
    return result;
}

std::optional<RAG_DOCUMENT> RAG_DB::find_document_by_title(int collection_id, const std::string& title) const
{
    if (!db) return std::nullopt;

    sqlite3_stmt* stmt = nullptr;
    std::string sql = std::string("SELECT ") + DOCUMENT_COLUMNS + " FROM documents WHERE collection_id = ? AND title = ?;";
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return std::nullopt;
    }
    sqlite3_bind_int(stmt, 1, collection_id);
    sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);

    std::optional<RAG_DOCUMENT> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) result = document_from_row(stmt);
    sqlite3_finalize(stmt);
    if (result) return result;

    // Fallback: titles are filenames (see rag_admin's sync), and a model
    // asked for a document by name tends to drop the file extension it
    // was shown (found via the same live test that motivated collections'
    // COLLATE NOCASE above) - "misc_notes" for a document actually titled
    // "misc_notes.txt". Comparing both sides with any extension stripped
    // catches that without changing what's actually stored (which would
    // risk two different-extension files colliding on one bare name).
    std::string bare_title = strip_extension(title);
    for (const RAG_DOCUMENT& doc : list_documents(collection_id)) {
        if (ascii_lowercase(strip_extension(doc.title)) == ascii_lowercase(bare_title)) return doc;
    }
    return std::nullopt;
}

bool RAG_DB::delete_document(int document_id)
{
    if (!db) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "DELETE FROM documents WHERE id = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int(stmt, 1, document_id);

    bool step_ok = sqlite3_step(stmt) == SQLITE_DONE;
    bool ok = step_ok && sqlite3_changes(db) > 0;
    if (!step_ok) error = sqlite3_errmsg(db);
    else if (!ok) error = "No document with that id";
    sqlite3_finalize(stmt);
    return ok;
}

bool RAG_DB::add_chunk(int document_id, int chunk_index, const std::string& chunk_text, const std::vector<float>& embedding)
{
    if (!db) return false;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO chunks (document_id, chunk_index, chunk_text, embedding, embedding_dim) "
                       "VALUES (?, ?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return false;
    }
    sqlite3_bind_int(stmt, 1, document_id);
    sqlite3_bind_int(stmt, 2, chunk_index);
    sqlite3_bind_text(stmt, 3, chunk_text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 4, embedding.data(), static_cast<int>(embedding.size() * sizeof(float)), SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 5, static_cast<int>(embedding.size()));

    bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok) error = sqlite3_errmsg(db);
    sqlite3_finalize(stmt);
    return ok;
}

std::vector<RAG_SEARCH_RESULT> RAG_DB::score_all_chunks(const std::vector<float>& query_embedding, std::optional<int> collection_id) const
{
    std::vector<RAG_SEARCH_RESULT> results;
    if (!db || query_embedding.empty()) return results;

    std::string sql =
        "SELECT c.document_id, c.chunk_index, c.chunk_text, c.embedding, c.embedding_dim, "
        "       d.title, col.name "
        "FROM chunks c "
        "JOIN documents d ON d.id = c.document_id "
        "JOIN collections col ON col.id = d.collection_id ";
    if (collection_id) sql += "WHERE d.collection_id = ? ";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return results;
    }
    if (collection_id) sqlite3_bind_int(stmt, 1, *collection_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int document_id = sqlite3_column_int(stmt, 0);
        int chunk_index = sqlite3_column_int(stmt, 1);
        std::string chunk_text = text_column(stmt, 2);
        const void* blob = sqlite3_column_blob(stmt, 3);
        int embedding_dim = sqlite3_column_int(stmt, 4);
        std::string title = text_column(stmt, 5);
        std::string collection_name = text_column(stmt, 6);

        std::vector<float> embedding(embedding_dim > 0 ? embedding_dim : 0);
        if (blob && embedding_dim > 0) {
            std::memcpy(embedding.data(), blob, static_cast<size_t>(embedding_dim) * sizeof(float));
        }

        RAG_SEARCH_RESULT r;
        r.document_id = document_id;
        r.document_title = title;
        r.collection_name = collection_name;
        r.chunk_index = chunk_index;
        r.chunk_text = chunk_text;
        r.score = cosine_similarity(query_embedding, embedding);
        results.push_back(r);
    }
    sqlite3_finalize(stmt);
    return results;
}

std::vector<RAG_SEARCH_RESULT> RAG_DB::search(const std::vector<float>& query_embedding, int top_k, std::optional<int> collection_id) const
{
    std::vector<RAG_SEARCH_RESULT> results = score_all_chunks(query_embedding, collection_id);

    std::sort(results.begin(), results.end(), [](const RAG_SEARCH_RESULT& a, const RAG_SEARCH_RESULT& b) {
        return a.score > b.score;
    });
    if (static_cast<int>(results.size()) > top_k) results.resize(static_cast<size_t>(top_k));

    return results;
}

std::vector<RAG_SEARCH_RESULT> RAG_DB::search_documents(const std::vector<float>& query_embedding, int top_k, std::optional<int> collection_id) const
{
    std::vector<RAG_SEARCH_RESULT> all_chunks = score_all_chunks(query_embedding, collection_id);

    // Keep only each document's single best-scoring chunk.
    std::vector<RAG_SEARCH_RESULT> best_per_document;
    for (const auto& chunk : all_chunks) {
        auto existing = std::find_if(best_per_document.begin(), best_per_document.end(),
            [&](const RAG_SEARCH_RESULT& r) { return r.document_id == chunk.document_id; });
        if (existing == best_per_document.end()) {
            best_per_document.push_back(chunk);
        } else if (chunk.score > existing->score) {
            *existing = chunk;
        }
    }

    std::sort(best_per_document.begin(), best_per_document.end(), [](const RAG_SEARCH_RESULT& a, const RAG_SEARCH_RESULT& b) {
        return a.score > b.score;
    });
    if (static_cast<int>(best_per_document.size()) > top_k) best_per_document.resize(static_cast<size_t>(top_k));

    return best_per_document;
}
