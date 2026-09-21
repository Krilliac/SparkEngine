#!/usr/bin/env python3
"""Exercise one native Windows MSI on a disposable hosted runner; no certification claims.

The caller must treat the successful package-smoke log as a process-exit
handoff: upload it immediately from the same job-owned workspace with no
repository-controlled command in between.
"""
from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "module-evidence"))
import strict_json  # noqa: E402
import package_evidence_io  # noqa: E402

if os.name == "nt":
    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _CreateFileW = _kernel32.CreateFileW
    _CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    ]
    _CreateFileW.restype = wintypes.HANDLE
    _CloseHandle = _kernel32.CloseHandle
    _CloseHandle.argtypes = [wintypes.HANDLE]
    _CloseHandle.restype = wintypes.BOOL
    _INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    _GENERIC_READ = 0x80000000
    _FILE_READ_ATTRIBUTES = 0x00000080
    _FILE_SHARE_READ = 0x00000001
    _OPEN_EXISTING = 3
    _FILE_ATTRIBUTE_NORMAL = 0x00000080
# CMake's WiX v3 template sets WIXUI_INSTALLDIR=INSTALL_ROOT. Query the actual
# database read-only before trusting that public directory property.
IDENTITY_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
$installer = New-Object -ComObject WindowsInstaller.Installer
$db = $installer.OpenDatabase($env:SPARK_MSI_PATH, 0)
$identity = @{}
foreach ($name in @('ProductName', 'ProductVersion', 'ProductCode', 'UpgradeCode')) {
    $query = 'SELECT `Value` FROM `Property` WHERE `Property` = ''{0}''' -f $name
    $view = $db.OpenView($query)
    $view.Execute()
    $record = $view.Fetch()
    if ($null -eq $record) { throw "Missing MSI property: $name" }
    $identity[$name] = $record.StringData(1)
    $view.Close()
}
$view = $db.OpenView('SELECT `Directory` FROM `Directory` WHERE `Directory` = ''INSTALL_ROOT''')
$view.Execute()
$record = $view.Fetch()
if ($null -eq $record) { throw 'MSI has no INSTALL_ROOT directory' }
$identity.InstallRoot = $record.StringData(1)
$view.Close()
$identity.ProductState = $installer.ProductState($identity.ProductCode)
$related = @()
foreach ($product in $installer.RelatedProducts($identity.UpgradeCode)) {
    $related += [string]$product
}
$identity.RelatedProducts = @($related)
$identity | ConvertTo-Json -Compress
"""


def run_command(argv, log, *, timeout, env=None, cwd=None):
    with log.open("w", encoding="utf-8") as output:
        return subprocess.run(argv, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, env=env, cwd=cwd).returncode


@contextmanager
def _hold_private_msi_identity(path):
    """Deny write/delete sharing while Windows Installer opens the verified copy."""
    if os.name != "nt":
        yield
        return
    handle = _CreateFileW(
        str(path), _GENERIC_READ | _FILE_READ_ATTRIBUTES, _FILE_SHARE_READ,
        None, _OPEN_EXISTING, _FILE_ATTRIBUTE_NORMAL, None,
    )
    raw_handle = ctypes.cast(handle, ctypes.c_void_p).value
    if raw_handle in (None, _INVALID_HANDLE_VALUE):
        error = ctypes.get_last_error()
        raise OSError(error, f"CreateFileW could not lock private MSI identity: {error}")
    try:
        yield
    finally:
        if not _CloseHandle(handle):
            error = ctypes.get_last_error()
            raise OSError(error, f"CloseHandle failed for private MSI identity: {error}")


_READY_RE = re.compile(r"SPARK_MODULE_READY count=([0-9]+)")
_NULLRHI_RE = re.compile(
    r"SPARK_HEADLESS_RHI backend=null initialized=([0-9]+) frames=([0-9]+) shutdown=([0-9]+)"
)
_HEADLESS_LIFECYCLE_RE = re.compile(
    r"SPARK_HEADLESS_LIFECYCLE initialized=([0-9]+) updated=([0-9]+) fixed=([0-9]+) "
    r"rendered=([0-9]+) unloaded=([0-9]+) faults=([0-9]+)"
)
_D3D11_DEVICE_RE = re.compile(r"SPARK_D3D11_DEVICE driver=warp(?:\s.*)?")
_MODULE_LIFECYCLE_RE = re.compile(
    r"SPARK_MODULE_LIFECYCLE module=SparkGameFPS create=([0-9]+) load=([0-9]+) "
    r"update=([0-9]+) fixed=([0-9]+) render=([0-9]+) unload=([0-9]+) "
    r"destroy=([0-9]+) faults=([0-9]+)"
)
_SOURCE_SHA_RE = re.compile(r"[0-9a-f]{40}")


def _has_link_component(path: Path) -> bool:
    """Reject symlink/reparse components without rejecting Windows 8.3 aliases."""
    for component in (path, *path.parents):
        info = component.lstat()
        if (component.is_symlink()
                or getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)):
            return True
    return False


def _exact_record(lines, pattern, label):
    matches = [match for line in lines if (match := pattern.fullmatch(line))]
    if len(matches) != 1:
        raise ValueError(f"{label} produced {len(matches)} direct terminal record(s), expected exactly 1")
    return matches[0]


def _read_terminal_lines(log):
    try:
        return [line.strip() for line in log.read_text(encoding="utf-8", errors="replace").splitlines()]
    except OSError as exc:
        raise ValueError(f"cannot read runtime smoke log {log}: {exc}") from exc


def _validate_nullrhi_log(log):
    lines = _read_terminal_lines(log)
    # The engine may route device diagnostics through its logger, so the
    # forbidden marker is not required to start at column zero.  Reject any
    # standalone marker token before accepting the NullRHI terminal records.
    if any(re.search(r"(?:^|\s)SPARK_D3D11_DEVICE(?:\s|$)", line) for line in lines):
        raise ValueError("installed FPS NullRHI smoke emitted a D3D11 device record")
    ready = _exact_record(lines, _READY_RE, "installed FPS NullRHI module-ready")
    rhi = _exact_record(lines, _NULLRHI_RE, "installed FPS NullRHI backend")
    lifecycle = _exact_record(lines, _HEADLESS_LIFECYCLE_RE, "installed FPS NullRHI lifecycle")
    if int(ready.group(1)) != 1:
        raise ValueError("installed FPS NullRHI smoke initialized a module count other than 1")
    if (int(rhi.group(1)), int(rhi.group(3))) != (1, 1) or int(rhi.group(2)) < 1:
        raise ValueError("installed FPS NullRHI backend did not initialize, frame, and shut down exactly")
    initialized, updated, fixed, rendered, unloaded, faults = map(int, lifecycle.groups())
    if initialized != 1 or updated < 1 or fixed < 1 or rendered != 0 or unloaded != 1 or faults != 0:
        raise ValueError("installed FPS NullRHI lifecycle terminal record is invalid")


def _validate_d3d11_log(log):
    lines = _read_terminal_lines(log)
    _exact_record(lines, _D3D11_DEVICE_RE, "installed FPS D3D11/WARP device")
    lifecycle = _exact_record(lines, _MODULE_LIFECYCLE_RE, "installed FPS D3D11/WARP lifecycle")
    create, load, update, fixed, render, unload, destroy, faults = map(int, lifecycle.groups())
    if create != 1 or load != 1 or update < 1 or fixed < 1 or render < 1 or unload != 1 or destroy != 1 or faults != 0:
        raise ValueError("installed FPS D3D11/WARP lifecycle terminal record is invalid")


def _write_package_smoke_log(path, source_sha, msi_sha256):
    if not _SOURCE_SHA_RE.fullmatch(source_sha):
        raise ValueError("package-smoke source SHA must be 40 lower-case hexadecimal characters")
    if not re.fullmatch(r"[0-9a-f]{64}", msi_sha256):
        raise ValueError("package-smoke MSI SHA-256 must be 64 lower-case hexadecimal characters")
    content = (
        "[package-smoke] schema=package-smoke-v1\n"
        "[package-smoke] product=SparkEngine\n"
        "[package-smoke] module=SparkGameFPS\n"
        "[package-smoke] profile=stable-v1\n"
        f"[package-smoke] commit_sha={source_sha}\n"
        f"[package-smoke] msi_sha256={msi_sha256}\n"
        "[package-smoke] backend=nullrhi result=PASS\n"
        "[package-smoke] backend=d3d11-warp result=PASS\n"
        "[package-smoke] exit_code=0\n"
        "[package-smoke] PASS\n"
    )
    return package_evidence_io.publish_bytes_no_replace(path, content.encode("utf-8"))


def _write_result_report(path, report):
    payload = (json.dumps(report, indent=2) + "\n").encode("utf-8")
    return package_evidence_io.publish_bytes_no_replace(path, payload)


_SHIPPING_PACKAGE_MANIFEST_KEYS = frozenset({
    "schemaVersion", "commitSHA", "profile", "configuration", "version", "msi", "sha256",
})
_MAX_SHIPPING_PACKAGE_MANIFEST_BYTES = 1024 * 1024


def _reject_duplicate_json_keys(pairs):
    document = {}
    for key, value in pairs:
        if key in document:
            raise ValueError(f"duplicate JSON key {key!r}")
        document[key] = value
    return document


def _validate_shipping_package_manifest(path, source_sha, version, msi_name, msi_sha256):
    """Bind the selected MSI bytes to the producer's closed identity document."""
    try:
        data = strict_json.read_file_no_follow_bytes(
            Path(path).absolute(), max_bytes=_MAX_SHIPPING_PACKAGE_MANIFEST_BYTES,
        )
        document = json.loads(
            data.decode("utf-8"), object_pairs_hook=_reject_duplicate_json_keys,
        )
    except (OSError, UnicodeDecodeError, ValueError, json.JSONDecodeError,
            strict_json.StrictJSONError) as exc:
        raise ValueError(f"shipping package manifest is unreadable JSON: {exc}") from exc
    if not isinstance(document, dict) or set(document) != _SHIPPING_PACKAGE_MANIFEST_KEYS:
        raise ValueError("shipping package manifest does not have the closed required schema")
    if any(not isinstance(value, str) for value in document.values()):
        raise ValueError("shipping package manifest fields must all be strings")
    expected = {
        "schemaVersion": "spark-shipping-package-v1",
        "profile": "stable-v1",
        "configuration": "MinSizeRel",
        "version": version,
        "msi": msi_name,
        "sha256": msi_sha256,
    }
    if source_sha is None:
        if not re.fullmatch(r"[0-9a-f]{40}", document["commitSHA"]):
            raise ValueError("shipping package manifest commitSHA is not an exact lower-case source SHA")
    else:
        expected["commitSHA"] = source_sha
    for field, value in expected.items():
        if document[field] != value:
            label = "hash" if field == "sha256" else field
            raise ValueError(
                f"shipping package manifest {label} does not match the selected MSI identity"
            )


