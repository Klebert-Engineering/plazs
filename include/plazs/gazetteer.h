#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace plazs
{

/** A WOF place with compact search metadata and an optionally loaded GeoJSON boundary. */
struct Place
{
    int64_t id = 0;          /**< Stable upstream WOF ID, never an internal row number. */
    std::string name;        /**< Preferred English label, otherwise the upstream name. */
    std::string countryCode; /**< Upstream ISO country code, possibly empty. */
    std::string placeType;   /**< Country, region, locality, or another selected WOF type. */
    std::array<double, 2>
        position{}; /**< WGS84 longitude/latitude; not necessarily inside the area. */
    std::array<double, 4>
        bounds{}; /**< West, south, east, north; west > east crosses the dateline. */
    std::optional<int64_t> population; /**< Optional nonnegative ranking hint. */
    bool geometryAvailable = false;    /**< Whether the artifact retains a real area, not a bbox. */
    std::optional<nlohmann::json> geometry; /**< Polygon/MultiPolygon, present only after find(). */
};

/** An immutable, disk-backed gazetteer. Concurrent queries share no statement or decoder state.
 * Construction rejects missing/unsupported artifacts. Queries throw on corruption rather than
 * silently returning partial results. No network or GIS engine is required at runtime. */
class Gazetteer
{
public:
    /** Open and validate a prepared artifact; does not read or allocate all geometries. */
    explicit Gazetteer(std::filesystem::path databasePath);
    /** Release the read-only database handle after callers finish their queries. */
    ~Gazetteer();
    Gazetteer(Gazetteer const&) = delete;
    Gazetteer& operator=(Gazetteer const&) = delete;

    /** Match Unicode-normalized token prefixes (AND), ranked by name, type and population.
     * Queries are limited to 200 bytes/eight tokens; limit is clamped to 1..50. No geometry is
     * loaded. */
    std::vector<Place> search(std::string_view name, uint32_t limit = 10) const;
    /** Resolve a positive WOF ID, including any retained boundary. Unknown IDs return no match.
     * Excessive geometry size or decoder allocation raises std::length_error. */
    std::optional<Place> find(int64_t id) const;

private:
    std::filesystem::path databasePath_;
    sqlite3* db_ = nullptr;
    std::vector<std::string> placeTypes_;
    unsigned coordinateShift_ = 0;

    /** Build a literal prefix query using exactly the database's Unicode tokenizer. */
    std::string prefixQuery(std::string_view input) const;
    /** Execute a bounded name search or exact ID lookup without fetching boundaries. */
    std::vector<Place>
    query(std::string_view name, uint32_t limit, std::optional<int64_t> id = {}) const;
    /** Decode metadata and geometry availability without materializing the geometry. */
    Place readPlace(sqlite3_stmt* statement) const;
};

}  // namespace plazs
