#!/usr/bin/env python3
"""Public wording guards from tools/site-data/validate.py.

GOV-400 (PublicWording_LegalSurfaces): project wording stays inside the declared
license classification (``legal_public_wording_errors``).
OD-12 (PublicWording_DeferredPlatforms, PLT-230/240/250): mobile, OpenXR and
console stay unsupported in the contract and in public wording
(``deferred_platform_support_errors``, ``deferred_platform_claim_errors``).
OD-10/OD-11 (PublicWording_ExperimentalPlatforms, PLT-210/220): the Linux and
macOS rows stay experimental in the contract and are never summarized as
supported (``experimental_platform_support_errors``, ``experimental_platform_claim_errors``).
PLT-250 (PublicWording_ConsoleCertification): no CI, build configuration, doc or
source implies console certification (``console_certification_implication_errors``).

Each CTest selects its classes by name and checks the live tree directly. It never constructs the full contract Validator, so
it needs neither git nor a POSIX host and runs on every CTest platform.
"""

from __future__ import annotations

import copy
import json
import re
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "site-data"))

import validate as site_data_validate  # noqa: E402
from common import load_contract  # noqa: E402

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


class DeferredPlatformWordingTests(unittest.TestCase):
    """OD-12 (PLT-230/240/250): public wording never claims mobile, OpenXR or console support."""

    def errors(self, text: str, location: str = "README.md") -> list[str]:
        return site_data_validate.deferred_platform_claim_errors({location: text})

    def test_unqualified_support_claims_are_rejected_with_their_location(self) -> None:
        for text in (
            "SparkEngine supports iOS and Android.",
            "The engine runs on Xbox and PlayStation.",
            "Games deploy to Meta Quest headsets.",
            "SparkEngine provides a mobile platform abstraction for iOS and Android.",
            "| Platform | Status |\n| Nintendo Switch | Supported on the current release |",
        ):
            with self.subTest(text=text):
                errors = self.errors("intro\n" + text)
                self.assertEqual(1, len(errors), errors)
                self.assertRegex(errors[0], r"^README\.md:[23]: ")
                self.assertIn("OD-12", errors[0])

    def test_qualified_boundary_wording_is_allowed(self) -> None:
        for text in (
            "Console support is planned (PLT-250).",
            "SparkEngine does not support iOS or Android.",
            "Mobile platforms are unsupported and deferred from stable-v1 (OD-12, PLT-230).",
            "VR is a framework stub designed for OpenXR runtimes such as SteamVR.",
            "| iOS | Supported on the roadmap only |",
        ):
            with self.subTest(text=text):
                self.assertEqual([], self.errors(text))

    def test_rule_is_per_sentence_not_per_line(self) -> None:
        errors = self.errors("Console support is planned. SparkEngine ships on Xbox today.")
        self.assertEqual(1, len(errors), errors)

    def test_developer_console_and_fenced_code_are_not_platform_claims(self) -> None:
        self.assertEqual([], self.errors("The engine provides console commands for module inspection."))
        self.assertEqual([], self.errors("```cmake\nset(X ON CACHE BOOL \"Enable Android platform support\")\n```"))

    def test_non_text_surface_is_an_error(self) -> None:
        self.assertEqual(
            ["a.md: deferred-platform wording source must be text"],
            site_data_validate.deferred_platform_claim_errors({"a.md": None}),
        )

    def test_live_governed_surfaces_carry_no_deferred_platform_claim(self) -> None:
        contract = load_contract()
        surfaces, missing = site_data_validate.deferred_platform_claim_surfaces(REPO_ROOT, contract)
        self.assertEqual([], missing, "every governed surface must exist; a missing file is never a pass")
        for page in ("wiki/platform/Mobile-Platform.md", "wiki/platform/VR-Support.md", "docs/plans/FEATURE_ROADMAP.md"):
            self.assertIn(page, surfaces)
        self.assertTrue(site_data_validate.REQUIRED_GLOBAL_PUBLIC_CLAIM_SURFACES <= surfaces.keys())
        errors = site_data_validate.deferred_platform_claim_errors(surfaces)
        self.assertEqual([], errors, "\n".join(errors))


