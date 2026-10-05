# Gazetteer Data

plazs prepares one read-only SQLite artifact with metadata, a searchable name index,
and zserio-packed boundaries. The runtime has no GIS or ingestion dependencies.

## Download And Pin

Obtain the global administrative SQLite export from
[Geocode Earth's WOF downloads](https://geocode.earth/data/whosonfirst/combined/).
The inventory is at `https://data.geocode.earth/wof/dist/sqlite/inventory.json`.
Country extracts use the same format and work with the same importer.

Keep downloads outside the source checkout. Save the complete inventory entry for
`whosonfirst-data-admin-latest.db.bz2` as `snapshot.json`, including the source
commit, member repositories, sizes and both SHA-256 hashes. `latest` is mutable:
an inventory/file mismatch must fail, not silently become a different release.

For example, after saving that inventory entry:

```sh
curl --http1.1 -fL --retry 5 --retry-all-errors -C - \
  https://data.geocode.earth/wof/dist/sqlite/whosonfirst-data-admin-latest.db.bz2 \
  -o whosonfirst-data-admin-latest.db.bz2.part
python3 - <<'PY'
import hashlib, json
from pathlib import Path
manifest = json.loads(Path('snapshot.json').read_text())
archive = Path('whosonfirst-data-admin-latest.db.bz2.part')
digest = hashlib.sha256()
with archive.open('rb') as stream:
    for block in iter(lambda: stream.read(8 * 1024 * 1024), b''):
        digest.update(block)
if archive.stat().st_size != manifest['size_compressed'] or digest.hexdigest() != manifest['sha256_compressed']:
    raise SystemExit('Archive does not match pinned inventory')
archive.rename('whosonfirst-data-admin-latest.db.bz2')
PY
bzip2 -dc whosonfirst-data-admin-latest.db.bz2 > whosonfirst-data-admin-latest.db
```

The importer verifies the **uncompressed** file against the pinned size/hash too,
so interrupted decompression cannot publish an artifact. `lbzip2 -n 8 -dc` is an
optional faster decompressor; neither decompressor is part of the runtime.
Allow space for the archive, unpacked database and derived artifact concurrently.
The inventory inspected on 2026-10-05 advertises an 8.6 GB archive and 42.4 GB
database, with an October 2025 export vintage; download date is not data freshness.

Retain the upstream `LICENSE.md` from the WOF data repositories alongside the
snapshot. WOF's own work is CC0, but incorporated sources have additional
attribution requirements: do not label the combined dataset simply "CC0". See
the [WOF license inventory](https://whosonfirst.org/docs/licenses/) and the
[data repository license](https://github.com/whosonfirst-data/whosonfirst-data-admin-de/blob/master/LICENSE.md).
The importer embeds that supplied text unchanged and aggregates distinct
`src:*`/`qs_source` credits once in dataset metadata. Distribute those notices with derived artifacts
and show the returned attribution/license link when consuming the data.

## Prepare

Build plazs and install `tools/requirements.txt` in an isolated Python environment.
The CMake `add_zserio_library` helper generates C++ and Python from the same
`schema/plazs_data.zs`. Use those generated modules, not a separately maintained codec.

```sh
PYTHONPATH=build/python .venv/bin/python tools/import_whosonfirst.py \
  /data/wof/whosonfirst-data-admin-latest.db /data/wof/places.sqlite \
  --manifest /data/wof/snapshot.json --license /data/wof/LICENSE.md \
  --min-locality-population 5000 --simplify-degrees 0.001
```

On Windows set `PYTHONPATH=build/python` in the environment and use the venv's
`Scripts/python.exe`. Preparing the global dataset is an explicit offline task,
never part of ordinary library compilation or application startup.

Defaults retain continents, countries, dependencies, macroregions, regions and
localities, all live name languages, and unknown-current records. Alternate,
deprecated, ceased, superseded and known-inactive records are excluded.

Selection options:

- `--placetypes country region locality`: indexed categories.
- `--geometry-placetypes country region`: categories retaining actual boundaries;
  an empty list retains names/extents only.
- `--languages eng deu fra`: additional alias languages. Primary English and the
  original name remain. This is data selection, not lossless compression.
- `--min-locality-population 5000`: exclude smaller/unknown-population localities;
  other categories remain. Prefer valid `wof:population`, then `gn:population`.
- `--simplify-degrees 0.001`: optional topology-preserving angular simplification,
  before quantization. Default zero. This is not a metre-based error guarantee.
- `--coordinate-shift 7`: round NDS coordinates divided by `2^7` before packing.
  Default 7, supported 0..16. Zero retains NDS precision; seven gives approximately
  1.2-metre grid spacing at the equator, finer longitudinal spacing at higher latitudes.

Quantization is deliberately lossy. The default per-axis rounding error is about
0.6 metres at the equator, separate from simplification and source inaccuracies.
These are navigation/selection boundaries, not authoritative legal geometry.
The population filter does not guarantee completeness: upstream populations may
be absent or stale. Download date is not data freshness.

## Geometry Validation

The importer repairs invalid upstream areas with `make_valid`, optionally
simplifies them, converts through ndslive-math, and rounds to the chosen NDS grid.
It drops consecutive duplicates and repeated closure vertices. Rings with fewer
than three distinct points disappear. If an exterior collapses, its component is
removed; if the whole area collapses, the place remains searchable with its
original extent but without an advertised polygon.

Quantization can cause new intersections or merge holes with exteriors. Repair in
integer-grid space, snap repair intersections with GEOS valid-output precision
reduction, and validate the resulting MultiPolygon before encoding. Discard
non-area components; never fabricate a polygon from a degenerate ring or bbox.
Original MultiPolygon containers remain MultiPolygons even with one component.

The reader clamps a rounded positive dateline/pole to the last representable NDS
coordinate rather than overflowing into the opposite hemisphere. Metadata bounds
remain the upstream WGS84 values, including full-world/dateline-crossing boxes.

The artifact records library versions, schema checksum, grid, simplification,
selection, repairs and collapsed components. Any unresolved invalid geometry,
failed checksum or oversized packed boundary aborts publication atomically.

## Format 1

SQLite application ID `0x504C5A53` (PLZS), user version **1**. No legacy codecs.
Applications must reject an unsupported format and rebuild/update their artifact.

- `place`: WOF integer ID, display name, country, numeric place type, full-precision
  NDS representative coordinate, original WGS84 bounds and optional population.
- `place_fts`: contentless FTS5, Unicode61 with diacritic removal, `detail=none`,
  no stored positions or document-length table. Queries tokenize identically and
  intersect literal token prefixes. SQL/FTS syntax is not exposed to callers.
- `boundary`: WOF ID and a `(chunk, offset, size)` span. This index is necessary
  for random access, not a duplicate byte count alongside a per-place BLOB.
- `geometry_chunk`: normally at most 4 MiB of contiguous independent boundaries.
  One larger boundary may occupy its own chunk, up to the 16 MiB decoder limit.
  Native incremental BLOB reads allocate only the requested span, not the slab.
- `metadata`: snapshot, selection, coordinate shift, place-type dictionary,
  supplied license, aggregate source credits and import statistics.

Every boundary has ordered polygons/rings and packed absolute coordinates in
blocks of at most 64 vertices. Zserio performs per-field delta packing. Closures
are implicit; exterior rings precede holes. There is no per-place JSON `details`
column and no secondary compression envelope.

The native reader caps packed spans at 16 MiB, temporary generated-object
allocations at 32 MiB and decoded coordinates at one million vertices. It validates
spans, chunk availability, block sizes, coordinates and final zero padding. It
never truncates a boundary to meet these limits. Expanded GeoJSON can still be
much larger than its packed representation.

## Publication And Reproducibility

The importer verifies the complete input SHA-256 before reading, streams the
selection with bounded SQLite caches, buffers at most one geometry chunk,
optimizes FTS, checks span integrity, vacuums and atomically replaces its output.
The previous good artifact survives failed imports. Identical input/settings/tools
produce deterministic bytes; different GEOS/SQLite versions may change bytes.

Track prepared data with **Git LFS**, never ordinary Git blobs. The global profile
is `data/places.sqlite`; recipe, snapshot, SHA-256 and notices accompany it. Do not
check in the multi-gigabyte raw WOF export. `tests/data/places.sqlite` is synthetic
and small, but uses the same LFS policy. CI must checkout with `lfs: true`.
The prepared profile is also published as a checksum-verified release asset.

The first prepared profile is
[`wof-20251014-format1`](https://github.com/Klebert-Engineering/plazs/releases/tag/wof-20251014-format1):
64,459 places and 47,957 boundaries, all alias languages, localities with known
population of at least 5,000, `--simplify-degrees 0.001 --coordinate-shift 7`.
Its download date does not make the October 2025 upstream snapshot newer.

Consumers may explicitly include `cmake/dataset.cmake` and call
`plazs_fetch_dataset(output_path)` to obtain this prepared SQLite file. The helper
uses the Git LFS checkout if present, otherwise downloads/caches the identical
release asset with pinned SHA-256 and serialized concurrent downloads. Building
the library itself does not invoke this helper. A custom offline artifact can
be passed directly to the reader instead.

## Storage Decisions

The original compact mapget artifact measured 58.78 MB: 45.25 MB geometry table,
8.60 MB FTS and 4.86 MB place records. Its packed geometry payload was 41.54 MB.

Measured alternatives on that dataset:

- Dense internal FTS document IDs save 1.45 MB in FTS, but their additional public-ID
  mapping/index leaves only 0.43 MB total savings. Keep the single identity index.
- A MARISA dictionary with association tables estimates a 6.32 MB search index.
  The tested DAWG variants were larger. Retain FTS5 rather than add a second Unicode
  tokenizer, new runtime dependency and search implementation for about 2.3 MB.
- Shared geometry chunks save 2.79 MB without changing any coordinates.
- A seven-bit coordinate shift plus topology cleanup reduces the packed geometry
  payload from 41.54 MB to 27.84 MB in the completed global import.
- Per-boundary Zstandard slightly increases the already packed payload. Whole-file
  compression can reduce download size, but not the installed database size.

These decisions prioritize measured savings and a small runtime. A different
search index can be revisited if larger inventories justify it, without changing
mapget or its public REST/MCP contract.

The validated format-1 artifact is **42,258,432 bytes** (42.26 MB / 40.30 MiB),
28.1% smaller than the previous 58,781,696-byte NDS-precision artifact:

| Ownership (SQLite allocated pages) | Bytes |
| --- | ---: |
| Geometry chunks | 27,877,376 |
| Boundary span index | 851,968 |
| FTS5 search index and configuration | 8,597,504 |
| Place records | 4,857,856 |
| Metadata, its index and schema | 73,728 |

The same number of places and boundaries remains. This does not mean identical
geometry: 402 collapsed rings and 71 collapsed components were removed; 157
quantized boundaries needed repair. All 64,459 IDs were resolved with the native
reader and all 47,957 decoded polygons passed GEOS validity checks. Decoded areas
contain 24,703 holes; the largest boundary has 194,861 vertices including closures,
within the native reader's budget. These checks prove structural validity, not
survey accuracy or a metre-level guarantee for the simplified/source geometry.

## Tests

`ctest --test-dir build --output-on-failure` covers native search/ranking, Unicode
prefix matching, bounds, geometry decoding, corrupt spans and allocation bombs;
and offline selection, aliases, provenance, atomic publication, deterministic
output, quantization, repairs, holes, islands and chunk rollover.

All test data is synthetic. The native query tool provides an integration point
for real-artifact audits and downstream HTTP adapters. Global data validation
is separate from these fast, network-free tests.
