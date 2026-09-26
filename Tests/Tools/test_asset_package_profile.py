#!/usr/bin/env python3
"""RDY-020 / OD-09: the stable-v1 package ships only its module asset closure, with no NOASSERTION asset.

OD-09 excludes the TERRAFRONT assets without recorded provenance (license
``NOASSERTION``) from the stable-v1 package while keeping them in the
repository. RDY-020 further limits stable-v1 to the runtime asset closure of
its in-profile modules (tools/asset-integrity/package_closure.py). These tests
cover the parts of that contract:

* the closure is derived from module and reviewed engine source literals,
  reviewed seeds, and the scenes/materials those reference; an unresolved or
  case-mismatched reference (including a top-level directory case typo), a
  stale unshipped-reference exemption, or a NOASSERTION entry the closure
  needs fails the derivation;
* the verifier derives a stable-v1 package manifest and the install exclusions
  from the reviewed repository manifest;
* ``verify --profile stable-v1`` rejects any package manifest that includes a
  NOASSERTION entry, relabels one, or adds an unreviewed file, and never skips
  the closure check for a standalone source manifest;
* cmake/SparkRuntimeAssets.cmake installs exactly the derived set, so the
  installed stable-v1 tree verifies while a default-profile tree is rejected.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools" / "asset-integrity" / "verify_asset_integrity.py"
RUNTIME_ASSETS_CMAKE = REPO_ROOT / "cmake" / "SparkRuntimeAssets.cmake"
REPO_MANIFEST = REPO_ROOT / "Assets" / "assets.integrity.json"
SPEC = importlib.util.spec_from_file_location("verify_asset_integrity_profile", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
vai = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(vai)
closure_lib = vai._package_closure_module()

KEPT_MODEL = b"o kept\n"
MIXED_KEPT = b"mixed kept texture\n"
MIXED_UNRECORDED = b"mixed unrecorded texture\n"
UNRECORDED = b"unrecorded texture\n"
README = b"# Fixture assets\n"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run_verifier(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(SCRIPT), *args],
        capture_output=True, text=True, check=False, timeout=120)


FIXTURE_MODULE_SOURCE = """// Models/commented.obj is not a reference.
#include "Models/included.h"
static const wchar_t* kModel = L"Models/kept.obj";
static const char* kTexture = R"spark(Textures/mixed/a.png)spark";
static const char kQuote = '"';
static const int kCount = 1'000;
"""


class Fixture:
    """repo/Assets with asserted, NOASSERTION, and mixed directories plus one in-profile module."""

    UNRECORDED_FILES = (
        "Textures/Unrecorded/deep/one.png",
        "Textures/Unrecorded/two.png",
        "Textures/mixed/b+(1).png",
    )
    CLOSURE = ("Models/kept.obj", "README.md", "Textures/mixed/a.png")

    def __init__(self, base: Path, extra_files: dict[str, bytes] | None = None,
                 module_source: str = FIXTURE_MODULE_SOURCE) -> None:
        self.repo = base / "repo"
        self.assets = self.repo / "Assets"
        self.policy_path = self.repo / "provenance.json"
        files = {
            "Models/kept.obj": KEPT_MODEL,
            "Textures/mixed/a.png": MIXED_KEPT,
            "Textures/mixed/b+(1).png": MIXED_UNRECORDED,
            "Textures/Unrecorded/deep/one.png": UNRECORDED,
            "Textures/Unrecorded/two.png": UNRECORDED + b"2",
            "README.md": README,
            **(extra_files or {}),
        }
        for relative, data in files.items():
            path = self.assets / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.repo / "LICENSE").write_text("Fixture License\n", encoding="utf-8")
        extra_roots = sorted(
            {f"{relative.split('/', 1)[0]}/" for relative in extra_files or {}} - {"Models/", "Textures/"})
        policy = {
            "version": 1,
            "root": "Assets",
            "licenses": {
                "LicenseRef-Fixture": {"name": "Fixture License"},
                "NOASSERTION": {"name": "No license asserted"},
            },
            "rules": [
                {
                    "id": "authored",
                    "license": "LicenseRef-Fixture",
                    "provenance": "Repository-authored content",
                    "evidence": ["LICENSE"],
                    "files": {"README.md": sha256(README)},
                    "prefixes": ["Models/", "Textures/mixed/", *extra_roots],
                },
                {
                    "id": "unrecorded",
                    "license": "NOASSERTION",
                    "provenance": "No tracked origin record",
                    "evidence": [],
                    "gap": "RDY-020",
                    "files": {"Textures/mixed/b+(1).png": sha256(MIXED_UNRECORDED)},
                    "prefixes": ["Textures/Unrecorded/"],
                },
            ],
        }
        self.policy_path.write_text(json.dumps(policy, indent=2) + "\n", encoding="utf-8")
        self.module_source = self.repo / "GameModules" / "FixtureGame" / "Source" / "Game.cpp"
        self.module_source.parent.mkdir(parents=True)
        self.module_source.write_text(module_source, encoding="utf-8")
        self.engine_source = self.repo / "Engine" / "Source" / "Primitive.h"
        self.engine_source.parent.mkdir(parents=True)
        self.engine_source.write_text('const char* kUnused = "Engine/Source/Primitive.h";\n', encoding="utf-8")
        self.write_inventory({"stable-v1": "required"})
        self.write_profiles(["FixtureGame"], [{"path": "README.md", "reason": "Asset README beside the manifest"}])
        manifest, errors = vai.generate_manifest(self.assets, provenance_policy=self.policy_path)
        if errors:
            raise AssertionError("\n".join(str(error) for error in errors))
        self.manifest_path = self.assets / vai.MANIFEST_FILENAME
        vai.write_manifest(manifest, self.manifest_path)
        self.manifest = vai.load_manifest(self.manifest_path)

    def write_inventory(self, applicability: dict[str, str]) -> None:
        inventory = self.repo / "GameModules" / "module-content-inventory.json"
        inventory.write_text(json.dumps({
            "schemaVersion": 1,
            "modules": [{
                "name": "FixtureGame",
                "sourceDirectory": "GameModules/FixtureGame/Source",
                "profileApplicability": applicability,
            }],
        }), encoding="utf-8")

    def write_profiles(self, modules: list[str], seeds: list[dict[str, str]],
                       unshipped: list[dict[str, str]] | None = None) -> None:
        profiles = self.repo / "tools" / "asset-integrity" / "package-profiles.json"
        profiles.parent.mkdir(parents=True, exist_ok=True)
        profiles.write_text(json.dumps({
            "version": 1,
            "profiles": {"stable-v1": {
                "modules": modules,
                "engineSources": [{"path": "Engine/Source/", "reason": "Fixture engine code"}],
                "seeds": seeds,
                "unshippedReferences": unshipped or [],
            }},
        }), encoding="utf-8")

    def closure(self) -> dict[str, list[str]]:
        closure, _ = vai.derive_profile_closure(self.repo, self.manifest, "stable-v1")
        assert closure is not None
        return closure


class ClosureTests(unittest.TestCase):
    def test_closure_is_module_literals_plus_seeds(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary), extra_files={"Models/unused.obj": b"o unused\n"})
            closure = fixture.closure()
            self.assertEqual(sorted(closure), list(Fixture.CLOSURE))
            self.assertEqual(closure["Models/kept.obj"], ["GameModules/FixtureGame/Source/Game.cpp:3"])
            self.assertEqual(closure["README.md"], ["seed: Asset README beside the manifest"])
            derived, excluded = vai.derive_package_manifest(
                fixture.manifest, "stable-v1", closure)
            self.assertEqual([entry["path"] for entry in derived["entries"]], list(Fixture.CLOSURE))
            # The asserted but unreferenced model leaves with the NOASSERTION files.
            self.assertEqual(sorted(excluded), sorted([*Fixture.UNRECORDED_FILES, "Models/unused.obj"]))

    def test_scene_and_material_references_are_followed(self) -> None:
        scene = (b"[Object]\nname=Crate\nmodel=crate.obj\nmaterial=Assets/Materials/Wood.json\n"
                 b"# model=commented.obj\n")
        material = json.dumps({"name": "Wood", "albedo": "Textures/mixed/a.png",
                               "layers": [{"normal": "Models/normal.png"}]}).encode()
        source = 'const char* kScene = "Scenes/level.scene";\n'
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary), module_source=source, extra_files={
                "Scenes/level.scene": scene,
                "Models/crate.obj": b"o crate\n",
                "Models/normal.png": b"normal\n",
                "Materials/Wood.json": material,
            })
            closure = fixture.closure()
            self.assertEqual(sorted(closure), [
                "Materials/Wood.json", "Models/crate.obj", "Models/normal.png", "README.md",
                "Scenes/level.scene", "Textures/mixed/a.png"])
            self.assertEqual(closure["Models/crate.obj"], ["Scenes/level.scene:3"])
            self.assertEqual(closure["Textures/mixed/a.png"], ["Materials/Wood.json"])

    def test_unresolved_or_case_mismatched_reference_fails(self) -> None:
        cases = {
            "Models/missing.obj": "does not declare",
            "Models/Kept.obj": "differs in case from the declared 'Models/kept.obj'",
            # A top-level case typo that a case-insensitive filesystem tolerates.
            "Assets/models/kept.obj": "differs in case from the declared 'Models/kept.obj'",
            "models/kept.obj": "differs in case",
            "assets\\\\Models\\\\Kept.obj": "differs in case",
        }
        for literal, message in cases.items():
            with self.subTest(literal=literal), tempfile.TemporaryDirectory() as temporary:
                fixture = Fixture(Path(temporary), module_source=f'auto path = "{literal}";\n')
                with self.assertRaisesRegex(vai.ManifestFormatError, message):
                    fixture.closure()

    def test_backslash_and_dot_prefixed_references_resolve(self) -> None:
        # C++ source escapes a backslash as two; both spellings name Models/kept.obj.
        for literal in ("Models\\\\kept.obj", "./Models/kept.obj", ".\\\\Assets\\\\Models\\\\kept.obj"):
            with self.subTest(literal=literal), tempfile.TemporaryDirectory() as temporary:
                fixture = Fixture(Path(temporary), module_source=f'auto path = "{literal}";\n')
                self.assertEqual(sorted(fixture.closure()), ["Models/kept.obj", "README.md"])

    def test_engine_source_literals_join_the_closure(self) -> None:
        # Engine code the module runs on (for example a primitive's default OBJ
        # model) is part of the closure even though no module source names it.
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary), module_source="int unused;\n",
                              extra_files={"Models/Cube.obj": b"o cube\n"})
            fixture.engine_source.write_text(
                'std::wstring m_modelPath = L"Assets/Models/Cube.obj";\n', encoding="utf-8")
            closure = fixture.closure()
            self.assertEqual(sorted(closure), ["Models/Cube.obj", "README.md"])
            self.assertEqual(closure["Models/Cube.obj"], ["Engine/Source/Primitive.h:1"])

    def test_unshipped_references_must_be_reviewed_and_current(self) -> None:
        reason = "Stored in a field no code reads"
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary), module_source='auto path = "Models/never_opened.dds";\n')
            with self.assertRaisesRegex(vai.ManifestFormatError, "does not declare"):
                fixture.closure()
            fixture.write_profiles(["FixtureGame"], [{"path": "README.md", "reason": "README"}],
                                   [{"path": "Models/never_opened.dds", "reason": reason}])
            self.assertEqual(sorted(fixture.closure()), ["README.md"])
            # An exemption that no scanned source uses any more is stale.
            fixture.module_source.write_text("int unused;\n", encoding="utf-8")
            with self.assertRaisesRegex(vai.ManifestFormatError, "occurs in no scanned source"):
                fixture.closure()
            # An exemption may not hide a file the manifest declares.
            fixture.module_source.write_text('auto path = "Models/kept.obj";\n', encoding="utf-8")
            fixture.write_profiles(["FixtureGame"], [{"path": "README.md", "reason": "README"}],
                                   [{"path": "Models/kept.obj", "reason": reason}])
            with self.assertRaisesRegex(vai.ManifestFormatError, "names a declared manifest entry"):
                fixture.closure()
            fixture.write_profiles(["FixtureGame"], [{"path": "README.md", "reason": "README"}],
                                   [{"path": "Models/kept.obj", "reason": " "}])
            with self.assertRaisesRegex(vai.ManifestFormatError, "non-empty path and reason"):
                fixture.closure()

    def test_closure_needing_a_noassertion_asset_fails(self) -> None:
        # Fail-closed: OD-09 forbids shipping it, and silently dropping it would
        # ship a package that cannot load its own content.
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary), module_source='auto t = "Textures/Unrecorded/two.png";\n')
            with self.assertRaisesRegex(vai.ManifestFormatError, r"Textures/Unrecorded/two\.png.*NOASSERTION"):
                fixture.closure()
            with self.assertRaisesRegex(vai.ManifestFormatError, "NOASSERTION"):
                vai.derive_package_manifest(fixture.manifest, "stable-v1", ["Textures/Unrecorded/two.png"])

    def test_profile_modules_must_match_the_module_inventory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary))
            fixture.write_inventory({"stable-v1": "excluded"})
            with self.assertRaisesRegex(vai.ManifestFormatError, "do not match the module inventory"):
                fixture.closure()
            fixture.write_inventory({"stable-v1": "required"})
            fixture.write_profiles(["FixtureGame"], [{"path": "Engine/", "reason": "missing seed"}])
            with self.assertRaisesRegex(vai.ManifestFormatError, "matches no source manifest entry"):
                fixture.closure()
            fixture.write_profiles(["FixtureGame"], [{"path": "README.md", "reason": " "}])
            with self.assertRaisesRegex(vai.ManifestFormatError, "non-empty path and reason"):
                fixture.closure()

    def test_default_profile_has_no_closure(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary))
            self.assertEqual(vai.derive_profile_closure(fixture.repo, fixture.manifest, "default"), (None, []))


class LiteralLexerTests(unittest.TestCase):
    def test_comments_preprocessor_and_char_literals_are_skipped(self) -> None:
        literals = closure_lib.cpp_string_literals(
            '#include "Models/a.obj"\n'
            '#define X \\\n  "Models/b.obj"\n'
            '/* "Models/c.obj" */ // "Models/d.obj"\n'
            "char q = '\"'; int n = 0x1'F; auto s = u8\"Models/e.obj\";\n"
            'auto r = LR"x(Models/"f".obj)x"; auto t = "esc\\"aped";\n')
        self.assertEqual(literals, [(5, "Models/e.obj"), (6, 'Models/"f".obj'), (6, 'esc\\"aped')])

    def test_asset_reference_filter(self) -> None:
        top = frozenset({"Models", "Audio"})
        self.assertEqual(closure_lib.asset_reference("Assets/Models/a.obj", top), "Models/a.obj")
        for value, expected in (("models/a.obj", "models/a.obj"), ("Models\\a.obj", "Models/a.obj"),
                                ("Models\\\\a.obj", "Models/a.obj"), ("./Models/a.obj", "Models/a.obj"),
                                ("ASSETS/audio/b.wav", "audio/b.wav")):
            self.assertEqual(closure_lib.asset_reference(value, top), expected, value)
        for value in ("Audio/AudioEngine.h", "Models/", "Models/../x.obj", "Other/a.obj", "Models/noext",
                      "/Models/a.obj", "../Models/a.obj"):
            self.assertIsNone(closure_lib.asset_reference(value, top), value)


class DerivationTests(unittest.TestCase):
    def test_stable_v1_drops_every_noassertion_entry(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary))
            derived, excluded = vai.derive_package_manifest(fixture.manifest, "stable-v1")
            self.assertEqual(sorted(excluded), sorted(Fixture.UNRECORDED_FILES))
            paths = [entry["path"] for entry in derived["entries"]]
            self.assertEqual(paths, ["Models/kept.obj", "README.md", "Textures/mixed/a.png"])
            self.assertEqual(derived["fileCount"], 3)
            self.assertNotIn(vai.NOASSERTION, {entry["license"] for entry in derived["entries"]})
            self.assertEqual(vai.package_profile_errors(derived, "stable-v1", "derived", fixture.manifest), [])

    def test_default_profile_keeps_every_entry(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary))
            derived, excluded = vai.derive_package_manifest(fixture.manifest, "default")
            self.assertEqual(excluded, [])
            self.assertEqual(derived["entries"], fixture.manifest["entries"])

    def test_stable_v1_refuses_manifest_without_license_records(self) -> None:
        legacy = {"version": 1, "algorithm": "sha256", "root": "Assets", "fileCount": 0, "entries": []}
        with self.assertRaisesRegex(vai.ManifestFormatError, "records each entry's license"):
            vai.derive_package_manifest(legacy, "stable-v1")
        with self.assertRaisesRegex(vai.ManifestFormatError, "unknown package profile"):
            vai.derive_package_manifest(legacy, "nightly")

    def test_exclusions_collapse_only_directories_without_kept_files(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary))
            _, excluded = vai.derive_package_manifest(fixture.manifest, "stable-v1")
            prefixes = vai.package_exclusion_prefixes(
                (entry["path"] for entry in fixture.manifest["entries"]), excluded)
            self.assertEqual(prefixes, ["Textures/Unrecorded/", "Textures/mixed/b+(1).png"])

    def test_cli_writes_manifest_and_exclusions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary))
            output = Path(temporary) / "stable.json"
            exclusions = Path(temporary) / "excluded.txt"
            result = run_verifier(
                "package-profile", str(fixture.manifest_path), "--profile", "stable-v1",
                "--output", str(output), "--exclusions", str(exclusions), "--repo-root", str(fixture.repo),
                "--inputs", str(Path(temporary) / "inputs.txt"))
            self.assertEqual(result.returncode, 0, result.stderr)
            inputs = (Path(temporary) / "inputs.txt").read_text(encoding="utf-8").splitlines()
            # Source directories, not individual C/C++ files, are re-derivation inputs.
            self.assertIn(fixture.module_source.parent.as_posix(), inputs)
            self.assertIn(fixture.engine_source.parent.as_posix(), inputs)
            self.assertNotIn(fixture.module_source.as_posix(), inputs)
            self.assertIn(
                "3 of 6 entries kept, 3 entries (3 NOASSERTION, 0 outside the profile closure) excluded",
                result.stdout)
            self.assertEqual(vai.load_manifest(output)["fileCount"], 3)
            self.assertEqual(
                exclusions.read_text(encoding="utf-8").splitlines(),
                ["Textures/Unrecorded/", "Textures/mixed/b+(1).png"])
            clash = run_verifier(
                "package-profile", str(fixture.manifest_path), "--profile", "stable-v1",
                "--output", str(fixture.manifest_path), "--exclusions", str(exclusions))
            self.assertNotEqual(clash.returncode, 0)


class PackageCheckTests(unittest.TestCase):
    def test_full_manifest_is_rejected_for_stable_v1(self) -> None:
        # Fail-before: the unfiltered tree and manifest that every package
        # carried before OD-09 was enforced.
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary))
            plain = run_verifier("verify", str(fixture.manifest_path), "--root", str(fixture.assets))
            self.assertEqual(plain.returncode, 0, plain.stderr)
            result = run_verifier(
                "verify", str(fixture.manifest_path), "--root", str(fixture.assets),
                "--profile", "stable-v1", "--source-manifest", str(fixture.manifest_path))
            self.assertEqual(result.returncode, 1)
            for relative in Fixture.UNRECORDED_FILES:
                self.assertIn(f"[profile-excluded] {relative}", result.stderr)
            default = run_verifier(
                "verify", str(fixture.manifest_path), "--root", str(fixture.assets), "--profile", "default")
            self.assertEqual(default.returncode, 0, default.stderr)

    def test_relabelled_unreviewed_and_altered_entries_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary))
            derived, _ = vai.derive_package_manifest(fixture.manifest, "stable-v1")
            source_unrecorded = next(
                entry for entry in fixture.manifest["entries"] if entry["path"] == "Textures/Unrecorded/two.png")
            relabelled = dict(source_unrecorded, license="LicenseRef-Fixture")
            unreviewed = dict(derived["entries"][0], path="Models/zz_extra.obj")
            altered = dict(derived["entries"][1], provenance="rewritten claim")
            tampered = {
                **derived,
                "entries": sorted(
                    [altered, derived["entries"][2], relabelled, unreviewed], key=lambda entry: entry["path"]),
            }
            tampered["fileCount"] = len(tampered["entries"])
            categories = {
                (error.path, error.category)
                for error in vai.package_profile_errors(tampered, "stable-v1", "tampered", fixture.manifest)
            }
            self.assertEqual(categories, {
                ("Textures/Unrecorded/two.png", "profile-excluded"),
                ("Models/zz_extra.obj", "profile-unreviewed"),
                ("README.md", "profile-mismatch"),
            })

    def test_package_must_ship_exactly_the_closure(self) -> None:
        # Fail-before: a package built by NOASSERTION exclusion alone ships every
        # asserted file, including content no in-profile module loads.
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary), extra_files={"Models/unused.obj": b"o unused\n"})
            asserted, _ = vai.derive_package_manifest(fixture.manifest, "stable-v1")
            package = Path(temporary) / "package" / "Assets"
            for entry in asserted["entries"]:
                target = package / entry["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(fixture.assets / entry["path"], target)
            manifest_path = package / vai.MANIFEST_FILENAME
            vai.write_manifest(asserted, manifest_path)
            errors = vai.verify_package_manifest(
                manifest_path, package, "stable-v1", source_manifest_path=fixture.manifest_path)
            self.assertEqual([(error.path, error.category) for error in errors],
                             [("Models/unused.obj", "profile-outside-closure")])

            # Dropping a closure file from the package and its manifest is also caught.
            (package / "Models/unused.obj").unlink()
            (package / "Models/kept.obj").unlink()
            dropped = ("Models/unused.obj", "Models/kept.obj")
            trimmed = {**asserted, "entries": [entry for entry in asserted["entries"] if entry["path"] not in dropped]}
            trimmed["fileCount"] = len(trimmed["entries"])
            vai.write_manifest(trimmed, manifest_path)
            errors = vai.verify_package_manifest(
                manifest_path, package, "stable-v1", source_manifest_path=fixture.manifest_path)
            self.assertEqual([(error.path, error.category) for error in errors],
                             [("Models/kept.obj", "profile-incomplete")])

    def test_standalone_source_manifest_does_not_skip_the_closure(self) -> None:
        # Fail-before: a source manifest outside a checkout with profile
        # definitions silently skipped profile-outside-closure/-incomplete.
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Fixture(Path(temporary), extra_files={"Models/unused.obj": b"o unused\n"})
            asserted, _ = vai.derive_package_manifest(fixture.manifest, "stable-v1")
            package = Path(temporary) / "package" / "Assets"
            for entry in asserted["entries"]:
                target = package / entry["path"]
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(fixture.assets / entry["path"], target)
            manifest_path = package / vai.MANIFEST_FILENAME
            vai.write_manifest(asserted, manifest_path)
            standalone = Path(temporary) / "copied-source.json"
            shutil.copyfile(fixture.manifest_path, standalone)
            no_definitions = Path(temporary) / "empty-checkout"
            no_definitions.mkdir()
            original = vai.REPO_ROOT
            try:
                # Without definitions anywhere the check fails closed.
                vai.REPO_ROOT = no_definitions
                errors = vai.verify_package_manifest(
                    manifest_path, package, "stable-v1", source_manifest_path=standalone)
                self.assertEqual([error.category for error in errors], ["profile-closure"])
                # Otherwise it falls back to the verifier's own checkout.
                vai.REPO_ROOT = fixture.repo
                errors = vai.verify_package_manifest(
                    manifest_path, package, "stable-v1", source_manifest_path=standalone)
                self.assertEqual([(error.path, error.category) for error in errors],
                                 [("Models/unused.obj", "profile-outside-closure")])
            finally:
                vai.REPO_ROOT = original

    def test_stable_v1_package_check_requires_schema_v2(self) -> None:
        legacy = {"version": 1, "algorithm": "sha256", "root": "Assets", "fileCount": 0, "entries": []}
        errors = vai.package_profile_errors(legacy, "stable-v1", "legacy")
        self.assertEqual([error.category for error in errors], ["provenance-missing"])
        self.assertEqual(vai.package_profile_errors(legacy, "default", "legacy"), [])


class RepositoryProfileTests(unittest.TestCase):
    def test_repository_stable_v1_manifest_has_no_noassertion_entry(self) -> None:
        manifest = vai.load_manifest(REPO_MANIFEST)
        derived, excluded = vai.derive_package_manifest(manifest, "stable-v1")
        unasserted = [entry["path"] for entry in manifest["entries"] if entry["license"] == vai.NOASSERTION]
        self.assertEqual(sorted(excluded), sorted(unasserted))
        self.assertEqual(derived["fileCount"] + len(excluded), manifest["fileCount"])
        self.assertEqual(vai.package_profile_errors(derived, "stable-v1", "repository", manifest), [])
        # The excluded assets stay in the repository.
        for relative in excluded:
            self.assertTrue((REPO_ROOT / "Assets" / relative).is_file(), relative)

    def test_check_all_reports_the_stable_v1_exclusion(self) -> None:
        result = run_verifier("check-all", "--repo-root", str(REPO_ROOT))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(
            result.stdout,
            r"package profile stable-v1: \d+ entries packaged, \d+ entries \(\d+ NOASSERTION, "
            r"\d+ outside the profile closure\) excluded")

    def test_repository_stable_v1_closure_is_the_fps_runtime_set(self) -> None:
        manifest = vai.load_manifest(REPO_MANIFEST)
        closure, inputs = vai.derive_profile_closure(REPO_ROOT, manifest, "stable-v1")
        assert closure is not None
        # The FPS entry scene, its materials' textures, and the engine branding ship.
        for required in ("Scenes/level1.scene", "Materials/Concrete.json", "Textures/concrete_diffuse.png",
                         "Models/pistol.obj", "Engine/Branding/sparkengine_splash.wav", "README.md"):
            self.assertIn(required, closure)
        # The engine primitive objects FPS and SceneManager instantiate load
        # their default OBJ models from engine headers, not module source.
        for primitive in ("Cube", "Plane", "Sphere", "Wall", "Pyramid", "Ramp"):
            reasons = closure[f"Models/{primitive}.obj"]
            self.assertTrue(reasons[0].startswith(f"SparkEngine/Source/Game/{primitive}Object.h:"), reasons)
        # Content that only other modules load does not.
        for path in closure:
            self.assertFalse(path.startswith(("MMO/", "MMOFPS/", "Scenes/MMO", "Models/MMO", "Audio/MMO")), path)
        entries = {entry["path"]: entry for entry in manifest["entries"]}
        self.assertNotIn(vai.NOASSERTION, {entries[path]["license"] for path in closure})
        self.assertIn(REPO_ROOT / "Assets" / "Scenes" / "level1.scene", inputs)
        self.assertIn(REPO_ROOT / "SparkEngine" / "Source", inputs)

    def test_every_engine_and_module_asset_literal_is_shipped_or_reviewed_as_unshipped(self) -> None:
        # The derivation itself enforces this (an unlisted literal fails it);
        # this pins that each reviewed exemption names a file that does not
        # exist, so an exemption can never hide a real asset.
        definition = closure_lib.load_profile_definition(REPO_ROOT, "stable-v1")
        self.assertIn({"path": "SparkEngine/Source/", "reason": definition["engineSources"][0]["reason"]},
                      definition["engineSources"])
        for item in definition["unshippedReferences"]:
            self.assertFalse((REPO_ROOT / "Assets" / item["path"]).exists(), item["path"])

    def test_check_all_profile_strict_provenance_judges_only_shipped_entries(self) -> None:
        shipped = run_verifier("check-all", "--repo-root", str(REPO_ROOT), "--profile", "stable-v1",
                               "--strict-provenance")
        self.assertEqual(shipped.returncode, 0, shipped.stderr)
        self.assertRegex(shipped.stdout, r"package profile stable-v1 provenance: (\d+) entries, \1 license-asserted, "
                                         r"0 NOASSERTION")
        # The whole-repository gate still fails while any NOASSERTION entry remains.
        if any(entry["license"] == vai.NOASSERTION for entry in vai.load_manifest(REPO_MANIFEST)["entries"]):
            repository = run_verifier("check-all", "--repo-root", str(REPO_ROOT), "--strict-provenance")
            self.assertEqual(repository.returncode, 1)
            self.assertIn("the repository manifest has", repository.stderr)
            default = run_verifier("check-all", "--repo-root", str(REPO_ROOT), "--profile", "default",
                                   "--strict-provenance")
            self.assertEqual(default.returncode, 1)
            self.assertIn("package profile default ships", default.stderr)


@unittest.skipIf(shutil.which("cmake") is None or shutil.which("git") is None, "cmake and git are required")
class InstallRuleTests(unittest.TestCase):
    """Configure and install a fixture project through SparkRuntimeAssets.cmake."""

    def _install(self, fixture: Fixture, base: Path, profile: str) -> Path:
        build = base / f"build-{profile}"
        install = base / f"install-{profile}"
        env = dict(os.environ, GIT_CONFIG_NOSYSTEM="1")
        for command in (
            ["cmake", "-S", str(fixture.repo), "-B", str(build),
             f"-DSPARK_RUNTIME_ASSETS_HELPER={RUNTIME_ASSETS_CMAKE}",
             f"-DSPARK_ASSET_VERIFIER={SCRIPT}",
             f"-DSPARK_ASSET_PROFILE={profile}",
             f"-DPython3_EXECUTABLE={sys.executable}"],
            ["cmake", "--install", str(build), "--prefix", str(install), "--component", "runtime"],
        ):
            result = subprocess.run(command, capture_output=True, text=True, check=False, env=env, timeout=300)
            self.assertEqual(result.returncode, 0, f"{command}\n{result.stdout}\n{result.stderr}")
        return install / "bin" / "Assets"

    def _prepare_project(self, fixture: Fixture) -> None:
        (fixture.repo / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.25)\n"
            "project(SparkRuntimeAssetsFixture NONE)\n"
            "include(\"${SPARK_RUNTIME_ASSETS_HELPER}\")\n"
            "spark_install_runtime_assets(\n"
            "    ROOT Assets\n"
            "    DESTINATION bin/Assets\n"
            "    COMPONENT runtime\n"
            "    PROFILE ${SPARK_ASSET_PROFILE}\n"
            "    VERIFIER \"${SPARK_ASSET_VERIFIER}\"\n"
            "    DIRECTORIES Models Textures\n"
            "    ROOT_FILES README.md)\n",
            encoding="utf-8")
        for command in (["git", "init", "--quiet"], ["git", "add", "--all"]):
            subprocess.run(command, cwd=fixture.repo, check=True, capture_output=True, timeout=60)

    def test_stable_v1_install_excludes_noassertion_and_verifies(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            fixture = Fixture(base)
            self._prepare_project(fixture)

            stable = self._install(fixture, base, "stable-v1")
            for relative in Fixture.UNRECORDED_FILES:
                self.assertFalse((stable / relative).exists(), relative)
            for relative in ("Models/kept.obj", "Textures/mixed/a.png", "README.md"):
                self.assertTrue((stable / relative).is_file(), relative)
            result = run_verifier(
                "verify", str(stable / vai.MANIFEST_FILENAME), "--root", str(stable),
                "--profile", "stable-v1", "--source-manifest", str(fixture.manifest_path))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("3 entries verified", result.stdout)
            # The repository keeps the excluded assets.
            for relative in Fixture.UNRECORDED_FILES:
                self.assertTrue((fixture.assets / relative).is_file(), relative)

            # A default-profile package still carries them and must not pass as
            # stable-v1.
            default = self._install(fixture, base, "default")
            self.assertTrue((default / "Textures/Unrecorded/two.png").is_file())
            plain = run_verifier("verify", str(default / vai.MANIFEST_FILENAME), "--root", str(default))
            self.assertEqual(plain.returncode, 0, plain.stderr)
            rejected = run_verifier(
                "verify", str(default / vai.MANIFEST_FILENAME), "--root", str(default),
                "--profile", "stable-v1", "--source-manifest", str(fixture.manifest_path))
            self.assertEqual(rejected.returncode, 1)
            self.assertIn("[profile-excluded] Textures/Unrecorded/two.png", rejected.stderr)

            # Copying an excluded file into the stable-v1 tree is caught as an
            # undeclared file even though the manifest was not touched.
            smuggled = stable / "Textures/Unrecorded/two.png"
            smuggled.parent.mkdir(parents=True)
            smuggled.write_bytes(UNRECORDED + b"2")
            result = run_verifier(
                "verify", str(stable / vai.MANIFEST_FILENAME), "--root", str(stable),
                "--profile", "stable-v1", "--source-manifest", str(fixture.manifest_path))
            self.assertEqual(result.returncode, 1)
            self.assertIn("[undeclared] Textures/Unrecorded/two.png", result.stderr)


if __name__ == "__main__":
    unittest.main()
