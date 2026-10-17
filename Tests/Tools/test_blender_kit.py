"""Unit tests for tools/blender/validate_kit.py using small fixture kits in a temp dir."""
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from blender.validate_kit import validate_kit

MTL = "newmtl Kit_stone\nKd 0.4 0.4 0.4\n"
UVS = ["vt 0 0", "vt 1 0", "vt 1 1", "vt 0 1"]


def box_obj(name, triangles=12):
    """Unit cube OBJ (y from 0 to 1) keeping the first `triangles` of its 12 triangles."""
    lines = [f"mtllib {name}.mtl", f"o {name}"]
    lines += [f"v {x} {y} {z}" for x in (-.5, .5) for y in (0, 1) for z in (-.5, .5)]
    lines += UVS
    lines += ["vn -1 0 0", "vn 1 0 0", "vn 0 -1 0", "vn 0 1 0", "vn 0 0 -1", "vn 0 0 1"]
    lines.append("usemtl Kit_stone")
    quads = [((1, 2, 4, 3), 1), ((5, 7, 8, 6), 2), ((1, 5, 6, 2), 3),
             ((3, 4, 8, 7), 4), ((1, 3, 7, 5), 5), ((2, 6, 8, 4), 6)]
    faces = []
    for (a, b, c, d), normal in quads:
        faces.append(f"f {a}/1/{normal} {b}/2/{normal} {c}/3/{normal}")
        faces.append(f"f {a}/1/{normal} {c}/3/{normal} {d}/4/{normal}")
    return "\n".join(lines + faces[:triangles]) + "\n"


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


class ValidateKitTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.kit = self.root / "Assets/Models/Test/Kit"
        self.kit.mkdir(parents=True)
        art = self.root / "Art/Blender/Test"
        art.mkdir(parents=True)
        for relative, text in (("LICENSE", "license\n"), ("tools/blender/author_test_kit.py", "# author\n"),
                               ("tools/blender/spark_kit.py", "# library\n"), ("Art/Blender/Test/test_kit.blend", "blend")):
            (self.root / relative).parent.mkdir(parents=True, exist_ok=True)
            (self.root / relative).write_text(text)
        self.write_mesh("crate", 12)
        self.write_mesh("crate_lod1", 6)
        self.write_mesh("crate_collision", 12)
        self.provenance = art / "provenance.json"
        self.write_provenance()

    def write_mesh(self, name, triangles):
        (self.kit / f"{name}.obj").write_text(box_obj(name, triangles))
        (self.kit / f"{name}.mtl").write_text(MTL)

    def mesh_record(self, name):
        obj = self.kit / f"{name}.obj"
        triangles = sum(line.startswith("f ") for line in obj.read_text().splitlines())
        return {"obj_path": obj.relative_to(self.root).as_posix(),
                "mtl_path": obj.with_suffix(".mtl").relative_to(self.root).as_posix(),
                "bounds": {"min": [-.5, 0.0, -.5], "max": [.5, 1.0, .5]}, "triangle_count": triangles,
                "obj_sha256": sha(obj), "mtl_sha256": sha(obj.with_suffix(".mtl"))}

    def write_provenance(self, collision_cap=12, budget=100):
        def entry(relative):
            return {"path": relative, "sha256": sha(self.root / relative)}

        asset = {"name": "crate", **self.mesh_record("crate"), "triangle_budget": budget,
                 "variants": {"lod1": {**self.mesh_record("crate_lod1"), "decimate_ratio": .5},
                              "collision": {**self.mesh_record("crate_collision"), "kind": "box",
                                            "triangle_cap": collision_cap}}}
        manifest = {"schema_version": 1, "blender_version": "4.0.2", "module": "Test",
                    "author_script": entry("tools/blender/author_test_kit.py"),
                    "library": entry("tools/blender/spark_kit.py"),
                    "blend": entry("Art/Blender/Test/test_kit.blend"),
                    "license": {"name": "Spark Open License 1.0", **entry("LICENSE")}, "assets": [asset]}
        self.provenance.write_text(json.dumps(manifest, indent=2))

    def assert_rejected(self, pattern):
        with self.assertRaisesRegex(ValueError, pattern):
            validate_kit(self.provenance)

    def test_valid_kit_passes(self):
        self.assertEqual(validate_kit(self.provenance),
                         [{"name": "crate", "triangles": 12, "lod1": 6, "collision": 12}])

    def test_missing_file_is_rejected(self):
        (self.kit / "crate_lod1.obj").unlink()
        self.assert_rejected("missing file")

    def test_hash_mismatch_is_rejected(self):
        (self.root / "Art/Blender/Test/test_kit.blend").write_text("edited")
        self.assert_rejected("sha256 mismatch")

    def test_faceless_mesh_is_rejected(self):
        self.write_mesh("crate", 0)
        self.write_provenance()
        self.assert_rejected("no faces")

    def test_missing_normals_are_rejected(self):
        obj = self.kit / "crate.obj"
        lines = [line for line in obj.read_text().splitlines() if not line.startswith("vn ")]
        lines = [" ".join(["f"] + [corner.rsplit("/", 1)[0] for corner in line.split()[1:]])
                 if line.startswith("f ") else line for line in lines]
        obj.write_text("\n".join(lines) + "\n")
        self.write_provenance()
        self.assert_rejected("normal")

    def test_lod_not_smaller_is_rejected(self):
        self.write_mesh("crate_lod1", 12)
        self.write_provenance()
        self.assert_rejected("LOD1 .* not smaller")

    def test_over_budget_collision_is_rejected(self):
        self.write_provenance(collision_cap=10)
        self.assert_rejected("collision 12 triangles exceed cap 10")

    def test_over_budget_source_is_rejected(self):
        self.write_provenance(budget=11)
        self.assert_rejected("exceed budget 11")

    def test_undefined_material_is_rejected(self):
        (self.kit / "crate.mtl").write_text("newmtl Other\nKd 1 1 1\n")
        self.write_provenance()
        self.assert_rejected("materials missing from MTL")

    def test_bounds_drift_is_rejected(self):
        obj = self.kit / "crate.obj"
        obj.write_text(obj.read_text().replace("v 0.5 1 0.5", "v 0.5 1.5 0.5"))
        self.write_provenance()
        self.assert_rejected("bounds differ")


if __name__ == "__main__":
    unittest.main()
