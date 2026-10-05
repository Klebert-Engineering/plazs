#!/usr/bin/env python3
"""Rebuild the small synthetic adapter fixture; no downloaded data is committed."""
from pathlib import Path
import shutil
import tempfile
from wof_fixture import WofFixture


def main():
    """Build atomically through the production importer, then publish the test artifact."""
    with tempfile.TemporaryDirectory() as directory:
        fixture = WofFixture(directory)
        fixture.prepare()
        shutil.copyfile(fixture.output, Path(__file__).resolve().parents[1] / "tests/data/places.sqlite")


if __name__ == "__main__":
    main()
