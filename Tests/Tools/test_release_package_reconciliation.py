#!/usr/bin/env python3
"""Fail-closed checks for final-package evidence and publication byte coverage."""
import copy
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("release_package_reconciliation",
                                            ROOT / "tools/release_package_reconciliation.py")
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)
SOURCE = {"sha": "a" * 40, "dependencyLockSha256": "b" * 64}
NAME = "SparkEngine-1.0.0-Windows-AMD64-MinSizeRel.zip"
DIGEST = "c" * 64


def report():
    return {"schema": tool.SCHEMA, "platform": "Windows", "configuration": "MinSizeRel",
            "source": SOURCE.copy(), "errors": [], "artifacts": [{
                "name": NAME, "sha256": DIGEST, "inventory": {
                    "schema": "spark-package-inventory-reconciliation-v1", "packageFiles": 12,
                    "errors": [], "source": SOURCE.copy(),
                    "artifact": {"name": NAME, "sha256": DIGEST, "format": "zip"}}}]}


class FinalPackageReconciliationTests(unittest.TestCase):
    def verify(self, documents=None, published=None):
        tool.verify_documents([report()] if documents is None else documents,
                              {NAME: DIGEST} if published is None else published, SOURCE)

    def test_accepts_exact_final_bytes_and_identical_alias(self):
        self.verify()
        self.verify(published={NAME: DIGEST, "SparkEngine-Windows-x64-Release.zip": DIGEST})

    def test_missing_extra_changed_and_alias_only_publication_are_rejected(self):
        for published in ({}, {NAME: "d" * 64}, {NAME: DIGEST, "foreign.exe": "d" * 64}, {"alias.zip": DIGEST}):
            with self.subTest(published=published), self.assertRaises(ValueError):
                self.verify(published=published)

    def test_empty_duplicate_and_unknown_report_schemas_are_rejected(self):
        for documents in ([], [report(), report()], [{**report(), "unverified": True}]):
            with self.subTest(documents=documents), self.assertRaises(ValueError):
                self.verify(documents)

    def test_source_and_lock_drift_are_rejected(self):
        for key in SOURCE:
            document = report()
            document["source"][key] = "f" * len(SOURCE[key])
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "source/lock"):
                self.verify([document])

    def test_missing_failed_and_wrong_byte_inventories_are_rejected(self):
        for kind in ("missing", "failed", "wrong-bytes", "wrong-name", "empty"):
            document = report()
            entry = document["artifacts"][0]
            if kind == "missing":
                entry["inventory"] = None
            elif kind == "failed":
                entry["inventory"]["errors"] = ["unmapped dependency"]
            elif kind == "empty":
                entry["inventory"]["packageFiles"] = 0
            elif kind == "wrong-name":
                entry["inventory"]["artifact"]["name"] = "other.zip"
            else:
                entry["inventory"]["artifact"]["sha256"] = "e" * 64
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                self.verify([document])

    def test_installer_outer_hash_cannot_stand_in_for_final_inventory(self):
        document = report()
        document["errors"] = ["final native/standalone payload inventory has not been verified"]
        with self.assertRaisesRegex(ValueError, "blocked or failed"):
            self.verify([document])

    def test_record_reports_unverified_native_payload_instead_of_accepting_stage(self):
        package = Path("packages/SparkEngine-1.0.0-Runtime.msi")
        fake_sbom = unittest.mock.Mock()
        fake_sbom.load_inventory.return_value = []
        fake_sbom.load_rules.return_value = object()
        with patch.object(tool, "load_tool", return_value=fake_sbom), \
                patch.object(tool, "source_identity", return_value=SOURCE), \
                patch.object(Path, "iterdir", return_value=iter([package])), \
                patch.object(tool, "regular", side_effect=lambda path: path), \
                patch.object(tool.provenance, "_sha256_file", return_value=DIGEST):
            result = tool.create(Path("packages"), ROOT, SOURCE["sha"], "Windows", "MinSizeRel", [])
        self.assertTrue(result["errors"])
        self.assertIsNone(result["artifacts"][0]["inventory"])
        fake_sbom.reconcile_archive.assert_not_called()

    def test_unsafe_artifact_names_and_duplicate_json_fields_are_rejected(self):
        for name in ("../payload.zip", "C:payload.zip", "a/b.zip", "file.zip\n"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                tool.safe_name(name)
        with self.assertRaises(ValueError):
            tool.unique_object([("errors", []), ("errors", ["failure"])])


if __name__ == "__main__":
    unittest.main()
