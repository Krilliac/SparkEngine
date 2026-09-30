#!/usr/bin/env python3
"""CI-110 / PERF-100: the committed golden-image baseline set is self-consistent.

The C++ lanes (CTest SparkOpenGLGoldenTests, VulkanGoldenTests) only compare
baselines on Linux software rasterizers. This suite checks the committed set on
any host, without a GPU:

* Tests/GoldenImages/manifest.json parses under exactly the rules of
  SparkEngine/Source/Utils/GoldenImageManifest.h (schemaVersion 1, no unknown or
  missing keys, known backendRow whose software flag agrees, scene id shape,
  threshold ranges, non-empty reviewer, 64-hex SHA-256, no duplicate
  scene/backendRow, no duplicate JSON keys);
* every entry's PNG exists at <backendRow>/<scene>.png with the reviewed
  SHA-256, is a structurally valid PNG (signature, CRC-checked IHDR, IEND), and
  every committed PNG has exactly one entry (no orphans);
* a baseline whose review is still pending cannot sit on a hardware row;
* each golden lane source declares a kRow and a kScenes set and is registered
  in Tests/CMakeLists.txt; the union of kScenes over the lanes of one row
  equals the manifest's scenes for that row, and no scene is claimed by two
  lanes of the same row.
"""

from __future__ import annotations

import hashlib
import json
import math
import re
import shutil
import struct
import tempfile
import unittest
import zlib
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
GOLDEN_ROOT = REPO_ROOT / "Tests" / "GoldenImages"
TESTS_CMAKE = REPO_ROOT / "Tests" / "CMakeLists.txt"
LANE_GLOB = "TestRHI*Golden*.cpp"

# Mirrors GoldenManifest::ResolveBackendRow: row -> software rasterizer.
BACKEND_ROWS = {
    "d3d11-warp": True,
    "d3d11-hw": False,
    "opengl-llvmpipe": True,
    "vulkan-lavapipe": True,
}
ENTRY_FIELDS = (
    "scene",
    "backendRow",
    "software",
    "perPixelThreshold",
    "tolerancePercent",
    "reviewer",
    "baselineSha256",
)
MAX_PIXEL_DISTANCE = 441.68
PENDING_REVIEW = "owner review pending"
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
_SCENE_RE = re.compile(r"[A-Za-z0-9_-]{1,128}")
_SHA_RE = re.compile(r"[0-9a-f]{64}")
_LANE_ROW_RE = re.compile(r'constexpr\s+const\s+char\s*\*\s*kRow\s*=\s*"([^"]+)"')
_LANE_SCENES_RE = re.compile(r"kScenes\s*=\s*\{([^}]*)\}", re.DOTALL)


def _is_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON key {key!r}")
        result[key] = value
    return result


def entry_errors(index: int, entry: Any) -> list[str]:
    where = f"entries[{index}]"
    if not isinstance(entry, dict) or set(entry) - set(ENTRY_FIELDS):
        return [f"{where}: entry must be an object with only the documented fields"]
    missing = [field for field in ENTRY_FIELDS if field not in entry]
    if missing:
        return [f"{where}: missing field {missing[0]!r}"]
    if not all(isinstance(entry[f], str) for f in ("scene", "backendRow", "reviewer", "baselineSha256")):
        return [f"{where}: field has the wrong type"]
    if not isinstance(entry["software"], bool):
        return [f"{where}: field has the wrong type"]
    if not _is_number(entry["perPixelThreshold"]) or not _is_number(entry["tolerancePercent"]):
        return [f"{where}: field has the wrong type"]

    errors: list[str] = []
    if _SCENE_RE.fullmatch(entry["scene"]) is None:
        errors.append(f"{where}: scene id must be 1-128 chars of [A-Za-z0-9_-]")
    row = entry["backendRow"]
    if row not in BACKEND_ROWS:
        errors.append(f"{where}: unknown backendRow {row!r}")
    elif BACKEND_ROWS[row] != entry["software"]:
        errors.append(f"{where}: software flag disagrees with backendRow {row!r}")
    per_pixel = float(entry["perPixelThreshold"])
    if not math.isfinite(per_pixel) or not 0.0 <= per_pixel <= MAX_PIXEL_DISTANCE:
        errors.append(f"{where}: perPixelThreshold must be finite and within [0, {MAX_PIXEL_DISTANCE}]")
    tolerance = float(entry["tolerancePercent"])
    if not math.isfinite(tolerance) or not 0.0 <= tolerance <= 100.0:
        errors.append(f"{where}: tolerancePercent must be finite and within [0, 100]")
    if not entry["reviewer"].strip(" \t"):
        errors.append(f"{where}: reviewer must be non-empty")
    elif PENDING_REVIEW in entry["reviewer"] and not BACKEND_ROWS.get(row, True):
        errors.append(f"{where}: a baseline with {PENDING_REVIEW!r} cannot back hardware row {row!r}")
    if _SHA_RE.fullmatch(entry["baselineSha256"]) is None:
        errors.append(f"{where}: baselineSha256 must be 64 lowercase hex characters")
    return errors


