// rag_sync - the "sync collection folders into the database" logic shared
// by rag_admin (menu-driven, on request) and rag_tool (automatic, on a
// timer). Moved out of rag_admin.cpp so both can call the exact same code
// instead of drifting apart - see sync_profile_collections()'s own comment
// below for what it actually does.

#pragma once

#include <string>

#include "rag_db.hpp"
#include "rag_embed.hpp"

// Aggregate result of one sync_profile_collections() run. Printing/
// displaying these is deliberately left to the caller - rag_admin shows
// them as a one-line human-readable summary; rag_tool folds them into its
// own status display.
struct RAG_SYNC_STATS {
    int imported = 0;
    int updated = 0;
    int unchanged = 0;
    int removed = 0;

    // True if another process already held the sync lock for this profile
    // and this call didn't run at all (every count above stays 0) - see
    // sync_profile_collections()'s own comment for the file lock this
    // reports on.
    bool skipped_busy = false;
};

// Syncs everything for one profile against its on-disk folders: the
// special "conversations" collection against profile_chat_logs_dir() (if
// that directory exists), then every other collection against its own
// profile_collection_dir() - the same three-way diff (new file -> import,
// unchanged content -> skip, changed content -> delete and reimport,
// source file gone -> delete the document) rag_admin's "Update database"
// menu option has always done.
//
// Guarded by a non-blocking advisory file lock
// (profile_sync_lock_path(profile_name)) for the whole call: if another
// process (rag_admin running this by hand, or another rag_tool) already
// holds it, this returns immediately with skipped_busy set rather than
// waiting - simpler and safer than blocking a process that needs to stay
// responsive (rag_tool's wire protocol) or than risking two processes
// racing the same import.
//
// verbose (default true - rag_admin's plain-terminal CLI wants this
// unchanged) prints a per-file progress line ("foo.txt: unchanged,
// skipping", etc.) straight to std::cout as it goes. rag_tool passes false:
// its terminal is ncurses-owned (see olli_display.hpp), and a raw stdout
// write in the middle of an ncurses redraw cycle corrupts the display -
// ncurses has no idea the write happened, so its next redraw repositions
// the cursor based on a screen state that's now wrong (confirmed live: a
// sync produced pages of text scattered at random screen positions).
// rag_tool already reports the same per-file information back to olli as
// this function's summary result and its own activity-area line (see its
// main()) instead.
RAG_SYNC_STATS sync_profile_collections(RAG_DB& db, RAG_EMBEDDER& embedder, const std::string& profile_name, bool verbose = true);
