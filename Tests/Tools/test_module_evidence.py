#!/usr/bin/env python3
"""Adversarial tests for the RDY-010 module evidence validator.

Every case identified `B01`..`B36` is a bypass shape that an independent
hostile probe demonstrated the previous validator accepted — 36 of 36 hostile
manifests validated clean while 60 unit tests passed.  Each is pinned here as
a rejection test, so the false-green cannot return silently.

The suite deliberately opens with `TestGoldenPath`: a validator that rejects
everything would satisfy every rejection test in this file while being just as
useless as one that accepts everything.  The golden tests prove the validator
still says yes to a fully evidenced, fully truthful manifest.

No repository file is modified.  Fixtures are built in temporary directories.
"""

from __future__ import annotations

from contextlib import ExitStack, redirect_stderr, redirect_stdout
import copy
from dataclasses import dataclass, replace
import hashlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path
from typing import Any
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "module-evidence"))

import artifacts  # noqa: E402
import lifecycle as lifecycle_mod  # noqa: E402
import paths as paths_mod  # noqa: E402
import provenance  # noqa: E402
import schema as schema_mod  # noqa: E402
import strict_json  # noqa: E402
import targets as targets_mod  # noqa: E402
import validate_manifest as validate_manifest_mod  # noqa: E402
from schema import EVIDENCE_PRODUCERS, expected_library_names  # noqa: E402
from validate_manifest import ManifestValidator, load_manifest  # noqa: E402

INCLUDED = "SparkGameFPS"
DECOY = "SparkGameDecoy"
PROFILE = "stable-v1"


# --------------------------------------------------------------------------
# Fixture construction
# --------------------------------------------------------------------------
def _git(repo: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        capture_output=True, text=True, check=True, timeout=60,
    )


def build_fake_repo(root: Path) -> str:
    """A minimal repository with the shape the validator reads.

    Returns the HEAD commit SHA.  A real git repository is used because the
    revision and source-tree bindings are verified through git; stubbing them
    would test the stub rather than the binding.
    """
    for module in (INCLUDED, DECOY):
        src = root / "GameModules" / module / "Source"
        src.mkdir(parents=True, exist_ok=True)
        (src / "Main.cpp").write_text(
            f"// {module} entry point\nextern \"C\" void CreateModule() {{}}\n",
            encoding="utf-8",
        )
        # add_library appears only inside a comment: text that looks like a
        # target declaration but declares nothing.
        (root / "GameModules" / module / "CMakeLists.txt").write_text(
            f"# add_library({module} SHARED Source/Main.cpp)\n"
            f"message(STATUS \"nothing is built here\")\n",
            encoding="utf-8",
        )

    site = root / "docs" / "site"
    site.mkdir(parents=True, exist_ok=True)
    (site / "readiness.json").write_text(
        json.dumps({"releaseProfiles": [{"id": PROFILE, "name": "Stable v1"}]}),
        encoding="utf-8",
    )
    items = root / "docs" / "readiness" / "work-items"
    items.mkdir(parents=True, exist_ok=True)
    (items / "00-truth.json").write_text(
        json.dumps({"workItems": [
            {"id": "MOD-310"}, {"id": "RDY-015"}, {"id": "RDY-010"},
        ]}),
        encoding="utf-8",
    )

    # Artifacts a producer would have written.  They are build outputs, so they
    # live outside version control; the validator requires them to exist and
    # carry semantically valid content.
    junit_path = root / EVIDENCE_PRODUCERS["junit-xml"]["artifact"]
    junit_path.parent.mkdir(parents=True, exist_ok=True)
    junit_path.write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<testsuites tests="5" failures="0" errors="0">\n'
        f'  <testsuite name="{INCLUDED}" tests="5" failures="0" errors="0">\n'
        f'    <testcase name="test_load" classname="{INCLUDED}.Module"/>\n'
        f'    <testcase name="test_init" classname="{INCLUDED}.Module"/>\n'
        f'    <testcase name="test_update" classname="{INCLUDED}.Module"/>\n'
        f'    <testcase name="test_unload" classname="{INCLUDED}.Module"/>\n'
        f'    <testcase name="test_shutdown" classname="{INCLUDED}.Module"/>\n'
        '  </testsuite>\n'
        '</testsuites>\n',
        encoding="utf-8",
    )
    smoke_path = root / EVIDENCE_PRODUCERS["package-smoke-log"]["artifact"]
    smoke_path.parent.mkdir(parents=True, exist_ok=True)
    smoke_path.write_text(
        f"[package-smoke] {INCLUDED}\n"
        f"[package-smoke] product=SparkEngine\n"
        f"[package-smoke] module={INCLUDED}\n"
        f"[package-smoke] library={INCLUDED}.dll\n"
        f"[package-smoke] exit_code=0\n"
        f"[package-smoke] PASS\n",
        encoding="utf-8",
    )

    _git(root, "init", "-q", "-b", "main")
    (root / ".gitignore").write_text("build/\n", encoding="utf-8")
    _git(root, "config", "user.email", "test@example.invalid")
    _git(root, "config", "user.name", "Test")
    _git(root, "add", "-A")
    _git(root, "commit", "-q", "-m", "fixture")
    return _git(root, "rev-parse", "HEAD").stdout.strip()


def module_entry(name: str, *, included: bool) -> dict[str, Any]:
    libs = expected_library_names(name)
    entry: dict[str, Any] = {
        "name": name,
        "cmakeTarget": name,
        "sharedLibrary": {"windows": libs["windows"], "linux": libs["linux"]},
        "sourceDirectory": f"GameModules/{name}/Source",
        "moduleKind": "Game",
        "profileApplicability": {PROFILE: "required" if included else "outside"},
        "dependencies": [],
    }
    if included:
        entry["packageSmokeOwner"] = "MOD-310"
        entry["evidenceBindings"] = [
            {"type": t, "artifactPattern": EVIDENCE_PRODUCERS[t]["artifact"]}
            for t in ("cmake-target-index", "lifecycle-log", "junit-xml",
                      "package-smoke-log")
        ]
    else:
        entry["evidenceBindings"] = []
        entry["experimentalSeparation"] = {"trackedUnder": "RDY-015"}
    return entry


def base_manifest(*, with_decoy: bool = False) -> dict[str, Any]:
    modules = [module_entry(INCLUDED, included=True)]
    excluded: list[str] = []
    if with_decoy:
        modules.append(module_entry(DECOY, included=False))
        excluded.append(DECOY)
    return {
        "schemaVersion": "stable-v2",
        "profiles": [{
            "id": PROFILE,
            "includedModules": [INCLUDED],
            "excludedModules": excluded,
        }],
        "modules": modules,
    }


def target_index(*, name: str = INCLUDED, ttype: str = "SHARED_LIBRARY",
                 sources: list[str] | None = None,
                 name_on_disk: str | None = None,
                 source_directory: str | None = None) -> dict[str, Any]:
    if sources is None:
        sources = [f"GameModules/{name}/Source/Main.cpp"]
    return {
        "schemaVersion": targets_mod.INDEX_SCHEMA_VERSION,
        "targets": {
            name: {
                "name": name,
                "type": ttype,
                "nameOnDisk": name_on_disk or f"lib{name}.so",
                "sourceDirectory": source_directory or f"GameModules/{name}",
                "sources": sources,
                "artifacts": [f"bin/lib{name}.so"],
            }
        },
    }


FAKE_ENGINE_SHA256 = "a" * 64
FAKE_ENGINE_PATH = r"C:\verified-artifact\SparkEngine.exe"
FAKE_MODULE_SHA256 = "b" * 64


def pe_image() -> bytes:
    """Minimal PE-shaped fixture; a bare MZ marker is not an executable."""
    image = bytearray(8192)
    image[:2] = b"MZ"
    image[0x3C:0x40] = (0x80).to_bytes(4, "little")
    image[0x80:0x84] = b"PE\0\0"
    return bytes(image)


@dataclass(frozen=True)
class FakeLeaseIdentity:
    """A complete, handle-style identity for collector lease tests."""

    volume_serial: int
    file_index: int
    size: int
    last_write_time: int = 1


class FakeImageLease:
    """Controlled lease double for exercising the collector's real boundaries."""

    def __init__(self, path: Path, *, identity: FakeLeaseIdentity,
                 digest: str, final_path: str | None = None,
                 attributes: int = 0, image: bytes | None = None) -> None:
        self.path = path
        self.identity = identity
        self.final_path = final_path or str(path)
        self.attributes = attributes
        self._digest = digest
        self._image = image or pe_image()
        self.closed = False

    def read_at(self, offset: int, size: int) -> bytes:
        if self.closed:
            raise OSError("attempted to read a closed fake image lease")
        return self._image[offset:offset + size]

    def sha256(self) -> str:
        if self.closed:
            raise OSError("attempted to hash a closed fake image lease")
        return self._digest

    def close(self) -> None:
        self.closed = True


class FakeOutputDirectoryLease:
    """Controlled directory-handle double for collector publication tests."""

    def __init__(self, path: Path, *, identity: FakeLeaseIdentity,
                 attributes: int, final_path: str | None = None) -> None:
        self.path = path
        self.identity = identity
        self.attributes = attributes
        self.final_path = final_path or str(path)
        self.closed = False

    def snapshot(self) -> tuple[FakeLeaseIdentity, int]:
        if self.closed:
            raise OSError("attempted to inspect a closed fake output directory lease")
        return self.identity, self.attributes

    def close(self) -> None:
        self.closed = True


class FakeOutputArtifact:
    """A test-only fixed-leaf authority used by explicit collector test seams."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.closed = False

    def duplicate(self) -> FakeOutputArtifact:
        if self.closed:
            raise OSError("attempted to duplicate a closed fake output artifact")
        return FakeOutputArtifact(self.path)

    def verify(self, expected_path: Path) -> None:
        if self.closed:
            raise OSError("attempted to verify a closed fake output artifact")
        if self.path != expected_path or not self.path.is_file():
            raise OSError("fake output artifact no longer names its fixed leaf")

    def close(self) -> None:
        self.closed = True

    def discard(self) -> None:
        if self.closed:
            return
        self.path.unlink(missing_ok=True)
        self.closed = True


def write_image_manifest(root: Path, engine: Path, module: Path, sha: str,
                          *, engine_digest: str | None = None) -> Path:
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    path = root / "module-lifecycle-images.json"
    path.write_text(json.dumps({
        "schemaVersion": "spark-image-manifest-v1", "commitSHA": sha,
        "images": [
            {"path": "SparkEngine.exe", "sha256": engine_digest or digest(engine)},
            {"path": "SparkGameFPS.dll", "sha256": digest(module)},
        ],
    }), encoding="utf-8")
    return path


def lifecycle_evidence(repo: Path, sha: str, *, module: str = INCLUDED,
                       phases: dict[str, int] | None = None,
                       tree_sha: str | None = None) -> dict[str, Any]:
    if tree_sha is None:
        tree_sha, err = lifecycle_mod.source_tree_sha(
            repo, sha, f"GameModules/{module}/Source"
        )
        assert tree_sha is not None, err
    if phases is None:
        phases = {p: 1 for p in lifecycle_mod.REQUIRED_RUNTIME_PHASES}
        phases["OnUpdate"] = 120
        # These are part of the stable-v1 headless collector contract even
        # while older consumers only required the five lifecycle phases.
        phases["OnFixedUpdate"] = 30
        phases["OnRender"] = 120
    return {
        "schemaVersion": lifecycle_mod.LIFECYCLE_SCHEMA_VERSION,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "commitSHA": sha,
        "records": [{
            "module": module,
            "sharedLibrary": expected_library_names(module)["windows"],
            "sourceDirectory": f"GameModules/{module}/Source",
            "sourceTreeSHA": tree_sha,
            "runner": "headless-exec",
            "phases": phases,
            "engineSHA256": FAKE_ENGINE_SHA256,
            "enginePath": FAKE_ENGINE_PATH,
            "moduleSHA256": FAKE_MODULE_SHA256,
            "modulePath": rf"C:\verified-artifact\{expected_library_names(module)['windows']}",
        }],
    }


def target_index_for_revision(sha: str) -> dict[str, Any]:
    """Add the external revision fields required by release target evidence."""
    document = target_index()
    document.update({
        "commitSHA": sha,
        "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "source": "test",
    })
    return document


class FixtureCase(unittest.TestCase):
    """Base class owning one temporary repository for the whole class."""

    repo: Path
    sha: str
    _tmp: str

    @classmethod
    def setUpClass(cls) -> None:
        cls._tmp = tempfile.mkdtemp(prefix="spark-rdy010-")
        cls.repo = Path(cls._tmp) / "repo"
        cls.repo.mkdir()
        cls.sha = build_fake_repo(cls.repo)

    @classmethod
    def tearDownClass(cls) -> None:
        shutil.rmtree(cls._tmp, ignore_errors=True)

    def validate(self, manifest: dict[str, Any], **over: Any) -> list[str]:
        default_target_index = target_index()
        default_target_index.update({
            "commitSHA": self.sha,
            "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "source": "test",
        })
        kwargs: dict[str, Any] = {
            "target_index": default_target_index,
            "lifecycle_evidence": lifecycle_evidence(self.repo, self.sha),
            "expected_sha": self.sha,
        }
        kwargs.update(over)
        supplied_target_index = kwargs.get("target_index")
        if isinstance(supplied_target_index, dict) and "commitSHA" not in supplied_target_index:
            supplied_target_index.setdefault("commitSHA", self.sha)
            supplied_target_index.setdefault(
                "generatedAt", datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            )
            supplied_target_index.setdefault("source", "test")
        return ManifestValidator(manifest, self.repo, **kwargs).validate()

    def assertRejected(self, manifest: dict[str, Any], case: str,
                       **over: Any) -> list[str]:
        errors = self.validate(manifest, **over)
        self.assertNotEqual(
            errors, [],
            f"[{case}] hostile manifest was ACCEPTED — this is a false-green",
        )
        return errors

    def assertAccepted(self, manifest: dict[str, Any], **over: Any) -> None:
        errors = self.validate(manifest, **over)
        self.assertEqual(errors, [], f"truthful manifest was rejected: {errors}")


# --------------------------------------------------------------------------
# Golden path — the validator must still say yes to the truth
# --------------------------------------------------------------------------
class TestGoldenPath(FixtureCase):
    """Guards against a validator that passes every rejection test by
    rejecting everything."""

    def test_fully_evidenced_manifest_is_accepted(self) -> None:
        self.assertAccepted(base_manifest())

    def test_manifest_with_experimental_module_is_accepted(self) -> None:
        self.assertAccepted(base_manifest(with_decoy=True))

    def test_shipped_manifest_passes_the_declarative_layer(self) -> None:
        """The checked-in manifest must at least be internally consistent."""
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json"
        )
        errors = ManifestValidator(manifest, REPO_ROOT, policy_only=True).validate()
        self.assertEqual(errors, [], f"shipped manifest has policy errors: {errors}")

    def test_shipped_manifest_declares_all_eleven_modules(self) -> None:
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json"
        )
        self.assertEqual(len(manifest["modules"]), 11)

    def test_shipped_manifest_blocks_without_runtime_evidence(self) -> None:
        """RDY-010's own manifest must not validate while evidence is absent."""
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json"
        )
        errors = ManifestValidator(manifest, REPO_ROOT).validate()
        self.assertTrue(
            any("unavailable" in e for e in errors),
            f"expected a blocking unavailable-evidence error, got {errors}",
        )


# --------------------------------------------------------------------------
# B01-B02 — target and source proof must be authoritative, not textual
# --------------------------------------------------------------------------
class TestTargetProof(FixtureCase):

    def test_B01_comment_only_add_library_is_not_a_target(self) -> None:
        """The fixture's CMakeLists declares the target only in a comment."""
        errors = self.assertRejected(base_manifest(), "B01", target_index=None,
                                     target_error="no codemodel")
        self.assertTrue(any("target evidence is unavailable" in e.lower()
                            or "unavailable" in e for e in errors))

    def test_B01b_target_absent_from_codemodel_is_rejected(self) -> None:
        self.assertRejected(base_manifest(), "B01b",
                            target_index=target_index(name="SomethingElse"))

    def test_B02_target_with_zero_sources_is_rejected(self) -> None:
        self.assertRejected(base_manifest(), "B02",
                            target_index=target_index(sources=[]))

    def test_B02b_sources_outside_declared_tree_are_rejected(self) -> None:
        self.assertRejected(
            base_manifest(), "B02b",
            target_index=target_index(sources=["SparkEngine/Source/Other.cpp"]),
        )

    def test_target_of_wrong_type_is_rejected(self) -> None:
        self.assertRejected(base_manifest(), "B01c",
                            target_index=target_index(ttype="STATIC_LIBRARY"))

    def test_target_index_naming_a_foreign_library_is_rejected(self) -> None:
        self.assertRejected(base_manifest(), "B01d",
                            target_index=target_index(name_on_disk="libOther.so"))

    def test_toolchain_variant_library_names_are_accepted(self) -> None:
        """MinGW emits libX.dll, MSVC X.dll, GCC libX.so — all legitimate."""
        for on_disk in (f"lib{INCLUDED}.so", f"lib{INCLUDED}.dll",
                        f"{INCLUDED}.dll", f"lib{INCLUDED}.dylib"):
            with self.subTest(nameOnDisk=on_disk):
                self.assertAccepted(
                    base_manifest(),
                    target_index=target_index(name_on_disk=on_disk),
                )

    def test_missing_target_ancestor_is_rejected_rather_than_returning_empty(self) -> None:
        with self.assertRaises(targets_mod.TargetEvidenceRejected):
            targets_mod.load_target_index(self.repo / "nope" / "targets.json")

    def test_missing_target_leaf_is_the_only_unavailable_case(self) -> None:
        with self.assertRaises(targets_mod.TargetEvidenceUnavailable):
            targets_mod.load_target_index(self.repo / "missing-targets.json")

    def test_codemodel_with_no_targets_raises(self) -> None:
        reply = self.repo / "emptyreply"
        reply.mkdir(exist_ok=True)
        with self.assertRaises(targets_mod.TargetEvidenceUnavailable):
            targets_mod.extract_from_reply(reply)


# --------------------------------------------------------------------------
# B03-B09 — exact lexical and canonical source paths
# --------------------------------------------------------------------------
class TestSourceDirectoryExactness(FixtureCase):

    def _with_source(self, value: str) -> dict[str, Any]:
        m = base_manifest()
        m["modules"][0]["sourceDirectory"] = value
        return m

    def test_B03_module_root_is_not_the_source_tree(self) -> None:
        self.assertRejected(self._with_source(f"GameModules/{INCLUDED}"), "B03")

    def test_B04_source_subdirectory_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/Source/Core"), "B04")

    def test_B05_dot_segment_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/./Source"), "B05")

    def test_B06_empty_segment_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules//{INCLUDED}/Source"), "B06")

    def test_B07_case_aliased_source_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/source"), "B07")

    def test_B07b_case_aliased_root_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"gamemodules/{INCLUDED}/Source"), "B07b")

    def test_B08_trailing_slash_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/Source/"), "B08")

    def test_B09_reparse_point_source_is_rejected(self) -> None:
        link = self.repo / "GameModules" / INCLUDED / "Linked"
        target = self.repo / "GameModules" / DECOY / "Source"
        made = False
        if not link.exists():
            try:
                os.symlink(str(target), str(link), target_is_directory=True)
                made = True
            except (OSError, NotImplementedError):
                if sys.platform == "win32":
                    rc = subprocess.run(
                        ["cmd", "/c", "mklink", "/J", str(link), str(target)],
                        capture_output=True, text=True, check=False,
                    )
                    made = rc.returncode == 0
        if not (made or link.exists()):
            self.skipTest("this OS/account cannot create symlinks or junctions")
        self.assertTrue(
            paths_mod._is_reparse_point(link),
            "fixture link was not detected as a reparse point",
        )
        # Even under its exact expected name, a reparse point is not source.
        errors = paths_mod.check_on_disk(
            f"GameModules/{INCLUDED}/Linked", self.repo)
        self.assertNotEqual(errors, [], "[B09] reparse point was ACCEPTED")

    def test_traversal_escape_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/Source/../../{DECOY}/Source"),
            "B03b")

    def test_sibling_module_source_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{DECOY}/Source"), "B03c")

    def test_absolute_and_home_paths_are_rejected(self) -> None:
        for value in ("/etc/passwd", "C:/Windows", "~/evil", "$HOME/evil",
                      "%USERPROFILE%/evil", "\\\\server\\share"):
            with self.subTest(path=value):
                self.assertRejected(self._with_source(value), "B03d")

    def test_alternate_data_stream_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GameModules/{INCLUDED}/Source:hidden"), "B03e")

    def test_short_name_alias_is_rejected(self) -> None:
        self.assertRejected(
            self._with_source(f"GAMEMO~1/{INCLUDED}/Source"), "B03f")

    def test_reserved_device_name_is_rejected(self) -> None:
        self.assertRejected(self._with_source(f"GameModules/{INCLUDED}/NUL"),
                            "B03g")

    def test_nonexistent_directory_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["name"] = "SparkGameGhost"
        m["modules"][0]["cmakeTarget"] = "SparkGameGhost"
        m["modules"][0]["sharedLibrary"] = {
            "windows": "SparkGameGhost.dll", "linux": "libSparkGameGhost.so"}
        m["modules"][0]["sourceDirectory"] = "GameModules/SparkGameGhost/Source"
        m["profiles"][0]["includedModules"] = ["SparkGameGhost"]
        self.assertRejected(m, "B03h")


