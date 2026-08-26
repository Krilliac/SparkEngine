import contextlib
import importlib.util
import io
import json
import os
import shutil
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


class SparkNewTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.engine = self.root / "Engine"
        self.output = self.root / "Projects"
        self.output.mkdir()
        template = self.engine / "Templates" / "MMOStarter"
        write_json(
            template / "MMOStarter.sparkproject",
            {"name": "MMOStarter", "defaultScene": "Scenes/Frontier.sparkscene"},
        )
        write_json(
            template / "spark.modules.json",
            {"modules": [{"name": "MMOStarter", "path": "MMOStarter.dll"}]},
        )
        (template / "Source").mkdir()
        (template / "Source" / "GameModule.h").write_text(
            "class MMOStarterModule {};\n", encoding="utf-8"
        )

    def tearDown(self):
        self.temp.cleanup()

    def test_new_rewrites_project_descriptor_identity_before_renaming(self):
        args = SimpleNamespace(name="FrontierGame", template="MMOStarter", output=str(self.output))
        with mock.patch.object(spark_cli, "find_engine_root", return_value=self.engine):
            result = spark_cli.cmd_new(args)

        project = self.output / "FrontierGame"
        descriptor = json.loads((project / "FrontierGame.sparkproject").read_text(encoding="utf-8"))
        manifest = json.loads((project / "spark.modules.json").read_text(encoding="utf-8"))
        self.assertEqual(result, 0)
        self.assertFalse((project / "MMOStarter.sparkproject").exists())
        self.assertEqual(descriptor["name"], "FrontierGame")
        self.assertEqual(manifest["modules"][0]["name"], "FrontierGame")
        self.assertIn("FrontierGameModule", (project / "Source" / "GameModule.h").read_text(encoding="utf-8"))


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
        if spark_cli.current_platform() != "windows":
            host.chmod(0o755)
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

    def test_run_uses_explicit_manifest_for_all_windows_modules(self):
        touch(self.root / "CMakeLists.txt")
        module_names = ["Example.dll", "ExampleAddon.dll"]
        write_json(self.root / "spark.modules.json", {
            "modules": [
                {"name": "Example", "path": module_names[0]},
                {"name": "ExampleAddon", "path": f"plugins/{module_names[1]}"},
            ],
        })
        build_dir = self.root / "build" / "Debug"
        touch(build_dir / "CMakeCache.txt")
        for module_name in module_names:
            module = build_dir / "Debug" / module_name
            touch(module)
            touch(Path(str(module) + ".sparkabi"))
        host = self.root / "engine" / "SparkGame.exe"
        touch(host)
        captured = {}

        def capture_run(command, cwd):
            captured["command"] = command
            captured["cwd"] = cwd
            captured["manifest"] = json.loads(Path(command[2]).read_text(encoding="utf-8"))
            return subprocess_result(0)

        with working_directory(self.root), mock.patch.object(
            spark_cli, "current_platform", return_value="windows"
        ), mock.patch.object(
            spark_cli, "cmd_build", return_value=0
        ) as build, mock.patch.object(
            spark_cli, "find_runtime_host", return_value=host.resolve()
        ), mock.patch.object(spark_cli.subprocess, "run", side_effect=capture_run):
            result = spark_cli.cmd_run(self.args(no_build=False, runtime_args=["--", "-test-frames", "2"]))

        self.assertEqual(result, 0)
        build.assert_called_once()
        self.assertEqual(captured["command"][0], str(host.resolve()))
        self.assertEqual(captured["command"][1], "-manifest")
        self.assertEqual(captured["command"][3:], [
            "-project", str((self.root / "Example.sparkproject").resolve()), "-test-frames", "2"
        ])
        self.assertEqual(captured["cwd"], self.root.resolve())
        self.assertEqual(
            [entry["path"] for entry in captured["manifest"]["modules"]],
            [(build_dir / "Debug" / name).resolve().as_posix() for name in module_names],
        )

    def test_run_rejects_extra_nested_path_metadata(self):
        write_json(self.root / "spark.modules.json", {
            "modules": [{"path": "Example.dll", "metadata": {"path": "unexpected.dll"}}],
        })
        output = io.StringIO()

        with working_directory(self.root), contextlib.redirect_stdout(output), mock.patch.object(
            spark_cli.subprocess, "run"
        ) as run:
            result = spark_cli.cmd_run(self.args())

        self.assertEqual(result, 1)
        self.assertIn("unsafe extra path key", output.getvalue())
        run.assert_not_called()

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
            ["cmake", "--build", str(configured.resolve()), "--config", "Release"],
            cwd=self.root.resolve(),
        )

    def test_run_reports_ambiguous_project_descriptors(self):
        write_json(self.root / "Second.sparkproject", {"name": "Second"})
        output = io.StringIO()
        with working_directory(self.root), contextlib.redirect_stdout(output):
            result = spark_cli.cmd_run(self.args())

        self.assertEqual(result, 1)
        self.assertIn("Multiple project descriptors", output.getvalue())


class SparkPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "Example"
        self.root.mkdir()
        touch(self.root / "CMakeLists.txt")
        write_json(self.root / "Example.sparkproject", {
            "name": "Example",
            "defaultScene": "Scenes/Default.sparkscene",
        })

        self.module_name = "Example.dll" if spark_cli.current_platform() == "windows" else "libExample.so"
        write_json(self.root / "spark.modules.json", {"modules": [{"path": self.module_name}]})
        self.build_dir = self.root / "build" / "Release"
        touch(self.build_dir / "CMakeCache.txt")
        self.module = self.build_dir / "Release" / self.module_name
        touch(self.module)
        touch(Path(str(self.module) + ".sparkabi"))
        touch(self.module.with_suffix(".pdb"))

        touch(self.root / "Assets" / "project.asset")
        touch(self.root / "Scenes" / "Default.sparkscene")
        touch(self.root / "Config" / "Game.ini")

        self.engine_root = Path(self.temp.name) / "Engine"
        self.host = (
            self.engine_root / "build" / "bin" / "Release" / spark_cli.executable_filename()
        )
        touch(self.host)
        if spark_cli.current_platform() != "windows":
            self.host.chmod(0o755)
        touch(self.host.parent / "Shaders" / "Basic.hlsl")
        touch(self.host.parent / "Resources" / "Config" / "Runtime.ini")
        touch(self.host.parent / "Assets" / "Engine" / "Branding" / "splash.wav")

    def tearDown(self):
        self.temp.cleanup()

    def args(self, **overrides):
        values = {
            "config": "Release",
            "output": None,
            "platform": None,
            "strip": False,
            "compress": False,
            "force": False,
        }
        values.update(overrides)
        return SimpleNamespace(**values)

    def run_package(self, args=None):
        output = io.StringIO()
        with working_directory(self.root), contextlib.redirect_stdout(output), mock.patch.object(
            spark_cli, "cmd_build", return_value=0
        ) as build, mock.patch.object(spark_cli, "find_engine_root", return_value=self.engine_root):
            result = spark_cli.cmd_package(args or self.args())
        return result, output.getvalue(), build

    def package_path(self):
        return self.root / "dist" / f"Example-{spark_cli.current_platform()}-release"

    def create_owned_package(self, package=None):
        package = package or self.package_path()
        write_json(package / "manifest.json", {
            "packageOwner": spark_cli.PACKAGE_OWNER,
            "packageFormatVersion": spark_cli.PACKAGE_FORMAT_VERSION,
            "project": "Example",
            "platform": spark_cli.current_platform(),
            "config": "Release",
        })
        write_json(package / "spark.modules.json", {"modules": [{"path": self.module_name}]})
        touch(package / "keep.txt")
        return package

    def test_package_assembles_complete_runnable_contract(self):
        result, output, build = self.run_package(self.args(strip=True, compress=True))

        self.assertEqual(result, 0, output)
        build.assert_called_once()
        package = self.root / "dist" / f"Example-{spark_cli.current_platform()}-release"
        host_name = "SparkGame.exe" if spark_cli.current_platform() == "windows" else "SparkGame"
        preview_name = "SparkGame Scene.exe" if spark_cli.current_platform() == "windows" else "SparkGameScene"

        self.assertTrue((package / host_name).is_file())
        self.assertTrue((package / self.module_name).is_file())
        self.assertTrue(Path(str(package / self.module_name) + ".sparkabi").is_file())
        self.assertTrue((package / "Shaders" / "Basic.hlsl").is_file())
        self.assertTrue((package / "Resources" / "Config" / "Runtime.ini").is_file())
        self.assertTrue((package / "Assets" / "Engine" / "Branding" / "splash.wav").is_file())
        self.assertTrue((package / "Assets" / "project.asset").is_file())
        self.assertTrue((package / "Scenes" / "Default.sparkscene").is_file())
        self.assertTrue((package / "Config" / "Game.ini").is_file())
        self.assertTrue((package / "Startup.sparkscene").is_file())
        self.assertTrue((package / "ScenePreview" / preview_name).is_file())
        self.assertTrue((package / "Example.sparkproject").is_file())
        self.assertFalse((package / "Example.pdb").exists())

        modules, error = spark_cli.read_module_manifest(package)
        self.assertIsNone(error)
        self.assertEqual(modules, [(package / self.module_name).resolve()])
        host, error = spark_cli.find_package_host(package, "Example")
        self.assertIsNone(error)
        self.assertEqual(host, (package / host_name).resolve())

        package_info = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(package_info["packageOwner"], spark_cli.PACKAGE_OWNER)
        self.assertEqual(package_info["packageFormatVersion"], spark_cli.PACKAGE_FORMAT_VERSION)
        self.assertFalse(package_info["stripped"])
        self.assertTrue(package_info["stripRequested"])
        self.assertFalse(package_info["compressed"])
        self.assertTrue(package_info["compressionRequested"])
        launcher_name = "LaunchGame.cmd" if spark_cli.current_platform() == "windows" else "LaunchGame.sh"
        self.assertEqual(package_info["entrypoint"], launcher_name)
        self.assertEqual(package_info["workingDirectory"], ".")
        self.assertEqual(package_info["host"], host_name)
        self.assertEqual(package_info["modules"], [self.module_name])
        self.assertEqual(package_info["startupScene"], "Startup.sparkscene")
        self.assertIn("module bytes remain unchanged", output)
        self.assertIn("keep raw assets", output)

    def test_package_failure_preserves_previous_package(self):
        package = self.create_owned_package()
        shutil.rmtree(self.host.parent / "Shaders")

        result, output, _ = self.run_package()

        self.assertEqual(result, 1)
        self.assertIn("Shaders directory is missing", output)
        self.assertTrue((package / "keep.txt").is_file())
        self.assertFalse(any(package.parent.glob(f".{package.name}.transaction-*")))

    def test_package_rejects_output_inside_project_content_before_build(self):
        output = io.StringIO()
        with working_directory(self.root), contextlib.redirect_stdout(output), mock.patch.object(
            spark_cli, "cmd_build"
        ) as build:
            result = spark_cli.cmd_package(self.args(output="Assets"))

        self.assertEqual(result, 1)
        self.assertIn("cannot replace or contain the project root", output.getvalue())
        build.assert_not_called()

    def test_package_rejects_output_whose_final_directory_contains_project(self):
        package_name = f"Example-{spark_cli.current_platform()}-release"
        package_container = self.root.parent / package_name
        self.root.rename(package_container)
        project = package_container / "SourceProject"
        project.mkdir()
        for child in list(package_container.iterdir()):
            if child != project:
                child.replace(project / child.name)
        self.root = project
        output_root = self.root.parent.parent

        output = io.StringIO()
        with working_directory(self.root), contextlib.redirect_stdout(output), mock.patch.object(
            spark_cli, "cmd_build"
        ) as build:
            result = spark_cli.cmd_package(self.args(output=str(output_root)))

        self.assertEqual(result, 1)
        self.assertIn("cannot replace or contain the project root", output.getvalue())
        self.assertTrue((self.root / "Example.sparkproject").is_file())
        build.assert_not_called()

    def test_package_preserves_unrelated_predictable_transient_directories(self):
        package = self.root / "dist" / f"Example-{spark_cli.current_platform()}-release"
        legacy_staging = package.parent / f".{package.name}.staging"
        legacy_backup = package.parent / f".{package.name}.backup"
        touch(legacy_staging / "owned-by-user.txt")
        touch(legacy_backup / "recovery.txt")

        result, output, _ = self.run_package()

        self.assertEqual(result, 0, output)
        self.assertTrue((legacy_staging / "owned-by-user.txt").is_file())
        self.assertTrue((legacy_backup / "recovery.txt").is_file())

    def test_package_rejects_output_inside_active_build_tree_before_build(self):
        output = io.StringIO()
        with working_directory(self.root), contextlib.redirect_stdout(output), mock.patch.object(
            spark_cli, "cmd_build"
        ) as build:
            result = spark_cli.cmd_package(self.args(output=str(self.build_dir / "packages")))

        self.assertEqual(result, 1)
        self.assertIn("overlap the active build tree", output.getvalue())
        build.assert_not_called()

    def test_package_rejects_output_inside_runtime_source_before_build(self):
        result, output, build = self.run_package(self.args(output=str(self.host.parent)))

        self.assertEqual(result, 1)
        self.assertIn("overlap the runtime host source directory", output)
        build.assert_not_called()

    def test_package_preserves_multi_module_manifest_metadata(self):
        addon_name = "ExampleAddon.dll" if spark_cli.current_platform() == "windows" else "libExampleAddon.so"
        addon = self.module.parent / addon_name
        touch(addon)
        touch(Path(str(addon) + ".sparkabi"))
        write_json(self.root / "spark.modules.json", {
            "schemaVersion": 7,
            "packagePolicy": "preserve-me",
            "modules": [
                {
                    "name": "ExampleGame",
                    "path": self.module_name,
                    "loadOrder": 1000,
                    "kind": "game",
                },
                {
                    "name": "ExampleAddon",
                    "path": f"plugins/{addon_name}",
                    "loadOrder": 1200,
                    "kind": "addon",
                    "optional": True,
                },
            ],
        })

        result, output, _ = self.run_package()

        self.assertEqual(result, 0, output)
        package = self.root / "dist" / f"Example-{spark_cli.current_platform()}-release"
        packaged_manifest = json.loads((package / "spark.modules.json").read_text(encoding="utf-8"))
        self.assertEqual(packaged_manifest["schemaVersion"], 7)
        self.assertEqual(packaged_manifest["packagePolicy"], "preserve-me")
        self.assertEqual(packaged_manifest["modules"][0], {
            "name": "ExampleGame",
            "path": self.module_name,
            "loadOrder": 1000,
            "kind": "game",
        })
        self.assertEqual(packaged_manifest["modules"][1], {
            "name": "ExampleAddon",
            "path": addon_name,
            "loadOrder": 1200,
            "kind": "addon",
            "optional": True,
        })
        modules, error = spark_cli.read_module_manifest(package)
        self.assertIsNone(error)
        self.assertEqual(modules, [
            (package / self.module_name).resolve(),
            (package / addon_name).resolve(),
        ])
        package_info = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(package_info["modules"], [self.module_name, addon_name])
        self.assertEqual(package_info["binaries"], 4)

    def test_package_publish_failure_restores_previous_package(self):
        package = self.create_owned_package()
        real_replace = Path.replace

        def fail_staging_replace(source, target):
            if source.name == "staging":
                raise OSError("injected publish failure")
            return real_replace(source, target)

        with mock.patch.object(Path, "replace", new=fail_staging_replace):
            result, output, _ = self.run_package()

        self.assertEqual(result, 1)
        self.assertIn("previous package was restored", output)
        self.assertTrue((package / "keep.txt").is_file())
        self.assertFalse(any(package.parent.glob(f".{package.name}.transaction-*")))

    def test_package_preserves_recovery_data_when_restore_fails(self):
        package = self.create_owned_package()
        real_replace = Path.replace

        def fail_publish_and_restore(source, target):
            if source.name in {"staging", "previous-package"}:
                raise OSError(f"injected {source.name} failure")
            return real_replace(source, target)

        with mock.patch.object(Path, "replace", new=fail_publish_and_restore):
            result, output, _ = self.run_package()

        self.assertEqual(result, 1)
        self.assertIn("recovery data was preserved", output)
        transactions = list(package.parent.glob(f".{package.name}.transaction-*"))
        self.assertEqual(len(transactions), 1)
        self.assertTrue((transactions[0] / "previous-package" / "keep.txt").is_file())
        self.assertFalse(package.exists())

    def test_package_rejects_unknown_existing_target_without_force(self):
        package = self.package_path()
        touch(package / "unrelated.txt")

        result, output, build = self.run_package()

        self.assertEqual(result, 1)
        self.assertIn("not an owned Spark CLI package", output)
        self.assertTrue((package / "unrelated.txt").is_file())
        build.assert_not_called()

    def test_package_force_replaces_unknown_non_linked_target(self):
        package = self.package_path()
        touch(package / "unrelated.txt")

        result, output, _ = self.run_package(self.args(force=True))

        self.assertEqual(result, 0, output)
        self.assertFalse((package / "unrelated.txt").exists())
        package_info = json.loads((package / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(package_info["packageOwner"], spark_cli.PACKAGE_OWNER)

    def test_package_replaces_owned_prior_package_without_force(self):
        package = self.create_owned_package()

        result, output, _ = self.run_package()

        self.assertEqual(result, 0, output)
        self.assertFalse((package / "keep.txt").exists())
        self.assertTrue((package / "LaunchGame.cmd").exists() or (package / "LaunchGame.sh").exists())

    def test_package_never_replaces_link_target_even_with_force(self):
        package = self.package_path()
        target = self.root / "unrelated-target"
        touch(target / "keep.txt")
        package.parent.mkdir(parents=True, exist_ok=True)
        try:
            package.symlink_to(target, target_is_directory=True)
        except OSError as exc:
            self.skipTest(f"directory symlinks are unavailable: {exc}")

        result, output, build = self.run_package(self.args(force=True))

        self.assertEqual(result, 1)
        self.assertIn("will never be replaced", output)
        self.assertTrue((target / "keep.txt").is_file())
        self.assertTrue(package.is_symlink())
        build.assert_not_called()

    def test_package_rejects_link_like_target_before_resolution(self):
        package = self.package_path()
        touch(package / "keep.txt")
        real_link_check = spark_cli._path_is_link_like
        canonical_package = spark_cli._absolute_unresolved(package)

        def report_final_as_link(path):
            return (spark_cli._absolute_unresolved(path) == canonical_package or
                    real_link_check(path))

        with mock.patch.object(spark_cli, "_path_is_link_like", side_effect=report_final_as_link):
            result, output, build = self.run_package(self.args(force=True))

        self.assertEqual(result, 1)
        self.assertIn("will never be replaced", output)
        self.assertTrue((package / "keep.txt").is_file())
        build.assert_not_called()

    def test_copy_tree_rejects_link_like_source_root(self):
        source = self.root / "Assets"
        destination = self.root / "staged" / "Assets"
        touch(source / "safe.txt")
        real_link_check = spark_cli._path_is_link_like

        with mock.patch.object(
            spark_cli,
            "_path_is_link_like",
            side_effect=lambda path: Path(path) == source or real_link_check(path),
        ):
            count, error = spark_cli._copy_tree(source, destination, self.root)

        self.assertEqual(count, 0)
        self.assertIn("linked or escaping path", error)
        self.assertFalse(destination.exists())

    def test_copy_tree_rejects_link_like_descendant(self):
        source = self.root / "Assets"
        linked = source / "Linked"
        destination = self.root / "staged" / "Assets"
        touch(linked / "unsafe.txt")
        real_link_check = spark_cli._path_is_link_like

        with mock.patch.object(
            spark_cli,
            "_path_is_link_like",
            side_effect=lambda path: Path(path) == linked or real_link_check(path),
        ):
            count, error = spark_cli._copy_tree(source, destination, self.root)

        self.assertEqual(count, 0)
        self.assertIn("linked or escaping path", error)
        self.assertFalse(destination.exists())

    def test_package_recovers_exactly_one_validated_owned_backup(self):
        package = self.package_path()
        transaction = package.parent / f".{package.name}.transaction-interrupted"
        self.create_owned_package(transaction / "previous-package")
        marker_error = spark_cli._write_transaction_marker(
            transaction, package, "Example", spark_cli.current_platform(), "Release"
        )
        self.assertIsNone(marker_error)
        shutil.rmtree(self.host.parent / "Shaders")

        result, output, _ = self.run_package()

        self.assertEqual(result, 1)
        self.assertIn("Recovered interrupted package publication", output)
        self.assertIn("Shaders directory is missing", output)
        self.assertTrue((package / "keep.txt").is_file())
        self.assertFalse(transaction.exists())

    def test_package_preserves_ambiguous_recovery_directories(self):
        package = self.package_path()
        transactions = []
        for suffix in ("one", "two"):
            transaction = package.parent / f".{package.name}.transaction-{suffix}"
            self.create_owned_package(transaction / "previous-package")
            self.assertIsNone(spark_cli._write_transaction_marker(
                transaction, package, "Example", spark_cli.current_platform(), "Release"
            ))
            transactions.append(transaction)

        result, output, build = self.run_package()

        self.assertEqual(result, 1)
        self.assertIn("2 possible recovery directories", output)
        self.assertTrue(all(transaction.is_dir() for transaction in transactions))
        build.assert_not_called()

    def test_package_preserves_stale_recovery_directory(self):
        package = self.package_path()
        transaction = package.parent / f".{package.name}.transaction-stale"
        self.create_owned_package(transaction / "previous-package")
        self.assertIsNone(spark_cli._write_transaction_marker(
            transaction, package, "DifferentProject", spark_cli.current_platform(), "Release"
        ))

        result, output, build = self.run_package()

        self.assertEqual(result, 1)
        self.assertIn("Stale or foreign", output)
        self.assertTrue(transaction.is_dir())
        build.assert_not_called()

    def test_package_rejects_nested_manifest_path_metadata_before_build(self):
        write_json(self.root / "spark.modules.json", {
            "path": "unexpected-root-module.dll",
            "modules": [{"path": self.module_name}],
        })

        result, output, build = self.run_package()

        self.assertEqual(result, 1)
        self.assertIn("unsafe path key at root.path", output)
        build.assert_not_called()

    def test_default_scene_accepts_windows_separators(self):
        scene, error = spark_cli._find_startup_scene(
            self.root, {"defaultScene": "Scenes\\Default.sparkscene"}
        )

        self.assertIsNone(error)
        self.assertEqual(scene, (self.root / "Scenes" / "Default.sparkscene").resolve())

    def test_posix_runtime_host_must_be_executable(self):
        posix_host = self.engine_root / "build" / "bin" / "Release" / "SparkGame"
        touch(posix_host)

        with mock.patch.object(
            spark_cli, "current_platform", return_value="linux"
        ), mock.patch.object(spark_cli.os, "access", return_value=False), mock.patch.dict(
            spark_cli.os.environ, {"SPARKENGINE_RUNTIME_HOST": ""}
        ):
            found = spark_cli.find_runtime_host(self.engine_root, "Release")

        self.assertIsNone(found)

    def test_posix_runtime_host_accepts_executable(self):
        posix_host = self.engine_root / "build" / "bin" / "Release" / "SparkEngine"
        touch(posix_host)
        posix_host.chmod(0o755)

        with mock.patch.object(
            spark_cli, "current_platform", return_value="linux"
        ), mock.patch.dict(spark_cli.os.environ, {"SPARKENGINE_RUNTIME_HOST": ""}):
            found = spark_cli.find_runtime_host(self.engine_root, "Release")

        self.assertEqual(found, posix_host.resolve())

    def test_package_refreshes_configured_source_tree_runtime_host(self):
        touch(self.engine_root / "CMakeLists.txt")
        touch(self.engine_root / "build" / "CMakeCache.txt")

        with mock.patch.object(spark_cli.subprocess, "run", return_value=subprocess_result(0)) as run:
            result, output, _ = self.run_package()

        self.assertEqual(result, 0, output)
        run.assert_called_once_with(
            [
                "cmake", "--build", str(self.engine_root / "build"),
                "--config", "Release", "--target", "SparkEngine",
            ],
            cwd=self.engine_root,
        )
        self.assertIn("Refreshing SparkEngine runtime host", output)

    def test_package_stops_when_runtime_host_refresh_fails(self):
        touch(self.engine_root / "build" / "CMakeCache.txt")
        output = io.StringIO()
        with working_directory(self.root), contextlib.redirect_stdout(output), mock.patch.object(
            spark_cli, "cmd_build"
        ) as project_build, mock.patch.object(
            spark_cli, "find_engine_root", return_value=self.engine_root
        ), mock.patch.object(
            spark_cli.subprocess, "run", return_value=subprocess_result(7)
        ):
            result = spark_cli.cmd_package(self.args())

        self.assertEqual(result, 7)
        self.assertIn("runtime host build failed", output.getvalue())
        project_build.assert_not_called()

    def test_package_rejects_ambiguous_built_modules(self):
        duplicate = self.build_dir / "Release" / "stale" / self.module_name
        touch(duplicate)
        touch(Path(str(duplicate) + ".sparkabi"))

        result, output, _ = self.run_package()

        self.assertEqual(result, 1)
        self.assertIn("Multiple matching built modules", output)
        self.assertFalse((self.root / "dist").exists())


class SparkShippedTemplatePackageTests(unittest.TestCase):
    """Exercise the real template metadata/content through package and run."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.engine_source = CLI_PATH.parents[2]
        self.engine_runtime = self.root / "EngineRuntime"
        self.host = (
            self.engine_runtime / "build" / "bin" / "Release" /
            spark_cli.executable_filename()
        )
        touch(self.host)
        if spark_cli.current_platform() != "windows":
            self.host.chmod(0o755)
        touch(self.host.parent / "Shaders" / "Basic.hlsl")

    def tearDown(self):
        self.temp.cleanup()

    def test_every_shipped_template_assembles_and_launches_as_a_package(self):
        template_sources = sorted(
            path.parent
            for path in (self.engine_source / "Templates").glob("*/*.sparkproject")
        )
        self.assertEqual(
            [path.name for path in template_sources],
            [
                "Blank3D", "EmptyProject", "FPSStarter", "MMOStarter",
                "MultiplayerArena", "PlatformerKit", "RPGStarter",
                "ThirdPersonStarter", "TopDownStarter",
            ],
        )

        for template_source in template_sources:
            with self.subTest(template=template_source.name):
                project_root = self.root / "Projects" / template_source.name
                shutil.copytree(
                    template_source,
                    project_root,
                    ignore=shutil.ignore_patterns("build", "dist"),
                )
                descriptor_path = next(project_root.glob("*.sparkproject"))
                descriptor = json.loads(descriptor_path.read_text(encoding="utf-8"))
                source_manifest = json.loads(
                    (project_root / "spark.modules.json").read_text(encoding="utf-8")
                )
                declared_module = Path(source_manifest["modules"][0]["path"])
                module_stem = declared_module.stem
                module_name = (
                    f"{module_stem}.dll" if spark_cli.current_platform() == "windows"
                    else f"lib{module_stem}.so"
                )
                build_dir = project_root / "build" / "Release"
                touch(build_dir / "CMakeCache.txt")
                module = build_dir / "Release" / module_name
                touch(module)
                touch(Path(str(module) + ".sparkabi"))

                package_output = self.root / "Packages" / template_source.name
                package_args = SimpleNamespace(
                    config="Release",
                    output=str(package_output),
                    platform=None,
                    strip=False,
                    compress=False,
                    force=False,
                )
                package_log = io.StringIO()
                with working_directory(project_root), contextlib.redirect_stdout(
                    package_log
                ), mock.patch.object(
                    spark_cli, "cmd_build", return_value=0
                ), mock.patch.object(
                    spark_cli, "find_engine_root", return_value=self.engine_runtime
                ):
                    package_result = spark_cli.cmd_package(package_args)

                self.assertEqual(package_result, 0, package_log.getvalue())
                package_root = package_output / (
                    f"{descriptor['name']}-{spark_cli.current_platform()}-release"
                )
                package_manifest = json.loads(
                    (package_root / "manifest.json").read_text(encoding="utf-8")
                )
                startup_scene = descriptor["defaultScene"].replace("\\", "/")
                self.assertEqual(package_manifest["startupScene"], "Startup.sparkscene")
                self.assertEqual(
                    (package_root / "Startup.sparkscene").read_bytes(),
                    (project_root / startup_scene).read_bytes(),
                )
                for content_directory in ("Assets", "Scenes", "Config"):
                    source_files = sorted(
                        path.relative_to(project_root / content_directory)
                        for path in (project_root / content_directory).rglob("*")
                        if path.is_file()
                    )
                    packaged_files = sorted(
                        path.relative_to(package_root / content_directory)
                        for path in (package_root / content_directory).rglob("*")
                        if path.is_file()
                    )
                    self.assertEqual(packaged_files, source_files)

                run_args = SimpleNamespace(
                    config="Release",
                    no_build=True,
                    package=package_root,
                    runtime_args=["--", "-headless", "-test-frames", "1"],
                )
                with working_directory(project_root), mock.patch.object(
                    spark_cli.subprocess, "run", return_value=subprocess_result(0)
                ) as run:
                    run_result = spark_cli.cmd_run(run_args)
                self.assertEqual(run_result, 0)
                run.assert_called_once_with(
                    [str((package_root / package_manifest["host"]).resolve()),
                     "-headless", "-test-frames", "1"],
                    cwd=package_root.resolve(),
                )


def subprocess_result(returncode):
    return SimpleNamespace(returncode=returncode)


if __name__ == "__main__":
    unittest.main()
