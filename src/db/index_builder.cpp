/// @file
/// @brief Implementation of the gw2index builder.
///
/// Workers read+decompress+classify entries into a queue; one writer commits them
/// to SQLite in batches. Splitting it that way keeps SQLite single-threaded (it is
/// much happier that way) while the expensive part -- decompressing ~800k entries
/// far enough to sniff their type -- scales across cores.

#include "castlemist/db/index_builder.h"

#include "sqlite3.h"

#include "castlemist/native/gw2dat.h"
#include "castlemist/native/cmp_decompress_method0.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <queue>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

using nlohmann::json;

namespace castlemist::db {
namespace {

struct ChunkInfo { std::string fourcc; int version; std::string variant; uint32_t size; };

struct EntryResult {
    uint32_t base_id = 0;
    uint64_t offset = 0;
    uint32_t size = 0;              // on-disk (stored) size
    uint16_t comp_flag = 0;
    uint32_t crc = 0;
    uint32_t mft_usize = 0;         // MFT-declared uncompressed size
    int      actually_compressed = 0;
    uint32_t size_stripped = 0;     // after CRC32 strip
    uint32_t size_final = 0;        // after decompress (== stripped if not compressed)
    std::string magic, type, container, error, fingerprint;
    std::vector<uint32_t> file_ids;
    std::vector<ChunkInfo> chunks;
};

std::string fourcc_str(const uint8_t* b) {
    std::string s;
    for (int i = 0; i < 4; ++i) s.push_back((b[i] >= 32 && b[i] < 127) ? (char)b[i] : '.');
    return s;
}

// Sniff the decompressed bytes into a coarse type + (for packfiles) container fourcc.
void classify(const std::vector<uint8_t>& d, EntryResult& r) {
    if (d.size() >= 4) r.magic = fourcc_str(d.data());
    if (d.size() < 4) { r.type = "empty"; return; }
    auto eq = [&](size_t off, const char* m, size_t n) {
        return d.size() >= off + n && std::memcmp(d.data() + off, m, n) == 0;
    };
    if (d[0] == 'P' && d[1] == 'F') {
        r.type = "packfile";
        if (d.size() >= 12) r.container = fourcc_str(d.data() + 8);
        return;
    }
    // The ArenaNet texture family. The C-prefixed variants are listed alongside the
    // A-prefixed ones because the archive really does carry them -- there is a CTEU
    // entry in a retail dat that would otherwise be filed as "binary".
    static const char* atex[] = {"ATEX", "ATTX", "ATEC", "ATEP", "ATEU", "ATET",
                                 "CTEX", "CTTX", "CTEC", "CTEP", "CTEU", "CTET"};
    for (const char* m : atex) if (eq(0, m, 4)) { r.type = "texture"; return; }
    if (eq(0, "DDS ", 4)) { r.type = "dds"; return; }
    if (eq(0, "strs", 4)) { r.type = "strs"; return; }
    if (eq(0, "RIFF", 4)) { r.type = "riff"; return; }
    if (eq(0, "\x89PNG", 4)) { r.type = "png"; return; }
    if (d[0] == 0xFF && d[1] == 0xD8) { r.type = "jpeg"; return; }
    if (eq(0, "MZ", 2)) { r.type = "exe"; return; }
    if (eq(0, "asnd", 4)) { r.type = "asnd"; return; }
    r.type = "binary";
}

// Resolve a chunk's struct variant from the template: container-specific
// fileTypes[container][fourcc][version], else global chunks[fourcc][version].
std::string resolve_variant(const json& tpl, const std::string& container,
                            const std::string& fourcc, int version) {
    const std::string vs = std::to_string(version);
    auto ft = tpl.find("fileTypes");
    if (ft != tpl.end()) {
        auto c = ft->find(container);
        if (c != ft->end()) {
            auto f = c->find(fourcc);
            if (f != c->end()) { auto v = f->find(vs); if (v != f->end()) return v->get<std::string>(); }
        }
    }
    auto ch = tpl.find("chunks");
    if (ch != tpl.end()) {
        auto f = ch->find(fourcc);
        if (f != ch->end()) { auto v = f->find(vs); if (v != f->end()) return v->get<std::string>();
            if (!f->empty()) return "?v" + vs; } // fourcc known, version not mapped
    }
    return ""; // unknown chunk
}

// Walk a packfile's chunk table: fourcc + version (rd16 at chunk-data start) + variant.
void parse_chunks(const std::vector<uint8_t>& d, const json& tpl, EntryResult& r) {
    if (d.size() < 12) return;
    auto rd16 = [&](size_t p) -> uint32_t { return (p + 2 <= d.size()) ? (d[p] | (d[p + 1] << 8)) : 0; };
    auto rd32 = [&](size_t p) -> uint32_t {
        return (p + 4 <= d.size()) ? (d[p] | (d[p + 1] << 8) | (d[p + 2] << 16) | ((uint32_t)d[p + 3] << 24)) : 0;
    };
    size_t pos = rd16(6); // headerSize
    int guard = 0;
    while (pos + 8 <= d.size() && guard++ < 4096) {
        std::string fourcc = fourcc_str(d.data() + pos);
        uint32_t chunk_size = rd32(pos + 4);
        int version = (int)rd16(pos + 8);           // chunk data starts here; first field = version
        r.chunks.push_back({fourcc, version, resolve_variant(tpl, r.container, fourcc, version), chunk_size});
        size_t next = pos + 8 + chunk_size;
        if (next <= pos) break;
        pos = next;
    }
}

std::string fingerprint_of(const MftData& e) {
    char fp[96];
    std::snprintf(fp, sizeof fp, "%llu|%u|%u|%u",
                  (unsigned long long)e.offset, e.size, e.crc, e.compression_flag);
    return fp;
}

void process_entry(const std::string& dat_path, const MftData& e, uint32_t base_id,
                   const json& tpl, EntryResult& r) {
    r.base_id = base_id;
    r.offset = e.offset; r.size = e.size; r.comp_flag = e.compression_flag;
    r.crc = e.crc; r.mft_usize = e.uncompressed_size;
    r.fingerprint = fingerprint_of(e);
    if (e.size == 0) { r.type = "empty"; return; }

    std::vector<uint8_t> raw;
    try { raw = read_entry_bytes(dat_path, e); }
    catch (const std::exception& ex) { r.error = std::string("read: ") + ex.what(); return; }

    std::vector<uint8_t> stripped = castlemist::cmp::strip_crc32(std::span<const uint8_t>(raw));
    r.size_stripped = (uint32_t)stripped.size();

    std::vector<uint8_t> final_bytes;
    if (e.compression_flag != 0 && stripped.size() >= 8) {
        uint32_t usize = stripped[4] | (stripped[5] << 8) | (stripped[6] << 16) | ((uint32_t)stripped[7] << 24);
        try {
            final_bytes = castlemist::cmp::decompress_method0(std::span<const uint8_t>(stripped).subspan(8), usize);
            r.actually_compressed = 1;
        } catch (const std::exception&) {
            // Flagged compressed but the payload is not a valid Method0 stream.
            r.actually_compressed = 0;
            r.error = "flagged-compressed-but-not";
            final_bytes = stripped; // index what it actually is
        }
    } else {
        final_bytes = std::move(stripped);
    }
    r.size_final = (uint32_t)final_bytes.size();

    classify(final_bytes, r);
    if (r.type == "packfile") parse_chunks(final_bytes, tpl, r);
}

bool exec(sqlite3* db, const char* sql, std::string& err) {
    char* msg = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
        err = msg ? msg : "sqlite error";
        sqlite3_free(msg);
        return false;
    }
    sqlite3_free(msg);
    return true;
}

bool init_schema(sqlite3* db, std::string& err) {
    if (!exec(db,
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;"
        "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
        "CREATE TABLE IF NOT EXISTS entries("
        " base_id INTEGER PRIMARY KEY, offset INTEGER, size INTEGER, comp_flag INTEGER, crc INTEGER,"
        " mft_usize INTEGER, fingerprint TEXT, actually_compressed INTEGER,"
        " size_stripped INTEGER, size_final INTEGER, uncompressed_size INTEGER,"
        " magic TEXT, type TEXT, container TEXT, error TEXT);"
        "CREATE TABLE IF NOT EXISTS file_ids(file_id INTEGER PRIMARY KEY, base_id INTEGER);"
        "CREATE TABLE IF NOT EXISTS chunks(base_id INTEGER, seq INTEGER, fourcc TEXT, version INTEGER,"
        " struct_variant TEXT, chunk_size INTEGER);"
        "CREATE INDEX IF NOT EXISTS ix_fileids_base ON file_ids(base_id);"
        "CREATE INDEX IF NOT EXISTS ix_chunks_base ON chunks(base_id);"
        "CREATE INDEX IF NOT EXISTS ix_chunks_fourcc ON chunks(fourcc);"
        "CREATE INDEX IF NOT EXISTS ix_entries_type ON entries(type);"
        "CREATE INDEX IF NOT EXISTS ix_entries_container ON entries(container);", err))
        return false;

    // Migrate a DB written before `uncompressed_size` existed. CREATE TABLE IF NOT
    // EXISTS leaves such a table alone, so without this the writer's INSERT fails to
    // PREPARE and every row is silently dropped while the run still reports success
    // -- which is how a re-index can quietly delete entries.
    bool has_usize = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(entries)", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* col = sqlite3_column_text(st, 1);
            if (col && std::strcmp((const char*)col, "uncompressed_size") == 0) has_usize = true;
        }
    }
    sqlite3_finalize(st);
    if (!has_usize)
        return exec(db, "ALTER TABLE entries ADD COLUMN uncompressed_size INTEGER;"
                        "UPDATE entries SET uncompressed_size = size_final;", err);
    return true;
}

