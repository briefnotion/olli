#include "subcon_store.hpp"

#include <fstream>

namespace {
    // A file that exists but won't parse is moved aside (path + ".corrupt")
    // instead of being treated as absent and overwritten by the next save.
    void set_aside_corrupt_file(const std::filesystem::path& path, const SUBCON_NOTE_FN& note)
    {
        std::filesystem::path aside = path;
        aside += ".corrupt";
        std::error_code ec;
        std::filesystem::rename(path, aside, ec);
        note("couldn't read " + path.filename().string() + " - moved it aside as " + aside.filename().string() + " and starting fresh");
    }

    // Same UTF-8 lesson as the old subcon (found live 2026-09-26, three
    // SIGABRTs): dump()'s default error_handler_t::strict throws on invalid
    // UTF-8, and note content / findings are built from live, free-form
    // model output. 'replace' swaps bad bytes for U+FFFD instead of
    // throwing; the try/catch in each save below is defense-in-depth on top,
    // not a substitute - losing one saved note to a write hiccup is far
    // smaller than losing the process.
    std::string dump_safely(const json& j)
    {
        return j.dump(2, ' ', false, json::error_handler_t::replace);
    }

    // Tasks dropped by subcon_load_todo_items()'s reconcile are appended here
    // rather than discarded - they carry real history (what was found, and
    // when). Never read back by subcon itself, only there for a person.
    void archive_retired_tasks(const std::filesystem::path& path, const std::vector<SUBCON_TODO_ITEM>& retired, const SUBCON_NOTE_FN& note)
    {
        if (retired.empty()) return;

        json archive = json::array();
        {
            std::ifstream existing(path);
            if (existing)
            {
                bool readable = true;
                try { existing >> archive; }
                catch (const std::exception&) { readable = false; }
                if (!readable || !archive.is_array())
                {
                    existing.close();
                    set_aside_corrupt_file(path, note);
                    archive = json::array();
                }
            }
        }
        for (const SUBCON_TODO_ITEM& item : retired) archive.push_back(item);

        try
        {
            if (!subcon_write_file_atomically(path, dump_safely(archive)))
                note("failed to write " + path.filename().string());
        }
        catch (const std::exception& e)
        {
            note(std::string("failed to archive retired tasks: ") + e.what());
        }
    }
}

bool subcon_write_file_atomically(const std::filesystem::path& path, const std::string& text)
{
    std::filesystem::path temp_path = path;
    temp_path += ".tmp";
    {
        std::ofstream file(temp_path, std::ios::trunc);
        if (!file) return false;
        file << text;
        file.flush();
        if (!file) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temp_path, path, ec);
    return !ec;
}

std::vector<SUBCON_NOTE> subcon_load_queue(const std::filesystem::path& path, const SUBCON_NOTE_FN& note)
{
    std::vector<SUBCON_NOTE> queue;
    std::ifstream file(path);
    if (!file) return queue;

    try
    {
        json data;
        file >> data;
        queue = data.get<std::vector<SUBCON_NOTE>>();
    }
    catch (const std::exception&)
    {
        queue.clear();
        file.close();
        set_aside_corrupt_file(path, note);
    }
    return queue;
}

void subcon_save_queue(const std::filesystem::path& path, const std::vector<SUBCON_NOTE>& queue, const SUBCON_NOTE_FN& note)
{
    try
    {
        if (!subcon_write_file_atomically(path, dump_safely(json(queue))))
            note("failed to write " + path.filename().string());
    }
    catch (const std::exception& e)
    {
        note(std::string("failed to save queue: ") + e.what());
    }
}

std::vector<SUBCON_TODO_ITEM> subcon_load_todo_items(const std::filesystem::path& path, const std::filesystem::path& retired_path, const SUBCON_NOTE_FN& note)
{
    std::vector<SUBCON_TODO_ITEM> loaded;
    {
        std::ifstream file(path);
        if (file)
        {
            try
            {
                json data;
                file >> data;
                loaded = data.get<std::vector<SUBCON_TODO_ITEM>>();
            }
            catch (const std::exception&)
            {
                // Treated as a first run - but kept aside, not overwritten:
                // it holds every task's run times and history.
                loaded.clear();
                file.close();
                set_aside_corrupt_file(path, note);
            }
        }
    }

    std::vector<SUBCON_TODO_ITEM> retired;
    std::vector<SUBCON_TODO_ITEM> items = reconcile_subcon_tasks(loaded, subcon_task_defs(), retired);
    if (!retired.empty())
    {
        archive_retired_tasks(retired_path, retired, note);
        note("retired " + std::to_string(retired.size()) + " old task(s) to " + retired_path.filename().string());
    }
    return items;
}

void subcon_save_todo_items(const std::filesystem::path& path, const std::vector<SUBCON_TODO_ITEM>& items, const SUBCON_NOTE_FN& note)
{
    try
    {
        if (!subcon_write_file_atomically(path, dump_safely(json(items))))
            note("failed to write " + path.filename().string());
    }
    catch (const std::exception& e)
    {
        note(std::string("failed to save todo items: ") + e.what());
    }
}
