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

import ntpath
import re
import subprocess
from pathlib import Path, PureWindowsPath
from typing import Any

import provenance
import strict_json

LIFECYCLE_SCHEMA_VERSION = "module-lifecycle-v1"

# Phases that the stable-v1 Windows headless collector must observe.  The
# free exports bracket the module's existence; the virtuals prove that the
# loaded module advanced both simulation and rendering before orderly teardown.
REQUIRED_RUNTIME_PHASES = (
    "CreateModule",
    "OnLoad",
    "OnUpdate",
    "OnFixedUpdate",
    "OnRender",
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
     "phases", "engineSHA256", "enginePath", "moduleSHA256", "modulePath"}
)

ENGINE_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

LIFECYCLE_DOCUMENT_KEYS = frozenset(
    {"schemaVersion", "generatedAt", "commitSHA", "records"}
)

# The stable-v1 release profile has one intentionally narrow runtime proof.
# Accepting a generic test runner, Linux image, or another module's source tree
# would turn a manually manufactured record into a release attestation.
STABLE_V1_MODULE = "SparkGameFPS"
STABLE_V1_SOURCE_DIRECTORY = "GameModules/SparkGameFPS/Source"
STABLE_V1_SHARED_LIBRARY = "SparkGameFPS.dll"
STABLE_V1_RUNNER = "headless-exec"
STABLE_V1_ENGINE_LEAF = "SparkEngine.exe"
STABLE_V1_MODULE_LEAF = "SparkGameFPS.dll"

_WINDOWS_RESERVED_NAMES = frozenset(
    {"CON", "PRN", "AUX", "NUL", *(f"COM{number}" for number in range(1, 10)),
     *(f"LPT{number}" for number in range(1, 10))}
)


class LifecycleEvidenceUnavailable(RuntimeError):
    """The exact lifecycle evidence leaf is genuinely absent.

    This is intentionally narrow: it is the only lifecycle loader outcome that
    ``--allow-declared-gaps`` may downgrade.  A present but malformed or unsafe
    artifact is not evidence of an absent producer.
    """


class LifecycleEvidenceRejected(LifecycleEvidenceUnavailable):
    """Present lifecycle evidence cannot safely be accepted by the release gate."""


class LifecycleEvidenceAuthorityError(LifecycleEvidenceRejected):
    """No held no-follow authority could safely establish the evidence bytes."""


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


def source_tree_sha_rooted(
    root: strict_json.NoFollowDirectoryLease,
    commit_sha: str,
    source_directory: str,
) -> tuple[str | None, str | None]:
    """Resolve a module source tree through descriptor-rooted POSIX Git."""
    spec = f"{commit_sha}:{source_directory}"
    try:
        proc = provenance.run_rooted_git(root, "rev-parse", spec)
    except (OSError, subprocess.SubprocessError, strict_json.NoFollowAuthorityError) as exc:
        return None, f"cannot run rooted git to hash {spec}: {exc}"
    if proc.returncode != 0:
        return None, (
            f"rooted git cannot resolve {spec}: {proc.stderr.strip()} — the declared "
            "source directory does not exist at the revision under test"
        )
    sha = proc.stdout.strip()
    if not provenance.SHA_RE.match(sha):
        return None, f"rooted git rev-parse {spec} returned {sha!r}"
    return sha, None


def check_document_shape(document: Any, label: str) -> list[str]:
    """Return errors unless a stable-v1 lifecycle document has one record.

    The loader and injected-evidence path must apply the same closed-world
    shape check.  Otherwise an in-memory caller can bypass the loader and let
    a later mapping silently discard duplicate or non-object records.
    """
    if not isinstance(document, dict):
        return [f"{label}: lifecycle evidence must be an object"]

    errors: list[str] = []
    present = set(document)
    missing = LIFECYCLE_DOCUMENT_KEYS - present
    unknown = present - LIFECYCLE_DOCUMENT_KEYS
    if missing:
        errors.append(f"{label}: lifecycle evidence missing top-level keys {sorted(missing)}")
    if unknown:
        errors.append(f"{label}: lifecycle evidence has unknown top-level keys {sorted(unknown)}")
    if errors:
        return errors

    if document["schemaVersion"] != LIFECYCLE_SCHEMA_VERSION:
        errors.append(
            f"{label}: lifecycle evidence schemaVersion must be "
            f"{LIFECYCLE_SCHEMA_VERSION!r}, got {document['schemaVersion']!r}"
        )

    records = document["records"]
    if not isinstance(records, list) or len(records) != 1:
        errors.append(
            f"{label}: stable-v1 lifecycle evidence must contain exactly one record"
        )
    elif not isinstance(records[0], dict):
        errors.append(
            f"{label}: stable-v1 lifecycle evidence record must be an object"
        )
    return errors


