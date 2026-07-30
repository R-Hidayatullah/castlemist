/// @file
/// @brief Builds the gw2index SQLite database from a .dat archive.
///
/// Same indexer the `gw2index` tool runs -- lifted out of it so the app can build
/// an index itself instead of telling the user to go find a command line. The tool
/// is now a thin wrapper over this, so there is one implementation to keep correct.
///
/// A cold run walks every MFT entry, decompresses each far enough to learn its real
/// type, and records sizes, verified compression truth, sniffed type/container and
/// the packfile chunk list. On a retail archive that is ~800k entries and takes a
/// while, so the build is multithreaded, resumable, and cancellable.

#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>

namespace castlemist::db {

/// @brief What to index and how hard to work at it.
struct BuildOptions {
    std::string dat_path;       ///< Archive to read.
    std::string db_path;        ///< SQLite file to create or update.
    std::string template_path;  ///< gw2_packfile.json; empty = index without struct variants.
    int threads = 0;            ///< 0 = hardware_concurrency.
    /// Re-index every entry instead of skipping ones whose fingerprint is unchanged.
    /// A normal re-run after a game patch only touches what actually moved.
    bool full = false;
};

/// @brief How far along a build is. Counts are cumulative and only grow.
struct BuildProgress {
    size_t total = 0;      ///< MFT entries in the archive.
    size_t processed = 0;  ///< Entries read and classified this run.
    size_t skipped = 0;    ///< Entries whose fingerprint matched, so nothing was re-read.
    size_t written = 0;    ///< Rows committed so far.
    bool loading_mft = false;  ///< True during the initial MFT load, before totals are known.
};

/// @brief Build or refresh an index.
///
/// Blocking -- run it on a worker thread if the caller has a UI to keep alive.
///
/// @param opt         What to build.
/// @param on_progress Called periodically from the writer thread, never from the
///                    workers. May be empty. Do not touch UI directly from it:
///                    marshal to the UI thread.
/// @param cancel      Polled by the workers and the writer; when it turns true the
///                    build stops early and commits what it already has, leaving a
///                    valid (partial) database that a later run can resume from.
///                    May be null.
/// @param err         Set on failure.
/// @return True on success, including a clean cancellation.
bool build_index(const BuildOptions& opt,
                 const std::function<void(const BuildProgress&)>& on_progress,
                 const std::atomic<bool>* cancel,
                 std::string& err);

} // namespace castlemist::db
