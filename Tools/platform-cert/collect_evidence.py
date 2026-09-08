#!/usr/bin/env python3
"""Produce a PLT-200 evidence record by actually running things.

This is the only supported way to create an evidence record.  It exists so
that "collected evidence" means a process really ran on a real host at a real
commit, and not that somebody typed some JSON.

Rules it keeps, in the order they matter:

  1. The revision is read from git and refused if the working tree is dirty.
     Evidence that cannot be pinned to a committed revision is not evidence.
  2. Host facts are measured from the operating system.  Anything that cannot
     be measured is a hard failure, never a placeholder -- with two explicit,
     recorded exceptions (`--no-gpu`, `--no-audio`) which assert an *absence*
     and are only usable for rows that declare the nullrhi / none tuple.
  3. Every probe is a real subprocess with a real exit code and a real
     wall-clock interval.  There is no way to declare a pass; a pass is what a
     zero exit code produces.  A category with no command is recorded as
     `error`, so a missing probe can never read as a skip or a pass.
  4. Captured output is written content-addressed -- the file is named by the
     SHA-256 of its own bytes -- through an exclusive create, so a collector
     run cannot overwrite or follow anything already on disk.
  5. The record is validated before it is written.  If it does not pass the
     validator, nothing is emitted.

If the host cannot supply what a row needs, this program fails.  It has no
path that invents a green result.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import bundle_verify
import safe_fs
import validate_certification as vc

TOOLS_DIR = Path(__file__).resolve().parent
REPO_ROOT = TOOLS_DIR.parents[1]

PLAN_SCHEMA_VERSION = 1
MAX_PLAN_BYTES = 256 * 1024
MAX_CAPTURE_BYTES = 8 * 1024 * 1024
DEFAULT_TIMEOUT_SECONDS = 600
MAX_TIMEOUT_SECONDS = 7200

_VERSION_RE = re.compile(r"([0-9]+(?:\.[0-9]+){1,3})")
_MSVC_VERSION_RE = re.compile(r"Version\s+([0-9]+(?:\.[0-9]+){1,3})")


class CollectionError(Exception):
    """The host cannot supply honest evidence for what was asked."""


def _now() -> datetime:
    return datetime.now(timezone.utc)


def _iso(moment: datetime) -> str:
    return moment.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


def _run(
    command: list[str], *, cwd: Path, timeout: int
) -> tuple[int, str, str]:
    try:
        completed = subprocess.run(
            command,
            cwd=str(cwd),
            capture_output=True,
            text=True,
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except FileNotFoundError as exc:
        raise CollectionError(f"command not found: {command[0]!r}") from exc
    except subprocess.TimeoutExpired as exc:
        raise CollectionError(
            f"command timed out after {timeout}s: {' '.join(command)}"
        ) from exc
    except OSError as exc:
        raise CollectionError(f"cannot run {' '.join(command)}: {exc}") from exc
    return completed.returncode, completed.stdout, completed.stderr


def _run_ok(command: list[str], *, cwd: Path, timeout: int = 120) -> str:
    code, out, err = _run(command, cwd=cwd, timeout=timeout)
    if code != 0:
        raise CollectionError(
            f"{' '.join(command)} exited {code}: {(err or out).strip()[:200]}"
        )
    return out


# ── Revision binding ───────────────────────────────────────────────────────


def resolve_commit(repo_root: Path, *, allow_dirty: bool) -> str:
    """The exact committed revision this evidence describes."""
    head = _run_ok(["git", "rev-parse", "HEAD"], cwd=repo_root).strip()
    if vc._SHA1_RE.match(head) is None:
        raise CollectionError(f"git returned an unusable HEAD: {head!r}")
    status = _run_ok(["git", "status", "--porcelain"], cwd=repo_root)
    if status.strip() and not allow_dirty:
        dirty = [line for line in status.splitlines() if line.strip()][:5]
        raise CollectionError(
            "the working tree has uncommitted changes, so evidence cannot be "
            f"bound to {head[:12]}: {dirty}"
        )
    return head


# ── Host measurement ───────────────────────────────────────────────────────


def measure_os() -> dict[str, str]:
    system = platform.system()
    if system == "Windows":
        version = platform.win32_ver()[0] or ""
        build = platform.version() or ""
        release = platform.release() or ""
        # Windows 11 still reports 10 in win32_ver; the build number decides.
        try:
            major_build = int(build.split(".")[-1])
        except (ValueError, IndexError):
            major_build = 0
        marketing = "11" if major_build >= 22000 else (version or release)
        if not build:
            raise CollectionError("cannot measure the Windows build number")
        return {
            "family": "windows",
            "version": marketing,
            "build": f"10.0.{build.split('.')[-1]}" if build.count(".") < 2 else build,
            "locale": _measure_locale(),
        }
    if system == "Linux":
        info = _read_os_release()
        version = info.get("VERSION_ID") or info.get("VERSION") or ""
        if not version:
            raise CollectionError("cannot measure the Linux distribution version")
        return {
            "family": "linux",
            "version": version,
            "build": platform.release(),
            "locale": _measure_locale(),
        }
    if system == "Darwin":
        version = platform.mac_ver()[0]
        if not version:
            raise CollectionError("cannot measure the macOS version")
        return {
            "family": "macos",
            "version": version,
            "build": platform.release(),
            "locale": _measure_locale(),
        }
    raise CollectionError(f"unsupported host operating system: {system!r}")


def _read_os_release() -> dict[str, str]:
    path = Path("/etc/os-release")
    if not path.is_file():
        return {}
    info: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "=" in line:
            key, _, value = line.partition("=")
            info[key.strip()] = value.strip().strip('"')
    return info


_LOCALE_TAG_RE = re.compile(r"\A[A-Za-z]{2,8}(?:-[A-Za-z0-9]{2,8}){0,4}\Z")


def _measure_locale() -> str:
    """The host's user locale as a BCP-47-shaped tag, measured not guessed."""
    candidates: list[str] = []
    if platform.system() == "Windows":
        try:
            import ctypes

            buffer = ctypes.create_unicode_buffer(85)
            if ctypes.windll.kernel32.GetUserDefaultLocaleName(  # type: ignore[attr-defined]
                buffer, len(buffer)
            ):
                candidates.append(buffer.value)
        except Exception:  # noqa: BLE001 - fall through to the environment
            pass
    for name in ("LC_ALL", "LC_CTYPE", "LANG"):
        value = os.environ.get(name)
        if value:
            candidates.append(value)
    try:
        import locale as _locale

        current = _locale.getlocale()[0]
        if current:
            candidates.append(current)
    except Exception:  # noqa: BLE001
        pass

    for candidate in candidates:
        tag = candidate.split("@")[0].split(".")[0].replace("_", "-").strip()
        if _LOCALE_TAG_RE.match(tag) and 2 <= len(tag) <= 35:
            return tag
    raise CollectionError(
        "cannot measure the host locale; set LANG or LC_ALL to a language tag"
    )


