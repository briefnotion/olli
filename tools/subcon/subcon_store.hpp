// Subcon's persisted state: the task list (todo_items.json), the note queue
// (queue.json) and the retired-task archive. Ported from the old in-olli
// subcon (source/subcon_worker.cpp) with the same on-disk formats, so an
// existing profile's files load unchanged. Plain functions, no logging
// dependency: failures are reported through the 'note' callback so the
// caller decides where they go (the cycle log).

#pragma once

#include "subcon_tasks.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using SUBCON_NOTE_FN = std::function<void(const std::string&)>;

// Writes a file so a crash or kill mid-write can't leave it half-written:
// the full text goes to a sibling temp file first, then rename() swaps it
// into place (atomic on the same filesystem). Found in review 2026-10-28
// (old subcon): truncating and rewriting in place meant an interrupted
// write left a file that then failed to parse, which load treats as a
// first run - silently resetting every task's run cap and losing the
// history. Returns false if anything went wrong.
bool subcon_write_file_atomically(const std::filesystem::path& path, const std::string& text);

// Loads the persisted queue. A missing file (first run) is an empty queue;
// a file that exists but won't parse is moved aside as <name>.corrupt (so it
// can still be recovered by hand) and the queue starts empty.
std::vector<SUBCON_NOTE> subcon_load_queue(const std::filesystem::path& path, const SUBCON_NOTE_FN& note);
void subcon_save_queue(const std::filesystem::path& path, const std::vector<SUBCON_NOTE>& queue, const SUBCON_NOTE_FN& note);

// Loads the task list and merges it with the definitions in code
// (subcon_task_defs()): state survives for tasks that still exist, tasks
// removed from the definitions are appended to retired_path instead of being
// lost. A missing or unreadable file starts from the definitions alone - an
// empty list would leave the reasoning loop with nothing to prioritize.
std::vector<SUBCON_TODO_ITEM> subcon_load_todo_items(const std::filesystem::path& path, const std::filesystem::path& retired_path, const SUBCON_NOTE_FN& note);
void subcon_save_todo_items(const std::filesystem::path& path, const std::vector<SUBCON_TODO_ITEM>& items, const SUBCON_NOTE_FN& note);
