#!/usr/bin/env python3
"""Check corrupted license metadata against real FPS sources and asset bytes.

Only the parsed integrity document is replaced; source scanning, provenance
policy validation, evidence containment and file hashing execute production code.
"""
import copy
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("module_asset_checker", ROOT / "tools/check-module-asset-refs.py")
CHECKER = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = CHECKER
spec.loader.exec_module(CHECKER)


class LicenseMetadataTests(unittest.TestCase):
    def test_missing_license_fails(self):
        self.check_corruption(None)

    def test_relabelled_license_fails(self):
        self.check_corruption("NOASSERTION")

    def check_corruption(self, value):
        root, entries = CHECKER._integrity_entries(ROOT)
        entries = copy.deepcopy(entries)
        path = "Assets/Audio/ambient_wind.wav"
        if value is None:
            entries[path].pop("license")
        else:
            entries[path]["license"] = value
        with patch.object(CHECKER, "_integrity_entries", return_value=(root, entries)):
            reports, errors = CHECKER.check(ROOT, ["SparkGameFPS"])
        self.assertEqual(errors, [])
        self.assertEqual(len(reports), 1)
        self.assertTrue(reports[0].failed)
        self.assertTrue(any(path in problem and "license differs" in problem
                            for problem in reports[0].integrity_problems), reports[0].integrity_problems)


if __name__ == "__main__":
    unittest.main()
