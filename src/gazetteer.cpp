#include "plazs/gazetteer.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <limits>
#include <memory>
#include <memory_resource>
#include <optional>
#include <sstream>

#include "ndsmath/wgs84.h"
#include "plazs_data/Boundary.h"
#include "zserio/BitStreamReader.h"

namespace plazs
{
namespace
{

constexpr uint32_t kHardMaxLimit = 50;
constexpr int kWofApplicationId = 0x504C5A53;
constexpr int kWofFormatVersion = 1;
constexpr int kMaxGeometryBytes = 16 * 1024 * 1024;

/** Bound generated decoder allocations, including malicious packed repeat-count arrays. */
class BoundaryMemory final : public zserio::pmr::MemoryResource
{
    size_t allocated_ = 0;

    /** Reject oversized allocation requests before they reach the process heap. */
    void* doAllocate(size_t bytes, size_t alignment) override
    {
        if (bytes > 32 * 1024 * 1024 - allocated_)
            throw std::length_error("Place boundary exceeds decoder memory budget");
        auto* memory = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        allocated_ += bytes;
        return memory;
    }
    /** Account for memory released while unwinding a malformed stream. */
    void doDeallocate(void* memory, size_t bytes, size_t alignment) override
    {
        std::pmr::new_delete_resource()->deallocate(memory, bytes, alignment);
        allocated_ -= bytes;
    }
    /** Every decode has an independent allocation budget. */
    bool doIsEqual(zserio::pmr::MemoryResource const& other) const noexcept override
    {
        return this == &other;
    }
};

/** Trim leading and trailing ASCII whitespace before building an FTS query. */
std::string trim(std::string_view value)
{
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

}  // namespace

/** Tokenize exactly like the index: Unicode punctuation must not become a phrase query. */
std::string Gazetteer::prefixQuery(std::string_view input) const
{
    fts5_api* api = nullptr;
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT fts5(?1)", -1, &raw, nullptr) != SQLITE_OK)
        throw std::runtime_error("Location tokenizer unavailable");
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(raw, sqlite3_finalize);
    sqlite3_bind_pointer(raw, 1, &api, "fts5_api_ptr", nullptr);
    sqlite3_step(raw);
    fts5_tokenizer tokenizer{};
    void* context = nullptr;
    if (!api || api->xFindTokenizer(api, "unicode61", &context, &tokenizer) != SQLITE_OK)
        throw std::runtime_error("Location tokenizer unavailable");
    Fts5Tokenizer* instance = nullptr;
    char const* options[] = {"remove_diacritics", "2"};
    if (tokenizer.xCreate(context, options, 2, &instance) != SQLITE_OK)
        throw std::runtime_error("Location tokenizer unavailable");
    std::unique_ptr<Fts5Tokenizer, decltype(tokenizer.xDelete)> owner(instance, tokenizer.xDelete);
    std::vector<std::string> tokens;
    auto status = tokenizer.xTokenize(
        instance,
        &tokens,
        FTS5_TOKENIZE_QUERY,
        input.data(),
        input.size(),
        [](void* context, int, char const* text, int size, int, int) -> int
        {
            auto& tokens = *static_cast<std::vector<std::string>*>(context);
            // Never throw across SQLite's C callback boundary or truncate UTF-8 mid-codepoint.
            try {
                if (tokens.size() < 8)
                    tokens.emplace_back(text, size);
                return SQLITE_OK;
            }
            catch (...) {
                return SQLITE_NOMEM;
            }
        });
    if (status != SQLITE_OK)
        throw std::runtime_error("Location tokenization failed");
    std::ostringstream query;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i)
            query << " AND ";
        query << '\"' << tokens[i] << "\"*";
    }
    return query.str();
}

