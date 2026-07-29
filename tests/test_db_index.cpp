/// @file
/// @brief Tests for the read-only gw2index SQLite accessor.
///
/// The fixture is a real SQLite file built here with the same schema
/// `gw2index` writes, so the queries under test run against real SQL rather
/// than a stub. Two schema variants are exercised: the current one with an
/// `uncompressed_size` column, and the older one that only has `size_final` --
/// the accessor has to work with both, because indexes built months apart are
/// still on disk.

#include "test_framework.h"

#include "castlemist/db/index_db.h"

#include "sqlite3.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

/// @brief A SQLite file that deletes itself when the test ends.
class TempDb {
public:
    explicit TempDb(const char* tag) {
        char buf[MAX_PATH_LEN];
        std::snprintf(buf, sizeof buf, "gw2b_test_index_%s.db", tag);
        path_ = buf;
        std::remove(path_.c_str());
    }
    ~TempDb() {
        castlemist::db::close();
        std::remove(path_.c_str());
    }

    const std::string& path() const { return path_; }
    std::wstring wpath() const { return std::wstring(path_.begin(), path_.end()); }

    /// @brief Run a batch of statements, failing the test if SQLite rejects any.
    void exec(const char* sql) {
        sqlite3* db = nullptr;
        if (sqlite3_open(path_.c_str(), &db) != SQLITE_OK) {
            sqlite3_close(db);
            ::castlemist::test::fail(__FILE__, __LINE__, "could not create " + path_);
        }
        char* err = nullptr;
        int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
        std::string message = err ? err : "";
        sqlite3_free(err);
        sqlite3_close(db);
        if (rc != SQLITE_OK) ::castlemist::test::fail(__FILE__, __LINE__, "sqlite: " + message);
    }

private:
    static constexpr size_t MAX_PATH_LEN = 260;
    std::string path_;
};

/// The schema `gw2index` writes, minus the columns nothing here reads.
const char* kSchema = R"(
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE entries (
    base_id INTEGER PRIMARY KEY, type TEXT, container TEXT, error TEXT, magic TEXT,
    size INTEGER, size_stripped INTEGER, size_final INTEGER,
    uncompressed_size INTEGER, comp_flag INTEGER, actually_compressed INTEGER);
CREATE TABLE file_ids (file_id INTEGER, base_id INTEGER);
CREATE TABLE chunks (base_id INTEGER, seq INTEGER, fourcc TEXT, version INTEGER, struct_variant TEXT);

INSERT INTO meta VALUES ('dat_path', 'C:\Games\Gw2.dat');
INSERT INTO entries VALUES (100,'packfile','MODL','','PF',  1000, 990, 4000, 4000, 8, 1);
INSERT INTO entries VALUES (101,'texture', 'ATEX','','ATEX', 2000,1990, 8000, 8000, 8, 1);
INSERT INTO entries VALUES (102,'packfile','cntc','','PF',  3000,2990,12000,12000, 0, 0);
INSERT INTO entries VALUES (103,'texture', '',    '','ATEX', 500, 490, 1000, 1000, 8, 1);

INSERT INTO file_ids VALUES (2440959, 100);
INSERT INTO chunks VALUES (100, 0, 'GEOM', 3, 'ModelFileGeometryV3');
INSERT INTO chunks VALUES (100, 1, 'MODL', 65, 'ModelFileDataV65');
)";

} // namespace

CM_TEST(index_db, starts_closed) {
    castlemist::db::close();
    CHECK_FALSE(castlemist::db::is_open());
    CHECK_EQ(castlemist::db::entry_count(), size_t{0});
    CHECK(castlemist::db::types().empty());
}

