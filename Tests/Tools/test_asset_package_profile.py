#!/usr/bin/env python3
"""RDY-020 / OD-09: the stable-v1 package ships no NOASSERTION asset.

OD-09 excludes the TERRAFRONT assets without recorded provenance (license
``NOASSERTION``) from the stable-v1 package while keeping them in the
repository. These tests cover the three parts of that contract:

* the verifier derives a stable-v1 package manifest and the install exclusions
  from the reviewed repository manifest;
* ``verify --profile stable-v1`` rejects any package manifest that includes a
  NOASSERTION entry, relabels one, or adds an unreviewed file;
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


class Fixture:
    """repo/Assets with asserted, NOASSERTION, and mixed directories."""

    UNRECORDED_FILES = (
        "Textures/Unrecorded/deep/one.png",
        "Textures/Unrecorded/two.png",
        "Textures/mixed/b+(1).png",
    )

    def __init__(self, base: Path) -> None:
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
        }
        for relative, data in files.items():
            path = self.assets / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        (self.repo / "LICENSE").write_text("Fixture License\n", encoding="utf-8")
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
                    "prefixes": ["Models/", "Textures/mixed/"],
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
        manifest, errors = vai.generate_manifest(self.assets, provenance_policy=self.policy_path)
        if errors:
            raise AssertionError("\n".join(str(error) for error in errors))
        self.manifest_path = self.assets / vai.MANIFEST_FILENAME
        vai.write_manifest(manifest, self.manifest_path)
        self.manifest = vai.load_manifest(self.manifest_path)


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
                "--output", str(output), "--exclusions", str(exclusions))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("3 of 6 entries kept, 3 NOASSERTION entries excluded", result.stdout)
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
            result.stdout, r"package profile stable-v1: \d+ entries packaged, \d+ NOASSERTION entries excluded")


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
