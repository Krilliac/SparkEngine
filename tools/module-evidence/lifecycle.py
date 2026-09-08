#!/usr/bin/env python3
"""Runtime lifecycle evidence: phases actually executed, not phases declared.

What a declared phase list proves
---------------------------------
Nothing.  `lifecyclePhases` in the policy manifest is a list of strings a human
typed.  A module with no code at all can declare all fourteen phases and read
as fully exercised.  Worse, the vocabulary itself conflates two different
things: `SparkGetModuleCompatibility`, `CreateModule` and `DestroyModule` are
free ``extern "C"`` exports resolved by name out of the shared library, while
`OnLoad`, `OnUpdate` and the rest are C++ virtual methods on `IModule` — and
six of those (`OnFixedUpdate`, `OnRender`, `OnImGui`, `OnResize`, `OnPause`,
`OnResume`) have no-op defaults in the base class, so "the module has this
phase" is true of every module whether or not it implements anything.

What this module requires instead
---------------------------------
A *lifecycle evidence* document, written by a real run of the engine's
ModuleManager against a really-loaded shared library, recording which phases
were actually entered.  The document is bound to the exact source under test
by the git tree object hash of the module's source directory: that hash is
computed by git over the tree contents, so evidence generated against a
different revision of the module cannot be presented as evidence for this one.

Absent evidence is a blocking gap.  It is never treated as a pass.
"""

from __future__ import annotations

import re
import subprocess
from pathlib import Path
from typing import Any

import provenance
import strict_json

LIFECYCLE_SCHEMA_VERSION = "module-lifecycle-v1"

# Phases that must be observed to have really executed for an included module.
# These are the four the acceptance criteria name — load, update, unload,
# shutdown — expressed in the ABI's own vocabulary.  `CreateModule` and
# `DestroyModule` are the free exports that bracket the module's existence;
# `OnLoad`/`OnUpdate`/`OnUnload` are the virtuals that must be entered.
REQUIRED_RUNTIME_PHASES = (
    "CreateModule",
    "OnLoad",
    "OnUpdate",
    "OnUnload",
    "DestroyModule",
)

# Every phase name a lifecycle record may legitimately report.
OBSERVABLE_PHASES = frozenset(
    set(REQUIRED_RUNTIME_PHASES)
    | {
        "SparkGetModuleCompatibility",
        "GetModuleInfo",
        "OnFixedUpdate",
        "OnRender",
        "OnImGui",
        "OnResize",
        "OnPause",
        "OnResume",
        "CanUnload",
    }
)

REQUIRED_RECORD_KEYS = frozenset(
    {"module", "sharedLibrary", "sourceDirectory", "sourceTreeSHA", "runner",
     "phases", "engineSHA256", "enginePath"}
)

ENGINE_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

VALID_RUNNERS = frozenset({"ctest", "spark-automation", "headless-exec"})


class LifecycleEvidenceUnavailable(RuntimeError):
    """No runtime lifecycle evidence could be read.

    Raised rather than returning an empty result so that "nothing ran" can
    never be mistaken for "nothing failed".
    """


def source_tree_sha(repo_root: Path, commit_sha: str, source_directory: str) -> tuple[str | None, str | None]:
    """Git tree object hash of a module's source directory at a revision.

    Returns (sha, error).  This is the binding between evidence and the exact
    source it was produced for: git computes it over the tree contents, so it
    changes whenever any file under the directory changes.
    """
    spec = f"{commit_sha}:{source_directory}"
    try:
        proc = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", spec],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        return None, f"cannot run git to hash {spec}: {exc}"
    if proc.returncode != 0:
        return None, (
            f"git cannot resolve {spec}: {proc.stderr.strip()} — the declared "
            f"source directory does not exist at the revision under test"
        )
    sha = proc.stdout.strip()
    if not provenance.SHA_RE.match(sha):
        return None, f"git rev-parse {spec} returned {sha!r}"
    return sha, None


