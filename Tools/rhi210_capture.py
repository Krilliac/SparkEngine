#!/usr/bin/env python3
"""Capture and review packet tooling for the RHI-210 WARP golden lanes.

This tool deliberately treats the test executable as a capture oracle.  It
never writes repository baselines or manifest entries, and a successful PNG
readback is not treated as a passing golden test.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import html
import json
import math
import os
import re
import struct
import subprocess
import sys
import zlib
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Iterable

RUNS = 5
TIMEOUT_SECONDS = 300
GOLDEN_ROOT = Path(__file__).resolve().parents[1] / "Tests/GoldenImages"
SELECTORS = (
    "SPARK_TEST_FILE", "SPARK_TEST_NAME", "SPARK_TEST_NAME_PREFIX", "SPARK_TEST_EXCLUDE",
    "SPARK_TEST_LIMIT", "SPARK_TEST_EXPECT_COUNT", "SPARK_RHI210_DISABLE_PASS",
)
TESTS_BY_LANE = {
    "TestRHI210D3D11FrameGoldenReal.cpp": ("D3D11FrameGolden_ManifestHasEveryScene", "D3D11FrameGolden_ForwardLit", "D3D11FrameGolden_ForwardMaterial", "D3D11FrameGolden_DrawList", "D3D11FrameGolden_PostChain"),
    "TestRHI210D3D11SceneGoldenReal.cpp": ("D3D11SceneGolden_ManifestHasEveryScene", "D3D11SceneGolden_FPSLevel1MainCamera", "D3D11SceneGolden_FPSLevel1Overview"),
    "TestRHI210D3D11WorldGoldenReal.cpp": ("D3D11WorldGolden_ManifestHasEveryScene", "D3D11WorldGolden_OpaqueAndSprites"),
    "TestRHI210D3D11PrimaryGoldenReal.cpp": ("D3D11PrimaryGolden_ManifestHasEveryScene", "D3D11PrimaryGolden_DeferredGeometry", "D3D11PrimaryGolden_DeferredLighting", "D3D11PrimaryGolden_ShadowDepth"),
}
TEST_NAME_BY_SCENE = {
    "Frame_ForwardLit": "D3D11FrameGolden_ForwardLit",
    "Frame_ForwardMaterial": "D3D11FrameGolden_ForwardMaterial",
    "Frame_DrawList": "D3D11FrameGolden_DrawList",
    "Frame_PostChain": "D3D11FrameGolden_PostChain",
    "Scene_FPSLevel1_MainCamera": "D3D11SceneGolden_FPSLevel1MainCamera",
    "Scene_FPSLevel1_Overview": "D3D11SceneGolden_FPSLevel1Overview",
    "World_OpaqueAndSprites": "D3D11WorldGolden_OpaqueAndSprites",
    "Primary_DeferredGeometry": "D3D11PrimaryGolden_DeferredGeometry",
    "Primary_DeferredLighting": "D3D11PrimaryGolden_DeferredLighting",
    "Primary_ShadowDepth": "D3D11PrimaryGolden_ShadowDepth",
}
LANES = (
    ("TestRHI210D3D11FrameGoldenReal.cpp", 5, ("Frame_ForwardLit", "Frame_ForwardMaterial", "Frame_DrawList", "Frame_PostChain")),
    ("TestRHI210D3D11SceneGoldenReal.cpp", 3, ("Scene_FPSLevel1_MainCamera", "Scene_FPSLevel1_Overview")),
    ("TestRHI210D3D11WorldGoldenReal.cpp", 2, ("World_OpaqueAndSprites",)),
    ("TestRHI210D3D11PrimaryGoldenReal.cpp", 4, ("Primary_DeferredGeometry", "Primary_DeferredLighting", "Primary_ShadowDepth")),
)
MUTANTS = {
    "Frame_ForwardLit": ("forward", "TestRHI210D3D11FrameGoldenReal.cpp", 5),
    "Frame_ForwardMaterial": ("forward-material", "TestRHI210D3D11FrameGoldenReal.cpp", 5),
    "Frame_DrawList": ("drawlist", "TestRHI210D3D11FrameGoldenReal.cpp", 5),
    "Frame_PostChain": ("postchain", "TestRHI210D3D11FrameGoldenReal.cpp", 5),
    "Scene_FPSLevel1_MainCamera": ("scene", "TestRHI210D3D11SceneGoldenReal.cpp", 3),
    "Scene_FPSLevel1_Overview": ("scene", "TestRHI210D3D11SceneGoldenReal.cpp", 3),
    "World_OpaqueAndSprites": ("world", "TestRHI210D3D11WorldGoldenReal.cpp", 2),
    "Primary_DeferredGeometry": ("deferred-geometry", "TestRHI210D3D11PrimaryGoldenReal.cpp", 4),
    "Primary_DeferredLighting": ("deferred-lighting", "TestRHI210D3D11PrimaryGoldenReal.cpp", 4),
    "Primary_ShadowDepth": ("shadow", "TestRHI210D3D11PrimaryGoldenReal.cpp", 4),
}


class CaptureError(RuntimeError):
    pass


def _environment(output: Path, *, disable: str | None = None, file_name: str, count: int) -> dict[str, str]:
    env = os.environ.copy()
    for name in list(env):
        if name.upper().startswith("SPARK_TEST_"):
            del env[name]
    for name in SELECTORS:
        env.pop(name, None)
    env.update({
        "SPARK_TEST_FILE": file_name,
        "SPARK_TEST_EXPECT_COUNT": str(count),
        "SPARK_GOLDEN_WRITE_ACTUAL": "1",
        "SPARK_GOLDEN_OUTPUT_DIR": str(output),
    })
    if disable is not None:
        env["SPARK_RHI210_DISABLE_PASS"] = disable
    return env


def _run_one(exe: Path, output: Path, file_name: str, count: int, *, disable: str | None = None,
             junit_path: Path) -> dict:
    env = _environment(output, disable=disable, file_name=file_name, count=count)
    try:
        completed = subprocess.run(
            [str(exe), "--warn-is-error", "--empty-is-error", "--junit-xml", str(junit_path)],
            cwd=str(exe.parent), env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=TIMEOUT_SECONDS, check=False,
        )
        stdout = completed.stdout or ""
        stderr = completed.stderr or ""
        timed_out = False
    except subprocess.TimeoutExpired as error:
        stdout = error.stdout or ""
        stderr = error.stderr or ""
        completed = None
        timed_out = True
    if isinstance(stdout, bytes):
        stdout = stdout.decode("utf-8", errors="replace")
    if isinstance(stderr, bytes):
        stderr = stderr.decode("utf-8", errors="replace")
    return {"returncode": None if completed is None else completed.returncode, "timedOut": timed_out,
            "stdout": stdout, "stderr": stderr, "junitPath": str(junit_path)}


def _write_result(path: Path, result: dict) -> None:
    path.write_text(result["stdout"] + result["stderr"], encoding="utf-8", errors="replace")
    path.with_suffix(".json").write_text(json.dumps({k: v for k, v in result.items()}, indent=2), encoding="utf-8")


def _validate_junit(path: Path, file_name: str, count: int, *, mutant_scene: str | None = None) -> int:
    if not path.is_file():
        raise CaptureError(f"{file_name}: missing JUnit report {path}")
    try:
        testcases = ET.parse(path).getroot().findall(".//testcase")
    except ET.ParseError as error:
        raise CaptureError(f"{file_name}: invalid JUnit report: {error}") from error
    expected = set(TESTS_BY_LANE[file_name])
    actual = [case.get("name", "") for case in testcases]
    if len(testcases) != count or set(actual) != expected or len(set(actual)) != len(actual):
        raise CaptureError(f"{file_name}: JUnit testcase set/count mismatch: {actual}")
    failed = 0
    token = MUTANTS[mutant_scene][0] if mutant_scene is not None else None
    affected = {TEST_NAME_BY_SCENE[s] for s, info in MUTANTS.items() if info[0] == token}
    if token == "forward":
        affected.update(("D3D11FrameGolden_ForwardMaterial", "D3D11FrameGolden_PostChain"))
    if token == "deferred-geometry":
        affected.add("D3D11PrimaryGolden_DeferredLighting")
    pixel_assertions = (
        "Spark::GoldenImageTestRunner::FrameHasRenderedContent(", "tally.failures == 0 ", "failures == 0 ",
        "RHI210Golden::ChannelError(", "changed > ", "post->GetActivePassCount() == 4 ",
        "stats.drawn == 6u ", "HasWrittenTexels(", "draws == 1u ",
    )
    for case in testcases:
        if (any(case.find(tag) is not None for tag in ("skipped", "error", "flakyFailure"))
                or case.get("empty") in ("1", "true")
                or any(p.get("name") in ("empty", "flaky", "waived-assertions")
                       and p.get("value") not in ("0", "false") for p in case.findall(".//property"))):
            raise CaptureError(f"{file_name}/{case.get('name')}: skipped or empty testcase")
        failures = case.findall("failure")
        if not failures:
            continue
        if len(failures) != 1:
            raise CaptureError(f"{file_name}: duplicate failure records")
        failed += 1
        text = "".join(failures[0].itertext())
        name = case.get("name", "")
        lines = [line.strip() for line in text.splitlines() if line.strip()]
        if not lines:
            raise CaptureError(f"{file_name}/{name}: empty failure record")
        for line in lines:
            manifest_failure = name.endswith("ManifestHasEveryScene") and re.fullmatch(
                r"FATAL: entry != entries\.end\(\) was false \(.+:\d+\)", line)
            golden_failure = not name.endswith("ManifestHasEveryScene") and re.fullmatch(
                r"FAIL: RHI210Golden::MatchesGolden\(.+\) was false \(.+:\d+\)", line)
            target_failure = name in affected and any(line.startswith("FAIL: " + expr) for expr in pixel_assertions)
            if not (manifest_failure or golden_failure or target_failure):
                raise CaptureError(f"{file_name}/{name}: unexpected JUnit failure: {line[:300]}")
    return failed


def _validate_capture_result(result: dict, label: str, file_name: str, count: int, *, mutant_scene: str | None = None) -> None:
    if (type(result.get("timedOut")) is not bool or type(result.get("returncode")) is not int
            or not isinstance(result.get("stdout"), str) or not isinstance(result.get("stderr"), str)
            or not isinstance(result.get("junitPath"), str)):
        raise CaptureError(f"{label}: malformed process result")
    if result["timedOut"]:
        raise CaptureError(f"{label} timed out after {TIMEOUT_SECONDS}s")
    if result["returncode"] not in (0, 1):
        raise CaptureError(f"{label}: unexpected process exit {result['returncode']}")
    combined = result.get("stdout", "") + result.get("stderr", "")
    for line in combined.splitlines():
        if re.search(r"(?:warning|warnings|error|errors|corruption)\s*[:=]\s*[1-9][0-9]*", line, re.IGNORECASE):
            raise CaptureError(f"{label} reports non-zero validation diagnostics: {line[:300]}")
    failed = _validate_junit(Path(result["junitPath"]), file_name, count, mutant_scene=mutant_scene)
    if (result["returncode"] == 0) != (failed == 0):
        raise CaptureError(f"{label}: process exit disagrees with JUnit")


def _png(path: Path) -> tuple[int, int, bytes]:
    max_file_bytes = 64 * 1024 * 1024
    try:
        if path.stat().st_size > max_file_bytes:
            raise CaptureError(f"oversized PNG: {path}")
        with path.open("rb") as stream:
            data = stream.read(max_file_bytes + 1)
    except OSError as error:
        raise CaptureError(f"cannot read PNG {path}: {error}") from error
    if len(data) > max_file_bytes or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise CaptureError(f"invalid or oversized PNG: {path}")
    pos = 8
    idat = bytearray()
    width = height = bit_depth = color_type = interlace = None
    saw_ihdr = saw_idat = saw_iend = idat_ended = False
    while pos < len(data):
        if pos + 12 > len(data):
            raise CaptureError(f"truncated PNG chunk: {path}")
        length = struct.unpack_from(">I", data, pos)[0]
        end = pos + 12 + length
        if length > 64 * 1024 * 1024 or end > len(data):
            raise CaptureError(f"invalid PNG chunk bounds: {path}")
        kind = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        crc = struct.unpack_from(">I", data, pos + 8 + length)[0]
        if binascii.crc32(kind + chunk) & 0xFFFFFFFF != crc:
            raise CaptureError(f"PNG CRC mismatch in {path}")
        if pos == 8 and kind != b"IHDR":
            raise CaptureError(f"IHDR must be first: {path}")
        if saw_idat and kind != b"IDAT":
            idat_ended = True
        if kind == b"IHDR":
            if saw_ihdr or pos != 8 or length != 13:
                raise CaptureError(f"invalid IHDR: {path}")
            width, height, bit_depth, color_type, compression, filt, interlace = struct.unpack(">IIBBBBB", chunk)
            if not width or not height or width * height > 16_777_216 or bit_depth != 8 or color_type not in (2, 6) or compression or filt or interlace:
                raise CaptureError(f"unsupported PNG format: {path}")
            saw_ihdr = True
        elif kind == b"IDAT":
            if not saw_ihdr or saw_iend or idat_ended:
                raise CaptureError(f"IDAT out of order: {path}")
            idat.extend(chunk)
            saw_idat = True
        elif kind == b"IEND":
            if saw_iend or length != 0 or not saw_ihdr or not saw_idat:
                raise CaptureError(f"invalid IEND: {path}")
            saw_iend = True
            pos = end
            break
        elif kind[0] & 32 == 0 and kind != b"PLTE":
            raise CaptureError(f"unsupported critical PNG chunk: {path}")
        pos = end
    if pos != len(data) or not saw_iend or width is None or height is None or not idat:
        raise CaptureError(f"PNG lacks IHDR/IDAT: {path}")
    channels = 4 if color_type == 6 else 3
    stride = width * channels
    expected_bytes = (stride + 1) * height
    if expected_bytes > max_file_bytes:
        raise CaptureError(f"PNG decoded data exceeds limit: {path}")
    decoder = zlib.decompressobj()
    raw = decoder.decompress(bytes(idat), expected_bytes + 1)
    if len(raw) > expected_bytes:
        raise CaptureError(f"PNG decoded data exceeds limit: {path}")
    # decompress(max_length) bounds the entire output. flush(length) is only an
    # initial allocation hint, not an output limit, so it must not be used here.
    if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail or len(raw) != expected_bytes:
        raise CaptureError(f"invalid or oversized PNG stream: {path}")
    rows: list[bytearray] = []
    offset = 0
    for _ in range(height):
        filter_type = raw[offset]
        encoded = raw[offset + 1:offset + 1 + stride]
        offset += stride + 1
        row = bytearray(encoded)
        prior = rows[-1] if rows else bytearray(stride)
        for i in range(stride):
            left = row[i - channels] if i >= channels else 0
            up = prior[i]
            upper_left = prior[i - channels] if i >= channels else 0
            if filter_type == 1:
                row[i] = (row[i] + left) & 255
            elif filter_type == 2:
                row[i] = (row[i] + up) & 255
            elif filter_type == 3:
                row[i] = (row[i] + ((left + up) // 2)) & 255
            elif filter_type == 4:
                estimate = left + up - upper_left
                pa, pb, pc = abs(estimate - left), abs(estimate - up), abs(estimate - upper_left)
                row[i] = (row[i] + (left if pa <= pb and pa <= pc else up if pb <= pc else upper_left)) & 255
            elif filter_type != 0:
                raise CaptureError(f"unsupported PNG filter {filter_type}: {path}")
        rows.append(row)
    rgb = bytearray()
    for row in rows:
        if channels == 3:
            rgb.extend(row)
        else:
            rgb.extend(b"".join(row[i:i + 3] for i in range(0, len(row), 4)))
    return width, height, bytes(rgb)


def _distance(a: tuple[int, int, bytes], b: tuple[int, int, bytes]) -> float:
    if a[:2] != b[:2]:
        raise CaptureError("capture dimensions differ")
    return max(math.sqrt(sum((x - y) ** 2 for x, y in zip(a[2][i:i + 3], b[2][i:i + 3])))
               for i in range(0, len(a[2]), 3))


def _scene_png(root: Path, scene: str) -> Path:
    path = root / f"d3d11-warp_{scene}.png"
    if not path.is_file():
        raise CaptureError(f"missing captured PNG: {path}")
    return path


def _validate_persisted(root: Path, run_label: str, file_name: str, count: int, *, mutant_scene: str | None = None) -> None:
    stem = Path(file_name).stem
    if mutant_scene is None:
        record_path = root / (stem + ".json")
        log_path = root / (stem + ".log")
    else:
        record_path = root / (stem + "_" + mutant_scene + ".json")
        log_path = root / (stem + "_" + mutant_scene + ".log")
    if not record_path.is_file() or not log_path.is_file():
        raise CaptureError(f"{run_label}: missing persisted process record or log")
    try:
        record = json.loads(record_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise CaptureError(f"{run_label}: invalid process record: {error}") from error
    if not isinstance(record, dict):
        raise CaptureError(f"{run_label}: malformed process record")
    expected_junit = log_path.with_suffix(".xml")
    if (not isinstance(record.get("junitPath"), str) or Path(record["junitPath"]).name != expected_junit.name
            or not isinstance(record.get("stdout"), str) or not isinstance(record.get("stderr"), str)
            or log_path.read_text(encoding="utf-8") != record["stdout"] + record["stderr"]):
        raise CaptureError(f"{run_label}: log or JUnit identity differs from process record")
    record["junitPath"] = str(expected_junit)
    _validate_capture_result(record, run_label, file_name, count, mutant_scene=mutant_scene)


def analyze(root: Path) -> dict:
    if root.resolve().is_relative_to(GOLDEN_ROOT.resolve()):
        raise CaptureError("review packets must stay outside the committed baseline directory")
    measurements = []
    for run in range(1, RUNS + 1):
        for file_name, count, _ in LANES:
            _validate_persisted(root / f"run-{run}", f"run-{run}/{file_name}", file_name, count)
    for scene, (token, file_name, count) in MUTANTS.items():
        _validate_persisted(root / "mutants" / token, f"mutant {token}/{scene}", file_name, count, mutant_scene=scene)
    for _, _, scenes in LANES:
        for scene in scenes:
            frames = [_png(_scene_png(root / f"run-{i}", scene)) for i in range(1, RUNS + 1)]
            pair_distances = [_distance(frames[i], frames[j]) for i in range(RUNS) for j in range(i + 1, RUNS)]
            proposed = math.ceil(max(pair_distances))
            measurements.append({
                "scene": scene, "backendRow": "d3d11-warp", "runs": RUNS,
                "pairMaxEuclideanRGB": max(pair_distances), "pairDistances": pair_distances,
                "proposedPerPixelThreshold": proposed, "proposedTolerancePercent": 0,
                "status": "awaiting-owner-review",
                "sha256": [hashlib.sha256(_scene_png(root / f"run-{i}", scene).read_bytes()).hexdigest() for i in range(1, RUNS + 1)],
            })
    by_scene = {item["scene"]: item for item in measurements}
    for scene, (token, _, _) in MUTANTS.items():
        mutant = _png(_scene_png(root / "mutants" / token, scene))
        baseline = _png(_scene_png(root / "run-1", scene))
        distance = _distance(baseline, mutant)
        by_scene[scene]["mutant"] = {"token": token, "maxEuclideanRGB": distance,
                                      "exceedsProposal": distance > by_scene[scene]["proposedPerPixelThreshold"]}
        if not by_scene[scene]["mutant"]["exceedsProposal"]:
            raise CaptureError(f"mutant {token} did not exceed proposed threshold for {scene}")
    document = {"schemaVersion": 1, "status": "awaiting-owner-review", "entries": measurements}
    (root / "measurements.json").write_text(json.dumps(document, indent=2), encoding="utf-8")
    _contact_sheet(root, measurements)
    return document


def _contact_sheet(root: Path, measurements: Iterable[dict]) -> None:
    cards = []
    for item in measurements:
        scene = item["scene"]
        encoded = base64.b64encode(_scene_png(root / "run-1", scene).read_bytes()).decode("ascii")
        cards.append(f"<figure><figcaption>{html.escape(scene)} — proposed threshold {item['proposedPerPixelThreshold']}</figcaption>"
                     f"<img src='data:image/png;base64,{encoded}' alt='{html.escape(scene)}'></figure>")
    page = "<!doctype html><meta charset='utf-8'><title>RHI-210 WARP capture review</title><style>body{font-family:sans-serif}main{display:grid;grid-template-columns:repeat(2,1fr);gap:1rem}img{max-width:100%;image-rendering:auto}</style><main>" + "".join(cards) + "</main>"
    (root / "contact-sheet.html").write_text(page, encoding="utf-8")


def capture(exe: Path, output: Path) -> None:
    if output.resolve().is_relative_to(GOLDEN_ROOT.resolve()):
        raise CaptureError("captures must stay outside the committed baseline directory")
    if output.exists() and any(output.iterdir()):
        raise CaptureError(f"output must be a new empty directory: {output}")
    output.mkdir(parents=True, exist_ok=True)
    for run in range(1, RUNS + 1):
        for file_name, count, _ in LANES:
            run_dir = output / f"run-{run}"
            run_dir.mkdir(parents=True, exist_ok=True)
            junit_path = run_dir / (Path(file_name).stem + ".xml")
            result = _run_one(exe, run_dir, file_name, count, junit_path=junit_path)
            log = run_dir / (Path(file_name).stem + ".log")
            _write_result(log, result)
            _validate_capture_result(result, f"run-{run}/{file_name}", file_name, count)
    for scene, (token, file_name, count) in MUTANTS.items():
        mutant_dir = output / "mutants" / token
        mutant_dir.mkdir(parents=True, exist_ok=True)
        junit_path = mutant_dir / (Path(file_name).stem + "_" + scene + ".xml")
        result = _run_one(exe, mutant_dir, file_name, count, disable=token, junit_path=junit_path)
        _write_result(mutant_dir / (Path(file_name).stem + "_" + scene + ".log"), result)
        _validate_capture_result(result, f"mutant {token}/{scene}", file_name, count, mutant_scene=scene)
    analyze(output)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--analyze", type=Path, help="analyze an existing capture directory")
    args = parser.parse_args(argv)
    try:
        if args.analyze is not None:
            analyze(args.analyze)
        else:
            if args.exe is None:
                parser.error("--exe is required unless --analyze is used")
            if args.output is None:
                parser.error("--output is required unless --analyze is used")
            capture(args.exe.resolve(), args.output.resolve())
    except (CaptureError, OSError, subprocess.SubprocessError, zlib.error) as error:
        print(f"rhi210_capture: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
