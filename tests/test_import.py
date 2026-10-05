"""Offline ingestion regression tests, independent of mapget or HTTP."""
import hashlib
import json
import sqlite3
import tempfile
import unittest
from pathlib import Path
from wof_fixture import WofFixture, IMPORTER, POLYGON, MULTIPOLYGON, decode_boundary, assert_boundary, read_boundary

class WofImportTests(unittest.TestCase):
    """Verify filtering, provenance and publication before involving C++ or HTTP."""

    def setUp(self):
        """Give each preparation test its own complete reproducible source snapshot."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.fixture = WofFixture(directory.name)

    def test_selection_and_provenance(self):
        """Unknown-current survives; inactive, alternate and unselected records do not."""
        stats = self.fixture.prepare()
        self.assertEqual(stats["counts"]["places"], 4)
        self.assertEqual(stats["counts"]["boundaries"], 3)
        with sqlite3.connect(self.fixture.output) as db:
            self.assertEqual(db.execute("SELECT id FROM place ORDER BY id").fetchall(), [(1,), (2,), (3,), (9,)])
            assert_boundary(self, decode_boundary(read_boundary(db, 2)), MULTIPOLYGON)
            columns = [row[1] for row in db.execute("PRAGMA table_info(place)")]
            self.assertNotIn("details", columns)
            self.assertNotIn("geometry_bytes", columns)
            self.assertEqual(db.execute("SELECT typeof(data) FROM geometry_chunk LIMIT 1").fetchone()[0], "blob")
            credit = json.loads(db.execute("SELECT value FROM metadata WHERE key='attribution'").fetchone()[0])
            self.assertIn("Fixture credit", credit["sources"]["qs_source"])
            self.assertEqual(db.execute("SELECT value FROM metadata WHERE key='license'").fetchone()[0], "Synthetic fixture attribution\n")
            self.assertEqual(db.execute("SELECT rowid FROM place_fts WHERE place_fts MATCH 'historicalalias'").fetchall(), [])
        db.close()

    def test_language_and_geometry_selection(self):
        """Name/geometry selection is explicit and does not erase a record's real extent."""
        self.fixture.prepare(languages=["eng"], geometry_placetypes=[])
        with sqlite3.connect(self.fixture.output) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM boundary").fetchone()[0], 0)
            self.assertEqual(db.execute("SELECT rowid FROM place_fts WHERE place_fts MATCH 'Allemagne'").fetchall(), [])
            self.assertEqual(db.execute("SELECT west,east FROM place WHERE id=1").fetchone(), (10, 14))
        db.close()

    def test_population_selection_does_not_drop_countries_or_regions(self):
        """A locality without population may be explicitly excluded, never a country/state."""
        stats = self.fixture.prepare(min_locality_population=5000)
        self.assertEqual(stats["counts"]["places"], 3)
        self.assertEqual(stats["counts"]["excluded_locality_population_unknown"], 1)
        self.assertEqual(stats["counts"]["country"], 2)
        self.assertEqual(stats["counts"]["region"], 1)

    def test_population_prefers_wof_and_falls_back_to_geonames(self):
        """A canonical population wins over an old concordance value, even if it is smaller."""
        for canonical, expected in ((1000, 3), (6000, 4), (None, 4)):
            with self.subTest(canonical=canonical):
                with sqlite3.connect(self.fixture.source) as db:
                    feature = json.loads(db.execute("SELECT body FROM geojson WHERE id=3 AND is_alt=0").fetchone()[0])
                    feature["properties"].update({"wof:population": canonical, "gn:population": 7000})
                    db.execute("UPDATE geojson SET body=? WHERE id=3 AND is_alt=0", (json.dumps(feature),))
                db.close()
                self.fixture.manifest.write_text(json.dumps({"size": self.fixture.source.stat().st_size,
                    "sha256": hashlib.sha256(self.fixture.source.read_bytes()).hexdigest()}))
                stats = self.fixture.prepare(min_locality_population=5000)
                self.assertEqual(stats["counts"]["places"], expected)

    def test_checksum_failure_preserves_published_database(self):
        """A partial/replaced upstream input must never replace the previous good artifact."""
        self.fixture.output.write_bytes(b"previous artifact")
        self.fixture.manifest.write_text(json.dumps({"size": self.fixture.source.stat().st_size, "sha256": "0" * 64}))
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            self.fixture.prepare()
        self.assertEqual(self.fixture.output.read_bytes(), b"previous artifact")
        self.assertEqual(list(self.fixture.root.glob("*.tmp")), [])

    def test_deterministic_artifact(self):
        """The same snapshot and selection reproduce identical SQLite bytes."""
        self.fixture.prepare()
        first = self.fixture.output.read_bytes()
        self.fixture.prepare()
        self.assertEqual(first, self.fixture.output.read_bytes())

    def test_published_fixture_matches_contract(self):
        """Adapter fixtures must carry this format and the same searchable/geometric content.

        Provenance records build-tool versions, so compare content rather than platform-specific bytes.
        """
        self.fixture.prepare()
        with sqlite3.connect(self.fixture.output) as actual, sqlite3.connect(
                Path(__file__).parent / "data/places.sqlite") as published:
            for table in ("place", "boundary", "geometry_chunk"):
                self.assertEqual(actual.execute(f"SELECT * FROM {table} ORDER BY id").fetchall(),
                                 published.execute(f"SELECT * FROM {table} ORDER BY id").fetchall())
            self.assertEqual(actual.execute("PRAGMA application_id").fetchone(), published.execute("PRAGMA application_id").fetchone())
            self.assertEqual(actual.execute("PRAGMA user_version").fetchone(), published.execute("PRAGMA user_version").fetchone())
        actual.close()
        published.close()

    def test_grid_scale_and_boundaries(self):
        """The scale is explicit; a small lossy grid may collapse rings but never invent an area."""
        for shift in (0, 7, 16):
            with self.subTest(shift=shift):
                self.fixture.prepare(coordinate_shift=shift)
                with sqlite3.connect(self.fixture.output) as db:
                    self.assertEqual(db.execute("SELECT value FROM metadata WHERE key='coordinateShift'").fetchone()[0], str(shift))
                    decoded = decode_boundary(read_boundary(db, 1), shift)
                    self.assertTrue(IMPORTER.shape(decoded).is_valid)
                    self.assertLessEqual(IMPORTER.shape(decoded).hausdorff_distance(IMPORTER.shape(POLYGON)), 360 * 2**shift / 2**32)
                db.close()
        for shift in (-1, 17, 1.5):
            with self.assertRaises(ValueError):
                self.fixture.prepare(coordinate_shift=shift)

    def test_chunk_rollover_and_large_boundary(self):
        """Chunking is lossless and keeps one boundary contiguous even when it exceeds a normal slab."""
        importer = IMPORTER.WofImporter(self.fixture.source, self.fixture.output, self.fixture.manifest, self.fixture.license)
        with sqlite3.connect(":memory:") as db:
            db.executescript(IMPORTER.SCHEMA)
            values = [b"a" * (4 * 1024 * 1024 - 8), b"different", b"b" * (5 * 1024 * 1024)]
            for ident, data in enumerate(values):
                importer.store_boundary(db, ident + 1, data)
            importer.flush_geometry(db)
            self.assertEqual(db.execute("SELECT count(*) FROM geometry_chunk").fetchone()[0], 3)
            for ident, data in enumerate(values):
                self.assertEqual(read_boundary(db, ident + 1), data)
            with self.assertRaises(ValueError):
                importer.store_boundary(db, 4, b"x" * (16 * 1024 * 1024 + 1))
        db.close()

    def test_simplification_keeps_holes_and_single_part_multipolygons(self):
        """GEOS may unwrap or rotate rings; the stored container and meaningful rings survive."""
        importer = IMPORTER.WofImporter(self.fixture.source, self.fixture.output, self.fixture.manifest,
                                        self.fixture.license, simplify_degrees=0.001)
        geometry = {"type": "MultiPolygon", "coordinates": [POLYGON["coordinates"]]}
        encoded = importer.boundary({"id": 1, "geometry": geometry}, "country")
        assert_boundary(self, decode_boundary(encoded), geometry)

    def test_quantization_discards_collapsed_holes_and_islands(self):
        """Sub-grid areas disappear; surviving polygons remain valid, with no fabricated triangle."""
        importer = IMPORTER.WofImporter(self.fixture.source, self.fixture.output, self.fixture.manifest, self.fixture.license)
        tiny = [[11, 48], [11 + 1e-10, 48], [11, 48 + 1e-10], [11, 48]]
        exterior = POLYGON["coordinates"][0]
        geometry = {"type": "MultiPolygon", "coordinates": [[exterior, tiny], [tiny]]}
        decoded = decode_boundary(importer.boundary({"geometry": geometry}, "country"))
        self.assertTrue(IMPORTER.shape(decoded).is_valid)
        self.assertEqual(len(decoded["coordinates"]), 1)
        self.assertEqual(len(decoded["coordinates"][0]), 1)
        self.assertGreater(importer.counts["collapsed_rings"], 0)
        self.assertIsNone(importer.boundary({"geometry": {"type": "Polygon", "coordinates": [tiny]}}, "country"))
        self.assertEqual(importer.counts["collapsed_boundaries"], 1)

    def test_invalid_upstream_and_world_edges(self):
        """Repair a bow tie; keep the positive dateline and pole on their own side of the world."""
        importer = IMPORTER.WofImporter(self.fixture.source, self.fixture.output, self.fixture.manifest, self.fixture.license)
        invalid = {"type": "Polygon", "coordinates": [[[10, 47], [11, 48], [11, 47], [10, 48], [10, 47]]]}
        result = decode_boundary(importer.boundary({"geometry": invalid}, "country"))
        self.assertTrue(IMPORTER.shape(result).is_valid)
        self.assertEqual(result["type"], "MultiPolygon")
        self.assertEqual(importer.counts["repaired_upstream_boundaries"], 1)
        for x, y in ((180, 90), (-180, -90)):
            triangle = [[x,y], [x * 0.99,y], [x,y * 0.99], [x,y]]
            encoded = importer.boundary({"geometry": {"type": "Polygon", "coordinates": [triangle]}}, "country")
            result = decode_boundary(encoded)
            self.assertTrue(IMPORTER.shape(result).is_valid)
            for lon, lat in result["coordinates"][0]:
                self.assertGreater(lon * x, 0)
                self.assertGreater(lat * y, 0)

    def test_block_boundaries_and_repeated_coordinates(self):
        """Multiple packed blocks retain every non-redundant vertex and implicit closure."""
        import math
        ring = [[11 + math.cos(i * math.tau / 150), 48 + math.sin(i * math.tau / 150)] for i in range(150)]
        geometry = {"type": "Polygon", "coordinates": [ring + [ring[-1], ring[0]]]}
        importer = IMPORTER.WofImporter(self.fixture.source, self.fixture.output, self.fixture.manifest, self.fixture.license)
        payload = importer.boundary({"geometry": geometry}, "country")
        encoded = IMPORTER.zserio.deserialize_from_bytes(IMPORTER.Boundary, payload)
        blocks = encoded.polygons[0].rings[0].blocks
        self.assertEqual([len(block.points) for block in blocks], [64,64,22])
        expected = {"type": "Polygon", "coordinates": [ring + [ring[0]]]}
        assert_boundary(self, decode_boundary(payload), expected)



if __name__ == "__main__":
    unittest.main()
