#!/usr/bin/env python3
"""Unit tests for the RHI-210 capture/review packet tool."""

from __future__ import annotations

import importlib.util
import json
import re
import struct
import shutil
import unittest
import zlib
from unittest import mock
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("rhi210_capture", ROOT / "Tools" / "rhi210_capture.py")
assert SPEC and SPEC.loader
capture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(capture)


def png(width: int, height: int, rgb: bytes, *, alpha: bool = False) -> bytes:
    channels = 4 if alpha else 3
    rows = b"".join(b"\x00" + rgb[y * width * channels:(y + 1) * width * channels] for y in range(height))
    color_type = 6 if alpha else 2
    ihdr = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")


class CaptureToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.root = ROOT / (".rhi210-test-data-" + __import__("uuid").uuid4().hex)
        self.root.mkdir()

    def tearDown(self) -> None:
        if self.root.exists():
            self.assertEqual(self.root.resolve().parent, ROOT.resolve())
            shutil.rmtree(self.root)

    def seed_records(self, root: Path, value: int = 0, mutant_value: int = 20) -> None:
        for run in range(1, 6):
            directory = root / f"run-{run}"
            directory.mkdir()
            for file_name, count, scenes in capture.LANES:
                stem = Path(file_name).stem
                (directory / (stem + ".log")).write_text("pending", encoding="utf-8")
                junit = directory / (stem + ".xml")
                cases = []
                for name in capture.TESTS_BY_LANE[file_name]:
                    failure = ("FATAL: entry != entries.end() was false" if name.endswith("ManifestHasEveryScene")
                               else "FAIL: RHI210Golden::MatchesGolden(...) was false") + f" ({file_name}:1)"
                    cases.append(f'<testcase name="{name}"><failure>{failure}</failure></testcase>')
                junit.write_text("<testsuite>" + "".join(cases) + "</testsuite>", encoding="utf-8")
                record = {"returncode": 1, "timedOut": False, "stdout": "pending", "stderr": "", "junitPath": str(junit)}
                (directory / (stem + ".json")).write_text(__import__("json").dumps(record), encoding="utf-8")
                for scene in scenes:
                    (directory / f"d3d11-warp_{scene}.png").write_bytes(png(1, 1, bytes((value + run - 1, 0, 0))))
        for scene, (token, file_name, count) in capture.MUTANTS.items():
            directory = root / "mutants" / token
            directory.mkdir(parents=True, exist_ok=True)
            stem = Path(file_name).stem
            log = directory / (stem + "_" + scene + ".log")
            log.write_text("pending", encoding="utf-8")
            junit = directory / (stem + "_" + scene + ".xml")
            cases = []
            for name in capture.TESTS_BY_LANE[file_name]:
                failure = ("FATAL: entry != entries.end() was false" if name.endswith("ManifestHasEveryScene")
                           else "FAIL: RHI210Golden::MatchesGolden(...) was false") + f" ({file_name}:1)"
                cases.append(f'<testcase name="{name}"><failure>{failure}</failure></testcase>')
            junit.write_text("<testsuite>" + "".join(cases) + "</testsuite>", encoding="utf-8")
            record = {"returncode": 1, "timedOut": False, "stdout": "pending", "stderr": "", "junitPath": str(junit)}
            (directory / (stem + "_" + scene + ".json")).write_text(__import__("json").dumps(record), encoding="utf-8")
            (directory / f"d3d11-warp_{scene}.png").write_bytes(png(1, 1, bytes((mutant_value, 0, 0))))

    def test_png_decoder_handles_rgb_and_rgba(self) -> None:
        rgb = bytes((1, 2, 3, 4, 5, 6))
        (self.root / "rgb.png").write_bytes(png(2, 1, rgb))
        self.assertEqual(capture._png(self.root / "rgb.png"), (2, 1, rgb))
        rgba = bytes((1, 2, 3, 255, 4, 5, 6, 128))
        (self.root / "rgba.png").write_bytes(png(2, 1, rgba, alpha=True))
        self.assertEqual(capture._png(self.root / "rgba.png"), (2, 1, rgb))

    def test_numeric_variance_proposes_threshold_and_mutants_exceed(self) -> None:
        self.seed_records(self.root)
        report = capture.analyze(self.root)
        self.assertEqual(report["status"], "awaiting-owner-review")
        self.assertEqual(report["entries"][0]["proposedPerPixelThreshold"], 4)
        self.assertTrue(all(entry["mutant"]["exceedsProposal"] for entry in report["entries"]))
        self.assertTrue((self.root / "contact-sheet.html").is_file())

    def test_missing_capture_is_rejected(self) -> None:
        with self.assertRaises(capture.CaptureError):
            capture.analyze(self.root)

    def test_identical_mutant_is_rejected(self) -> None:
        self.seed_records(self.root, mutant_value=0)
        with self.assertRaises(capture.CaptureError):
            capture.analyze(self.root)

    def test_unexpected_dirty_log_is_rejected(self) -> None:
        junit = self.root / "dirty.xml"
        junit.write_text('<testsuite><testcase name="D3D11FrameGolden_ManifestHasEveryScene"><failure>ASSERT bad</failure></testcase>' +
                         "".join(f'<testcase name="{name}"/>' for name in capture.TESTS_BY_LANE[capture.LANES[0][0]][1:]) + "</testsuite>", encoding="utf-8")
        result = {"returncode": 1, "timedOut": False, "stdout": "", "stderr": "fatal error", "junitPath": str(junit)}
        with self.assertRaises(capture.CaptureError):
            capture._validate_capture_result(result, "dirty", capture.LANES[0][0], 5)

    def test_reanalysis_rejects_forged_or_missing_record(self) -> None:
        self.seed_records(self.root)
        record = self.root / "run-1" / "TestRHI210D3D11FrameGoldenReal.json"
        record.unlink()
        with self.assertRaises(capture.CaptureError):
            capture.analyze(self.root)

    def test_zero_variance_proposes_zero(self) -> None:
        self.seed_records(self.root, mutant_value=30)
        for run in range(1, 6):
            for _, _, scenes in capture.LANES:
                for scene in scenes:
                    (self.root / f"run-{run}" / f"d3d11-warp_{scene}.png").write_bytes(png(1, 1, bytes((7, 0, 0))))
        report = capture.analyze(self.root)
        self.assertEqual(report["entries"][0]["proposedPerPixelThreshold"], 0)

    def test_timeout_bytes_are_normalized(self) -> None:
        with mock.patch("subprocess.run", side_effect=__import__("subprocess").TimeoutExpired("x", 1, output=b"out", stderr=b"err")):
            result = capture._run_one(Path("spark.exe"), self.root, capture.LANES[0][0], 5, junit_path=self.root / "x.xml")
        self.assertEqual(result["stdout"], "out")
        self.assertEqual(result["stderr"], "err")

    def test_truncated_png_is_rejected(self) -> None:
        path = self.root / "broken.png"
        path.write_bytes(png(1, 1, bytes((1, 2, 3)))[:-4])
        with self.assertRaises(capture.CaptureError):
            capture._png(path)

    def test_expected_golden_failure_cannot_hide_another_assertion(self) -> None:
        self.seed_records(self.root)
        path = self.root / "run-1/TestRHI210D3D11FrameGoldenReal.xml"
        tree = capture.ET.parse(path)
        failure = tree.getroot().findall("testcase")[1].find("failure")
        failure.text += "\nFAIL: validation->errors == 0 (1 != 0) at fixture.h:123"
        tree.write(path, encoding="utf-8")
        with self.assertRaisesRegex(capture.CaptureError, "validation->errors"):
            capture.analyze(self.root)

    def test_reanalysis_rejects_changed_log_and_crash_exit(self) -> None:
        self.seed_records(self.root)
        root = self.root / "run-1"
        log = root / "TestRHI210D3D11FrameGoldenReal.log"
        log.write_text("changed", encoding="utf-8")
        with self.assertRaisesRegex(capture.CaptureError, "log or JUnit identity"):
            capture.analyze(self.root)
        log.write_text("pending", encoding="utf-8")
        record = log.with_suffix(".json")
        data = json.loads(record.read_text())
        data["returncode"] = -1073741819
        record.write_text(json.dumps(data), encoding="utf-8")
        with self.assertRaisesRegex(capture.CaptureError, "unexpected process exit"):
            capture.analyze(self.root)

    def test_oversized_inflated_stream_is_rejected(self) -> None:
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
        data = png(1, 1, bytes((0, 0, 0)))
        bomb = data[:33] + chunk(b"IDAT", zlib.compress(bytes(65536))) + data[-12:]
        path = self.root / "bomb.png"
        path.write_bytes(bomb)
        with self.assertRaisesRegex(capture.CaptureError, "exceeds limit"):
            capture._png(path)

    def test_lane_names_counts_and_pending_scenes_match_sources(self) -> None:
        cmake = (ROOT / "Tests/CMakeLists.txt").read_text(encoding="utf-8")
        manifest = json.loads((ROOT / "Tests/GoldenImages/manifest.json").read_text())
        declared = {e["scene"] for e in manifest["entries"]
                    if e["backendRow"] == "d3d11-warp" and e["scene"] in capture.MUTANTS}
        scenes = set()
        for file_name, count, lane_scenes in capture.LANES:
            source = (ROOT / "Tests" / file_name).read_text(encoding="utf-8")
            names = re.findall(r"\bTEST\((\w+)\)", source)
            self.assertEqual(set(names), set(capture.TESTS_BY_LANE[file_name]))
            self.assertEqual(count, len(names))
            self.assertIn(f"SPARK_TEST_FILE={file_name};SPARK_TEST_EXPECT_COUNT={count};", cmake)
            scenes.update(lane_scenes)
        self.assertEqual(declared, scenes)

    def test_capture_cannot_write_inside_baseline_directory(self) -> None:
        with self.assertRaisesRegex(capture.CaptureError, "outside the committed baseline"):
            capture.capture(Path("unused.exe"), capture.GOLDEN_ROOT / "unreviewed")


if __name__ == "__main__":
    unittest.main()