def load_lifecycle_evidence_bytes(data: bytes, origin: str) -> dict[str, Any]:
    """Load lifecycle evidence from exact already-held bytes, or raise."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise LifecycleEvidenceRejected(
            f"lifecycle evidence is present but not UTF-8: {exc}"
        ) from exc
    try:
        document = strict_json.loads(text, origin=origin)
    except strict_json.StrictJSONError as exc:
        raise LifecycleEvidenceRejected(
            f"lifecycle evidence is present but unusable: {exc}"
        ) from exc
    shape_errors = check_document_shape(document, origin)
    if shape_errors:
        raise LifecycleEvidenceRejected("; ".join(shape_errors))
    assert isinstance(document, dict)
    return document


def load_lifecycle_evidence(path: Path) -> dict[str, Any]:
    """Load lifecycle evidence from exact no-follow file bytes, or raise."""
    try:
        data = strict_json.read_file_no_follow_bytes(
            path, max_bytes=strict_json.DEFAULT_LIMITS.document_bytes,
        )
    except strict_json.NoFollowEvidenceMissing as exc:
        raise LifecycleEvidenceUnavailable(
            f"lifecycle evidence leaf is absent: {exc}"
        ) from exc
    except strict_json.NoFollowAuthorityError as exc:
        raise LifecycleEvidenceAuthorityError(
            f"lifecycle evidence authority rejected: {exc}"
        ) from exc
    except strict_json.StrictJSONError as exc:
        raise LifecycleEvidenceRejected(
            f"lifecycle evidence is present but unusable: {exc}"
        ) from exc
    return load_lifecycle_evidence_bytes(data, str(path))


def _windows_final_path_error(value: Any, expected_leaf: str, label: str) -> str | None:
    """Reject non-canonical or alias-prone Windows final path strings.

    ``GetFinalPathNameByHandleW`` produces a drive-qualified path.  The
    collector may preserve its extended ``\\\\?\\`` spelling in evidence, so
    accept that one prefix while rejecting UNC, relative, Unix, ADS, dot, and
    case-alias spellings.  ``PureWindowsPath`` and ``ntpath`` deliberately
    parse the value as Windows syntax even when policy validation runs on a
    Linux host.
    """
    if not isinstance(value, str) or isinstance(value, bool) or not value or len(value) > 512:
        return (
            f"{label} must be a non-empty string (≤512 chars) naming the "
            f"Windows collector image, got {value!r}"
        )
    if "\0" in value:
        return f"{label} contains a NUL byte"
    if "/" in value:
        return f"{label} must use a canonical Windows final path, not slash aliases"

    canonical = value
    if canonical.startswith("\\\\?\\"):
        canonical = canonical[4:]
        if canonical[:4].casefold() == "unc\\":
            return f"{label} must be a drive-qualified Windows path, not UNC"
    elif canonical.startswith("\\\\"):
        return f"{label} must be a drive-qualified Windows path, not UNC"

    drive, tail = ntpath.splitdrive(canonical)
    if not re.fullmatch(r"[A-Za-z]:", drive) or not tail.startswith("\\"):
        return f"{label} must be an absolute drive-qualified Windows path"
    if canonical != ntpath.normpath(canonical):
        return f"{label} is not a canonical Windows final path"

    segments = tail[1:].split("\\")
    if not segments or any(not segment for segment in segments):
        return f"{label} is not a canonical Windows final path"
    for segment in segments:
        if segment in {".", ".."}:
            return f"{label} contains a traversal or dot segment"
        if ":" in segment:
            return f"{label} contains an alternate-data-stream alias"
        if segment.endswith((".", " ")):
            return f"{label} contains a trailing-dot or trailing-space alias"
        if any(character in segment for character in '<>"|?*'):
            return f"{label} contains a Windows device or wildcard alias"
        if segment.split(".", 1)[0].upper() in _WINDOWS_RESERVED_NAMES:
            return f"{label} contains a reserved Windows device name {segment!r}"

    parsed = PureWindowsPath(canonical)
    if not parsed.is_absolute() or parsed.name != expected_leaf:
        return (
            f"{label} must exactly name the collector image {expected_leaf!r}, "
            f"got {parsed.name!r}"
        )
    return None


def check_record(
    record: Any,
    module_name: str,
    expected_source_directory: str,
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

    if module_name != STABLE_V1_MODULE:
        errors.append(
            f"{label}: stable-v1 collector evidence is only defined for "
            f"{STABLE_V1_MODULE!r}, not {module_name!r}"
        )

    if expected_source_directory != STABLE_V1_SOURCE_DIRECTORY:
        errors.append(
            f"{label}: stable-v1 manifest sourceDirectory must be "
            f"{STABLE_V1_SOURCE_DIRECTORY!r}, got {expected_source_directory!r}"
        )
    if record["sourceDirectory"] != STABLE_V1_SOURCE_DIRECTORY:
        errors.append(
            f"{label}: sourceDirectory must exactly match the collector source "
            f"{STABLE_V1_SOURCE_DIRECTORY!r}, got {record['sourceDirectory']!r}"
        )

    if record["sourceTreeSHA"] != expected_source_tree_sha:
        errors.append(
            f"{label}: lifecycle evidence was produced for source tree "
            f"{record['sourceTreeSHA']!r} but the source under test hashes to "
            f"{expected_source_tree_sha!r} — evidence from a different revision "
            f"of the module does not prove anything about this one"
        )

    expected_windows_library = (
        declared_libraries.get("windows") if isinstance(declared_libraries, dict) else None
    )
    if expected_windows_library != STABLE_V1_SHARED_LIBRARY:
        errors.append(
            f"{label}: stable-v1 manifest windows sharedLibrary must be "
            f"{STABLE_V1_SHARED_LIBRARY!r}, got {expected_windows_library!r}"
        )
    if record["sharedLibrary"] != STABLE_V1_SHARED_LIBRARY:
        errors.append(
            f"{label}: sharedLibrary must be the Windows collector DLL "
            f"{STABLE_V1_SHARED_LIBRARY!r}, got {record['sharedLibrary']!r}"
        )

    runner = record["runner"]
    if runner != STABLE_V1_RUNNER:
        errors.append(
            f"{label}: runner must be the stable-v1 collector "
            f"{STABLE_V1_RUNNER!r}, got {runner!r}"
        )

    engine_sha = record.get("engineSHA256")
    if not isinstance(engine_sha, str) or not ENGINE_SHA256_RE.fullmatch(engine_sha):
        errors.append(
            f"{label}: engineSHA256 must be a 64-character lowercase hex digest "
            f"of the engine binary that produced this evidence, got {engine_sha!r}"
        )

    engine_path = record.get("enginePath")
    engine_path_error = _windows_final_path_error(
        engine_path, STABLE_V1_ENGINE_LEAF, f"{label}: enginePath"
    )
    if engine_path_error:
        errors.append(engine_path_error)

    module_sha = record.get("moduleSHA256")
    if not isinstance(module_sha, str) or not ENGINE_SHA256_RE.fullmatch(module_sha):
        errors.append(
            f"{label}: moduleSHA256 must be a 64-character lowercase hex digest "
            f"of the loaded module binary, got {module_sha!r}"
        )

    module_path = record.get("modulePath")
    module_path_error = _windows_final_path_error(
        module_path, STABLE_V1_MODULE_LEAF, f"{label}: modulePath"
    )
    if module_path_error:
        errors.append(module_path_error)

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
