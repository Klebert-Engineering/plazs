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

/**
 * An immutable, disk-backed gazetteer. Concurrent queries share no statement or
 * decoder state. Construction validates the artifact format and metadata;
 * individual boundaries are checked when find() decodes them. SQL and decode
 * failures propagate as exceptions. No network or GIS engine is needed at runtime.
 */
class Gazetteer
{
public:
    /** Open and validate a prepared artifact; does not read or allocate all geometries. */
    explicit Gazetteer(std::filesystem::path databasePath);

    /** Release the read-only database handle after callers finish their queries. */
    ~Gazetteer();

    Gazetteer(Gazetteer const&) = delete;
    Gazetteer& operator=(Gazetteer const&) = delete;

    /**
     * Match Unicode-normalized token prefixes (AND), ranked by name, type and population.
     * Input over 200 bytes, trimmed input under two bytes, or input without tokens
     * returns no matches. Only the first eight tokens constrain the query; limit is
     * clamped to 1..50. Results contain metadata and geometry availability, not boundaries.
     * No geometry is loaded.
     */
    std::vector<Place> search(std::string_view name, uint32_t limit = 10) const;

    /**
     * Resolve a positive WOF ID, including any retained boundary; invalid/unknown IDs
     * return no match. Encoded boundaries over 16 MiB, decoder allocations over 32 MiB,
     * or more than one million decoded vertices raise std::length_error. These are
     * separate format/allocation limits, not a total bound on the returned GeoJSON.
     */
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