def load_manifest(golden_root: Path) -> tuple[list[dict[str, Any]], list[str]]:
    """Return (valid entries, errors). Any error invalidates the manifest."""
    try:
        text = (golden_root / "manifest.json").read_text(encoding="utf-8")
    except (OSError, ValueError) as exc:
        return [], [f"manifest.json: {exc}"]
    return parse_manifest_text(text)


def parse_manifest_text(text: str) -> tuple[list[dict[str, Any]], list[str]]:
    """Parse manifest.json content (also used by tools/perf-budget/check_golden_review.py)."""
    try:
        document = json.loads(text, object_pairs_hook=_reject_duplicate_keys)
    except ValueError as exc:
        return [], [f"manifest.json: {exc}"]
    if (
        not isinstance(document, dict)
        or set(document) != {"schemaVersion", "entries"}
        or not _is_number(document["schemaVersion"])
        or document["schemaVersion"] != 1
        or not isinstance(document["entries"], list)
    ):
        return [], ['manifest.json: root must be {"schemaVersion": 1, "entries": [...]}']

    errors: list[str] = []
    seen: set[tuple[str, str]] = set()
    for index, entry in enumerate(document["entries"]):
        problems = entry_errors(index, entry)
        if problems:
            errors.extend(problems)
            continue
        key = (entry["scene"], entry["backendRow"])
        if key in seen:
            errors.append(f"entries[{index}]: duplicate scene/backendRow {key}")
        seen.add(key)
    return (document["entries"] if not errors else []), errors


def png_errors(label: str, data: bytes) -> list[str]:
    if not data.startswith(PNG_SIGNATURE):
        return [f"{label}: not a PNG (bad signature)"]
    if len(data) < 8 + 25 + 12:
        return [f"{label}: truncated PNG"]
    length, chunk_type = struct.unpack_from(">I4s", data, 8)
    if chunk_type != b"IHDR" or length != 13:
        return [f"{label}: first chunk must be a 13-byte IHDR"]
    ihdr = data[16:29]
    (crc,) = struct.unpack_from(">I", data, 29)
    if zlib.crc32(b"IHDR" + ihdr) != crc:
        return [f"{label}: IHDR CRC mismatch"]
    width, height, bit_depth, color_type = struct.unpack(">IIBB", ihdr[:10])
    errors: list[str] = []
    if width == 0 or height == 0:
        errors.append(f"{label}: IHDR has a zero dimension")
    if color_type not in (0, 2, 3, 4, 6) or bit_depth not in (1, 2, 4, 8, 16):
        errors.append(f"{label}: IHDR color type {color_type} / bit depth {bit_depth} is invalid")
    if data[-12:] != b"\x00\x00\x00\x00IEND\xaeB`\x82":
        errors.append(f"{label}: PNG does not end with an IEND chunk")
    return errors


def baseline_errors(golden_root: Path, entries: list[dict[str, Any]]) -> list[str]:
    errors: list[str] = []
    expected: set[str] = set()
    for entry in entries:
        relative = f"{entry['backendRow']}/{entry['scene']}.png"
        expected.add(relative)
        path = golden_root / relative
        if not path.is_file():
            errors.append(f"{relative}: baseline PNG is missing")
            continue
        data = path.read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if digest != entry["baselineSha256"]:
            errors.append(f"{relative}: SHA-256 {digest} != reviewed {entry['baselineSha256']}")
        errors.extend(png_errors(relative, data))
    committed = {p.relative_to(golden_root).as_posix() for p in golden_root.rglob("*.png")}
    for orphan in sorted(committed - expected):
        errors.append(f"{orphan}: committed baseline has no manifest entry")
    return errors


