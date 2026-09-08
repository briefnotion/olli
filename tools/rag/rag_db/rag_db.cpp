#include "rag_db.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
    constexpr const char* SCHEMA_SQL = R"SQL(
        PRAGMA foreign_keys = ON;

        CREATE TABLE IF NOT EXISTS collections (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            name TEXT NOT NULL UNIQUE,
            description TEXT NOT NULL DEFAULT ''
        );

        CREATE TABLE IF NOT EXISTS documents (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE,
            title TEXT NOT NULL,
            source TEXT NOT NULL DEFAULT '',
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

int RAG_DB::add_document(int collection_id, const std::string& title, const std::string& source, const std::string& metadata_json)
{
    if (!db) return -1;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "INSERT INTO documents (collection_id, title, source, metadata) VALUES (?, ?, ?, ?);";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, collection_id);
    sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, metadata_json.c_str(), -1, SQLITE_TRANSIENT);

    int result_id = -1;
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        result_id = static_cast<int>(sqlite3_last_insert_rowid(db));
    } else {
        error = sqlite3_errmsg(db);
    }
    sqlite3_finalize(stmt);
    return result_id;
}

std::vector<RAG_DOCUMENT> RAG_DB::list_documents(int collection_id) const
{
    std::vector<RAG_DOCUMENT> results;
    if (!db) return results;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT id, collection_id, title, source, metadata, imported_at "
                       "FROM documents WHERE collection_id = ? ORDER BY id;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        error = sqlite3_errmsg(db);
        return results;
    }
    sqlite3_bind_int(stmt, 1, collection_id);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RAG_DOCUMENT d;
        d.id = sqlite3_column_int(stmt, 0);
        d.collection_id = sqlite3_column_int(stmt, 1);
        d.title = text_column(stmt, 2);
        d.source = text_column(stmt, 3);
        d.metadata = text_column(stmt, 4);
        d.imported_at = text_column(stmt, 5);
        results.push_back(d);
    }
    sqlite3_finalize(stmt);
    return results;
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

std::vector<RAG_SEARCH_RESULT> RAG_DB::search(const std::vector<float>& query_embedding, int top_k, std::optional<int> collection_id) const
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

    std::sort(results.begin(), results.end(), [](const RAG_SEARCH_RESULT& a, const RAG_SEARCH_RESULT& b) {
        return a.score > b.score;
    });
    if (static_cast<int>(results.size()) > top_k) results.resize(static_cast<size_t>(top_k));

    return results;
}