class ConsoleCertificationImplicationTests(unittest.TestCase):
    """PLT-250: no CI, build configuration, doc or source implies console certification."""

    def errors(self, text: str, location: str) -> list[str]:
        return site_data_validate.console_certification_implication_errors({location: text})

    def test_console_ci_and_cmake_identifiers_are_rejected_with_their_location(self) -> None:
        cases = (
            (".github/workflows/build.yml", "jobs:\n  build:\n    runs-on: [self-hosted, ps5]"),
            (".github/workflows/release.yml", "strategy:\n  matrix:\n    platform: [windows, xbox-series]"),
            (".github/workflows/nightly.yaml", "jobs:\n  build-gdkx:\n    runs-on: windows-2022"),
            ("CMakeLists.txt", 'intro\noption(ENABLE_PLAYSTATION "Build the console target" OFF)'),
            ("cmake/Platforms.cmake", "intro\nset(SPARK_TARGET_NINTENDO ON)"),
            ("CMakePresets.json", '{\n  "configurePresets": [{"name": "scarlett-release"}]'),
        )
        for location, text in cases:
            with self.subTest(location=location):
                errors = self.errors(text, location)
                self.assertEqual(1, len(errors), errors)
                self.assertRegex(errors[0], rf"^{re.escape(location)}:[23]: ")
                self.assertIn("PLT-250", errors[0])

    def test_ci_comments_and_unrelated_identifiers_are_not_console_lanes(self) -> None:
        text = "# No PS5 or Xbox runner exists (PLT-250).\njobs:\n  build-windows:\n    runs-on: windows-2022\n"
        self.assertEqual([], self.errors(text, ".github/workflows/build.yml"))
        self.assertEqual([], self.errors("option(ENABLE_SWITCH_STATEMENT_CHECK OFF)", "CMakeLists.txt"))

    def test_unqualified_certification_wording_is_rejected_in_docs_and_source(self) -> None:
        cases = (
            ("README.md", "The engine passes PlayStation certification."),
            ("wiki/platform/Accessibility.md", "| **PlayStation Certification** | Subtitle support |"),
            ("docs/guides/Consoles.md", "Builds are Xbox certified and meet XR-015."),
            ("SparkEngine/Source/Core/Platform.h", "// Satisfies Nintendo lotcheck requirements"),
            ("wiki/Build-Guide.md", "Games pass console certification on day one."),
        )
        for location, text in cases:
            with self.subTest(location=location):
                errors = self.errors("intro\n" + text, location)
                self.assertEqual(1, len(errors), errors)
                self.assertRegex(errors[0], rf"^{re.escape(location)}:2: ")
                self.assertIn("implies console certification", errors[0])

    def test_qualified_or_unrelated_wording_is_allowed(self) -> None:
        for text in (
            "PlayStation certification is not available (PLT-250).",
            "Xbox certification is planned and needs platform agreements.",
            "| **PlayStation accessibility guidance** (reference only; console support is planned, PLT-250) | x |",
            "The Windows installer certification covers MSI signing.",
            "SparkConsole.exe is blocked and uncertified.",
            "Xbox controller layout for gamepad input.",
        ):
            with self.subTest(text=text):
                self.assertEqual([], self.errors(text, "README.md"))

    def test_rule_is_per_sentence_not_per_line(self) -> None:
        errors = self.errors(
            "PlayStation certification is planned. Xbox certification is complete.", "README.md"
        )
        self.assertEqual(1, len(errors), errors)
        self.assertIn("'Xbox'", errors[0])

    def test_non_text_surface_is_an_error(self) -> None:
        self.assertEqual(
            ["a.md: console-certification source must be text"],
            site_data_validate.console_certification_implication_errors({"a.md": None}),
        )

    def test_live_tree_implies_no_console_certification(self) -> None:
        surfaces = site_data_validate.console_certification_surfaces(REPO_ROOT)
        for governed in (
            ".github/workflows/build.yml",
            ".github/workflows/release.yml",
            "CMakeLists.txt",
            "CMakePresets.json",
            "README.md",
            "wiki/platform/Accessibility.md",
            "SparkEngine/Source/Engine/OnlineServices/OnlineServices.h",
        ):
            self.assertIn(governed, surfaces, "a governed surface that is not scanned is never a pass")
        self.assertTrue(site_data_validate.REQUIRED_GLOBAL_PUBLIC_CLAIM_SURFACES <= surfaces.keys())
        self.assertFalse(any(path.startswith("wiki/reference/") for path in surfaces))
        errors = site_data_validate.console_certification_implication_errors(surfaces)
        self.assertEqual([], errors, "\n".join(errors))