def lane_errors(tests_root: Path, cmake_text: str, entries: list[dict[str, Any]]) -> list[str]:
    """Every manifest scene is compared by exactly one registered lane of its row.

    A row may be split across several lane sources (one fixture per source file),
    so the union of their kScenes must equal the manifest's scenes for that row
    and no scene may be claimed by two lanes of the same row.
    """
    manifest_rows: dict[str, set[str]] = {}
    for entry in entries:
        manifest_rows.setdefault(entry["backendRow"], set()).add(entry["scene"])

    errors: list[str] = []
    # A baseline-comparing lane declares the backend row it renders on; other
    # golden sources (for example the D3D11 GoldenImageTest wiring) do not.
    lanes: list[tuple[Path, str, re.Match[str]]] = []
    for lane in sorted(tests_root.glob(LANE_GLOB)):
        text = lane.read_text(encoding="utf-8")
        row_match = _LANE_ROW_RE.search(text)
        if row_match is not None:
            lanes.append((lane, text, row_match))
    if not lanes:
        return [f"no golden lane source matching Tests/{LANE_GLOB} declares a kRow"]

    # row -> scene -> lane sources declaring it
    declared: dict[str, dict[str, list[str]]] = {}
    for lane, text, row_match in lanes:
        scenes_match = _LANE_SCENES_RE.search(text)
        if scenes_match is None:
            errors.append(f"{lane.name}: golden lane declares kRow but no kScenes")
            continue
        row = row_match.group(1)
        row_scenes = declared.setdefault(row, {})
        for scene in set(re.findall(r'"([^"]+)"', scenes_match.group(1))):
            row_scenes.setdefault(scene, []).append(lane.name)
        if f"SPARK_TEST_FILE={lane.name};" not in cmake_text:
            errors.append(f"{lane.name}: golden lane is not registered in Tests/CMakeLists.txt")

    for row in sorted(declared):
        row_scenes = declared[row]
        for scene in sorted(row_scenes):
            if len(row_scenes[scene]) > 1:
                errors.append(f"{row}/{scene}: declared by more than one lane {sorted(row_scenes[scene])}")
        union = set(row_scenes)
        expected = manifest_rows.get(row, set())
        if union != expected:
            owners = sorted({name for names in row_scenes.values() for name in names})
            errors.append(
                f"{', '.join(owners)}: kScenes {sorted(union)} != manifest scenes for {row} {sorted(expected)}"
            )
    for row in sorted(set(manifest_rows) - set(declared)):
        errors.append(f"manifest row {row} has baselines but no golden lane compares them")
    return errors


def golden_errors(golden_root: Path, tests_root: Path, cmake_text: str) -> list[str]:
    entries, errors = load_manifest(golden_root)
    if errors:
        return errors
    if not entries:
        return ["manifest.json has no entries; the committed baseline set cannot be empty"]
    return baseline_errors(golden_root, entries) + lane_errors(tests_root, cmake_text, entries)


class GoldenManifestLiveTests(unittest.TestCase):
    def test_committed_baseline_set_is_consistent(self) -> None:
        errors = golden_errors(GOLDEN_ROOT, REPO_ROOT / "Tests", TESTS_CMAKE.read_text(encoding="utf-8"))
        self.assertEqual([], errors, "\n".join(errors))

    def test_every_committed_row_is_covered(self) -> None:
        entries, errors = load_manifest(GOLDEN_ROOT)
        self.assertEqual([], errors)
        rows = {entry["backendRow"] for entry in entries}
        self.assertEqual({"d3d11-warp", "opengl-llvmpipe", "vulkan-lavapipe"}, rows)
        self.assertEqual(len(entries), len(list(GOLDEN_ROOT.rglob("*.png"))))


