"""ASSET-220 regressions: `spark validate` and `spark migrate` must not claim
work they did not do.

`validate` has to inspect the scene format the editor actually writes
(.sparkscene with `*Path` fields), refuse references that escape the project,
and fail when nothing was inspected. `migrate` must audit the real
AssetFileHeader layout (Core/AssetMigration.h, memcpy'd little-endian, so the
magic 0x5350524B is "KRPS" on disk) and never report a migration it did not
perform, because no migration steps ship.
"""

import contextlib
import importlib.util
import io
import json
import os
import struct
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

CLI_PATH = Path(__file__).resolve().parents[1] / "spark_cli.py"
REPO_ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("spark_cli_asset220", CLI_PATH)
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


def mesh_scene(*mesh_paths, material_path=""):
    entities = []
    for index, mesh in enumerate(mesh_paths):
        entities.append({
            "id": index,
            "name": f"Entity{index}",
            "parent": -1,
            "components": [
                {"type": "Transform", "fields": {"position": "0,0,0"}},
                {"type": "MeshRenderer", "fields": {
                    "meshPath": mesh,
                    "materialPath": material_path,
                    "visible": "true",
                }},
            ],
        })
    return {"entities": entities}


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def touch(path, data=b"x"):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


class ValidateAsset220Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.outer = Path(self.temp.name)
        self.root = self.outer / "Game"
        self.root.mkdir()
        (self.root / "Game.sparkproject").write_text("{}", encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def run_validate(self, path=None, fmt="text", strict=False):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = spark_cli.cmd_validate(SimpleNamespace(
                path=str(path if path is not None else self.root), strict=strict, format=fmt))
        return result, output.getvalue()

    def test_missing_sparkscene_mesh_path_fails(self):
        write_json(self.root / "Scenes" / "Arena.sparkscene",
                   mesh_scene("Assets/Models/missing_wall.obj"))

        result, output = self.run_validate()

        self.assertEqual(result, 1, output)
        self.assertIn("Assets/Models/missing_wall.obj", output)

    def test_missing_sparkscene_material_path_fails(self):
        touch(self.root / "Assets" / "Models" / "wall.obj")
        write_json(self.root / "Scenes" / "Arena.sparkscene",
                   mesh_scene("Assets/Models/wall.obj", material_path="Assets/Materials/gone.material"))

        result, output = self.run_validate()

        self.assertEqual(result, 1, output)
        self.assertIn("Assets/Materials/gone.material", output)

    def test_valid_scene_passes_and_counts_only_inspected_files(self):
        touch(self.root / "Assets" / "Models" / "wall.obj")
        touch(self.root / "Assets" / "Textures" / "unused.png")
        touch(self.root / "Assets" / "Audio" / "unused.wav")
        touch(self.root / "Shaders" / "unused.hlsl")
        write_json(self.root / "Scenes" / "Arena.sparkscene",
                   mesh_scene("Assets/Models/wall.obj", "__spark_primitive_ground__.obj",
                              "__spark_primitive_Sphere.obj"))

        result, output = self.run_validate(fmt="json")

        self.assertEqual(result, 0, output)
        report = json.loads(output[output.index("{"):])
        self.assertEqual(report["totalChecked"], 1)
        self.assertEqual(report["errors"], [])
        self.assertTrue(report["passed"])

    def test_reference_escaping_project_fails_even_if_file_exists(self):
        touch(self.outer / "outside.obj")
        write_json(self.root / "Scenes" / "Arena.sparkscene", mesh_scene("../outside.obj"))

        result, output = self.run_validate()

        self.assertEqual(result, 1, output)
        self.assertIn("escapes the project", output)

    def test_absolute_reference_fails(self):
        absolute = self.root / "Assets" / "Models" / "wall.obj"
        touch(absolute)
        write_json(self.root / "Scenes" / "Arena.sparkscene", mesh_scene(str(absolute.resolve())))

        result, output = self.run_validate()

        self.assertEqual(result, 1, output)
        self.assertIn("must be project-relative", output)

    def test_primitive_prefix_only_exempts_meshes(self):
        write_json(self.root / "Scenes" / "Arena.sparkscene",
                   mesh_scene("__spark_primitive_Cube.obj", material_path="__spark_primitive_fake.material"))

        result, output = self.run_validate()

        self.assertEqual(result, 1, output)
        self.assertIn("__spark_primitive_fake.material", output)

    def test_nothing_inspected_fails(self):
        touch(self.root / "Assets" / "Textures" / "a.png")
        touch(self.root / "Assets" / "Audio" / "b.wav")
        touch(self.root / "Shaders" / "c.hlsl")

        result, output = self.run_validate()

        self.assertEqual(result, 1, output)
        self.assertIn("No scene or material files", output)

    def test_subdirectory_resolves_against_project_root(self):
        touch(self.root / "Assets" / "Models" / "wall.obj")
        write_json(self.root / "Scenes" / "Arena.sparkscene", mesh_scene("Assets/Models/wall.obj"))

        result, output = self.run_validate(path=self.root / "Scenes")

        self.assertEqual(result, 0, output)

    def test_legacy_scene_missing_mesh_is_an_error(self):
        write_json(self.root / "Level.scene",
                   {"entities": [{"components": [{"mesh": "Assets/Models/nope.obj"}]}]})

        result, output = self.run_validate()

        self.assertEqual(result, 1, output)
        self.assertIn("Assets/Models/nope.obj", output)

    def test_unparseable_material_fails(self):
        touch(self.root / "Assets" / "Materials" / "Broken.material", b"{not json")

        result, output = self.run_validate()

        self.assertEqual(result, 1, output)
        self.assertIn("Broken.material", output)

    def test_shipped_templates_validate_clean(self):
        templates = sorted(path for path in (REPO_ROOT / "Templates").iterdir()
                           if path.is_dir() and any(path.glob("*.sparkproject")))
        self.assertGreater(len(templates), 0)
        for template in templates:
            with self.subTest(template=template.name):
                result, output = self.run_validate(path=template)
                self.assertEqual(result, 0, output)


def asset_header(version=(1, 0, 0), asset_type=1, header_size=32, data_size=None,
                 payload=b"payload!", magic=0x5350524B):
    if data_size is None:
        data_size = len(payload)
    # uint32 magic, 3x uint16 version, uint8 type, pad, uint32 crc,
    # uint32 headerSize, pad to 8, uint64 dataSize == sizeof(AssetFileHeader) 32
    header = struct.pack("<IHHHBxIIxxxxQ", magic, *version, asset_type, 0, header_size, data_size)
    assert len(header) == 32
    return header + payload


class MigrateAsset220Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def run_migrate(self, dry_run=False, backup=True):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            result = spark_cli.cmd_migrate(SimpleNamespace(path=str(self.root), dry_run=dry_run, backup=backup))
        return result, output.getvalue()

    def snapshot(self):
        return {path.relative_to(self.root).as_posix(): path.read_bytes()
                for path in sorted(self.root.rglob("*")) if path.is_file()}

    def assert_untouched(self, before):
        self.assertEqual(self.snapshot(), before)

    def test_header_magic_on_disk_is_krps(self):
        self.assertEqual(asset_header()[:4], b"KRPS")

    def test_old_version_fails_without_claiming_or_writing(self):
        touch(self.root / "Level.scene", asset_header(version=(0, 9, 0)))
        before = self.snapshot()

        result, output = self.run_migrate()

        self.assertEqual(result, 1, output)
        self.assertNotIn("Migrated to", output)
        self.assertIn("no migration steps", output)
        self.assert_untouched(before)

    def test_current_version_passes_read_only(self):
        touch(self.root / "Level.scene", asset_header())
        touch(self.root / "Mat.material", asset_header(asset_type=2))
        before = self.snapshot()

        result, output = self.run_migrate()

        self.assertEqual(result, 0, output)
        self.assertIn("v1.0.0", output)
        self.assert_untouched(before)

    def test_save_magic_is_not_treated_as_asset_header(self):
        touch(self.root / "Slot1.save", b"SPRK" + b"\x00" * 16)
        before = self.snapshot()

        result, output = self.run_migrate()

        self.assertNotIn("Migrated to", output)
        self.assertEqual(result, 0, output)
        self.assert_untouched(before)

    def test_truncated_header_fails(self):
        touch(self.root / "Level.scene", asset_header()[:20])

        result, output = self.run_migrate()

        self.assertEqual(result, 1, output)
        self.assertIn("truncated", output)

    def test_over_claiming_data_size_fails(self):
        touch(self.root / "Level.scene", asset_header(data_size=4096))

        result, output = self.run_migrate()

        self.assertEqual(result, 1, output)
        self.assertIn("claims", output)

    def test_header_size_past_end_fails(self):
        touch(self.root / "Level.scene", asset_header(header_size=4096, data_size=0))

        result, output = self.run_migrate()

        self.assertEqual(result, 1, output)

    def test_zero_header_size_and_bad_type_fail(self):
        touch(self.root / "A.scene", asset_header(header_size=0))
        touch(self.root / "B.scene", asset_header(asset_type=9))

        result, output = self.run_migrate()

        self.assertEqual(result, 1, output)
        self.assertIn("A.scene", output)
        self.assertIn("B.scene", output)

    def test_future_version_fails(self):
        touch(self.root / "Level.scene", asset_header(version=(2, 0, 0)))

        result, output = self.run_migrate()

        self.assertEqual(result, 1, output)
        self.assertIn("newer", output)

    def test_dry_run_is_also_read_only_and_fails_on_old_version(self):
        touch(self.root / "Level.prefab", asset_header(version=(0, 1, 0), asset_type=3))
        before = self.snapshot()

        result, output = self.run_migrate(dry_run=True)

        self.assertEqual(result, 1, output)
        self.assertNotIn("Would migrate", output)
        self.assert_untouched(before)


if __name__ == "__main__":
    unittest.main()
