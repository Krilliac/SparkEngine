"""Regression checks for the real MMO source validator and legacy generator."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import shutil

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from blender.validate_mmo_props import NAMES, validate_model


class SourceContractTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "chest.obj"
        directory = ROOT / "Assets/Models/MMO"
        for suffix in (".obj", ".mtl"):
            shutil.copyfile(directory / ("chest" + suffix), self.path.with_suffix(suffix))

    def change_first(self, prefix, transform):
        lines = self.path.read_text().splitlines()
        for index, line in enumerate(lines):
            if line.startswith(prefix):
                lines[index] = transform(line)
                self.path.write_text("\n".join(lines) + "\n")
                return
        self.fail("The real authored fixture lacks " + prefix)

    def test_real_authored_export_passes(self):
        self.assertGreater(validate_model(self.path)["triangles"], 0)

    def test_missing_uv_reference_is_rejected(self):
        def omit_uv(line):
            parts = line.split()
            vertex, _, normal = parts[1].split("/")
            parts[1] = vertex + "//" + normal
            return " ".join(parts)
        self.change_first("f ", omit_uv)
        with self.assertRaisesRegex(ValueError, "lacks authored UV"):
            validate_model(self.path)

    def test_zero_normal_is_rejected(self):
        self.change_first("vn ", lambda _: "vn 0 0 0")
        with self.assertRaisesRegex(ValueError, "not unit length"):
            validate_model(self.path)

    def test_nonfinite_position_is_rejected(self):
        self.change_first("v ", lambda _: "v nan 0 0")
        with self.assertRaisesRegex(ValueError, "Nonfinite position"):
            validate_model(self.path)

    def test_reversed_triangle_is_rejected(self):
        def reverse(line):
            parts = line.split()
            return " ".join([parts[0], parts[1], parts[3], parts[2]])
        self.change_first("f ", reverse)
        with self.assertRaisesRegex(ValueError, "winding"):
            validate_model(self.path)

    def test_missing_material_library_is_rejected(self):
        self.path.with_suffix(".mtl").unlink()
        with self.assertRaisesRegex(ValueError, "Missing material library"):
            validate_model(self.path)

    def test_unknown_material_is_rejected(self):
        self.change_first("usemtl ", lambda _: "usemtl NoSuchMaterial")
        with self.assertRaisesRegex(ValueError, "undefined material"):
            validate_model(self.path)

    def test_unreferenced_extrema_cannot_hide_shrunken_geometry(self):
        lines = self.path.read_text().splitlines()
        for index, line in enumerate(lines):
            if line.startswith("v "):
                lines[index] = "v " + " ".join(str(float(value) * 0.5) for value in line.split()[1:4])
        # These retain the old declared extrema but are never rendered/imported.
        lines.extend(["v -.5 -.5 -.5", "v .5 .5 .5"])
        self.path.write_text("\n".join(lines) + "\n")
        with self.assertRaisesRegex(ValueError, "gameplay bounds changed"):
            validate_model(self.path)


class GeneratorPreservationTests(unittest.TestCase):
    def test_invalid_curated_models_are_not_overwritten_by_placeholders(self):
        spec = importlib.util.spec_from_file_location("mmo_generator", ROOT / "tools/generate_mmo_assets.py")
        generator = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(generator)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            directory = root / "Assets/Models/MMO"
            directory.mkdir(parents=True)
            for name in NAMES:
                (directory / f"{name}.obj").write_text("# deliberately incomplete curated source\n")
            before = {path.relative_to(root).as_posix(): path.read_bytes()
                      for path in root.rglob("*") if path.is_file()}
            generator.ASSET_ROOT = str(root / "Assets")
            with self.assertRaises(ValueError):
                generator.main()
            self.assertEqual(before, {path.relative_to(root).as_posix(): path.read_bytes()
                                      for path in root.rglob("*") if path.is_file()})


if __name__ == "__main__":
    unittest.main()