class ExperimentalPlatformWordingTests(unittest.TestCase):
    """OD-10/OD-11 (PLT-210/PLT-220): experimental Linux and macOS rows are never summarized as supported."""

    def errors(self, text: str, location: str = "README.md") -> list[str]:
        return site_data_validate.experimental_platform_claim_errors({location: text})

    def test_unqualified_support_claims_are_rejected_with_their_location(self) -> None:
        for text in (
            "SparkEngine supports Linux.",
            "Runs on Ubuntu 24.04 out of the box.",
            "macOS is fully supported.",
            "The engine ships on Apple Silicon.",
            "| Linux | Supported on every release |",
        ):
            with self.subTest(text=text):
                errors = self.errors("intro\n" + text)
                self.assertEqual(1, len(errors), errors)
                self.assertRegex(errors[0], r"^README\.md:2: ")
                self.assertIn("PLT-210/PLT-220", errors[0])

    def test_qualified_or_scoped_wording_is_allowed(self) -> None:
        for text in (
            "Linux is experimental and outside stable-v1.",
            "SparkEngine builds on Linux in CI.",
            "macOS support is deferred (OD-11).",
            "The sanitizer lanes run on Ubuntu 24.04.",
            "The declared Linux support matrix is still open.",
            "Smoke tests run on all hosts, including a MacOS path.",
            "The GPU supportsRaytracing query exists on macOS 12.",
        ):
            with self.subTest(text=text):
                self.assertEqual([], self.errors(text))

    def test_rule_is_per_sentence_not_per_line(self) -> None:
        errors = self.errors("Linux is experimental. SparkEngine supports macOS.")
        self.assertEqual(1, len(errors), errors)
        self.assertIn("'macOS'", errors[0])

    def test_fenced_code_is_not_a_claim(self) -> None:
        self.assertEqual([], self.errors("```text\nSparkEngine supports Linux.\n```"))

    def test_non_text_surface_is_an_error(self) -> None:
        self.assertEqual(
            ["a.md: experimental-platform wording source must be text"],
            site_data_validate.experimental_platform_claim_errors({"a.md": None}),
        )

    def test_live_governed_surfaces_summarize_no_experimental_row_as_supported(self) -> None:
        contract = load_contract()
        surfaces, missing = site_data_validate.experimental_platform_claim_surfaces(REPO_ROOT, contract)
        self.assertEqual([], missing, "every governed surface must exist; a missing file is never a pass")
        for page in ("wiki/platform/System-Requirements.md", "wiki/platform/Cross-Compilation-Wine-Testing.md"):
            self.assertIn(page, surfaces)
        self.assertTrue(site_data_validate.REQUIRED_GLOBAL_PUBLIC_CLAIM_SURFACES <= surfaces.keys())
        errors = site_data_validate.experimental_platform_claim_errors(surfaces)
        self.assertEqual([], errors, "\n".join(errors))


