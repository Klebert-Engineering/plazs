"""Synthetic WOF input and geometry assertions shared by plazs tests."""
import hashlib
import json
from pathlib import Path
import sqlite3
import import_whosonfirst as IMPORTER

def decode_boundary(payload, shift=7):
    """Read the actual generated Python schema, mirroring no binary encoding logic."""
    encoded = IMPORTER.zserio.deserialize_from_bytes(IMPORTER.Boundary, payload)
    polygons = []
    for polygon in encoded.polygons:
        rings = []
        for ring in polygon.rings:
            points = []
            for block in ring.blocks:
                for point in block.points:
                    wgs = IMPORTER.Wgs84.from_nds_coordinates(min(point.longitude * 2**shift, 2**31-1), min(point.latitude * 2**shift, 2**30-1))
                    points.append([wgs.x, wgs.y])
            rings.append(points + [points[0]])
        polygons.append(rings)
    return {"type": "MultiPolygon" if encoded.multi_polygon else "Polygon",
            "coordinates": polygons if encoded.multi_polygon else polygons[0]}


def read_boundary(db, ident):
    """Read one span from its slab for offline schema tests."""
    return db.execute("SELECT substr(g.data,b.offset+1,b.size) FROM boundary b JOIN geometry_chunk g ON g.id=b.chunk WHERE b.id=?", (ident,)).fetchone()[0]


def assert_boundary(test, actual, expected):
    """Allow NDS quantization and harmless GEOS ring rotation, not missing holes or islands."""
    test.assertEqual(actual["type"], expected["type"])
    test.assertTrue(IMPORTER.shape(actual).is_valid)
    test.assertTrue(IMPORTER.shape(actual).equals_exact(IMPORTER.shape(expected), 8e-6, normalize=True), actual)


POLYGON = {"type": "Polygon", "coordinates": [
    [[10, 47], [12, 47], [12, 49], [10, 49], [10, 47]],
    [[10.5, 47.5], [10.5, 48], [11, 48], [11, 47.5], [10.5, 47.5]],
]}
MULTIPOLYGON = {"type": "MultiPolygon", "coordinates": [POLYGON["coordinates"], [
    [[13, 47], [14, 47], [14, 48], [13, 48], [13, 47]]]]}


class WofFixture:
    """Own a synthetic upstream export with real table/annotation shapes and no licensed data."""

    def __init__(self, directory):
        """Include live/unknown/historical records, multilingual aliases, holes and islands."""
        self.root = Path(directory)
        self.source = self.root / "source.sqlite"
        self.output = self.root / "places.sqlite"
        self.manifest = self.root / "snapshot.json"
        self.license = self.root / "LICENSE.md"
        self.license.write_text("Synthetic fixture attribution\n", encoding="utf-8")
        with sqlite3.connect(self.source) as db:
            db.executescript("""
                CREATE TABLE spr(id INTEGER PRIMARY KEY, parent_id INTEGER, name TEXT,
                  placetype TEXT, country TEXT, latitude REAL, longitude REAL,
                  min_latitude REAL, min_longitude REAL, max_latitude REAL, max_longitude REAL,
                  is_current INTEGER, is_deprecated INTEGER, is_ceased INTEGER, is_superseded INTEGER);
                CREATE TABLE geojson(id INTEGER, body TEXT, is_alt INTEGER);
            """)
            for ident, name, kind, geometry in (
                (1, "Deutschland", "country", POLYGON),
                (2, "Bayern", "region", MULTIPOLYGON),
                (3, "M\u00fcnchen", "locality", {"type": "Point", "coordinates": [11.5, 48]}),
                (4, "Oldtown", "locality", POLYGON),
                (5, "Oldtown", "locality", POLYGON),
                (6, "Oldtown", "locality", POLYGON),
                (7, "Oldtown", "locality", POLYGON),
                (8, "Smallhood", "neighbourhood", POLYGON),
                (9, "Crossing", "country", MULTIPOLYGON),
            ):
                db.execute("INSERT INTO spr VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)", (
                    ident, 1 if kind != "country" else -1, name, kind, "DE", 48, 11.5,
                    47, 170 if ident == 9 else 10, 49, -175 if ident == 9 else 14,
                    0 if ident == 4 else -1 if ident == 2 else 1,
                    int(ident == 5), int(ident == 6), int(ident == 7)))
                properties = {
                    "wof:id": ident, "wof:placetype": kind,
                    "name:eng_x_preferred": [{1: "Germany", 2: "Bavaria", 3: "Munich"}.get(ident, name)],
                    "name:deu_x_preferred": [name], "name:fra_x_variant": ["Allemagne" if ident == 1 else name],
                    "name:eng_x_historical": ["Historicalalias"],
                    "wof:hierarchy": [{"country_id": 1, "region_id": 2, "locality_id": ident}],
                    "src:geom": "synthetic", "qs_source": "Fixture credit", "mz:is_approximate": 0,
                }
                feature = {"type": "Feature", "id": ident, "properties": properties, "geometry": geometry}
                db.execute("INSERT INTO geojson VALUES (?,?,0)", (ident, json.dumps(feature)))
                db.execute("INSERT INTO geojson VALUES (?,?,1)", (ident, json.dumps(feature)))
        db.close()
        self.manifest.write_text(json.dumps({"size": self.source.stat().st_size,
                                            "sha256": hashlib.sha256(self.source.read_bytes()).hexdigest(),
                                            "commit": "synthetic"}))

    def prepare(self, **options):
        """Use precisely the production importer, not a duplicate prepared-schema fixture."""
        return IMPORTER.WofImporter(self.source, self.output, self.manifest, self.license, **options).run()


