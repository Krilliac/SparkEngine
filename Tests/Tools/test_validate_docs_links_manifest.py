import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

import validate_docs_links as links


class TrackedManifestTests(unittest.TestCase):
    def test_manifest_preserves_merged_directory_casing(self) -> None:
        with tempfile.TemporaryDirectory(prefix="docs-tracked-manifest-") as temporary:
            root = Path(temporary)
            (root / "Tools").mkdir()
            (root / "Tools" / "x.c").write_text("x", encoding="utf-8")
            (root / "Tools" / "y.c").write_text("y", encoding="utf-8")
            manifest = root / ".docs-tracked-files"
            # The logical lowercase spelling is the tracked spelling even though
            # the case-insensitive fixture has one physical Tools/ directory.
            manifest.write_bytes(b"tools/x.c\0Tools/y.c\0")
            with mock.patch.dict(os.environ, {"SPARK_DOC_TRACKED_PATHS": str(manifest)}, clear=False):
                tracked = links.load_tracked_tree(root)
            self.assertIsNotNone(tracked)
            self.assertTrue(links.exact_case(root / "tools" / "x.c", root, tracked))
            self.assertTrue(links.exact_case(root / "Tools" / "y.c", root, tracked))
            self.assertFalse(links.exact_case(root / "Tools" / "x.c", root, tracked))
            self.assertFalse(links.exact_case(root / "TOOLS" / "x.c", root, tracked))

    def test_manifest_malformed_input_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="docs-tracked-manifest-") as temporary:
            root = Path(temporary)
            manifest = root / ".docs-tracked-files"
            for payload in (b"Tools/x.c", b"", b"\0", b"Tools/x.c\0\0", b"\xff\0", b"../x.c\0",
                            b"Tools/x.c\0Tools/x.c\0"):
                with self.subTest(payload=payload):
                    manifest.write_bytes(payload)
                    with mock.patch.dict(os.environ, {"SPARK_DOC_TRACKED_PATHS": str(manifest)}, clear=False):
                        with self.assertRaises(links.LinkContractError):
                            links.load_tracked_tree(root)

    def test_manifest_rejects_casefold_duplicate_and_outside_path(self) -> None:
        with tempfile.TemporaryDirectory(prefix="docs-tracked-manifest-") as temporary:
            root = Path(temporary)
            manifest = root / ".docs-tracked-files"
            manifest.write_bytes(b"Tools/x.c\0tools/x.c\0")
            with mock.patch.dict(os.environ, {"SPARK_DOC_TRACKED_PATHS": str(manifest)}, clear=False):
                with self.assertRaises(links.LinkContractError):
                    links.load_tracked_tree(root)
            outside = root.parent / (root.name + "-outside")
            outside.write_bytes(b"Tools/x.c\0")
            try:
                with mock.patch.dict(os.environ, {"SPARK_DOC_TRACKED_PATHS": str(outside)}, clear=False):
                    with self.assertRaises(links.LinkContractError):
                        links.load_tracked_tree(root)
            finally:
                outside.unlink()


if __name__ == "__main__":
    unittest.main()
