"""Qualify a Windows installed host against the existing benign N+1/N-1 fixtures.

No build, install, fixture mutation or cleanup. Keep the fresh evidence tree.
This complements (does not replace) the same-host installed positive lifecycle test.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


KEYS = frozenset("format struct_size magic sdk_version runtime_abi_version compiler_family "
                 "compiler_abi_version cxx_language_level runtime_library iterator_debug_level "
                 "pointer_size binary_sha256".split())


def parse_sidecar(text):
    result = {}
    for line in text.splitlines():
        if not line:
            continue
        key, separator, value = line.partition("=")
        if not separator or key not in KEYS or key in result:
            raise ValueError("Malformed, duplicate or unknown ABI field")
        pattern = r"[0-9a-fA-F]{64}" if key == "binary_sha256" else r"[0-9]+"
        if not re.fullmatch(pattern, value):
            raise ValueError(f"Invalid ABI field {key}")
        if key != "binary_sha256" and int(value) > 0xFFFFFFFF:
            raise ValueError(f"ABI field {key} exceeds uint32")
        result[key] = value.lower() if key == "binary_sha256" else str(int(value))
    if result.keys() != KEYS:
        raise ValueError("Missing ABI fields")
    return result


def validate_fixture(reference, fixture, offset):
    expected = int(reference["sdk_version"])
    declared = int(fixture["sdk_version"])
    if declared != expected + offset:
        raise ValueError(f"Fixture SDK must differ by {offset:+d}")
    for key in KEYS - {"binary_sha256", "sdk_version"}:
        if reference[key] != fixture[key]:
            raise ValueError(f"Fixture differs in unrelated ABI field {key}")
    return expected, declared


def rejection_error(returncode, output, module, expected, declared, sentinel_exists):
    if returncode != 2:
        return f"Expected required-game rejection exit 2, got {returncode}"
    if sentinel_exists:
        return "Fixture executed a constructor, injection or factory phase"
    diagnostic = (f"Module '{module}' rejected before OS load: SDK ABI version mismatch: "
                  f"field 'sdk_version' host expects {expected}, module declares {declared}; "
                  "stable-v1 module ABI is exact-match only (N-1 modules are not loaded)")
    if diagnostic not in output:
        return "Missing exact requested-path pre-load SDK mismatch diagnostic"
    if "SPARK_MODULE_READY" in output:
        return "Rejected run reported a ready module"
    records = [line for line in output.splitlines() if line.startswith("SPARK_HEADLESS_LIFECYCLE")]
    expected_record = ("SPARK_HEADLESS_LIFECYCLE initialized=0 updated=0 fixed=0 "
                       "rendered=0 unloaded=0 faults=0")
    if records != [expected_record]:
        return "Expected one zero-callback lifecycle record"
    return None


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def checked_image(path):
    if not path.is_file():
        raise ValueError(f"Missing module image: {path}")
    sidecar = Path(str(path) + ".sparkabi")
    fields = parse_sidecar(sidecar.read_text(encoding="utf-8"))
    if fields["binary_sha256"] != digest(path):
        raise ValueError(f"Image/sidecar hash mismatch: {path}")
    return fields, sidecar


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installed-root", required=True, type=Path)
    parser.add_argument("--newer", required=True, type=Path)
    parser.add_argument("--previous", required=True, type=Path)
    parser.add_argument("--evidence-root", required=True, type=Path)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--configuration", required=True, choices=("MinSizeRel",))
    args = parser.parse_args(argv)
    if os.name != "nt":
        parser.error("This qualification route requires the native Windows installed host")
    if not re.fullmatch(r"[0-9a-f]{40}", args.source_sha):
        parser.error("--source-sha must be the exact 40-character source revision")
    prefix = args.installed_root.resolve(strict=True)
    evidence = args.evidence_root.resolve(strict=True)
    if evidence.is_relative_to(prefix):
        parser.error("Evidence must be outside the installed prefix")
    if not str(evidence).isascii() or len(str(evidence)) > 160:
        parser.error("Use a short ASCII evidence path for the existing fixture's narrow fopen sentinel")
    engine = prefix / "bin" / "SparkEngine.exe"
    reference_image = prefix / "bin" / "SparkGameFPS.dll"
    if not engine.is_file() or (prefix / "CMakeCache.txt").exists() or (prefix / "bin/CMakeCache.txt").exists():
        parser.error("Expected an installed prefix containing bin/SparkEngine.exe")
    if not engine.resolve().is_relative_to(prefix) or not reference_image.resolve().is_relative_to(prefix):
        parser.error("Installed host and reference image must resolve inside the prefix")
    reference, reference_sidecar = checked_image(reference_image)
    fixtures = []
    for name, supplied, offset, filename in (
        ("newer", args.newer, 1, "SparkMismatchedModuleFixture.dll"),
        ("previous", args.previous, -1, "SparkPreviousSdkModuleFixture.dll"),
    ):
        source = supplied.resolve(strict=True)
        if source.name != filename:
            parser.error(f"Expected existing target artifact {filename}")
        fields, sidecar = checked_image(source)
        expected, declared = validate_fixture(reference, fields, offset)
        fixtures.append((name, source, sidecar, expected, declared))
    tracked = [engine, reference_image, reference_sidecar]
    for _, source, sidecar, _, _ in fixtures:
        tracked.extend((source, sidecar))
    before = {str(path): digest(path) for path in tracked}
    root = Path(tempfile.mkdtemp(prefix="installed-abi-", dir=evidence))
    report = {"declared_source_sha": args.source_sha, "declared_configuration": args.configuration,
              "scope": "installed-host pre-load SDK N+1/N-1 rejection", "inputs": before,
              "cases": [], "passed": False}
    try:
        for name, source, sidecar, expected, declared in fixtures:
            case = root / name
            case.mkdir()
            module = case / source.name
            shutil.copyfile(source, module)
            shutil.copyfile(sidecar, Path(str(module) + ".sparkabi"))
            sentinel = case / "unexpected-execution.txt"
            # Establish writable sentinel storage without touching installed files.
            sentinel.write_text("writability-control", encoding="ascii")
            sentinel.unlink()
            env = os.environ.copy()
            env.pop("SPARK_ENGINE_DIR", None)
            env.update(SPARK_RHI_BACKEND="null", SPARK_MODULE_ABI_SENTINEL=str(sentinel),
                       LOCALAPPDATA=str(case / "local"), APPDATA=str(case / "roaming"))
            # Logs/config/saves/trace use LOCALAPPDATA even if Windows startup
            # anchors a manifest-bearing package's cwd back at the executable.
            (case / "local").mkdir()
            (case / "roaming").mkdir()
            command = [str(engine), "-headless", "-game", str(module), "-require-game",
                       "-test-frames", "8", "-threads", "2", "-no-subprocess"]
            result = subprocess.run(command, cwd=case, env=env, capture_output=True,
                                    text=True, encoding="utf-8", errors="replace", timeout=120)
            (case / "stdout.log").write_text(result.stdout, encoding="utf-8")
            (case / "stderr.log").write_text(result.stderr, encoding="utf-8")
            error = rejection_error(result.returncode, result.stdout + "\n" + result.stderr,
                                    str(module), expected, declared, sentinel.exists())
            if digest(module) != before[str(source)] or digest(Path(str(module) + ".sparkabi")) != before[str(sidecar)]:
                error = "Copied fixture image or sidecar changed during qualification"
            report["cases"].append({"name": name, "command": command, "cwd": str(case), "exit": result.returncode,
                                    "expected_sdk": expected, "declared_sdk": declared, "error": error})
        if {str(path): digest(path) for path in tracked} != before:
            raise ValueError("Qualification input changed during execution")
        report["passed"] = len(report["cases"]) == 2 and all(c["error"] is None for c in report["cases"])
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
        if isinstance(error, subprocess.TimeoutExpired):
            for filename, content in (("stdout.log", error.stdout), ("stderr.log", error.stderr)):
                if isinstance(content, bytes):
                    content = content.decode("utf-8", errors="replace")
                (case / filename).write_text(content or "", encoding="utf-8")
    finally:
        (root / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(root / "report.json")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
