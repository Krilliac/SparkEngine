"""Pure verdict/metadata tests; never starts a native host or loads a module."""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock
import contextlib
import io
import json
import subprocess
import tempfile
import os
from types import SimpleNamespace

PATH = Path(__file__).resolve().parents[1] / "PackageSmoke/run_installed_module_abi_rejection.py"
SPEC = importlib.util.spec_from_file_location("installed_abi", PATH)
ABI = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ABI)


class InstalledAbiTests(unittest.TestCase):
    def output(self, module="C:/case/fixture.dll", expected=9, declared=10):
        return (f"Module '{module}' rejected before OS load: SDK ABI version mismatch: "
                f"field 'sdk_version' host expects {expected}, module declares {declared}; "
                "stable-v1 module ABI is exact-match only (N-1 modules are not loaded)\n"
                "SPARK_HEADLESS_LIFECYCLE initialized=0 updated=0 fixed=0 rendered=0 unloaded=0 faults=0\n")

    def verdict(self, output=None, exit_code=2, sentinel=False):
        return ABI.rejection_error(exit_code, self.output() if output is None else output,
                                   "C:/case/fixture.dll", 9, 10, sentinel)

    def test_both_sdk_directions_accept_only_exact_evidence(self):
        self.assertIsNone(self.verdict())
        self.assertIsNone(ABI.rejection_error(2, self.output(declared=8), "C:/case/fixture.dll", 9, 8, False))

    def test_wrong_exit_or_execution_sentinel_fails(self):
        for code in (0, 1, 3, -1):
            with self.subTest(code=code):
                self.assertIsNotNone(self.verdict(exit_code=code))
        self.assertIsNotNone(self.verdict(sentinel=True))

    def test_wrong_path_version_or_loader_stage_fails(self):
        for output in (self.output(module="C:/another.dll"), self.output(expected=8),
                       self.output(declared=8), self.output().replace("before OS load", "after OS load"),
                       "Failed to load a dependency DLL"):
            with self.subTest(output=output):
                self.assertIsNotNone(self.verdict(output))

    def test_any_ready_or_nonzero_callback_evidence_fails(self):
        self.assertIsNotNone(self.verdict(self.output() + "SPARK_MODULE_READY count=1\n"))
        for key in ("initialized", "updated", "fixed", "rendered", "unloaded", "faults"):
            with self.subTest(key=key):
                self.assertIsNotNone(self.verdict(self.output().replace(key + "=0", key + "=1")))

    def test_missing_duplicate_malformed_lifecycle_fails(self):
        output = self.output()
        record = output.splitlines()[-1]
        for bad in (output.replace(record, ""), output + record + "\n", output.replace(record, record + " extra")):
            self.assertIsNotNone(self.verdict(bad))

    def sidecar(self):
        return "\n".join(f"{key}={'a' * 64 if key == 'binary_sha256' else '9'}" for key in sorted(ABI.KEYS))

    def test_metadata_requires_complete_unique_bounded_fields(self):
        good = self.sidecar()
        self.assertEqual(len(ABI.parse_sidecar(good)), 12)
        for bad in (good + "\nsdk_version=9", good + "\nextra=1", good.replace("sdk_version=9", ""),
                    good.replace("sdk_version=9", "sdk_version=4294967296"),
                    good.replace("a" * 64, "ab12"), good.replace("sdk_version=9", "sdk_version=-1")):
            with self.subTest(bad=bad):
                with self.assertRaises(ValueError):
                    ABI.parse_sidecar(bad)

    def test_fixture_must_change_only_sdk_in_requested_direction(self):
        reference = ABI.parse_sidecar(self.sidecar())
        fixture = dict(reference, sdk_version="10", binary_sha256="b" * 64)
        self.assertEqual(ABI.validate_fixture(reference, fixture, 1), (9, 10))
        for changed in (dict(fixture, sdk_version="9"), dict(fixture, runtime_library="1"),
                        dict(fixture, compiler_abi_version="1")):
            with self.assertRaises(ValueError):
                ABI.validate_fixture(reference, changed, 1)


class OrchestrationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="abi-contract-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.prefix = self.root / "installed"
        self.bin = self.prefix / "bin"
        self.bin.mkdir(parents=True)
        self.engine = self.bin / "SparkEngine.exe"
        self.engine.write_bytes(b"not executable: orchestration fixture")
        # Production may anchor this layout, but writable paths are user-rooted.
        for name in ("manifest.json", "spark.modules.json"):
            (self.bin / name).write_text("{}")
        self.evidence = self.root / "evidence"
        self.evidence.mkdir()
        def image(name, version):
            path = self.root / name
            if name == "SparkGameFPS.dll":
                path = self.bin / name
            path.write_bytes(b"inert unit-test data " + name.encode())
            fields = ABI.parse_sidecar(InstalledAbiTests().sidecar())
            fields.update(sdk_version=str(version), binary_sha256=ABI.digest(path))
            Path(str(path) + ".sparkabi").write_text("\n".join(f"{k}={v}" for k, v in fields.items()))
            return path
        image("SparkGameFPS.dll", 9)
        self.newer = image("SparkMismatchedModuleFixture.dll", 10)
        self.previous = image("SparkPreviousSdkModuleFixture.dll", 8)
        self.argv = ["--installed-root", str(self.prefix), "--newer", str(self.newer),
                     "--previous", str(self.previous), "--evidence-root", str(self.evidence),
                     "--source-sha", "a" * 40, "--configuration", "MinSizeRel"]

    def response(self, command, **kwargs):
        module = Path(command[command.index("-game") + 1])
        self.assertEqual(command, [str(self.engine), "-headless", "-game", str(module), "-require-game",
                                   "-test-frames", "8", "-threads", "2", "-no-subprocess"])
        self.assertTrue(module.is_absolute())
        self.assertEqual(kwargs["cwd"], module.parent)
        self.assertFalse(module.parent.is_relative_to(self.prefix))
        env = kwargs["env"]
        self.assertNotIn("SPARK_ENGINE_DIR", env)
        self.assertEqual(env["SPARK_RHI_BACKEND"], "null")
        for key in ("LOCALAPPDATA", "APPDATA", "SPARK_MODULE_ABI_SENTINEL"):
            self.assertTrue(Path(env[key]).is_relative_to(module.parent))
        self.assertFalse(Path(env["SPARK_MODULE_ABI_SENTINEL"]).exists())
        self.assertEqual(kwargs["timeout"], 120)
        declared = 10 if "Mismatched" in module.name else 8
        return subprocess.CompletedProcess(command, 2, InstalledAbiTests().output(str(module), 9, declared), "")

    def run_driver(self, effect):
        with mock.patch.object(ABI.subprocess, "run", side_effect=effect) as runner, \
                mock.patch.object(ABI, "os", SimpleNamespace(name="nt", environ=os.environ)), \
                mock.patch.dict(ABI.os.environ, {"SPARK_ENGINE_DIR": "must-not-reach-child"}), \
                contextlib.redirect_stdout(io.StringIO()):
            result = ABI.main(self.argv)
        reports = list(self.evidence.glob("installed-abi-*/report.json"))
        self.assertEqual(len(reports), 1)
        return result, json.loads(reports[0].read_text()), reports[0].parent, runner

    def test_arguments_cwd_environment_and_immutable_install(self):
        before = {str(p): p.read_bytes() for p in self.prefix.rglob("*") if p.is_file()}
        result, report, _, runner = self.run_driver(self.response)
        self.assertEqual(result, 0)
        self.assertTrue(report["passed"])
        self.assertEqual(runner.call_count, 2)
        self.assertEqual(before, {str(p): p.read_bytes() for p in self.prefix.rglob("*") if p.is_file()})
        self.assertNotEqual(runner.call_args_list[0].kwargs["cwd"], runner.call_args_list[1].kwargs["cwd"])

    def test_timeout_fails_and_retains_partial_output(self):
        def timeout(command, **kwargs):
            self.response(command, **kwargs)
            raise subprocess.TimeoutExpired(command, 120, output=b"partial stdout", stderr=b"partial stderr")
        result, report, root, runner = self.run_driver(timeout)
        self.assertEqual(result, 1)
        self.assertFalse(report["passed"])
        self.assertIn("timed out", report["error"])
        self.assertEqual(runner.call_count, 1)
        self.assertEqual((root / "newer/stdout.log").read_text(), "partial stdout")
        self.assertEqual((root / "newer/stderr.log").read_text(), "partial stderr")

    def test_original_input_mutation_fails(self):
        def mutate(command, **kwargs):
            response = self.response(command, **kwargs)
            self.engine.write_bytes(b"changed inert host fixture")
            return response
        result, report, _, _ = self.run_driver(mutate)
        self.assertEqual(result, 1)
        self.assertIn("input changed", report["error"])

    def test_copied_fixture_mutation_fails(self):
        def mutate(command, **kwargs):
            response = self.response(command, **kwargs)
            Path(command[command.index("-game") + 1]).write_bytes(b"changed inert module fixture")
            return response
        result, report, _, _ = self.run_driver(mutate)
        self.assertEqual(result, 1)
        self.assertTrue(all("changed" in case["error"] for case in report["cases"]))


if __name__ == "__main__":
    unittest.main()
