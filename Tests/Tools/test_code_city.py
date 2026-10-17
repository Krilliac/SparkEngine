#!/usr/bin/env python3
"""Tests for tools/architecture-viz/generate_code_city.py against the real repository.

The generator is run over the live tracked tree (no fixtures), so these tests
also catch a layout or include-resolution regression introduced by a source
reorganization.
"""
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "architecture-viz"))

import generate_code_city as city  # noqa: E402


class CodeCityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.paths = city.tracked_sources()
        cls.data = city.build_city(cls.paths)
        cls.files = cls.data["files"]
        cls.by_path = {f["path"]: i for i, f in enumerate(cls.files)}

    def test_every_tracked_source_outside_third_party_is_a_building(self) -> None:
        self.assertGreater(len(self.files), 1000)
        self.assertFalse([p for p in self.paths if p.startswith("ThirdParty/")])
        self.assertIn("SparkEngine/Source/Core/EngineContext.h", self.by_path)
        for entry in self.files:
            self.assertGreater(entry["loc"], 0, entry["path"])

    def test_buildings_sit_inside_their_district_and_districts_inside_their_project(self) -> None:
        eps = 0.05
        for entry in self.files:
            dx, dz, dw, dh = self.data["districts"][entry["d"]]["rect"]
            half = entry["s"] / 2
            self.assertTrue(dx - eps <= entry["x"] - half and entry["x"] + half <= dx + dw + eps, entry["path"])
            self.assertTrue(dz - eps <= entry["z"] - half and entry["z"] + half <= dz + dh + eps, entry["path"])
        for district in self.data["districts"]:
            px, pz, pw, ph = self.data["projects"][district["project"]]["rect"]
            dx, dz, dw, dh = district["rect"]
            self.assertTrue(px - eps <= dx and dx + dw <= px + pw + eps, district["name"])
            self.assertTrue(pz - eps <= dz and dz + dh <= pz + ph + eps, district["name"])

    def test_districts_of_a_project_do_not_overlap(self) -> None:
        by_project: dict[int, list[list[float]]] = {}
        for district in self.data["districts"]:
            by_project.setdefault(district["project"], []).append(district["rect"])
        for rects in by_project.values():
            for i, a in enumerate(rects):
                for b in rects[i + 1:]:
                    overlap_w = min(a[0] + a[2], b[0] + b[2]) - max(a[0], b[0])
                    overlap_h = min(a[1] + a[3], b[1] + b[3]) - max(a[1], b[1])
                    self.assertFalse(overlap_w > 0.05 and overlap_h > 0.05, (a, b))

    def test_includes_resolve_to_real_files(self) -> None:
        source = self.by_path["SparkEngine/Source/Core/EngineContext.cpp"]
        header = self.by_path["SparkEngine/Source/Core/EngineContext.h"]
        self.assertIn(header, self.files[source]["inc"])
        # Quoted includes that name no tracked file stay rare; a jump means the resolver broke.
        self.assertLess(self.data["meta"]["unresolvedIncludes"], 50)
        self.assertGreater(self.data["meta"]["includeEdges"], 5000)

    def test_locate_groups_by_subsystem(self) -> None:
        self.assertEqual(city.locate("SparkEngine/Source/Engine/AI/BehaviorTree.cpp"), ("SparkEngine", "Engine/AI"))
        self.assertEqual(city.locate("SparkEngine/Source/Graphics/RHI/D3D11/X.cpp"), ("SparkEngine", "Graphics/RHI"))
        self.assertEqual(city.locate("SparkEngine/Source/Core/EngineContext.h"), ("SparkEngine", "Core"))
        self.assertEqual(city.locate("GameModules/SparkGameFPS/Source/Weapons/Gun.cpp"),
                         ("GameModules/SparkGameFPS", "Weapons"))
        self.assertEqual(city.locate("SparkEditor/Source/Panels/InspectorPanel.cpp"), ("SparkEditor", "Panels"))

    def test_squarify_tiles_the_rectangle_exactly(self) -> None:
        rects = city.squarify([6, 6, 4, 3, 2, 2, 1], 0, 0, 6, 4)
        self.assertAlmostEqual(sum(w * h for _, _, w, h in rects), 24.0, places=6)
        for x, z, w, h in rects:
            self.assertTrue(-1e-9 <= x and x + w <= 6 + 1e-9 and -1e-9 <= z and z + h <= 4 + 1e-9)

    def test_rendered_page_embeds_parseable_data_once(self) -> None:
        html = city.render(self.data)
        self.assertNotIn(city.DATA_MARKER, html)
        match = re.search(r"window\.CODE_CITY = (\{.*?\});</script>", html, re.S)
        self.assertIsNotNone(match)
        embedded = json.loads(match.group(1).replace("<\\/", "</"))
        self.assertEqual(embedded["meta"]["files"], len(self.files))
        # Pinned three.js from an allowed CDN; no other remote script origins.
        self.assertIn("https://cdn.jsdelivr.net/npm/three@", html)
        self.assertEqual(set(re.findall(r'https://([^/"]+)/', html)), {"cdn.jsdelivr.net"})

    def test_cli_writes_the_page(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "city" / "index.html"
            self.assertEqual(city.main(["--output", str(out)]), 0)
            self.assertGreater(out.stat().st_size, 100_000)


if __name__ == "__main__":
    unittest.main()