def capability_of(contract: dict, identifier: str) -> dict:
    return next(c for c in contract["readiness"]["capabilities"] if c["id"] == identifier)


class DeferredPlatformSupportTests(unittest.TestCase):
    """The readiness contract keeps deferred platforms unsupported and blocked while their item is open."""

    def setUp(self) -> None:
        self.contract = load_contract()

    def test_live_contract_passes(self) -> None:
        self.assertEqual(3, len(site_data_validate.DEFERRED_PLATFORMS))
        self.assertEqual([], site_data_validate.deferred_platform_support_errors(self.contract))

    def test_flipping_support_or_release_is_rejected_while_the_item_is_open(self) -> None:
        cases = (
            ("platform.mobile", "support", "supported"),
            ("platform.vr", "support", "experimental"),
            ("platform.console", "support", "experimental"),
            ("platform.console", "release", "candidate"),
        )
        for identifier, field, value in cases:
            with self.subTest(capability=identifier, field=field):
                contract = copy.deepcopy(self.contract)
                capability_of(contract, identifier)[field] = value
                errors = site_data_validate.deferred_platform_support_errors(contract)
                self.assertEqual(1, len(errors), errors)
                self.assertIn(f"capabilities.{identifier}: {field} is {value!r}", errors[0])

    def test_console_platform_authority_record_is_rejected(self) -> None:
        capability_of(self.contract, "platform.console")["platformAuthority"] = {"owner": "someone"}
        errors = site_data_validate.deferred_platform_support_errors(self.contract)
        self.assertEqual(1, len(errors), errors)
        self.assertIn("OWNER-DECISIONS.md", errors[0])

    def test_done_item_lifts_the_deferral(self) -> None:
        capability_of(self.contract, "platform.mobile")["support"] = "supported"
        next(i for i in self.contract["workItems"] if i["id"] == "PLT-230")["status"] = "done"
        self.assertEqual([], site_data_validate.deferred_platform_support_errors(self.contract))

    def test_missing_capability_or_item_is_an_error(self) -> None:
        self.contract["readiness"]["capabilities"] = [
            c for c in self.contract["readiness"]["capabilities"] if c["id"] != "platform.vr"
        ]
        self.contract["workItems"] = [i for i in self.contract["workItems"] if i["id"] != "PLT-250"]
        errors = site_data_validate.deferred_platform_support_errors(self.contract)
        self.assertEqual(2, len(errors), errors)


class ExperimentalPlatformSupportTests(unittest.TestCase):
    """The readiness contract keeps experimental hosts experimental and unpromoted while their item is open."""

    def setUp(self) -> None:
        self.contract = load_contract()

    def test_live_contract_passes(self) -> None:
        self.assertEqual({"platform.linux", "platform.macos"}, set(site_data_validate.EXPERIMENTAL_PLATFORMS))
        self.assertEqual([], site_data_validate.experimental_platform_support_errors(self.contract))

    def test_promoting_an_open_experimental_row_is_rejected(self) -> None:
        cases = (
            ("platform.linux", "support", "primary"),
            ("platform.linux", "release", "candidate"),
            ("platform.macos", "support", "supported"),
            ("platform.macos", "release", "ready"),
        )
        for identifier, field, value in cases:
            with self.subTest(capability=identifier, field=field):
                contract = copy.deepcopy(self.contract)
                capability_of(contract, identifier)[field] = value
                errors = site_data_validate.experimental_platform_support_errors(contract)
                self.assertEqual(1, len(errors), errors)
                self.assertIn(f"capabilities.{identifier}: {field} is {value!r}", errors[0])

    def test_done_item_lifts_the_restriction(self) -> None:
        capability_of(self.contract, "platform.linux")["support"] = "supported"
        next(i for i in self.contract["workItems"] if i["id"] == "PLT-210")["status"] = "done"
        self.assertEqual([], site_data_validate.experimental_platform_support_errors(self.contract))


if __name__ == "__main__":
    unittest.main()
