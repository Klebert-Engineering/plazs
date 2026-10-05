#include "ndsmath/wgs84.h"
#include "plazs/gazetteer.h"
#include "plazs_data/Boundary.h"
#include "zserio/SerializeUtil.h"

#include <sqlite3.h>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <memory>

namespace
{
/** Own an isolated version-1 gazetteer and remove it after all lookup handles close. */
class LocationDatabase
{
public:
    std::filesystem::path path;
    sqlite3* db = nullptr;

    /** Populate two settlements and their compact FTS vocabulary. */
    LocationDatabase()
    {
        static std::atomic<unsigned> serial{0};
        path = std::filesystem::temp_directory_path() /
            ("plazs-location-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
             std::to_string(serial++) + ".sqlite");
        REQUIRE(sqlite3_open(path.string().c_str(), &db) == SQLITE_OK);
        sql(R"sql(
            PRAGMA application_id=1347181139;
            PRAGMA user_version=1;
            CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);
            INSERT INTO metadata VALUES ('placeTypes','["continent","country","dependency","macroregion","region","locality"]');
            CREATE TABLE place(id INTEGER PRIMARY KEY,name TEXT,country_code TEXT,placetype INTEGER,
                latitude INTEGER,longitude INTEGER,west REAL,south REAL,east REAL,north REAL,population INTEGER);
            CREATE TABLE boundary(id INTEGER PRIMARY KEY,chunk INTEGER,offset INTEGER,size INTEGER);
            CREATE TABLE geometry_chunk(id INTEGER PRIMARY KEY,data BLOB NOT NULL);
            INSERT INTO metadata VALUES ('coordinateShift','0');
            CREATE VIRTUAL TABLE place_fts USING fts5(terms,content='',detail=none,columnsize=0,
                tokenize='unicode61 remove_diacritics 2');
            INSERT INTO place VALUES (1,'Munich','DE',5,572662306,137200344,11,47,12,49,1000000);
            INSERT INTO place VALUES (2,'Munich','US',5,0,0,0,0,0,0,190);
            INSERT INTO place_fts(rowid,terms) VALUES (1,'Munich Muenchen München DE'),(2,'Munich US');
        )sql");
    }
    /** Close before unlinking, including on Windows. */
    ~LocationDatabase()
    {
        sqlite3_close(db);
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    /** Execute fixture SQL with errors surfaced at the failing test. */
    void sql(char const* query)
    {
        INFO(sqlite3_errmsg(db));
        REQUIRE(sqlite3_exec(db, query, nullptr, nullptr, nullptr) == SQLITE_OK);
    }
    /** Insert bytes produced by the real schema writer, not a hand-coded wire format. */
    void boundary(plazs_data::Boundary& boundary)
    {
        auto buffer = zserio::serialize(boundary);
        sqlite3_stmt* raw = nullptr;
        REQUIRE(
            sqlite3_prepare_v2(
                db,
                "INSERT OR REPLACE INTO geometry_chunk VALUES (1,?)",
                -1,
                &raw,
                nullptr) == SQLITE_OK);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
        REQUIRE(
            sqlite3_bind_blob(
                raw,
                1,
                buffer.getData().data(),
                buffer.getData().size(),
                SQLITE_TRANSIENT) == SQLITE_OK);
        REQUIRE(sqlite3_step(raw) == SQLITE_DONE);
        sql("INSERT OR REPLACE INTO boundary SELECT 1,1,0,length(data) FROM geometry_chunk WHERE "
            "id=1");
    }
};
}  // namespace

