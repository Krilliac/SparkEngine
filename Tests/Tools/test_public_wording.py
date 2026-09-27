#!/usr/bin/env python3
"""GOV-400: public project wording stays inside the declared license classification.

This suite exercises ``legal_public_wording_errors`` from tools/site-data/validate.py
directly against the live tree. It never constructs the full contract Validator, so
it needs neither git nor a POSIX host and runs on every CTest platform.
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

import validate as site_data_validate  # noqa: E402

CONTENT_PATH = REPO_ROOT / "docs" / "site" / "content.json"


def declared_license() -> dict:
    return json.loads(CONTENT_PATH.read_text(encoding="utf-8"))["legal"]["license"]


class LegalPublicWordingLiveTests(unittest.TestCase):
    """The committed public surfaces satisfy the non-OSI wording guard."""

    def test_declared_license_matches_repository_pin_and_license_file(self) -> None:
        license_data = declared_license()
        self.assertEqual(site_data_validate.CURRENT_LICENSE_DECLARATION["name"], license_data["name"])
        self.assertEqual(site_data_validate.CURRENT_LICENSE_DECLARATION["kind"], license_data["kind"])
        self.assertIs(False, license_data["osiApproved"])
        license_file = REPO_ROOT / license_data["sourcePath"]
        first_line = license_file.read_text(encoding="utf-8").splitlines()[0].strip()
        self.assertEqual(license_data["name"], first_line)

    def test_every_governed_public_claim_surface_is_wording_checked(self) -> None:
        missing = sorted(
            site_data_validate.REQUIRED_GLOBAL_PUBLIC_CLAIM_SURFACES
            - site_data_validate.LEGAL_PUBLIC_WORDING_SURFACES
        )
        self.assertEqual([], missing)
        for contributor_surface in ("CONTRIBUTING.md", "wiki/advanced/Contributing.md"):
            self.assertIn(contributor_surface, site_data_validate.LEGAL_PUBLIC_WORDING_SURFACES)

    def test_live_public_surfaces_carry_no_unreviewed_open_source_wording(self) -> None:
        surfaces: dict[str, str] = {}
        missing: list[str] = []
        for relative in sorted(site_data_validate.LEGAL_PUBLIC_WORDING_SURFACES):
            path = REPO_ROOT / relative
            if not path.is_file():
                missing.append(relative)
                continue
            surfaces[relative] = path.read_text(encoding="utf-8", errors="replace")
        self.assertEqual([], missing, "every wording surface must exist; a missing file is never a pass")
        self.assertEqual(len(site_data_validate.LEGAL_PUBLIC_WORDING_SURFACES), len(surfaces))
        errors = site_data_validate.legal_public_wording_errors(declared_license(), surfaces)
        self.assertEqual([], errors, "\n".join(errors))


class LegalPublicWordingRuleTests(unittest.TestCase):
    """Semantics of the wording rule on synthetic input."""

    def test_unnegated_project_wording_is_rejected_with_its_location(self) -> None:
        for text in ("A C++23 open-source game engine.", "An Open Source engine."):
            with self.subTest(text=text):
                errors = site_data_validate.legal_public_wording_errors(
                    {"osiApproved": False}, {"SECURITY.md": "intro\n" + text}
                )
                self.assertEqual(1, len(errors), errors)
                self.assertIn("SECURITY.md:2:", errors[0])
                self.assertIn("unreviewed open-source wording", errors[0])

    def test_negated_wording_explaining_the_distinction_is_allowed(self) -> None:
        for text in (
            "This project is not open-source under an OSI-approved license.",
            "SparkEngine is never an open source project.",
            "It is no longer an open-source project.",
        ):
            with self.subTest(text=text):
                self.assertEqual(
                    [],
                    site_data_validate.legal_public_wording_errors({"osiApproved": False}, {"a.md": text}),
                )

    def test_every_unnegated_occurrence_on_a_line_is_reported(self) -> None:
        errors = site_data_validate.legal_public_wording_errors(
            {"osiApproved": False},
            {"a.md": "Not open-source, but the open-source community likes it."},
        )
        self.assertEqual(1, len(errors), errors)

    def test_non_text_surface_is_an_error(self) -> None:
        errors = site_data_validate.legal_public_wording_errors({"osiApproved": False}, {"a.md": None})
        self.assertEqual(["a.md: legal wording source must be text"], errors)

    def test_guard_applies_only_to_an_explicit_non_osi_declaration(self) -> None:
        text = {"README.md": "A C++23 open-source game engine."}
        self.assertEqual([], site_data_validate.legal_public_wording_errors({"osiApproved": True}, text))
        self.assertEqual([], site_data_validate.legal_public_wording_errors({}, text))
        self.assertEqual([], site_data_validate.legal_public_wording_errors(None, text))


if __name__ == "__main__":
    unittest.main()
