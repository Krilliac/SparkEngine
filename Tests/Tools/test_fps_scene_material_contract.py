#!/usr/bin/env python3
"""Validate authored material coverage for the packaged SparkGameFPS scene.

This is intentionally a content contract rather than a renderer test.  The
legacy scene loader preserves ``material=`` values, while GameObject rendering
only has a supported path for confined JSON materials.  Keeping the contract
here prevents a newly authored visible object from silently falling back to a
white debug material.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SCENE_PATH = REPO_ROOT / "Assets" / "Scenes" / "level1.scene"
ASSET_ROOT = REPO_ROOT / "Assets"
MATERIAL_PREFIX = "Assets/Materials/"


class FPSSceneMaterialContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.scene_text = SCENE_PATH.read_text(encoding="utf-8")
        cls.objects = cls._parse_objects(cls.scene_text)

    @staticmethod
    def _parse_objects(scene_text: str) -> list[dict[str, str]]:
        objects: list[dict[str, str]] = []
        section = ""
        current: dict[str, str] | None = None

        def finish() -> None:
            if current is not None:
                objects.append(dict(current))

        for raw_line in scene_text.splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                finish()
                section = line[1:-1]
                current = {} if section == "Object" else None
                continue
            if section == "Object" and current is not None and "=" in line:
                key, value = line.split("=", 1)
                current[key.strip()] = value.strip()
        finish()
        return objects

    @staticmethod
    def _confined_asset(relative_path: str) -> Path:
        if not relative_path.startswith(MATERIAL_PREFIX):
            raise AssertionError(f"asset path is outside the project material namespace: {relative_path!r}")
        if "\\" in relative_path or "//" in relative_path or ".." in Path(relative_path).parts:
            raise AssertionError(f"asset path is not canonical: {relative_path!r}")
        candidate = (REPO_ROOT / Path(*relative_path.split("/"))).resolve(strict=False)
        root = REPO_ROOT.resolve()
        try:
            candidate.relative_to(root)
        except ValueError as error:
            raise AssertionError(f"asset path escapes the project root: {relative_path!r}") from error
        return candidate

    @staticmethod
    def _confined_material_texture(material_path: Path, declared_path: str) -> Path:
        if not declared_path or Path(declared_path).is_absolute():
            raise AssertionError(f"material texture path must be project-relative: {declared_path!r}")
        if "\\" in declared_path or "//" in declared_path or ".." in Path(declared_path).parts:
            raise AssertionError(f"material texture path is not canonical: {declared_path!r}")
        relative = declared_path.removeprefix("Assets/")
        candidate = (ASSET_ROOT / relative).resolve(strict=False)
        try:
            candidate.relative_to(ASSET_ROOT.resolve())
        except ValueError as error:
            raise AssertionError(
                f"material texture escapes Assets: {material_path.relative_to(REPO_ROOT)} -> {declared_path!r}"
            ) from error
        return candidate

    def test_every_renderable_object_has_a_confined_existing_material(self) -> None:
        self.assertGreater(len(self.objects), 0, "level1.scene contains no renderable objects")
        missing = [obj.get("name", "<unnamed>") for obj in self.objects if not obj.get("material")]
        self.assertEqual(missing, [], f"renderable FPS objects without authored materials: {missing}")

        for obj in self.objects:
            name = obj.get("name", "<unnamed>")
            material = obj["material"]
            self.assertTrue(material.startswith(MATERIAL_PREFIX), f"{name}: unsupported material path {material!r}")
            self.assertTrue(material.endswith(".json"), f"{name}: material is not JSON: {material!r}")
            material_path = self._confined_asset(material)
            self.assertTrue(material_path.is_file(), f"{name}: material does not exist: {material}")

    def test_material_json_texture_references_are_confined_and_present(self) -> None:
        materials = {obj["material"] for obj in self.objects}
        for material in sorted(materials):
            material_path = self._confined_asset(material)
            definition = json.loads(material_path.read_text(encoding="utf-8"))
            self.assertIsInstance(definition, dict, material)
            for key in ("albedo", "normal"):
                declared = definition.get(key)
                self.assertIsInstance(declared, str, f"{material}: missing string {key}")
                texture_path = self._confined_material_texture(material_path, declared)
                self.assertTrue(texture_path.is_file(), f"{material}: {key} texture does not exist: {declared}")

    def test_contract_rejects_missing_and_escaping_materials(self) -> None:
        missing = self.scene_text.replace(
            "name=Arena_Floor_Main\nmaterial=Assets/Materials/Terrain_Dirt.json\n",
            "name=Arena_Floor_Main\n",
            1,
        )
        self.assertEqual(
            [obj.get("name") for obj in self._parse_objects(missing) if not obj.get("material")],
            ["Arena_Floor_Main"],
        )

        escaping = self.scene_text.replace(
            "material=Assets/Materials/Concrete.json", "material=Assets/Materials/../Secrets.json", 1
        )
        escaped_object = next(obj for obj in self._parse_objects(escaping) if obj.get("material", "").endswith("Secrets.json"))
        with self.assertRaises(AssertionError):
            self._confined_asset(escaped_object["material"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
