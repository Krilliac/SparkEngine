#!/usr/bin/env python3
"""Exercise one native Windows MSI on a disposable hosted runner; no certification claims."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
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


def qualify(packages, version, manifest, runner_temp, logs, *, runner=run_command,
            msiexec, powershell, cmake, source_sha=None):
    logs.mkdir(parents=True, exist_ok=False)
    errors = []
    report = {"scope": "hosted-windows-msi-install-uninstall", "source_sha": source_sha,
              "version": version, "errors": errors, "certifies_windows11": False}
    attempted = False
    validated = False
    package = None
    install_root = None
    digest = None

    def execute(label, argv, *, env=None, cwd=None, timeout=900):
        code = runner(argv, logs / f"{label}.log", timeout=timeout, env=env, cwd=cwd)
        if code != 0:
            raise ValueError(f"{label} failed with exit {code}")

    def package_digest():
        digest_state = hashlib.sha256()
        with package.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest_state.update(chunk)
        return digest_state.hexdigest()

    def identity(label):
        command = base64.b64encode(IDENTITY_SCRIPT.encode("utf-16-le")).decode("ascii")
        execute(label, [powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-EncodedCommand", command],
                env={**os.environ, "SPARK_MSI_PATH": str(package)})
        info = json.loads((logs / f"{label}.log").read_text(encoding="utf-8-sig"))
        if (not isinstance(info, dict) or type(info.get("ProductState")) is not int
                or not isinstance(info.get("RelatedProducts"), list)
                or any(not isinstance(code, str) for code in info.get("RelatedProducts", []))
                or any(not isinstance(info.get(key), str)
                       for key in ("ProductName", "ProductVersion", "ProductCode", "UpgradeCode", "InstallRoot"))):
            raise ValueError("MSI identity query did not return the required schema")
        return info

    try:
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
            raise ValueError("Invalid MSI release version")
        expected = f"SparkEngine-{version}-Windows-AMD64-MinSizeRel-Runtime.msi"
        candidates = sorted(path for path in packages.iterdir() if path.suffix.lower() == ".msi")
        if len(candidates) != 1 or candidates[0].name != expected:
            raise ValueError("Expected exactly one versioned Windows Shipping Runtime MSI")
        package = candidates[0].absolute()
        if not package.is_file() or package.is_symlink() or package.resolve() != package:
            raise ValueError("MSI path must be a regular file without symlink traversal")
        if not manifest.is_file() or not runner_temp.is_dir():
            raise ValueError("Trusted build manifest and runner temporary directory are required")
        digest = package_digest()
        report.update(msi=package.name, sha256=digest)
        info = identity("identity-before")
        if (info.get("ProductName") != "SparkEngine" or info.get("ProductVersion") != version
                or info.get("InstallRoot") != "INSTALL_ROOT"
                or any(not re.fullmatch(r"\{[0-9A-Fa-f]{8}-(?:[0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}\}", info[key])
                       for key in ("ProductCode", "UpgradeCode"))):
            raise ValueError("MSI database identity or install directory does not match the expected package")
        if info.get("ProductState") != -1:
            raise ValueError("MSI product already registered; refusing to modify an existing installation")
        if info["RelatedProducts"]:
            raise ValueError("Related MSI products are registered; refusing an unintended upgrade")
        report["product_code"] = info["ProductCode"]
        report["upgrade_code"] = info["UpgradeCode"]
        scratch = Path(tempfile.mkdtemp(prefix="spark-msi-", dir=runner_temp)).resolve()
        install_root = scratch / "install"
        report["install_root"] = str(install_root)
        if package_digest() != digest:
            raise ValueError("MSI changed during identity validation")
        attempted = True
        execute("install", [msiexec, "/i", str(package), "/qn", "/norestart", "/L*V",
                            str(logs / "msi-install.log"), f"INSTALL_ROOT={install_root}"])
        execute("validate", [cmake, f"-DSPARK_PACKAGE_ROOT={install_root}", "-DSPARK_PACKAGE_LAYOUT=runtime",
                             "-DSPARK_PACKAGE_PROFILE=stable-v1", f"-DSPARK_PACKAGE_EXPECTED_MODULE_MANIFEST={manifest}",
                             "-DSPARK_EXECUTABLE_SUFFIX:STRING=.exe", "-P",
                             str(ROOT / "cmake/ValidateStagedPackageExecutables.cmake")])
        execute("fps-nullrhi", [str(install_root / "bin/SparkEngine.exe"), "-headless", "-game",
                               str(install_root / "bin/SparkGameFPS.dll"), "-test-frames", "5", "-require-game",
                               "-no-subprocess", "-no-jobsystem"], cwd=install_root / "bin", timeout=120)
        with (logs / "fps-nullrhi.log").open(encoding="utf-8", errors="replace") as output:
            if not any(re.fullmatch(r"SPARK_MODULE_READY count=[1-9][0-9]*", line.strip()) for line in output):
                raise ValueError("Installed FPS NullRHI smoke did not report an initialized module")
        validated = True
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        errors.append(str(error))
    finally:
        if attempted:
            try:
                if package_digest() != digest:
                    raise ValueError("MSI changed after selection; refusing to execute substituted uninstall package")
                execute("uninstall", [msiexec, "/x", str(package), "/qn", "/norestart", "/L*V",
                                      str(logs / "msi-uninstall.log")])
                remaining = identity("identity-after")
                if remaining["ProductState"] != -1 or remaining["RelatedProducts"]:
                    raise ValueError("MSI product registration remains after uninstall")
            except (OSError, ValueError, subprocess.SubprocessError) as error:
                errors.append(str(error))
            # Never hide uninstall defects by deleting installed files ourselves.
            if install_root.exists() or install_root.is_symlink():
                errors.append(f"Uninstall residue remains at {install_root}")
        report["passed"] = validated and not errors
        (logs / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for error in errors:
        print(error)
    return int(not report["passed"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packages", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--runner-temp", type=Path, required=True)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--source-sha", required=True)
    args = parser.parse_args()
    if os.name != "nt" or not re.fullmatch(r"[0-9a-f]{40}", args.source_sha):
        parser.error("Native Windows and an exact source commit are required")
    system = Path(os.environ["SystemRoot"]) / "System32"
    return qualify(args.packages.resolve(), args.version, args.manifest.resolve(), args.runner_temp.resolve(),
                   args.logs.resolve(), msiexec=str(system / "msiexec.exe"),
                   powershell=str(system / "WindowsPowerShell/v1.0/powershell.exe"), cmake="cmake",
                   source_sha=args.source_sha)


if __name__ == "__main__":
    raise SystemExit(main())
