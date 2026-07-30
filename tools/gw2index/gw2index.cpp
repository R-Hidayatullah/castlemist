// gw2index -- fast, resumable, multithreaded indexer for a GW2 .dat archive.
//
// For every MFT entry it records: baseId, all fileIds, the raw MFT header (offset,
// size, compression flag, CRC, MFT-declared uncompressed size), the *verified*
// compression truth (some entries are flagged compressed but are not), the sizes
// before/after CRC32-strip and decompression, the sniffed type/container, and -- for
// packfiles -- the list of chunks (fourcc + version + the template struct variant that
// applies). No raw file data is stored. Output is a queryable SQLite database.
//
// Resumable: each entry's fingerprint (offset|size|crc|flag) is stored; a re-run skips
// unchanged entries and only re-processes patched/new ones.
//
// The indexer itself lives in castlemist::db::build_index so the app can run it too
// (File > Build Index DB...). This is the command-line face of it -- argument
// parsing and progress printing, nothing more.
//
// Usage: gw2index --dat <path> --out <index.db> [--template <gw2_packfile.json>]
//                 [--threads N] [--full]     (--full ignores fingerprints, re-does all)

#include "castlemist/db/index_builder.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    castlemist::db::BuildOptions opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (a == "--dat") opt.dat_path = next();
        else if (a == "--out") opt.db_path = next();
        else if (a == "--template") opt.template_path = next();
        else if (a == "--threads") opt.threads = std::atoi(next().c_str());
        else if (a == "--full") opt.full = true;
    }
    if (opt.dat_path.empty() || opt.db_path.empty()) {
        std::fprintf(stderr, "usage: gw2index --dat <path> --out <index.db> [--template j] [--threads N] [--full]\n");
        return 2;
    }

    bool announced = false;
    auto on_progress = [&](const castlemist::db::BuildProgress& p) {
        if (p.loading_mft) { std::fprintf(stderr, "loading MFT from %s ...\n", opt.dat_path.c_str()); return; }
        if (!announced) { std::fprintf(stderr, "MFT entries: %zu\n", p.total); announced = true; }
        std::fprintf(stderr, "  ...%zu written (processed %zu, skipped %zu of %zu)\n",
                     p.written, p.processed, p.skipped, p.total);
    };

    std::string err;
    if (!castlemist::db::build_index(opt, on_progress, nullptr, err)) {
        std::fprintf(stderr, "gw2index: %s\n", err.c_str());
        return 1;
    }
    std::fprintf(stderr, "done -> %s\n", opt.db_path.c_str());
    return 0;
}