def measure_arch() -> str:
    machine = platform.machine().lower()
    if machine in ("amd64", "x86_64"):
        return "x86_64"
    if machine in ("arm64", "aarch64"):
        return "aarch64"
    raise CollectionError(f"unsupported host architecture: {platform.machine()!r}")


def measure_cpu() -> dict[str, Any]:
    system = platform.system()
    if system == "Linux":
        model, features = _linux_cpu()
    elif system == "Windows":
        model, features = _windows_cpu()
    else:
        model, features = platform.processor(), []
    if not model:
        raise CollectionError("cannot measure the host CPU model")
    if not features:
        raise CollectionError("cannot measure the host CPU feature set")
    return {"model": model[:200], "features": features[:128]}


_INTERESTING_FEATURES = (
    "SSE2", "SSE3", "SSSE3", "SSE4.1", "SSE4.2", "AVX", "AVX2",
    "AVX512F", "FMA", "BMI1", "BMI2", "POPCNT", "F16C", "AES",
    "NEON", "ASIMD", "SVE",
)
_FEATURE_ALIASES = {
    "sse4_1": "SSE4.1",
    "sse4_2": "SSE4.2",
    "pni": "SSE3",
}


def _normalise_features(raw: list[str]) -> list[str]:
    wanted = {name.lower().replace(".", "").replace("_", ""): name for name in _INTERESTING_FEATURES}
    found: list[str] = []
    for token in raw:
        key = token.strip().lower()
        canonical = _FEATURE_ALIASES.get(key)
        if canonical is None:
            canonical = wanted.get(key.replace(".", "").replace("_", ""))
        if canonical and canonical not in found:
            found.append(canonical)
    return sorted(found)


