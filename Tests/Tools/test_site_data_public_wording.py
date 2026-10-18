#!/usr/bin/env python3
"""Public profile/status claims on newly discovered wiki pages stay governed."""

from __future__ import annotations

import sys
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/site-data"))
import validate as site_validate
from common import SiteDataError, load_contract


class DiscoveredPublicWordingTests(unittest.TestCase):
    def check_wiki_mutation(self, sentence: str, expected: str) -> None:
        contract = load_contract()
        validator = site_validate.Validator(contract)
        source = ROOT / "wiki/subsystems/Audio.md"
        original_read = Path.read_text

        def read(path, *args, **kwargs):
            text = original_read(path, *args, **kwargs)
            return text + "\n\n" + sentence + "\n" if path == source else text

        with mock.patch.object(Path, "read_text", read):
            with self.assertRaises(SiteDataError):
                validator.validate()
        self.assertTrue(any(
            "wiki/subsystems/Audio.md:" in error and expected in error for error in validator.errors
        ), validator.errors)

    def test_new_wiki_production_readiness_claim_is_rejected(self) -> None:
        self.check_wiki_mutation("SparkEngine audio is production-ready.", "forbidden unqualified claim")

    def test_new_wiki_profile_breadth_claim_is_rejected(self) -> None:
        self.check_wiki_mutation("stable-v1 supports all platforms.", "inside the profile")


if __name__ == "__main__":
    unittest.main()