# --------------------------------------------------------------------------
# B10 — declared phases are not runtime proof
# --------------------------------------------------------------------------
class TestLifecycleIsRuntimeProof(FixtureCase):

    def _prepare_cli_evidence_root(self, root: Path) -> tuple[str, Path, Path]:
        """Build one self-contained, semantically valid CLI evidence fixture."""
        sha = build_fake_repo(root)
        target_path = root / EVIDENCE_PRODUCERS["cmake-target-index"]["artifact"]
        target_path.parent.mkdir(parents=True, exist_ok=True)
        targets = target_index()
        targets.update({
            "schemaVersion": targets_mod.INDEX_SCHEMA_VERSION,
            "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "commitSHA": sha,
            "source": "cmake-file-api",
        })
        target_path.write_text(json.dumps(targets), encoding="utf-8")
        evidence_path = root / EVIDENCE_PRODUCERS["lifecycle-log"]["artifact"]
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        evidence_path.write_text(
            json.dumps(lifecycle_evidence(root, sha)), encoding="utf-8",
        )
        manifest_path = root / "cli-manifest.json"
        manifest_path.write_text(json.dumps(base_manifest()), encoding="utf-8")
        return sha, evidence_path, manifest_path

    def _write_cli_lifecycle_gap_ledger(self, name: str) -> Path:
        """Write the one intentionally declared absence used by CLI regressions."""
        path = self.repo / f"{name}-evidence-gaps.json"
        path.write_text(json.dumps({
            "schemaVersion": "evidence-gaps-v1",
            "gaps": [{
                "evidenceType": "lifecycle-log",
                "trackedUnder": "RDY-010",
                "reason": (
                    "The fixture deliberately models a genuinely absent lifecycle "
                    "producer while the RDY-010 gap remains tracked."
                ),
            }],
        }), encoding="utf-8")
        return path


    def test_B10_declared_phase_list_is_not_accepted_as_evidence(self) -> None:
        """The schema no longer even has a lifecyclePhases key to declare."""
        m = base_manifest()
        m["modules"][0]["lifecyclePhases"] = [
            "SparkGetModuleCompatibility", "CreateModule", "GetModuleInfo",
            "OnLoad", "OnUpdate", "OnUnload", "DestroyModule",
        ]
        errors = self.assertRejected(m, "B10")
        self.assertTrue(any("unknown keys" in e and "lifecyclePhases" in e
                            for e in errors), errors)

    def test_missing_lifecycle_evidence_blocks(self) -> None:
        errors = self.assertRejected(
            base_manifest(), "B10b",
            lifecycle_evidence=None, lifecycle_error="not found")
        self.assertTrue(any("lifecycle evidence is unavailable" in e for e in errors))

    def test_zero_count_phase_did_not_run(self) -> None:
        phases = {p: 1 for p in lifecycle_mod.REQUIRED_RUNTIME_PHASES}
        phases["OnUpdate"] = 0
        self.assertRejected(
            base_manifest(), "B10c",
            lifecycle_evidence=lifecycle_evidence(self.repo, self.sha, phases=phases))

    def test_each_required_phase_is_individually_required(self) -> None:
        for phase in lifecycle_mod.REQUIRED_RUNTIME_PHASES:
            with self.subTest(phase=phase):
                phases = {p: 1 for p in lifecycle_mod.REQUIRED_RUNTIME_PHASES}
                del phases[phase]
                self.assertRejected(
                    base_manifest(), f"B10-{phase}",
                    lifecycle_evidence=lifecycle_evidence(
                        self.repo, self.sha, phases=phases))

    def test_evidence_for_a_different_source_tree_is_rejected(self) -> None:
        self.assertRejected(
            base_manifest(), "B10d",
            lifecycle_evidence=lifecycle_evidence(
                self.repo, self.sha, tree_sha="0" * 40))

    def test_evidence_for_a_different_module_is_rejected(self) -> None:
        self.assertRejected(
            base_manifest(), "B10e",
            lifecycle_evidence=lifecycle_evidence(self.repo, self.sha, module=DECOY))

    def test_unknown_runner_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["runner"] = "trust-me"
        self.assertRejected(base_manifest(), "B10f", lifecycle_evidence=ev)

    def test_unknown_phase_name_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["phases"]["OnMagic"] = 1
        self.assertRejected(base_manifest(), "B10g", lifecycle_evidence=ev)

    def test_lifecycle_record_with_unknown_keys_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["backdoor"] = "accepted"
        self.assertRejected(base_manifest(), "B10h", lifecycle_evidence=ev)

    def test_boolean_phase_count_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["phases"]["OnUpdate"] = True
        self.assertRejected(base_manifest(), "B10i", lifecycle_evidence=ev)

    def test_empty_records_raises_rather_than_passing(self) -> None:
        path = self.repo / "lc.json"
        path.write_text(json.dumps({
            "schemaVersion": lifecycle_mod.LIFECYCLE_SCHEMA_VERSION,
            "records": [],
        }), encoding="utf-8")
        with self.assertRaises(lifecycle_mod.LifecycleEvidenceUnavailable):
            lifecycle_mod.load_lifecycle_evidence(path)

    def test_loader_classifies_only_a_missing_leaf_as_downgradeable(self) -> None:
        """A gap ledger may soften absence, never a present unsafe document."""
        missing = self.repo / "missing-lifecycle-evidence.json"
        with self.assertRaises(lifecycle_mod.LifecycleEvidenceUnavailable) as raised:
            lifecycle_mod.load_lifecycle_evidence(missing)
        self.assertNotIsInstance(
            raised.exception, lifecycle_mod.LifecycleEvidenceRejected,
            "a missing lifecycle leaf was misclassified as a fatal rejection",
        )

    def test_loader_classifies_missing_ancestor_as_fatal_authority(self) -> None:
        """A vanished parent is not evidence that only the final leaf is absent."""
        missing = self.repo / "missing-lifecycle-parent" / "module-lifecycle.json"
        with self.assertRaises(lifecycle_mod.LifecycleEvidenceAuthorityError):
            lifecycle_mod.load_lifecycle_evidence(missing)

    def test_root_lease_reads_relative_bytes_without_path_reopen(self) -> None:
        """A held repository root supplies exact bytes for a relative artifact."""
        root = self.repo / "rooted-reader-root"
        evidence = root / "build" / "module-evidence" / "module-targets.json"
        evidence.parent.mkdir(parents=True)
        payload = b'{"rooted": true}\n'
        evidence.write_bytes(payload)

        with strict_json.open_no_follow_directory_lease(root, label="test root") as lease:
            actual = lease.read_relative_bytes(
                "build/module-evidence/module-targets.json", max_bytes=1024,
            )

        self.assertEqual(actual, payload)

    def test_root_lease_rejects_relative_reparse_ancestor(self) -> None:
        """A child reparse cannot redirect a read rooted at the held repository."""
        root = self.repo / "rooted-reader-reparse-root"
        root.mkdir()
        attacker = self.repo / "rooted-reader-attacker"
        attacker.mkdir()
        (attacker / "module-targets.json").write_text("{}", encoding="utf-8")
        alias = root / "build"
        try:
            os.symlink(str(attacker), str(alias), target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"cannot create rooted relative reparse fixture: {exc}")

        with strict_json.open_no_follow_directory_lease(root, label="test root") as lease:
            with self.assertRaises(strict_json.NoFollowAuthorityError):
                lease.read_relative_bytes("build/module-targets.json", max_bytes=1024)

    @unittest.skipIf(os.name == "nt", "Windows root handles prevent the rename itself")
    def test_posix_root_lease_reads_original_bytes_after_root_rename(self) -> None:
        """POSIX rooted descriptors survive a root pathname replacement."""
        root = self.repo / "rooted-reader-renamed-root"
        evidence = root / "build" / "module-evidence" / "module-targets.json"
        evidence.parent.mkdir(parents=True)
        payload = b'{"original": true}\n'
        evidence.write_bytes(payload)
        moved = self.repo / "rooted-reader-moved-root"

        with strict_json.open_no_follow_directory_lease(root, label="test root") as lease:
            root.rename(moved)
            actual = lease.read_relative_bytes(
                "build/module-evidence/module-targets.json", max_bytes=1024,
            )

        self.assertEqual(actual, payload)

    @unittest.skipIf(os.name == "nt", "POSIX descriptor-rooted Git regression")
    def test_posix_root_lease_git_cwd_survives_root_rename(self) -> None:
        """Git receives an inherited directory descriptor, not a rebuilt path."""
        root = self.repo / "rooted-git-root"
        expected_sha = build_fake_repo(root)
        moved = self.repo / "rooted-git-root-moved"

        with strict_json.open_no_follow_directory_lease(root, label="test root") as lease:
            cwd, pass_fds = lease.posix_git_cwd()
            root.rename(moved)
            proc = subprocess.run(
                ["git", "-C", cwd, "rev-parse", "HEAD"],
                capture_output=True, check=False, text=True, timeout=30,
                pass_fds=pass_fds,
            )

        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.strip(), expected_sha)

    @unittest.skipIf(os.name == "nt", "POSIX rooted contract regression")
    def test_posix_root_lease_reads_contracts_after_root_rename(self) -> None:
        """Profile and work-item identities come from the held original root."""
        root = self.repo / "rooted-contract-root"
        build_fake_repo(root)
        moved = self.repo / "rooted-contract-root-moved"

        with strict_json.open_no_follow_directory_lease(root, label="test root") as lease:
            root.rename(moved)
            profiles, profile_error = schema_mod.load_known_profile_ids_rooted(lease)
            items, item_error = schema_mod.load_known_work_item_ids_rooted(lease)

        self.assertIsNone(profile_error)
        self.assertIsNone(item_error)
        self.assertIn("stable-v1", profiles)
        self.assertIn("RDY-010", items)

    @unittest.skipIf(os.name == "nt", "POSIX rooted source-directory regression")
    def test_posix_root_lease_checks_source_after_root_rename(self) -> None:
        """Source-directory identity is checked under the held original root."""
        root = self.repo / "rooted-source-root"
        build_fake_repo(root)
        moved = self.repo / "rooted-source-root-moved"

        with strict_json.open_no_follow_directory_lease(root, label="test root") as lease:
            root.rename(moved)
            errors = paths_mod.check_source_directory_rooted(
                "GameModules/SparkGameFPS/Source", "SparkGameFPS", lease,
            )

        self.assertEqual(errors, [], errors)

    @unittest.skipIf(os.name == "nt", "POSIX rooted provenance regression")
    def test_posix_root_lease_binds_head_and_source_tree_after_root_rename(self) -> None:
        """Revision and tree binding use descriptor-rooted Git after rename."""
        root = self.repo / "rooted-provenance-root"
        expected_sha = build_fake_repo(root)
        moved = self.repo / "rooted-provenance-root-moved"

        with strict_json.open_no_follow_directory_lease(root, label="test root") as lease:
            root.rename(moved)
            head, head_error = provenance.resolve_head_sha_rooted(lease)
            binding_errors = provenance.check_revision_binding_rooted(
                expected_sha, expected_sha, lease, "test commit",
            )
            tree_sha, tree_error = lifecycle_mod.source_tree_sha_rooted(
                lease, expected_sha, "GameModules/SparkGameFPS/Source",
            )

        self.assertIsNone(head_error)
        self.assertEqual(head, expected_sha)
        self.assertEqual(binding_errors, [], binding_errors)
        self.assertIsNone(tree_error)
        self.assertIsNotNone(tree_sha)

    @unittest.skipIf(os.name == "nt", "POSIX rooted Git environment regression")
    def test_posix_rooted_git_sanitizes_environment_overrides(self) -> None:
        """GIT_DIR and related overrides cannot replace the held checkout."""
        root = self.repo / "rooted-git-environment-root"
        expected_sha = build_fake_repo(root)
        attacker = self.repo / "rooted-git-environment-attacker"
        build_fake_repo(attacker)
        (attacker / "attacker-marker.txt").write_text("different rooted Git fixture\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(attacker), "add", "attacker-marker.txt"], check=True)
        subprocess.run(
            ["git", "-C", str(attacker), "-c", "user.email=test@example.invalid",
             "-c", "user.name=Test", "commit", "-m", "attacker fixture"],
            check=True, capture_output=True, text=True,
        )
        attacker_sha = subprocess.run(
            ["git", "-C", str(attacker), "rev-parse", "HEAD"],
            check=True, capture_output=True, text=True,
        ).stdout.strip()
        self.assertNotEqual(expected_sha, attacker_sha)

        with strict_json.open_no_follow_directory_lease(root, label="test root") as lease, \
                mock.patch.dict(os.environ, {
                    "GIT_DIR": str(attacker / ".git"),
                    "GIT_WORK_TREE": str(attacker),
                    "GIT_OBJECT_DIRECTORY": str(attacker / ".git" / "objects"),
                    "GIT_CONFIG_COUNT": "1",
                    "GIT_CONFIG_KEY_0": "core.worktree",
                    "GIT_CONFIG_VALUE_0": str(attacker),
                }, clear=False):
            head, error = provenance.resolve_head_sha_rooted(lease)

        self.assertIsNone(error)
        self.assertEqual(head, expected_sha)

    @unittest.skipIf(os.name == "nt", "POSIX rooted Git gitdir-file regression")
    def test_posix_rooted_git_rejects_external_gitdir_file(self) -> None:
        """A .git file cannot redirect rooted Git outside the held checkout."""
        root = self.repo / "rooted-gitdir-file-root"
        build_fake_repo(root)
        external_git = self.repo / "rooted-gitdir-file-external"
        (root / ".git").rename(external_git)
        (root / ".git").write_text(f"gitdir: {external_git}\n", encoding="utf-8")

        with strict_json.open_no_follow_directory_lease(root, label="test root") as lease:
            with self.assertRaises(strict_json.NoFollowAuthorityError):
                provenance.run_rooted_git(lease, "rev-parse", "HEAD")

    @unittest.skipIf(os.name == "nt", "POSIX rooted Git indirection regression")
    def test_posix_rooted_git_rejects_common_and_alternate_object_indirection(self) -> None:
        """Git metadata links cannot redirect held-root object lookup."""
        for name, relative in (
            ("commondir", ".git/commondir"),
            ("alternates", ".git/objects/info/alternates"),
        ):
            with self.subTest(name=name):
                root = self.repo / f"rooted-git-{name}-root"
                build_fake_repo(root)
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("/tmp/attacker-git\n", encoding="utf-8")

                with strict_json.open_no_follow_directory_lease(root, label="test root") as lease:
                    with self.assertRaises(strict_json.NoFollowAuthorityError):
                        provenance.run_rooted_git(lease, "rev-parse", "HEAD")

    def test_loader_classifies_malformed_present_evidence_as_fatal_rejection(self) -> None:
        path = self.repo / "malformed-lifecycle-evidence.json"
        path.write_text("{not valid JSON", encoding="utf-8")
        with self.assertRaises(lifecycle_mod.LifecycleEvidenceRejected):
            lifecycle_mod.load_lifecycle_evidence(path)

    def test_loader_rejects_reparse_leaf_instead_of_reading_its_target(self) -> None:
        """A lifecycle leaf reparse point cannot redirect the evidence reader."""
        target = self.repo / "external-lifecycle-evidence.json"
        target.write_text(
            json.dumps(lifecycle_evidence(self.repo, self.sha)), encoding="utf-8",
        )
        leaf = self.repo / "module-lifecycle-reparse.json"
        try:
            os.symlink(str(target), str(leaf))
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"cannot create a lifecycle leaf reparse fixture: {exc}")

        with self.assertRaisesRegex(
            lifecycle_mod.LifecycleEvidenceRejected, "reparse|symlink|unsafe",
        ):
            lifecycle_mod.load_lifecycle_evidence(leaf)
        self.assertTrue(target.is_file(), "the external evidence target was altered")

    def test_loader_rejects_post_handoff_ancestor_swap_without_accepting_external_json(self) -> None:
        """A producer-directory swap after exit must not redirect the validator."""
        output_directory = self.repo / "build" / "module-evidence"
        output_directory.mkdir(parents=True, exist_ok=True)
        evidence_path = output_directory / "module-lifecycle.json"
        evidence_path.write_text(
            json.dumps(lifecycle_evidence(self.repo, self.sha)), encoding="utf-8",
        )
        external_directory = self.repo / "external-handoff-target"
        external_directory.mkdir()
        external_evidence = external_directory / evidence_path.name
        attacker_document = lifecycle_evidence(self.repo, self.sha)
        attacker_document["records"][0]["phases"]["OnUpdate"] = 999_999
        attacker_payload = json.dumps(attacker_document)
        external_evidence.write_text(attacker_payload, encoding="utf-8")
        preserved_directory = self.repo / "module-evidence-before-swap"
        output_directory.rename(preserved_directory)
        try:
            os.symlink(str(external_directory), str(output_directory), target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            preserved_directory.rename(output_directory)
            self.skipTest(f"cannot create a lifecycle ancestor reparse fixture: {exc}")

        with self.assertRaisesRegex(
            lifecycle_mod.LifecycleEvidenceRejected, "reparse|symlink|unsafe",
        ):
            lifecycle_mod.load_lifecycle_evidence(evidence_path)
        self.assertEqual(external_evidence.read_text(encoding="utf-8"), attacker_payload)

    def test_loader_uses_held_no_follow_bytes_not_the_legacy_path_reader(self) -> None:
        """The lifecycle consumer cannot regress to strict_json's path-reopen API."""
        path = self.repo / "held-lifecycle-evidence.json"
        expected = lifecycle_evidence(self.repo, self.sha)
        path.write_text(json.dumps(expected), encoding="utf-8")

        with mock.patch(
            "strict_json.load_file",
            side_effect=AssertionError("legacy path reader was invoked"),
        ) as legacy_reader:
            loaded = lifecycle_mod.load_lifecycle_evidence(path)

        legacy_reader.assert_not_called()
        self.assertEqual(loaded, expected)

    @unittest.skipUnless(os.name == "nt", "native Windows consumer binding is unavailable")
    def test_loader_fails_closed_without_native_no_follow_reader(self) -> None:
        """Windows validation must not fall back to a path-following JSON read."""
        path = self.repo / "unavailable-native-lifecycle-evidence.json"
        path.write_text(json.dumps(lifecycle_evidence(self.repo, self.sha)), encoding="utf-8")

        with mock.patch("strict_json._WINDOWS_NO_FOLLOW_READER_AVAILABLE", False):
            with self.assertRaisesRegex(
                lifecycle_mod.LifecycleEvidenceAuthorityError, "unavailable",
            ):
                lifecycle_mod.load_lifecycle_evidence(path)

    def test_main_rejects_default_evidence_after_repo_root_reparse_swap(self) -> None:
        """CLI default evidence cannot inherit a repo-root symlink's target."""
        trusted_root = self.repo / "cli-root-before-swap"
        attacker_root = self.repo / "cli-root-attacker"
        self._prepare_cli_evidence_root(trusted_root)
        _attacker_sha, attacker_evidence, manifest_path = \
            self._prepare_cli_evidence_root(attacker_root)
        attacker_payload = attacker_evidence.read_text(encoding="utf-8")
        gap_ledger = self._write_cli_lifecycle_gap_ledger("root-reparse")

        invoked_root = self.repo / "cli-root"
        trusted_root.rename(invoked_root)
        preserved_root = self.repo / "cli-root-preserved"
        invoked_root.rename(preserved_root)
        try:
            os.symlink(str(attacker_root), str(invoked_root), target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            preserved_root.rename(invoked_root)
            self.skipTest(f"cannot create a repo-root reparse fixture: {exc}")

        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(invoked_root),
            "--manifest", str(manifest_path),
            "--allow-declared-gaps", str(gap_ledger),
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertNotEqual(
            result, 0,
            "repo-root resolution followed the reparse target and accepted attacker evidence",
        )
        self.assertIn("FATAL:", stderr.getvalue())
        self.assertNotIn("KNOWN GAP:", stderr.getvalue())
        self.assertEqual(attacker_evidence.read_text(encoding="utf-8"), attacker_payload)

    def test_main_rejects_explicit_normal_evidence_when_repo_root_is_a_reparse(self) -> None:
        """An explicit evidence path cannot bypass repository-root authority."""
        trusted_root = self.repo / "cli-explicit-root-before-swap"
        attacker_root = self.repo / "cli-explicit-root-attacker"
        self._prepare_cli_evidence_root(trusted_root)
        _attacker_sha, attacker_evidence, manifest_path = \
            self._prepare_cli_evidence_root(attacker_root)
        attacker_payload = attacker_evidence.read_text(encoding="utf-8")

        invoked_root = self.repo / "cli-explicit-root"
        trusted_root.rename(invoked_root)
        preserved_root = self.repo / "cli-explicit-root-preserved"
        invoked_root.rename(preserved_root)
        try:
            os.symlink(str(attacker_root), str(invoked_root), target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            preserved_root.rename(invoked_root)
            self.skipTest(f"cannot create a repo-root reparse fixture: {exc}")

        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(invoked_root),
            "--manifest", str(manifest_path),
            "--lifecycle-evidence", str(attacker_evidence),
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertNotEqual(
            result, 0,
            "an explicit normal evidence path bypassed the reparse-bearing repository root",
        )
        self.assertIn("FATAL:", stderr.getvalue())
        self.assertEqual(attacker_evidence.read_text(encoding="utf-8"), attacker_payload)

    def test_main_rejects_repo_root_beneath_a_reparse_ancestor(self) -> None:
        """A normal final root below a junction/symlink ancestor is still unsafe."""
        trusted_parent = self.repo / "cli-root-ancestor-trusted"
        attacker_parent = self.repo / "cli-root-ancestor-attacker"
        trusted_root = trusted_parent / "repo"
        attacker_root = attacker_parent / "repo"
        self._prepare_cli_evidence_root(trusted_root)
        _attacker_sha, attacker_evidence, manifest_path = \
            self._prepare_cli_evidence_root(attacker_root)
        attacker_payload = attacker_evidence.read_text(encoding="utf-8")

        ancestor = self.repo / "cli-root-ancestor-link"
        try:
            os.symlink(str(attacker_parent), str(ancestor), target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"cannot create a repo-root ancestor reparse fixture: {exc}")

        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(ancestor / "repo"),
            "--manifest", str(manifest_path),
            "--lifecycle-evidence", str(attacker_evidence),
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertNotEqual(result, 0)
        self.assertIn("FATAL:", stderr.getvalue())
        self.assertEqual(attacker_evidence.read_text(encoding="utf-8"), attacker_payload)

    def test_main_rejects_explicit_lifecycle_dot_segment_before_loader(self) -> None:
        """An explicit dot segment cannot be normalized away before held loading."""
        root = self.repo / "cli-explicit-dot-root"
        _sha, evidence_path, manifest_path = self._prepare_cli_evidence_root(root)
        gap_ledger = self._write_cli_lifecycle_gap_ledger("explicit-dot")
        explicit_alias = str(evidence_path.parent) + os.sep + "." + os.sep + evidence_path.name
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(root),
            "--manifest", str(manifest_path),
            "--lifecycle-evidence", explicit_alias,
            "--allow-declared-gaps", str(gap_ledger),
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertNotEqual(
            result, 0,
            "explicit lifecycle evidence dot segment was normalized before no-follow loading",
        )
        self.assertIn("dot or traversal segment", stderr.getvalue())

    def test_main_rejects_explicit_lifecycle_reparse_path(self) -> None:
        """The explicit CLI override retains the loader's no-follow contract."""
        root = self.repo / "cli-explicit-reparse-root"
        _sha, evidence_path, manifest_path = self._prepare_cli_evidence_root(root)
        attacker_evidence = self.repo / "cli-explicit-reparse-target.json"
        attacker_payload = evidence_path.read_text(encoding="utf-8")
        attacker_evidence.write_text(attacker_payload, encoding="utf-8")
        gap_ledger = self._write_cli_lifecycle_gap_ledger("explicit-reparse")
        alias = root / "explicit-lifecycle-reparse.json"
        try:
            os.symlink(str(attacker_evidence), str(alias))
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"cannot create an explicit lifecycle reparse fixture: {exc}")

        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(root),
            "--manifest", str(manifest_path),
            "--lifecycle-evidence", str(alias),
            "--allow-declared-gaps", str(gap_ledger),
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertNotEqual(result, 0)
        self.assertIn("FATAL:", stderr.getvalue())
        self.assertNotIn("KNOWN GAP:", stderr.getvalue())
        self.assertEqual(attacker_evidence.read_text(encoding="utf-8"), attacker_payload)

    def test_main_rejects_malformed_present_evidence_even_with_a_declared_gap(self) -> None:
        """The ledger ratchet applies only to absence, never malformed bytes."""
        root = self.repo / "cli-malformed-root"
        _sha, evidence_path, manifest_path = self._prepare_cli_evidence_root(root)
        evidence_path.write_text("{not valid JSON", encoding="utf-8")
        gap_ledger = self._write_cli_lifecycle_gap_ledger("malformed-present")
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(root),
            "--manifest", str(manifest_path),
            "--allow-declared-gaps", str(gap_ledger),
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertNotEqual(result, 0)
        self.assertIn("FATAL:", stderr.getvalue())
        self.assertNotIn("KNOWN GAP:", stderr.getvalue())

    def test_main_allows_a_declared_gap_for_a_genuinely_absent_lifecycle_leaf(self) -> None:
        """The release ledger remains compatible with an actually absent producer."""
        root = self.repo / "cli-absent-root"
        _sha, evidence_path, manifest_path = self._prepare_cli_evidence_root(root)
        evidence_path.unlink()
        gap_ledger = self._write_cli_lifecycle_gap_ledger("missing-leaf")
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(root),
            "--manifest", str(manifest_path),
            "--allow-declared-gaps", str(gap_ledger),
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertEqual(result, 0, stderr.getvalue())
        self.assertIn("KNOWN GAP: lifecycle-log", stderr.getvalue())
        self.assertIn("INCOMPLETE:", stdout.getvalue())

    def test_main_rejects_missing_explicit_lifecycle_ancestor_even_with_a_declared_gap(self) -> None:
        """Only the fixed leaf, never its parent chain, is a downgradeable gap."""
        root = self.repo / "cli-missing-ancestor-root"
        _sha, _evidence_path, manifest_path = self._prepare_cli_evidence_root(root)
        gap_ledger = self._write_cli_lifecycle_gap_ledger("missing-ancestor")
        missing = root / "missing-lifecycle-parent" / "module-lifecycle.json"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(root),
            "--manifest", str(manifest_path),
            "--lifecycle-evidence", str(missing),
            "--allow-declared-gaps", str(gap_ledger),
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertNotEqual(result, 0)
        self.assertIn("FATAL:", stderr.getvalue())
        self.assertNotIn("KNOWN GAP:", stderr.getvalue())

    def test_main_rejects_repo_root_dot_segment_before_default_loader(self) -> None:
        """A root alias cannot be normalized before default evidence composition."""
        root = self.repo / "cli-root-dot-root"
        self._prepare_cli_evidence_root(root)
        manifest_path = root / "cli-manifest.json"
        root_alias = str(root) + os.sep + "."
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", root_alias,
            "--manifest", str(manifest_path),
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertNotEqual(result, 0)
        self.assertIn("--repo-root contains a dot or traversal segment", stderr.getvalue())

    def test_main_rejects_current_directory_root_for_lifecycle_validation(self) -> None:
        """`.` is not authority-preserving on all supported platforms."""
        root = self.repo / "cli-current-directory-root"
        self._prepare_cli_evidence_root(root)
        manifest_path = root / "cli-manifest.json"
        original_cwd = os.getcwd()
        stdout = io.StringIO()
        stderr = io.StringIO()
        try:
            os.chdir(root)
            with mock.patch.object(sys, "argv", [
                "validate_manifest.py", "--repo-root", ".",
                "--manifest", str(manifest_path),
            ]), redirect_stdout(stdout), redirect_stderr(stderr):
                result = validate_manifest_mod.main()
        finally:
            os.chdir(original_cwd)

        self.assertNotEqual(result, 0)
        self.assertIn("--repo-root contains a dot or traversal segment", stderr.getvalue())

    def test_main_rejects_symlinked_current_directory_root_for_lifecycle_validation(self) -> None:
        """A physicalized POSIX getcwd must not erase a symlink-bearing `.` root."""
        root = self.repo / "cli-symlinked-current-directory-root"
        self._prepare_cli_evidence_root(root)
        manifest_path = root / "cli-manifest.json"
        alias = self.repo / "cli-symlinked-current-directory-alias"
        try:
            os.symlink(str(root), str(alias), target_is_directory=True)
        except (OSError, NotImplementedError) as exc:
            self.skipTest(f"cannot create a current-directory reparse fixture: {exc}")

        original_cwd = os.getcwd()
        stdout = io.StringIO()
        stderr = io.StringIO()
        try:
            os.chdir(alias)
            with mock.patch.object(sys, "argv", [
                "validate_manifest.py", "--repo-root", ".",
                "--manifest", str(manifest_path),
            ]), redirect_stdout(stdout), redirect_stderr(stderr):
                result = validate_manifest_mod.main()
        finally:
            os.chdir(original_cwd)

        self.assertNotEqual(result, 0)
        self.assertIn("--repo-root contains a dot or traversal segment", stderr.getvalue())

    def test_main_allows_current_directory_root_for_policy_only_validation(self) -> None:
        """Policy-only editing retains its established `--repo-root .` workflow."""
        root = self.repo / "cli-policy-current-directory-root"
        self._prepare_cli_evidence_root(root)
        manifest_path = root / "cli-manifest.json"
        original_cwd = os.getcwd()
        stdout = io.StringIO()
        stderr = io.StringIO()
        try:
            os.chdir(root)
            with mock.patch.object(sys, "argv", [
                "validate_manifest.py", "--repo-root", ".",
                "--manifest", str(manifest_path), "--policy-only",
            ]), redirect_stdout(stdout), redirect_stderr(stderr):
                result = validate_manifest_mod.main()
        finally:
            os.chdir(original_cwd)

        self.assertEqual(result, 0, stderr.getvalue())
        self.assertIn("POLICY-ONLY:", stdout.getvalue())

    def test_main_blocks_positive_evidence_before_legacy_root_consumers(self) -> None:
        """Positive proof stops before mutable Git/source pathname consumers."""
        root = self.repo / "cli-post-read-root"
        sha, _evidence_path, _fixture_manifest = self._prepare_cli_evidence_root(root)
        manifest_path = root / "cli-manifest.json"
        explicit_evidence = self.repo / "cli-post-read-explicit-evidence.json"
        explicit_evidence.write_text(
            (root / EVIDENCE_PRODUCERS["lifecycle-log"]["artifact"])
            .read_text(encoding="utf-8"),
            encoding="utf-8",
        )

        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(
                lifecycle_mod, "source_tree_sha",
                side_effect=AssertionError("legacy source-tree path was reached"),
        ) as source_tree, \
                mock.patch.object(sys, "argv", [
                    "validate_manifest.py", "--repo-root", str(root),
                    "--manifest", str(manifest_path),
                    "--lifecycle-evidence", str(explicit_evidence),
                    "--expected-sha", sha,
                ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        source_tree.assert_not_called()
        if os.name == "nt":
            self.assertNotEqual(result, 0)
            self.assertIn("POSIX rooted release authority", stderr.getvalue())
        else:
            self.assertEqual(result, 0, stderr.getvalue())
            self.assertIn("OK: module evidence manifest is valid", stdout.getvalue())

    def test_main_accepts_default_lifecycle_evidence_at_a_real_lexical_path(self) -> None:
        """Only the POSIX rooted release authority may emit a positive result."""
        root = self.repo / "cli-normal-root"
        sha, _evidence_path, _fixture_manifest = self._prepare_cli_evidence_root(root)
        manifest_path = root / "cli-manifest.json"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(root),
            "--manifest", str(manifest_path),
            "--expected-sha", sha,
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        if os.name == "nt":
            self.assertNotEqual(result, 0)
            self.assertIn("POSIX rooted release authority", stderr.getvalue())
        else:
            self.assertEqual(result, 0, stderr.getvalue())
            self.assertIn("OK: module evidence manifest is valid", stdout.getvalue())

    def test_main_reads_default_target_index_through_held_root_bytes(self) -> None:
        """The release default cannot regress to a mutable target-index pathname."""
        root = self.repo / "cli-rooted-target-root"
        sha, _evidence_path, _fixture_manifest = self._prepare_cli_evidence_root(root)
        manifest_path = root / "cli-manifest.json"
        original_reader = strict_json.NoFollowDirectoryLease.read_relative_bytes
        calls: list[str] = []

        def record_reader(
            lease: strict_json.NoFollowDirectoryLease, relative: str, *, max_bytes: int,
        ) -> bytes:
            calls.append(relative)
            return original_reader(lease, relative, max_bytes=max_bytes)

        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(
            strict_json.NoFollowDirectoryLease, "read_relative_bytes",
            autospec=True, side_effect=record_reader,
        ), mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(root),
            "--manifest", str(manifest_path),
            "--expected-sha", sha,
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        if os.name == "nt":
            self.assertNotEqual(result, 0)
            self.assertIn("POSIX rooted release authority", stderr.getvalue())
        else:
            self.assertEqual(result, 0, stderr.getvalue())
        self.assertIn("build/module-evidence/module-targets.json", calls)
        self.assertIn("build/module-evidence/module-lifecycle.json", calls)

    @unittest.skipIf(os.name == "nt", "POSIX rooted release-authority integration")
    def test_posix_main_reads_default_manifest_and_artifacts_through_root(self) -> None:
        """The release-shaped CLI consumes all default evidence below held root."""
        root = self.repo / "cli-rooted-defaults-root"
        sha, _evidence_path, _fixture_manifest = self._prepare_cli_evidence_root(root)
        policy_dir = root / "tools" / "module-evidence"
        policy_dir.mkdir(parents=True, exist_ok=True)
        (policy_dir / "manifest.json").write_text(json.dumps(base_manifest()), encoding="utf-8")
        (policy_dir / "evidence-gaps.json").write_text(
            json.dumps({"schemaVersion": "evidence-gaps-v1", "gaps": []}),
            encoding="utf-8",
        )
        junit = root / EVIDENCE_PRODUCERS["junit-xml"]["artifact"]
        junit.parent.mkdir(parents=True, exist_ok=True)
        junit.write_text(
            '<testsuites tests="3"><testsuite tests="3">'
            '<testcase name="a" classname="SparkGameFPS"/>'
            '<testcase name="b" classname="SparkGameFPS"/>'
            '<testcase name="c" classname="SparkGameFPS"/>'
            '</testsuite></testsuites>',
            encoding="utf-8",
        )
        smoke = root / EVIDENCE_PRODUCERS["package-smoke-log"]["artifact"]
        smoke.parent.mkdir(parents=True, exist_ok=True)
        smoke.write_text(
            "SparkGameFPS\nmodule=SparkGameFPS\nexit_code=0\nPASS\n",
            encoding="utf-8",
        )
        original_reader = strict_json.NoFollowDirectoryLease.read_relative_bytes
        calls: list[str] = []

        def record_reader(
            lease: strict_json.NoFollowDirectoryLease, relative: str, *, max_bytes: int,
        ) -> bytes:
            calls.append(relative)
            return original_reader(lease, relative, max_bytes=max_bytes)

        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(
            strict_json.NoFollowDirectoryLease, "read_relative_bytes",
            autospec=True, side_effect=record_reader,
        ), mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(root),
            "--allow-declared-gaps", "tools/module-evidence/evidence-gaps.json",
            "--expected-sha", sha,
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertEqual(result, 0, stderr.getvalue())
        self.assertIn("OK: module evidence manifest is valid", stdout.getvalue())
        self.assertTrue({
            "tools/module-evidence/manifest.json",
            "tools/module-evidence/evidence-gaps.json",
            "build/module-evidence/module-targets.json",
            "build/module-evidence/module-lifecycle.json",
            EVIDENCE_PRODUCERS["junit-xml"]["artifact"],
            EVIDENCE_PRODUCERS["package-smoke-log"]["artifact"],
        }.issubset(set(calls)), calls)

    @unittest.skipIf(os.name == "nt", "POSIX external revision-anchor regression")
    def test_posix_main_rejects_positive_lifecycle_without_external_sha_anchor(self) -> None:
        """A local Git-derived HEAD cannot authorize a release-attesting result."""
        root = self.repo / "cli-no-external-sha-root"
        self._prepare_cli_evidence_root(root)
        manifest_path = root / "cli-manifest.json"
        stdout = io.StringIO()
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [
            "validate_manifest.py", "--repo-root", str(root),
            "--manifest", str(manifest_path),
        ]), redirect_stdout(stdout), redirect_stderr(stderr):
            result = validate_manifest_mod.main()

        self.assertNotEqual(result, 0)
        self.assertIn("externally injected --expected-sha", stderr.getvalue())

    def test_document_shape_is_rejected_by_loader_and_injected_validator(self) -> None:
        """Direct injection must not bypass the loader's closed document schema."""
        cases: list[tuple[str, Any]] = []

        duplicate = lifecycle_evidence(self.repo, self.sha)
        duplicate["records"].append(copy.deepcopy(duplicate["records"][0]))
        cases.append(("duplicate record", duplicate))

        extra = lifecycle_evidence(self.repo, self.sha)
        extra_record = copy.deepcopy(extra["records"][0])
        extra_record["module"] = DECOY
        extra["records"].append(extra_record)
        cases.append(("extra record", extra))

        non_object = lifecycle_evidence(self.repo, self.sha)
        non_object["records"].append("not a lifecycle record")
        cases.append(("non-object record", non_object))

        unknown_top_level = lifecycle_evidence(self.repo, self.sha)
        unknown_top_level["unreviewed"] = True
        cases.append(("unknown top-level key", unknown_top_level))

        for case, document in cases:
            with self.subTest(case=case):
                path = self.repo / "lifecycle-shape.json"
                path.write_text(json.dumps(document), encoding="utf-8")
                with self.assertRaises(lifecycle_mod.LifecycleEvidenceUnavailable):
                    lifecycle_mod.load_lifecycle_evidence(path)
                errors = self.assertRejected(
                    base_manifest(), f"B10-shape-{case}",
                    lifecycle_evidence=document,
                )
                self.assertTrue(errors, errors)

    def test_source_directory_must_exactly_match_the_planned_collector_source(self) -> None:
        """A same-tree alias cannot stand in for the collector's fixed source path."""
        for source_directory in (
            "GameModules/SparkGameFPS/Source/.",
            "GameModules/SparkGameFPS/source",
            "GameModules/SparkGameFPS/Source/./nested",
        ):
            with self.subTest(source_directory=source_directory):
                evidence = lifecycle_evidence(self.repo, self.sha)
                evidence["records"][0]["sourceDirectory"] = source_directory
                errors = self.assertRejected(
                    base_manifest(), "B10-source-directory",
                    lifecycle_evidence=evidence,
                )
                self.assertTrue(any("sourceDirectory" in error for error in errors),
                                errors)

    def test_only_the_windows_headless_collector_runner_and_dll_are_accepted(self) -> None:
        """CI test runners and Linux/manual DLL names are not release runtime proof."""
        for field, value in (
            ("runner", "ctest"),
            ("runner", "spark-automation"),
            ("sharedLibrary", "libSparkGameFPS.so"),
            ("sharedLibrary", "SparkGameFPS-Manual.dll"),
            ("sharedLibrary", "SparkGameFPS.DLL"),
        ):
            with self.subTest(field=field, value=value):
                evidence = lifecycle_evidence(self.repo, self.sha)
                evidence["records"][0][field] = value
                errors = self.assertRejected(
                    base_manifest(), "B10-windows-headless-contract",
                    lifecycle_evidence=evidence,
                )
                self.assertTrue(any(field in error for error in errors), errors)

    def test_paths_must_be_exact_windows_collector_final_paths(self) -> None:
        """Host paths and Windows aliases must not be accepted as collector output."""
        cases = (
            ("enginePath", "/usr/local/bin/SparkEngine.exe"),
            ("enginePath", "SparkEngine.exe"),
            ("enginePath", r"C:\verified-artifact\NotSparkEngine.exe"),
            ("enginePath", r"C:\verified-artifact\SparkEngine.exe:payload"),
            ("enginePath", r"C:\verified-artifact\SparkEngine.exe "),
            ("enginePath", r"C:\verified-artifact\sparkengine.exe"),
            ("enginePath", r"C:\verified-artifact\.\SparkEngine.exe"),
            ("modulePath", "/usr/local/lib/SparkGameFPS.dll"),
            ("modulePath", "SparkGameFPS.dll"),
            ("modulePath", r"C:\verified-artifact\NotSparkGameFPS.dll"),
            ("modulePath", r"C:\verified-artifact\SparkGameFPS.dll:payload"),
            ("modulePath", r"C:\verified-artifact\SparkGameFPS.dll "),
            ("modulePath", r"C:\verified-artifact\sparkgamefps.dll"),
            ("modulePath", r"C:\verified-artifact\.\SparkGameFPS.dll"),
        )
        for field, value in cases:
            with self.subTest(field=field, value=value):
                evidence = lifecycle_evidence(self.repo, self.sha)
                evidence["records"][0][field] = value
                errors = self.assertRejected(
                    base_manifest(), "B10-final-path",
                    lifecycle_evidence=evidence,
                )
                self.assertTrue(any(field in error for error in errors), errors)

    def test_extended_windows_final_paths_from_a_collector_are_accepted(self) -> None:
        """The extended namespace spelling is a valid GetFinalPathNameW result."""
        evidence = lifecycle_evidence(self.repo, self.sha)
        evidence["records"][0]["enginePath"] = (
            r"\\?\C:\verified-artifact\SparkEngine.exe"
        )
        evidence["records"][0]["modulePath"] = (
            r"\\?\C:\verified-artifact\SparkGameFPS.dll"
        )
        self.assertAccepted(base_manifest(), lifecycle_evidence=evidence)

    def test_fixed_and_render_phases_must_be_positive(self) -> None:
        """A loaded module that never fixed-steps or renders is not stable-v1 proof."""
        for phase in ("OnFixedUpdate", "OnRender"):
            for count in (None, 0):
                with self.subTest(phase=phase, count=count):
                    evidence = lifecycle_evidence(self.repo, self.sha)
                    if count is None:
                        del evidence["records"][0]["phases"][phase]
                    else:
                        evidence["records"][0]["phases"][phase] = count
                    errors = self.assertRejected(
                        base_manifest(), "B10-fixed-render",
                        lifecycle_evidence=evidence,
                    )
                    self.assertTrue(any(phase in error for error in errors), errors)

    def test_source_tree_sha_changes_when_source_changes(self) -> None:
        """The binding must actually discriminate."""
        first, err = lifecycle_mod.source_tree_sha(
            self.repo, self.sha, f"GameModules/{INCLUDED}/Source")
        self.assertIsNone(err)
        (self.repo / "GameModules" / INCLUDED / "Source" / "Extra.cpp").write_text(
            "// changed\n", encoding="utf-8")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", "change source")
        new_sha = _git(self.repo, "rev-parse", "HEAD").stdout.strip()
        second, err = lifecycle_mod.source_tree_sha(
            self.repo, new_sha, f"GameModules/{INCLUDED}/Source")
        self.assertIsNone(err)
        self.assertNotEqual(first, second)
        # Restore so later tests in this class see the original tree.
        _git(self.repo, "reset", "-q", "--hard", self.sha)


# --------------------------------------------------------------------------
# Engine binary authenticity — arbitrary marker printers are not engines
# --------------------------------------------------------------------------
class TestLifecycleAuthenticity(FixtureCase):

    def test_script_cmd_rejected_as_engine(self) -> None:
        from collect_lifecycle import validate_engine_binary
        script = self.repo / "fake.cmd"
        script.write_text("@echo off\necho hello\n", encoding="utf-8")
        err = validate_engine_binary(script)
        self.assertIsNotNone(err, "a .cmd script was accepted as an engine")
        self.assertIn("script", err)

    def test_script_bat_rejected_as_engine(self) -> None:
        from collect_lifecycle import validate_engine_binary
        script = self.repo / "fake.bat"
        script.write_text("@echo off\n", encoding="utf-8")
        err = validate_engine_binary(script)
        self.assertIsNotNone(err)

    def test_script_sh_rejected_as_engine(self) -> None:
        from collect_lifecycle import validate_engine_binary
        script = self.repo / "fake.sh"
        script.write_text("#!/bin/sh\necho hello\n", encoding="utf-8")
        err = validate_engine_binary(script)
        self.assertIsNotNone(err)

    def test_wrong_name_rejected_as_engine(self) -> None:
        from collect_lifecycle import validate_engine_binary
        binary = self.repo / "NotAnEngine.exe"
        binary.write_bytes(b"\x00" * 8192)
        err = validate_engine_binary(binary)
        self.assertIsNotNone(err, "a binary with wrong name was accepted")
        self.assertIn("stable-v1", err)

    def test_tiny_file_rejected_as_engine(self) -> None:
        from collect_lifecycle import validate_engine_binary
        binary = self.repo / "SparkEngine.exe"
        binary.write_bytes(b"\x00" * 100)
        err = validate_engine_binary(binary)
        self.assertIsNotNone(err, "a tiny binary was accepted as an engine")
        self.assertIn("bytes", err)

    def test_script_content_in_exe_rejected(self) -> None:
        from collect_lifecycle import validate_engine_binary
        binary = self.repo / "SparkEngine.exe"
        binary.write_bytes(b"@echo off\r\n" + b"\x00" * 8192)
        err = validate_engine_binary(binary)
        self.assertIsNotNone(err, "a script disguised as .exe was accepted")
        self.assertIn("script signature", err)

    def test_shebang_content_in_binary_rejected(self) -> None:
        from collect_lifecycle import validate_engine_binary
        binary = self.repo / "SparkEngine"
        binary.write_bytes(b"#!/bin/sh\necho lifecycle\n" + b"\x00" * 8192)
        err = validate_engine_binary(binary)
        self.assertIsNotNone(err)

    def test_symlink_engine_rejected(self) -> None:
        from collect_lifecycle import validate_engine_binary
        real = self.repo / "RealEngine"
        real.write_bytes(b"\x00" * 8192)
        link = self.repo / "SparkEngine"
        made = False
        try:
            os.symlink(str(real), str(link))
            made = True
        except (OSError, NotImplementedError):
            pass
        if not made:
            self.skipTest("cannot create symlinks on this platform")
        err = validate_engine_binary(link)
        self.assertIsNotNone(err, "a symlinked engine was accepted")
        self.assertIn("reparse point", err.lower() if "reparse" in err.lower() else err)

    def test_nonexistent_engine_rejected(self) -> None:
        from collect_lifecycle import validate_engine_binary
        err = validate_engine_binary(self.repo / "no-such-binary")
        self.assertIsNotNone(err)

    def test_plausible_engine_binary_accepted(self) -> None:
        from collect_lifecycle import validate_engine_binary
        binary = self.repo / "SparkEngine.exe"
        binary.write_bytes(pe_image())
        err = validate_engine_binary(binary)
        self.assertIsNone(err, f"valid engine binary rejected: {err}")

    def test_missing_engine_sha256_in_record_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        del ev["records"][0]["engineSHA256"]
        errors = self.assertRejected(base_manifest(), "AUTH1",
                                     lifecycle_evidence=ev)
        self.assertTrue(any("engineSHA256" in e or "missing" in e for e in errors))

    def test_missing_engine_path_in_record_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        del ev["records"][0]["enginePath"]
        errors = self.assertRejected(base_manifest(), "AUTH2",
                                     lifecycle_evidence=ev)
        self.assertTrue(any("enginePath" in e or "missing" in e for e in errors))

    def test_invalid_engine_sha256_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["engineSHA256"] = "not-a-hash"
        errors = self.assertRejected(base_manifest(), "AUTH3",
                                     lifecycle_evidence=ev)
        self.assertTrue(any("engineSHA256" in e for e in errors))

    def test_empty_engine_path_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["enginePath"] = ""
        errors = self.assertRejected(base_manifest(), "AUTH4",
                                     lifecycle_evidence=ev)
        self.assertTrue(any("enginePath" in e for e in errors))

# --------------------------------------------------------------------------
# Collector contract — one host-owned terminal record, no trace reconstruction
# --------------------------------------------------------------------------
class TestLifecycleCollector(FixtureCase):

    VALID_RECORD = (
        "SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=1 load=1 "
        "update=4 fixed=2 render=4 unload=1 destroy=1 faults=0"
    )

    def _collector_main_fixture(self, name: str) -> tuple[Path, list[str]]:
        """Build a truthful image pair for main() transaction tests."""
        root = self.repo / name
        root.mkdir()
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        # The release collector has one non-negotiable publication namespace.
        # Fixtures deliberately use that same path so transaction tests cannot
        # quietly exercise an arbitrary caller-controlled output directory.
        out = self.repo / "build" / "module-evidence" / "module-lifecycle.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        for artifact in (out, log):
            if artifact.exists():
                artifact.unlink()
        image_manifest = write_image_manifest(root, engine, module, self.sha)
        return out, [
            "collect_lifecycle.py", "--engine", str(engine), "--module", INCLUDED,
            "--module-image", str(module), "--working-directory", str(root),
            "--rhi-backend", "d3d11", "--image-manifest", str(image_manifest),
            "--out", str(out), "--commit-sha", self.sha,
        ]

    def _run_truthful_collector_main(
        self, argv: list[str], *extra_patches: object,
        output_operations: object | None = None,
        use_production_output_operations: bool = False,
    ) -> int:
        """Run main's real publication path while isolating the engine process."""
        import collect_lifecycle

        engine = Path(argv[argv.index("--engine") + 1])
        module = Path(argv[argv.index("--module-image") + 1])
        image_manifest = Path(argv[argv.index("--image-manifest") + 1])
        out = Path(argv[argv.index("--out") + 1])
        engine_digest = hashlib.sha256(engine.read_bytes()).hexdigest()
        module_digest = hashlib.sha256(module.read_bytes()).hexdigest()
        engine_image = collect_lifecycle.ImageVerification(
            digest=engine_digest,
            final_path=str(engine),
            identity=collect_lifecycle.ImageIdentity(1, 1, engine.stat().st_size, 1),
        )
        module_image = collect_lifecycle.ImageVerification(
            digest=module_digest,
            final_path=str(module),
            identity=collect_lifecycle.ImageIdentity(1, 2, module.stat().st_size, 1),
        )
        manifest_bytes = image_manifest.read_bytes()
        def manifest_lease_factory(path: Path) -> FakeImageLease:
            self.assertEqual(path, image_manifest)
            return FakeImageLease(
                path,
                identity=FakeLeaseIdentity(1, 3, len(manifest_bytes)),
                digest=hashlib.sha256(manifest_bytes).hexdigest(),
                image=manifest_bytes,
            )

        if output_operations is None and not use_production_output_operations:
            output_operations = self._fake_output_publication_operations(out)

        with ExitStack() as stack:
            stack.enter_context(mock.patch.object(sys, "argv", argv))
            stack.enter_context(mock.patch(
                "collect_lifecycle._stable_v1_windows_platform", return_value=True,
            ))
            stack.enter_context(mock.patch("collect_lifecycle.REPO_ROOT", self.repo))
            stack.enter_context(mock.patch(
                "collect_lifecycle.resolve_head_sha", return_value=(self.sha, None),
            ))
            stack.enter_context(mock.patch.dict(
                "collect_lifecycle.os.environ", {"GITHUB_SHA": self.sha}, clear=False,
            ))
            stack.enter_context(mock.patch(
                "collect_lifecycle.lifecycle_mod.source_tree_sha",
                return_value=("c" * 40, None),
            ))
            stack.enter_context(mock.patch(
                "collect_lifecycle.run_engine",
                return_value=(
                    collect_lifecycle.EngineOutput(
                        self.VALID_RECORD, "", engine_image, module_image,
                    ),
                    None,
                ),
            ))
            stack.enter_context(mock.patch(
                "collect_lifecycle.open_image_lease",
                side_effect=manifest_lease_factory,
            ))
            if output_operations is not None:
                stack.enter_context(mock.patch(
                    "collect_lifecycle._output_publication_operations_for_main",
                    return_value=output_operations,
                ))
            for patch in extra_patches:
                stack.enter_context(patch)
            return collect_lifecycle.main()

    def _assert_no_transaction_artifacts(self, out: Path) -> None:
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        self.assertFalse(out.exists(), f"partial JSON publication survived at {out}")
        self.assertFalse(log.exists(), f"partial log publication survived at {log}")
        for final_path in (out, log):
            self.assertEqual(
                list(final_path.parent.glob(f".{final_path.name}.*.tmp")), [],
                f"temporary publication sibling survived for {final_path.name}",
            )

    def _fake_output_namespace_factory(
        self, out: Path, *, output_attributes: int | None = None,
        output_final_path: str | None = None, identity_base: int = 1100,
    ) -> tuple[list[FakeOutputDirectoryLease], Any]:
        """Return ordered fake root/build/output directory leases for main()."""
        import collect_lifecycle

        paths = (self.repo, self.repo / "build", out.parent)
        leases = [
            FakeOutputDirectoryLease(
                path,
                identity=FakeLeaseIdentity(39, identity_base + ordinal, 0),
                attributes=(
                    output_attributes if ordinal == len(paths) - 1 and output_attributes is not None
                    else collect_lifecycle._FILE_ATTRIBUTE_DIRECTORY
                ),
                final_path=(
                    output_final_path if ordinal == len(paths) - 1 and output_final_path is not None
                    else str(path)
                ),
            )
            for ordinal, path in enumerate(paths)
        ]
        opened: list[FakeOutputDirectoryLease] = []

        def factory(path: Path) -> FakeOutputDirectoryLease:
            expected = paths[len(opened)]
            self.assertEqual(path, expected)
            lease = leases[len(opened)]
            opened.append(lease)
            return lease

        return leases, factory

    def _fake_output_publication_operations(
        self, out: Path, *,
        lease_factory: Any | None = None,
    ) -> object:
        """Build an explicit test-only replacement for native output authority."""
        import collect_lifecycle

        if lease_factory is None:
            _leases, lease_factory = self._fake_output_namespace_factory(out)

        def validate_directory(directory: object) -> object:
            if not isinstance(directory, FakeOutputDirectoryLease) or directory.closed:
                raise OSError("test output authority is not a live fake directory lease")
            return directory

        def clear(directory: object, *paths: Path) -> str | None:
            validate_directory(directory)
            for path in paths:
                path.unlink(missing_ok=True)
            return None

        def write(directory: object, final_path: Path, content: str) -> FakeOutputArtifact:
            validate_directory(directory)
            if final_path.parent != out.parent or final_path.name not in {
                collect_lifecycle.LIFECYCLE_OUTPUT_FILENAME,
                collect_lifecycle.LIFECYCLE_AUDIT_LOG_FILENAME,
            }:
                raise OSError("test output authority rejected a non-fixed output leaf")
            final_path.write_text(content, encoding="utf-8")
            return FakeOutputArtifact(final_path)

        def discard(artifacts: list[FakeOutputArtifact]) -> str | None:
            errors: list[str] = []
            for artifact in reversed(artifacts):
                try:
                    artifact.discard()
                except OSError as exc:
                    errors.append(str(exc))
            return "; ".join(errors) if errors else None

        def close_in_order(artifacts: list[FakeOutputArtifact]) -> str | None:
            for artifact in artifacts:
                try:
                    artifact.close()
                except OSError as exc:
                    return str(exc)
            return None

        def finalize_reserves(artifacts: list[FakeOutputArtifact]) -> str | None:
            """Tests release fake reserves synchronously; production cannot."""
            return close_in_order(artifacts)

        return collect_lifecycle.OutputPublicationOperations(
            lease_factory=lease_factory,
            open_namespace=lambda root, directory: (
                collect_lifecycle._open_output_namespace_leases(
                    root, directory, lease_factory=lease_factory,
                )
            ),
            validate_directory=validate_directory,
            clear=clear,
            write=write,
            discard=discard,
            close_in_order=close_in_order,
            finalize_reserves=finalize_reserves,
        )

    def test_main_allows_explicit_fake_output_operations_without_native_bindings(self) -> None:
        """Only a supplied test provider may replace unavailable native output authority."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("explicit-fake-output-operations")
        operations = self._fake_output_publication_operations(out)
        with mock.patch(
            "collect_lifecycle.WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE", False,
        ), mock.patch(
            "collect_lifecycle.open_output_directory_lease",
            side_effect=AssertionError("production native output factory was invoked"),
        ):
            self.assertEqual(self._run_truthful_collector_main(
                argv, output_operations=operations,
            ), 0)
        self.assertTrue(out.is_file())
        self.assertTrue((out.parent / "module-lifecycle-SparkGameFPS.log").is_file())

    def test_main_rejects_unavailable_native_output_operations_without_test_provider(self) -> None:
        """The production provider must fail closed when native output bindings are absent."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("unavailable-native-output-operations")
        with mock.patch(
            "collect_lifecycle.WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE", False,
        ), mock.patch("collect_lifecycle.run_engine") as child:
            self.assertEqual(self._run_truthful_collector_main(
                argv, use_production_output_operations=True,
            ), 1)
        child.assert_not_called()
        self._assert_no_transaction_artifacts(out)

    def test_main_removes_both_leaves_when_reserve_handoff_fails(self) -> None:
        """A pre-handoff reserve failure retains both exact leaves for rollback."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("late-retained-backup-close")
        base = self._fake_output_publication_operations(out)
        close_calls = 0

        def fail_reserve_handoff(artifacts: list[FakeOutputArtifact]) -> str | None:
            nonlocal close_calls
            close_calls += 1
            self.assertTrue(all(not artifact.closed for artifact in artifacts))
            return "injected reserve handoff failure"

        operations = collect_lifecycle.OutputPublicationOperations(
            lease_factory=base.lease_factory,
            open_namespace=base.open_namespace,
            validate_directory=base.validate_directory,
            clear=base.clear,
            write=base.write,
            discard=base.discard,
            close_in_order=base.close_in_order,
            finalize_reserves=fail_reserve_handoff,
        )
        with mock.patch(
            "collect_lifecycle.WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE", False,
        ):
            self.assertEqual(self._run_truthful_collector_main(
                argv, output_operations=operations,
            ), 1)
        self.assertEqual(close_calls, 1)
        self._assert_no_transaction_artifacts(out)

    def test_main_removes_both_leaves_when_primary_close_fails_after_release(self) -> None:
        """Backup guards roll back both leaves after post-release primary close failure."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("primary-close-after-release")
        leases, factory = self._fake_output_namespace_factory(out)
        base = self._fake_output_publication_operations(out, lease_factory=factory)
        close_calls = 0

        def fail_primary_close(artifacts: list[FakeOutputArtifact]) -> str | None:
            nonlocal close_calls
            close_calls += 1
            self.assertTrue(all(lease.closed for lease in leases))
            self.assertTrue(all(not artifact.closed for artifact in artifacts))
            return "injected post-release primary close failure"

        operations = replace(base, close_in_order=fail_primary_close)
        self.assertEqual(self._run_truthful_collector_main(
            argv, output_operations=operations,
        ), 1)
        self.assertEqual(close_calls, 1)
        self.assertTrue(all(lease.closed for lease in leases))
        self._assert_no_transaction_artifacts(out)

    def test_main_cleans_all_transaction_artifacts_after_transient_stale_unlink_failure(
        self,
    ) -> None:
        """A stale-clear failure must still run the native-handle cleanup transaction."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("stale-unlink")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        out.write_text("stale-json", encoding="utf-8")
        log.write_text("stale-log", encoding="utf-8")
        base = self._fake_output_publication_operations(out)
        real_clear = base.clear
        failed_once = False

        def fail_only_the_initial_clear(directory, *paths):
            nonlocal failed_once
            if not failed_once:
                failed_once = True
                return "injected stale JSON handle-delete failure"
            return real_clear(directory, *paths)

        operations = replace(base, clear=fail_only_the_initial_clear)
        self.assertEqual(self._run_truthful_collector_main(
            argv, output_operations=operations,
        ), 1)
        self.assertTrue(failed_once, "the stale-clear failure injection was not exercised")
        self._assert_no_transaction_artifacts(out)

    def test_main_cleans_all_transaction_artifacts_after_each_publication_failure(
        self,
    ) -> None:
        """A status or either direct-final write failure cannot leave a partial pair."""
        import collect_lifecycle

        real_print = print

        def fail_final_status(*args, **kwargs):
            if args and str(args[0]).startswith("OK: wrote "):
                raise OSError("injected final status failure")
            return real_print(*args, **kwargs)

        out, argv = self._collector_main_fixture("publish-status")
        self.assertEqual(self._run_truthful_collector_main(
            argv, mock.patch("builtins.print", side_effect=fail_final_status),
        ), 1)
        self._assert_no_transaction_artifacts(out)

        for ordinal in (1, 2):
            with self.subTest(failure=f"direct-write-{ordinal}"):
                out, argv = self._collector_main_fixture(f"publish-direct-write-{ordinal}")
                base = self._fake_output_publication_operations(out)
                real_write_output = base.write

                def fail_write_output(directory, final_path, content):
                    fail_write_output.call_count += 1
                    if fail_write_output.call_count == ordinal:
                        raise OSError(f"injected direct output write {ordinal} failure")
                    return real_write_output(directory, final_path, content)

                fail_write_output.call_count = 0
                self.assertEqual(self._run_truthful_collector_main(
                    argv, output_operations=replace(base, write=fail_write_output),
                ), 1)
                self.assertEqual(fail_write_output.call_count, ordinal)
                self._assert_no_transaction_artifacts(out)

    def test_main_cleans_all_transaction_artifacts_after_direct_writer_failures(
        self,
    ) -> None:
        """Write, flush, or writer-close failures cannot leave a direct final leaf."""

        for phase in ("write", "flush", "close"):
            with self.subTest(phase=phase):
                out, argv = self._collector_main_fixture(f"temp-{phase}")
                base = self._fake_output_publication_operations(out)
                write_calls = 0

                def fail_at_direct_writer_boundary(directory, final_path, content):
                    nonlocal write_calls
                    write_calls += 1
                    if phase == "write":
                        raise OSError("injected direct writer write failure")
                    artifact = base.write(directory, final_path, content)
                    if phase == "flush":
                        raise OSError("injected direct writer flush failure")
                    raise OSError("injected direct writer close failure")

                self.assertEqual(self._run_truthful_collector_main(
                    argv, output_operations=replace(
                        base, write=fail_at_direct_writer_boundary,
                    ),
                ), 1)
                self.assertEqual(write_calls, 1)
                self._assert_no_transaction_artifacts(out)

    def test_write_temp_closes_descriptor_when_fdopen_never_takes_ownership(self) -> None:
        """A failed fdopen construction cannot leak the mkstemp descriptor."""
        import collect_lifecycle

        root = self.repo / "fdopen-construction-failure"
        root.mkdir()
        final_path = root / "module-lifecycle.json"
        tracked: set[Path] = set()
        descriptors: list[int] = []

        def fail_before_ownership(descriptor: int, *args, **kwargs):
            descriptors.append(descriptor)
            raise OSError("injected fdopen construction failure")

        with mock.patch("collect_lifecycle.os.fdopen", side_effect=fail_before_ownership):
            with self.assertRaisesRegex(OSError, "fdopen construction failure"):
                collect_lifecycle._write_temp(final_path, "payload", tracked_temps=tracked)

        self.assertEqual(len(descriptors), 1)
        with self.assertRaises(OSError):
            os.fstat(descriptors[0])
        self.assertFalse(final_path.exists())
        self.assertEqual(tracked, set())
        self.assertEqual(list(root.glob(".module-lifecycle.json.*.tmp")), [])

    def test_engine_output_audit_log_always_delimits_stdout_and_stderr(self) -> None:
        """The audit trail remains line-oriented when stdout lacks a newline."""
        from collect_lifecycle import EngineOutput

        self.assertEqual(
            EngineOutput("stdout-without-newline", "stderr").audit_log,
            "--- stdout ---\nstdout-without-newline\n--- stderr ---\nstderr",
        )

    def test_main_rejects_unsafe_output_and_module_inputs_before_any_clear(self) -> None:
        """No caller-controlled alias or collision can select a deletion target."""
        import collect_lifecycle

        cases = (
            ("module-traversal", "--module", r"\..\..\victim"),
            ("output-is-log", "--out", "log"),
            ("output-is-engine", "--out", "engine"),
            ("output-is-module", "--out", "module"),
            ("output-is-manifest", "--out", "manifest"),
            ("engine-trailing-dot", "--engine", "engine-trailing-dot"),
            ("module-image-ads", "--module-image", "module-image-ads"),
            ("output-dot-alias", "--out", "dot-alias"),
            ("output-trailing-dot", "--out", "trailing-dot"),
            ("output-trailing-space", "--out", "trailing-space"),
            ("output-ads", "--out", "ads"),
        )
        for name, option, replacement in cases:
            with self.subTest(case=name):
                out, argv = self._collector_main_fixture(f"namespace-{name}")
                log = out.parent / "module-lifecycle-SparkGameFPS.log"
                engine = Path(argv[argv.index("--engine") + 1])
                module = Path(argv[argv.index("--module-image") + 1])
                manifest = Path(argv[argv.index("--image-manifest") + 1])
                out.write_text("protected-json", encoding="utf-8")
                log.write_text("protected-log", encoding="utf-8")
                values = {
                    "log": str(log),
                    "engine": str(engine),
                    "module": str(module),
                    "manifest": str(manifest),
                    "engine-trailing-dot": str(engine) + ".",
                    "module-image-ads": str(module) + ":alternate",
                    "dot-alias": str(self.repo / "build") +
                    r"\.\module-evidence\module-lifecycle.json",
                    "trailing-dot": str(out) + ".",
                    "trailing-space": str(out) + " ",
                    "ads": str(out) + ":audit",
                }
                argv = list(argv)
                argv[argv.index(option) + 1] = values.get(replacement, replacement)
                protected = {
                    engine: engine.read_bytes(),
                    module: module.read_bytes(),
                    manifest: manifest.read_bytes(),
                }
                with mock.patch.object(sys, "argv", argv), \
                     mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
                     mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
                     mock.patch("collect_lifecycle.resolve_head_sha",
                                return_value=(self.sha, None)), \
                     mock.patch("collect_lifecycle._clear_output_artifacts",
                                wraps=collect_lifecycle._clear_output_artifacts) as clear, \
                     mock.patch("collect_lifecycle.run_engine") as child:
                    self.assertEqual(collect_lifecycle.main(), 1)
                clear.assert_not_called()
                child.assert_not_called()
                self.assertEqual(out.read_text(encoding="utf-8"), "protected-json")
                self.assertEqual(log.read_text(encoding="utf-8"), "protected-log")
                for path, content in protected.items():
                    self.assertEqual(path.read_bytes(), content, path)

    def test_main_rejects_reparse_output_namespace_before_any_clear(self) -> None:
        """Every fixed output ancestor is checked before stale evidence is touched."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("namespace-reparse")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        out.write_text("protected-json", encoding="utf-8")
        log.write_text("protected-log", encoding="utf-8")

        leases, factory = self._fake_output_namespace_factory(
            out,
            output_attributes=(collect_lifecycle._FILE_ATTRIBUTE_DIRECTORY |
                               collect_lifecycle._FILE_ATTRIBUTE_REPARSE_POINT),
        )
        base = self._fake_output_publication_operations(out, lease_factory=factory)
        clear = mock.Mock(wraps=base.clear)
        operations = replace(base, clear=clear)
        child = mock.patch("collect_lifecycle.run_engine")
        self.assertEqual(self._run_truthful_collector_main(
            argv, child, output_operations=operations,
        ), 1)

        clear.assert_not_called()
        self.assertTrue(all(lease.closed for lease in leases))
        self.assertEqual(out.read_text(encoding="utf-8"), "protected-json")
        self.assertEqual(log.read_text(encoding="utf-8"), "protected-log")

    def test_main_rejects_untrusted_output_directory_leases_before_any_clear(self) -> None:
        """A path alias, reparse point, or non-directory handle cannot authorize cleanup."""
        import collect_lifecycle

        cases = (
            ("path-alias", str(self.repo / "other-evidence-directory"),
             collect_lifecycle._FILE_ATTRIBUTE_DIRECTORY),
            ("reparse", None, collect_lifecycle._FILE_ATTRIBUTE_DIRECTORY |
             collect_lifecycle._FILE_ATTRIBUTE_REPARSE_POINT),
            ("not-directory", None, 0),
        )
        (self.repo / "other-evidence-directory").mkdir(exist_ok=True)
        for name, final_path, attributes in cases:
            with self.subTest(case=name):
                out, argv = self._collector_main_fixture(f"output-lease-{name}")
                log = out.parent / "module-lifecycle-SparkGameFPS.log"
                out.write_text("protected-json", encoding="utf-8")
                log.write_text("protected-log", encoding="utf-8")
                leases, factory = self._fake_output_namespace_factory(
                    out,
                    output_attributes=attributes,
                    output_final_path=final_path,
                    identity_base=1000,
                )
                base = self._fake_output_publication_operations(
                    out, lease_factory=factory,
                )
                clear = mock.Mock(wraps=base.clear)
                self.assertEqual(self._run_truthful_collector_main(
                    argv, output_operations=replace(base, clear=clear),
                ), 1)
                clear.assert_not_called()
                self.assertTrue(all(lease.closed for lease in leases))
                self.assertEqual(out.read_text(encoding="utf-8"), "protected-json")
                self.assertEqual(log.read_text(encoding="utf-8"), "protected-log")

    def test_main_holds_output_directory_lease_through_cleanup_and_publication(self) -> None:
        """Every destructive/output operation runs while the fixed directory handle is live."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("output-lease-lifetime")
        leases, factory = self._fake_output_namespace_factory(out)
        events: list[str] = []
        written_paths: list[Path] = []
        base = self._fake_output_publication_operations(out, lease_factory=factory)
        real_clear = base.clear
        real_write = base.write

        def observe_clear(directory, *paths: Path) -> str | None:
            self.assertTrue(all(not lease.closed for lease in leases),
                            "namespace lease closed before cleanup")
            events.append("clear")
            return real_clear(directory, *paths)

        def observe_write(directory, final_path: Path, content: str):
            self.assertTrue(all(not lease.closed for lease in leases),
                            "namespace lease closed before direct-final write")
            if final_path == out:
                self.assertTrue(
                    (out.parent / "module-lifecycle-SparkGameFPS.log").exists(),
                    "authoritative JSON creation began before the audit leaf existed",
                )
            events.append("write")
            written_paths.append(final_path)
            return real_write(directory, final_path, content)

        def observe_primary_close(artifacts: list[FakeOutputArtifact]) -> str | None:
            self.assertTrue(all(lease.closed for lease in leases),
                            "a final leaf guard closed before namespace release")
            events.append("close-primary")
            return base.close_in_order(artifacts)

        def observe_reserve_finalize(artifacts: list[FakeOutputArtifact]) -> str | None:
            self.assertTrue(all(lease.closed for lease in leases),
                            "a reserve finalized before namespace release")
            events.append("finalize-reserves")
            return base.finalize_reserves(artifacts)

        self.assertEqual(self._run_truthful_collector_main(
            argv,
            output_operations=replace(
                base, clear=observe_clear, write=observe_write,
                close_in_order=observe_primary_close,
                finalize_reserves=observe_reserve_finalize,
            ),
        ), 0)
        self.assertEqual(
            events,
            ["clear", "write", "write", "close-primary", "finalize-reserves"],
        )
        self.assertEqual(
            written_paths,
            [out.parent / "module-lifecycle-SparkGameFPS.log", out],
        )
        self.assertEqual(len(leases), 3)
        self.assertTrue(all(lease.closed for lease in leases))

    def test_main_removes_pair_when_output_anchor_close_fails(self) -> None:
        """A post-publication namespace-close error leaves no authoritative JSON."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("output-anchor-close-failure")
        leases, factory = self._fake_output_namespace_factory(out)
        close_failures = 0
        clear_calls = 0
        base = self._fake_output_publication_operations(out, lease_factory=factory)
        real_clear = base.clear
        original_close = leases[-1].close

        def fail_once() -> None:
            nonlocal close_failures
            if close_failures == 0:
                close_failures += 1
                raise OSError("injected output-anchor close failure")
            original_close()

        leases[-1].close = fail_once  # type: ignore[method-assign]

        def observe_clear(directory, *paths: Path) -> str | None:
            nonlocal clear_calls
            clear_calls += 1
            return real_clear(directory, *paths)

        self.assertEqual(self._run_truthful_collector_main(
            argv, output_operations=replace(base, clear=observe_clear),
        ), 1)

        self.assertEqual(close_failures, 1)
        self.assertEqual(
            clear_calls, 2,
            "the early stale clear and rooted rollback must both retain output authority",
        )
        self.assertTrue(all(lease.closed for lease in leases))
        self._assert_no_transaction_artifacts(out)

    def test_main_removes_pair_after_final_handle_validation_failure(self) -> None:
        """A final identity/path verification failure cannot leave JSON accepted."""
        out, argv = self._collector_main_fixture("output-final-validation-failure")
        real_verify = FakeOutputArtifact.verify
        json_verifications = 0

        def reject_second_json_verify(artifact, final_path: Path) -> None:
            nonlocal json_verifications
            if final_path == out:
                json_verifications += 1
                if json_verifications == 2:
                    raise OSError("injected post-publication JSON handle validation failure")
            real_verify(artifact, final_path)

        with mock.patch.object(
            FakeOutputArtifact, "verify",
            autospec=True,
            side_effect=reject_second_json_verify,
        ):
            self.assertEqual(self._run_truthful_collector_main(argv), 1)

        self.assertEqual(json_verifications, 2)
        self._assert_no_transaction_artifacts(out)

    def test_manifest_lease_reads_held_bytes_not_replaced_path(self) -> None:
        """A manifest lease, not a later pathname read, supplies authority."""
        import collect_lifecycle

        root = self.repo / "manifest-held-bytes"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        manifest = write_image_manifest(root, engine, module, self.sha)
        held_bytes = manifest.read_bytes()
        manifest.write_text('{"schemaVersion":"attacker-replacement"}', encoding="utf-8")
        lease = FakeImageLease(
            manifest,
            identity=FakeLeaseIdentity(31, 901, len(held_bytes)),
            digest=hashlib.sha256(held_bytes).hexdigest(),
            image=held_bytes,
        )

        trusted, error = collect_lifecycle.load_image_manifest_from_lease(
            lease, manifest, root, self.sha,
        )

        self.assertIsNone(error)
        self.assertEqual(
            trusted,
            {
                "SparkEngine.exe": hashlib.sha256(pe_image()).hexdigest(),
                "SparkGameFPS.dll": hashlib.sha256(pe_image()).hexdigest(),
            },
        )
        lease.close()

    def test_main_holds_manifest_lease_across_run_and_publishes_distinct_audit_log(self) -> None:
        """Replacing the pathname after its lease opens cannot swap manifest authority."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("manifest-held-through-run")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        engine = Path(argv[argv.index("--engine") + 1])
        module = Path(argv[argv.index("--module-image") + 1])
        manifest = Path(argv[argv.index("--image-manifest") + 1])
        held_bytes = manifest.read_bytes()
        lease = FakeImageLease(
            manifest,
            identity=FakeLeaseIdentity(32, 902, len(held_bytes)),
            digest=hashlib.sha256(held_bytes).hexdigest(),
            image=held_bytes,
        )
        engine_digest = hashlib.sha256(engine.read_bytes()).hexdigest()
        module_digest = hashlib.sha256(module.read_bytes()).hexdigest()
        engine_image = collect_lifecycle.ImageVerification(
            engine_digest, str(engine),
            collect_lifecycle.ImageIdentity(32, 903, engine.stat().st_size, 1),
        )
        module_image = collect_lifecycle.ImageVerification(
            module_digest, str(module),
            collect_lifecycle.ImageIdentity(32, 904, module.stat().st_size, 1),
        )
        opened: list[FakeImageLease] = []

        def manifest_factory(path: Path) -> FakeImageLease:
            self.assertEqual(path, manifest)
            opened.append(lease)
            # A path-based parse after open would now authorize this replacement.
            manifest.write_text('{"schemaVersion":"attacker-replacement"}',
                                encoding="utf-8")
            return lease

        def run_with_live_manifest(*args, **kwargs):
            self.assertFalse(lease.closed, "manifest lease was released before image leases/run")
            return collect_lifecycle.EngineOutput(
                self.VALID_RECORD, "stderr-without-newline", engine_image, module_image,
            ), None

        output_operations = self._fake_output_publication_operations(out)
        with mock.patch.object(sys, "argv", argv), \
             mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
             mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch.dict("collect_lifecycle.os.environ", {"GITHUB_SHA": self.sha}, clear=False), \
             mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                        return_value=("c" * 40, None)), \
             mock.patch("collect_lifecycle.open_image_lease", side_effect=manifest_factory), \
             mock.patch("collect_lifecycle.run_engine", side_effect=run_with_live_manifest), \
             mock.patch("collect_lifecycle._output_publication_operations_for_main",
                        return_value=output_operations):
            self.assertEqual(collect_lifecycle.main(), 0)

        self.assertEqual(opened, [lease])
        self.assertTrue(lease.closed)
        self.assertTrue(out.is_file())
        self.assertTrue(log.is_file())
        self.assertNotEqual(out, log)
        self.assertIn("\n--- stderr ---\n", log.read_text(encoding="utf-8"))

    def test_main_closes_manifest_lease_after_engine_failure(self) -> None:
        """Manifest lease ownership is released even when launch/collection fails."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("manifest-close-on-failure")
        manifest = Path(argv[argv.index("--image-manifest") + 1])
        manifest_bytes = manifest.read_bytes()
        lease = FakeImageLease(
            manifest,
            identity=FakeLeaseIdentity(33, 905, len(manifest_bytes)),
            digest=hashlib.sha256(manifest_bytes).hexdigest(),
            image=manifest_bytes,
        )
        output_operations = self._fake_output_publication_operations(out)
        with mock.patch.object(sys, "argv", argv), \
             mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
             mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch.dict("collect_lifecycle.os.environ", {"GITHUB_SHA": self.sha}, clear=False), \
             mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                        return_value=("c" * 40, None)), \
             mock.patch("collect_lifecycle.open_image_lease", return_value=lease), \
             mock.patch("collect_lifecycle.run_engine", return_value=(None, "injected launch failure")), \
             mock.patch("collect_lifecycle._output_publication_operations_for_main",
                        return_value=output_operations):
            self.assertEqual(collect_lifecycle.main(), 1)

        self.assertTrue(lease.closed)
        self._assert_no_transaction_artifacts(out)

    def test_main_retries_manifest_close_after_a_failed_release(self) -> None:
        """A failed manifest CloseHandle retains ownership for finally cleanup."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("manifest-close-retry")
        manifest = Path(argv[argv.index("--image-manifest") + 1])
        payload = manifest.read_bytes()
        lease = FakeImageLease(
            manifest,
            identity=FakeLeaseIdentity(33, 906, len(payload)),
            digest=hashlib.sha256(payload).hexdigest(), image=payload,
        )
        close_calls = 0

        def fail_then_close() -> None:
            nonlocal close_calls
            close_calls += 1
            if close_calls == 1:
                raise OSError("injected manifest CloseHandle failure")
            lease.closed = True

        lease.close = fail_then_close  # type: ignore[method-assign]
        self.assertEqual(self._run_truthful_collector_main(
            argv,
            mock.patch("collect_lifecycle.open_image_lease", return_value=lease),
        ), 1)
        self.assertEqual(close_calls, 2)
        self.assertTrue(lease.closed)
        self._assert_no_transaction_artifacts(out)

    def test_main_fails_closed_when_native_manifest_lease_is_unavailable(self) -> None:
        """A manifest-lease failure clears stale evidence after the fixed namespace is held."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("manifest-native-lease-required")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        out.write_text("preexisting-json", encoding="utf-8")
        log.write_text("preexisting-log", encoding="utf-8")
        output_operations = self._fake_output_publication_operations(out)
        with mock.patch.object(sys, "argv", argv), \
             mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
             mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch.dict("collect_lifecycle.os.environ", {"GITHUB_SHA": self.sha}, clear=False), \
             mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                        return_value=("c" * 40, None)), \
             mock.patch("collect_lifecycle.WINDOWS_IMAGE_LEASE_AVAILABLE", False), \
             mock.patch("collect_lifecycle.run_engine") as child, \
             mock.patch("collect_lifecycle._output_publication_operations_for_main",
                        return_value=output_operations):
             self.assertEqual(collect_lifecycle.main(), 1)

        child.assert_not_called()
        self._assert_no_transaction_artifacts(out)

    def test_main_clears_stale_pair_after_source_tree_failure(self) -> None:
        """A source-tree binding failure cannot leave an earlier lifecycle pair publishable."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("source-tree-failure-cleans-stale")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        out.write_text("preexisting-json", encoding="utf-8")
        log.write_text("preexisting-log", encoding="utf-8")

        self.assertEqual(self._run_truthful_collector_main(
            argv,
            mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                       return_value=(None, "injected source-tree failure")),
        ), 1)

        self._assert_no_transaction_artifacts(out)

    def test_display_final_path_preserves_leaf_case_while_comparisons_fold_case(self) -> None:
        """Display paths retain the canonical image spelling; security comparisons do not."""
        import collect_lifecycle

        self.assertEqual(
            collect_lifecycle._display_windows_final_path(
                r"\\?\C:\verified-artifact\.\SparkEngine.exe"),
            r"C:\verified-artifact\SparkEngine.exe",
        )
        self.assertEqual(
            collect_lifecycle._display_windows_final_path(
                r"\\?\UNC\server\share\verified-artifact\SparkGameFPS.dll"),
            r"\\server\share\verified-artifact\SparkGameFPS.dll",
        )
        self.assertEqual(
            collect_lifecycle._normalise_windows_final_path(
                r"\\?\C:\verified-artifact\SparkEngine.exe"),
            r"c:\verified-artifact\sparkengine.exe",
        )

    def test_main_records_handle_derived_paths_and_digests(self) -> None:
        """Published evidence keeps canonical lease-leaf casing for the consumer contract."""
        import collect_lifecycle

        out, argv = self._collector_main_fixture("handle-derived-record")
        engine = Path(argv[argv.index("--engine") + 1])
        module = Path(argv[argv.index("--module-image") + 1])
        engine_digest = hashlib.sha256(engine.read_bytes()).hexdigest()
        module_digest = hashlib.sha256(module.read_bytes()).hexdigest()
        engine_image = collect_lifecycle.ImageVerification(
            digest=engine_digest,
            final_path=collect_lifecycle._display_windows_final_path(
                r"\\?\C:\verified-artifact\.\SparkEngine.exe"),
            identity=collect_lifecycle.ImageIdentity(21, 701, engine.stat().st_size, 1),
        )
        module_image = collect_lifecycle.ImageVerification(
            digest=module_digest,
            final_path=collect_lifecycle._display_windows_final_path(
                r"\\?\C:\verified-artifact\SparkGameFPS.dll"),
            identity=collect_lifecycle.ImageIdentity(21, 702, module.stat().st_size, 1),
        )

        self.assertEqual(self._run_truthful_collector_main(
            argv,
            mock.patch(
                "collect_lifecycle.run_engine",
                return_value=(
                    collect_lifecycle.EngineOutput(
                        self.VALID_RECORD, "", engine_image, module_image,
                    ),
                    None,
                ),
            ),
        ), 0)

        record = json.loads(out.read_text(encoding="utf-8"))["records"][0]
        self.assertEqual(record["engineSHA256"], engine_digest)
        self.assertEqual(record["enginePath"], r"C:\verified-artifact\SparkEngine.exe")
        self.assertEqual(record["moduleSHA256"], module_digest)
        self.assertEqual(record["modulePath"], r"C:\verified-artifact\SparkGameFPS.dll")

        evidence = lifecycle_evidence(self.repo, self.sha)
        evidence["records"][0]["enginePath"] = record["enginePath"]
        evidence["records"][0]["modulePath"] = record["modulePath"]
        self.assertAccepted(base_manifest(), lifecycle_evidence=evidence)

        casing_alias = copy.deepcopy(evidence)
        casing_alias["records"][0]["enginePath"] = r"C:\verified-artifact\sparkengine.exe"
        errors = self.assertRejected(
            base_manifest(), "collector-display-path-casing-alias",
            lifecycle_evidence=casing_alias,
        )
        self.assertTrue(any("enginePath" in error for error in errors), errors)

    def test_parses_exact_standalone_terminal_record(self) -> None:
        from collect_lifecycle import parse_terminal_record
        self.assertEqual(parse_terminal_record(self.VALID_RECORD, INCLUDED), {
            "CreateModule": 1, "OnLoad": 1, "OnUpdate": 4,
            "OnFixedUpdate": 2, "OnRender": 4, "OnUnload": 1,
            "DestroyModule": 1,
        })

    def test_rejects_duplicate_prefixed_wrong_or_malformed_terminal_records(self) -> None:
        from collect_lifecycle import parse_terminal_record
        bad_inputs = (
            self.VALID_RECORD + "\n" + self.VALID_RECORD,
            "[info] " + self.VALID_RECORD,
            self.VALID_RECORD.replace("module=SparkGameFPS", "module=Other"),
            self.VALID_RECORD.replace("faults=0", "faults=0 extra=1"),
            self.VALID_RECORD.replace("create=1", "create=0"),
            self.VALID_RECORD.replace("faults=0", "faults=1"),
        )
        for text in bad_inputs:
            with self.subTest(text=text), self.assertRaises(ValueError):
                parse_terminal_record(text, INCLUDED)

    def test_rejects_marker_in_stderr_or_embedded_stream_copy(self) -> None:
        from collect_lifecycle import parse_terminal_streams
        self.assertEqual(parse_terminal_streams(self.VALID_RECORD, "", INCLUDED)["OnLoad"], 1)
        for stdout, stderr in ((self.VALID_RECORD, self.VALID_RECORD),
                               ("[info] " + self.VALID_RECORD, ""),
                               (self.VALID_RECORD + "\nlog " + self.VALID_RECORD, ""),
                               ("", self.VALID_RECORD)):
            with self.subTest(stdout=stdout, stderr=stderr), self.assertRaises(ValueError):
                parse_terminal_streams(stdout, stderr, INCLUDED)

    @unittest.skipUnless(os.name == "nt", "stable-v1 collector command contract is Windows-only")
    def test_run_command_and_environment_are_the_stable_v1_contract(self) -> None:
        from collect_lifecycle import run_engine
        root = self.repo / "package"
        root.mkdir(exist_ok=True)
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        completed = subprocess.CompletedProcess([], 0, self.VALID_RECORD, "")
        digest = hashlib.sha256(pe_image()).hexdigest()
        with mock.patch("collect_lifecycle.subprocess.run", return_value=completed) as run:
            captured, err = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest),
            )
        self.assertIsNone(err)
        self.assertEqual(captured.stdout, self.VALID_RECORD)
        cmd = run.call_args.args[0]
        self.assertEqual(cmd, [
            str(engine), "-game", str(module), "-require-game",
            "-test-seconds", "1.0", "-threads", "2", "-window-size", "640x360",
            "-no-subprocess",
        ])
        env = run.call_args.kwargs["env"]
        self.assertEqual(env["SPARK_RHI_BACKEND"], "d3d11")
        self.assertEqual(env["SPARK_D3D11_DRIVER"], "warp")
        self.assertEqual(run.call_args.kwargs["cwd"], str(root))

    def test_run_engine_keeps_image_leases_live_and_records_handle_metadata(self) -> None:
        """A child may start only while both authenticated image leases are held."""
        from collect_lifecycle import run_engine

        root = self.repo / "leased-package"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        engine_identity = FakeLeaseIdentity(11, 101, len(pe_image()))
        module_identity = FakeLeaseIdentity(11, 102, len(pe_image()))
        leases = [
            FakeImageLease(engine, identity=engine_identity, digest=digest),
            FakeImageLease(module, identity=module_identity, digest=digest),
            FakeImageLease(engine, identity=engine_identity, digest=digest),
            FakeImageLease(module, identity=module_identity, digest=digest),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        def child(*args, **kwargs):
            self.assertEqual(len(opened), 2)
            self.assertFalse(opened[0].closed)
            self.assertFalse(opened[1].closed)
            return subprocess.CompletedProcess([], 0, self.VALID_RECORD, "")

        with mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.subprocess.run", side_effect=child):
            captured, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIsNone(error)
        self.assertEqual([lease.path for lease in opened],
                         [engine, module, engine, module])
        self.assertTrue(all(lease.closed for lease in opened))
        self.assertEqual(captured.engine_image.digest, digest)
        self.assertEqual(captured.engine_image.final_path, str(engine))
        self.assertEqual(captured.module_image.digest, digest)
        self.assertEqual(captured.module_image.final_path, str(module))

    def test_run_engine_closes_held_leases_when_child_launch_fails(self) -> None:
        """A launch error cannot leak the write/delete-excluding image handles."""
        from collect_lifecycle import run_engine

        root = self.repo / "lease-launch-failure"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        leases = [
            FakeImageLease(engine, identity=FakeLeaseIdentity(12, 201, len(pe_image())), digest=digest),
            FakeImageLease(module, identity=FakeLeaseIdentity(12, 202, len(pe_image())), digest=digest),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        with mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.subprocess.run", side_effect=OSError("launch denied")):
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIn("cannot launch engine", error or "")
        self.assertEqual(len(opened), 2)
        self.assertTrue(all(lease.closed for lease in opened))

    def test_run_engine_closes_first_lease_when_second_lease_acquisition_fails(self) -> None:
        """A partially acquired image pair never leaves the first Windows handle open."""
        from collect_lifecycle import run_engine

        root = self.repo / "lease-open-failure"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        first = FakeImageLease(
            engine, identity=FakeLeaseIdentity(12, 203, len(pe_image())), digest=digest,
        )
        calls = 0

        def factory(path: Path) -> FakeImageLease:
            nonlocal calls
            calls += 1
            if calls == 1:
                self.assertEqual(path, engine)
                return first
            raise OSError("second image lease denied")

        with mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.subprocess.run") as child:
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIn("second image lease denied", error or "")
        child.assert_not_called()
        self.assertTrue(first.closed)

    def test_run_engine_rejects_post_run_lease_identity_change_and_closes_every_lease(self) -> None:
        """A pathname that changes identity after launch cannot produce evidence."""
        from collect_lifecycle import run_engine

        root = self.repo / "lease-identity-mutation"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        engine_identity = FakeLeaseIdentity(13, 301, len(pe_image()))
        module_identity = FakeLeaseIdentity(13, 302, len(pe_image()))
        leases = [
            FakeImageLease(engine, identity=engine_identity, digest=digest),
            FakeImageLease(module, identity=module_identity, digest=digest),
            FakeImageLease(engine, identity=FakeLeaseIdentity(13, 399, len(pe_image())), digest=digest),
            FakeImageLease(module, identity=module_identity, digest=digest),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        with mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.subprocess.run", return_value=subprocess.CompletedProcess(
                 [], 0, self.VALID_RECORD, "",
             )):
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIn("changed", error or "")
        self.assertEqual(len(opened), 4)
        self.assertTrue(all(lease.closed for lease in opened))

    def test_run_engine_rejects_engine_module_identity_alias_before_launch(self) -> None:
        """The executable and DLL cannot be two names for one hard-linked image."""
        from collect_lifecycle import run_engine

        root = self.repo / "lease-identity-alias"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        shared = FakeLeaseIdentity(14, 401, len(pe_image()))
        leases = [
            FakeImageLease(engine, identity=shared, digest=digest),
            FakeImageLease(module, identity=shared, digest=digest),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        with mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.subprocess.run") as child:
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIn("same file identity", error or "")
        child.assert_not_called()
        self.assertEqual(len(opened), 2)
        self.assertTrue(all(lease.closed for lease in opened))

    def test_run_engine_rejects_handle_final_path_alias_before_launch(self) -> None:
        """A path resolved from a handle must still equal the requested image path."""
        from collect_lifecycle import run_engine

        root = self.repo / "lease-final-path-alias"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()
        leases = [
            FakeImageLease(
                engine, identity=FakeLeaseIdentity(16, 601, len(pe_image())),
                digest=digest, final_path=str(root / "other" / "SparkEngine.exe"),
            ),
            FakeImageLease(
                module, identity=FakeLeaseIdentity(16, 602, len(pe_image())), digest=digest,
            ),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        with mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.subprocess.run") as child:
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest), lease_factory=factory,
            )

        self.assertIn("handle path", error or "")
        child.assert_not_called()
        self.assertTrue(all(lease.closed for lease in opened))

    def test_run_engine_fails_closed_without_native_lease_support(self) -> None:
        """Windows collection never falls back to path-only hashing."""
        from collect_lifecycle import run_engine

        root = self.repo / "no-native-lease"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        digest = hashlib.sha256(pe_image()).hexdigest()

        with mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.WINDOWS_IMAGE_LEASE_AVAILABLE", False), \
             mock.patch("collect_lifecycle.subprocess.run") as child:
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(digest, digest),
            )

        self.assertIn("native Windows image leases", error or "")
        child.assert_not_called()

    def test_resolve_collection_sha_rejects_environment_commit_disagreement(self) -> None:
        """A CI environment SHA cannot silently overrule the checked-out revision."""
        from collect_lifecycle import resolve_collection_sha

        with mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch.dict("collect_lifecycle.os.environ", {"GITHUB_SHA": "f" * 40}, clear=False):
            sha, error = resolve_collection_sha(self.sha)

        self.assertIsNone(sha)
        self.assertIn("GITHUB_SHA", error or "")

    def test_main_rejects_noncanonical_manifest_or_commit_before_launch(self) -> None:
        """Only the fixed artifact-root manifest for checkout HEAD can authorize a run."""
        import collect_lifecycle

        root = self.repo / "fixed-manifest-package"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        manifest = write_image_manifest(root, engine, module, self.sha)
        wrong_name = root / "image-manifest.json"
        wrong_name.write_text(manifest.read_text(encoding="utf-8"), encoding="utf-8")
        outside = self.repo / "outside-image-manifest"
        outside.mkdir()
        outside_manifest = outside / "module-lifecycle-images.json"
        outside_manifest.write_text(manifest.read_text(encoding="utf-8"), encoding="utf-8")
        cases = (
            ("wrong-name", wrong_name, self.sha, self.sha),
            ("lexical-traversal", root / "nested" / ".." / manifest.name,
             self.sha, self.sha),
            ("case-alias", str(root / "MODULE-LIFECYCLE-IMAGES.JSON"),
             self.sha, self.sha),
            ("ads-alias", str(manifest) + ":alternate", self.sha, self.sha),
            ("outside-root", outside_manifest, self.sha, self.sha),
            ("commit-mismatch", manifest, self.sha, "f" * 40),
        )

        for name, image_manifest, requested_sha, head_sha in cases:
            with self.subTest(case=name):
                out = self.repo / "build" / "module-evidence" / "module-lifecycle.json"
                out.parent.mkdir(parents=True, exist_ok=True)
                for artifact in (out, out.parent / "module-lifecycle-SparkGameFPS.log"):
                    if artifact.exists():
                        artifact.unlink()
                argv = [
                    "collect_lifecycle.py", "--engine", str(engine), "--module", INCLUDED,
                    "--module-image", str(module), "--working-directory", str(root),
                    "--rhi-backend", "d3d11", "--image-manifest", str(image_manifest),
                    "--out", str(out), "--commit-sha", requested_sha,
                ]
                output_operations = self._fake_output_publication_operations(out)
                with mock.patch.object(sys, "argv", argv), \
                     mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
                     mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
                     mock.patch("collect_lifecycle.resolve_head_sha", return_value=(head_sha, None)), \
                     mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                                return_value=("c" * 40, None)), \
                     mock.patch("collect_lifecycle.run_engine") as child, \
                     mock.patch("collect_lifecycle._output_publication_operations_for_main",
                                return_value=output_operations):
                    self.assertEqual(collect_lifecycle.main(), 1)
                child.assert_not_called()
                self.assertFalse(out.exists())

    @unittest.skipUnless(os.name == "nt", "native Windows image leases are unavailable")
    def test_native_image_lease_requests_read_only_nondelete_share_mode(self) -> None:
        """The native lease denies new write and delete opens for the image epoch."""
        import collect_lifecycle

        if not collect_lifecycle.WINDOWS_IMAGE_LEASE_AVAILABLE:
            self.skipTest("native Windows image lease binding is unavailable")
        root = self.repo / "native-lease-intent"
        root.mkdir()
        engine = root / "SparkEngine.exe"
        engine.write_bytes(pe_image())
        native_create = collect_lifecycle._CreateFileW
        native_set_information = collect_lifecycle._SetHandleInformation
        with mock.patch("collect_lifecycle._CreateFileW", wraps=native_create) as create, \
             mock.patch("collect_lifecycle._SetHandleInformation",
                        wraps=native_set_information) as set_information:
            lease = collect_lifecycle.open_image_lease(engine)
        try:
            args = create.call_args.args
            self.assertEqual(args[1], collect_lifecycle._GENERIC_READ |
                             collect_lifecycle._FILE_READ_ATTRIBUTES)
            self.assertEqual(args[2], collect_lifecycle._FILE_SHARE_READ)
            self.assertEqual(args[4], collect_lifecycle._OPEN_EXISTING)
            required_flags = (collect_lifecycle._FILE_ATTRIBUTE_NORMAL |
                              collect_lifecycle._FILE_FLAG_OPEN_REPARSE_POINT)
            self.assertEqual(args[5] & required_flags, required_flags)
            self.assertEqual(
                set_information.call_args.args[1:],
                (collect_lifecycle._HANDLE_FLAG_INHERIT, 0),
            )
            self.assertTrue(lease.final_path.endswith(r"\SparkEngine.exe"), lease.final_path)
            self.assertEqual(lease.sha256(), hashlib.sha256(pe_image()).hexdigest())
            with self.assertRaises(OSError):
                engine.write_bytes(pe_image())
        finally:
            lease.close()

    @unittest.skipUnless(os.name == "nt", "native Windows handles are unavailable")
    def test_image_lease_close_failure_keeps_retry_authority_live(self) -> None:
        """A failed CloseHandle must not make a still-live image lease look closed."""
        import collect_lifecycle

        lease = collect_lifecycle.ImageLease(
            self.repo / "close-retry.exe", 0x1234,
            collect_lifecycle.ImageIdentity(1, 2, 3, 4), 0,
            r"C:\verified-artifact\SparkEngine.exe",
        )
        with mock.patch("collect_lifecycle._CloseHandle", side_effect=(False, True)) as close, \
             mock.patch("collect_lifecycle.ctypes.get_last_error", return_value=5):
            with self.assertRaisesRegex(OSError, "CloseHandle failed"):
                lease.close()
            self.assertFalse(lease.closed)
            lease.close()
        self.assertTrue(lease.closed)
        self.assertEqual(close.call_count, 2)

    @unittest.skipUnless(os.name == "nt", "native Windows directory leases are unavailable")
    def test_native_output_namespace_leases_anchor_renames_and_publish_by_handle(self) -> None:
        """Rooted no-follow I/O, rather than share flags, contains reparse mutation."""
        import collect_lifecycle

        if not collect_lifecycle.WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE:
            self.skipTest("native Windows output-directory lease binding is unavailable")
        container = self.repo / "native-output-directory-lease-container"
        container.mkdir()
        root = container / "repo"
        root.mkdir()
        build = root / "build"
        build.mkdir()
        directory = build / "module-evidence"
        directory.mkdir()
        renamed_output = build / "renamed-evidence"
        replacement = build / "replacement-evidence"
        renamed_build = root / "renamed-build"
        renamed_root = container / "renamed-root"
        replacement.mkdir()
        native_create = collect_lifecycle._CreateFileW
        native_relative_create = collect_lifecycle._NtCreateFile
        native_set_information = collect_lifecycle._SetHandleInformation
        with mock.patch("collect_lifecycle._CreateFileW", wraps=native_create) as create, \
             mock.patch("collect_lifecycle._NtCreateFile",
                        wraps=native_relative_create) as relative_create, \
             mock.patch("collect_lifecycle._SetHandleInformation",
                        wraps=native_set_information) as set_information:
            leases, error = collect_lifecycle._open_output_namespace_leases(root, directory)
        self.assertIsNone(error)
        self.assertIsNotNone(leases)
        assert leases is not None
        try:
            self.assertEqual(len(create.call_args_list), 1)
            root_args = create.call_args.args
            self.assertEqual(
                root_args[1],
                collect_lifecycle._FILE_LIST_DIRECTORY |
                collect_lifecycle._FILE_READ_ATTRIBUTES |
                collect_lifecycle._SYNCHRONIZE,
            )
            self.assertEqual(root_args[2], collect_lifecycle._FILE_SHARE_READ)
            self.assertEqual(root_args[4], collect_lifecycle._OPEN_EXISTING)
            required_flags = (collect_lifecycle._FILE_FLAG_BACKUP_SEMANTICS |
                              collect_lifecycle._FILE_FLAG_OPEN_REPARSE_POINT)
            self.assertEqual(root_args[5] & required_flags, required_flags)
            self.assertEqual(len(relative_create.call_args_list), 2)
            for expected_leaf, call in zip(("build", "module-evidence"),
                                           relative_create.call_args_list, strict=True):
                args = call.args
                self.assertEqual(
                    args[1],
                    collect_lifecycle._FILE_LIST_DIRECTORY |
                    collect_lifecycle._FILE_READ_ATTRIBUTES |
                    collect_lifecycle._SYNCHRONIZE,
                )
                self.assertEqual(args[6], collect_lifecycle._FILE_SHARE_READ)
                self.assertEqual(args[7], collect_lifecycle._FILE_OPEN_IF)
                required_options = (
                    collect_lifecycle._FILE_DIRECTORY_FILE |
                    collect_lifecycle._FILE_FLAG_OPEN_REPARSE_POINT |
                    collect_lifecycle._FILE_SYNCHRONOUS_IO_NONALERT
                )
                self.assertEqual(args[8] & required_options, required_options)
                object_name = args[2]._obj.ObjectName.contents
                self.assertEqual(object_name.Buffer[:object_name.Length // 2], expected_leaf)
            self.assertEqual(set_information.call_count, 3)
            for call in set_information.call_args_list:
                self.assertEqual(
                    call.args[1:],
                    (collect_lifecycle._HANDLE_FLAG_INHERIT, 0),
                )
            self.assertTrue(all(
                anchor.lease.attributes & collect_lifecycle._FILE_ATTRIBUTE_DIRECTORY
                for anchor in leases
            ))
            self.assertTrue(leases[-1].lease.final_path.endswith(r"\module-evidence"),
                            leases[-1].lease.final_path)
            with self.assertRaises(OSError):
                directory.rename(renamed_output)
            with self.assertRaises(OSError):
                os.replace(replacement, directory)
            with self.assertRaises(OSError):
                build.rename(renamed_build)
            with self.assertRaises(OSError):
                root.rename(renamed_root)
            staged = directory / "staged.tmp"
            published = directory / "module-lifecycle-SparkGameFPS.log"
            staged.write_text("audit", encoding="utf-8")
            with self.assertRaises(OSError):
                os.replace(staged, published)
            # This is a compatibility check only. FILE_SHARE_READ rejects an
            # ordinary GENERIC_WRITE open, but FILE_WRITE_ATTRIBUTES can still
            # mutate reparse metadata; the dedicated mutation test below proves
            # containment comes from rooted FILE_OPEN_REPARSE_POINT operations.
            writer = native_create(
                str(directory), collect_lifecycle._GENERIC_WRITE,
                collect_lifecycle._FILE_SHARE_READ | collect_lifecycle._FILE_SHARE_WRITE,
                None, collect_lifecycle._OPEN_EXISTING,
                collect_lifecycle._FILE_FLAG_BACKUP_SEMANTICS |
                collect_lifecycle._FILE_FLAG_OPEN_REPARSE_POINT,
                None,
            )
            self.assertIn(writer, (None, collect_lifecycle._INVALID_HANDLE_VALUE))
            stale = directory / "module-lifecycle.json"
            stale.write_text("stale", encoding="utf-8")
            output_lease = leases[-1].lease
            self.assertIsNone(collect_lifecycle._clear_output_artifacts(output_lease, stale))
            self.assertFalse(stale.exists())
            with mock.patch(
                "collect_lifecycle._NtCreateFile", wraps=native_relative_create,
            ) as direct_create:
                published_artifact = collect_lifecycle._write_output_artifact(
                    output_lease, published, "audit",
                )
            self.assertEqual(
                len(direct_create.call_args_list), 1,
                "direct final publication must use exactly one rooted child open",
            )
            writer_args = direct_create.call_args_list[0].args
            self.assertEqual(
                writer_args[1],
                collect_lifecycle._GENERIC_READ | collect_lifecycle._GENERIC_WRITE |
                collect_lifecycle._FILE_READ_ATTRIBUTES | collect_lifecycle._DELETE |
                collect_lifecycle._SYNCHRONIZE,
            )
            self.assertEqual(writer_args[6], collect_lifecycle._FILE_SHARE_READ)
            self.assertEqual(writer_args[7], collect_lifecycle._FILE_CREATE)
            required_file_options = (
                collect_lifecycle._FILE_NON_DIRECTORY_FILE |
                collect_lifecycle._FILE_FLAG_OPEN_REPARSE_POINT |
                collect_lifecycle._FILE_SYNCHRONOUS_IO_NONALERT
            )
            self.assertEqual(
                writer_args[8] & required_file_options, required_file_options,
            )
            self.assertEqual(
                writer_args[2]._obj.RootDirectory, output_lease._handle,
                "direct final publication must be rooted at the held output directory HANDLE",
            )
            backup_artifact = published_artifact.duplicate()
            published_artifact.close()
            self.assertEqual(
                collect_lifecycle._native_read_at(
                    backup_artifact._handle, 0, len("audit"),
                ),
                b"audit",
            )
            self.assertIsNone(collect_lifecycle._discard_output_artifacts(
                [backup_artifact],
            ))
            self.assertFalse(published.exists())
            self.assertTrue(directory.is_dir())
            self.assertTrue(replacement.is_dir())
        finally:
            close_error = collect_lifecycle._close_image_leases(
                [anchor.lease for anchor in leases],
            )
        self.assertIsNone(close_error)
        self.assertTrue(all(anchor.lease.closed for anchor in leases))

    @unittest.skipUnless(os.name == "nt", "native Windows readonly rollback tests are unavailable")
    def test_native_exact_guard_deletes_readonly_artifact_without_path_reopen(self) -> None:
        """FileDispositionInfoEx removes a hostile-readonly final leaf by its guard."""
        import ctypes
        from ctypes import wintypes
        import collect_lifecycle

        if not collect_lifecycle.WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE:
            self.skipTest("native Windows output-directory lease binding is unavailable")

        class FileBasicInformation(ctypes.Structure):
            _fields_ = [
                ("CreationTime", ctypes.c_longlong),
                ("LastAccessTime", ctypes.c_longlong),
                ("LastWriteTime", ctypes.c_longlong),
                ("ChangeTime", ctypes.c_longlong),
                ("FileAttributes", wintypes.DWORD),
            ]

        namespace, error = collect_lifecycle._open_output_namespace_leases(
            self.repo, self.repo / "build" / "module-evidence",
        )
        self.assertIsNone(error)
        assert namespace is not None
        output_lease = namespace[-1].lease
        guard = collect_lifecycle._write_output_artifact(
            output_lease,
            self.repo / "build" / "module-evidence" /
            collect_lifecycle.LIFECYCLE_OUTPUT_FILENAME,
            "{\"stale\": true}\n",
        )
        hostile = collect_lifecycle._CreateFileW(
            str(self.repo / "build" / "module-evidence" /
                collect_lifecycle.LIFECYCLE_OUTPUT_FILENAME),
            collect_lifecycle._FILE_WRITE_ATTRIBUTES,
            collect_lifecycle._FILE_SHARE_READ | collect_lifecycle._FILE_SHARE_WRITE | 0x00000004,
            None,
            collect_lifecycle._OPEN_EXISTING,
            collect_lifecycle._FILE_ATTRIBUTE_NORMAL |
            collect_lifecycle._FILE_FLAG_OPEN_REPARSE_POINT,
            None,
        )
        self.assertNotIn(hostile, (None, collect_lifecycle._INVALID_HANDLE_VALUE))
        assert hostile not in (None, collect_lifecycle._INVALID_HANDLE_VALUE)
        try:
            basic = FileBasicInformation(-1, -1, -1, -1, 0x00000001)
            self.assertTrue(
                collect_lifecycle._SetFileInformationByHandle(
                    hostile, 0, ctypes.byref(basic), ctypes.sizeof(basic),
                ),
                f"could not set hostile readonly attribute: {ctypes.get_last_error()}",
            )
            guard.discard()
            self.assertFalse(
                (self.repo / "build" / "module-evidence" /
                 collect_lifecycle.LIFECYCLE_OUTPUT_FILENAME).exists(),
                "readonly lifecycle artifact retained its pathname after guarded rollback",
            )
        finally:
            if not guard.closed:
                guard.close()
            self.assertTrue(collect_lifecycle._CloseHandle(hostile))
            close_error = collect_lifecycle._close_image_leases(
                [anchor.lease for anchor in namespace],
            )
            self.assertIsNone(close_error)

    @unittest.skipUnless(os.name == "nt", "native Windows readonly rollback tests are unavailable")
    def test_exact_guard_requests_only_readonly_safe_disposition_flags(self) -> None:
        """Rollback must not silently downgrade to legacy FileDispositionInfo."""
        import ctypes
        import collect_lifecycle

        guard = collect_lifecycle.OutputArtifactHandle(
            0x4567, object(), collect_lifecycle.LIFECYCLE_OUTPUT_FILENAME,
        )
        expected_flags = (
            collect_lifecycle._FILE_DISPOSITION_FLAG_DELETE |
            collect_lifecycle._FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
            collect_lifecycle._FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE
        )
        with mock.patch("collect_lifecycle._SetFileInformationByHandle", return_value=True) as set_info, \
             mock.patch("collect_lifecycle._CloseHandle", return_value=True):
            guard.discard()

        args = set_info.call_args.args
        self.assertEqual(args[1], collect_lifecycle._FILE_DISPOSITION_INFO_EX)
        disposition = ctypes.cast(
            args[2], ctypes.POINTER(collect_lifecycle._FILE_DISPOSITION_INFORMATION_EX),
        ).contents
        self.assertEqual(disposition.Flags, expected_flags)

    @unittest.skipUnless(os.name == "nt", "native Windows output reserve tests are unavailable")
    def test_native_process_lifetime_output_reserves_block_writes_until_exit(self) -> None:
        """A detached final JSON HANDLE stays authoritative until its process exits."""
        import collect_lifecycle

        if not collect_lifecycle.WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE:
            self.skipTest("native Windows output-directory lease binding is unavailable")
        directory = self.repo / "native-process-lifetime-reserves"
        directory.mkdir()
        json_path = directory / collect_lifecycle.LIFECYCLE_OUTPUT_FILENAME
        child_code = """
import sys
import time
from pathlib import Path
import collect_lifecycle as collector

directory = Path(sys.argv[1])
lease = collector.open_output_directory_lease(directory)
audit = collector._write_output_artifact(
    lease, directory / collector.LIFECYCLE_AUDIT_LOG_FILENAME, "audit\\n",
)
json_guard = collector._write_output_artifact(
    lease, directory / collector.LIFECYCLE_OUTPUT_FILENAME, "{\\\"ok\\\": true}\\n",
)
audit_reserve = audit.duplicate()
json_reserve = json_guard.duplicate()
audit.close()
json_guard.close()
error = collector._handoff_output_reserves_to_process_exit(
    [audit_reserve, json_reserve],
)
if error:
    raise RuntimeError(error)
lease.close()
print("READY", flush=True)
time.sleep(60)
"""
        module_directory = REPO_ROOT / "tools" / "module-evidence"
        environment = dict(os.environ)
        prior_pythonpath = environment.get("PYTHONPATH")
        environment["PYTHONPATH"] = (
            str(module_directory) + (os.pathsep + prior_pythonpath if prior_pythonpath else "")
        )
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        process = subprocess.Popen(
            [sys.executable, "-c", child_code, str(directory)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=environment,
        )
        try:
            assert process.stdout is not None
            self.assertEqual(process.stdout.readline().strip(), "READY")
            writer = collect_lifecycle._CreateFileW(
                str(json_path), collect_lifecycle._GENERIC_WRITE,
                collect_lifecycle._FILE_SHARE_READ |
                collect_lifecycle._FILE_SHARE_WRITE | 0x00000004,
                None, collect_lifecycle._OPEN_EXISTING,
                collect_lifecycle._FILE_ATTRIBUTE_NORMAL |
                collect_lifecycle._FILE_FLAG_OPEN_REPARSE_POINT,
                None,
            )
            if writer not in (None, collect_lifecycle._INVALID_HANDLE_VALUE):
                collect_lifecycle._CloseHandle(writer)
                self.fail("a writer opened the JSON while the child reserve was live")
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate(timeout=10)
        writer = collect_lifecycle._CreateFileW(
            str(json_path), collect_lifecycle._GENERIC_WRITE,
            collect_lifecycle._FILE_SHARE_READ |
            collect_lifecycle._FILE_SHARE_WRITE | 0x00000004,
            None, collect_lifecycle._OPEN_EXISTING,
            collect_lifecycle._FILE_ATTRIBUTE_NORMAL |
            collect_lifecycle._FILE_FLAG_OPEN_REPARSE_POINT,
            None,
        )
        self.assertNotIn(writer, (None, collect_lifecycle._INVALID_HANDLE_VALUE))
        assert writer not in (None, collect_lifecycle._INVALID_HANDLE_VALUE)
        self.assertTrue(collect_lifecycle._CloseHandle(writer))

    @unittest.skipUnless(os.name == "nt", "native Windows rooted output tests are unavailable")
    def test_native_rooted_output_authority_rejects_post_acquisition_reparse_mutation(self) -> None:
        """FILE_WRITE_ATTRIBUTES reparse attacks cannot redirect rooted child operations."""
        import ctypes
        from ctypes import wintypes
        import struct
        import collect_lifecycle

        if not collect_lifecycle.WINDOWS_OUTPUT_DIRECTORY_LEASE_AVAILABLE:
            self.skipTest("native Windows output-directory lease binding is unavailable")

        fsctl_set_reparse_point = 0x000900A4
        fsctl_delete_reparse_point = 0x000900AC
        io_reparse_tag_mount_point = 0xA0000003
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        device_io_control = kernel32.DeviceIoControl
        device_io_control.argtypes = [
            wintypes.HANDLE, wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
            wintypes.LPVOID, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
            wintypes.LPVOID,
        ]
        device_io_control.restype = wintypes.BOOL

        def mount_point_data(target: Path) -> bytes:
            substitute = ("\\??\\" + str(target)).encode("utf-16-le")
            printable = str(target).encode("utf-16-le")
            substitute_terminator = b"\0\0"
            printable_terminator = b"\0\0"
            data_length = (
                8 + len(substitute) + len(substitute_terminator) +
                len(printable) + len(printable_terminator)
            )
            return (
                struct.pack(
                    "<LHHHHHH",
                    io_reparse_tag_mount_point, data_length, 0,
                    0, len(substitute),
                    len(substitute) + len(substitute_terminator), len(printable),
                ) +
                substitute + substitute_terminator + printable + printable_terminator
            )

        def set_mount_point(handle: int, target: Path) -> None:
            data = mount_point_data(target)
            buffer = ctypes.create_string_buffer(data)
            returned = wintypes.DWORD()
            if not device_io_control(
                handle, fsctl_set_reparse_point, buffer, len(data),
                None, 0, ctypes.byref(returned), None,
            ):
                self.fail(
                    "FILE_WRITE_ATTRIBUTES did not permit FSCTL_SET_REPARSE_POINT: "
                    f"{ctypes.get_last_error()}"
                )

        def clear_mount_point(handle: int) -> None:
            data = struct.pack("<LHH", io_reparse_tag_mount_point, 0, 0)
            buffer = ctypes.create_string_buffer(data)
            returned = wintypes.DWORD()
            if not device_io_control(
                handle, fsctl_delete_reparse_point, buffer, len(data),
                None, 0, ctypes.byref(returned), None,
            ):
                self.fail(f"FSCTL_DELETE_REPARSE_POINT failed: {ctypes.get_last_error()}")

        def open_write_attributes(path: Path) -> int:
            handle = collect_lifecycle._CreateFileW(
                str(path),
                collect_lifecycle._FILE_WRITE_ATTRIBUTES,
                collect_lifecycle._FILE_SHARE_READ | collect_lifecycle._FILE_SHARE_WRITE,
                None,
                collect_lifecycle._OPEN_EXISTING,
                collect_lifecycle._FILE_FLAG_BACKUP_SEMANTICS |
                collect_lifecycle._FILE_FLAG_OPEN_REPARSE_POINT,
                None,
            )
            self.assertNotIn(handle, (None, collect_lifecycle._INVALID_HANDLE_VALUE))
            return handle

        def close_handles(handles: list[object]) -> None:
            error = collect_lifecycle._close_image_leases(handles)
            self.assertIsNone(error, error)

        container = self.repo / "native-rooted-reparse-container"
        container.mkdir()
        victim = container / "victim"
        victim.mkdir()

        # Root: acquire it once by full path, mutate it in place, then ensure
        # its rooted child acquisition neither follows the junction nor creates
        # a victim child.
        root = container / "root"
        root.mkdir()
        root_lease = collect_lifecycle.open_output_directory_lease(root)
        root_writer = open_write_attributes(root)
        try:
            set_mount_point(root_writer, victim)
            with self.assertRaisesRegex(OSError, "0xC0000280"):
                collect_lifecycle._open_relative_output_directory(
                    root_lease, root / "build", "build",
                )
            self.assertFalse((victim / "build").exists())
        finally:
            clear_mount_point(root_writer)
            collect_lifecycle._CloseHandle(root_writer)
            close_handles([root_lease])

        # Build: root-relative acquisition succeeds normally, but a mutation
        # after that acquisition cannot redirect the output-directory child.
        root = container / "build-root"
        root.mkdir()
        root_lease = collect_lifecycle.open_output_directory_lease(root)
        build = root / "build"
        build_lease = collect_lifecycle._open_relative_output_directory(
            root_lease, build, "build",
        )
        build_writer = open_write_attributes(build)
        try:
            set_mount_point(build_writer, victim)
            with self.assertRaisesRegex(OSError, "0xC0000280"):
                collect_lifecycle._open_relative_output_directory(
                    build_lease, build / "module-evidence", "module-evidence",
                )
            self.assertFalse((victim / "module-evidence").exists())
        finally:
            clear_mount_point(build_writer)
            collect_lifecycle._CloseHandle(build_writer)
            close_handles([build_lease, root_lease])

        # Output: after every ancestor is pinned, FILE_WRITE_ATTRIBUTES can
        # still set a junction. Rooted FILE_CREATE and FILE_OPEN cleanup must
        # fail closed and leave external fixed-name victims untouched.
        root = container / "output-root"
        root.mkdir()
        leases, error = collect_lifecycle._open_output_namespace_leases(
            root, root / "build" / "module-evidence",
        )
        self.assertIsNone(error)
        assert leases is not None
        output_directory = root / "build" / "module-evidence"
        output_lease = leases[-1].lease
        output_writer = open_write_attributes(output_directory)
        victim_json = victim / collect_lifecycle.LIFECYCLE_OUTPUT_FILENAME
        victim_log = victim / collect_lifecycle.LIFECYCLE_AUDIT_LOG_FILENAME
        victim_json.write_text("victim-json", encoding="utf-8")
        victim_log.write_text("victim-log", encoding="utf-8")
        try:
            set_mount_point(output_writer, victim)
            with self.assertRaisesRegex(OSError, "0xC0000280"):
                collect_lifecycle._write_output_artifact(
                    output_lease,
                    output_directory / collect_lifecycle.LIFECYCLE_AUDIT_LOG_FILENAME,
                    "attacker",
                )
            clear_error = collect_lifecycle._clear_output_artifacts(
                output_lease,
                output_directory / collect_lifecycle.LIFECYCLE_OUTPUT_FILENAME,
                output_directory / collect_lifecycle.LIFECYCLE_AUDIT_LOG_FILENAME,
            )
            self.assertIsNotNone(clear_error)
            self.assertIn("0xC0000280", clear_error)
            self.assertEqual(victim_json.read_text(encoding="utf-8"), "victim-json")
            self.assertEqual(victim_log.read_text(encoding="utf-8"), "victim-log")
        finally:
            clear_mount_point(output_writer)
            collect_lifecycle._CloseHandle(output_writer)
            close_handles([anchor.lease for anchor in leases])

        # A fixed leaf that is itself a reparse point must also be opened as
        # the reparse object and rejected, never followed to a victim.
        leaf_root = container / "leaf-root"
        leaf_root.mkdir()
        leases, error = collect_lifecycle._open_output_namespace_leases(
            leaf_root, leaf_root / "build" / "module-evidence",
        )
        self.assertIsNone(error)
        assert leases is not None
        output_directory = leaf_root / "build" / "module-evidence"
        output_lease = leases[-1].lease
        leaf = output_directory / collect_lifecycle.LIFECYCLE_OUTPUT_FILENAME
        leaf.mkdir()
        leaf_writer = open_write_attributes(leaf)
        leaf_victim = victim / "leaf-victim"
        leaf_victim.mkdir()
        try:
            set_mount_point(leaf_writer, leaf_victim)
            clear_error = collect_lifecycle._clear_output_artifacts(output_lease, leaf)
            self.assertIsNotNone(clear_error)
            self.assertTrue(leaf_victim.is_dir())
        finally:
            clear_mount_point(leaf_writer)
            collect_lifecycle._CloseHandle(leaf_writer)
            leaf.rmdir()
            close_handles([anchor.lease for anchor in leases])

    def test_rejects_outside_script_and_reparse_images(self) -> None:
        from collect_lifecycle import validate_image_pair
        root = self.repo / "package"
        root.mkdir(exist_ok=True)
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        outside = self.repo / "SparkGameFPS.dll"
        outside.write_bytes(pe_image())
        self.assertIsNotNone(validate_image_pair(engine, outside, INCLUDED, root))
        script = root / "SparkGameFPS.cmd"
        script.write_text("@echo off\n", encoding="utf-8")
        self.assertIsNotNone(validate_image_pair(engine, script, INCLUDED, root))
        link = root / "linked.dll"
        try:
            os.symlink(str(module), str(link))
        except (OSError, NotImplementedError):
            self.skipTest("cannot create symlinks on this platform")
        self.assertIsNotNone(validate_image_pair(engine, link, INCLUDED, root))

    def test_main_writes_no_json_or_log_after_failed_or_invalid_run(self) -> None:
        import collect_lifecycle

        out, argv = self._collector_main_fixture("failed-or-invalid-run")
        engine = Path(argv[argv.index("--engine") + 1])
        module = Path(argv[argv.index("--module-image") + 1])
        engine_image = collect_lifecycle.ImageVerification(
            hashlib.sha256(engine.read_bytes()).hexdigest(), str(engine),
            collect_lifecycle.ImageIdentity(41, 1001, engine.stat().st_size, 1),
        )
        module_image = collect_lifecycle.ImageVerification(
            hashlib.sha256(module.read_bytes()).hexdigest(), str(module),
            collect_lifecycle.ImageIdentity(41, 1002, module.stat().st_size, 1),
        )
        cases = (
            ("failed-engine", collect_lifecycle.EngineOutput(
                "bad output", "failure", engine_image, module_image,
            ), "injected non-zero engine exit"),
            ("malformed-stream", collect_lifecycle.EngineOutput(
                "malformed output", "", engine_image, module_image,
            ), None),
        )
        for name, captured, error in cases:
            with self.subTest(case=name):
                self.assertEqual(self._run_truthful_collector_main(
                    argv,
                    mock.patch("collect_lifecycle.run_engine",
                               return_value=(captured, error)),
                ), 1)
            self.assertFalse(out.exists())
            self.assertFalse((out.parent / "module-lifecycle-SparkGameFPS.log").exists())

    def test_module_digest_and_path_are_required_and_valid(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        for key, value in (("moduleSHA256", None), ("modulePath", ""),
                           ("moduleSHA256", "not-a-digest")):
            candidate = copy.deepcopy(ev)
            if value is None:
                del candidate["records"][0][key]
            else:
                candidate["records"][0][key] = value
            errors = self.assertRejected(base_manifest(), f"collector-{key}",
                                         lifecycle_evidence=candidate)
            self.assertTrue(any(key in error or "missing" in error for error in errors))

    def test_run_engine_rejects_marker_printer_and_nonstable_engine_identity(self) -> None:
        from collect_lifecycle import run_engine
        root = self.repo / "authenticated-package"
        root.mkdir()
        module = root / "SparkGameFPS.dll"
        module.write_bytes(pe_image())
        for name, content in (("SparkEngine.exe", b"MZ" + b"\0" * 8192),
                              ("SparkConsole.exe", pe_image())):
            engine = root / name
            engine.write_bytes(content)
            expected = (
                hashlib.sha256(content).hexdigest(),
                hashlib.sha256(pe_image()).hexdigest(),
            )
            with self.subTest(engine=name), \
                  mock.patch("collect_lifecycle.subprocess.run") as run:
                _, error = run_engine(
                    engine, module, INCLUDED, root, "d3d11", 30,
                    expected_digests=expected,
                )
            self.assertIsNotNone(error)
            run.assert_not_called()

    def test_main_requires_trusted_manifest_and_rejects_digest_mismatch_before_launch(self) -> None:
        import collect_lifecycle
        root = self.repo / "manifest-package"
        root.mkdir()
        engine, module = root / "SparkEngine.exe", root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        out = self.repo / "build" / "module-evidence" / "module-lifecycle.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        bad_manifest = write_image_manifest(root, engine, module, self.sha,
                                            engine_digest="0" * 64)
        manifest_bytes = bad_manifest.read_bytes()
        lease = FakeImageLease(
            bad_manifest,
            identity=FakeLeaseIdentity(42, 1101, len(manifest_bytes)),
            digest=hashlib.sha256(manifest_bytes).hexdigest(),
            image=manifest_bytes,
        )
        image_digest = hashlib.sha256(pe_image()).hexdigest()
        engine_lease = FakeImageLease(
            engine,
            identity=FakeLeaseIdentity(42, 1102, len(pe_image())),
            digest=image_digest,
        )
        module_lease = FakeImageLease(
            module,
            identity=FakeLeaseIdentity(42, 1103, len(pe_image())),
            digest=image_digest,
        )
        opened: list[FakeImageLease] = []

        def lease_factory(path: Path) -> FakeImageLease:
            choices = {
                bad_manifest: lease,
                engine: engine_lease,
                module: module_lease,
            }
            selected = choices[path]
            opened.append(selected)
            return selected

        argv = ["collect_lifecycle.py", "--engine", str(engine), "--module", INCLUDED,
                "--module-image", str(module), "--working-directory", str(root),
                "--rhi-backend", "d3d11", "--image-manifest", str(bad_manifest),
                "--out", str(out), "--commit-sha", self.sha]
        output_operations = self._fake_output_publication_operations(out)
        with mock.patch.object(sys, "argv", argv), \
             mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True), \
             mock.patch("collect_lifecycle.REPO_ROOT", self.repo), \
             mock.patch("collect_lifecycle.resolve_head_sha", return_value=(self.sha, None)), \
             mock.patch.dict("collect_lifecycle.os.environ", {"GITHUB_SHA": self.sha}, clear=False), \
             mock.patch("collect_lifecycle.lifecycle_mod.source_tree_sha",
                        return_value=("c" * 40, None)), \
             mock.patch("collect_lifecycle.open_image_lease", side_effect=lease_factory), \
             mock.patch("collect_lifecycle.subprocess.run") as child, \
             mock.patch("collect_lifecycle._output_publication_operations_for_main",
                        return_value=output_operations):
             self.assertEqual(collect_lifecycle.main(), 1)
        child.assert_not_called()
        self.assertEqual(opened, [lease, engine_lease, module_lease])
        self.assertTrue(all(item.closed for item in opened))
        self.assertFalse(out.exists())

    @unittest.skipUnless(os.name == "nt", "stable-v1 collector path contract is Windows-only")
    def test_run_engine_uses_canonical_paths_for_relative_arguments(self) -> None:
        from collect_lifecycle import run_engine
        root = self.repo / "relative-package"
        root.mkdir()
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        completed = subprocess.CompletedProcess([], 0, self.VALID_RECORD, "")
        previous = Path.cwd()
        try:
            os.chdir(self.repo)
            digest = hashlib.sha256(pe_image()).hexdigest()
            with mock.patch("collect_lifecycle.subprocess.run", return_value=completed) as run:
                _, error = run_engine(Path("relative-package/SparkEngine.exe"),
                                       Path("relative-package/SparkGameFPS.dll"),
                                       INCLUDED, Path("relative-package"), "d3d11", 30,
                                       expected_digests=(digest, digest))
        finally:
            os.chdir(previous)
        self.assertIsNone(error)
        self.assertEqual(run.call_args.args[0][0], str(engine.resolve()))
        self.assertEqual(run.call_args.args[0][2], str(module.resolve()))
        self.assertEqual(run.call_args.kwargs["cwd"], str(root.resolve()))

    def test_rejects_raw_root_and_intermediate_reparse_paths(self) -> None:
        from collect_lifecycle import validate_image_pair
        root = self.repo / "reparse-package"
        root.mkdir()
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        root_link = self.repo / "reparse-root-link"
        inner = root / "real-inner"
        inner.mkdir()
        (inner / "SparkEngine.exe").write_bytes(pe_image())
        (inner / "SparkGameFPS.dll").write_bytes(pe_image())
        inner_link = root / "inner-link"
        try:
            os.symlink(str(root), str(root_link), target_is_directory=True)
            os.symlink(str(inner), str(inner_link), target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("cannot create symlinks on this platform")
        self.assertIsNotNone(validate_image_pair(root_link / "SparkEngine.exe",
                                                  root_link / "SparkGameFPS.dll",
                                                  INCLUDED, root_link))
        self.assertIsNotNone(validate_image_pair(inner_link / "SparkEngine.exe",
                                                  inner_link / "SparkGameFPS.dll",
                                                  INCLUDED, root))

    def test_run_engine_rejects_image_mutation_after_launch(self) -> None:
        """A manifest digest that changes after launch is rejected through the leases."""
        from collect_lifecycle import run_engine
        root = self.repo / "mutation-package"
        root.mkdir()
        engine = root / "SparkEngine.exe"
        module = root / "SparkGameFPS.dll"
        engine.write_bytes(pe_image())
        module.write_bytes(pe_image())
        engine_digest = "a" * 64
        module_digest = "b" * 64
        engine_identity = FakeLeaseIdentity(15, 501, len(pe_image()))
        module_identity = FakeLeaseIdentity(15, 502, len(pe_image()))
        leases = [
            FakeImageLease(engine, identity=engine_identity, digest=engine_digest),
            FakeImageLease(module, identity=module_identity, digest=module_digest),
            FakeImageLease(engine, identity=engine_identity, digest="c" * 64),
            FakeImageLease(module, identity=module_identity, digest=module_digest),
        ]
        opened: list[FakeImageLease] = []

        def factory(path: Path) -> FakeImageLease:
            lease = leases[len(opened)]
            self.assertEqual(path, lease.path)
            opened.append(lease)
            return lease

        completed = subprocess.CompletedProcess([], 0, self.VALID_RECORD, "")
        with mock.patch("collect_lifecycle.subprocess.run", return_value=completed), \
             mock.patch("collect_lifecycle._stable_v1_windows_platform", return_value=True):
            _, error = run_engine(
                engine, module, INCLUDED, root, "d3d11", 30,
                expected_digests=(engine_digest, module_digest), lease_factory=factory,
            )
        self.assertIn("changed", error or "")
        self.assertTrue(all(lease.closed for lease in opened))

    def test_main_clears_stale_output_after_post_validation_run_or_write_failure(self) -> None:
        """Once all paths/manifests validate, later failures remove stale evidence."""
        out, argv = self._collector_main_fixture("stale-post-validation")
        log = out.parent / "module-lifecycle-SparkGameFPS.log"
        for name in ("run-engine", "direct-write"):
            out.write_text("stale-json", encoding="utf-8")
            log.write_text("stale-log", encoding="utf-8")
            with self.subTest(failure=name):
                if name == "run-engine":
                    self.assertEqual(self._run_truthful_collector_main(
                        argv,
                        mock.patch(
                            "collect_lifecycle.run_engine",
                            return_value=(None, "injected post-validation engine failure"),
                        ),
                    ), 1)
                else:
                    base = self._fake_output_publication_operations(out)

                    def fail_write(directory, final_path, content):
                        raise OSError("injected direct output write failure")

                    self.assertEqual(self._run_truthful_collector_main(
                        argv, output_operations=replace(base, write=fail_write),
                    ), 1)
            self.assertFalse(out.exists())
            self.assertFalse(log.exists())

    def test_digest_rejects_trailing_newline(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["records"][0]["moduleSHA256"] = "b" * 64 + "\n"
        errors = self.assertRejected(base_manifest(), "digest-newline",
                                     lifecycle_evidence=ev)
        self.assertTrue(any("moduleSHA256" in error for error in errors))


# --------------------------------------------------------------------------
# B11-B17 — evidence bindings must name real producers
# --------------------------------------------------------------------------
class TestEvidenceBindings(FixtureCase):

    def test_B11_binding_to_a_never_produced_artifact_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][2]["artifactPattern"] = \
            "build/never-produced.xml"
        errors = self.assertRejected(m, "B11")
        self.assertTrue(any("producer" in e for e in errors), errors)

    def test_B12_included_module_with_no_bindings_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"] = []
        self.assertRejected(m, "B12")

    def test_B12b_each_required_evidence_type_is_individually_required(self) -> None:
        for i in range(4):
            with self.subTest(dropped=i):
                m = base_manifest()
                del m["modules"][0]["evidenceBindings"][i]
                self.assertRejected(m, f"B12b-{i}")

    def test_B13_included_module_without_package_smoke_owner_is_rejected(self) -> None:
        m = base_manifest()
        del m["modules"][0]["packageSmokeOwner"]
        self.assertRejected(m, "B13")

    def test_B14_invented_package_smoke_owner_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["packageSmokeOwner"] = "TOTALLY-FAKE-999"
        self.assertRejected(m, "B14")

    def test_B14b_well_formed_but_unknown_owner_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["packageSmokeOwner"] = "ZZZ-999"
        errors = self.assertRejected(m, "B14b")
        self.assertTrue(any("not a readiness work item" in e for e in errors), errors)

    def test_B15_currently_unproduced_lifecycle_log_path_is_rejected(self) -> None:
        """The historical binding to build/module-lifecycle-<mod>.log."""
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][1]["artifactPattern"] = \
            f"build/module-lifecycle-{INCLUDED}.log"
        self.assertRejected(m, "B15")

    def test_B16_binding_with_unknown_keys_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][0]["backdoor"] = "accepted"
        self.assertRejected(m, "B16")

    def test_B17_duplicate_binding_type_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"].append(
            copy.deepcopy(m["modules"][0]["evidenceBindings"][0]))
        self.assertRejected(m, "B17")

    def test_unknown_evidence_type_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][0]["type"] = "vibes"
        self.assertRejected(m, "B11b")

    def test_glob_artifact_pattern_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][0]["artifactPattern"] = "build/*.json"
        self.assertRejected(m, "B11c")

    def test_traversal_artifact_pattern_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["evidenceBindings"][0]["artifactPattern"] = \
            "build/../../etc/shadow"
        self.assertRejected(m, "B11d")

    def test_every_evidence_type_names_a_producer(self) -> None:
        for etype, producer in EVIDENCE_PRODUCERS.items():
            with self.subTest(type=etype):
                for key in ("producer", "definedIn", "artifact", "ciJob"):
                    self.assertTrue(producer.get(key),
                                    f"{etype} declares no {key}")


# --------------------------------------------------------------------------
# B18-B21 — strict, duplicate-aware, bounded JSON
# --------------------------------------------------------------------------
class TestStrictJSON(unittest.TestCase):

    def test_B18_duplicate_property_is_rejected(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"schemaVersion":"evil","schemaVersion":"stable-v2"}')

    def test_B18b_duplicate_property_in_nested_object_is_rejected(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"a":{"b":1,"b":2}}')

    def test_B18c_duplicate_property_inside_array_element_is_rejected(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"a":[{"b":1,"b":2}]}')

    def test_B19_nan_and_infinity_literals_are_rejected(self) -> None:
        for literal in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(literal=literal):
                with self.assertRaises(strict_json.StrictJSONError):
                    strict_json.loads('{"a":%s}' % literal)

    def test_B19b_overflowing_float_is_rejected(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"a":1e400}')

    def test_depth_limit_is_enforced(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads("[" * 40 + "]" * 40)

    def test_document_size_limit_is_enforced(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"a":"' + "x" * (600 * 1024) + '"}')

    def test_container_item_limit_is_enforced(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads(
                json.dumps(
                    {"a": list(range(strict_json.DEFAULT_LIMITS.container_items + 1))}
                )
            )

    def test_module_target_container_item_limit_is_enforced(self) -> None:
        """The File API exception remains bounded for hostile artifacts."""
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads(
                json.dumps(
                    {
                        "sources": list(
                            range(strict_json.MODULE_TARGET_LIMITS.container_items + 1)
                        )
                    }
                ),
                limits=strict_json.MODULE_TARGET_LIMITS,
            )

    def test_string_length_limit_is_enforced(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads(json.dumps({"a": "x" * 600}))

    def test_node_count_limit_is_enforced(self) -> None:
        limits = strict_json.Limits(total_nodes=10)
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads(json.dumps({"a": list(range(50))}), limits=limits)

    def test_valid_document_is_accepted(self) -> None:
        self.assertEqual(strict_json.loads('{"a":[1,2,{"b":"x"}]}'),
                         {"a": [1, 2, {"b": "x"}]})

    def test_contract_limits_still_reject_duplicates(self) -> None:
        with self.assertRaises(strict_json.StrictJSONError):
            strict_json.loads('{"a":1,"a":2}', limits=strict_json.CONTRACT_LIMITS)

    def test_readiness_contract_parses_under_contract_limits(self) -> None:
        """The relaxed limits must actually admit the real contract."""
        document = strict_json.load_file(
            REPO_ROOT / "docs" / "site" / "readiness.json",
            limits=strict_json.CONTRACT_LIMITS,
        )
        self.assertIn("releaseProfiles", document)


class TestNestedUnknownKeys(FixtureCase):

    def test_B20_profile_with_unknown_keys_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["backdoor"] = "accepted"
        self.assertRejected(m, "B20")

    def test_B21_experimental_separation_unknown_keys_are_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][1]["experimentalSeparation"]["backdoor"] = "accepted"
        self.assertRejected(m, "B21")

    def test_top_level_unknown_keys_are_rejected(self) -> None:
        m = base_manifest()
        m["backdoor"] = "accepted"
        self.assertRejected(m, "B20b")

    def test_module_unknown_keys_are_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["backdoor"] = "accepted"
        self.assertRejected(m, "B20c")

    def test_self_attested_revision_keys_are_rejected(self) -> None:
        """A committed manifest may not claim its own commit or time."""
        for key, value in (("commitSHA", "a" * 40),
                           ("generatedAt", "2026-08-28T00:00:00Z")):
            with self.subTest(key=key):
                m = base_manifest()
                m[key] = value
                self.assertRejected(m, f"B33-{key}")

    def test_unknown_shared_library_platform_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["sharedLibrary"]["haiku"] = "libX.so"
        self.assertRejected(m, "B20d")


# --------------------------------------------------------------------------
# B22-B27 — the profile partition
# --------------------------------------------------------------------------
class TestProfilePartition(FixtureCase):

    def test_B22_unclassified_module_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["profiles"][0]["excludedModules"] = []
        errors = self.assertRejected(m, "B22")
        self.assertTrue(any("neither included nor excluded" in e for e in errors))

    def test_B23_duplicate_in_included_list_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["includedModules"] = [INCLUDED, INCLUDED]
        self.assertRejected(m, "B23")

    def test_B23b_duplicate_in_excluded_list_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["profiles"][0]["excludedModules"] = [DECOY, DECOY]
        self.assertRejected(m, "B23b")

    def test_B24_empty_included_list_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["includedModules"] = []
        m["profiles"][0]["excludedModules"] = [INCLUDED]
        m["modules"][0]["profileApplicability"] = {PROFILE: "outside"}
        m["modules"][0]["experimentalSeparation"] = {"trackedUnder": "RDY-015"}
        errors = self.assertRejected(m, "B24")
        self.assertTrue(any("empty" in e for e in errors), errors)

    def test_B25_empty_profile_id_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["id"] = ""
        m["modules"][0]["profileApplicability"] = {"": "required"}
        self.assertRejected(m, "B25")

    def test_B25b_unknown_profile_id_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["id"] = "invented-profile"
        m["modules"][0]["profileApplicability"] = {"invented-profile": "required"}
        errors = self.assertRejected(m, "B25b")
        self.assertTrue(any("readiness.json" in e for e in errors), errors)

    def test_B26_extra_profile_applicability_key_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["profileApplicability"]["invented-profile"] = "required"
        self.assertRejected(m, "B26")

    def test_B27_missing_profile_applicability_key_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["profileApplicability"] = {"invented-profile": "required"}
        self.assertRejected(m, "B27")

    def test_empty_profile_applicability_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["profileApplicability"] = {}
        self.assertRejected(m, "B27b")

    def test_module_in_both_lists_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["excludedModules"] = [INCLUDED]
        self.assertRejected(m, "B22b")

    def test_reference_to_undeclared_module_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"][0]["excludedModules"] = ["SparkGamePhantom"]
        self.assertRejected(m, "B22c")

    def test_duplicate_profile_ids_are_rejected(self) -> None:
        m = base_manifest()
        m["profiles"].append(copy.deepcopy(m["profiles"][0]))
        self.assertRejected(m, "B25c")

    def test_no_profiles_is_rejected(self) -> None:
        m = base_manifest()
        m["profiles"] = []
        self.assertRejected(m, "B24b")

    def test_included_module_declaring_itself_outside_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["profileApplicability"] = {PROFILE: "outside"}
        self.assertRejected(m, "B22d")

    def test_excluded_module_declaring_itself_required_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][1]["profileApplicability"] = {PROFILE: "required"}
        self.assertRejected(m, "B22e")


# --------------------------------------------------------------------------
# B28-B32 — cross-referenced identities
# --------------------------------------------------------------------------
class TestCrossReferences(FixtureCase):

    def test_B28_dependency_on_unknown_module_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["dependencies"] = ["NoSuchModule"]
        self.assertRejected(m, "B28")

    def test_B29_duplicate_dependencies_are_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][0]["dependencies"] = [DECOY, DECOY]
        self.assertRejected(m, "B29")

    def test_B29b_self_dependency_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["dependencies"] = [INCLUDED]
        self.assertRejected(m, "B29b")

    def test_B30_unrelated_dll_identity_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["sharedLibrary"]["windows"] = "Completely-Unrelated.dll"
        errors = self.assertRejected(m, "B30")
        self.assertTrue(any("may not claim a library identity" in e for e in errors))

    def test_B30b_wrong_platform_convention_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["sharedLibrary"]["linux"] = f"{INCLUDED}.so"
        self.assertRejected(m, "B30b")

    def test_B31_windows_case_colliding_libraries_are_rejected(self) -> None:
        """SparkGameFPS.dll and SparkGameFps.dll are one file on Windows.

        Both modules here are individually well formed — each name matches the
        naming rules and each library follows the platform convention — so
        only the collision check can catch them.  The assertion names that
        specific error rather than accepting any failure, because the
        case-variant directory does not exist on a case-insensitive host and
        would otherwise mask the collision behind a path error.
        """
        variant = "SparkGameFps"
        m = base_manifest(with_decoy=True)
        m["modules"][1]["name"] = variant
        m["modules"][1]["cmakeTarget"] = variant
        m["modules"][1]["sharedLibrary"] = {
            "windows": expected_library_names(variant)["windows"],
            "linux": expected_library_names(variant)["linux"],
        }
        m["modules"][1]["sourceDirectory"] = f"GameModules/{variant}/Source"
        m["profiles"][0]["excludedModules"] = [variant]
        errors = self.assertRejected(m, "B31")
        self.assertTrue(
            any("same file" in e for e in errors),
            f"[B31] case-colliding library identities were not detected: {errors}",
        )

    def test_B31b_case_colliding_module_names_are_rejected(self) -> None:
        variant = "SparkGameFps"
        m = base_manifest(with_decoy=True)
        m["modules"][1]["name"] = variant
        m["modules"][1]["cmakeTarget"] = variant
        m["modules"][1]["sharedLibrary"] = {
            "windows": expected_library_names(variant)["windows"],
            "linux": expected_library_names(variant)["linux"],
        }
        m["modules"][1]["sourceDirectory"] = f"GameModules/{variant}/Source"
        m["profiles"][0]["excludedModules"] = [variant]
        errors = self.assertRejected(m, "B31b")
        self.assertTrue(
            any("module name" in e and "collides" in e for e in errors),
            f"[B31b] case-colliding module names were not detected: {errors}",
        )

    def test_B32_invented_tracker_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][1]["experimentalSeparation"]["trackedUnder"] = "NOT-A-TRACKER-42"
        self.assertRejected(m, "B32")

    def test_B32b_well_formed_unknown_tracker_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][1]["experimentalSeparation"]["trackedUnder"] = "ZZZ-999"
        self.assertRejected(m, "B32b")

    def test_B32c_outside_module_without_tracker_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        del m["modules"][1]["experimentalSeparation"]
        self.assertRejected(m, "B32c")

    def test_cmake_target_differing_from_name_is_rejected(self) -> None:
        m = base_manifest()
        m["modules"][0]["cmakeTarget"] = "SomethingElse"
        self.assertRejected(m, "B30c")

    def test_duplicate_module_names_are_rejected(self) -> None:
        m = base_manifest()
        m["modules"].append(copy.deepcopy(m["modules"][0]))
        self.assertRejected(m, "B31c")

    def test_experimental_module_required_in_profile_is_rejected(self) -> None:
        m = base_manifest(with_decoy=True)
        m["modules"][1]["profileApplicability"] = {PROFILE: "shared"}
        m["profiles"][0]["includedModules"] = [INCLUDED, DECOY]
        m["profiles"][0]["excludedModules"] = []
        self.assertRejected(m, "B32d")


# --------------------------------------------------------------------------
# B33-B36 — revision and time provenance
# --------------------------------------------------------------------------
class TestProvenance(FixtureCase):

    def test_B33_arbitrary_hex_sha_is_not_a_revision(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["commitSHA"] = "a" * 40
        errors = self.assertRejected(base_manifest(), "B33",
                                     lifecycle_evidence=ev, expected_sha="a" * 40)
        self.assertTrue(any("not an object in this repository" in e for e in errors),
                        errors)

    def test_B34_real_but_unrelated_sha_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["commitSHA"] = "0" * 39 + "1"
        self.assertRejected(base_manifest(), "B34", lifecycle_evidence=ev)

    def test_B34b_evidence_from_a_different_commit_is_rejected(self) -> None:
        """Shape-valid, object-valid, but not the revision under test."""
        (self.repo / "unrelated.txt").write_text("x", encoding="utf-8")
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", "second")
        other = _git(self.repo, "rev-parse", "HEAD").stdout.strip()
        try:
            ev = lifecycle_evidence(self.repo, self.sha)
            ev["commitSHA"] = other
            errors = self.assertRejected(base_manifest(), "B34b",
                                         lifecycle_evidence=ev)
            self.assertTrue(any("stale evidence" in e for e in errors), errors)
        finally:
            _git(self.repo, "reset", "-q", "--hard", self.sha)

    def test_target_index_from_a_different_commit_is_rejected(self) -> None:
        """A replayed configure index cannot pair with current lifecycle proof."""
        (self.repo / "target-index-replay.txt").write_text("new revision", encoding="utf-8")
        _git(self.repo, "add", "target-index-replay.txt")
        _git(self.repo, "commit", "-q", "-m", "target index replay revision")
        current = _git(self.repo, "rev-parse", "HEAD").stdout.strip()
        try:
            stale_index = target_index()
            stale_index.update({
                "commitSHA": self.sha,
                "generatedAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                "source": "test",
            })
            errors = self.assertRejected(
                base_manifest(), "target-index-stale-replay",
                target_index=stale_index,
                lifecycle_evidence=lifecycle_evidence(self.repo, current),
                expected_sha=current,
            )
            self.assertTrue(any("target evidence commitSHA" in error for error in errors), errors)
            self.assertTrue(any("stale evidence" in error for error in errors), errors)
        finally:
            _git(self.repo, "reset", "-q", "--hard", self.sha)

    def test_B35_null_generated_at_is_rejected(self) -> None:
        ev = lifecycle_evidence(self.repo, self.sha)
        ev["generatedAt"] = None
        errors = self.assertRejected(base_manifest(), "B35", lifecycle_evidence=ev)
        self.assertTrue(any("null" in e for e in errors), errors)

    def test_B36_non_timestamp_generated_at_is_rejected(self) -> None:
        for value in ("whenever", "2026-08-28", "2026-08-28T00:00:00",
                      "not-a-date", "", "2026-13-45T99:99:99Z"):
            with self.subTest(value=value):
                ev = lifecycle_evidence(self.repo, self.sha)
                ev["generatedAt"] = value
                self.assertRejected(base_manifest(), "B36",
                                    lifecycle_evidence=ev)

    def test_naive_timestamp_without_offset_is_rejected(self) -> None:
        self.assertNotEqual(
            provenance.check_rfc3339("2026-08-28T00:00:00", "t"), [])

    def test_far_future_timestamp_is_rejected(self) -> None:
        self.assertNotEqual(
            provenance.check_rfc3339("2099-01-01T00:00:00Z", "t"), [])

    def test_prehistoric_timestamp_is_rejected(self) -> None:
        self.assertNotEqual(
            provenance.check_rfc3339("1999-01-01T00:00:00Z", "t"), [])

    def test_valid_rfc3339_forms_are_accepted(self) -> None:
        for value in ("2026-08-28T07:01:00Z", "2026-08-28T07:01:00+00:00",
                      "2026-08-28T07:01:00.123Z", "2026-08-28T07:01:00-05:00"):
            with self.subTest(value=value):
                self.assertEqual(provenance.check_rfc3339(value, "t"), [])

    def test_missing_expected_sha_blocks(self) -> None:
        errors = self.assertRejected(base_manifest(), "B33b", expected_sha=None)
        self.assertTrue(any("revision under test" in e for e in errors), errors)

    def test_head_sha_resolves_in_a_real_checkout(self) -> None:
        sha, err = provenance.resolve_head_sha(self.repo)
        self.assertIsNone(err)
        self.assertEqual(sha, self.sha)

    def test_sha_shape_rejects_non_hex_and_short_values(self) -> None:
        for value in ("", "abc", "A" * 40, "g" * 40, None, 12345, "a" * 41):
            with self.subTest(value=value):
                self.assertNotEqual(provenance.check_sha_shape(value, "s"), [])


# --------------------------------------------------------------------------
# Registry integrity and the shipped contract
# --------------------------------------------------------------------------
class TestRegistryIntegrity(FixtureCase):

    def test_unreadable_profile_registry_blocks(self) -> None:
        (self.repo / "docs" / "site" / "readiness.json").unlink()
        try:
            errors = self.assertRejected(base_manifest(), "REG1")
            self.assertTrue(any("release profile registry unavailable" in e
                                for e in errors), errors)
        finally:
            (self.repo / "docs" / "site" / "readiness.json").write_text(
                json.dumps({"releaseProfiles": [{"id": PROFILE}]}), encoding="utf-8")

    def test_unreadable_work_item_registry_blocks(self) -> None:
        path = self.repo / "docs" / "readiness" / "work-items" / "00-truth.json"
        saved = path.read_text(encoding="utf-8")
        path.unlink()
        try:
            errors = self.assertRejected(base_manifest(), "REG2")
            self.assertTrue(any("work item registry unavailable" in e
                                for e in errors), errors)
        finally:
            path.write_text(saved, encoding="utf-8")

    def test_shipped_owner_and_tracker_exist_in_the_real_contract(self) -> None:
        from schema import load_known_work_item_ids
        ids, err = load_known_work_item_ids(REPO_ROOT)
        self.assertIsNone(err, f"real work item registry unreadable: {err}")
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json")
        for module in manifest["modules"]:
            owner = module.get("packageSmokeOwner")
            if owner is not None:
                self.assertIn(owner, ids, f"{module['name']} owner {owner}")
            sep = module.get("experimentalSeparation")
            if sep:
                self.assertIn(sep["trackedUnder"], ids)

    def test_shipped_profile_exists_in_the_real_contract(self) -> None:
        from schema import load_known_profile_ids
        ids, err = load_known_profile_ids(REPO_ROOT)
        self.assertIsNone(err)
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json")
        for profile in manifest["profiles"]:
            self.assertIn(profile["id"], ids)

    def test_shipped_source_directories_exist_with_exact_case(self) -> None:
        manifest = load_manifest(
            REPO_ROOT / "tools" / "module-evidence" / "manifest.json")
        for module in manifest["modules"]:
            with self.subTest(module=module["name"]):
                self.assertEqual(
                    paths_mod.check_source_directory(
                        module["sourceDirectory"], module["name"], REPO_ROOT),
                    [])

    def test_manifest_is_not_an_object_is_rejected(self) -> None:
        self.assertNotEqual(ManifestValidator([], self.repo).validate(), [])

    def test_wrong_schema_version_is_rejected(self) -> None:
        m = base_manifest()
        m["schemaVersion"] = "stable-v1"
        self.assertRejected(m, "SV1")

    def test_module_count_and_collection_bounds(self) -> None:
        m = base_manifest()
        m["modules"] = [module_entry(INCLUDED, included=True)] * 200
        self.assertRejected(m, "BND1")


# --------------------------------------------------------------------------
# CMake File API — parsed from a real reply, and from a real configure
# --------------------------------------------------------------------------
class TestEvidenceGapLedger(FixtureCase):
    """The one softening in the validator, and the ratchet that bounds it."""

    def _ledger(self, gaps: list[dict[str, Any]]) -> Path:
        path = self.repo / "gaps.json"
        path.write_text(json.dumps(
            {"schemaVersion": "evidence-gaps-v1", "gaps": gaps}), encoding="utf-8")
        return path

    def _load(self, gaps: list[dict[str, Any]]) -> dict[str, str]:
        from validate_manifest import load_declared_gaps
        return load_declared_gaps(self._ledger(gaps),
                                  {"RDY-010", "RDY-015", "MOD-310"})

    def _valid_gap(self, etype: str = "lifecycle-log") -> dict[str, Any]:
        return {
            "evidenceType": etype,
            "trackedUnder": "RDY-010",
            "reason": "x" * 60,
        }

    def test_declared_gap_downgrades_absence_to_a_warning(self) -> None:
        v = ManifestValidator(
            base_manifest(), self.repo,
            target_index=target_index_for_revision(self.sha),
            lifecycle_evidence=None, lifecycle_error="absent",
            expected_sha=self.sha,
            declared_gaps={"lifecycle-log": "RDY-010"},
        )
        self.assertEqual(v.validate(), [])
        self.assertTrue(any("lifecycle-log" in w for w in v.warnings), v.warnings)

    def test_undeclared_gap_is_still_a_hard_failure(self) -> None:
        v = ManifestValidator(
            base_manifest(), self.repo,
            target_index=None, target_error="absent",
            lifecycle_evidence=lifecycle_evidence(self.repo, self.sha),
            expected_sha=self.sha,
            declared_gaps={"lifecycle-log": "RDY-010"},
        )
        self.assertNotEqual(v.validate(), [])

    def test_ratchet_declared_gap_whose_evidence_appeared_fails(self) -> None:
        """The ledger may only shrink."""
        v = ManifestValidator(
            base_manifest(), self.repo,
            target_index=target_index_for_revision(self.sha),
            lifecycle_evidence=lifecycle_evidence(self.repo, self.sha),
            expected_sha=self.sha,
            declared_gaps={"lifecycle-log": "RDY-010"},
        )
        errors = v.validate()
        self.assertTrue(any("evidence is now available" in e for e in errors),
                        errors)

    def test_missing_artifact_evidence_is_rejected(self) -> None:
        artifact = self.repo / EVIDENCE_PRODUCERS["junit-xml"]["artifact"]
        saved = artifact.read_text(encoding="utf-8")
        artifact.unlink()
        try:
            errors = self.assertRejected(base_manifest(), "ART1")
            self.assertTrue(any("was not produced" in e for e in errors), errors)
        finally:
            artifact.write_text(saved, encoding="utf-8")

    def test_ledger_rejects_unknown_evidence_type(self) -> None:
        from validate_manifest import ManifestError
        with self.assertRaises(ManifestError):
            self._load([{**self._valid_gap(), "evidenceType": "vibes"}])

    def test_ledger_rejects_untracked_gap(self) -> None:
        from validate_manifest import ManifestError
        with self.assertRaises(ManifestError):
            self._load([{**self._valid_gap(), "trackedUnder": "ZZZ-999"}])

    def test_ledger_rejects_malformed_tracker(self) -> None:
        from validate_manifest import ManifestError
        with self.assertRaises(ManifestError):
            self._load([{**self._valid_gap(), "trackedUnder": "whatever"}])

    def test_ledger_rejects_unexplained_gap(self) -> None:
        from validate_manifest import ManifestError
        with self.assertRaises(ManifestError):
            self._load([{**self._valid_gap(), "reason": "because"}])

    def test_ledger_rejects_duplicate_and_unknown_keys(self) -> None:
        from validate_manifest import ManifestError
        with self.assertRaises(ManifestError):
            self._load([self._valid_gap(), self._valid_gap()])
        with self.assertRaises(ManifestError):
            self._load([{**self._valid_gap(), "backdoor": "accepted"}])

    def test_ledger_rejects_wrong_schema_version(self) -> None:
        from validate_manifest import ManifestError, load_declared_gaps
        path = self.repo / "bad-gaps.json"
        path.write_text(json.dumps({"schemaVersion": "v9", "gaps": []}),
                        encoding="utf-8")
        with self.assertRaises(ManifestError):
            load_declared_gaps(path, {"RDY-010"})

    def test_valid_ledger_loads(self) -> None:
        self.assertEqual(self._load([self._valid_gap()]),
                         {"lifecycle-log": "RDY-010"})

    def test_shipped_ledger_is_valid_and_nonempty(self) -> None:
        from schema import load_known_work_item_ids
        from validate_manifest import load_declared_gaps
        ids, err = load_known_work_item_ids(REPO_ROOT)
        self.assertIsNone(err)
        gaps = load_declared_gaps(
            REPO_ROOT / "tools" / "module-evidence" / "evidence-gaps.json", ids)
        self.assertNotEqual(gaps, {},
                            "an empty ledger would mean RDY-010 is closeable")
        self.assertIn("lifecycle-log", gaps)


class TestCIWiring(unittest.TestCase):
    """The gate is only a gate if CI actually runs it, blockingly.

    Parsed as text rather than YAML so the check needs no dependency the CI
    runner does not already have.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = (REPO_ROOT / ".github" / "workflows" / "build.yml").read_text(
            encoding="utf-8")

    def _job_block(self, name: str) -> str:
        start = self.workflow.index(f"\n  {name}:\n")
        rest = self.workflow[start + 1:]
        lines = rest.split("\n")
        out = [lines[0]]
        for line in lines[1:]:
            if line and not line.startswith("   ") and not line.startswith("  #"):
                if line.startswith("  ") and line.rstrip().endswith(":"):
                    break
            out.append(line)
        return "\n".join(out)

    def test_module_evidence_job_exists(self) -> None:
        self.assertIn("\n  module-evidence:\n", self.workflow)

    def test_module_evidence_is_a_required_gate(self) -> None:
        gate = self.workflow[self.workflow.index("\n  required-ci-gate:\n"):]
        needs = gate[gate.index("needs:"):gate.index("runs-on:")]
        self.assertIn("- module-evidence", needs,
                      "module-evidence is not in required-ci-gate needs")

    def test_module_evidence_is_not_continue_on_error(self) -> None:
        self.assertNotIn("continue-on-error", self._job_block("module-evidence"))

    def test_module_evidence_runs_a_real_configure(self) -> None:
        block = self._job_block("module-evidence")
        self.assertIn("collect_targets.py", block)
        self.assertIn("-DBUILD_GAME_MODULES=ON", block)

    def test_module_evidence_gate_is_not_policy_only(self) -> None:
        """A --policy-only run must never be the release gate."""
        block = self._job_block("module-evidence")
        self.assertIn("--allow-declared-gaps", block)
        self.assertNotIn("--policy-only", block)

    def test_module_evidence_binds_the_revision_under_test(self) -> None:
        self.assertIn("--expected-sha", self._job_block("module-evidence"))

    def test_module_evidence_uses_an_explicit_absolute_workspace_root(self) -> None:
        """The release gate must not rely on POSIX `getcwd()` for root authority."""
        block = self._job_block("module-evidence")
        self.assertIn('--repo-root "$GITHUB_WORKSPACE"', block)
        self.assertNotIn("--repo-root .", block)
        self.assertNotIn("--target-evidence", block)

    def test_gate_consumes_really_produced_junit_evidence(self) -> None:
        block = self._job_block("module-evidence")
        self.assertIn("download-artifact", block)
        self.assertIn("test-results-linux-gcc-Release", block)

    def test_every_gap_tracker_is_a_real_work_item(self) -> None:
        from schema import load_known_work_item_ids
        ids, err = load_known_work_item_ids(REPO_ROOT)
        self.assertIsNone(err)
        gaps = json.loads(
            (REPO_ROOT / "tools" / "module-evidence" / "evidence-gaps.json")
            .read_text(encoding="utf-8"))["gaps"]
        for gap in gaps:
            with self.subTest(gap=gap["evidenceType"]):
                self.assertIn(gap["trackedUnder"], ids)

    def test_action_pins_are_shared_with_the_rest_of_the_workflow(self) -> None:
        """A pin used once is a pin nobody reviewed."""
        import re
        block = self._job_block("module-evidence")
        for action, pin in re.findall(r"uses: (actions/[\w-]+)@([a-f0-9]{40})", block):
            with self.subTest(action=action):
                self.assertGreaterEqual(
                    self.workflow.count(f"{action}@{pin}"), 2,
                    f"{action}@{pin} appears only in the module-evidence job; "
                    f"reuse the pin the rest of the workflow already uses",
                )


class TestTargetContainment(FixtureCase):
    """Adversarial tests for target evidence containment, provenance, and
    strict JSON parsing — attacks exposed by rdy010_target_adversarial_probe.py."""

    def _write_reply(self, reply: Path, *, codemodel_json_file: str = "codemodel-v2-abc.json",
                     target_json_file: str = "target-X.json",
                     target_content: str | None = None,
                     codemodel_content: str | None = None,
                     include_shared_fallback: bool = False) -> None:
        """Build a minimal File API reply directory."""
        if target_content is None:
            target_content = json.dumps({
                "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                "nameOnDisk": "libSparkGameFPS.so",
                "paths": {"source": "GameModules/SparkGameFPS"},
                "sources": [{"path": "GameModules/SparkGameFPS/Source/Main.cpp"}],
                "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
            })
        if codemodel_content is None:
            codemodel_content = json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS", "jsonFile": target_json_file}],
                }]
            })
        reply.mkdir(parents=True, exist_ok=True)
        (reply / target_json_file).write_text(target_content, encoding="utf-8")
        (reply / codemodel_json_file).write_text(codemodel_content, encoding="utf-8")
        (reply / "index-1.json").write_text(json.dumps({
            "reply": {targets_mod.CLIENT_NAME: {"query.json": {
                "responses": [{"kind": "codemodel",
                               "jsonFile": codemodel_json_file}]
            }}}
        }), encoding="utf-8")
        if include_shared_fallback:
            (reply / "codemodel-v2-shared.json").write_text(
                codemodel_content, encoding="utf-8")

    def test_path_traversal_in_codemodel_jsonfile_is_rejected(self) -> None:
        """A jsonFile with ../ escaping the reply directory must be blocked."""
        with tempfile.TemporaryDirectory(prefix="spark-traversal-") as tmp:
            reply = Path(tmp) / "reply"
            reply.mkdir()
            forged = Path(tmp) / "forged-codemodel.json"
            forged.write_text(json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS", "jsonFile": "target-X.json"}],
                }]
            }), encoding="utf-8")
            (reply / "target-X.json").write_text(json.dumps({
                "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                "nameOnDisk": "libSparkGameFPS.so",
                "paths": {"source": "GameModules/SparkGameFPS"},
                "sources": [{"path": "GameModules/SparkGameFPS/Source/Main.cpp"}],
                "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
            }), encoding="utf-8")
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {targets_mod.CLIENT_NAME: {"query.json": {
                    "responses": [{"kind": "codemodel",
                                   "jsonFile": "../forged-codemodel.json"}]
                }}}
            }), encoding="utf-8")
            with self.assertRaises(targets_mod.TargetEvidenceUnavailable) as cm:
                targets_mod.extract_from_reply(reply)
            self.assertIn("path traversal", str(cm.exception).lower())

    def test_path_traversal_in_target_jsonfile_is_rejected(self) -> None:
        """A target jsonFile reference escaping the reply dir must be blocked."""
        with tempfile.TemporaryDirectory(prefix="spark-traversal-") as tmp:
            reply = Path(tmp) / "reply"
            reply.mkdir()
            forged_target = Path(tmp) / "forged-target.json"
            forged_target.write_text(json.dumps({
                "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                "nameOnDisk": "libSparkGameFPS.so",
                "paths": {"source": "GameModules/SparkGameFPS"},
                "sources": [{"path": "GameModules/SparkGameFPS/Source/Main.cpp"}],
                "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
            }), encoding="utf-8")
            codemodel = reply / "codemodel-v2-abc.json"
            codemodel.write_text(json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS",
                                 "jsonFile": "../forged-target.json"}],
                }]
            }), encoding="utf-8")
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {targets_mod.CLIENT_NAME: {"query.json": {
                    "responses": [{"kind": "codemodel",
                                   "jsonFile": "codemodel-v2-abc.json"}]
                }}}
            }), encoding="utf-8")
            with self.assertRaises(targets_mod.TargetEvidenceUnavailable) as cm:
                targets_mod.extract_from_reply(reply)
            self.assertIn("path traversal", str(cm.exception).lower())

    def test_duplicate_keys_in_target_json_are_rejected(self) -> None:
        """Duplicate JSON keys (type shadowing) must be caught by strict_json."""
        with tempfile.TemporaryDirectory(prefix="spark-dup-") as tmp:
            reply = Path(tmp)
            (reply / "target-X.json").write_text(
                '{"name":"SparkGameFPS","type":"EXECUTABLE",'
                '"type":"SHARED_LIBRARY","nameOnDisk":"libSparkGameFPS.so",'
                '"paths":{"source":"GameModules/SparkGameFPS"},'
                '"sources":[{"path":"GameModules/SparkGameFPS/Source/Main.cpp"}],'
                '"artifacts":[{"path":"bin/libSparkGameFPS.so"}]}',
                encoding="utf-8",
            )
            (reply / "codemodel-v2-abc.json").write_text(json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS", "jsonFile": "target-X.json"}],
                }]
            }), encoding="utf-8")
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {targets_mod.CLIENT_NAME: {"query.json": {
                    "responses": [{"kind": "codemodel",
                                   "jsonFile": "codemodel-v2-abc.json"}]
                }}}
            }), encoding="utf-8")
            with self.assertRaises((
                targets_mod.TargetEvidenceUnavailable,
                strict_json.StrictJSONError,
            )):
                targets_mod.extract_from_reply(reply)

    def test_cmake_target_with_more_than_512_sources_is_accepted(self) -> None:
        """Real CMake File API targets can legitimately exceed 512 sources."""
        source_count = 526
        target_content = json.dumps({
            "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
            "nameOnDisk": "libSparkGameFPS.so",
            "paths": {"source": "GameModules/SparkGameFPS"},
            "sources": [
                {"path": f"GameModules/SparkGameFPS/Source/Unit{index}.cpp"}
                for index in range(source_count)
            ],
            "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
        })
        with tempfile.TemporaryDirectory(prefix="spark-many-sources-") as tmp:
            reply = Path(tmp) / "reply"
            self._write_reply(reply, target_content=target_content)
            index = targets_mod.extract_from_reply(reply)
            target_index_path = Path(tmp) / "module-targets.json"
            targets_mod.write_index(
                index,
                target_index_path,
                commit_sha=self.sha,
                generated_at=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                source="test",
            )
            captured = targets_mod.load_target_index(target_index_path)
        self.assertEqual(len(captured["targets"]["SparkGameFPS"]["sources"]), source_count)

    def test_shared_reply_fallback_is_gone(self) -> None:
        """Without a client-specific reply, existence of a shared codemodel
        must not substitute — only our own client reply is authoritative."""
        with tempfile.TemporaryDirectory(prefix="spark-fallback-") as tmp:
            reply = Path(tmp)
            (reply / "codemodel-v2-shared.json").write_text(json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS", "jsonFile": "target-X.json"}],
                }]
            }), encoding="utf-8")
            (reply / "target-X.json").write_text(json.dumps({
                "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                "nameOnDisk": "libSparkGameFPS.so",
                "paths": {"source": "GameModules/SparkGameFPS"},
                "sources": [{"path": "GameModules/SparkGameFPS/Source/Main.cpp"}],
                "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
            }), encoding="utf-8")
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {"some-other-client": {"query.json": {
                    "responses": [{"kind": "codemodel",
                                   "jsonFile": "codemodel-v2-shared.json"}]
                }}}
            }), encoding="utf-8")
            with self.assertRaises(targets_mod.TargetEvidenceUnavailable) as cm:
                targets_mod.extract_from_reply(reply)
            self.assertIn("authoritative", str(cm.exception))

    def test_foreign_source_directory_in_codemodel_is_rejected(self) -> None:
        """A target claiming source in C:/not-the-repository must be caught."""
        foreign_index = target_index()
        foreign_index["targets"]["SparkGameFPS"]["sourceDirectory"] = "C:/not-the-repository/GameModules/SparkGameFPS"
        errors = targets_mod.check_target(
            foreign_index, "SparkGameFPS", "SparkGameFPS",
            {"windows": "SparkGameFPS.dll", "linux": "libSparkGameFPS.so"},
            "forged",
        )
        self.assertTrue(
            any("foreign source tree" in e for e in errors),
            f"foreign source directory was not rejected: {errors}",
        )

    def test_load_target_index_rejects_malformed_commit_sha(self) -> None:
        """Provenance with invalid commitSHA must be rejected."""
        from datetime import datetime, timezone
        with tempfile.TemporaryDirectory(prefix="spark-prov-") as tmp:
            path = Path(tmp) / "targets.json"
            targets_mod.write_index(
                {"SparkGameFPS": {"name": "SparkGameFPS", "type": "SHARED_LIBRARY"}},
                path,
                commit_sha="NOT-A-VALID-SHA",
                generated_at=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                source="test",
            )
            with self.assertRaises(targets_mod.TargetEvidenceRejected) as cm:
                targets_mod.load_target_index(path)
            self.assertIn("provenance", str(cm.exception).lower())

    def test_load_target_index_rejects_malformed_generated_at(self) -> None:
        """Provenance with invalid generatedAt must be rejected."""
        with tempfile.TemporaryDirectory(prefix="spark-prov-") as tmp:
            path = Path(tmp) / "targets.json"
            targets_mod.write_index(
                {"SparkGameFPS": {"name": "SparkGameFPS", "type": "SHARED_LIBRARY"}},
                path,
                commit_sha="a" * 40,
                generated_at="not-a-timestamp",
                source="test",
            )
            with self.assertRaises(targets_mod.TargetEvidenceRejected) as cm:
                targets_mod.load_target_index(path)
            self.assertIn("provenance", str(cm.exception).lower())

    def test_load_target_index_accepts_valid_provenance(self) -> None:
        """A well-formed document with valid provenance must load."""
        from datetime import datetime, timezone
        with tempfile.TemporaryDirectory(prefix="spark-prov-") as tmp:
            path = Path(tmp) / "targets.json"
            targets_mod.write_index(
                {"SparkGameFPS": {"name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                                  "sources": ["a.cpp"], "artifacts": ["a.so"]}},
                path,
                commit_sha=self.sha,
                generated_at=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                source="test",
            )
            doc = targets_mod.load_target_index(path)
            self.assertIn("SparkGameFPS", doc["targets"])

    def test_load_target_index_rejects_reparse_leaf_without_reading_target(self) -> None:
        """A target evidence symlink cannot redirect the release consumer."""
        from datetime import datetime, timezone
        with tempfile.TemporaryDirectory(prefix="spark-target-reparse-") as tmp:
            root = Path(tmp)
            target = root / "real-targets.json"
            targets_mod.write_index(
                {"SparkGameFPS": {"name": "SparkGameFPS", "type": "SHARED_LIBRARY"}},
                target,
                commit_sha=self.sha,
                generated_at=datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                source="test",
            )
            alias = root / "targets.json"
            try:
                os.symlink(str(target), str(alias))
            except (OSError, NotImplementedError) as exc:
                self.skipTest(f"cannot create target evidence reparse fixture: {exc}")

            with self.assertRaises(targets_mod.TargetEvidenceRejected):
                targets_mod.load_target_index(alias)
            self.assertTrue(target.is_file(), "the real target index was altered")


class TestCMakeFileAPI(unittest.TestCase):
    """The File API is the authority for target existence; prove it is read
    correctly, and that a configure actually discriminates."""

    def test_extract_from_reply_reads_a_recorded_codemodel(self) -> None:
        with tempfile.TemporaryDirectory(prefix="spark-reply-") as tmp:
            reply = Path(tmp)
            (reply / "target-X.json").write_text(json.dumps({
                "name": "SparkGameFPS", "type": "SHARED_LIBRARY",
                "nameOnDisk": "libSparkGameFPS.so",
                "paths": {"source": "GameModules/SparkGameFPS"},
                "sources": [{"path": "GameModules/SparkGameFPS/Source/Main.cpp"}],
                "artifacts": [{"path": "bin/libSparkGameFPS.so"}],
            }), encoding="utf-8")
            (reply / "codemodel-v2-abc.json").write_text(json.dumps({
                "configurations": [{
                    "name": "Release",
                    "targets": [{"name": "SparkGameFPS", "jsonFile": "target-X.json"}],
                }]
            }), encoding="utf-8")
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {targets_mod.CLIENT_NAME: {"query.json": {
                    "responses": [{"kind": "codemodel",
                                   "jsonFile": "codemodel-v2-abc.json"}]
                }}}
            }), encoding="utf-8")
            index = targets_mod.extract_from_reply(reply)
        self.assertEqual(index["SparkGameFPS"]["type"], "SHARED_LIBRARY")
        self.assertEqual(index["SparkGameFPS"]["sources"],
                         ["GameModules/SparkGameFPS/Source/Main.cpp"])

    def test_query_error_in_reply_raises(self) -> None:
        with tempfile.TemporaryDirectory(prefix="spark-reply-") as tmp:
            reply = Path(tmp)
            (reply / "index-1.json").write_text(json.dumps({
                "reply": {targets_mod.CLIENT_NAME: {
                    "query.json": {"error": "unknown request kind"}}}
            }), encoding="utf-8")
            with self.assertRaises(targets_mod.TargetEvidenceUnavailable):
                targets_mod.extract_from_reply(reply)

    def test_undecorate_library_name_handles_every_toolchain(self) -> None:
        for filename, expected in (
            ("libSparkGameFPS.so", "SparkGameFPS"),
            ("libSparkGameFPS.dll", "SparkGameFPS"),
            ("SparkGameFPS.dll", "SparkGameFPS"),
            ("libSparkGameFPS.dylib", "SparkGameFPS"),
        ):
            with self.subTest(filename=filename):
                self.assertEqual(targets_mod.undecorate_library_name(filename),
                                 expected)

    @unittest.skipIf(shutil.which("cmake") is None, "cmake is not on PATH")
    def test_real_configure_discriminates_a_declared_target(self) -> None:
        """A configure must report the target only when it is really created.

        This is the property a regex over CMakeLists.txt cannot have: the
        control project declares its target through a helper function, so the
        string `add_library(` never appears in the file that declares it.
        """
        with tempfile.TemporaryDirectory(prefix="spark-cmake-") as tmp:
            src = Path(tmp) / "src"
            src.mkdir()
            (src / "a.cpp").write_text("int spark_probe() { return 0; }\n",
                                       encoding="utf-8")
            (src / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.25)\n"
                "project(SparkProbe CXX)\n"
                "function(spark_add_game_module name)\n"
                "  add_library(${name} SHARED ${ARGN})\n"
                "endfunction()\n"
                "option(MAKE_IT \"\" ON)\n"
                "if(MAKE_IT)\n"
                "  spark_add_game_module(ProbeModule a.cpp)\n"
                "endif()\n"
                "# add_library(GhostModule SHARED a.cpp)\n",
                encoding="utf-8",
            )
            text = (src / "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertNotIn("add_library(ProbeModule", text,
                             "fixture must not declare the target literally")
            self.assertIn("# add_library(GhostModule", text)

            for make_it, expect_target in ((True, True), (False, False)):
                build = Path(tmp) / f"build-{make_it}"
                build.mkdir()
                targets_mod.write_query(build)
                proc = subprocess.run(
                    ["cmake", "-S", str(src), "-B", str(build),
                     f"-DMAKE_IT={'ON' if make_it else 'OFF'}"],
                    capture_output=True, text=True, timeout=600, check=False,
                )
                if proc.returncode != 0:
                    self.skipTest(
                        f"cmake configure failed in this environment: "
                        f"{proc.stderr[-400:]}"
                    )
                reply = build / ".cmake" / "api" / "v1" / "reply"
                if expect_target:
                    index = targets_mod.extract_from_reply(reply)
                    self.assertIn("ProbeModule", index)
                    self.assertEqual(index["ProbeModule"]["type"], "SHARED_LIBRARY")
                    self.assertTrue(index["ProbeModule"]["sources"])
                    self.assertNotIn(
                        "GhostModule", index,
                        "a commented-out add_library must not yield a target",
                    )
                else:
                    index = targets_mod.extract_from_reply(reply)
                    self.assertNotIn(
                        "ProbeModule", index,
                        "target reported despite not being created",
                    )


# --------------------------------------------------------------------------
# Artifact semantic validation — the zero-byte bypass (rdy010_artifact_adversarial_probe)
# --------------------------------------------------------------------------
class TestArtifactSemanticValidation(FixtureCase):
    """Zero-byte and trivially fabricated artifacts must be rejected."""

    def _junit_xml(self, content: str) -> Path:
        path = self.repo / "build" / "test-junit.xml"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def _smoke_log(self, content: str) -> Path:
        path = self.repo / "build" / "test-smoke.log"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    VALID_JUNIT = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<testsuites tests="5" failures="0" errors="0">\n'
        '  <testsuite name="SparkGameFPS" tests="5" failures="0" errors="0">\n'
        '    <testcase name="test_load" classname="SparkGameFPS.Module"/>\n'
        '    <testcase name="test_init" classname="SparkGameFPS.Module"/>\n'
        '    <testcase name="test_update" classname="SparkGameFPS.Module"/>\n'
        '    <testcase name="test_unload" classname="SparkGameFPS.Module"/>\n'
        '    <testcase name="test_shutdown" classname="SparkGameFPS.Module"/>\n'
        '  </testsuite>\n'
        '</testsuites>\n'
    )
    VALID_SMOKE = (
        "[package-smoke] SparkGameFPS\n"
        "[package-smoke] product=SparkEngine\n"
        "[package-smoke] module=SparkGameFPS\n"
        "[package-smoke] library=SparkGameFPS.dll\n"
        "[package-smoke] exit_code=0\n"
        "[package-smoke] PASS\n"
    )

    def test_zero_byte_junit_xml_is_rejected(self) -> None:
        path = self._junit_xml("")
        path.write_bytes(b"")
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(any("zero bytes" in e for e in errors), errors)

    def test_valid_junit_xml_is_accepted(self) -> None:
        path = self._junit_xml(self.VALID_JUNIT)
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertEqual(errors, [], f"valid JUnit XML rejected: {errors}")

    def test_junit_xml_with_zero_tests_is_rejected(self) -> None:
        path = self._junit_xml(
            '<testsuites tests="0" failures="0" errors="0">'
            '</testsuites>'
        )
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(any("zero tests" in e for e in errors), errors)

    def test_junit_xml_with_no_testcases_is_rejected(self) -> None:
        path = self._junit_xml(
            '<testsuites tests="5" failures="0" errors="0">'
            '</testsuites>'
        )
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(any("no <testcase>" in e for e in errors), errors)

    def test_invalid_xml_is_rejected(self) -> None:
        path = self._junit_xml("this is not xml at all {{{")
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(
            any("not valid XML" in e or "cannot be parsed" in e for e in errors),
            errors,
        )

    def test_junit_xml_with_too_few_testcases_is_rejected(self) -> None:
        path = self._junit_xml(
            '<testsuites tests="1" failures="0" errors="0">'
            '  <testsuite name="X" tests="1">'
            '    <testcase name="t1" classname="X.Y"/>'
            '  </testsuite>'
            '</testsuites>'
        )
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(any("minimum" in e for e in errors), errors)

    def test_junit_xml_with_entity_declaration_is_rejected(self) -> None:
        path = self._junit_xml(
            '<?xml version="1.0"?>\n'
            '<!DOCTYPE foo [\n'
            '  <!ENTITY xxe "pwned">\n'
            ']>\n'
            '<testsuites tests="3" failures="0" errors="0">\n'
            '  <testsuite name="X" tests="3">\n'
            '    <testcase name="t1" classname="X.Y"/>\n'
            '    <testcase name="t2" classname="X.Y"/>\n'
            '    <testcase name="t3" classname="X.Y"/>\n'
            '  </testsuite>\n'
            '</testsuites>\n'
        )
        errors = artifacts.validate_junit_xml(path, INCLUDED)
        self.assertTrue(
            any("entity" in e.lower() or "not valid XML" in e for e in errors),
            f"XXE entity declaration was accepted: {errors}",
        )

    def test_zero_byte_package_smoke_is_rejected(self) -> None:
        path = self._smoke_log("")
        path.write_bytes(b"")
        errors = artifacts.validate_package_smoke(path, INCLUDED)
        self.assertTrue(any("zero bytes" in e for e in errors), errors)

    def test_valid_package_smoke_is_accepted(self) -> None:
        path = self._smoke_log(self.VALID_SMOKE)
        errors = artifacts.validate_package_smoke(path, INCLUDED)
        self.assertEqual(errors, [], f"valid smoke log rejected: {errors}")

    def test_package_smoke_without_module_name_is_rejected(self) -> None:
        path = self._smoke_log(
            "[package-smoke] SomeOtherModule\n"
            "[package-smoke] product=SparkEngine\n"
            "[package-smoke] module=SomeOtherModule\n"
            "[package-smoke] exit_code=0\n"
            "[package-smoke] PASS\n"
        )
        errors = artifacts.validate_package_smoke(path, INCLUDED)
        self.assertTrue(any("does not mention" in e for e in errors), errors)

    def test_package_smoke_without_pass_indicator_is_rejected(self) -> None:
        path = self._smoke_log(
            f"[package-smoke] {INCLUDED}\n"
            f"[package-smoke] module={INCLUDED}\n"
            f"[package-smoke] library={INCLUDED}.dll\n"
            "[package-smoke] exit_code=1\n"
            "[package-smoke] FAIL\n"
        )
        errors = artifacts.validate_package_smoke(path, INCLUDED)
        self.assertTrue(any("pass indicator" in e for e in errors), errors)

    def test_whitespace_only_smoke_log_is_rejected(self) -> None:
        path = self._smoke_log("   \n  \n\n  \n")
        errors = artifacts.validate_package_smoke(path, INCLUDED)
        self.assertTrue(len(errors) > 0, "whitespace-only log accepted")

    def test_artifact_dispatcher_routes_correctly(self) -> None:
        path = self._junit_xml(self.VALID_JUNIT)
        self.assertEqual(artifacts.validate_artifact(path, "junit-xml", INCLUDED), [])
        self.assertEqual(
            artifacts.validate_artifact(path, "cmake-target-index", INCLUDED), [])

    def test_held_byte_dispatcher_routes_correctly(self) -> None:
        self.assertEqual(
            artifacts.validate_artifact_bytes(
                self.VALID_JUNIT.encode("utf-8"), "test-junit.xml", "junit-xml", INCLUDED,
            ),
            [],
        )
        self.assertEqual(
            artifacts.validate_artifact_bytes(
                self.VALID_SMOKE.encode("utf-8"), "test-smoke.log", "package-smoke-log", INCLUDED,
            ),
            [],
        )

    def test_full_validator_rejects_zero_byte_artifacts(self) -> None:
        """The full ManifestValidator must reject zero-byte evidence files."""
        m = base_manifest()
        junit = self.repo / EVIDENCE_PRODUCERS["junit-xml"]["artifact"]
        junit.parent.mkdir(parents=True, exist_ok=True)
        junit.write_bytes(b"")
        smoke = self.repo / EVIDENCE_PRODUCERS["package-smoke-log"]["artifact"]
        smoke.parent.mkdir(parents=True, exist_ok=True)
        smoke.write_bytes(b"")
        errors = self.validate(m)
        self.assertTrue(
            any("zero bytes" in e for e in errors),
            f"zero-byte artifact was accepted by the full validator: {errors}",
        )

    def test_full_validator_consumes_artifacts_from_held_root_bytes(self) -> None:
        """Normal semantic artifacts stay valid when read from rooted bytes."""
        m = base_manifest()
        junit = self.repo / EVIDENCE_PRODUCERS["junit-xml"]["artifact"]
        junit.parent.mkdir(parents=True, exist_ok=True)
        junit.write_text(self.VALID_JUNIT, encoding="utf-8")
        smoke = self.repo / EVIDENCE_PRODUCERS["package-smoke-log"]["artifact"]
        smoke.parent.mkdir(parents=True, exist_ok=True)
        smoke.write_text(self.VALID_SMOKE, encoding="utf-8")

        with strict_json.open_no_follow_directory_lease(self.repo, label="test root") as lease:
            errors = ManifestValidator(
                m, self.repo,
                target_index=target_index_for_revision(self.sha),
                lifecycle_evidence=lifecycle_evidence(self.repo, self.sha),
                expected_sha=self.sha,
                root_authority=lease,
            ).validate()

        self.assertEqual(errors, [], errors)

    @unittest.skipIf(os.name == "nt", "POSIX rooted artifact-ratchet regression")
    def test_rooted_artifact_branch_rejects_a_stale_declared_gap(self) -> None:
        """A rooted present artifact must trip the same ledger ratchet as paths."""
        m = base_manifest()
        junit = self.repo / EVIDENCE_PRODUCERS["junit-xml"]["artifact"]
        junit.parent.mkdir(parents=True, exist_ok=True)
        junit.write_text(self.VALID_JUNIT, encoding="utf-8")
        smoke = self.repo / EVIDENCE_PRODUCERS["package-smoke-log"]["artifact"]
        smoke.parent.mkdir(parents=True, exist_ok=True)
        smoke.write_text(self.VALID_SMOKE, encoding="utf-8")

        with strict_json.open_no_follow_directory_lease(self.repo, label="test root") as lease:
            errors = ManifestValidator(
                m, self.repo,
                target_index=target_index_for_revision(self.sha),
                lifecycle_evidence=lifecycle_evidence(self.repo, self.sha),
                expected_sha=self.sha,
                declared_gaps={"package-smoke-log": "MOD-310"},
                root_authority=lease,
            ).validate()

        self.assertTrue(
            any("recorded as a known evidence gap" in error for error in errors),
            errors,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
