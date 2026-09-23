#!/usr/bin/env python3
"""REL-100 per-artifact build provenance: record, verify, and workflow wiring.

Acceptance criterion: every stable-v1 artifact records source SHA,
dependency-lock digest, exact toolchain, and configuration. These tests drive
the real tool against a real git repository, a CMake-shaped build tree, and
package bytes; the workflow tests pin the tool into every package-producing job
and before the first release mutation.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
# A flat file: the Visual Studio ignore rule `[Rr]elease*/` hides any
# `tools/release*/` directory from git, which would ship a workflow step whose
# script was never committed.
TOOL = ROOT / "tools" / "release_build_provenance.py"
WORKFLOW = ROOT / ".github" / "workflows" / "release.yml"
VERSION = "7.8.9"
LOCK_REL = "ThirdParty/dependencies.lock"


def run_tool(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, str(TOOL), *args], capture_output=True, text=True,
                          encoding="utf-8", errors="replace", check=False, timeout=60)


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True,
                          check=True).stdout.strip()


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class Fixture:
    """A committed source tree, a configured build tree, and CPack outputs."""

    def __init__(self, base: Path, *, multi_config: bool = True) -> None:
        self.source = base / "source"
        self.build = base / "build"
        self.packages = self.build / "packages"
        self.out = base / "records" / "build-provenance-Windows-MinSizeRel.json"
        (self.source / "ThirdParty").mkdir(parents=True)
        (self.source / LOCK_REL).write_text("entt 9c5281c79a241ec810f999ad9675fa32f51f934f\n", encoding="utf-8")
        git(self.source, "init", "-q")
        git(self.source, "add", LOCK_REL)
        git(self.source, "-c", "user.name=t", "-c", "user.email=t@example.invalid",
            "-c", "commit.gpgsign=false", "commit", "-q", "-m", "fixture")
        self.sha = git(self.source, "rev-parse", "HEAD")

        compiler_dir = self.build / "CMakeFiles" / "3.30.2"
        compiler_dir.mkdir(parents=True)
        config_line = ("CMAKE_CONFIGURATION_TYPES:STRING=Debug;Release;MinSizeRel;RelWithDebInfo"
                       if multi_config else "CMAKE_BUILD_TYPE:STRING=MinSizeRel")
        (self.build / "CMakeCache.txt").write_text("\n".join([
            "# This is the CMakeCache file.",
            config_line,
            f"SPARK_ENGINE_VERSION:STRING={VERSION}",
            "CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022",
            "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64",
            "CMAKE_GENERATOR_TOOLSET:INTERNAL=v143",
            "CMAKE_CACHE_MAJOR_VERSION:INTERNAL=3",
            "CMAKE_CACHE_MINOR_VERSION:INTERNAL=30",
            "CMAKE_CACHE_PATCH_VERSION:INTERNAL=2",
            "",
        ]), encoding="utf-8")
        (compiler_dir / "CMakeCXXCompiler.cmake").write_text("\n".join([
            'set(CMAKE_CXX_COMPILER "C:/VS/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe")',
            'set(CMAKE_CXX_COMPILER_ID "MSVC")',
            'set(CMAKE_CXX_COMPILER_VERSION "19.44.35228.0")',
            'set(CMAKE_CXX_COMPILER_ARCHITECTURE_ID "x64")',
            "",
        ]), encoding="utf-8")
        self.packages.mkdir()
        (self.packages / f"SparkEngine-{VERSION}-Windows-AMD64-MinSizeRel.zip").write_bytes(b"zip-bytes")
        (self.packages / f"SparkEngine-{VERSION}-Windows-AMD64-MinSizeRel-Runtime.msi").write_bytes(b"msi-bytes")
        (self.packages / "shipping-package-manifest.json").write_text("{}", encoding="utf-8")
        (self.packages / "_CPack_Packages").mkdir()

    def record(self, *extra: str, sha: str | None = None, version: str = VERSION,
               configuration: str = "MinSizeRel") -> subprocess.CompletedProcess[str]:
        return run_tool(
            "record", "--source-root", str(self.source), "--source-sha", sha or self.sha,
            "--version", version, "--profile", "stable-v1", "--platform", "Windows",
            "--configuration", configuration, "--build-dir", str(self.build),
            "--packages", str(self.packages), "--out", str(self.out), *extra)

    def publish(self, assets: Path) -> Path:
        """Flatten packages the way the release job does and list expected assets."""
        assets.mkdir(parents=True, exist_ok=True)
        names = []
        for package in sorted(self.packages.iterdir()):
            if package.is_file():
                shutil.copyfile(package, assets / package.name)
                names.append(package.name)
        alias = assets / "SparkEngine-Windows-x64-MinSizeRel.zip"
        shutil.copyfile(self.packages / f"SparkEngine-{VERSION}-Windows-AMD64-MinSizeRel.zip", alias)
        names.append(alias.name)
        (assets / "SHA256SUMS").write_text("", encoding="utf-8")
        names.append("SHA256SUMS")
        listing = assets / "expected-release-assets.txt"
        listing.write_text("\n".join(names) + "\n", encoding="utf-8")
        return listing

    def verify(self, assets: Path, listing: Path, *extra: str, sha: str | None = None,
               version: str = VERSION) -> subprocess.CompletedProcess[str]:
        return run_tool(
            "verify", "--records-dir", str(self.out.parent), "--assets-dir", str(assets),
            "--assets-file", str(listing), "--source-root", str(self.source),
            "--source-sha", sha or self.sha, "--version", version, *extra)


class RecordTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.base = Path(self._temp.name)
        self.fx = Fixture(self.base)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def test_record_binds_source_lock_toolchain_configuration_and_artifacts(self) -> None:
        result = self.fx.record()
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(self.fx.out.read_text(encoding="utf-8"))
        self.assertEqual(record["schemaVersion"], "spark-build-provenance-v1")
        self.assertEqual(record["sourceSHA"], self.fx.sha)
        self.assertEqual(record["version"], VERSION)
        self.assertEqual(record["profile"], "stable-v1")
        self.assertEqual(record["platform"], "Windows")
        self.assertEqual(record["configuration"], "MinSizeRel")
        # The committed blob is the platform-independent identity: a Windows
        # autocrlf checkout and a POSIX checkout must record the same digest.
        committed = subprocess.run(["git", "-C", str(self.fx.source), "show", f"HEAD:{LOCK_REL}"],
                                   capture_output=True, check=True).stdout
        self.assertEqual(record["dependencyLock"],
                         {"path": LOCK_REL, "sha256": hashlib.sha256(committed).hexdigest()})
        toolchain = record["toolchain"]
        self.assertEqual(toolchain["cmakeVersion"], "3.30.2")
        self.assertEqual(toolchain["generator"], "Visual Studio 17 2022")
        self.assertEqual(toolchain["generatorPlatform"], "x64")
        self.assertEqual(toolchain["generatorToolset"], "v143")
        self.assertEqual(toolchain["compilers"]["CXX"]["id"], "MSVC")
        self.assertEqual(toolchain["compilers"]["CXX"]["version"], "19.44.35228.0")
        self.assertEqual(toolchain["compilers"]["CXX"]["architecture"], "x64")
        self.assertEqual(record["cmakeConfiguration"],
                         {"buildType": None,
                          "configurationTypes": ["Debug", "Release", "MinSizeRel", "RelWithDebInfo"]})
        # Only distributable package bytes are artifacts; manifests and CPack scratch are not.
        self.assertEqual(
            [(a["name"], a["sha256"]) for a in record["artifacts"]],
            sorted((p.name, sha256(p)) for p in self.fx.packages.iterdir()
                   if p.suffix in (".zip", ".msi")))

    def test_single_config_build_type_is_recorded_and_must_match(self) -> None:
        with tempfile.TemporaryDirectory() as other:
            fx = Fixture(Path(other), multi_config=False)
            ok = fx.record()
            self.assertEqual(ok.returncode, 0, ok.stderr)
            record = json.loads(fx.out.read_text(encoding="utf-8"))
            self.assertEqual(record["cmakeConfiguration"],
                             {"buildType": "MinSizeRel", "configurationTypes": None})
            fx.out.unlink()
            wrong = fx.record(configuration="Release")
            self.assertNotEqual(wrong.returncode, 0)
            self.assertIn("CMAKE_BUILD_TYPE", wrong.stderr)
            self.assertFalse(fx.out.exists())

    def test_rejects_configuration_absent_from_multi_config_generator(self) -> None:
        result = self.fx.record(configuration="Shipping")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CMAKE_CONFIGURATION_TYPES", result.stderr)

    def test_rejects_source_sha_that_is_not_the_checked_out_commit(self) -> None:
        result = self.fx.record(sha="0" * 40)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source SHA", result.stderr)
        self.assertFalse(self.fx.out.exists())

    def test_rejects_build_tree_configured_for_a_different_version(self) -> None:
        result = self.fx.record(version="7.8.10")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("SPARK_ENGINE_VERSION", result.stderr)

    def test_rejects_uncommitted_dependency_lock(self) -> None:
        (self.fx.source / LOCK_REL).write_text("entt drifted\n", encoding="utf-8")
        result = self.fx.record()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dependency lock", result.stderr)

    def test_crlf_checkout_of_the_committed_lock_records_the_blob_digest(self) -> None:
        git(self.fx.source, "config", "core.autocrlf", "true")
        committed = subprocess.run(["git", "-C", str(self.fx.source), "show", f"HEAD:{LOCK_REL}"],
                                   capture_output=True, check=True).stdout
        self.assertNotIn(b"\r\n", committed)
        (self.fx.source / LOCK_REL).write_bytes(committed.replace(b"\n", b"\r\n"))
        result = self.fx.record()
        self.assertEqual(result.returncode, 0, result.stderr)
        record = json.loads(self.fx.out.read_text(encoding="utf-8"))
        self.assertEqual(record["dependencyLock"]["sha256"], hashlib.sha256(committed).hexdigest())

    def test_rejects_missing_compiler_identity(self) -> None:
        compiler = self.fx.build / "CMakeFiles" / "3.30.2" / "CMakeCXXCompiler.cmake"
        compiler.write_text('set(CMAKE_CXX_COMPILER_ID "MSVC")\n', encoding="utf-8")
        result = self.fx.record()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CMAKE_CXX_COMPILER_VERSION", result.stderr)

    def test_rejects_empty_package_directory(self) -> None:
        for package in self.fx.packages.glob("*.*"):
            package.unlink()
        result = self.fx.record()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no distributable", result.stderr)

    def test_rejects_engine_package_named_for_another_version(self) -> None:
        (self.fx.packages / "SparkEngine-1.0.0-Windows-AMD64-MinSizeRel.zip").write_bytes(b"old")
        result = self.fx.record()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("1.0.0", result.stderr)

    def test_never_replaces_an_existing_record(self) -> None:
        self.assertEqual(self.fx.record().returncode, 0)
        before = self.fx.out.read_bytes()
        again = self.fx.record()
        self.assertNotEqual(again.returncode, 0)
        self.assertEqual(self.fx.out.read_bytes(), before)


class VerifyTests(unittest.TestCase):
    def setUp(self) -> None:
        self._temp = tempfile.TemporaryDirectory()
        self.base = Path(self._temp.name)
        self.fx = Fixture(self.base)
        recorded = self.fx.record()
        self.assertEqual(recorded.returncode, 0, recorded.stderr)
        self.assets = self.base / "assets"
        self.listing = self.fx.publish(self.assets)

    def tearDown(self) -> None:
        self._temp.cleanup()

    def rewrite_record(self, mutate) -> None:
        record = json.loads(self.fx.out.read_text(encoding="utf-8"))
        mutate(record)
        self.fx.out.write_text(json.dumps(record), encoding="utf-8")

    def test_verified_record_covers_every_published_package_including_aliases(self) -> None:
        result = self.fx.verify(self.assets, self.listing, "--stable")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("3 distributable asset(s)", result.stdout)

    def test_rejects_published_bytes_that_no_record_describes(self) -> None:
        (self.assets / f"SparkEngine-{VERSION}-Windows-AMD64-MinSizeRel-Runtime.msi").write_bytes(b"tampered")
        result = self.fx.verify(self.assets, self.listing)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not covered", result.stderr)

    def test_rejects_recorded_artifact_that_is_not_published(self) -> None:
        names = [line for line in self.listing.read_text(encoding="utf-8").splitlines()
                 if not line.endswith(".msi")]
        self.listing.write_text("\n".join(names) + "\n", encoding="utf-8")
        result = self.fx.verify(self.assets, self.listing)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("not published", result.stderr)

    def test_rejects_record_from_another_source_commit(self) -> None:
        # The release checkout advanced (same lock), but the record names the old build.
        (self.fx.source / "README.md").write_text("later\n", encoding="utf-8")
        git(self.fx.source, "add", "README.md")
        git(self.fx.source, "-c", "user.name=t", "-c", "user.email=t@example.invalid",
            "-c", "commit.gpgsign=false", "commit", "-q", "-m", "later")
        result = self.fx.verify(self.assets, self.listing, sha=git(self.fx.source, "rev-parse", "HEAD"))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sourceSHA", result.stderr)

    def test_rejects_release_checkout_that_is_not_the_declared_source(self) -> None:
        result = self.fx.verify(self.assets, self.listing, sha="f" * 40)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("release checkout", result.stderr)

    def test_rejects_record_for_another_version(self) -> None:
        result = self.fx.verify(self.assets, self.listing, version="7.8.10")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("version", result.stderr)

    def test_rejects_dependency_lock_digest_that_differs_from_the_source_commit(self) -> None:
        self.rewrite_record(lambda r: r["dependencyLock"].__setitem__("sha256", "0" * 64))
        result = self.fx.verify(self.assets, self.listing)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("dependency lock", result.stderr)

    def test_rejects_unknown_or_missing_fields(self) -> None:
        self.rewrite_record(lambda r: r.__setitem__("unsigned", True))
        extra = self.fx.verify(self.assets, self.listing)
        self.assertNotEqual(extra.returncode, 0)
        self.assertIn("schema", extra.stderr)
        self.rewrite_record(lambda r: (r.pop("unsigned"), r.pop("toolchain")))
        missing = self.fx.verify(self.assets, self.listing)
        self.assertNotEqual(missing.returncode, 0)
        self.assertIn("schema", missing.stderr)

    def test_rejects_duplicate_json_keys(self) -> None:
        text = self.fx.out.read_text(encoding="utf-8")
        self.fx.out.write_text(text.replace('"version":', '"version": "0.0.1", "version":', 1),
                               encoding="utf-8")
        result = self.fx.verify(self.assets, self.listing)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate", result.stderr)

    def test_rejects_absent_records(self) -> None:
        self.fx.out.unlink()
        result = self.fx.verify(self.assets, self.listing)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("no build provenance records", result.stderr)

    def test_stable_requires_exactly_one_windows_shipping_record(self) -> None:
        self.rewrite_record(lambda r: r.__setitem__("configuration", "Release"))
        result = self.fx.verify(self.assets, self.listing, "--stable")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("stable", result.stderr)

    def test_rejects_missing_published_file(self) -> None:
        (self.assets / "SparkEngine-Windows-x64-MinSizeRel.zip").unlink()
        result = self.fx.verify(self.assets, self.listing)
        self.assertNotEqual(result.returncode, 0)


def _load_workflow() -> dict:
    import yaml  # PyYAML is installed in the release controller before this runs.

    return yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))


def _step_index(steps: list[dict], predicate) -> int:
    matches = [i for i, step in enumerate(steps) if predicate(step)]
    if len(matches) != 1:
        raise AssertionError(f"expected exactly one matching step, found {len(matches)}")
    return matches[0]


def _named(name: str):
    return lambda step: step.get("name") == name


def _runs(fragment: str):
    return lambda step: fragment in str(step.get("run", ""))


class WorkflowWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        try:
            cls.jobs = _load_workflow()["jobs"]
        except ImportError as error:  # pragma: no cover - environment guard
            raise AssertionError("PyYAML is required to verify release workflow wiring") from error

    def assert_unconditional(self, step: dict) -> None:
        self.assertNotIn("if", step, step.get("name"))
        self.assertNotIn("continue-on-error", step, step.get("name"))

    def test_every_package_job_records_provenance_after_final_bytes_and_uploads_it(self) -> None:
        last_byte_mutation = {
            "build-windows": "Check signed installer bytes after native qualification",
            "build-linux": "Extract and smoke-test portable package",
            "build-macos": "Extract and smoke-test portable package",
            "build-installer": "Launch staged executable",
        }
        for job_name, predecessor in last_byte_mutation.items():
            with self.subTest(job=job_name):
                steps = self.jobs[job_name]["steps"]
                record = _step_index(steps, _runs("tools/release_build_provenance.py record"))
                self.assert_unconditional(steps[record])
                self.assertGreater(record, _step_index(steps, _named(predecessor)))
                run = steps[record]["run"]
                self.assertIn("--source-sha", run)
                self.assertIn("${{ github.sha }}", run)
                self.assertIn("needs.prepare.outputs.cmake_version", run)
                upload = _step_index(
                    steps, lambda s: str(s.get("uses", "")).startswith("actions/upload-artifact@")
                    and "build-provenance" in str(s.get("with", {}).get("name", "")))
                self.assertGreater(upload, record)
                self.assert_unconditional(steps[upload])
                self.assertEqual(steps[upload]["with"]["if-no-files-found"], "error")
                package_upload = [s for s in steps if str(s.get("uses", "")).startswith("actions/upload-artifact@")
                                  and "build-provenance" not in str(s.get("with", {}).get("name", ""))
                                  and "diagnostics" not in str(s.get("with", {}).get("name", ""))
                                  and "qualification" not in str(s.get("with", {}).get("name", ""))
                                  and "signature-verification" not in str(s.get("with", {}).get("name", ""))]
                for package in package_upload:
                    self.assertNotIn("build-provenance", str(package["with"].get("path", "")))

    def test_release_verifies_provenance_before_any_publication_mutation(self) -> None:
        steps = self.jobs["release"]["steps"]
        download = _step_index(
            steps, lambda s: str(s.get("uses", "")).startswith("actions/download-artifact@")
            and "build-provenance" in str(s.get("with", {}).get("pattern", "")))
        self.assert_unconditional(steps[download])
        pattern = steps[download]["with"]["pattern"]
        self.assertIn("build-provenance-Windows-MinSizeRel", pattern)
        self.assertIn("build-provenance-*", pattern)
        verify = _step_index(steps, _runs("tools/release_build_provenance.py verify"))
        self.assert_unconditional(steps[verify])
        self.assertGreater(verify, download)
        self.assertGreater(verify, _step_index(steps, _named("Collect release assets")))
        self.assertLess(verify, _step_index(steps, _named("Inspect existing release before mutation")))
        run = steps[verify]["run"]
        self.assertIn("expected-release-assets.txt", run)
        self.assertIn("--stable", run)
        self.assertIn("set -euo pipefail", run)

    def test_stable_asset_download_excludes_provenance_records(self) -> None:
        steps = self.jobs["release"]["steps"]
        packages = steps[_step_index(steps, _named("Download channel build artifacts"))]
        self.assertIn("'SparkEngine-Windows-MinSizeRel-packages'", packages["with"]["pattern"])

    def test_workflow_tool_and_test_are_not_git_ignored(self) -> None:
        for path in (TOOL, Path(__file__).resolve()):
            with self.subTest(path=path.name):
                ignored = subprocess.run(["git", "-C", str(ROOT), "check-ignore", "-q", str(path)],
                                         capture_output=True, check=False)
                self.assertEqual(ignored.returncode, 1, f"{path} is git-ignored and would never ship")

    def test_release_controller_self_tests_the_provenance_gate(self) -> None:
        steps = self.jobs["prepare"]["steps"]
        index = _step_index(steps, _runs("Tests/Tools/test_release_build_provenance.py"))
        self.assert_unconditional(steps[index])
        self.assertLess(index, _step_index(steps, _named("Compute release metadata")))


if __name__ == "__main__":
    unittest.main()