Gazetteer::Gazetteer(std::filesystem::path databasePath) : databasePath_(std::move(databasePath))
{
    if (databasePath_.empty() || !std::filesystem::exists(databasePath_))
        throw std::runtime_error("Gazetteer file does not exist: " + databasePath_.string());

    auto filename = databasePath_.u8string();
    auto rc = sqlite3_open_v2(
        reinterpret_cast<char const*>(filename.c_str()),
        &db_,
        SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> guard(db_, sqlite3_close);
    if (rc != SQLITE_OK)
        throw std::runtime_error(
            "Cannot open gazetteer: " +
            std::string(db_ ? sqlite3_errmsg(db_) : "allocation failed"));
    auto integerPragma = [this](char const* sql)
    {
        sqlite3_stmt* raw = nullptr;
        sqlite3_prepare_v2(db_, sql, -1, &raw, nullptr);
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        return stmt && sqlite3_step(stmt.get()) == SQLITE_ROW ?
            sqlite3_column_int(stmt.get(), 0) :
            -1;
    };
    sqlite3_stmt* check = nullptr;
    auto const schema =
        "SELECT p.id,p.name,p.country_code,p.placetype,p.latitude,p.longitude,p.west,p.south,"
        "p.east,p.north,p.population,b.chunk,b.offset,b.size,g.data FROM place p LEFT JOIN "
        "boundary b ON b.id=p.id LEFT JOIN geometry_chunk g ON g.id=b.chunk "
        "LIMIT 0";
    bool valid = integerPragma("PRAGMA application_id") == kWofApplicationId &&
        integerPragma("PRAGMA user_version") == kWofFormatVersion &&
        sqlite3_prepare_v2(db_, schema, -1, &check, nullptr) == SQLITE_OK;
    sqlite3_finalize(check);
    check = nullptr;
    valid = valid &&
        sqlite3_prepare_v2(
            db_,
            "SELECT rowid FROM place_fts WHERE place_fts MATCH 'test' LIMIT 0",
            -1,
            &check,
            nullptr) == SQLITE_OK;
    sqlite3_finalize(check);
    check = nullptr;
    if (valid &&
        sqlite3_prepare_v2(
            db_,
            "SELECT value FROM metadata WHERE key='placeTypes'",
            -1,
            &check,
            nullptr) == SQLITE_OK)
    {
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>
            statement(check, sqlite3_finalize);
        valid = sqlite3_step(check) == SQLITE_ROW && sqlite3_column_type(check, 0) == SQLITE_TEXT &&
            sqlite3_column_bytes(check, 0) < 65536;
        if (valid) {
            try {
                placeTypes_ =
                    nlohmann::json::parse(
                        reinterpret_cast<char const*>(sqlite3_column_text(check, 0)))
                        .get<std::vector<std::string>>();
                valid = !placeTypes_.empty();
            }
            catch (std::exception const&) {
                valid = false;
            }
        }
    }
    else {
        sqlite3_finalize(check);
        valid = false;
    }
    if (valid) {
        sqlite3_stmt* raw = nullptr;
        valid =
            sqlite3_prepare_v2(
                db_,
                "SELECT value FROM metadata WHERE key='coordinateShift'",
                -1,
                &raw,
                nullptr) == SQLITE_OK;
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> scale(raw, sqlite3_finalize);
        valid = valid && sqlite3_step(raw) == SQLITE_ROW &&
            sqlite3_column_type(raw, 0) == SQLITE_TEXT;
        if (valid) {
            auto text = reinterpret_cast<char const*>(sqlite3_column_text(raw, 0));
            auto size = sqlite3_column_bytes(raw, 0);
            auto [end, error] = std::from_chars(text, text + size, coordinateShift_);
            valid = error == std::errc() && end == text + size && coordinateShift_ <= 16;
        }
    }
    // Raw WOF exports intentionally are not served: preparation supplies FTS, selection and
    // provenance.
    if (!valid)
        throw std::runtime_error("Unsupported gazetteer format: " + databasePath_.string());
    guard.release();
}

Gazetteer::~Gazetteer()
{
    if (db_) {
        sqlite3_close(db_);
    }
}

std::vector<Place> Gazetteer::search(std::string_view name, uint32_t limit) const
{
    if (name.size() > 200) {
        return {};
    }
    auto trimmed = trim(name);
    if (trimmed.size() < 2) {
        return {};
    }
    return query(trimmed, limit);
}

std::vector<Place>
Gazetteer::query(std::string_view name, uint32_t limit, std::optional<int64_t> id) const
{
    auto trimmed = std::string(name);
    auto ftsQuery = id ? std::string{} : prefixQuery(trimmed);
    if (!id && ftsQuery.empty()) {
        return {};
    }

    limit = std::max<uint32_t>(1, std::min<uint32_t>(limit, kHardMaxLimit));
    auto prefix = trimmed + "%";

    std::string sql =
        "SELECT id,name,latitude,longitude,country_code,population,west,south,east,north,placetype,"
        "EXISTS(SELECT 1 FROM boundary WHERE boundary.id=place.id) FROM place WHERE ";
    sql += id ? "id=?1" : R"sql(
        id IN (SELECT rowid FROM place_fts WHERE place_fts MATCH ?1)
        ORDER BY CASE WHEN name = ?2 COLLATE NOCASE THEN 0 WHEN name LIKE ?3 THEN 1 ELSE 2 END,
                 CASE WHEN placetype <= 2 THEN 0 WHEN placetype <= 4 THEN 1 ELSE 2 END,
                 COALESCE(population,0) DESC, name COLLATE NOCASE, id LIMIT ?4
    )sql";

    sqlite3_stmt* stmt = nullptr;
    auto rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> statement(stmt, sqlite3_finalize);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(
            "Cannot prepare gazetteer query: " + std::string(sqlite3_errmsg(db_)));
    }

    if (id)
        sqlite3_bind_int64(stmt, 1, *id);
    else {
        sqlite3_bind_text(stmt, 1, ftsQuery.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, trimmed.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, prefix.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 4, static_cast<int>(limit));
    }

    std::vector<Place> matches;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        matches.push_back(readPlace(stmt));
    }
    if (rc != SQLITE_DONE) {
        throw std::runtime_error("Gazetteer query failed: " + std::string(sqlite3_errmsg(db_)));
    }
    return matches;
}

