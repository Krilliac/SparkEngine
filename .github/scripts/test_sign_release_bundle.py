#!/usr/bin/env python3
"""Synthetic end-to-end contract test for the protected detached signer.

This test never reads a release secret or certificate store. It creates a
short-lived key/PFX in a private temporary directory and exercises the same
signer, archive extractor, and release verifier used by stable publication.
"""

from __future__ import annotations

import base64
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SIGNER = ROOT / ".github" / "scripts" / "sign_release_bundle.ps1"
EXTRACTOR = ROOT / ".github" / "scripts" / "extract_release_signature_bundle.py"
VERIFIER = ROOT / ".github" / "scripts" / "verify_release_bundle.py"
OPENSSL = shutil.which("openssl")
PWSH = shutil.which("pwsh")


def run(*args: str, **kwargs) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, check=True, capture_output=True, text=True, **kwargs)


def run_bytes(*args: str, **kwargs) -> subprocess.CompletedProcess[bytes]:
    return subprocess.run(args, check=True, capture_output=True, text=False, **kwargs)


def required_tools() -> tuple[str, str]:
    if OPENSSL and PWSH:
        return OPENSSL, PWSH
    message = "synthetic detached-signer test requires openssl and pwsh"
    if os.environ.get("CI", "").lower() == "true":
        raise SystemExit(message)
    print(f"SKIP: {message}")
    raise SystemExit(0)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    openssl, pwsh = required_tools()
    with tempfile.TemporaryDirectory(prefix="spark-release-sign-test-") as raw:
        root = Path(raw)
        assets = root / "assets"
        assets.mkdir()
        package = assets / "SparkEngine-0.9.0-Windows-AMD64-MinSizeRel.zip"
        package.write_bytes(b"synthetic package payload\n")
        (assets / "shipping-package-manifest.json").write_text("{}\n", encoding="utf-8")
        (assets / "SparkEngine-Exact-CI-Evidence.json").write_text(
            json.dumps({"schemaVersion": 2, "sourceCommit": "a" * 40}) + "\n",
            encoding="utf-8",
        )
        (assets / "SparkEngine-SBOM.spdx.json").write_text(json.dumps({
            "spdxVersion": "SPDX-2.3",
            "SPDXID": "SPDXRef-DOCUMENT",
            "documentNamespace": "https://example.invalid/spark-test",
            "packages": [{"SPDXID": "SPDXRef-Package"}],
            "files": [{
                "SPDXID": "SPDXRef-File",
                "fileName": package.name,
                "checksums": [{"algorithm": "SHA256", "checksumValue": sha256(package)}],
            }],
        }) + "\n", encoding="utf-8")
        asset_names = sorted(path.name for path in assets.iterdir())
        (assets / "SHA256SUMS").write_text(
            "".join(f"{sha256(assets / name)}  {name}\n" for name in asset_names
                    if name != "SparkEngine-Exact-CI-Evidence.json"),
            encoding="utf-8",
        )
        expected = root / "expected-release-assets.txt"
        expected.write_text("\n".join(sorted(asset_names + ["SHA256SUMS"])) + "\n", encoding="utf-8")

        key = root / "signer.key"
        certificate = root / "signer.crt"
        pfx = root / "signer.pfx"
        run(openssl, "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=SparkEngine synthetic signer", "-keyout", str(key), "-out", str(certificate))
        run(openssl, "pkcs12", "-export", "-out", str(pfx), "-inkey", str(key), "-in", str(certificate),
            "-passout", "pass:testpass")
        public_pem = run(openssl, "x509", "-in", str(certificate), "-pubkey", "-noout").stdout.encode()
        public_der = run_bytes(openssl, "pkey", "-pubin", "-outform", "DER", input=public_pem).stdout
        fingerprint = hashlib.sha256(public_der).hexdigest()
        thumbprint = run(openssl, "x509", "-in", str(certificate), "-noout", "-fingerprint", "-sha1").stdout.split("=", 1)[1].replace(":", "").strip()

        output = root / "signature-bundle"
        environment = {
            **os.environ,
            "SPARK_RELEASE_SIGNING_PFX_BASE64": base64.b64encode(pfx.read_bytes()).decode("ascii"),
            "SPARK_RELEASE_SIGNING_PFX_PASSWORD": "testpass",
        }
        run(pwsh, "-NoLogo", "-NoProfile", "-NonInteractive", "-File", str(SIGNER),
            "-InputRoot", str(assets), "-ExpectedAssetsFile", str(expected),
            "-OutputDirectory", str(output), "-SourceCommit", "a" * 40,
            "-SignerFingerprint", fingerprint, "-SignerCertificateThumbprint", thumbprint,
            env=environment)

        archive = root / "signature-bundle.tar.gz"
        with tarfile.open(archive, "w:gz") as stream:
            for child in sorted(output.iterdir()):
                stream.add(child, arcname=child.name)
        extracted = root / "extracted"
        run(sys.executable, str(EXTRACTOR), "--archive", str(archive), "--destination", str(extracted))
        verifier_args = [
            sys.executable, str(VERIFIER), "--bundle-directory", str(assets),
            "--expected-assets-file", str(expected), "--sha256sums", str(assets / "SHA256SUMS"),
            "--sbom", str(assets / "SparkEngine-SBOM.spdx.json"),
            "--provenance-manifest", str(assets / "SparkEngine-Exact-CI-Evidence.json"),
            "--signature-directory", str(extracted),
            "--signature-manifest", str(extracted / "release-signatures.json"),
            "--trusted-public-key", str(extracted / "spark-release-public-key.pem"),
            "--trusted-key-fingerprint", fingerprint, "--source-commit", "a" * 40,
            "--openssl", openssl,
        ]
        run(*verifier_args)
        package.write_bytes(package.read_bytes() + b"tampered")
        rejected = subprocess.run(verifier_args, capture_output=True, text=True)
        if rejected.returncode == 0:
            raise AssertionError("release verifier accepted a tampered payload")
    print("synthetic detached-signer roundtrip passed; tamper rejected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