TEST_CASE("WOF location lookup searches names without fetching geometry", "[gazetteer]")
{
    LocationDatabase fixture;
    plazs::Gazetteer lookup(fixture.path);
    auto matches = lookup.search("munich", 10);
    REQUIRE(matches.size() == 2);
    CHECK(matches[0].id == 1);
    CHECK(matches[0].name == "Munich");
    CHECK(matches[0].placeType == "locality");
    CHECK(std::abs(matches[0].position[0] - 11.5) < 1e-7);
    CHECK(std::abs(matches[0].position[1] - 48) < 1e-7);
    CHECK(matches[0].population == 1000000);
    CHECK(matches[0].bounds == std::array<double, 4>{11, 47, 12, 49});
    CHECK(lookup.search("munchen", 1).front().id == 1);
    CHECK(lookup.search("Munich DE", 5).size() == 1);
    CHECK(lookup.search("Munich_DE", 5).size() == 1);
    CHECK(
        lookup
            .search(
                "Munich\xe2\x80\x93"
                "DE",
                5)
            .size() == 1);
    CHECK(lookup.search("Muen* OR unknown", 5).empty());
    CHECK(lookup.search("", 5).empty());
    CHECK(lookup.search(std::string(201, 'x'), 5).empty());
    CHECK(lookup.search("mun", 1).size() == 1);
    REQUIRE(lookup.find(1));
    CHECK(lookup.find(1)->name == matches[0].name);
    CHECK_FALSE(lookup.find(404));
}

TEST_CASE("Location boundaries decode NDS blocks and preserve holes and islands", "[gazetteer]")
{
    LocationDatabase fixture;
    plazs_data::CoordinateBlock block;
    block.getPoints().emplace_back(0, 0);
    block.getPoints().emplace_back(100, 0);
    block.getPoints().emplace_back(100, 100);
    plazs_data::Ring ring;
    ring.getBlocks().push_back(block);
    plazs_data::Polygon polygon;
    polygon.getRings().push_back(ring);
    plazs_data::Boundary boundary;
    boundary.getPolygons().push_back(polygon);
    fixture.boundary(boundary);
    plazs::Gazetteer lookup(fixture.path);
    CHECK_FALSE(lookup.search("Munich", 1).front().geometry);
    auto match = lookup.find(1);
    REQUIRE(match);
    REQUIRE(match->geometry);
    auto const& geometry = *match->geometry;
    CHECK(geometry["type"] == "Polygon");
    CHECK(geometry["coordinates"][0].size() == 4);
    CHECK(geometry["coordinates"][0].front() == geometry["coordinates"][0].back());
    CHECK(geometry["coordinates"][0][1][0] == ndsmath::HighPrecWgs84::fromNdsCoordinates(100, 0).x);
    boundary.setMultiPolygon(true);
    auto hole = ring;
    hole.getBlocks().front().getPoints() = {{50, 20}, {60, 20}, {60, 30}};
    boundary.getPolygons().front().getRings().push_back(hole);
    auto island = polygon;
    for (auto& point : island.getRings().front().getBlocks().front().getPoints())
        point.setLongitude(point.getLongitude() + 200);
    boundary.getPolygons().push_back(island);
    fixture.boundary(boundary);
    auto multi = *lookup.find(1)->geometry;
    CHECK(multi["type"] == "MultiPolygon");
    CHECK(multi["coordinates"].size() == 2);
    CHECK(multi["coordinates"][0].size() == 2);
    fixture.sql("UPDATE place SET west=170,east=-175 WHERE id=1");
    CHECK(lookup.find(1)->bounds == std::array<double, 4>{170, 47, -175, 49});
    fixture.sql("UPDATE place SET west=-180,east=180 WHERE id=1");
    CHECK(lookup.find(1)->bounds == std::array<double, 4>{-180, 47, 180, 49});
}

TEST_CASE("Geometry spans are checked before allocating or decoding", "[gazetteer]")
{
    LocationDatabase fixture;
    fixture.sql(
        "INSERT INTO geometry_chunk VALUES (1,x'01020304'); INSERT INTO boundary VALUES (1,1,0,4)");
    plazs::Gazetteer lookup(fixture.path);
    for (auto update :
         {"UPDATE boundary SET offset=-1",
          "UPDATE boundary SET offset=9223372036854775807",
          "UPDATE boundary SET offset=0,size=5",
          "UPDATE boundary SET size=0",
          "UPDATE boundary SET size=2,chunk=999",
          "UPDATE boundary SET chunk=1,offset='bad'"})
    {
        fixture.sql(update);
        CHECK_THROWS(lookup.find(1));
        CHECK(lookup.search("Munich", 1).size() == 1);
    }
}

