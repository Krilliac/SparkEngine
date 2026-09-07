#!/usr/bin/env python3
"""Fail closed on final stable Windows outer installer signatures; never signs files."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess

SIGNATURE_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$signature = Microsoft.PowerShell.Security\Get-AuthenticodeSignature -LiteralPath $env:SPARK_SIGNATURE_PATH
@{
    Status = [string]$signature.Status
    SignatureType = [string]$signature.SignatureType
    SignerThumbprint = $signature.SignerCertificate.Thumbprint
    SignerSubject = $signature.SignerCertificate.Subject
    TimestampThumbprint = $signature.TimeStamperCertificate.Thumbprint
    TimestampSubject = $signature.TimeStamperCertificate.Subject
} | ConvertTo-Json -Compress
"""


def trusted_powershell():
    if os.name != "nt":
        raise ValueError("Native Windows Authenticode verification is required")
    return str(Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe")


def regular_file(path):
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or path.is_symlink() or path.resolve() != path.absolute()
            or getattr(info, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)):
        raise ValueError(f"Installer must be a regular file without link traversal: {path.name}")


def selected_packages(packages, version, source_sha):
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Invalid stable release version")
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha):
        raise ValueError("Exact source commit SHA is required")
    prefix = f"SparkEngine-{version}-Windows-AMD64-MinSizeRel-Runtime"
    expected = {prefix + ".exe", prefix + ".msi"}
    files = sorted(p.absolute() for p in packages.iterdir() if p.suffix.lower() in (".exe", ".msi"))
    if {p.name for p in files} != expected or len(files) != 2:
        raise ValueError("Expected exactly the versioned stable Runtime.exe and Runtime.msi installers")
    for path in files:
        regular_file(path)
    return files


def digest(path):
    regular_file(path)
    with path.open("rb") as stream:
        state = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            state.update(chunk)
        return state.hexdigest()


def verify(packages, version, source_sha, thumbprint, report_path, *, powershell, runner=subprocess.run):
    report = {"scope": "stable-windows-outer-installers-only", "version": version, "source_sha": source_sha,
              "publisher_thumbprint": thumbprint.upper(), "passed": False, "artifacts": [], "errors": []}
    try:
        if not re.fullmatch(r"[0-9a-fA-F]{40}", thumbprint):
            raise ValueError("SPARK_RELEASE_SIGNER_THUMBPRINT must configure the publisher certificate's 40-hex thumbprint")
        files = selected_packages(packages, version, source_sha)
        encoded = base64.b64encode(SIGNATURE_SCRIPT.encode("utf-16-le")).decode("ascii")
        for path in files:
            before = digest(path)
            entry = {"name": path.name, "sha256": before}
            report["artifacts"].append(entry)
            result = runner([powershell, "-NoLogo", "-NoProfile", "-NonInteractive", "-EncodedCommand", encoded],
                            env={**os.environ, "SPARK_SIGNATURE_PATH": str(path)}, capture_output=True,
                            text=True, encoding="utf-8-sig", timeout=120, check=False)
            if digest(path) != before:
                raise ValueError(f"Installer changed during signature verification: {path.name}")
            if result.returncode != 0:
                raise ValueError(f"Authenticode process failed with exit {result.returncode}: {path.name}")
            signature = json.loads(result.stdout)
            if not isinstance(signature, dict):
                raise ValueError("Invalid signature evidence schema")
            entry["signature"] = signature
            if signature.get("Status") != "Valid":
                raise ValueError(f"Unacceptable signature status for {path.name}: {signature.get('Status')}")
            if signature.get("SignatureType") != "Authenticode":
                raise ValueError(f"Embedded Authenticode signature required: {path.name}")
            signer = signature.get("SignerThumbprint")
            if not isinstance(signer, str) or signer.upper() != thumbprint.upper():
                raise ValueError(f"Unexpected publisher certificate: {path.name}")
            timestamp = signature.get("TimestampThumbprint")
            if not isinstance(timestamp, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", timestamp):
                raise ValueError(f"Timestamp certificate required: {path.name}")
            if any(not isinstance(signature.get(key), str) or not signature[key]
                   for key in ("SignerSubject", "TimestampSubject")):
                raise ValueError("Invalid signature certificate evidence schema")
        report["passed"] = True
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report["errors"].append(str(error))
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return int(not report["passed"])


def check_hashes(packages, version, source_sha, report_path):
    """Require the bytes verified earlier to survive native qualification unchanged."""
    try:
        files = selected_packages(packages, version, source_sha)
        report = json.loads(report_path.read_text(encoding="utf-8"))
        if (not isinstance(report, dict) or report.get("passed") is not True
                or report.get("version") != version or report.get("source_sha") != source_sha
                or report.get("scope") != "stable-windows-outer-installers-only"):
            raise ValueError("Signature report identity or result is invalid")
        expected = [{"name": p.name, "sha256": digest(p)} for p in files]
        actual = [{"name": item["name"], "sha256": item["sha256"]} for item in report["artifacts"]]
        if actual != expected:
            raise ValueError("Installer bytes changed after signature verification")
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(error)
        return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packages", required=True, type=Path)
    parser.add_argument("--version", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--check-hashes", action="store_true")
    args = parser.parse_args()
    if args.check_hashes:
        return check_hashes(args.packages, args.version, args.source_sha, args.report)
    try:
        powershell = trusted_powershell()
    except (ValueError, KeyError) as error:
        parser.error(str(error))
    result = verify(args.packages, args.version, args.source_sha,
                    os.environ.get("SPARK_RELEASE_SIGNER_THUMBPRINT", ""), args.report, powershell=powershell)
    if result:
        print(args.report.read_text(encoding="utf-8"))
    return result


if __name__ == "__main__":
    raise SystemExit(main())
