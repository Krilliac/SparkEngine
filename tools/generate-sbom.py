#!/usr/bin/env python3
"""SPDX 2.3 SBOM generation and package-inventory reconciliation (SEC-110).

``generate`` (the default) writes a deterministic SPDX-2.3 JSON document that
describes every third-party dependency recorded in ThirdParty/dependencies.lock
and ThirdParty/supply-chain.lock: name, locked version or gitlink revision,
source location, SPDX license (resolved by the reviewed ``license_policy`` of
tools/check-supply-chain.py, never guessed), and the lockfile's integrity pin
(submodule gitlink or vendored tree digest).

So that a vulnerability scanner can match the document (the PR-time
``dependency-policy`` job scans it with grype), every dependency's
``license_policy`` record must carry an NVD CPE, emitted as a ``cpe23Type``
reference, or a reviewed ``cpe_unavailable_reason``. A submodule, whose pin is a
commit no advisory names, must also record the upstream release it derives
from (``upstream_version``, ``+N`` for N commits past it) with its evidence: the
release tag's commit, checked against the gitlink, or a reviewed source. The
upstream release is the package version and a ``pkg:generic`` purl.

The document is bound to a source commit the same way REL-100 build provenance
is: it records the commit SHA and the SHA-256 of the *committed*
dependencies.lock blob, computed by tools/release_build_provenance.py. A
provenance record and an SBOM for the same release therefore join on
``dependencyLock.sha256`` without a second identity scheme. Generation refuses
a checkout whose lockfiles differ from the committed blobs, and ``--check``
regenerates the document and requires byte equality, so a consumer holding the
source checkout can verify an SBOM it was given.

``reconcile`` compares a shipped package with the lock. The package is either
a CMake install manifest (install_manifest.txt) or a staged package tree.
Every file is classified with the package rule set shared with the GOV-400
notice-coverage gate (cmake/PackageNoticeCoverageRules.json), so there is one
definition of "third-party payload". Reconciliation fails when:

  * a file under a third-party install root maps to no rule;
  * a file maps to a component that dependencies.lock does not lock;
  * a rule names a component that dependencies.lock does not lock;
  * a locked component with install payload rules ships no file, unless the
    caller names it with ``--not-configured`` (and then it must really be
    absent, so a stale declaration also fails);
  * the package's THIRD_PARTY_NOTICES.txt inventory does not list exactly the
    locked dependencies at their locked versions (a package built from a
    different lock).

Header-only dependencies that are compiled into binaries and install no file
of their own cannot be observed in an inventory; they are reported as
``compiledInOnly`` rather than claimed as verified.

Exit codes: 0 success, 1 policy failure (reconciliation errors, stale
``--check``, lock inconsistency), 2 malformed or unreadable input.

Usage:
  python3 tools/generate-sbom.py --out sbom.spdx.json
  python3 tools/generate-sbom.py --check sbom.spdx.json
  python3 tools/generate-sbom.py reconcile --install-manifest build/linux-gcc-release/install_manifest.txt
  python3 tools/generate-sbom.py reconcile --package-root stage --not-configured SDL2
  python3 tools/generate-sbom.py reconcile --archive SparkEngine-1.2.3.zip --json-report reconcile-final.json
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from types import ModuleType
from typing import Any
from urllib.parse import quote

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parent

SPDX_VERSION = "SPDX-2.3"
TOOL_NAME = "SparkEngine-generate-sbom-1"
ROOT_SPDX_ID = "SPDXRef-SparkEngine"
RECONCILE_SCHEMA = "spark-package-inventory-reconciliation-v1"
# Font entries cmake/SparkThirdPartyAudit.cmake writes into the packaged inventory. Fonts are
# covered by the GOV-400 notice gate, not dependencies.lock, so they are reported, not failed.
FONT_ENTRY_PATTERN = re.compile(r"^.+ \((?:editor font|font embedded in .+)\)$")
SOURCE_LICENSE_PATH = "LICENSE"
ENGINE_VERSION_RE = re.compile(r'^set\(SPARK_ENGINE_VERSION "([0-9]+\.[0-9]+\.[0-9]+)" CACHE', re.MULTILINE)
GITHUB_URL_RE = re.compile(r"^https://github\.com/([A-Za-z0-9._-]+)/([A-Za-z0-9._-]+?)(?:\.git)?/?$")
SHA1_RE = re.compile(r"^[0-9a-f]{40}$")
NOTICE_NAME = "THIRD_PARTY_NOTICES.txt"
MAX_INSTALL_MANIFEST_BYTES = 64 << 20
MAX_INSTALL_MANIFEST_LINES = 1_000_000
MAX_PACKAGE_FILES = 1_000_000
MAX_NOTICE_BYTES = 8 << 20
MAX_ARCHIVE_BYTES = 8 << 30
MAX_ARCHIVE_MEMBER_BYTES = 4 << 30
MAX_ARCHIVE_MEMBERS = 1_000_000


class SbomError(Exception):
    """A policy failure: the lock, the checkout, or the package is not consistent."""


class InputError(Exception):
    """A malformed or unreadable input; the tool cannot reach a verdict."""


def _load_tool(name: str, path: Path) -> ModuleType:
    spec = importlib.util.spec_from_file_location(name, str(path))
    if spec is None or spec.loader is None:
        raise InputError(f"cannot load {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


# One parser per input: the supply-chain checker expands dependencies.lock
# through CMake and resolves licenses, the provenance tool defines the lock
# digest, and the notices generator owns the package rule set.
supply_chain = _load_tool("spark_sbom_check_supply_chain", TOOLS_DIR / "check-supply-chain.py")
provenance = _load_tool("spark_sbom_release_build_provenance", TOOLS_DIR / "release_build_provenance.py")
notices = _load_tool("spark_sbom_third_party_notices", TOOLS_DIR / "governance" / "generate_third_party_notices.py")


@dataclass(frozen=True)
class Dependency:
    name: str
    source: str
    version: str
    declared_license: str
    spdx_license: str
    local_path: str
    kind: str  # "submodule" or "vendored"
    pin: str  # gitlink revision, or vendored tree digest
    file_count: int | None
    # Vulnerability identity from the reviewed license_policy record.
    upstream_version: str | None = None
    cpe: str | None = None
    cpe_unavailable_reason: str | None = None


# --------------------------------------------------------------------------- lock inventory


def load_inventory(root: Path) -> list[Dependency]:
    """Read both lockfiles and require that they describe the same dependency set."""
    root_resolved = root.resolve()
    lockfile = supply_chain.load_lockfile(root, root_resolved)
    entries = supply_chain.export_manifest_entries(root, root_resolved)

    licenses = supply_chain.CheckResult()
    resolved = {item["name"]: item["spdx"] for item in supply_chain.check_license_policy(lockfile, entries, licenses)}
    errors = [f"{v.path}: {v.message}" for v in licenses.violations if v.severity == "error"]

    policy: dict[str, dict[str, str]] = lockfile.get("license_policy", {}).get("dependencies", {})
    gitlinks: dict[str, str] = lockfile["submodule_gitlinks"]
    vendored: list[str] = lockfile["managed_vendored_dirs"]
    digests: dict[str, dict[str, Any]] = lockfile["tree_digests"]
    dependencies: list[Dependency] = []
    names: set[str] = set()
    paths: set[str] = set()
    for fields in entries:
        if len(fields) != supply_chain.MANIFEST_FIELD_COUNT:
            errors.append(f"{supply_chain.MANIFEST_REL}: entry has {len(fields)} fields: {'|'.join(fields)[:80]}")
            continue
        name = fields[supply_chain.F_NAME].strip()
        local_path = fields[supply_chain.F_LOCAL_PATH].strip()
        version = fields[supply_chain.F_VERSION].strip()
        if not name or not fields[supply_chain.F_LICENSE].strip():
            errors.append(f"{supply_chain.MANIFEST_REL}: entry for {local_path or '?'} has an empty name or license")
            continue
        if name in names or local_path in paths:
            errors.append(f"{supply_chain.MANIFEST_REL}: duplicate dependency {name!r} or path {local_path!r}")
            continue
        names.add(name)
        paths.add(local_path)
        if local_path in gitlinks:
            kind, pin, file_count = "submodule", gitlinks[local_path], None
            if version != pin:
                errors.append(
                    f"{name}: dependencies.lock renders revision {version!r} but supply-chain.lock "
                    f"pins gitlink {pin} for {local_path}"
                )
        elif local_path in vendored:
            record = digests.get(local_path)
            if not isinstance(record, dict) or "digest" not in record:
                errors.append(f"{name}: supply-chain.lock has no tree digest for vendored {local_path}")
                continue
            kind, pin, file_count = "vendored", record["digest"], record.get("file_count")
        else:
            errors.append(
                f"{name}: {local_path} is neither a locked submodule gitlink nor a managed vendored "
                "directory in supply-chain.lock"
            )
            continue
        if name not in resolved:
            # Never drop a locked dependency silently: an entry the license policy did not resolve is an error
            # even when check_license_policy skipped it without recording a violation.
            errors.append(f"{name}: no resolved SPDX license")
            continue
        record = policy.get(name, {})
        identity_errors = _identity_errors(name, kind, pin, record)
        if identity_errors:
            errors.extend(identity_errors)
            continue
        dependencies.append(
            Dependency(
                name=name,
                source=fields[supply_chain.F_SOURCE].strip(),
                version=version,
                declared_license=fields[supply_chain.F_LICENSE].strip(),
                spdx_license=resolved[name],
                local_path=local_path,
                kind=kind,
                pin=pin,
                file_count=file_count,
                upstream_version=record.get("upstream_version"),
                cpe=record.get("cpe"),
                cpe_unavailable_reason=record.get("cpe_unavailable_reason"),
            )
        )

    for container in sorted(set(gitlinks) | set(vendored)):
        if container not in paths:
            errors.append(f"{container} is locked in supply-chain.lock but has no dependencies.lock entry")
    if errors:
        raise SbomError("lock inventory is inconsistent:\n  " + "\n  ".join(errors))
    return sorted(dependencies, key=lambda dep: dep.name.lower())


def _identity_errors(name: str, kind: str, pin: str, record: dict[str, str]) -> list[str]:
    """Why a dependency cannot be matched by a vulnerability scan of this SBOM (empty when it can).

    Every dependency needs a CPE or a reviewed reason there is none. A submodule
    is pinned to a commit, which no advisory names, so it also needs the upstream
    release it derives from. When that release is backed by a tag commit, the
    version says whether the gitlink is that commit ("2.32.0") or past it
    ("2.32.0+231", 231 commits later).
    """
    where = f"{supply_chain.LOCKFILE_REL} license_policy.dependencies[{name!r}]"
    errors = []
    if "cpe" not in record and "cpe_unavailable_reason" not in record:
        errors.append(f"{name}: {where} names no cpe or cpe_unavailable_reason, so no scanner can match it")
    if kind != "submodule":
        return errors
    version = record.get("upstream_version")
    if version is None or not ({"upstream_tag_commit", "upstream_version_source"} & record.keys()):
        errors.append(
            f"{name}: submodule pinned to commit {pin} needs an upstream_version and its evidence "
            f"(upstream_tag_commit or upstream_version_source) in {where}"
        )
        return errors
    tag_commit = record.get("upstream_tag_commit")
    ahead = supply_chain.UPSTREAM_VERSION_RE.fullmatch(version).group("ahead")
    if tag_commit == pin and ahead:
        errors.append(f"{name}: the gitlink is the release tag commit {tag_commit}; upstream_version {version!r} "
                      "must not claim commits past it")
    elif tag_commit is not None and tag_commit != pin and not ahead:
        errors.append(f"{name}: upstream_version {version!r} names the release itself, but the gitlink {pin} is not "
                      f"its tag commit {tag_commit}; record the commits past it as +N")
    return errors


# --------------------------------------------------------------------------- SPDX document


def _git_text(root: Path, *args: str) -> str:
    try:
        return provenance._git(root, *args).decode("utf-8")
    except provenance.ProvenanceError as error:
        raise InputError(str(error)) from error


def _spdx_id(name: str) -> str:
    return "SPDXRef-Package-" + re.sub(r"[^A-Za-z0-9.-]+", "-", name).strip("-")


def _source_url(source: str) -> str | None:
    token = source.split()[0] if source.split() else ""
    return token if token.startswith("https://") else None


def _package(dep: Dependency) -> dict[str, Any]:
    url = _source_url(dep.source)
    github = GITHUB_URL_RE.match(url) if url else None
    if dep.kind == "submodule":
        location = f"git+{url}@{dep.pin}" if url else "NOASSERTION"
        purl_version: str | None = dep.pin
        source_info = f"Git submodule {dep.local_path} pinned to gitlink {dep.pin} by ThirdParty/supply-chain.lock."
    else:
        location = url or "NOASSERTION"
        # The lock records a vendored snapshot's version as reviewed text, not
        # as a verified upstream ref, so no purl version is asserted for it.
        purl_version = None
        source_info = (
            f"Vendored tree {dep.local_path} pinned by ThirdParty/supply-chain.lock tree digest "
            f"sha256:{dep.pin} over {dep.file_count} tracked file(s)."
        )
    package: dict[str, Any] = {
        "SPDXID": _spdx_id(dep.name),
        "name": dep.name,
        # Scanners compare this with advisory ranges, so it is the upstream
        # release when one is recorded; the lock pin stays in sourceInfo.
        "versionInfo": dep.upstream_version or dep.version,
        "downloadLocation": location,
        "filesAnalyzed": False,
        "licenseConcluded": dep.spdx_license,
        "licenseDeclared": dep.spdx_license,
        "copyrightText": "NOASSERTION",
        "supplier": "NOASSERTION",
        "primaryPackagePurpose": "LIBRARY",
        "sourceInfo": source_info,
        "comment": f"dependencies.lock declares license {dep.declared_license!r} and version {dep.version!r}; "
        f"source {dep.source}.",
    }
    references = []
    if dep.cpe:
        references.append({"referenceCategory": "SECURITY", "referenceType": "cpe23Type", "referenceLocator": dep.cpe})
    else:
        package["comment"] += f" No CPE: {dep.cpe_unavailable_reason}"
    if github and purl_version:
        owner, repo = github.group(1).lower(), github.group(2).lower()
        references.append(
            {
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": f"pkg:github/{owner}/{repo}@{purl_version}",
            }
        )
    if dep.upstream_version:
        references.append(
            {
                "referenceCategory": "PACKAGE-MANAGER",
                "referenceType": "purl",
                "referenceLocator": f"pkg:generic/{quote(dep.name, safe='')}@{quote(dep.upstream_version, safe='.')}",
            }
        )
    if references:
        package["externalRefs"] = references
    return package


def generate(root: Path, source_sha: str | None = None) -> dict[str, Any]:
    """Build the SPDX document for the committed lock state at ``source_sha`` (HEAD by default)."""
    head = _git_text(root, "rev-parse", "HEAD").strip()
    sha = source_sha or head
    if not SHA1_RE.match(sha):
        raise InputError("source SHA must be 40 lower-case hexadecimal characters")
    if sha != head:
        raise SbomError(f"checked-out commit {head} is not the declared source SHA {sha}")
    for rel in (provenance.LOCK_PATH, supply_chain.LOCKFILE_REL):
        unchanged = subprocess.run(
            ["git", "-C", str(root), "diff", "--quiet", sha, "--", rel], capture_output=True, check=False
        )
        if unchanged.returncode != 0:
            raise SbomError(f"{rel} in the working tree differs from the committed blob at {sha}")

    dependencies = load_inventory(root)
    lock_digest = provenance._committed_lock_digest(root, sha)
    supply_digest = hashlib.sha256(provenance._git(root, "show", f"{sha}:{supply_chain.LOCKFILE_REL}")).hexdigest()
    committed = int(_git_text(root, "show", "-s", "--format=%ct", sha).strip())
    created = datetime.fromtimestamp(committed, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    cmake_lists = _git_text(root, "show", f"{sha}:CMakeLists.txt")
    version_match = ENGINE_VERSION_RE.search(cmake_lists)
    if not version_match:
        raise InputError("CMakeLists.txt does not set SPARK_ENGINE_VERSION")
    license_text = _git_text(root, "show", f"{sha}:{SOURCE_LICENSE_PATH}").replace("\r\n", "\n")
    license_name = license_text.split("\n", 1)[0].strip()
    if not license_name:
        raise InputError(f"{SOURCE_LICENSE_PATH} has no license title on its first line")
    license_ref = "LicenseRef-" + re.sub(r"[^A-Za-z0-9.-]+", "-", license_name).strip("-")

    binding = f"sourceSHA {sha}; dependencyLock {provenance.LOCK_PATH} sha256 {lock_digest}"
    root_package = {
        "SPDXID": ROOT_SPDX_ID,
        "name": "SparkEngine",
        "versionInfo": version_match.group(1),
        "downloadLocation": "NOASSERTION",
        "filesAnalyzed": False,
        "licenseConcluded": license_ref,
        "licenseDeclared": license_ref,
        "copyrightText": "NOASSERTION",
        "supplier": "NOASSERTION",
        "primaryPackagePurpose": "APPLICATION",
        "sourceInfo": f"Built from {binding}.",
    }
    packages = [root_package] + [_package(dep) for dep in dependencies]
    relationships = [
        {"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES", "relatedSpdxElement": ROOT_SPDX_ID}
    ] + [
        {"spdxElementId": ROOT_SPDX_ID, "relationshipType": "DEPENDS_ON", "relatedSpdxElement": _spdx_id(dep.name)}
        for dep in dependencies
    ]
    return {
        "spdxVersion": SPDX_VERSION,
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": f"SparkEngine-{version_match.group(1)}-{sha[:12]}",
        "documentNamespace": f"urn:spark-engine:spdx:{sha}:{lock_digest}",
        "creationInfo": {
            "created": created,
            "creators": [f"Tool: {TOOL_NAME}"],
            "comment": (
                f"Generated by tools/generate-sbom.py from ThirdParty/dependencies.lock and "
                f"ThirdParty/supply-chain.lock (sha256 {supply_digest}) at {binding}. The dependencyLock "
                "digest is the one REL-100 build provenance records (tools/release_build_provenance.py)."
            ),
        },
        "hasExtractedLicensingInfos": [
            {"licenseId": license_ref, "name": license_name, "extractedText": license_text}
        ],
        "packages": packages,
        "relationships": relationships,
    }


def render(document: dict[str, Any]) -> str:
    return json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def _write_atomic(target: Path, text: str) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    handle, temp = tempfile.mkstemp(prefix=".sbom-", dir=str(target.parent))
    try:
        with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
        os.replace(temp, target)
    except BaseException:
        Path(temp).unlink(missing_ok=True)
        raise


# --------------------------------------------------------------------------- reconciliation


def _read_install_manifest(manifest: Path, prefix: str | None) -> tuple[list[str], Path]:
    """Return package-relative paths from a CMake install manifest and the install prefix."""
    if not manifest.is_file() or manifest.is_symlink():
        raise InputError(f"{manifest} is missing or is not a regular non-link file")
    if manifest.stat().st_size > MAX_INSTALL_MANIFEST_BYTES:
        raise InputError(f"{manifest} exceeds {MAX_INSTALL_MANIFEST_BYTES} bytes")
    try:
        lines = [line.strip() for line in manifest.read_text(encoding="utf-8").splitlines() if line.strip()]
    except UnicodeDecodeError as error:
        raise InputError(f"{manifest}: not UTF-8: {error}") from error
    if not lines:
        raise InputError(f"{manifest} lists no installed file")
    if len(lines) > MAX_INSTALL_MANIFEST_LINES:
        raise InputError(f"{manifest} lists more than {MAX_INSTALL_MANIFEST_LINES} files")
    lines = [line.replace("\\", "/") for line in lines]

    if prefix is None:
        # The package-level notice sits at the install root; nested copies
        # (for example the SDK's) are longer, so the shortest one is the root.
        candidates = sorted((line for line in lines if line.endswith("/" + NOTICE_NAME)), key=len)
        if not candidates:
            raise InputError(f"{manifest} installs no {NOTICE_NAME}; pass --install-prefix")
        prefix = candidates[0][: -len("/" + NOTICE_NAME)]
    prefix = prefix.replace("\\", "/").rstrip("/")
    if not prefix:
        raise InputError("install prefix is empty")

    relative: list[str] = []
    for line in lines:
        if not line.startswith(prefix + "/"):
            raise InputError(f"{manifest}: {line} is outside install prefix {prefix}")
        rel = line[len(prefix) + 1 :]
        if not rel or any(part in ("", ".", "..") for part in rel.split("/")):
            raise InputError(f"{manifest}: {line} is not a normalized path under {prefix}")
        relative.append(rel)
    return sorted(set(relative)), Path(prefix)


def _walk_package(package_root: Path) -> list[str]:
    if not package_root.is_dir() or package_root.is_symlink():
        raise InputError(f"{package_root} is not a real directory")
    files: list[str] = []
    for directory, subdirs, names in os.walk(package_root):
        base = Path(directory)
        # A symlinked directory is an entry of the package, never descended into.
        for sub in list(subdirs):
            if (base / sub).is_symlink():
                subdirs.remove(sub)
                names.append(sub)
        for name in names:
            files.append((base / name).relative_to(package_root).as_posix())
            if len(files) > MAX_PACKAGE_FILES:
                raise InputError(f"{package_root} holds more than {MAX_PACKAGE_FILES} files")
    return sorted(files)


WINDOWS_RESERVED_NAMES = {"con", "prn", "aux", "nul", *(f"com{i}" for i in range(1, 10)), *(f"lpt{i}" for i in range(1, 10))}


def _archive_member_path(name: str) -> tuple[Path, str]:
    """Return a safe relative extraction path and case-folded collision key."""
    if "\\" in name or any(ord(character) < 32 for character in name):
        raise InputError(f"archive member {name!r} contains an unsafe path character")
    normalized = name
    raw_parts = normalized.split("/")
    if raw_parts and raw_parts[-1] == "":
        raw_parts.pop()
    if not normalized or normalized.startswith("/") or any(part in ("", ".", "..") for part in raw_parts):
        raise InputError(f"archive member {name!r} is not a normalized relative path")
    for part in raw_parts:
        if ":" in part or part.endswith((".", " ")):
            raise InputError(f"archive member {name!r} contains an unsafe Windows path component")
        if part.split(".", 1)[0].casefold() in WINDOWS_RESERVED_NAMES:
            raise InputError(f"archive member {name!r} uses a reserved Windows path component")
    path = Path(*raw_parts)
    return path, "/".join(raw_parts).casefold()


def _safe_archive_target(root: Path, member: Path) -> Path:
    target = (root / member).resolve()
    if root.resolve() not in target.parents:
        raise InputError(f"archive member {member.as_posix()!r} escapes extraction root")
    return target


def _archive_package_root(extracted: Path) -> Path:
    """Find the package root containing the generated notices inventory."""
    if (extracted / NOTICE_NAME).is_file():
        return extracted
    children = list(extracted.iterdir())
    if len(children) != 1 or not children[0].is_dir() or children[0].is_symlink():
        raise InputError(f"archive must be flat or contain exactly one top-level package directory with {NOTICE_NAME}")
    package_root = children[0]
    if not (package_root / NOTICE_NAME).is_file() or (package_root / NOTICE_NAME).is_symlink():
        raise InputError(f"archive package directory must contain {NOTICE_NAME} directly")
    return package_root


def _extract_zip(archive: Path, destination: Path) -> None:
    with zipfile.ZipFile(archive) as bundle:
        members = bundle.infolist()
        if len(members) > MAX_ARCHIVE_MEMBERS:
            raise InputError(f"{archive} contains more than {MAX_ARCHIVE_MEMBERS} members")
        total = 0
        seen: set[str] = set()
        for info in members:
            member, key = _archive_member_path(info.filename)
            if key in seen:
                raise InputError(f"archive contains duplicate path {info.filename!r}")
            seen.add(key)
            mode = (info.external_attr >> 16) & 0o170000
            if info.is_dir():
                if mode not in (0, 0o040000):
                    raise InputError(f"archive member {info.filename!r} is not a regular directory")
                _safe_archive_target(destination, member).mkdir(parents=True, exist_ok=True)
                continue
            if mode not in (0, 0o100000):
                raise InputError(f"archive member {info.filename!r} is not a regular file")
            if info.file_size > MAX_ARCHIVE_MEMBER_BYTES:
                raise InputError(f"archive member {info.filename!r} exceeds {MAX_ARCHIVE_MEMBER_BYTES} bytes")
            total += info.file_size
            if total > MAX_ARCHIVE_BYTES:
                raise InputError(f"archive expands beyond {MAX_ARCHIVE_BYTES} bytes")
            target = _safe_archive_target(destination, member)
            target.parent.mkdir(parents=True, exist_ok=True)
            with bundle.open(info) as source, target.open("xb") as output:
                while chunk := source.read(1024 * 1024):
                    output.write(chunk)


def _extract_tar(archive: Path, destination: Path) -> None:
    with tarfile.open(archive, mode="r:*") as bundle:
        total = 0
        seen: set[str] = set()
        for index, info in enumerate(bundle):
            if index >= MAX_ARCHIVE_MEMBERS:
                raise InputError(f"{archive} contains more than {MAX_ARCHIVE_MEMBERS} members")
            member, key = _archive_member_path(info.name)
            if key in seen:
                raise InputError(f"archive contains duplicate path {info.name!r}")
            seen.add(key)
            target = _safe_archive_target(destination, member)
            if info.isdir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            if not info.isfile():
                raise InputError(f"archive member {info.name!r} is not a regular file or directory")
            if info.size > MAX_ARCHIVE_MEMBER_BYTES:
                raise InputError(f"archive member {info.name!r} exceeds {MAX_ARCHIVE_MEMBER_BYTES} bytes")
            total += info.size
            if total > MAX_ARCHIVE_BYTES:
                raise InputError(f"archive expands beyond {MAX_ARCHIVE_BYTES} bytes")
            target.parent.mkdir(parents=True, exist_ok=True)
            source = bundle.extractfile(info)
            if source is None:
                raise InputError(f"archive member {info.name!r} cannot be read")
            with source, target.open("xb") as output:
                while chunk := source.read(1024 * 1024):
                    output.write(chunk)


def _archive_format(archive: Path) -> str:
    lowered = archive.name.lower()
    if lowered.endswith(".zip"):
        return "zip"
    if lowered.endswith((".tar.gz", ".tgz")):
        return "tar.gz"
    raise InputError(f"{archive}: final archive must use .zip, .tar.gz, or .tgz")


def _archive_report(
    report: dict[str, Any], archive: Path, source_root: Path, artifact_digest: str | None = None
) -> dict[str, Any]:
    try:
        source_sha = _git_text(source_root, "rev-parse", "HEAD").strip()
        lock_sha = provenance._committed_lock_digest(source_root, source_sha)
    except (InputError, provenance.ProvenanceError) as error:
        raise InputError(f"cannot bind archive report to source lock: {error}") from error
    digest = artifact_digest
    if digest is None:
        digest_builder = hashlib.sha256()
        with archive.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest_builder.update(chunk)
        digest = digest_builder.hexdigest()
    report["artifact"] = {
        "name": archive.name,
        "format": _archive_format(archive),
        "sha256": digest,
    }
    report["source"] = {"sha": source_sha, "dependencyLockSha256": lock_sha}
    return report


def reconcile_archive(
    inventory: list[Dependency],
    rules: Any,
    archive: Path,
    not_configured: list[str],
    source_root: Path = REPO_ROOT,
) -> dict[str, Any]:
    """Extract and reconcile one final archive, binding the result to its bytes and source lock."""
    if not archive.is_file() or archive.is_symlink():
        raise InputError(f"{archive} is missing or is not a regular non-link file")
    if archive.stat().st_size > MAX_ARCHIVE_BYTES:
        raise InputError(f"{archive} exceeds {MAX_ARCHIVE_BYTES} bytes")
    format_name = _archive_format(archive)
    archive_digest = hashlib.sha256()
    with archive.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            archive_digest.update(chunk)
    before_digest = archive_digest.hexdigest()
    with tempfile.TemporaryDirectory(prefix="spark-sbom-archive-") as raw:
        extracted = Path(raw)
        try:
            if format_name == "zip":
                _extract_zip(archive, extracted)
            else:
                _extract_tar(archive, extracted)
        except (OSError, tarfile.TarError, zipfile.BadZipFile) as error:
            raise InputError(f"cannot safely extract {archive}: {error}") from error
        package_root = _archive_package_root(extracted)
        report = reconcile(
            inventory, rules, _walk_package(package_root), package_root / NOTICE_NAME, not_configured
        )
    after_digest = hashlib.sha256()
    with archive.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            after_digest.update(chunk)
    if after_digest.hexdigest() != before_digest:
        raise InputError(f"{archive} changed while it was being reconciled")
    return _archive_report(report, archive, source_root, after_digest.hexdigest())


def _notice_inventory(path: Path) -> dict[str, str]:
    """Map each THIRD_PARTY_NOTICES.txt inventory entry name to its Version line."""
    if not path.is_file() or path.is_symlink():
        raise SbomError(f"package notice {path} is missing or is not a regular non-link file")
    if path.stat().st_size > MAX_NOTICE_BYTES:
        raise InputError(f"{path} exceeds {MAX_NOTICE_BYTES} bytes")
    text = path.read_bytes().decode("utf-8", errors="strict").replace("\r\n", "\n")
    start = text.find(notices.PACKAGE_INVENTORY_MARKER)
    end = text.find(notices.PACKAGE_TEXTS_MARKER)
    if start < 0 or end < start:
        raise InputError(f"{path} has no generated dependency inventory section")
    inventory: dict[str, str] = {}
    current: str | None = None
    for line in text[start + len(notices.PACKAGE_INVENTORY_MARKER) : end].split("\n"):
        if not line:
            current = None
        elif current is None:
            if line.startswith(" "):
                raise InputError(f"{path}: inventory field without an entry name: {line!r}")
            if line in inventory:
                raise InputError(f"{path}: duplicate inventory entry {line!r}")
            current = line
            inventory[current] = ""
        elif line.startswith("  Version: "):
            inventory[current] = line[len("  Version: ") :]
    if not inventory:
        raise InputError(f"{path} has an empty dependency inventory")
    return inventory


def load_rules() -> Any:
    """The package rule set shared with the GOV-400 notice-coverage gate."""
    try:
        return notices.load_package_rules()
    except notices.NoticeInputError as error:
        raise InputError(str(error)) from error


def reconcile(
    inventory: list[Dependency],
    rules: Any,
    files: list[str],
    notice_path: Path,
    not_configured: list[str],
) -> dict[str, Any]:
    """Reconcile a package file list with the lock; the report's ``errors`` decide the verdict."""
    dependencies = {dep.name: dep for dep in inventory}
    errors: list[str] = []
    rule_components = sorted({rule.component for rule in rules.payload if rule.component})
    for component in rule_components:
        if component not in dependencies:
            errors.append(f"package rule names component '{component}', which dependencies.lock does not lock")

    present: dict[str, int] = {}
    first_party = 0
    for rel in files:
        if PurePosixPath(rel).suffix.lower() in rules.font_suffixes:
            continue  # fonts are inventoried by the GOV-400 notice gate, not the lock
        rule = next((r for r in rules.payload if r.pattern.search(rel)), None)
        if rule is None:
            if any(pattern.search(rel) for pattern in rules.roots):
                errors.append(f"{rel}: third-party install path that no package rule maps to a locked dependency")
            continue
        if rule.component is None:
            first_party += 1
        elif rule.component not in dependencies:
            errors.append(f"{rel}: ships component '{rule.component}', which dependencies.lock does not lock")
        else:
            present[rule.component] = present.get(rule.component, 0) + 1

    declared_absent = sorted(set(not_configured))
    for name in declared_absent:
        if name not in dependencies or name not in rule_components:
            errors.append(f"--not-configured {name}: not a locked dependency with install payload rules")
        elif name in present:
            errors.append(f"--not-configured {name}: the package ships {present[name]} file(s) of it")
    for name in rule_components:
        if name in dependencies and name not in present and name not in declared_absent:
            errors.append(f"locked dependency '{name}' has install payload rules but ships no file in this package")

    listed = _notice_inventory(notice_path)
    # System runtimes are owned by the package notice rule set, not
    # dependencies.lock.  Parse the same generated inventory that GOV-400
    # validates so this reconciliation accepts only an authoritative runtime
    # entry carrying redistribution terms and the shipped DLL names.
    try:
        notice_text = notice_path.read_bytes().decode("utf-8", errors="strict")
        notice_entries = notices.parse_package_notice(notice_text, rules, str(notice_path))
    except (OSError, UnicodeDecodeError, notices.NoticeInputError) as error:
        raise InputError(f"{notice_path}: cannot parse generated package inventory: {error}") from error
    runtime_names = {rule.system_runtime for rule in rules.payload if rule.system_runtime is not None}
    runtime_entries = {entry.name: entry for entry in notice_entries if entry.name in runtime_names}
    runtime_files: dict[str, set[str]] = {name: set() for name in runtime_names}
    for rel in files:
        rule = next((r for r in rules.payload if r.pattern.search(rel)), None)
        if rule is not None and rule.system_runtime in runtime_files:
            runtime_files[rule.system_runtime].add(PurePosixPath(rel).name)
    invalid_runtime_entries: set[str] = set()
    for name in sorted(runtime_names):
        if runtime_files[name] and name not in listed:
            errors.append(f"{NOTICE_NAME} has no authoritative system runtime entry '{name}'")
            invalid_runtime_entries.add(name)
            continue
        if not runtime_files[name] and name in listed:
            errors.append(f"{NOTICE_NAME} lists system runtime '{name}' but ships no runtime files")
            invalid_runtime_entries.add(name)
            continue
        if name not in listed:
            continue
        entry = runtime_entries.get(name)
        if entry is None or not entry.terms:
            errors.append(f"{NOTICE_NAME} system runtime '{name}' has no authoritative Terms line")
            invalid_runtime_entries.add(name)
            continue
        missing = sorted(runtime_files[name] - {PurePosixPath(path).name for path in entry.files})
        if missing:
            errors.append(
                f"{NOTICE_NAME} system runtime '{name}' does not list shipped file(s): {', '.join(missing)}"
            )
            invalid_runtime_entries.add(name)
    for name, dep in dependencies.items():
        if name not in listed:
            errors.append(f"{NOTICE_NAME} does not list locked dependency '{name}'")
        elif listed[name] != dep.version:
            errors.append(
                f"{NOTICE_NAME} lists '{name}' at version {listed[name]!r}; the lock pins {dep.version!r}"
            )
    font_entries = sorted(name for name in set(listed) - set(dependencies) if FONT_ENTRY_PATTERN.match(name))
    valid_runtime_entries = (runtime_names & set(listed)) - invalid_runtime_entries
    for name in sorted(set(listed) - set(dependencies) - set(font_entries) - valid_runtime_entries):
        errors.append(f"{NOTICE_NAME} lists '{name}', which dependencies.lock does not lock")

    return {
        "schema": RECONCILE_SCHEMA,
        "packageFiles": len(files),
        "firstPartyExemptFiles": first_party,
        "components": {name: present[name] for name in sorted(present)},
        "notConfigured": declared_absent,
        "compiledInOnly": sorted(name for name in dependencies if name not in rule_components),
        "fontEntries": font_entries,
        "errors": errors,
    }


