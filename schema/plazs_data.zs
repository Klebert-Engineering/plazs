package plazs_data;

/** NDS coordinates divided by 2^coordinateShift (artifact metadata), rounded before packing. */
struct Coordinate
{
    int32 longitude;
    int:31 latitude;
};

/** At most 64 points; independent packing contexts limit the effect of large deltas. */
struct CoordinateBlock
{
    packed Coordinate points[];
};

/** A closed ring, without repeating its first coordinate at the end. */
struct Ring
{
    CoordinateBlock blocks[];
};

/** The first ring is the exterior; subsequent rings are holes. */
struct Polygon
{
    Ring rings[];
};

/** Preserve the GeoJSON container type even for a single-component MultiPolygon. */
struct Boundary
{
    bool multiPolygon;
    Polygon polygons[];
};
