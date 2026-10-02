#!/usr/bin/env python3
"""Record and verify per-artifact build provenance (REL-100).

``record`` runs inside a package-producing job after the last step that can
change package bytes. It writes one closed JSON document binding every
distributable package digest to the exact source commit, the committed
dependency-lock digest, the configured toolchain, and the build configuration.

``verify`` runs in the release job before the first publication mutation. It
re-derives the source commit and dependency-lock digest from the release
checkout and requires every published distributable (aliases included) to be
covered by a record, and every recorded artifact to be published.

Schema v2 adds the platform toolchain (Windows SDK, exact MSVC tools version,
Visual Studio instance, linker and archiver) and the CPU instruction-set
baseline (SPARK_NATIVE_ARCH and the vendored Jolt USE_* options). A stable-v1
Windows record must carry every platform toolchain value and an ISA baseline
equal to the OD-04 floor (x86-64-v2: SSE4.2, no AVX/AVX2/FMA/F16C/LZCNT/TZCNT);
``record`` refuses to write one that does not, and ``verify --stable`` accepts
only such a v2 record. v1 records stay readable for nightly verification.

Every failure is fatal and exits non-zero; there is no advisory mode.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from typing import Any

SCHEMA_VERSION = "spark-build-provenance-v2"
LEGACY_SCHEMA_VERSION = "spark-build-provenance-v1"
STABLE_PROFILE = ("stable-v1", "Windows")
# OD-04: the stable-v1 CPU floor is x86-64 with SSE4.2 (x86-64-v2); AVX2 is not
# required. cmake/SparkCpuFloor.cmake pins vendored Jolt to exactly these options.
STABLE_CPU_FLOOR = "x86-64-v2"
JOLT_FLOOR_OPTIONS = {
    "USE_SSE4_1": True, "USE_SSE4_2": True, "USE_AVX": False, "USE_AVX2": False, "USE_AVX512": False,
    "USE_LZCNT": False, "USE_TZCNT": False, "USE_F16C": False, "USE_FMADD": False,
}
LOCK_PATH = "ThirdParty/dependencies.lock"
DISTRIBUTABLE_SUFFIXES = (".zip", ".tar.gz", ".exe", ".msi")
INSTALLER_PREFIX = "SparkInstaller-"
ENGINE_PREFIX = "SparkEngine-"

_SHA1 = re.compile(r"[0-9a-f]{40}")
_SHA256 = re.compile(r"[0-9a-f]{64}")
_SEMVER = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
_TOKEN = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]*")
_CACHE_LINE = re.compile(r"^(?P<key>[A-Za-z0-9_.+-]+)(?::[A-Z]+)?=(?P<value>.*)$")
_CMAKE_SET = re.compile(r'^set\((?P<key>[A-Z_]+) "(?P<value>[^"]*)"\)\s*$')
_WINDOWS_SDK = re.compile(r"10\.[0-9]+\.[0-9]+\.[0-9]+")
# The MSVC tools directory is the exact toolset build (e.g. 14.44.35207), finer
# than the v143 platform-toolset label or CMAKE_VS_PLATFORM_TOOLSET_VERSION.
_MSVC_TOOLS_DIR = re.compile(r"[/\\]VC[/\\]Tools[/\\]MSVC[/\\](?P<version>[0-9]+\.[0-9]+\.[0-9]+)[/\\]",
                             re.IGNORECASE)
_MSVC_TOOLS_VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+")
_CMAKE_TRUE = {"ON", "TRUE", "1", "YES", "Y"}
_CMAKE_FALSE = {"OFF", "FALSE", "0", "NO", "N"}

LEGACY_RECORD_KEYS = {"schemaVersion", "sourceSHA", "version", "profile", "platform", "configuration",
                      "dependencyLock", "toolchain", "cmakeConfiguration", "runnerImage", "artifacts"}
RECORD_KEYS = LEGACY_RECORD_KEYS | {"platformToolchain", "isaBaseline"}
PLATFORM_TOOLCHAIN_KEYS = {"windowsSdkVersion", "msvcToolsVersion", "visualStudioInstance", "linker", "archiver"}
ISA_BASELINE_KEYS = {"stableFloor", "nativeArch", "joltInstructionSets", "meetsStableFloor"}
TOOLCHAIN_KEYS = {"cmakeVersion", "generator", "generatorPlatform", "generatorToolset", "compilers"}
COMPILER_KEYS = {"id", "version", "architecture", "path"}


class ProvenanceError(Exception):
    """A provenance claim could not be established or did not verify."""


def _sha256_file(path: Path) -> str:
    state = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            state.update(chunk)
    return state.hexdigest()


def _git(source_root: Path, *args: str) -> bytes:
    result = subprocess.run(["git", "-C", str(source_root), *args], capture_output=True, check=False)
    if result.returncode != 0:
        raise ProvenanceError(f"git {' '.join(args)} failed: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def _committed_lock_digest(source_root: Path, source_sha: str) -> str:
    return hashlib.sha256(_git(source_root, "show", f"{source_sha}:{LOCK_PATH}")).hexdigest()


def _is_distributable(name: str) -> bool:
    return name.startswith(INSTALLER_PREFIX) or name.endswith(DISTRIBUTABLE_SUFFIXES)


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ProvenanceError(message)


def _meets_stable_floor(native_arch: bool | None, jolt: dict[str, bool | None]) -> bool:
    return native_arch is False and all(jolt.get(option) is want for option, want in JOLT_FLOOR_OPTIONS.items())


def _require_stable_platform(platform_toolchain: dict[str, Any], isa_baseline: dict[str, Any], where: str) -> None:
    """Fail closed unless a stable-v1 Windows build names its full toolchain and meets the CPU floor."""
    missing = sorted(key for key, value in platform_toolchain.items() if value is None)
    _require(not missing, f"{where} does not record platform toolchain value(s): {', '.join(missing)}")
    _require(bool(_WINDOWS_SDK.fullmatch(platform_toolchain["windowsSdkVersion"])),
             f"{where} Windows SDK version {platform_toolchain['windowsSdkVersion']!r} is not 10.x.y.z")
    _require(bool(_MSVC_TOOLS_VERSION.fullmatch(platform_toolchain["msvcToolsVersion"])),
             f"{where} MSVC tools version {platform_toolchain['msvcToolsVersion']!r} is not x.y.z")
    _require(isa_baseline["nativeArch"] is False,
             f"{where} ISA baseline is host-tuned (SPARK_NATIVE_ARCH={isa_baseline['nativeArch']}); "
             f"stable-v1 requires the {STABLE_CPU_FLOOR} floor")
    jolt = isa_baseline["joltInstructionSets"]
    wrong = sorted(f"{option}={jolt.get(option)} (floor {'ON' if want else 'OFF'})"
                   for option, want in JOLT_FLOOR_OPTIONS.items() if jolt.get(option) is not want)
    _require(not wrong, f"{where} ISA baseline disagrees with the {STABLE_CPU_FLOOR} floor: {', '.join(wrong)}")


# --------------------------------------------------------------------------- record

def _read_cache(build_dir: Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    _require(cache.is_file() and not cache.is_symlink(), f"CMakeCache.txt is missing from {build_dir}")
    entries: dict[str, str] = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        match = _CACHE_LINE.match(line)
        if match:
            entries[match.group("key")] = match.group("value")
    return entries


def _cmake_version(cache: dict[str, str]) -> str:
    parts = [cache.get(f"CMAKE_CACHE_{part}_VERSION", "") for part in ("MAJOR", "MINOR", "PATCH")]
    version = ".".join(parts)
    _require(all(p.isdigit() for p in parts), "CMakeCache.txt does not record the CMake version")
    return version


def _compiler(build_dir: Path, cmake_version: str, language: str, required: bool) -> dict[str, str] | None:
    path = build_dir / "CMakeFiles" / cmake_version / f"CMake{language}Compiler.cmake"
    if not path.is_file():
        _require(not required, f"{path} is missing; the {language} compiler identity is unknown")
        return None
    values: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = _CMAKE_SET.match(line.strip())
        if match:
            values.setdefault(match.group("key"), match.group("value"))
    prefix = f"CMAKE_{language}_COMPILER"
    identity = {
        "id": values.get(f"{prefix}_ID", ""),
        "version": values.get(f"{prefix}_VERSION", ""),
        "architecture": values.get(f"{prefix}_ARCHITECTURE_ID", ""),
        "path": values.get(prefix, ""),
    }
    for field, key in (("id", f"{prefix}_ID"), ("version", f"{prefix}_VERSION"), ("path", prefix)):
        _require(bool(identity[field]), f"{path.name} does not record {key}")
    return identity


def _configuration(cache: dict[str, str], configuration: str) -> dict[str, Any]:
    types = cache.get("CMAKE_CONFIGURATION_TYPES", "")
    if types:
        listed = [item for item in types.split(";") if item]
        _require(configuration in listed,
                 f"configuration {configuration} is not in CMAKE_CONFIGURATION_TYPES ({types})")
        return {"buildType": None, "configurationTypes": listed}
    build_type = cache.get("CMAKE_BUILD_TYPE", "")
    _require(build_type == configuration,
             f"single-config CMAKE_BUILD_TYPE '{build_type}' does not equal configuration {configuration}")
    return {"buildType": build_type, "configurationTypes": None}


def _cache_bool(cache: dict[str, str], key: str) -> bool | None:
    if key not in cache:
        return None
    value = cache[key].strip().upper()
    if value in _CMAKE_TRUE:
        return True
    if value in _CMAKE_FALSE:
        return False
    raise ProvenanceError(f"CMakeCache.txt {key}={cache[key]!r} is not a CMake boolean")


def _platform_toolchain(cache: dict[str, str], cxx: dict[str, str]) -> dict[str, str | None]:
    tools_dir = _MSVC_TOOLS_DIR.search(cxx["path"]) if cxx["id"] == "MSVC" else None
    return {
        # Written by the root CMakeLists.txt for Visual Studio generators.
        "windowsSdkVersion": cache.get("SPARK_TOOLCHAIN_WINDOWS_SDK_VERSION") or None,
        "msvcToolsVersion": tools_dir.group("version") if tools_dir else None,
        # CMake persists the selected Visual Studio instance for VS generators.
        "visualStudioInstance": cache.get("CMAKE_GENERATOR_INSTANCE") or None,
        "linker": cache.get("CMAKE_LINKER") or None,
        "archiver": cache.get("CMAKE_AR") or None,
    }


def _isa_baseline(cache: dict[str, str]) -> dict[str, Any]:
    native_arch = _cache_bool(cache, "SPARK_NATIVE_ARCH")
    jolt = {option: _cache_bool(cache, option) for option in JOLT_FLOOR_OPTIONS}
    return {
        "stableFloor": STABLE_CPU_FLOOR,
        "nativeArch": native_arch,
        "joltInstructionSets": jolt,
        "meetsStableFloor": _meets_stable_floor(native_arch, jolt),
    }


def _artifacts(packages: Path, version: str) -> list[dict[str, Any]]:
    _require(packages.is_dir() and not packages.is_symlink(), f"{packages} is not a real directory")
    artifacts = []
    for entry in sorted(packages.iterdir(), key=lambda p: p.name):
        if not _is_distributable(entry.name) or entry.is_dir():
            continue
        _require(entry.is_file() and not entry.is_symlink(), f"{entry.name} is not a regular file")
        if entry.name.startswith(ENGINE_PREFIX):
            _require(entry.name.startswith(f"{ENGINE_PREFIX}{version}-"),
                     f"{entry.name} is not named for engine version {version}")
        artifacts.append({"name": entry.name, "sha256": _sha256_file(entry), "bytes": entry.stat().st_size})
    _require(bool(artifacts), f"no distributable package was found in {packages}")
    return artifacts


def _write_no_replace(path: Path, document: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    payload = (json.dumps(document, indent=2, sort_keys=True) + "\n").encode("utf-8")
    try:
        descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o644)
    except FileExistsError as error:
        raise ProvenanceError(f"{path} already exists; provenance records are never replaced") from error
    with os.fdopen(descriptor, "wb") as stream:
        stream.write(payload)


def record(args: argparse.Namespace) -> dict[str, Any]:
    source_sha = args.source_sha
    _require(bool(_SHA1.fullmatch(source_sha)), "source SHA must be 40 lower-case hexadecimal characters")
    _require(bool(_SEMVER.fullmatch(args.version)), "version must be MAJOR.MINOR.PATCH")
    for label, value in (("profile", args.profile), ("platform", args.platform),
                         ("configuration", args.configuration)):
        _require(bool(_TOKEN.fullmatch(value)), f"{label} must be a simple token")

    head = _git(args.source_root, "rev-parse", "HEAD").decode().strip()
    _require(head == source_sha, f"checked-out commit {head} is not the declared source SHA {source_sha}")
    lock_file = args.source_root / LOCK_PATH
    _require(lock_file.is_file() and not lock_file.is_symlink(), f"{LOCK_PATH} is missing")
    # The digest is of the committed blob, so Windows (autocrlf) and POSIX
    # checkouts record one identity. Git's own filters decide whether the
    # working copy the build consumed still equals that blob.
    unchanged = subprocess.run(["git", "-C", str(args.source_root), "diff", "--quiet", source_sha, "--", LOCK_PATH],
                               capture_output=True, check=False)
    _require(unchanged.returncode == 0,
             f"the dependency lock used by the build differs from {LOCK_PATH} at {source_sha}")
    lock_digest = _committed_lock_digest(args.source_root, source_sha)

    cache = _read_cache(args.build_dir)
    configured = cache.get("SPARK_ENGINE_VERSION", "")
    _require(configured == args.version,
             f"build tree SPARK_ENGINE_VERSION '{configured}' does not equal release version {args.version}")
    generator = cache.get("CMAKE_GENERATOR", "")
    _require(bool(generator), "CMakeCache.txt does not record CMAKE_GENERATOR")
    cmake_version = _cmake_version(cache)
    compilers = {"CXX": _compiler(args.build_dir, cmake_version, "CXX", required=True)}
    c_compiler = _compiler(args.build_dir, cmake_version, "C", required=False)
    if c_compiler is not None:
        compilers["C"] = c_compiler
    platform_toolchain = _platform_toolchain(cache, compilers["CXX"])
    isa_baseline = _isa_baseline(cache)
    if (args.profile, args.platform) == STABLE_PROFILE:
        _require_stable_platform(platform_toolchain, isa_baseline, "the stable-v1 Windows build tree")

    document = {
        "schemaVersion": SCHEMA_VERSION,
        "sourceSHA": source_sha,
        "version": args.version,
        "profile": args.profile,
        "platform": args.platform,
        "configuration": args.configuration,
        "dependencyLock": {"path": LOCK_PATH, "sha256": lock_digest},
        "toolchain": {
            "cmakeVersion": cmake_version,
            "generator": generator,
            "generatorPlatform": cache.get("CMAKE_GENERATOR_PLATFORM") or None,
            "generatorToolset": cache.get("CMAKE_GENERATOR_TOOLSET") or None,
            "compilers": compilers,
        },
        "platformToolchain": platform_toolchain,
        "isaBaseline": isa_baseline,
        "cmakeConfiguration": _configuration(cache, args.configuration),
        "runnerImage": {"os": os.environ.get("ImageOS") or None,
                        "version": os.environ.get("ImageVersion") or None},
        "artifacts": _artifacts(args.packages, args.version),
    }
    _write_no_replace(args.out, document)
    return document


# --------------------------------------------------------------------------- verify

def _reject_duplicates(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    keys = [key for key, _ in pairs]
    duplicates = sorted({key for key in keys if keys.count(key) > 1})
    if duplicates:
        raise ProvenanceError(f"duplicate JSON key(s): {', '.join(duplicates)}")
    return dict(pairs)


def _closed(value: Any, keys: set[str], where: str) -> dict[str, Any]:
    _require(isinstance(value, dict) and set(value) == keys,
             f"{where} does not match the closed schema {sorted(keys)}")
    return value


def _string(value: Any, pattern: re.Pattern[str] | None, where: str) -> str:
    _require(isinstance(value, str) and bool(value), f"{where} must be a non-empty string (schema)")
    if pattern is not None:
        _require(bool(pattern.fullmatch(value)), f"{where} has an invalid value (schema)")
    return value


def _load_record(path: Path) -> dict[str, Any]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicates)
    except json.JSONDecodeError as error:
        raise ProvenanceError(f"{path.name} is not valid JSON: {error}") from error
    _require(isinstance(document, dict), f"{path.name} schema is not a JSON object")
    schema = document.get("schemaVersion")
    _require(schema in (SCHEMA_VERSION, LEGACY_SCHEMA_VERSION), f"{path.name} has an unknown schemaVersion")
    keys = RECORD_KEYS if schema == SCHEMA_VERSION else LEGACY_RECORD_KEYS
    record_doc = _closed(document, keys, f"{path.name} schema")
    for key in ("profile", "platform", "configuration"):
        _string(record_doc[key], _TOKEN, f"{path.name} {key}")
    lock = _closed(record_doc["dependencyLock"], {"path", "sha256"}, f"{path.name} dependencyLock schema")
    _require(lock["path"] == LOCK_PATH, f"{path.name} dependencyLock path is not {LOCK_PATH}")
    _string(lock["sha256"], _SHA256, f"{path.name} dependencyLock sha256")
    toolchain = _closed(record_doc["toolchain"], TOOLCHAIN_KEYS, f"{path.name} toolchain schema")
    _string(toolchain["cmakeVersion"], None, f"{path.name} cmakeVersion")
    _string(toolchain["generator"], None, f"{path.name} generator")
    compilers = toolchain["compilers"]
    _require(isinstance(compilers, dict) and "CXX" in compilers and set(compilers) <= {"C", "CXX"},
             f"{path.name} compilers schema requires CXX")
    for language, identity in compilers.items():
        identity = _closed(identity, COMPILER_KEYS, f"{path.name} {language} compiler schema")
        for field in ("id", "version", "path"):
            _string(identity[field], None, f"{path.name} {language} compiler {field}")
    _closed(record_doc["cmakeConfiguration"], {"buildType", "configurationTypes"},
            f"{path.name} cmakeConfiguration schema")
    if schema == SCHEMA_VERSION:
        _check_platform_schema(record_doc, path.name)
    _closed(record_doc["runnerImage"], {"os", "version"}, f"{path.name} runnerImage schema")
    artifacts = record_doc["artifacts"]
    _require(isinstance(artifacts, list) and bool(artifacts), f"{path.name} records no artifacts (schema)")
    for artifact in artifacts:
        artifact = _closed(artifact, {"name", "sha256", "bytes"}, f"{path.name} artifact schema")
        _string(artifact["name"], None, f"{path.name} artifact name")
        _string(artifact["sha256"], _SHA256, f"{path.name} artifact sha256")
        _require(isinstance(artifact["bytes"], int) and artifact["bytes"] >= 0,
                 f"{path.name} artifact bytes must be a non-negative integer (schema)")
    return record_doc


def _check_platform_schema(record_doc: dict[str, Any], name: str) -> None:
    platform_toolchain = _closed(record_doc["platformToolchain"], PLATFORM_TOOLCHAIN_KEYS,
                                 f"{name} platformToolchain schema")
    for key, value in platform_toolchain.items():
        if value is not None:
            _string(value, None, f"{name} platformToolchain {key}")
    isa = _closed(record_doc["isaBaseline"], ISA_BASELINE_KEYS, f"{name} isaBaseline schema")
    _require(isa["stableFloor"] == STABLE_CPU_FLOOR, f"{name} isaBaseline stableFloor is not {STABLE_CPU_FLOOR}")
    _require(isa["nativeArch"] is None or isinstance(isa["nativeArch"], bool),
             f"{name} isaBaseline nativeArch must be a boolean or null (schema)")
    jolt = _closed(isa["joltInstructionSets"], set(JOLT_FLOOR_OPTIONS), f"{name} joltInstructionSets schema")
    _require(all(value is None or isinstance(value, bool) for value in jolt.values()),
             f"{name} joltInstructionSets values must be booleans or null (schema)")
    _require(isa["meetsStableFloor"] is _meets_stable_floor(isa["nativeArch"], jolt),
             f"{name} isaBaseline meetsStableFloor contradicts its recorded instruction sets")
    if (record_doc["profile"], record_doc["platform"]) == STABLE_PROFILE:
        _require_stable_platform(platform_toolchain, isa, name)


def verify(args: argparse.Namespace) -> tuple[int, int]:
    _require(bool(_SHA1.fullmatch(args.source_sha)), "source SHA must be 40 lower-case hexadecimal characters")
    head = _git(args.source_root, "rev-parse", "HEAD").decode().strip()
    _require(head == args.source_sha, f"release checkout {head} is not the declared source SHA {args.source_sha}")
    expected_lock = _committed_lock_digest(args.source_root, args.source_sha)

    records_dir: Path = args.records_dir
    paths = sorted(records_dir.rglob("*.json")) if records_dir.is_dir() else []
    _require(bool(paths), f"no build provenance records were found under {records_dir}")
    records = []
    for path in paths:
        _require(path.is_file() and not path.is_symlink(), f"{path} is not a regular file")
        record_doc = _load_record(path)
        _require(record_doc["sourceSHA"] == args.source_sha,
                 f"{path.name} sourceSHA {record_doc['sourceSHA']} is not {args.source_sha}")
        _require(record_doc["version"] == args.version,
                 f"{path.name} version {record_doc['version']} is not {args.version}")
        _require(record_doc["dependencyLock"]["sha256"] == expected_lock,
                 f"{path.name} dependency lock digest differs from {LOCK_PATH} at {args.source_sha}")
        records.append(record_doc)

    if args.stable:
        _require(len(records) == 1, f"stable publication requires exactly one record; found {len(records)}")
        only = records[0]
        _require((only["profile"], only["platform"], only["configuration"]) == (*STABLE_PROFILE, "MinSizeRel"),
                 "stable publication requires the stable-v1 Windows MinSizeRel record")
        # _load_record already enforced the toolchain and CPU floor for a v2
        # stable-v1 Windows record; v1 cannot carry them, so it cannot publish.
        _require(only["schemaVersion"] == SCHEMA_VERSION,
                 f"stable publication requires a {SCHEMA_VERSION} record (Windows SDK, MSVC toolset, CPU baseline)")

    recorded: dict[str, set[str]] = {}
    for record_doc in records:
        for artifact in record_doc["artifacts"]:
            recorded.setdefault(artifact["sha256"], set()).add(artifact["name"])

    names = [line.strip() for line in args.assets_file.read_text(encoding="utf-8").splitlines() if line.strip()]
    published: dict[str, str] = {}
    for name in names:
        if not _is_distributable(name):
            continue
        _require("/" not in name and "\\" not in name and name not in (".", ".."), f"invalid asset name {name}")
        asset = args.assets_dir / name
        _require(asset.is_file() and not asset.is_symlink(), f"published asset {name} is not a regular file")
        published[name] = _sha256_file(asset)
    _require(bool(published), "the release asset list contains no distributable asset")

    uncovered = sorted(name for name, digest in published.items() if digest not in recorded)
    _require(not uncovered, f"published asset(s) not covered by build provenance: {', '.join(uncovered)}")
    published_digests = set(published.values())
    unpublished = sorted(name for digest, recorded_names in recorded.items()
                         if digest not in published_digests for name in recorded_names)
    _require(not unpublished, f"recorded artifact(s) not published: {', '.join(unpublished)}")
    return len(records), len(published)


# --------------------------------------------------------------------------- cli

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    rec = commands.add_parser("record", help="record provenance for one job's final package bytes")
    rec.add_argument("--source-root", type=Path, required=True)
    rec.add_argument("--source-sha", required=True)
    rec.add_argument("--version", required=True)
    rec.add_argument("--profile", required=True)
    rec.add_argument("--platform", required=True)
    rec.add_argument("--configuration", required=True)
    rec.add_argument("--build-dir", type=Path, required=True)
    rec.add_argument("--packages", type=Path, required=True)
    rec.add_argument("--out", type=Path, required=True)

    ver = commands.add_parser("verify", help="verify records cover exactly the published distributables")
    ver.add_argument("--records-dir", type=Path, required=True)
    ver.add_argument("--assets-dir", type=Path, required=True)
    ver.add_argument("--assets-file", type=Path, required=True)
    ver.add_argument("--source-root", type=Path, required=True)
    ver.add_argument("--source-sha", required=True)
    ver.add_argument("--version", required=True)
    ver.add_argument("--stable", action="store_true", help="require the single stable-v1 Windows Shipping record")

    args = parser.parse_args(argv)
    try:
        if args.command == "record":
            document = record(args)
            print(f"Recorded build provenance for {len(document['artifacts'])} artifact(s) at {args.out}")
        else:
            record_count, asset_count = verify(args)
            print(f"Verified {record_count} build provenance record(s) covering "
                  f"{asset_count} distributable asset(s) at {args.source_sha}")
    except (ProvenanceError, OSError) as error:
        print(f"BUILD PROVENANCE FAILURE: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
