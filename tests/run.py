#!/usr/bin/env python3
"""Run ingestion tests with the matching generated schema on every build platform."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, sys.argv.pop(1))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
unittest.main(module="test_import")