CM_TEST(index_db, opening_a_missing_file_reports_an_error) {
    std::string err;
    CHECK_FALSE(castlemist::db::open(L"no_such_index_file_12345.db", err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(castlemist::db::is_open());
}

CM_TEST(index_db, reads_metadata_and_counts) {
    TempDb db("meta");
    db.exec(kSchema);

    std::string err;
    CHECK(castlemist::db::open(db.wpath(), err));
    CHECK(castlemist::db::is_open());
    CHECK_EQ(castlemist::db::entry_count(), size_t{4});
    CHECK_EQ(castlemist::db::dat_path(), std::wstring(L"C:\\Games\\Gw2.dat"));
}

CM_TEST(index_db, lists_distinct_types_and_containers) {
    TempDb db("distinct");
    db.exec(kSchema);
    std::string err;
    CHECK(castlemist::db::open(db.wpath(), err));

    auto types = castlemist::db::types();
    CHECK_EQ(types.size(), size_t{2});
    CHECK_EQ(types[0], std::string("packfile")); // sorted
    CHECK_EQ(types[1], std::string("texture"));

    // The empty container of base 103 must not become a blank filter entry.
    auto containers = castlemist::db::containers();
    CHECK_EQ(containers.size(), size_t{3});
    CHECK_EQ(containers[0], std::string("ATEX"));
}

CM_TEST(index_db, filters_by_type_and_container) {
    TempDb db("filter");
    db.exec(kSchema);
    std::string err;
    CHECK(castlemist::db::open(db.wpath(), err));

    auto all = castlemist::db::query_base_ids("", "", 0, false, false, 100);
    CHECK_EQ(all.size(), size_t{4});

    auto packfiles = castlemist::db::query_base_ids("packfile", "", 0, false, false, 100);
    CHECK_EQ(packfiles.size(), size_t{2});

    auto modl = castlemist::db::query_base_ids("packfile", "MODL", 0, false, false, 100);
    CHECK_EQ(modl.size(), size_t{1});
    CHECK_EQ(modl[0], 100u);

    auto none = castlemist::db::query_base_ids("texture", "MODL", 0, false, false, 100);
    CHECK(none.empty());
}

CM_TEST(index_db, respects_the_row_limit) {
    TempDb db("limit");
    db.exec(kSchema);
    std::string err;
    CHECK(castlemist::db::open(db.wpath(), err));
    CHECK_EQ(castlemist::db::query_base_ids("", "", 0, false, false, 2).size(), size_t{2});
}

// A fileId is not a baseId: it has to be resolved through the file_ids table.
CM_TEST(index_db, looks_up_by_base_id_and_by_file_id) {
    TempDb db("byid");
    db.exec(kSchema);
    std::string err;
    CHECK(castlemist::db::open(db.wpath(), err));

    auto by_base = castlemist::db::query_base_ids("", "", 101, false, true, 10);
    CHECK_EQ(by_base.size(), size_t{1});
    CHECK_EQ(by_base[0], 101u);

    auto by_file = castlemist::db::query_base_ids("", "", 2440959, true, true, 10);
    CHECK_EQ(by_file.size(), size_t{1});
    CHECK_EQ(by_file[0], 100u);

    CHECK(castlemist::db::query_base_ids("", "", 999999, true, true, 10).empty());
}

CM_TEST(index_db, lookup_returns_sizes_flags_and_chunks) {
    TempDb db("lookup");
    db.exec(kSchema);
    std::string err;
    CHECK(castlemist::db::open(db.wpath(), err));

    castlemist::db::EntryInfo info = castlemist::db::lookup(100);
    CHECK(info.found);
    CHECK_EQ(info.type, std::string("packfile"));
    CHECK_EQ(info.container, std::string("MODL"));
    CHECK_EQ(info.size, 1000LL);
    CHECK_EQ(info.size_final, 4000LL);
    CHECK_EQ(info.comp_flag, 8);
    CHECK_EQ(info.actually_compressed, 1);
    CHECK_EQ(info.chunks.size(), size_t{2});
    CHECK(info.chunks[0].find("GEOM") != std::string::npos);

    CHECK_FALSE(castlemist::db::lookup(4242).found);
}

CM_TEST(index_db, streams_the_metadata_and_size_maps) {
    TempDb db("maps");
    db.exec(kSchema);
    std::string err;
    CHECK(castlemist::db::open(db.wpath(), err));

    int rows = 0;
    castlemist::db::load_meta_map([&](uint32_t, const char*, const char*) { ++rows; });
    CHECK_EQ(rows, 4);

    long long total = 0;
    castlemist::db::load_size_map([&](uint32_t, long long size) { total += size; });
    CHECK_EQ(total, 25000LL);
}

// Older indexes have no `uncompressed_size` column; the accessor must fall back
// to `size_final` rather than failing every size query.
CM_TEST(index_db, works_on_an_index_without_the_uncompressed_size_column) {
    TempDb db("legacy");
    db.exec(R"(
CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE entries (
    base_id INTEGER PRIMARY KEY, type TEXT, container TEXT, error TEXT, magic TEXT,
    size INTEGER, size_stripped INTEGER, size_final INTEGER,
    comp_flag INTEGER, actually_compressed INTEGER);
CREATE TABLE file_ids (file_id INTEGER, base_id INTEGER);
CREATE TABLE chunks (base_id INTEGER, seq INTEGER, fourcc TEXT, version INTEGER, struct_variant TEXT);
INSERT INTO meta VALUES ('dat_path','D:\old.dat');
INSERT INTO entries VALUES (7,'texture','ATEX','','ATEX',10,9,777,8,1);
)");
    std::string err;
    CHECK(castlemist::db::open(db.wpath(), err));
    CHECK_EQ(castlemist::db::lookup(7).size_final, 777LL);

    long long total = 0;
    castlemist::db::load_size_map([&](uint32_t, long long size) { total += size; });
    CHECK_EQ(total, 777LL);
}

// ---------------------------------------------------------------------------
// The real index, when one is present. Everything above proves the SQL is
// self-consistent; this proves it matches what `gw2index` actually writes.
// ---------------------------------------------------------------------------

namespace {

/// @brief Open the workspace's real index, or skip.
/// @see docs/generating-data.md for how gw2_index.db is produced.
void open_real_index() {
    const char* env = std::getenv("GW2_TEST_INDEX_DB");
    std::string err;
    if (env && *env) {
        std::string path(env);
        if (castlemist::db::open(std::wstring(path.begin(), path.end()), err)) return;
        SKIP("GW2_TEST_INDEX_DB did not open: " + err);
    }
    // The test binary runs from build/<preset>/bin/, so three levels up is the
    // repository root and dumps/index/ is where tools/gw2index writes.
    for (const wchar_t* candidate : {
             L"../../../dumps/index/gw2_index.db",
             L"dumps/index/gw2_index.db",
             L"../dumps/index/gw2_index.db",
         }) {
        if (castlemist::db::open(candidate, err)) return;
    }
    SKIP("no gw2_index.db (set GW2_TEST_INDEX_DB)");
}

} // namespace

CM_TEST(real_index, matches_the_schema_the_indexer_writes) {
    open_real_index();
    CHECK(castlemist::db::is_open());

    // A full Gw2.dat index holds hundreds of thousands of entries.
    CHECK(castlemist::db::entry_count() > 100000);
    CHECK_FALSE(castlemist::db::dat_path().empty());

    auto types = castlemist::db::types();
    CHECK_FALSE(types.empty());

    auto containers = castlemist::db::containers();
    CHECK_FALSE(containers.empty());
    bool has_cntc = false;
    for (const std::string& c : containers) has_cntc = has_cntc || c == "cntc";
    CHECK(has_cntc);

    castlemist::db::close();
}

// The cntc entries the extract tests use must actually be indexed as cntc.
CM_TEST(real_index, finds_the_cntc_packfiles) {
    open_real_index();
    auto cntc = castlemist::db::query_base_ids("packfile", "cntc", 0, false, false, 100);
    CHECK_FALSE(cntc.empty());

    castlemist::db::EntryInfo info = castlemist::db::lookup(cntc[0]);
    CHECK(info.found);
    CHECK_EQ(info.container, std::string("cntc"));
    CHECK(info.size_final > info.size);   // a cntc is heavily compressed
    castlemist::db::close();
}

CM_TEST(index_db, close_is_idempotent) {
    TempDb db("close");
    db.exec(kSchema);
    std::string err;
    CHECK(castlemist::db::open(db.wpath(), err));
    castlemist::db::close();
    castlemist::db::close();
    CHECK_FALSE(castlemist::db::is_open());
}
