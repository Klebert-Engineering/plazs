# Synthetic Gazetteer

`places.sqlite` is generated from `WofFixture` in `tests/wof_fixture.py`. Names and
simple polygons exercise the contract; these are not accurate country boundaries
or licensed sample-map data. Mapget uses this artifact to test its REST/MCP
adapter without installing GIS tools or duplicating the importer.

After changing the format/importer, regenerate with the matching schema:

```sh
PYTHONPATH=build/python:tools:tests .venv/bin/python tools/make_test_data.py
```

The ingestion tests check that the checked-in fixture still matches the recipe.