class GoldenManifestDriftTests(unittest.TestCase):
    """Each rule fails on a copy of the committed set with one defect."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name) / "GoldenImages"
        shutil.copytree(GOLDEN_ROOT, self.root)
        self.cmake_text = TESTS_CMAKE.read_text(encoding="utf-8")
        self.assertEqual([], self.errors())

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def errors(self) -> list[str]:
        return golden_errors(self.root, REPO_ROOT / "Tests", self.cmake_text)

    def manifest(self) -> dict[str, Any]:
        return json.loads((self.root / "manifest.json").read_text(encoding="utf-8"))

    def write_manifest(self, document: dict[str, Any]) -> None:
        (self.root / "manifest.json").write_text(json.dumps(document, indent=2), encoding="utf-8")

    def assertSingleError(self, needle: str) -> None:
        errors = self.errors()
        self.assertEqual(1, len(errors), errors)
        self.assertIn(needle, errors[0])

    def test_flipped_sha_digit_fails(self) -> None:
        document = self.manifest()
        sha = document["entries"][0]["baselineSha256"]
        document["entries"][0]["baselineSha256"] = ("0" if sha[0] != "0" else "1") + sha[1:]
        self.write_manifest(document)
        entry = document["entries"][0]
        self.assertSingleError(f"{entry['backendRow']}/{entry['scene']}.png: SHA-256")

    def test_orphan_png_fails(self) -> None:
        source = next(self.root.rglob("*.png"))
        shutil.copyfile(source, source.parent / "Orphan_Scene.png")
        self.assertSingleError("Orphan_Scene.png: committed baseline has no manifest entry")

    def test_missing_png_fails(self) -> None:
        entry = self.manifest()["entries"][0]
        (self.root / entry["backendRow"] / f"{entry['scene']}.png").unlink()
        errors = self.errors()
        self.assertIn(f"{entry['backendRow']}/{entry['scene']}.png: baseline PNG is missing", errors)

    def test_corrupt_png_with_matching_sha_fails(self) -> None:
        document = self.manifest()
        entry = document["entries"][0]
        path = self.root / entry["backendRow"] / f"{entry['scene']}.png"
        data = bytearray(path.read_bytes())
        data[20] ^= 0xFF  # inside IHDR width
        path.write_bytes(bytes(data))
        entry["baselineSha256"] = hashlib.sha256(bytes(data)).hexdigest()
        self.write_manifest(document)
        self.assertSingleError("IHDR CRC mismatch")

    def test_duplicate_entry_fails(self) -> None:
        document = self.manifest()
        document["entries"].append(dict(document["entries"][0]))
        self.write_manifest(document)
        self.assertSingleError("duplicate scene/backendRow")

    def test_duplicate_json_key_fails(self) -> None:
        text = (self.root / "manifest.json").read_text(encoding="utf-8")
        (self.root / "manifest.json").write_text(
            text.replace('"schemaVersion": 1,', '"schemaVersion": 1, "schemaVersion": 1,', 1), encoding="utf-8"
        )
        self.assertSingleError("duplicate JSON key")

    def test_schema_rules_match_the_cpp_parser(self) -> None:
        cases = (
            (lambda e: e.update(extra=1), "only the documented fields"),
            (lambda e: e.pop("reviewer"), "missing field 'reviewer'"),
            (lambda e: e.update(software=1), "wrong type"),
            (lambda e: e.update(perPixelThreshold=True), "wrong type"),
            (lambda e: e.update(scene="bad scene"), "scene id must be"),
            (lambda e: e.update(backendRow="metal-hw"), "unknown backendRow"),
            (lambda e: e.update(software=False), "software flag disagrees"),
            (lambda e: e.update(perPixelThreshold=500), "perPixelThreshold must be"),
            (lambda e: e.update(tolerancePercent=-1), "tolerancePercent must be"),
            (lambda e: e.update(reviewer=" \t"), "reviewer must be non-empty"),
            (lambda e: e.update(baselineSha256="A" * 64), "baselineSha256 must be"),
        )
        for mutate, needle in cases:
            with self.subTest(needle=needle):
                entry = copy_of(self.manifest()["entries"][0])
                mutate(entry)
                errors = entry_errors(0, entry)
                self.assertEqual(1, len(errors), errors)
                self.assertIn(needle, errors[0])

    def test_pending_review_cannot_back_a_hardware_row(self) -> None:
        entry = copy_of(self.manifest()["entries"][0])
        # The live entries are owner-reviewed now; plant a pending record explicitly.
        entry.update(backendRow="d3d11-hw", software=False, reviewer=f"agent capture; {PENDING_REVIEW}")
        errors = entry_errors(0, entry)
        self.assertEqual(1, len(errors), errors)
        self.assertIn("cannot back hardware row", errors[0])

    def test_wrong_root_fails(self) -> None:
        document = self.manifest()
        document["schemaVersion"] = 2
        self.write_manifest(document)
        self.assertSingleError("root must be")

    def test_empty_manifest_fails(self) -> None:
        for png in list(self.root.rglob("*.png")):
            png.unlink()
        self.write_manifest({"schemaVersion": 1, "entries": []})
        self.assertSingleError("has no entries")

    def test_lane_scene_drift_fails(self) -> None:
        document = self.manifest()
        removed = next(e for e in document["entries"] if e["backendRow"] == "vulkan-lavapipe")
        document["entries"].remove(removed)
        self.write_manifest(document)
        (self.root / removed["backendRow"] / f"{removed['scene']}.png").unlink()
        self.assertSingleError("TestRHI230VulkanGoldenReal.cpp: kScenes")

    def test_unregistered_lane_fails(self) -> None:
        self.cmake_text = self.cmake_text.replace("SPARK_TEST_FILE=TestRHI240OpenGLGoldenReal.cpp;", "")
        self.assertSingleError("TestRHI240OpenGLGoldenReal.cpp: golden lane is not registered")


class GoldenLaneUnionTests(unittest.TestCase):
    """One backend row split across several lane sources (one fixture each)."""

    ROW = "d3d11-warp"

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tests_root = Path(self._tmp.name)
        self.cmake_text = ""
        self.entries = [
            {"scene": scene, "backendRow": self.ROW} for scene in ("PassA", "PassB", "Canonical_Scene")
        ]

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def add_lane(self, name: str, scenes: list[str], *, registered: bool = True) -> None:
        quoted = ", ".join(f'"{scene}"' for scene in scenes)
        (self.tests_root / name).write_text(
            f'constexpr const char* kRow = "{self.ROW}";\n'
            f"const std::array<const char*, {len(scenes)}> kScenes = {{{quoted}}};\n",
            encoding="utf-8",
        )
        if registered:
            self.cmake_text += f"SPARK_TEST_FILE={name};\n"

    def errors(self) -> list[str]:
        return lane_errors(self.tests_root, self.cmake_text, self.entries)

    def test_two_lanes_whose_union_equals_the_manifest_pass(self) -> None:
        self.add_lane("TestRHI210PassGoldenReal.cpp", ["PassA", "PassB"])
        self.add_lane("TestRHI210SceneGoldenReal.cpp", ["Canonical_Scene"])
        self.assertEqual([], self.errors())

    def test_scene_declared_by_two_lanes_fails(self) -> None:
        self.add_lane("TestRHI210PassGoldenReal.cpp", ["PassA", "PassB"])
        self.add_lane("TestRHI210SceneGoldenReal.cpp", ["Canonical_Scene", "PassB"])
        errors = self.errors()
        self.assertEqual(1, len(errors), errors)
        self.assertIn(f"{self.ROW}/PassB: declared by more than one lane", errors[0])

    def test_lane_scene_missing_from_manifest_fails(self) -> None:
        self.add_lane("TestRHI210PassGoldenReal.cpp", ["PassA", "PassB", "PassC"])
        self.add_lane("TestRHI210SceneGoldenReal.cpp", ["Canonical_Scene"])
        errors = self.errors()
        self.assertEqual(1, len(errors), errors)
        self.assertIn(f"!= manifest scenes for {self.ROW}", errors[0])

    def test_manifest_scene_no_lane_declares_fails(self) -> None:
        self.add_lane("TestRHI210PassGoldenReal.cpp", ["PassA", "PassB"])
        errors = self.errors()
        self.assertEqual(1, len(errors), errors)
        self.assertIn(f"!= manifest scenes for {self.ROW}", errors[0])

    def test_unregistered_second_lane_fails(self) -> None:
        self.add_lane("TestRHI210PassGoldenReal.cpp", ["PassA", "PassB"])
        self.add_lane("TestRHI210SceneGoldenReal.cpp", ["Canonical_Scene"], registered=False)
        errors = self.errors()
        self.assertEqual(1, len(errors), errors)
        self.assertIn("TestRHI210SceneGoldenReal.cpp: golden lane is not registered", errors[0])


def copy_of(entry: dict[str, Any]) -> dict[str, Any]:
    return json.loads(json.dumps(entry))


if __name__ == "__main__":
    unittest.main()
