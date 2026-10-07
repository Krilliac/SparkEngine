"""Closed Windows Shipping input policy shared by provenance and parity checks.

This is a build-input lock, not a certification or release-readiness claim.
Release provenance reads the committed policy at its declared source SHA;
parity reads the trusted verifier checkout, never the producer's inventory.
"""
from __future__ import annotations

import json
import ntpath
from pathlib import Path
import re
import subprocess
from typing import Any

POLICY_PATH = "cmake/WindowsShippingToolchain.json"
POLICY_KEYS = {
    "schemaVersion", "profile", "platform", "configuration", "generator", "architecture",
    "platformToolset", "hostArchitecture", "msvcToolsVersion", "compilerId", "compilerVersion",
    "windowsSdkVersion",
}
_IDENTITY = {
    "schemaVersion": "spark-windows-shipping-toolchain-v1", "profile": "stable-v1",
    "platform": "Windows", "configuration": "MinSizeRel", "generator": "Visual Studio 17 2022",
    "architecture": "x64", "platformToolset": "v143", "hostArchitecture": "x64", "compilerId": "MSVC",
}
_VERSIONS = {"msvcToolsVersion": r"14\.[0-9]+\.[0-9]+",
             "compilerVersion": r"19\.[0-9]+\.[0-9]+\.[0-9]+",
             "windowsSdkVersion": r"10\.0\.[0-9]+\.[0-9]+"}


class ShippingToolchainError(ValueError):
    """Missing, malformed, or drifted Shipping input policy."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ShippingToolchainError(f"Shipping toolchain policy duplicate key: {key}")
        result[key] = value
    return result


def parse_policy(raw: bytes) -> dict[str, str]:
    if len(raw) > 16384:
        raise ShippingToolchainError("Shipping toolchain policy exceeds 16 KiB")
    try:
        policy = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object)
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ShippingToolchainError(f"Shipping toolchain policy is not valid JSON: {error}") from error
    if not isinstance(policy, dict) or set(policy) != POLICY_KEYS:
        raise ShippingToolchainError("Shipping toolchain policy does not match its closed schema")
    for key, value in policy.items():
        if not isinstance(value, str) or not value:
            raise ShippingToolchainError(f"Shipping toolchain policy {key} must be a non-empty string")
        if key in _IDENTITY and value != _IDENTITY[key]:
            raise ShippingToolchainError(f"Shipping toolchain policy {key} must be {_IDENTITY[key]!r}")
        if key in _VERSIONS and not re.fullmatch(_VERSIONS[key], value):
            raise ShippingToolchainError(f"Shipping toolchain policy {key} is malformed")
    return policy


def load_policy(source_root: Path, source_sha: str | None = None) -> dict[str, str]:
    path = source_root / POLICY_PATH
    if not path.is_file() or path.is_symlink():
        raise ShippingToolchainError(f"Shipping toolchain policy is missing: {POLICY_PATH}")
    if source_sha is None:
        return parse_policy(path.read_bytes())
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha):
        raise ShippingToolchainError("Shipping toolchain policy requires a full source SHA")
    blob = subprocess.run(["git", "-C", str(source_root), "show", f"{source_sha}:{POLICY_PATH}"],
                          capture_output=True, check=False)
    if blob.returncode:
        raise ShippingToolchainError(f"Shipping toolchain policy is absent at {source_sha}")
    unchanged = subprocess.run(["git", "-C", str(source_root), "diff", "--quiet", source_sha, "--", POLICY_PATH],
                               capture_output=True, check=False)
    if unchanged.returncode:
        raise ShippingToolchainError(f"Shipping toolchain policy differs from {POLICY_PATH} at {source_sha}")
    policy = parse_policy(blob.stdout)
    # Git's assume-unchanged/skip-worktree bits can suppress diff output. The
    # consumed policy must still agree with the committed source of authority.
    if parse_policy(path.read_bytes()) != policy:
        raise ShippingToolchainError(f"Shipping toolchain policy differs from {POLICY_PATH} at {source_sha}")
    return policy


def toolset_selector(policy: dict[str, str]) -> str:
    return (f"{policy['platformToolset']},host={policy['hostArchitecture']},"
            f"version={policy['msvcToolsVersion']}")


def compare_toolchain(policy: dict[str, str], *, configuration: Any, generator: Any, architecture: Any,
                      toolset: Any, compilers: dict[str, Any], windows_sdk: Any, linker: Any, archiver: Any,
                      instance: Any, require_c: bool = False) -> list[str]:
    """Compare measured identities; recorded paths use Windows rules on every host."""
    errors: list[str] = []

    def exact(name: str, actual: Any, expected: str) -> None:
        if actual != expected:
            errors.append(f"{name}={actual!r}, requires {expected!r}")

    exact("configuration", configuration, policy["configuration"])
    exact("generator", generator, policy["generator"])
    exact("architecture", architecture, policy["architecture"])
    exact("generatorToolset", toolset, toolset_selector(policy))
    exact("windowsSdkVersion", windows_sdk, policy["windowsSdkVersion"])
    expected_bin = ""
    if isinstance(instance, str) and ntpath.splitdrive(instance)[0] and ntpath.isabs(instance):
        expected_bin = ntpath.join(instance, "VC", "Tools", "MSVC", policy["msvcToolsVersion"],
                                   "bin", "Host" + policy["hostArchitecture"], policy["architecture"])
    else:
        errors.append("visualStudioInstance must name an absolute Windows installation path")

    def tool_path(name: str, actual: Any, executable: str) -> None:
        if (not expected_bin or not isinstance(actual, str)
                or ntpath.normcase(ntpath.normpath(actual)) != ntpath.normcase(ntpath.join(expected_bin, executable))):
            errors.append(f"{name}={actual!r}, requires the locked MSVC installation's {executable}")

    for language in ("C", "CXX") if require_c else ("CXX",):
        if language not in compilers:
            errors.append(f"{language} compiler identity is missing")
    for language, compiler in compilers.items():
        if not isinstance(compiler, dict):
            errors.append(f"{language} compiler identity is malformed")
            continue
        exact(f"{language} compilerId", compiler.get("id"), policy["compilerId"])
        exact(f"{language} compilerVersion", compiler.get("version"), policy["compilerVersion"])
        exact(f"{language} architecture", compiler.get("architecture"), policy["architecture"])
        tool_path(f"{language} compiler path", compiler.get("path"), "cl.exe")
    tool_path("linker", linker, "link.exe")
    tool_path("archiver", archiver, "lib.exe")
    return errors