Place Gazetteer::readPlace(sqlite3_stmt* stmt) const
{
    auto text = [stmt](int column) -> std::string
    {
        auto value = sqlite3_column_text(stmt, column);
        return value ? reinterpret_cast<char const*>(value) : "";
    };
    Place match;
    match.id = sqlite3_column_int64(stmt, 0);
    match.countryCode = text(4);
    match.name = text(1);
    auto point = ndsmath::HighPrecWgs84::fromNdsCoordinates(
        sqlite3_column_int(stmt, 3),
        sqlite3_column_int(stmt, 2));
    match.position = {point.x, point.y};
    match.bounds = {
        sqlite3_column_double(stmt, 6),
        sqlite3_column_double(stmt, 7),
        sqlite3_column_double(stmt, 8),
        sqlite3_column_double(stmt, 9)};
    if (sqlite3_column_type(stmt, 5) != SQLITE_NULL)
        match.population = sqlite3_column_int64(stmt, 5);
    match.placeType = placeTypes_.at(sqlite3_column_int(stmt, 10));
    match.geometryAvailable = sqlite3_column_int(stmt, 11) != 0;
    return match;
}

std::optional<Place> Gazetteer::find(int64_t numericId) const
{
    if (numericId <= 0)
        return {};
    auto matches = query({}, 1, numericId);
    if (matches.empty())
        return {};
    auto match = std::move(matches.front());
    if (match.geometryAvailable) {
        sqlite3_stmt* raw = nullptr;
        // Fetch only the span descriptor. Incremental BLOB I/O never materializes the full chunk.
        if (sqlite3_prepare_v2(
                db_,
                "SELECT chunk,offset,size FROM boundary WHERE id=?",
                -1,
                &raw,
                nullptr) != SQLITE_OK)
            throw std::runtime_error("Failed to prepare place boundary query");
        std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> stmt(raw, sqlite3_finalize);
        sqlite3_bind_int64(raw, 1, numericId);
        if (sqlite3_step(raw) != SQLITE_ROW)
            throw std::runtime_error("Failed to load place boundary");
        for (int column = 0; column < 3; ++column)
            if (sqlite3_column_type(raw, column) != SQLITE_INTEGER)
                throw std::runtime_error("Invalid boundary span type");
        auto chunk = sqlite3_column_int64(raw, 0);
        auto offset = sqlite3_column_int64(raw, 1);
        auto size = sqlite3_column_int64(raw, 2);
        if (size > kMaxGeometryBytes)
            throw std::length_error(
                "Place boundary exceeds 16 MiB; prepare a simplified gazetteer");
        if (chunk <= 0 || offset < 0 || offset > std::numeric_limits<int>::max() || size <= 0)
            throw std::runtime_error("Invalid boundary span");
        sqlite3_blob* handle = nullptr;
        auto rc = sqlite3_blob_open(db_, "main", "geometry_chunk", "data", chunk, 0, &handle);
        std::unique_ptr<sqlite3_blob, decltype(&sqlite3_blob_close)>
            blob(handle, sqlite3_blob_close);
        if (rc != SQLITE_OK || offset + size > sqlite3_blob_bytes(handle))
            throw std::runtime_error("Invalid boundary chunk or span");
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (sqlite3_blob_read(
                handle,
                bytes.data(),
                static_cast<int>(size),
                static_cast<int>(offset)) != SQLITE_OK)
            throw std::runtime_error("Failed to read place boundary");
        BoundaryMemory memory;
        zserio::BitStreamReader reader(bytes.data(), bytes.size());
        plazs_data::Boundary boundary(reader, plazs_data::Boundary::allocator_type(&memory));
        if (boundary.getPolygons().empty() ||
            (!boundary.getMultiPolygon() && boundary.getPolygons().size() != 1))
            throw std::runtime_error("Invalid place boundary container");
        auto polygons = nlohmann::json::array();
        size_t vertices = 0;
        for (auto const& polygon : boundary.getPolygons()) {
            auto rings = nlohmann::json::array();
            for (auto const& ring : polygon.getRings()) {
                auto points = nlohmann::json::array();
                for (auto const& block : ring.getBlocks()) {
                    if (block.getPoints().empty() || block.getPoints().size() > 64)
                        throw std::runtime_error("Invalid place boundary block");
                    vertices += block.getPoints().size();
                    if (vertices > 1000000)
                        throw std::length_error("Place boundary exceeds decoded vertex budget");
                    for (auto const& point : block.getPoints()) {
                        auto x = int64_t(point.getLongitude()) * (int64_t(1) << coordinateShift_);
                        auto y = int64_t(point.getLatitude()) * (int64_t(1) << coordinateShift_);
                        if (x < -(int64_t(1) << 31) || x > (int64_t(1) << 31) ||
                            y < -(int64_t(1) << 30) || y > (int64_t(1) << 30))
                            throw std::runtime_error("Boundary coordinate outside the world");
                        // Rounding may land exactly on +180/+90. Clamp to NDS's last representable
                        // positive point, never wrap a dateline polygon into the other hemisphere.
                        auto wgs = ndsmath::HighPrecWgs84::fromNdsCoordinates(
                            static_cast<int32_t>(std::min(x, (int64_t(1) << 31) - 1)),
                            static_cast<int32_t>(std::min(y, (int64_t(1) << 30) - 1)));
                        points.push_back({wgs.x, wgs.y});
                    }
                }
                if (points.size() < 3)
                    throw std::runtime_error("Invalid place boundary ring");
                points.push_back(points.front());
                rings.push_back(std::move(points));
            }
            if (rings.empty())
                throw std::runtime_error("Invalid place boundary polygon");
            polygons.push_back(std::move(rings));
        }
        // Permit only the serializer's final zero padding, not a concatenated payload.
        auto remaining = size * 8 - reader.getBitPosition();
        if (remaining > 7 || (remaining && reader.readBits(remaining) != 0))
            throw std::runtime_error("Trailing place boundary data");
        match.geometry = {
            {"type", boundary.getMultiPolygon() ? "MultiPolygon" : "Polygon"},
            {"coordinates",
             boundary.getMultiPolygon() ? std::move(polygons) : std::move(polygons.front())}};
    }
    return match;
}

}  // namespace plazs
