import contextlib
import importlib.util
import io
import json
import os
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


CLI_PATH = Path(__file__).resolve().parents[1] / "spark_cli.py"
SPEC = importlib.util.spec_from_file_location("spark_cli", CLI_PATH)
spark_cli = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(spark_cli)


@contextlib.contextmanager
def working_directory(path):
    previous = Path.cwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(previous)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def touch(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"test")


class SparkRunTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        write_json(self.root / "Example.sparkproject", {"name": "Example"})

    def tearDown(self):
        self.temp.cleanup()

    def args(self, **overrides):
        values = {
            "config": "Debug",
            "no_build": True,
            "package": None,
            "runtime_args": [],
        }
        values.update(overrides)
        return SimpleNamespace(**values)

    def create_package(self):
        package = self.root / "build" / "Output"
        module_name = "Example.dll" if spark_cli.current_platform() == "windows" else "libExample.so"
        host_name = "SparkGame.exe" if spark_cli.current_platform() == "windows" else "SparkGame"
        write_json(package / "spark.modules.json", {"modules": [{"path": module_name}]})
        touch(package / module_name)
        touch(Path(str(package / module_name) + ".sparkabi"))
        touch(package / host_name)
        if spark_cli.current_platform() != "windows":
            (package / host_name).chmod(0o755)
        return package, package / host_name

    def test_run_launches_editor_assembled_package_and_forwards_arguments(self):
        package, host = self.create_package()
        completed = subprocess_result(17)

        with working_directory(self.root), mock.patch.object(
            spark_cli.subprocess, "run", return_value=completed
        ) as run:
            result = spark_cli.cmd_run(self.args(runtime_args=["--", "-no-splash", "-test-frames", "2"]))

        self.assertEqual(result, 17)
        run.assert_called_once_with(
            [str(host.resolve()), "-no-splash", "-test-frames", "2"],
            cwd=package.resolve(),
        )

    def test_run_rejects_manifest_path_that_escapes_explicit_package(self):
        package = self.root / "unsafe-package"
        write_json(package / "spark.modules.json", {"modules": [{"path": "../Example.dll"}]})
        touch(self.root / "Example.dll")
        touch(Path(str(self.root / "Example.dll") + ".sparkabi"))

        output = io.StringIO()
        with working_directory(self.root), contextlib.redirect_stdout(output), mock.patch.object(
            spark_cli.subprocess, "run"
        ) as run:
            result = spark_cli.cmd_run(self.args(package=package))

        self.assertEqual(result, 1)
        self.assertIn("path escapes the package directory", output.getvalue())
        run.assert_not_called()

    def test_run_falls_back_to_engine_host_and_built_module(self):
        module_name = "Example.dll" if spark_cli.current_platform() == "windows" else "libExample.so"
        write_json(self.root / "spark.modules.json", {"modules": [{"path": module_name}]})
        build_dir = self.root / "build" / "Debug"
        touch(build_dir / "CMakeCache.txt")
        module = build_dir / "Debug" / module_name
        touch(module)
        touch(Path(str(module) + ".sparkabi"))

        engine_root = self.root / "engine"
        host = engine_root / "build" / "bin" / "Debug" / spark_cli.executable_filename()
        touch(host)
        completed = subprocess_result(0)

        with working_directory(self.root), mock.patch.object(
            spark_cli, "find_engine_root", return_value=engine_root
        ), mock.patch.object(spark_cli.subprocess, "run", return_value=completed) as run:
            result = spark_cli.cmd_run(self.args(runtime_args=["-no-splash"]))

        self.assertEqual(result, 0)
        run.assert_called_once_with(
            [
                str(host.resolve()),
                "-game", str(module.resolve()),
                "-project", str((self.root / "Example.sparkproject").resolve()),
                "-no-splash",
            ],
            cwd=self.root.resolve(),
        )

    def test_build_uses_editor_configuration_tree(self):
        touch(self.root / "CMakeLists.txt")
        configured = self.root / "build" / "Release"
        touch(configured / "CMakeCache.txt")
        completed = subprocess_result(0)

        with working_directory(self.root), mock.patch.object(
            spark_cli.subprocess, "run", return_value=completed
        ) as run:
            result = spark_cli.cmd_build(SimpleNamespace(config="Release"))

        self.assertEqual(result, 0)
        run.assert_called_once_with(
            ["cmake", "--build", str(configured), "--config", "Release"],
            cwd=self.root.resolve(),
        )

    def test_run_reports_ambiguous_project_descriptors(self):
        write_json(self.root / "Second.sparkproject", {"name": "Second"})
        output = io.StringIO()
        with working_directory(self.root), contextlib.redirect_stdout(output):
            result = spark_cli.cmd_run(self.args())

        self.assertEqual(result, 1)
        self.assertIn("Multiple project descriptors", output.getvalue())


def subprocess_result(returncode):
    return SimpleNamespace(returncode=returncode)


if __name__ == "__main__":
    unittest.main()