def _qualify_impl(packages, version, manifest, runner_temp, logs, *, runner=run_command,
                  msiexec, powershell, cmake, source_sha=None, package_manifest=None,
                  previous_packages=None, previous_version=None, previous_package_manifest=None):
    logs = Path(logs)
    if os.path.lexists(logs):
        raise ValueError("package-evidence log directory must be fresh")
    logs.mkdir(parents=True, exist_ok=False)
    errors = []
    transaction = any(value is not None for value in
                      (previous_packages, previous_version, previous_package_manifest))
    report = {"scope": ("hosted-windows-msi-upgrade-repair-rollback-uninstall"
                         if transaction else "hosted-windows-msi-install-uninstall"),
              "source_sha": source_sha,
              "version": version, "errors": errors, "certifies_windows11": False}
    attempted = False
    validated = False
    package = None
    install_root = None
    digest = None
    cleanup_package = None
    cleanup_digest = None
    upgrade_attempted = False
    previous_install_attempted = False
    old_product_code = None
    old_upgrade_code = None
    smoke_output = logs / "package-smoke.log"

    def execute(label, argv, *, env=None, cwd=None, timeout=900):
        code = runner(argv, logs / f"{label}.log", timeout=timeout, env=env, cwd=cwd)
        if code != 0:
            raise ValueError(f"{label} failed with exit {code}")

    def package_digest(selected_package=None):
        selected_package = selected_package or package
        digest_state = hashlib.sha256()
        with selected_package.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest_state.update(chunk)
        return digest_state.hexdigest()

    def identity(label, selected_package=None):
        selected_package = selected_package or package
        command = base64.b64encode(IDENTITY_SCRIPT.encode("utf-16-le")).decode("ascii")
        execute(label, [powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-EncodedCommand", command],
                env={**os.environ, "SPARK_MSI_PATH": str(selected_package)})
        info = json.loads((logs / f"{label}.log").read_text(encoding="utf-8-sig"))
        if (not isinstance(info, dict) or type(info.get("ProductState")) is not int
                or not isinstance(info.get("RelatedProducts"), list)
                or any(not isinstance(code, str) for code in info.get("RelatedProducts", []))
                or any(not isinstance(info.get(key), str)
                       for key in ("ProductName", "ProductVersion", "ProductCode", "UpgradeCode", "InstallRoot"))):
            raise ValueError("MSI identity query did not return the required schema")
        return info

    def validate_identity(info, expected_version, label):
        if (info.get("ProductName") != "SparkEngine"
                or info.get("ProductVersion") != expected_version
                or info.get("InstallRoot") != "INSTALL_ROOT"
                or any(not re.fullmatch(r"\{[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}\}", info[key])
                       for key in ("ProductCode", "UpgradeCode"))):
            raise ValueError(f"{label} MSI database identity or install directory does not match the expected package")

    try:
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
            raise ValueError("Invalid MSI release version")
        if not _SOURCE_SHA_RE.fullmatch(source_sha or ""):
            raise ValueError("source SHA must be 40 lower-case hexadecimal characters")
        packages = Path(packages)
        if not packages.is_dir() or packages.is_symlink():
            raise ValueError("shipping package directory must be a real directory")
        expected = f"SparkEngine-{version}-Windows-AMD64-MinSizeRel-Runtime.msi"
        candidates = sorted(path for path in packages.iterdir() if path.suffix.lower() == ".msi")
        if len(candidates) != 1 or candidates[0].name != expected:
            raise ValueError("Expected exactly one versioned Windows Shipping Runtime MSI")
        package = candidates[0].absolute()
        if not package.is_file() or _has_link_component(package):
            raise ValueError("MSI path must be a regular file without symlink traversal")
        if not manifest.is_file() or not runner_temp.is_dir():
            raise ValueError("Trusted build manifest and runner temporary directory are required")
        private_package_dir = logs / ".private-package"
        private_package_dir.mkdir()
        package, digest = package_evidence_io.copy_private_verified_file(
            package, private_package_dir, package.name,
        )
        if package_manifest is None:
            raise ValueError("trusted shipping package manifest is required")
        _validate_shipping_package_manifest(
            package_manifest, source_sha, version, package.name, digest,
        )
        report.update(msi=package.name, sha256=digest)
        report["shipping_package_manifest"] = str(package_manifest)
        old_package = old_digest = None
        if transaction:
            if previous_packages is None or previous_version is None or previous_package_manifest is None:
                raise ValueError(
                    "previous_packages, previous_version, and previous_package_manifest must be supplied together"
                )
            if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", previous_version):
                raise ValueError("Invalid previous MSI release version")
            previous_packages = Path(previous_packages)
            if not previous_packages.is_dir() or previous_packages.is_symlink():
                raise ValueError("previous shipping package directory must be a real directory")
            previous_expected = f"SparkEngine-{previous_version}-Windows-AMD64-MinSizeRel-Runtime.msi"
            previous_candidates = sorted(path for path in previous_packages.iterdir()
                                         if path.suffix.lower() == ".msi")
            if len(previous_candidates) != 1 or previous_candidates[0].name != previous_expected:
                raise ValueError("Expected exactly one versioned previous Windows Shipping Runtime MSI")
            previous_source = previous_candidates[0].absolute()
            if not previous_source.is_file() or _has_link_component(previous_source):
                raise ValueError("previous MSI path must be a regular file without symlink traversal")
            previous_private_dir = logs / ".private-previous-package"
            previous_private_dir.mkdir()
            old_package, old_digest = package_evidence_io.copy_private_verified_file(
                previous_source, previous_private_dir, previous_source.name,
            )
            _validate_shipping_package_manifest(
                previous_package_manifest, None, previous_version, old_package.name, old_digest,
            )
            report.update(previous_msi=old_package.name, previous_sha256=old_digest,
                          previous_shipping_package_manifest=str(previous_package_manifest))
        scratch = Path(tempfile.mkdtemp(prefix="spark-msi-", dir=runner_temp)).resolve()
        install_root = scratch / "install"
        local_app_data = scratch / "localappdata"
        user_data_root = local_app_data / "SparkEngine"
        user_data_root.mkdir(parents=True, exist_ok=False)
        user_data_sentinel = user_data_root / "release-qualification-sentinel.sav"
        user_data_sentinel_bytes = b"SparkEngine stable-v1 external user data\n"
        user_data_sentinel.write_bytes(user_data_sentinel_bytes)
        report["install_root"] = str(install_root)
        report["user_data_root"] = str(user_data_root)

        def verify_user_data(label):
            try:
                actual = user_data_sentinel.read_bytes()
            except OSError as error:
                raise ValueError(f"{label} could not read external user data sentinel: {error}") from error
            if actual != user_data_sentinel_bytes:
                raise ValueError(f"{label} changed external user data sentinel")
        with _hold_private_msi_identity(package):
            if package_digest(package) != digest:
                raise ValueError("private MSI changed during identity validation")
            if transaction:
                if package_digest(old_package) != old_digest:
                    raise ValueError("private previous MSI changed during identity validation")
                with _hold_private_msi_identity(old_package):
                    old_info = identity("identity-previous-before", old_package)
                validate_identity(old_info, previous_version, "previous")
                old_product_code = old_info["ProductCode"]
                old_upgrade_code = old_info["UpgradeCode"]
                if old_info.get("ProductState") != -1:
                    raise ValueError("previous MSI product is already registered; qualification requires an owned fresh install")
                if old_info["RelatedProducts"]:
                    raise ValueError("previous MSI related-product registration exists before owned install")
                if old_info.get("ProductState") == -1:
                    previous_install_attempted = True
                    execute("install-previous", [msiexec, "/i", str(old_package), "/qn", "/norestart", "/L*V",
                                                   str(logs / "msi-install-previous.log"), f"INSTALL_ROOT={install_root}"])
                    attempted = True
                cleanup_package, cleanup_digest = old_package, old_digest
            else:
                cleanup_package, cleanup_digest = package, digest
            info = identity("identity-before")
            if transaction:
                validate_identity(info, version, "current")
                if info.get("ProductState") != -1:
                    raise ValueError("current MSI product is already registered; refusing an unintended replacement")
                if info["UpgradeCode"] != old_upgrade_code:
                    raise ValueError("current MSI UpgradeCode does not match the previous product")
                if info["ProductCode"] == old_product_code:
                    raise ValueError("current MSI ProductCode must differ from the previous product")
            else:
                validate_identity(info, version, "current")
                if info.get("ProductState") != -1:
                    raise ValueError("MSI product already registered; refusing to modify an existing installation")
                if info["RelatedProducts"]:
                    raise ValueError("Related MSI products are registered; refusing an unintended upgrade")
            report["product_code"] = info["ProductCode"]
            report["upgrade_code"] = info["UpgradeCode"]
            if transaction:
                upgrade_attempted = True
                execute("upgrade", [msiexec, "/i", str(package), "/qn", "/norestart", "/L*V",
                                     str(logs / "msi-upgrade.log"), f"INSTALL_ROOT={install_root}"])
                attempted = True
                cleanup_package, cleanup_digest = package, digest
            else:
                attempted = True
                execute("install", [msiexec, "/i", str(package), "/qn", "/norestart", "/L*V",
                                    str(logs / "msi-install.log"), f"INSTALL_ROOT={install_root}"])
        if transaction:
            with _hold_private_msi_identity(package):
                if package_digest(package) != digest:
                    raise ValueError("private MSI changed before upgraded identity validation")
                upgraded = identity("identity-upgraded", package)
                if upgraded["ProductState"] != 5 or upgraded["ProductVersion"] != version:
                    raise ValueError("MSI upgrade did not establish the new installed product")
            with _hold_private_msi_identity(old_package):
                if package_digest(old_package) != old_digest:
                    raise ValueError("private previous MSI changed before upgraded identity validation")
            execute("repair", [msiexec, "/fvomus", str(package), "/qn", "/norestart", "/L*V",
                                str(logs / "msi-repair.log"), f"INSTALL_ROOT={install_root}"])
            verify_user_data("upgrade+repair")
        with _hold_private_msi_identity(package):
            if package_digest(package) != digest:
                raise ValueError("private MSI changed before installed identity validation")
            installed = identity("identity-installed")
            # INSTALLSTATE_DEFAULT (5) means installed for the current context;
            # advertised, absent, or broken registration is not a successful install.
            if installed["ProductState"] != 5:
                raise ValueError("MSI installation did not establish installed product registration")
        execute("validate", [cmake, f"-DSPARK_PACKAGE_ROOT={install_root}", "-DSPARK_PACKAGE_LAYOUT=runtime",
                             "-DSPARK_PACKAGE_PROFILE=stable-v1", f"-DSPARK_PACKAGE_EXPECTED_MODULE_MANIFEST={manifest}",
                             "-DSPARK_EXECUTABLE_SUFFIX:STRING=.exe", "-P",
                             str(ROOT / "cmake/ValidateStagedPackageExecutables.cmake")])
        execute("fps-nullrhi", [str(install_root / "bin/SparkEngine.exe"), "-headless", "-game",
                               str(install_root / "bin/SparkGameFPS.dll"), "-test-frames", "5", "-require-game",
                               "-no-subprocess", "-no-jobsystem"],
                env={**os.environ, "LOCALAPPDATA": str(local_app_data),
                     "SPARK_RHI_BACKEND": "null"},
                cwd=install_root / "bin", timeout=120)
        _validate_nullrhi_log(logs / "fps-nullrhi.log")
        execute("fps-d3d11-warp", [str(install_root / "bin/SparkEngine.exe"), "-game",
                                    str(install_root / "bin/SparkGameFPS.dll"), "-require-game",
                                    "-test-seconds", "1.0", "-threads", "2", "-window-size", "640x360",
                                    "-no-subprocess"],
                env={**os.environ, "LOCALAPPDATA": str(local_app_data),
                     "SPARK_RHI_BACKEND": "d3d11", "SPARK_D3D11_DRIVER": "warp"},
                cwd=install_root / "bin", timeout=120)
        _validate_d3d11_log(logs / "fps-d3d11-warp.log")
        verify_user_data("runtime validation")
        validated = True
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        errors.append(str(error))
        if transaction and upgrade_attempted and old_package is not None:
            try:
                # A nonzero installer exit may mean no mutation or a partial
                # mutation. Inspect registration before issuing destructive
                # rollback commands, preserving the original failure above.
                with _hold_private_msi_identity(package):
                    failed_upgrade = identity("identity-upgrade-failure", package)
                if failed_upgrade["ProductState"] == -1:
                    with _hold_private_msi_identity(old_package):
                        if package_digest(old_package) != old_digest:
                            raise ValueError("private previous MSI changed while inspecting failed upgrade")
                        old_after_failure = identity("identity-upgrade-failure-old", old_package)
                        if old_after_failure["ProductState"] == -1:
                            execute("rollback-install-previous", [msiexec, "/i", str(old_package), "/qn", "/norestart", "/L*V",
                                                                  str(logs / "msi-rollback-install.log"),
                                                                  f"INSTALL_ROOT={install_root}"])
                            restored = identity("identity-rollback", old_package)
                            if restored["ProductState"] != 5 or restored["ProductVersion"] != previous_version:
                                raise ValueError("failed upgrade could not restore the previous installed product")
                            verify_user_data("rollback restoration")
                            attempted = True
                            cleanup_package, cleanup_digest = old_package, old_digest
                        elif old_after_failure["ProductState"] != 5:
                            raise ValueError("failed upgrade left the previous product in an indeterminate state")
                    upgrade_attempted = False
                elif failed_upgrade["ProductState"] == 5:
                    with _hold_private_msi_identity(package):
                        if package_digest(package) != digest:
                            raise ValueError("private MSI changed; refusing rollback uninstall")
                        execute("rollback-uninstall-new", [msiexec, "/x", str(package), "/qn", "/norestart", "/L*V",
                                                            str(logs / "msi-rollback-uninstall.log")])
                    with _hold_private_msi_identity(old_package):
                        if package_digest(old_package) != old_digest:
                            raise ValueError("private previous MSI changed; refusing rollback install")
                        execute("rollback-install-previous", [msiexec, "/i", str(old_package), "/qn", "/norestart", "/L*V",
                                                              str(logs / "msi-rollback-install.log"),
                                                              f"INSTALL_ROOT={install_root}"])
                        restored = identity("identity-rollback", old_package)
                        if restored["ProductState"] != 5 or restored["ProductVersion"] != previous_version:
                            raise ValueError("rollback did not restore the previous installed product")
                        verify_user_data("rollback restoration")
                    cleanup_package, cleanup_digest = old_package, old_digest
                else:
                    raise ValueError("failed upgrade left an indeterminate MSI registration state")
            except (OSError, ValueError, subprocess.SubprocessError) as rollback_error:
                errors.append(f"rollback failed: {rollback_error}")
        elif transaction and previous_install_attempted and old_package is not None:
            try:
                with _hold_private_msi_identity(old_package):
                    if package_digest(old_package) != old_digest:
                        raise ValueError("private previous MSI changed after failed previous install")
                    old_after_failure = identity("identity-previous-install-failure", old_package)
                    if old_after_failure["ProductState"] == 5:
                        execute("cleanup-partial-previous", [msiexec, "/x", str(old_package), "/qn", "/norestart", "/L*V",
                                                              str(logs / "msi-cleanup-partial-previous.log")])
                        cleaned = identity("identity-previous-install-cleanup", old_package)
                        if cleaned["ProductState"] != -1:
                            raise ValueError("failed previous install left a registered product")
                    elif old_after_failure["ProductState"] != -1:
                        raise ValueError("failed previous install left an indeterminate MSI registration state")
                if install_root.exists() or install_root.is_symlink():
                    errors.append(f"Uninstall residue remains at {install_root}")
            except (OSError, ValueError, subprocess.SubprocessError) as cleanup_error:
                errors.append(f"previous-install cleanup failed: {cleanup_error}")
    finally:
        if attempted:
            try:
                if cleanup_package is None or cleanup_digest is None:
                    raise ValueError("cleanup MSI identity was not established")
                with _hold_private_msi_identity(cleanup_package):
                    if package_digest(cleanup_package) != cleanup_digest:
                        raise ValueError("private MSI changed; refusing to execute substituted uninstall package")
                    execute("uninstall", [msiexec, "/x", str(cleanup_package), "/qn", "/norestart", "/L*V",
                                          str(logs / "msi-uninstall.log")])
                    remaining = identity("identity-after", cleanup_package)
                    if remaining["ProductState"] != -1 or remaining["RelatedProducts"]:
                        raise ValueError("MSI product registration remains after uninstall")
                    verify_user_data("final uninstall")
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                errors.append(str(error))
            # Never hide uninstall defects by deleting installed files ourselves.
            if install_root.exists() or install_root.is_symlink():
                errors.append(f"Uninstall residue remains at {install_root}")
        if validated and not errors:
            try:
                _write_package_smoke_log(smoke_output, source_sha, digest)
                report["package_smoke_log"] = str(smoke_output)
            except (OSError, ValueError) as error:
                errors.append(str(error))
        report["passed"] = validated and not errors
        if errors:
            try:
                _write_result_report(logs / "result.json", report)
            except (OSError, ValueError) as error:
                errors.append(f"cannot publish qualification result report: {error}")
                report["passed"] = False
    for error in errors:
        print(error)
    return int(not report["passed"])


def qualify(packages, version, manifest, runner_temp, logs, *, runner=run_command,
            msiexec, powershell, cmake, source_sha=None, package_manifest=None,
            previous_packages=None, previous_version=None, previous_package_manifest=None):
    """Run native MSI qualification on Windows.

    The platform-independent transaction state machine lives in the private
    implementation so injected unit fixtures can exercise it without mutating
    Python's process-global ``os.name`` value.
    """
    if os.name != "nt":
        raise ValueError("Windows is required for native MSI qualification")
    return _qualify_impl(
        packages, version, manifest, runner_temp, logs,
        runner=runner, msiexec=msiexec, powershell=powershell, cmake=cmake,
        source_sha=source_sha, package_manifest=package_manifest,
        previous_packages=previous_packages, previous_version=previous_version,
        previous_package_manifest=previous_package_manifest,
    )


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packages", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--package-manifest", type=Path, required=True)
    parser.add_argument("--runner-temp", type=Path, required=True)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--previous-packages", type=Path)
    parser.add_argument("--previous-version")
    parser.add_argument("--previous-package-manifest", type=Path)
    args = parser.parse_args()
    previous_values = (args.previous_packages, args.previous_version, args.previous_package_manifest)
    if any(value is not None for value in previous_values) and not all(value is not None for value in previous_values):
        parser.error("--previous-packages, --previous-version, and --previous-package-manifest must be supplied together")
    if os.name != "nt" or not re.fullmatch(r"[0-9a-f]{40}", args.source_sha):
        parser.error("Native Windows and an exact source commit are required")
    system = Path(os.environ["SystemRoot"]) / "System32"
    return qualify(args.packages, args.version, args.manifest, args.runner_temp,
                   args.logs, msiexec=str(system / "msiexec.exe"),
                   powershell=str(system / "WindowsPowerShell/v1.0/powershell.exe"), cmake="cmake",
                   source_sha=args.source_sha, package_manifest=args.package_manifest,
                   previous_packages=args.previous_packages, previous_version=args.previous_version,
                   previous_package_manifest=args.previous_package_manifest)


if __name__ == "__main__":
    raise SystemExit(main())