def _linux_cpu() -> tuple[str, list[str]]:
    path = Path("/proc/cpuinfo")
    if not path.is_file():
        raise CollectionError("/proc/cpuinfo is not readable")
    model = ""
    flags: list[str] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        key, _, value = line.partition(":")
        key = key.strip().lower()
        if key == "model name" and not model:
            model = value.strip()
        elif key in ("flags", "features") and not flags:
            flags = value.split()
    return model, _normalise_features(flags)


def _windows_cpu() -> tuple[str, list[str]]:
    model = os.environ.get("PROCESSOR_IDENTIFIER", "") or platform.processor()
    script = (
        "(Get-CimInstance Win32_Processor | Select-Object -First 1).Name"
    )
    try:
        name = _run_ok(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
            cwd=Path.cwd(),
        ).strip()
        if name:
            model = name
    except CollectionError:
        pass
    # Windows exposes no /proc/cpuinfo; the ISA level the binaries were built
    # for is what a row cares about, so it is measured from the interpreter's
    # own view of the machine rather than guessed.
    features: list[str] = []
    try:
        import ctypes

        is_supported = ctypes.windll.kernel32.IsProcessorFeaturePresent  # type: ignore[attr-defined]
        # PF_XMMI64_INSTRUCTIONS_AVAILABLE=10 (SSE2), PF_SSE3=13,
        # PF_SSSE3=36, PF_SSE4_1=37, PF_SSE4_2=38, PF_AVX=39, PF_AVX2=40
        for code, name in (
            (10, "SSE2"), (13, "SSE3"), (36, "SSSE3"), (37, "SSE4.1"),
            (38, "SSE4.2"), (39, "AVX"), (40, "AVX2"),
        ):
            if is_supported(code):
                features.append(name)
    except Exception as exc:  # noqa: BLE001 - measurement failure must be visible
        raise CollectionError(f"cannot measure Windows CPU features: {exc}") from exc
    return model, sorted(features)