struct Queue {
    std::mutex m; std::condition_variable cv;
    std::queue<EntryResult> q; bool done = false;
    void push(EntryResult&& r) { { std::lock_guard<std::mutex> l(m); q.push(std::move(r)); } cv.notify_one(); }
    bool pop(EntryResult& out) {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return !q.empty() || done; });
        if (q.empty()) return false;
        out = std::move(q.front()); q.pop(); return true;
    }
    void finish() { { std::lock_guard<std::mutex> l(m); done = true; } cv.notify_all(); }
};

} // namespace

bool build_index(const BuildOptions& opt,
                 const std::function<void(const BuildProgress&)>& on_progress,
                 const std::atomic<bool>* cancel,
                 std::string& err) {
    auto cancelled = [&] { return cancel && cancel->load(); };
    auto report = [&](const BuildProgress& p) { if (on_progress) on_progress(p); };

    if (opt.dat_path.empty() || opt.db_path.empty()) {
        err = "both a .dat and an output .db path are required";
        return false;
    }

    json tpl = json::object();
    if (!opt.template_path.empty()) {
        std::ifstream t(opt.template_path, std::ios::binary);
        if (!t) { err = "cannot open struct template: " + opt.template_path; return false; }
        try { t >> tpl; }
        catch (const std::exception& ex) { err = std::string("struct template JSON: ") + ex.what(); return false; }
    }

    {
        BuildProgress p; p.loading_mft = true;
        report(p);
    }
    Gw2Dat dat;
    try { load_dat_file(dat, opt.dat_path); }
    catch (const std::exception& ex) { err = std::string("cannot load archive: ") + ex.what(); return false; }
    const size_t N = dat.mft_data_list.size();

    // baseId (1-based MFT index) -> fileIds.
    std::unordered_map<uint32_t, std::vector<uint32_t>> base_to_files;
    for (auto& b : dat.mft_base_id_data_list) base_to_files[b.base_id] = b.file_id;

    sqlite3* db = nullptr;
    if (sqlite3_open(opt.db_path.c_str(), &db) != SQLITE_OK) {
        err = "cannot open database: " + opt.db_path;
        if (db) sqlite3_close(db);
        return false;
    }
    if (!init_schema(db, err)) { sqlite3_close(db); return false; }

    // Resumable: load existing fingerprints unless a full rebuild was asked for.
    std::unordered_map<uint32_t, std::string> existing;
    if (!opt.full) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT base_id, fingerprint FROM entries", -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const unsigned char* fp = sqlite3_column_text(st, 1);
                existing[(uint32_t)sqlite3_column_int64(st, 0)] = fp ? reinterpret_cast<const char*>(fp) : "";
            }
        }
        sqlite3_finalize(st);
    }

    const int nthreads = opt.threads > 0
                       ? opt.threads
                       : (int)std::max(2u, std::thread::hardware_concurrency());

    Queue queue;
    std::atomic<size_t> cursor{0}, processed{0}, skipped{0};
    auto worker = [&]() {
        for (;;) {
            if (cancelled()) break;
            size_t i = cursor.fetch_add(1);
            if (i >= N) break;
            const MftData& e = dat.mft_data_list[i];
            uint32_t base_id = (uint32_t)(i + 1);
            std::string fp = fingerprint_of(e);
            auto it = existing.find(base_id);
            if (it != existing.end() && it->second == fp) { skipped.fetch_add(1); continue; }
            EntryResult r;
            process_entry(opt.dat_path, e, base_id, tpl, r);
            auto f = base_to_files.find(base_id);
            if (f != base_to_files.end()) r.file_ids = f->second;
            processed.fetch_add(1);
            queue.push(std::move(r));
        }
    };
    std::vector<std::thread> pool;
    pool.reserve((size_t)nthreads);
    for (int t = 0; t < nthreads; ++t) pool.emplace_back(worker);

    // Single writer, batched transactions. A failed PREPARE used to std::exit here,
    // which is survivable in a CLI and fatal in a GUI -- record the error and stop
    // instead, so the caller can report it and stay alive.
    std::string writer_err;
    size_t written = 0;
    std::thread writer([&]() {
        sqlite3_stmt *insE = nullptr, *insF = nullptr, *insC = nullptr, *delF = nullptr, *delC = nullptr;
        auto prep = [&](const char* sql, sqlite3_stmt** out) {
            if (sqlite3_prepare_v2(db, sql, -1, out, nullptr) != SQLITE_OK) {
                if (writer_err.empty())
                    writer_err = std::string("prepare failed: ") + sqlite3_errmsg(db);
                return false;
            }
            return true;
        };
        bool ok =
            prep("INSERT OR REPLACE INTO entries(base_id,offset,size,comp_flag,crc,mft_usize,fingerprint,"
                 "actually_compressed,size_stripped,size_final,uncompressed_size,magic,type,container,error)"
                 "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", &insE) &&
            prep("INSERT OR REPLACE INTO file_ids(file_id,base_id) VALUES(?,?)", &insF) &&
            prep("INSERT INTO chunks(base_id,seq,fourcc,version,struct_variant,chunk_size) VALUES(?,?,?,?,?,?)", &insC) &&
            prep("DELETE FROM file_ids WHERE base_id=?", &delF) &&
            prep("DELETE FROM chunks WHERE base_id=?", &delC);

        if (ok) {
            std::string ignored;
            exec(db, "BEGIN", ignored);
            EntryResult r;
            while (queue.pop(r)) {
                sqlite3_reset(insE);
                sqlite3_bind_int64(insE, 1, r.base_id);   sqlite3_bind_int64(insE, 2, (sqlite3_int64)r.offset);
                sqlite3_bind_int64(insE, 3, r.size);      sqlite3_bind_int(insE, 4, r.comp_flag);
                sqlite3_bind_int64(insE, 5, r.crc);       sqlite3_bind_int64(insE, 6, r.mft_usize);
                sqlite3_bind_text(insE, 7, r.fingerprint.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(insE, 8, r.actually_compressed);
                sqlite3_bind_int64(insE, 9, r.size_stripped); sqlite3_bind_int64(insE, 10, r.size_final);
                // uncompressed_size: the canonical, unambiguously-named decompressed byte
                // count (same value as size_final, which stays for backward compat). NB
                // this is NOT mft_usize -- the MFT header's own field is 0 for every
                // compressed entry (99.4% of a retail dat), so it cannot be used here.
                sqlite3_bind_int64(insE, 11, r.size_final);
                sqlite3_bind_text(insE, 12, r.magic.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insE, 13, r.type.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insE, 14, r.container.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(insE, 15, r.error.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(insE);
                // refresh child rows for this entry
                sqlite3_reset(delF); sqlite3_bind_int64(delF, 1, r.base_id); sqlite3_step(delF);
                sqlite3_reset(delC); sqlite3_bind_int64(delC, 1, r.base_id); sqlite3_step(delC);
                for (uint32_t fid : r.file_ids) {
                    sqlite3_reset(insF); sqlite3_bind_int64(insF, 1, fid);
                    sqlite3_bind_int64(insF, 2, r.base_id); sqlite3_step(insF);
                }
                for (size_t s = 0; s < r.chunks.size(); ++s) {
                    const auto& c = r.chunks[s];
                    sqlite3_reset(insC);
                    sqlite3_bind_int64(insC, 1, r.base_id); sqlite3_bind_int(insC, 2, (int)s);
                    sqlite3_bind_text(insC, 3, c.fourcc.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(insC, 4, c.version);
                    sqlite3_bind_text(insC, 5, c.variant.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(insC, 6, c.size);
                    sqlite3_step(insC);
                }
                // Commit periodically so a cancel or a crash leaves a usable database
                // that the next run resumes from, rather than one giant lost transaction.
                if (++written % 20000 == 0) {
                    exec(db, "COMMIT", ignored);
                    exec(db, "BEGIN", ignored);
                }
                if ((written % 500) == 0) {
                    BuildProgress p;
                    p.total = N; p.processed = processed.load();
                    p.skipped = skipped.load(); p.written = written;
                    report(p);
                }
            }
            exec(db, "COMMIT", ignored);
        } else {
            // Drain so the workers are not left blocking on a full queue.
            EntryResult r;
            while (queue.pop(r)) { }
        }
        sqlite3_finalize(insE); sqlite3_finalize(insF); sqlite3_finalize(insC);
        sqlite3_finalize(delF); sqlite3_finalize(delC);
    });

    for (auto& t : pool) t.join();
    queue.finish();
    writer.join();

    if (!writer_err.empty()) { err = writer_err; sqlite3_close(db); return false; }

    // Only stamp meta on a complete run: a cancelled build has not seen every entry,
    // and claiming otherwise would make the partial index look authoritative.
    if (!cancelled()) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)", -1, &st, nullptr) == SQLITE_OK) {
            auto put = [&](const char* k, const std::string& v) {
                sqlite3_reset(st); sqlite3_bind_text(st, 1, k, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, v.c_str(), -1, SQLITE_TRANSIENT); sqlite3_step(st);
            };
            put("dat_path", opt.dat_path);
            put("dat_size", std::to_string(dat.file_info.file_size));
            put("mft_entries", std::to_string(N));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);

    BuildProgress p;
    p.total = N; p.processed = processed.load(); p.skipped = skipped.load(); p.written = written;
    report(p);
    return true;
}

} // namespace castlemist::db
