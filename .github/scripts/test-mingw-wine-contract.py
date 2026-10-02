#!/usr/bin/env python3
"""CI-100: reject launch-only, stale/empty rendering and reduced Wine test evidence."""

import copy
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "Tools"))
from buildmatrix.workflow import parse_workflow_yaml


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SMOKE = load("mingw_smoke", ROOT / ".github/scripts/mingw-wine-smoke.py")
POLICY = load("mingw_policy", ROOT / ".github/scripts/test-workflow-failure-propagation.py")


class WineEvidenceTests(unittest.TestCase):
    def test_test_floor_failure_warning_and_missing_summary(self):
        self.assertEqual(SMOKE.MINIMUM_TESTS, 7500)
        self.assertEqual(SMOKE.check_tests("Tests:      7500 passed, 0 failed, 7500 total\n")["passedTests"], 7500)
        for text in ("", "Tests: 7499 passed, 0 failed, 7499 total", "Tests: 7500 passed, 1 failed, 7501 total",
                     "Tests: 7500 passed, 0 failed, 1 warned, 7501 total",
                     "Tests: 7500 passed, 0 failed\nTests: 7500 passed, 0 failed"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                SMOKE.check_tests(text)

    def test_cpu_selection_requires_dxvk_version_and_cpu_device(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            with self.assertRaises(ValueError):
                SMOKE.check_cpu_log(directory)
            log = directory / "SparkEngine_d3d11.log"
            dxgi = directory / "SparkEngine_dxgi.log"
            cpu = "info:  Creating device:\ninfo:  llvmpipe (LLVM 21.1.8, 256 bits):\n"
            log.write_text(cpu)
            with self.assertRaises(ValueError):
                SMOKE.check_cpu_log(directory)  # D3D11 alone is not version proof.
            for version, device in (("2.5.3", cpu), ("3.1.10", cpu),
                                    ("3.1.1", "info:  Creating device:\ninfo:  NVIDIA GPU:\n"),
                                    ("3.1.1", "info:  llvmpipe (LLVM):\n")):
                dxgi.write_text("info:  DXVK: v" + version + "\n")
                log.write_text(device)
                with self.subTest(version=version, device=device), self.assertRaises(ValueError):
                    SMOKE.check_cpu_log(directory)
            dxgi.write_text("info:  DXVK: v3.1.1\n")
            log.write_text(cpu)
            SMOKE.check_cpu_log(directory)
            dxgi.rename(directory / "OtherProcess_dxgi.log")
            with self.assertRaises(ValueError):
                SMOKE.check_cpu_log(directory)  # A different process cannot supply the pin.

    @unittest.skipUnless(os.name == "posix", "Wine execution contract runs on POSIX")
    def test_editor_discards_both_save_only_logs(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            d3d11 = directory / "SparkEditor_d3d11.log"
            dxgi = directory / "SparkEditor_dxgi.log"

            def launch(executable, args, output, env, log_name, seconds):
                if log_name == "editor-save.log":
                    scene = Path(args[args.index("--save-scene") + 1].removeprefix("Z:"))
                    scene.write_text(json.dumps({"entities": [{"id": 1}]}))
                    d3d11.write_text("save-only device evidence")
                    dxgi.write_text("save-only version evidence")
                else:
                    self.assertFalse(d3d11.exists())
                    self.assertFalse(dxgi.exists())

            with patch.object(SMOKE, "run_wine", side_effect=launch) as run, \
                    patch.object(SMOKE, "check_editor", return_value={"fresh": True}), \
                    patch.object(Path, "is_file", return_value=True):
                self.assertEqual(SMOKE.execute("editor", directory / "build", directory), {"fresh": True})
                self.assertEqual(run.call_count, 2)

    def test_editor_rejects_launch_only_occlusion_and_no_project(self):
        baseline = dict(schema=1, status="passed", projectLoaded=True, runResult=0,
                        renderedFrames=60, presentFailures=0, graphicsBackend="d3d11")
        with tempfile.TemporaryDirectory() as tmp, patch.object(SMOKE, "check_cpu_log"):
            path = Path(tmp) / "editor-result.json"
            path.write_text(json.dumps(baseline))
            self.assertTrue(SMOKE.check_editor(Path(tmp))["cpuRendering"])
            for key, value in (("renderedFrames", 0), ("renderedFrames", 59), ("presentFailures", 1),
                               ("graphicsBackend", "null"), ("projectLoaded", False), ("runResult", 255),
                               ("status", "scene-open-failed")):
                mutant = dict(baseline, **{key: value})
                path.write_text(json.dumps(mutant))
                with self.subTest(key=key), self.assertRaises(ValueError):
                    SMOKE.check_editor(Path(tmp))

    def test_engine_requires_real_command_response_draws_and_final_frame(self):
        audit = ("frame 0 t=0.0s | ok  | gfx_vsync off\n    > VSync disabled\n"
                 "frame 5 t=0.5s | ok  | gfx_screenshot frame-05.png\n"
                 "frame 59 t=2.0s | ok  | gfx_metrics\nDraw Calls: 1\nTriangles: 12\n")
        with tempfile.TemporaryDirectory() as tmp, patch.object(SMOKE, "check_cpu_log"), \
                patch.object(SMOKE, "check_capture", return_value="digest"):
            directory = Path(tmp)
            (directory / "engine.log").write_text("SPARK_SCENE_LOADED entities=3 renderables=1\n")
            path = directory / "exec-audit.log"
            path.write_text(audit)
            self.assertTrue(SMOKE.check_engine(directory)["agentControl"])
            for before, after in (("frame 59", "frame 58"), ("VSync disabled", "Unknown command"),
                                  ("Draw Calls: 1", "Draw Calls: 0"), ("Triangles: 12", "Triangles: 0"),
                                  ("ok  | gfx_vsync", "ERR | gfx_vsync")):
                path.write_text(audit.replace(before, after))
                with self.subTest(before=before), self.assertRaises(ValueError):
                    SMOKE.check_engine(directory)

    def test_capture_rejects_clear_only_and_invalid_png(self):
        def chunk(kind, data):
            return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

        def png(raw):
            return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 640, 480, 8, 2, 0, 0, 0))
                    + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "capture.png"
            pixels = bytearray((640 * 3 + 1) * 480)
            path.write_bytes(png(pixels))
            with self.assertRaises(ValueError):
                SMOKE.check_capture(path)
            pixels[1] = 255
            path.write_bytes(png(pixels))
            self.assertEqual(len(SMOKE.check_capture(path)), 64)
            path.write_bytes(b"not a PNG")
            with self.assertRaises(ValueError):
                SMOKE.check_capture(path)

    def test_rc255_and_failure_never_pass(self):
        with tempfile.TemporaryDirectory() as tmp, patch.object(SMOKE.subprocess, "Popen") as popen:
            for code in (255, 1):
                popen.return_value.wait.return_value = code
                with self.subTest(code=code), self.assertRaises(ValueError):
                    SMOKE.run_wine(Path("engine.exe"), [], Path(tmp), {}, "engine.log", 5)

    def test_workflow_rejects_removed_bypassed_and_advisory_smokes(self):
        source = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        baseline = parse_workflow_yaml(source)
        self.assertEqual(POLICY.experimental_mingw_lane_errors(baseline), [])
        for identity in ("mingw-engine-smoke", "mingw-editor-smoke", "mingw-tests"):
            for mutation in ("removed", "run", "if", "continue-on-error"):
                doc = copy.deepcopy(baseline)
                steps = doc["jobs"]["build-linux-mingw-wine"]["steps"]
                step = next(s for s in steps if s.get("id") == identity)
                if mutation == "removed":
                    steps.remove(step)
                else:
                    step[mutation] = True if mutation == "continue-on-error" else "true"
                with self.subTest(identity=identity, mutation=mutation):
                    self.assertTrue(POLICY.experimental_mingw_lane_errors(doc))
        for name in ("Summarize experimental MinGW execution", "Upload test results"):
            doc = copy.deepcopy(baseline)
            step = next(s for s in doc["jobs"]["build-linux-mingw-wine"]["steps"] if s.get("name") == name)
            step["if"] = "success()"
            self.assertTrue(POLICY.experimental_mingw_lane_errors(doc))

    def test_exclusions_are_specific_and_preserve_credential_tests(self):
        names = SMOKE.exclusions().split(",")
        self.assertEqual(len(names), 23)
        self.assertEqual(len(names), len(set(names)))
        self.assertTrue(all(re.fullmatch(r"[A-Za-z0-9]+_[A-Za-z0-9_]+", name) for name in names))
        for name in ("GatewaySecurity_AcceptsOnceAndRejectsReplay", "GatewaySecurity_RejectsTamperingAndExpiredTimestamp",
                     "GatewaySecurity_RejectsOversizedCredential"):
            self.assertFalse(any(excluded in name for excluded in names))

    def test_setup_is_hash_pinned_and_directxmath_has_one_owner(self):
        source = (ROOT / "tools/setup-mingw-wine.sh").read_text(encoding="utf-8")
        self.assertIn('DXVK_URL="https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz"', source)
        self.assertIn('DXVK_SHA256="40565b4a724aadc4433fa4e010b4b23916d9b1f1baeee64e17186db94f54e608"', source)
        self.assertLess(source.index('sha256sum --check --strict -'), source.index('tar xzf'))
        self.assertNotIn('install_directxmath', source)
        self.assertNotIn('DirectXMath/main', source)

    def test_corrupt_download_cannot_extract_even_with_existing_dll(self):
        """Execute the real installer with a fake downloader and a tampered archive."""
        bash = shutil.which("bash")
        if not bash:
            self.skipTest("Bash is unavailable; run this test in Linux/WSL")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "tools").mkdir()
            (root / "ThirdParty/dxvk/x64").mkdir(parents=True)
            (root / "ThirdParty/dxvk/x64/d3d11.dll").write_text("old DLL")
            script = root / "tools/setup-mingw-wine.sh"
            shutil.copyfile(ROOT / "tools/setup-mingw-wine.sh", script)
            # Shell functions are injected by BASH_ENV; network and extraction are never performed.
            shim = root / "shim.sh"
            shim.write_text('curl() { while [ "$1" != "--output" ]; do shift; done; shift; '
                            'printf corrupt > "$1"; }\ntar() { echo UNSAFE_EXTRACTION; return 99; }\n',
                            encoding="utf-8", newline="\n")
            result = subprocess.run([bash, str(script), "--dxvk-only"],
                                    env=dict(os.environ, BASH_ENV=str(shim)), capture_output=True, text=True, timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("DXVK SHA-256 mismatch", result.stdout)
            self.assertNotIn("UNSAFE_EXTRACTION", result.stdout)
            self.assertEqual((root / "ThirdParty/dxvk/x64/d3d11.dll").read_text(), "old DLL")


if __name__ == "__main__":
    unittest.main(verbosity=2)