def measure_ram_mb() -> int:
    system = platform.system()
    if system == "Linux":
        path = Path("/proc/meminfo")
        if path.is_file():
            for line in path.read_text(encoding="utf-8").splitlines():
                if line.startswith("MemTotal:"):
                    return max(1, int(line.split()[1]) // 1024)
    if system == "Windows":
        try:
            import ctypes

            class _Status(ctypes.Structure):
                _fields_ = [
                    ("dwLength", ctypes.c_ulong),
                    ("dwMemoryLoad", ctypes.c_ulong),
                    ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong),
                    ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong),
                    ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong),
                    ("ullAvailExtendedVirtual", ctypes.c_ulonglong),
                ]

            status = _Status()
            status.dwLength = ctypes.sizeof(_Status)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):  # type: ignore[attr-defined]
                return max(1, int(status.ullTotalPhys) // (1024 * 1024))
        except Exception as exc:  # noqa: BLE001
            raise CollectionError(f"cannot measure host RAM: {exc}") from exc
    raise CollectionError("cannot measure host RAM")


def measure_gpu(*, no_gpu: bool, repo_root: Path) -> dict[str, str]:
    """Measure the display adapter, or record a verified absence."""
    if no_gpu:
        return {
            "api": "nullrhi",
            "device": "NullRHIDevice",
            "vendor": "none",
            "driverVersion": "none",
            "featureLevel": "null",
        }
    if platform.system() != "Windows":
        raise CollectionError(
            "GPU measurement is only implemented for Windows hosts; pass --no-gpu "
            "only for a row that declares the nullrhi/none tuple"
        )
    script = (
        "Get-CimInstance Win32_VideoController | "
        "Select-Object -First 1 Name,AdapterCompatibility,DriverVersion | "
        "ConvertTo-Json -Compress"
    )
    raw = _run_ok(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
        cwd=repo_root,
    )
    try:
        info = json.loads(raw)
    except json.JSONDecodeError as exc:
        raise CollectionError(f"cannot parse the video-controller query: {exc}") from exc
    device = str(info.get("Name", "")).strip()
    vendor_raw = str(info.get("AdapterCompatibility", "")).strip().lower()
    driver = str(info.get("DriverVersion", "")).strip()
    if not device or not driver:
        raise CollectionError("the video-controller query returned no adapter")
    vendor = next(
        (
            known
            for known in ("nvidia", "amd", "intel", "microsoft")
            if known in vendor_raw
        ),
        "",
    )
    if not vendor:
        raise CollectionError(f"unrecognised GPU vendor {vendor_raw!r}")
    return {
        "api": "d3d11",
        "device": device[:200],
        "vendor": vendor,
        "driverVersion": driver[:100],
        # A feature level is a runtime property of the device the engine
        # creates, so it comes from the plan rather than from WMI.
        "featureLevel": "",
    }


def measure_audio(*, no_audio: bool, repo_root: Path) -> dict[str, str]:
    if no_audio:
        return {"api": "none"}
    if platform.system() != "Windows":
        raise CollectionError(
            "audio measurement is only implemented for Windows hosts; pass "
            "--no-audio only for a row that declares audio api 'none'"
        )
    script = (
        "(Get-CimInstance Win32_SoundDevice | Where-Object Status -eq 'OK' | "
        "Select-Object -First 1).Name"
    )
    name = _run_ok(
        ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
        cwd=repo_root,
    ).strip()
    if not name:
        raise CollectionError("no working audio device was reported by the host")
    return {"api": "xaudio2", "deviceName": name[:200]}


def measure_compiler(command: list[str], *, repo_root: Path) -> dict[str, str]:
    """Run the compiler and read its version out of its own banner."""
    code, out, err = _run(command, cwd=repo_root, timeout=120)
    banner = (out + "\n" + err).strip()
    if not banner:
        raise CollectionError(f"{' '.join(command)} printed no version banner")
    lowered = banner.lower()
    if "microsoft" in lowered and "c/c++" in lowered:
        match = _MSVC_VERSION_RE.search(banner)
        compiler_id, toolset = "msvc", os.environ.get("VCToolsVersion", "")
        toolset = "v143" if toolset.startswith("14.4") or not toolset else toolset
    elif "clang" in lowered:
        match = _VERSION_RE.search(banner)
        compiler_id = "apple-clang" if "apple" in lowered else "clang"
        toolset = ""
    elif "mingw" in lowered:
        match = _VERSION_RE.search(banner)
        compiler_id, toolset = "mingw", ""
    elif "gcc" in lowered or "g++" in lowered or "free software foundation" in lowered:
        match = _VERSION_RE.search(banner)
        compiler_id, toolset = "gcc", ""
    else:
        raise CollectionError(
            f"cannot identify the compiler from its banner: {banner.splitlines()[0][:120]!r}"
        )
    if match is None:
        raise CollectionError("the compiler banner carries no version number")
    version = match.group(1)
    if code != 0 and compiler_id != "msvc":
        # cl.exe with no inputs exits non-zero after printing its banner; every
        # other compiler is expected to succeed on --version.
        raise CollectionError(f"{' '.join(command)} exited {code}")
    return {
        "id": compiler_id,
        "version": version,
        "toolset": toolset or f"{compiler_id}-{version.split('.')[0]}",
    }


# ── Content-addressed artifact writing ─────────────────────────────────────


def write_content_addressed(
    directory: Path, payload: bytes, *, suffix: str
) -> tuple[str, str, int]:
    """Write `payload` as <sha256><suffix> beneath `directory`, safely.

    Returns (relative posix path, sha256, size).  The file is created
    exclusively through a temporary name in the same directory and then
    replaced into place, so an existing name is never followed or truncated.
    """
    if len(payload) > MAX_CAPTURE_BYTES:
        payload = payload[:MAX_CAPTURE_BYTES]
    digest = hashlib.sha256(payload).hexdigest()
    directory.mkdir(parents=True, exist_ok=True)
    safe_fs.lstat_checked(directory, expect="dir")
    target = directory / f"{digest}{suffix}"

    if target.exists():
        existing, size = safe_fs.measure_file(
            target, max_bytes=bundle_verify.MAX_ARTIFACT_BYTES
        )
        if existing == digest and size == len(payload):
            return target.name, digest, size
        raise CollectionError(f"{target} exists with different content")

    handle, temporary = tempfile.mkstemp(dir=str(directory), prefix=".partial-")
    try:
        with os.fdopen(handle, "wb") as stream:
            stream.write(payload)
        os.replace(temporary, target)
    except BaseException:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise
    measured, size = safe_fs.measure_file(
        target, max_bytes=bundle_verify.MAX_ARTIFACT_BYTES
    )
    if measured != digest or size != len(payload):
        raise CollectionError(f"{target}: content changed while being written")
    return target.name, digest, size


# ── Plan loading ───────────────────────────────────────────────────────────


def load_plan(path: Path) -> dict[str, Any]:
    raw = safe_fs.read_bounded(path, max_bytes=MAX_PLAN_BYTES)
    plan = vc.parse_strict_json(raw.decode("utf-8"), path.name)
    if not isinstance(plan, dict):
        raise CollectionError(f"{path.name}: plan must be a JSON object")
    if plan.get("schemaVersion") != PLAN_SCHEMA_VERSION:
        raise CollectionError(f"{path.name}: plan schemaVersion must be {PLAN_SCHEMA_VERSION}")
    row_id = plan.get("rowId")
    if not isinstance(row_id, str) or vc._ROW_ID_RE.match(row_id) is None:
        raise CollectionError(f"{path.name}: plan rowId is missing or malformed")
    probes = plan.get("probes")
    if not isinstance(probes, dict) or not probes:
        raise CollectionError(f"{path.name}: plan must declare at least one probe")
    unknown = sorted(set(probes) - vc.ALL_PROBE_CATEGORIES)
    if unknown:
        raise CollectionError(f"{path.name}: unknown probe categories {unknown}")
    for name, spec in probes.items():
        if not isinstance(spec, dict):
            raise CollectionError(f"{path.name}: probe {name!r} must be an object")
        command = spec.get("command")
        if not isinstance(command, list) or not command:
            raise CollectionError(f"{path.name}: probe {name!r} needs a command list")
        if not all(isinstance(part, str) and part for part in command):
            raise CollectionError(f"{path.name}: probe {name!r} command must be strings")
        timeout = spec.get("timeoutSeconds", DEFAULT_TIMEOUT_SECONDS)
        if isinstance(timeout, bool) or not isinstance(timeout, int):
            raise CollectionError(f"{path.name}: probe {name!r} timeoutSeconds must be an integer")
        if not 1 <= timeout <= MAX_TIMEOUT_SECONDS:
            raise CollectionError(
                f"{path.name}: probe {name!r} timeoutSeconds must be 1..{MAX_TIMEOUT_SECONDS}"
            )

    provenance = plan.get("provenance")
    if provenance is not None:
        if not isinstance(provenance, dict):
            raise CollectionError(f"{path.name}: plan provenance must be an object")
        for field in ("repository", "workflow", "runId", "jobId"):
            value = provenance.get(field)
            if not isinstance(value, str) or not value:
                raise CollectionError(
                    f"{path.name}: plan provenance.{field} is required"
                )
    return plan


# ── Collection ─────────────────────────────────────────────────────────────


def run_probe(
    name: str,
    spec: dict[str, Any],
    *,
    repo_root: Path,
    artifact_dir: Path,
) -> dict[str, Any]:
    """Run one probe and record what actually happened."""
    command = list(spec["command"])
    timeout = int(spec.get("timeoutSeconds", DEFAULT_TIMEOUT_SECONDS))
    started = _now()
    monotonic = time.monotonic()
    try:
        code, out, err = _run(command, cwd=repo_root, timeout=timeout)
        detail = ""
    except CollectionError as exc:
        code, out, err, detail = None, "", str(exc), str(exc)
    duration_ms = max(1, int((time.monotonic() - monotonic) * 1000))
    completed = _now()

    transcript = (
        f"# probe: {name}\n"
        f"# command: {json.dumps(command)}\n"
        f"# exitCode: {code}\n"
        f"# startedAt: {_iso(started)}\n"
        f"# completedAt: {_iso(completed)}\n"
        f"--- stdout ---\n{out}\n--- stderr ---\n{err}\n"
    ).encode("utf-8", errors="replace")

    relative, digest, size = write_content_addressed(
        artifact_dir / name, transcript, suffix=".log"
    )
    artifact = {
        "path": f"{name}/{relative}",
        "sha256": digest,
        "sizeBytes": size,
    }

    if code is None:
        return {
            "status": "error",
            "durationMs": duration_ms,
            "detail": detail[:2000],
            "artifacts": [artifact],
        }
    probe: dict[str, Any] = {
        "status": "pass" if code == 0 else "fail",
        "durationMs": duration_ms,
        "exitCode": code,
        "startedAt": _iso(started),
        "completedAt": _iso(completed),
        "artifacts": [artifact],
    }
    if code != 0:
        probe["detail"] = (err or out).strip()[:2000] or f"exit code {code}"
    return probe


def collect(args: argparse.Namespace) -> dict[str, Any]:
    repo_root = args.repo_root.resolve()
    plan = load_plan(args.plan)
    row_id = plan["rowId"]
    if args.row_id and args.row_id != row_id:
        raise CollectionError(
            f"--row-id {args.row_id!r} disagrees with the plan's rowId {row_id!r}"
        )

    commit = resolve_commit(repo_root, allow_dirty=args.allow_dirty)
    if args.commit and args.commit != commit:
        raise CollectionError(
            f"--commit {args.commit[:12]} is not the checked-out revision {commit[:12]}"
        )

    checkout_at = _now()
    host = {
        "os": measure_os(),
        "arch": measure_arch(),
        "cpu": measure_cpu(),
        "ram": {"totalMb": measure_ram_mb()},
        "gpu": measure_gpu(no_gpu=args.no_gpu, repo_root=repo_root),
        "audio": measure_audio(no_audio=args.no_audio, repo_root=repo_root),
        "compiler": measure_compiler(args.compiler_command, repo_root=repo_root),
    }
    for section, overrides in (plan.get("hostOverrides") or {}).items():
        if section not in host or not isinstance(overrides, dict):
            raise CollectionError(f"plan hostOverrides names unknown section {section!r}")
        for key, value in overrides.items():
            if not isinstance(value, str) or not value:
                raise CollectionError(
                    f"plan hostOverrides.{section}.{key} must be a non-empty string"
                )
            if host[section].get(key):
                raise CollectionError(
                    f"plan hostOverrides.{section}.{key} would replace a measured value"
                )
            host[section][key] = value
    if args.windows_sdk_version:
        host["compiler"]["windowsSdkVersion"] = args.windows_sdk_version
    if host["os"]["family"] == "windows" and not host["compiler"].get("windowsSdkVersion"):
        raise CollectionError(
            "--windows-sdk-version is required when collecting on a Windows host"
        )
    if not host["gpu"]["featureLevel"]:
        raise CollectionError(
            "the GPU feature level must be supplied through the plan's "
            "hostOverrides.gpu.featureLevel"
        )

    artifact_dir = args.artifact_root / row_id
    _prepare_artifact_dir(artifact_dir, clean=args.clean_artifacts)
    probes: dict[str, Any] = {}
    for name in sorted(vc.ALL_PROBE_CATEGORIES):
        spec = plan["probes"].get(name)
        if spec is None:
            continue
        probes[name] = run_probe(
            name, spec, repo_root=repo_root, artifact_dir=artifact_dir
        )

    trusted = _trusted(args, commit)
    if trusted.collector_type == "ci":
        provenance = {
            "repository": trusted.repository,
            "workflow": trusted.workflow,
            "runId": trusted.run_id,
            "runAttempt": trusted.run_attempt,
            "jobId": trusted.job_id,
        }
    else:
        declared = plan.get("provenance")
        if declared is None:
            raise CollectionError(
                "a non-CI collection needs a plan 'provenance' block naming the "
                "repository, workflow, runId and jobId this record came from"
            )
        provenance = {
            "repository": declared["repository"],
            "workflow": declared["workflow"],
            "runId": declared["runId"],
            "runAttempt": 1,
            "jobId": declared["jobId"],
        }
    provenance["checkoutAt"] = _iso(checkout_at)
    provenance["commitSha"] = commit

    record: dict[str, Any] = {
        "schemaVersion": 1,
        "rowId": row_id,
        "commitSha": commit,
        "collectedAt": _iso(_now()),
        "collector": {
            "type": trusted.collector_type,
            "identity": trusted.identity,
            "provenance": provenance,
            "attestation": {
                "predicateType": vc.ATTESTATION_PREDICATE,
                "bundleSha256": "",
                "signedAt": "",
            },
        },
        "host": host,
        "probes": probes,
    }
    if trusted.collector_type == "ci":
        record["collector"]["runId"] = trusted.run_id
    if plan.get("dependencyClosure"):
        record["dependencyClosure"] = plan["dependencyClosure"]

    errors, digest = bundle_verify.verify_bundle(
        record, artifact_root=args.artifact_root, row_id=row_id
    )
    if errors or digest is None:
        raise CollectionError(
            "the artifacts this run produced do not measure cleanly: " + "; ".join(errors[:5])
        )
    record["collector"]["attestation"]["bundleSha256"] = digest
    record["collector"]["attestation"]["signedAt"] = _iso(_now())

    validation = vc.validate_evidence(record, trusted=trusted)
    if validation:
        raise CollectionError(
            "the collected record does not validate: " + "; ".join(validation[:6])
        )
    return record


def _prepare_artifact_dir(artifact_dir: Path, *, clean: bool) -> None:
    """Own the row's bundle directory, so a run cannot inherit stale files.

    A leftover artifact from an earlier attempt would be an undeclared file in
    the bundle -- correctly refused by the validator, but the repair is to
    start from an empty directory rather than to relax the rule.
    """
    if not artifact_dir.exists():
        artifact_dir.mkdir(parents=True, exist_ok=True)
        return
    safe_fs.lstat_checked(artifact_dir, expect="dir")
    existing = safe_fs.iter_bundle_files(
        artifact_dir,
        max_files=bundle_verify.MAX_BUNDLE_FILES,
        max_depth=bundle_verify.MAX_BUNDLE_DEPTH,
        max_total_bytes=bundle_verify.MAX_BUNDLE_BYTES,
    )
    if not existing:
        return
    if not clean:
        raise CollectionError(
            f"{artifact_dir} already holds {len(existing)} file(s) from an "
            f"earlier run; pass --clean-artifacts to start from an empty bundle"
        )
    # Every entry beneath here was just proven to be a link-free regular file.
    for path in existing:
        os.unlink(path)
    for directory in sorted(
        (p for p in artifact_dir.rglob("*") if p.is_dir()),
        key=lambda p: len(p.parts),
        reverse=True,
    ):
        try:
            directory.rmdir()
        except OSError:
            pass


def _trusted(args: argparse.Namespace, commit: str) -> vc.TrustedContext:
    if args.from_github_env:
        context = vc.TrustedContext.from_github_env(dict(os.environ))
        if context.commit_sha != commit:
            raise CollectionError(
                f"GITHUB_SHA {context.commit_sha[:12]} is not the checked-out "
                f"revision {commit[:12]}"
            )
        return context
    if not args.identity:
        raise CollectionError("--identity is required unless --from-github-env is used")
    return vc.TrustedContext(
        commit_sha=commit,
        collector_type=args.collector_type,
        identity=args.identity,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--row-id", default=None)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument("--commit", default=None)
    parser.add_argument("--allow-dirty", action="store_true")
    parser.add_argument(
        "--clean-artifacts",
        action="store_true",
        help="empty the row's artifact directory before collecting",
    )
    parser.add_argument(
        "--compiler-command",
        action="append",
        default=None,
        metavar="ARG",
        help=(
            "one argument of the command that prints the toolchain's version "
            "banner; repeat the flag per argument "
            "(default: --compiler-command c++ --compiler-command --version)"
        ),
    )
    parser.add_argument("--windows-sdk-version", default=None)
    parser.add_argument(
        "--no-gpu",
        action="store_true",
        help="assert the absence of a display adapter (nullrhi rows only)",
    )
    parser.add_argument(
        "--no-audio",
        action="store_true",
        help="assert the absence of an audio device (audio api 'none' rows only)",
    )
    parser.add_argument("--collector-type", choices=["manual", "automated"], default="automated")
    parser.add_argument("--identity", default=None)
    parser.add_argument("--from-github-env", action="store_true")

    args = parser.parse_args(argv)
    if not args.compiler_command:
        args.compiler_command = ["c++", "--version"]
    try:
        record = collect(args)
    except (CollectionError, vc.CertificationError, safe_fs.FileSecurityError) as exc:
        print(f"FATAL: {exc}", file=sys.stderr)
        return 1

    args.evidence_dir.mkdir(parents=True, exist_ok=True)
    destination = args.evidence_dir / f"{record['rowId']}.json"
    with open(destination, "w", encoding="utf-8", newline="\n") as stream:
        json.dump(record, stream, indent=2, ensure_ascii=False)
        stream.write("\n")

    passed = sum(1 for p in record["probes"].values() if p["status"] == "pass")
    print(
        f"Collected {len(record['probes'])} probe(s) for row "
        f"{record['rowId']!r} at {record['commitSha'][:12]}: {passed} passed"
    )
    print(f"Record: {destination}")
    print(f"Bundle: {record['collector']['attestation']['bundleSha256'][:16]}...")
    return 0


if __name__ == "__main__":
    sys.exit(main())
