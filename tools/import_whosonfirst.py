#!/usr/bin/env python3
"""Prepare a compact, offline plazs gazetteer from a checksum-pinned WOF SQLite export.

No network access or runtime GIS dependency is needed. Shapely, ndslive-math and
zserio are ingestion-only requirements; PYTHONPATH must include the CMake-generated
location schema modules (build/python).
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import sqlite3
import tempfile

import shapely
from shapely.geometry import Polygon as ShapePolygon, MultiPolygon as ShapeMultiPolygon, shape
from ndslive.math import Wgs84
import zserio
from plazs_data.boundary import Boundary
from plazs_data.polygon import Polygon
from plazs_data.ring import Ring
from plazs_data.coordinate_block import CoordinateBlock
from plazs_data.coordinate import Coordinate


APPLICATION_ID = 0x504C5A53  # PLZS; deliberately distinct from upstream and former mapget artifacts.
FORMAT_VERSION = 1
DEFAULT_PLACETYPES = ("continent", "country", "dependency", "macroregion", "region", "locality")
LICENSE_URL = "https://whosonfirst.org/docs/licenses/"
SCHEMA = f"""
PRAGMA application_id = {APPLICATION_ID};
PRAGMA user_version = {FORMAT_VERSION};
PRAGMA journal_mode = OFF;
PRAGMA synchronous = OFF;
PRAGMA cache_size = -8192;
CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
CREATE TABLE place (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL, country_code TEXT NOT NULL,
  placetype INTEGER NOT NULL, latitude INTEGER NOT NULL, longitude INTEGER NOT NULL,
  west REAL NOT NULL, south REAL NOT NULL, east REAL NOT NULL, north REAL NOT NULL,
  population INTEGER
);
CREATE TABLE boundary (id INTEGER PRIMARY KEY, chunk INTEGER NOT NULL, offset INTEGER NOT NULL, size INTEGER NOT NULL);
CREATE TABLE geometry_chunk (id INTEGER PRIMARY KEY, data BLOB NOT NULL);
CREATE VIRTUAL TABLE place_fts USING fts5(
  terms, content='', detail=none, columnsize=0, tokenize='unicode61 remove_diacritics 2'
);
"""


class WofImporter:
    """Own snapshot verification, streaming selection and atomic artifact publication."""

    def __init__(self, source, output, manifest, license_path, *,
                 placetypes=DEFAULT_PLACETYPES, geometry_placetypes=None,
                 languages=(), simplify_degrees=0.0, min_locality_population=0, coordinate_shift=7):
        """Retain the selection and NDS grid shift; the default grid is about 1.2 m at the equator."""
        self.source = Path(source).resolve()
        self.output = Path(output).resolve()
        self.manifest = json.loads(Path(manifest).read_text(encoding="utf-8"))
        self.license = Path(license_path).read_text(encoding="utf-8")
        self.placetypes = sorted(set(placetypes))
        self.geometry_placetypes = set(geometry_placetypes if geometry_placetypes is not None else placetypes)
        self.languages = sorted(set(languages))
        self.simplify_degrees = simplify_degrees
        self.min_locality_population = min_locality_population
        self.coordinate_shift = coordinate_shift
        self.geometry_buffer = bytearray()
        self.geometry_chunk = 1
        self.counts = collections.Counter()
        self.geometry_bytes = collections.Counter()
        # The first six IDs have stable ranking semantics in the native lookup.
        self.place_types = list(DEFAULT_PLACETYPES) + sorted(set(self.placetypes) - set(DEFAULT_PLACETYPES))
        self.credits = collections.defaultdict(set)
        if self.source == self.output or not self.placetypes or not self.license.strip():
            raise ValueError("Distinct input/output, selected placetypes and upstream license are required")
        if not math.isfinite(simplify_degrees) or simplify_degrees < 0:
            raise ValueError("Simplification tolerance must be a finite nonnegative number of degrees")
        if not isinstance(coordinate_shift, int) or not 0 <= coordinate_shift <= 16:
            raise ValueError("Coordinate shift must be an integer in 0..16")
        if min_locality_population < 0:
            raise ValueError("Minimum locality population must be nonnegative")

    def verify(self):
        """Reject a changed/truncated upstream snapshot before reading any records."""
        if self.source.stat().st_size != self.manifest["size"]:
            raise ValueError("WOF input size does not match the pinned inventory entry")
        digest = hashlib.sha256()
        with self.source.open("rb") as stream:
            for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
                digest.update(block)
        if digest.hexdigest() != self.manifest["sha256"]:
            raise ValueError("WOF input SHA-256 does not match the pinned inventory entry")

    @staticmethod
    def encode(value):
        """Write deterministic UTF-8 JSON, rejecting non-finite source coordinates."""
        return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"), sort_keys=True)

    def names(self, properties, fallback):
        """Prefer the English label, retaining selected-language preferred/variant/colloquial aliases."""
        english = properties.get("name:eng_x_preferred", [])
        name = english[0] if isinstance(english, list) and english else fallback
        aliases = {fallback, name}
        for key, values in properties.items():
            if not key.startswith("name:") or not isinstance(values, list):
                continue
            language, _, usage = key[5:].partition("_x_")
            if self.languages and language not in self.languages:
                continue
            # Historical names can misleadingly match a current place; retain live aliases only.
            if usage in ("preferred", "variant", "colloquial"):
                aliases.update(value for value in values if isinstance(value, str) and value)
        return name, "\n".join(sorted(aliases))

    @staticmethod
    def polygons(geometry):
        """Yield area components only; collapsed lines/points are not place boundaries."""
        if geometry.geom_type == "Polygon":
            if not geometry.is_empty and geometry.area > 0:
                yield geometry
        elif geometry.geom_type in ("MultiPolygon", "GeometryCollection"):
            for member in geometry.geoms:
                yield from WofImporter.polygons(member)

    def ring(self, coordinates):
        """Quantize once, omit closure and adjacent duplicates, and discard sub-grid rings."""
        points = []
        for coordinate in coordinates:
            x, y = coordinate[:2]
            if not math.isfinite(x) or not math.isfinite(y) or not -180 <= x <= 180 or not -90 <= y <= 90:
                raise ValueError("Invalid WGS84 boundary coordinate")
            nds = Wgs84(x, y).to_nds_coordinates()
            point = tuple(round(value / (1 << self.coordinate_shift)) for value in nds)
            if not points or point != points[-1]:
                points.append(point)
        if len(points) > 1 and points[-1] == points[0]:
            points.pop()
        if len(set(points)) < 3:
            self.counts["collapsed_rings"] += 1
            return []
        return points

    def boundary(self, feature, placetype):
        """Repair/simplify areas, snap to NDS, then pack valid rings into bounded delta blocks."""
        geometry = feature.get("geometry")
        if placetype not in self.geometry_placetypes or not isinstance(geometry, dict):
            return None
        if geometry.get("type") not in ("Polygon", "MultiPolygon") or not geometry.get("coordinates"):
            return None
        original_multi = geometry["type"] == "MultiPolygon"
        area = shape(geometry)
        if not area.is_valid:
            self.counts["repaired_upstream_boundaries"] += 1
            area = shapely.make_valid(area)
        if self.simplify_degrees:
            simplified = area.simplify(self.simplify_degrees, preserve_topology=True)
            if simplified.is_valid and not simplified.is_empty:
                area = simplified
            else:
                self.counts["unsimplified_failed_boundaries"] += 1

        polygons = []
        for polygon in self.polygons(area):
            exterior = self.ring(polygon.exterior.coords)
            if not exterior:
                self.counts["collapsed_components"] += 1
                continue
            holes = [ring for interior in polygon.interiors if (ring := self.ring(interior.coords))]
            polygons.append(ShapePolygon(exterior, holes))
        area = ShapeMultiPolygon(polygons)
        if not area.is_valid:
            # Quantization can collapse a sliver or merge a hole into its exterior.
            # Repair in integer-coordinate space, not by inventing a larger polygon.
            self.counts["repaired_quantized_boundaries"] += 1
            area = shapely.make_valid(area)
        # Repair may introduce fractional intersections. The one-unit precision model
        # snaps those too and removes zero-area pieces, without an unbounded retry loop.
        area = shapely.set_precision(area, 1, mode="valid_output")
        pieces = list(self.polygons(area))
        if not pieces:
            self.counts["collapsed_boundaries"] += 1
            return None
        if not ShapeMultiPolygon(pieces).is_valid:
            raise ValueError(f"Invalid quantized boundary for WOF record {feature.get('id')}")
        encoded = []
        for polygon in pieces:
            rings = []
            for ring in [polygon.exterior, *polygon.interiors]:
                points = [Coordinate(int(x), int(y)) for x, y in list(ring.coords)[:-1]]
                if len(points) < 3:
                    raise ValueError("Invalid ring after NDS precision reduction")
                rings.append(Ring([CoordinateBlock(points[start:start + 64]) for start in range(0, len(points), 64)]))
            encoded.append(Polygon(rings))
        return bytes(zserio.serialize_to_bytes(Boundary(original_multi or len(encoded) != 1, encoded)))

    def flush_geometry(self, target):
        """Publish one bounded slab; per-place offsets retain independent random access."""
        if self.geometry_buffer:
            target.execute("INSERT INTO geometry_chunk VALUES (?,?)", (self.geometry_chunk, self.geometry_buffer))
            self.geometry_chunk += 1
            self.geometry_buffer.clear()

    def store_boundary(self, target, ident, geometry):
        """Keep each boundary contiguous; normal slabs are 4 MiB, exceptional polygons at most 16 MiB."""
        if len(geometry) > 16 * 1024 * 1024:
            raise ValueError(f"Boundary {ident} exceeds 16 MiB; choose a simplified profile")
        if self.geometry_buffer and len(self.geometry_buffer) + len(geometry) > 4 * 1024 * 1024:
            self.flush_geometry(target)
        target.execute("INSERT INTO boundary VALUES (?,?,?,?)", (
            ident, self.geometry_chunk, len(self.geometry_buffer), len(geometry)))
        self.geometry_buffer.extend(geometry)

    def insert(self, target, row):
        """Project one canonical feature into search metadata plus a separately loaded boundary."""
        feature = json.loads(row["body"])
        properties = feature["properties"]
        population = None
        for key in ("wof:population", "gn:population"):
            candidate = properties.get(key)
            if isinstance(candidate, int) and not isinstance(candidate, bool) and 0 <= candidate <= 2**63 - 1:
                population = candidate
                break
        if self.min_locality_population and row["placetype"] == "locality":
            # A compact profile deliberately excludes unknown population, rather than guessing it.
            if population is None or population < self.min_locality_population:
                self.counts["excluded_locality_population_unknown" if population is None else "excluded_locality_population_below_minimum"] += 1
                return
        name, aliases = self.names(properties, row["name"])
        coordinates = [row[key] for key in ("longitude", "latitude", "min_longitude", "min_latitude", "max_longitude", "max_latitude")]
        if not all(value is not None and math.isfinite(value) for value in coordinates):
            raise ValueError(f"Missing/non-finite coordinate in WOF record {row['id']}")
        if not all(-180 <= coordinates[i] <= 180 for i in (0, 2, 4)) or not all(-90 <= coordinates[i] <= 90 for i in (1, 3, 5)):
            raise ValueError(f"Out-of-range coordinate in WOF record {row['id']}")
        if coordinates[3] > coordinates[5]:
            raise ValueError(f"Inverted latitude extent in WOF record {row['id']}")
        geometry = self.boundary(feature, row["placetype"])
        size = len(geometry) if geometry else 0
        # Credit distinct upstream sources once per dataset, not in every place record.
        for key, value in properties.items():
            if key.startswith("src:") or key == "qs_source":
                self.credits[key].add(self.encode(value))
        longitude, latitude = Wgs84(row["longitude"], row["latitude"]).to_nds_coordinates()
        target.execute("INSERT INTO place VALUES (?,?,?,?,?,?,?,?,?,?,?)", (
            row["id"], name, row["country"], self.place_types.index(row["placetype"]), latitude, longitude,
            row["min_longitude"], row["min_latitude"], row["max_longitude"], row["max_latitude"], population))
        target.execute("INSERT INTO place_fts(rowid,terms) VALUES (?,?)", (row["id"], name + "\n" + aliases + "\n" + row["country"]))
        if geometry:
            self.store_boundary(target, row["id"], geometry)
            self.counts["boundaries"] += 1
        self.counts[row["placetype"]] += 1
        self.counts["places"] += 1
        self.geometry_bytes[row["placetype"]] += size

    def run(self):
        """Stream selected records with bounded caches, then publish only a complete checked database."""
        self.verify()
        self.output.parent.mkdir(parents=True, exist_ok=True)
        fd, temporary = tempfile.mkstemp(prefix=self.output.name + ".", suffix=".tmp", dir=self.output.parent)
        os.close(fd)
        try:
            with sqlite3.connect(self.source.as_uri() + "?mode=ro", uri=True) as source, sqlite3.connect(temporary) as target:
                source.row_factory = sqlite3.Row
                source.execute("PRAGMA cache_size = -8192")
                target.executescript(SCHEMA)
                selection = {"placetypes": self.placetypes, "geometryPlacetypes": sorted(self.geometry_placetypes),
                             "languages": self.languages, "simplificationDegrees": self.simplify_degrees,
                             "minLocalityPopulation": self.min_locality_population}
                selection.update({"shapelyVersion": shapely.__version__, "geosVersion": shapely.geos_version_string,
                                  "zserioVersion": importlib.metadata.version("zserio"),
                                  "ndsmathVersion": importlib.metadata.version("ndslive-math"),
                                  "schemaSha256": hashlib.sha256((Path(__file__).resolve().parents[1] / "schema/plazs_data.zs").read_bytes()).hexdigest(),
                                  "geometryEncoding": "zserio-nds-block64", "coordinateGridDegrees": 360 * 2**self.coordinate_shift / 2**32,
                                  "coordinateShift": self.coordinate_shift,
                                  "repair": "make_valid + NDS snap rounding; discard non-area components"})
                for key, value in (("snapshot", self.encode(self.manifest)), ("selection", self.encode(selection)), ("license", self.license),
                                   ("placeTypes", self.encode(self.place_types)), ("coordinateShift", str(self.coordinate_shift))):
                    target.execute("INSERT INTO metadata VALUES (?,?)", (key, value))
                placeholders = ",".join("?" for _ in self.placetypes)
                # Unknown current (-1) is not inactive. Alternate/historical geometries are not a search result.
                # Fix the outer scan/order to avoid SQLite sorting gigabytes of joined GeoJSON.
                # The snapshot's row order is reproducible; spr's integer primary key supplies metadata.
                query = f"""SELECT s.*, g.body FROM geojson g NOT INDEXED CROSS JOIN spr s ON s.id=g.id
                    WHERE s.placetype IN ({placeholders}) AND s.is_current != 0
                      AND s.is_deprecated != 1 AND s.is_ceased != 1 AND s.is_superseded != 1
                      AND g.is_alt=0 ORDER BY g.rowid"""
                for row in source.execute(query, self.placetypes):
                    self.counts["scanned"] += 1
                    self.insert(target, row)
                    if self.counts["scanned"] % 10000 == 0:
                        target.commit()
                        print(f"Scanned {self.counts['scanned']:,}, imported {self.counts['places']:,} places", flush=True)
                self.flush_geometry(target)
                target.execute("INSERT INTO place_fts(place_fts) VALUES ('optimize')")
                target.execute("INSERT INTO place_fts(place_fts) VALUES ('integrity-check')")
                stats = {"counts": dict(self.counts), "geometryBytes": dict(self.geometry_bytes)}
                target.execute("INSERT INTO metadata VALUES ('statistics',?)", (self.encode(stats),))
                attribution = {"name": "Who's On First", "url": "https://whosonfirst.org/", "licenseUrl": LICENSE_URL,
                               "sources": {key: [json.loads(value) for value in sorted(values)] for key, values in self.credits.items()}}
                target.execute("INSERT INTO metadata VALUES ('attribution',?)", (self.encode(attribution),))
                target.commit()
                target.execute("VACUUM")
                if target.execute("SELECT count(*) FROM boundary b LEFT JOIN geometry_chunk g ON g.id=b.chunk "
                                  "WHERE g.id IS NULL OR b.offset<0 OR b.size<=0 OR b.offset+b.size>length(g.data)").fetchone()[0]:
                    raise ValueError("Invalid boundary chunk index")
                if target.execute("PRAGMA quick_check").fetchone()[0] != "ok":
                    raise ValueError("Prepared WOF database failed integrity checking")
            # sqlite3 context managers commit/rollback but do not close handles (important on Windows).
            source.close()
            target.close()
            os.replace(temporary, self.output)
            return {**stats, "databaseBytes": self.output.stat().st_size}
        finally:
            if 'source' in locals():
                source.close()
            if 'target' in locals():
                target.close()
            Path(temporary).unlink(missing_ok=True)


def main():
    """Expose explicit data/geometry/language selection, independent of ordinary CMake builds."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Uncompressed WOF SQLite export")
    parser.add_argument("output", type=Path, help="Prepared SQLite database for --location-db")
    parser.add_argument("--manifest", type=Path, required=True, help="Pinned entry from the upstream SQLite inventory.json")
    parser.add_argument("--license", dest="license_path", type=Path, required=True, help="Upstream LICENSE.md, embedded unchanged")
    parser.add_argument("--placetypes", nargs="+", default=DEFAULT_PLACETYPES)
    parser.add_argument("--geometry-placetypes", nargs="*", help="Subset retaining boundaries; empty means names/extents only")
    parser.add_argument("--languages", nargs="*", default=(), help="ISO 639-3 alias languages; default all")
    parser.add_argument("--simplify-degrees", type=float, default=0, help="Optional topology-preserving tolerance in WGS84 degrees before NDS quantization")
    parser.add_argument("--coordinate-shift", type=int, default=7,
                        help="Drop this many NDS low bits before packing (0..16); default 7 is about 1.2 m at the equator")
    parser.add_argument("--min-locality-population", type=int, default=0,
                        help="Optional locality threshold; positive values also exclude missing population. Other placetypes are unaffected.")
    args = parser.parse_args()
    print(json.dumps(WofImporter(**vars(args)).run(), indent=2))


if __name__ == "__main__":
    main()