def load_lifecycle_evidence(path: Path) -> dict[str, Any]:
    """Load a lifecycle evidence document, or raise."""
    if not path.is_file():
        raise LifecycleEvidenceUnavailable(
            f"lifecycle evidence not found at {path} — a module's runtime "
            f"phases are proven by running it, not by declaring them"
        )
    try:
        document = strict_json.load_file(path)
    except strict_json.StrictJSONError as exc:
        raise LifecycleEvidenceUnavailable(f"lifecycle evidence unusable: {exc}") from exc
    if not isinstance(document, dict):
        raise LifecycleEvidenceUnavailable(f"{path}: lifecycle evidence must be an object")
    if document.get("schemaVersion") != LIFECYCLE_SCHEMA_VERSION:
        raise LifecycleEvidenceUnavailable(
            f"{path}: lifecycle evidence schemaVersion must be "
            f"{LIFECYCLE_SCHEMA_VERSION!r}, got {document.get('schemaVersion')!r}"
        )
    records = document.get("records")
    if not isinstance(records, list) or not records:
        raise LifecycleEvidenceUnavailable(
            f"{path}: lifecycle evidence contains no records — an empty run "
            f"is not a successful one"
        )
    return document


def check_record(
    record: Any,
    module_name: str,
    expected_source_tree_sha: str,
    declared_libraries: dict[str, str],
    label: str,
) -> list[str]:
    """Validate one module's runtime lifecycle record."""
    if not isinstance(record, dict):
        return [f"{label}: lifecycle record must be an object"]

    errors: list[str] = []
    present = set(record.keys())
    missing = REQUIRED_RECORD_KEYS - present
    if missing:
        errors.append(f"{label}: lifecycle record missing keys {sorted(missing)}")
    unknown = present - REQUIRED_RECORD_KEYS
    if unknown:
        errors.append(f"{label}: lifecycle record has unknown keys {sorted(unknown)}")
    if errors:
        return errors

    if record["module"] != module_name:
        errors.append(
            f"{label}: lifecycle record is for module {record['module']!r}, "
            f"not {module_name!r}"
        )

    if record["sourceTreeSHA"] != expected_source_tree_sha:
        errors.append(
            f"{label}: lifecycle evidence was produced for source tree "
            f"{record['sourceTreeSHA']!r} but the source under test hashes to "
            f"{expected_source_tree_sha!r} — evidence from a different revision "
            f"of the module does not prove anything about this one"
        )

    if record["sharedLibrary"] not in set(declared_libraries.values()):
        errors.append(
            f"{label}: lifecycle evidence loaded {record['sharedLibrary']!r}, "
            f"which is not among the declared shared libraries "
            f"{sorted(set(declared_libraries.values()))}"
        )

    runner = record["runner"]
    if runner not in VALID_RUNNERS:
        errors.append(
            f"{label}: runner {runner!r} is not one of {sorted(VALID_RUNNERS)}"
        )

    engine_sha = record.get("engineSHA256")
    if not isinstance(engine_sha, str) or not ENGINE_SHA256_RE.match(engine_sha):
        errors.append(
            f"{label}: engineSHA256 must be a 64-character lowercase hex digest "
            f"of the engine binary that produced this evidence, got {engine_sha!r}"
        )

    engine_path = record.get("enginePath")
    if not isinstance(engine_path, str) or not engine_path or len(engine_path) > 512:
        errors.append(
            f"{label}: enginePath must be a non-empty string (≤512 chars) "
            f"naming the engine executable, got {engine_path!r}"
        )

    phases = record["phases"]
    if not isinstance(phases, dict) or not phases:
        return errors + [f"{label}: lifecycle record declares no observed phases"]

    observed: set[str] = set()
    for phase, count in phases.items():
        if phase not in OBSERVABLE_PHASES:
            errors.append(f"{label}: unknown observed phase {phase!r}")
            continue
        if not isinstance(count, int) or isinstance(count, bool) or count < 0:
            errors.append(
                f"{label}: phase {phase!r} execution count must be a "
                f"non-negative integer, got {count!r}"
            )
            continue
        if count > 0:
            observed.add(phase)

    missing_runtime = [p for p in REQUIRED_RUNTIME_PHASES if p not in observed]
    if missing_runtime:
        errors.append(
            f"{label}: required lifecycle phases were never executed: "
            f"{missing_runtime} — a phase with a zero execution count did not run"
        )
    return errors