TEST_CASE("Grid metadata is validated instead of shifting by an unchecked value", "[gazetteer]")
{
    LocationDatabase fixture;
    for (auto update :
         {"UPDATE metadata SET value='-1' WHERE key='coordinateShift'",
          "UPDATE metadata SET value='17' WHERE key='coordinateShift'",
          "UPDATE metadata SET value='7x' WHERE key='coordinateShift'",
          "DELETE FROM metadata WHERE key='coordinateShift'"})
    {
        fixture.sql(update);
        CHECK_THROWS(plazs::Gazetteer(fixture.path));
    }
}

TEST_CASE(
    "Location boundary limits reject corrupt or oversized blobs without affecting search",
    "[gazetteer]")
{
    LocationDatabase fixture;
    plazs::Gazetteer lookup(fixture.path);
    fixture.sql(
        "INSERT INTO boundary VALUES (1,1,0,17*1024*1024); INSERT INTO geometry_chunk VALUES "
        "(1,zeroblob(17*1024*1024))");
    CHECK(lookup.search("Munich", 1).front().geometryAvailable);
    CHECK_THROWS_AS(lookup.find(1), std::length_error);
    fixture.sql("UPDATE boundary SET size=1; UPDATE geometry_chunk SET data=x'00'");
    CHECK_THROWS(lookup.find(1));
    // A tiny stream advertising a huge array must be bounded before allocating it.
    zserio::BitBuffer buffer(40);
    zserio::BitStreamWriter writer(buffer);
    writer.writeBool(true);
    writer.writeVarSize(100000000);
    sqlite3_stmt* raw = nullptr;
    REQUIRE(
        sqlite3_prepare_v2(fixture.db, "UPDATE geometry_chunk SET data=?", -1, &raw, nullptr) ==
        SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    REQUIRE(
        sqlite3_bind_blob(
            raw,
            1,
            buffer.getData().data(),
            buffer.getData().size(),
            SQLITE_TRANSIENT) == SQLITE_OK);
    REQUIRE(sqlite3_step(raw) == SQLITE_DONE);
    fixture.sql("UPDATE boundary SET size=(SELECT length(data) FROM geometry_chunk WHERE id=1)");
    CHECK_THROWS_AS(lookup.find(1), std::length_error);
}

TEST_CASE("Location lookup rejects legacy and unsupported databases", "[gazetteer]")
{
    CHECK_THROWS(plazs::Gazetteer("does-not-exist.sqlite"));
    LocationDatabase fixture;
    fixture.sql("PRAGMA user_version=0");
    CHECK_THROWS(plazs::Gazetteer(fixture.path));
    fixture.sql("PRAGMA user_version=999");
    CHECK_THROWS(plazs::Gazetteer(fixture.path));
    fixture.sql("PRAGMA user_version=1; PRAGMA application_id=0");
    CHECK_THROWS(plazs::Gazetteer(fixture.path));
}

TEST_CASE("Python-produced reduced-grid artifact supports concurrent native readers", "[gazetteer]")
{
    plazs::Gazetteer lookup(PLAZS_TEST_DATABASE);
    std::vector<std::future<bool>> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.push_back(std::async(
            std::launch::async,
            [&lookup]
            {
                for (int query = 0; query < 50; ++query) {
                    auto matches = lookup.search("Germany");
                    auto country = lookup.find(1);
                    if (matches.empty() || matches.front().id != 1 || !country ||
                        !country->geometry || (*country->geometry)["type"] != "Polygon")
                        return false;
                }
                return true;
            }));
    }
    for (auto& worker : workers)
        CHECK(worker.get());
}

TEST_CASE("Gazetteer filenames are UTF-8 on every platform", "[gazetteer]")
{
    LocationDatabase fixture;
    // Windows must close the writer before renaming; SQLite's open API expects UTF-8.
    REQUIRE(sqlite3_close(fixture.db) == SQLITE_OK);
    fixture.db = nullptr;
    auto renamed = fixture.path;
    renamed += std::filesystem::path(u8"-\u00fc\u6f22");
    std::filesystem::rename(fixture.path, renamed);
    fixture.path = renamed;
    plazs::Gazetteer lookup(fixture.path);
    CHECK(lookup.search("Munich").size() == 2);
}