# --------------------------------------------------------------------------- cli


def _generate_main(args: argparse.Namespace) -> int:
    text = render(generate(args.source_root, args.source_sha))
    if args.check is not None:
        try:
            existing = args.check.read_bytes()
        except OSError as error:
            raise InputError(f"cannot read {args.check}: {error}") from error
        # Byte comparison: a newline-translated copy has a different digest and must not pass.
        if existing != text.encode("utf-8"):
            print(
                f"SBOM CHECK FAILED: {args.check} does not equal the SBOM generated at this checkout",
                file=sys.stderr,
            )
            return 1
        print(f"SBOM {args.check} reproduces from this checkout")
        return 0
    if args.out is None:
        sys.stdout.write(text)
    else:
        _write_atomic(args.out, text)
        print(f"Wrote SPDX 2.3 SBOM with {len(json.loads(text)['packages']) - 1} dependencies to {args.out}")
    return 0


def _reconcile_main(args: argparse.Namespace) -> int:
    if args.archive is not None:
        report = reconcile_archive(
            load_inventory(args.source_root),
            load_rules(),
            args.archive,
            args.not_configured or [],
            args.source_root,
        )
    elif args.install_manifest is not None:
        files, prefix = _read_install_manifest(args.install_manifest, args.install_prefix)
        notice_path = prefix / NOTICE_NAME
        if NOTICE_NAME not in files:
            raise SbomError(f"the install manifest does not install {NOTICE_NAME} at {prefix}")
        report = reconcile(load_inventory(args.source_root), load_rules(), files, notice_path, args.not_configured or [])
    else:
        files = _walk_package(args.package_root)
        notice_path = args.package_root / NOTICE_NAME
        report = reconcile(load_inventory(args.source_root), load_rules(), files, notice_path, args.not_configured or [])
    if args.json_report is not None:
        _write_atomic(args.json_report, json.dumps(report, indent=2, sort_keys=True) + "\n")
    components = ", ".join(f"{name} ({count})" for name, count in report["components"].items())
    print(f"Package files: {report['packageFiles']}; locked components shipped: {components or 'none'}")
    if report["notConfigured"]:
        print(f"Declared not configured: {', '.join(report['notConfigured'])}")
    if report["compiledInOnly"]:
        print(f"Compiled in only (no install payload to observe): {', '.join(report['compiledInOnly'])}")
    if report["errors"]:
        print(f"PACKAGE INVENTORY RECONCILIATION FAILED ({len(report['errors'])} error(s)):", file=sys.stderr)
        for error in report["errors"]:
            print(f"  {error}", file=sys.stderr)
        return 1
    print("Package inventory reconciles with ThirdParty/dependencies.lock")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source-root", type=Path, default=REPO_ROOT)
    commands = parser.add_subparsers(dest="command")

    parser.add_argument("--source-sha", help="commit the SBOM describes; must be the checked-out HEAD")
    output = parser.add_mutually_exclusive_group()
    output.add_argument("--out", type=Path, help="write the SBOM here (default: stdout)")
    output.add_argument("--check", type=Path, help="require this file to equal the regenerated SBOM")

    rec = commands.add_parser("reconcile", help="reconcile a package inventory with the lock")
    source = rec.add_mutually_exclusive_group(required=True)
    source.add_argument("--install-manifest", type=Path, help="CMake install_manifest.txt of the package")
    source.add_argument("--package-root", type=Path, help="staged or extracted package tree")
    source.add_argument(
        "--archive", type=Path, help="final .zip, .tar.gz, or .tgz package archive; extract and reconcile safely"
    )
    rec.add_argument("--install-prefix", help="install prefix of --install-manifest (default: its notice's directory)")
    rec.add_argument(
        "--not-configured",
        action="append",
        metavar="NAME",
        help="a locked dependency this package configuration does not ship (repeatable; must be absent)",
    )
    rec.add_argument("--json-report", type=Path, help="also write the reconciliation report as JSON")

    args = parser.parse_args(argv)
    if args.command == "reconcile" and args.install_prefix and args.install_manifest is None:
        parser.error("--install-prefix requires --install-manifest")
    try:
        if args.command == "reconcile":
            return _reconcile_main(args)
        return _generate_main(args)
    except SbomError as error:
        print(f"SBOM FAILURE: {error}", file=sys.stderr)
        return 1
    except (InputError, OSError, UnicodeDecodeError) as error:
        print(f"SBOM INPUT ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
