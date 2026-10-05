# plazs

An offline place-name and boundary gazetteer. A read-only C++ library serves a
prepared SQLite artifact; an offline Python importer selects, repairs and packs
Who's On First (WOF) data. No map server, network geocoder or viewer is included.

- Unicode token-prefix search, including multilingual aliases and country codes.
- Stable WOF identities, country/region/locality metadata, coordinates and extents.
- On-demand Polygon/MultiPolygon retrieval with holes and islands.
- Quantized NDS coordinates in zserio-packed blocks, with bounded decoder memory.
- Checksum-pinned input, atomic publication, embedded provenance and source credits.

## Build

Requirements: CMake 3.20+, a C++20 compiler, Java for schema generation, and Python
3.10+ for ingestion/tests. The deployed reader does not require Java, Python or GEOS.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r tools/requirements.txt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 8
ctest --test-dir build --output-on-failure
```

On Windows activate `.venv/Scripts/Activate.ps1`, build with `--config Release`
and pass `-C Release` to CTest. Dependencies are pinned in
`cmake/dependencies.cmake`; parent-provided targets are reused.

Consume the source with CMake `add_subdirectory`/CPM and link `plazs::plazs`.
Set `PLAZS_BUILD_TESTS=OFF` and `PLAZS_BUILD_TOOLS=OFF` in a consuming project.
The public API is `include/plazs/gazetteer.h`:

```cpp
#include <plazs/gazetteer.h>

plazs::Gazetteer places("places.sqlite");
auto matches = places.search("Munchen DE", 5); // Metadata only; prefix-AND tokens.
auto germany = places.find(85633111);         // Optional place, including its boundary.
```

The same instance supports concurrent calls. Construction rejects missing or
unsupported artifacts; query errors never masquerade as an empty successful
result. Invalid IDs return no match. Search does not load geometry. Full lookup
reads only one boundary span, not an entire geometry chunk or dataset.

The optional tool prints JSON without mapget-specific identifiers or labels:

```sh
build/plazs-query places.sqlite search 'California'
build/plazs-query places.sqlite id 85633111
```

## Data

The [prepared global profile](https://github.com/Klebert-Engineering/plazs/releases/tag/wof-20251014-format1)
is 42.26 MB: 64,459 places and 47,957 boundaries. It retains countries/regions and
localities with a known population of at least 5,000, from an October 2025 WOF
snapshot. It is intentionally not a complete inventory of every locality.

See [data preparation](docs/data.md) for the pinned input, profile, format,
precision tradeoffs and validation. Prepared artifacts belong in release assets,
not Git. `tests/data/places.sqlite` is a tiny synthetic fixture, not real data.

The default boundary grid rounds away seven NDS low bits: approximately 1.2 metres
at the equator. Additional simplification is opt-in for the importer; the prepared
global profile uses 0.001-degree simplification. This is suitable for place
navigation/selection, not cadastral or authoritative boundary measurement.
Collapsed rings and islands may disappear; they are never replaced by fake areas.

## License And Attribution

Code is BSD-3-Clause; see [LICENSE](LICENSE). The reader/importer originated in
[mapget](https://github.com/ndsev/mapget); its copyright notice is retained.

Data has **separate licensing**. WOF's own work is CC0, but incorporated sources
have additional attribution requirements. Prepared artifacts embed the supplied
upstream license and aggregated source credits in `metadata`. Distribute these
notices with the data and expose attribution to users. See
[WOF's license inventory](https://whosonfirst.org/docs/licenses/).
