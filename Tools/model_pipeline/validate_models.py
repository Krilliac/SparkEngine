#!/usr/bin/env python3
"""Validate deterministic starter OBJ/MTL outputs without third-party modules."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import subprocess
from collections import defaultdict
from pathlib import Path


PACK_TEMPLATES = {
    "fps_starter": "FPSStarter",
    "mmo_starter": "MMOStarter",
    "multiplayer_arena": "MultiplayerArena",
    "platformer_kit": "PlatformerKit",
    "rpg_starter": "RPGStarter",
    "third_person_starter": "ThirdPersonStarter",
    "top_down_starter": "TopDownStarter",
}
MAX_ASSET_BYTES = 64 * 1024 * 1024
MODULE_PACK_DIRECTORIES = {
    "module_arpg": "ARPG",
    "module_racing": "Racing",
    "module_rts": "RTS",
    "module_openworld": "OpenWorld",
}
MODULE_GROUND_TOLERANCE_METERS = 0.005


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def confined_file(base: Path, value: str, *, label: str) -> Path:
    relative = Path(value)
    if relative.is_absolute():
        raise ValueError(f"{label}: absolute path is not allowed: {value}")
    resolved_base = base.resolve()
    resolved = (resolved_base / relative).resolve()
    if not resolved.is_relative_to(resolved_base):
        raise ValueError(f"{label}: path escapes its root: {value}")
    if not resolved.is_file():
        raise ValueError(f"{label}: missing regular file: {value}")
    size = resolved.stat().st_size
    if size <= 0 or size > MAX_ASSET_BYTES:
        raise ValueError(f"{label}: unreasonable file size ({size} bytes): {value}")
    return resolved


def tracked_files(root: Path) -> set[str] | None:
    """Return the Git index surface, or None for an exported source archive."""
    result = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        check=False,
        capture_output=True,
    )
    if result.returncode != 0:
        return None
    return {path.decode("utf-8").replace("\\", "/") for path in result.stdout.split(b"\0") if path}


def require_index_match(root: Path, path: Path, tracked: set[str] | None, *, label: str) -> None:
    """Ensure validation evidence describes the proposed Git snapshot, not only the worktree."""
    if tracked is None:
        return
    relative = path.relative_to(root).as_posix()
    if relative not in tracked:
        raise ValueError(f"{label} is absent from the Git index: {relative}")
    result = subprocess.run(
        ["git", "-C", str(root), "diff", "--quiet", "--", relative],
        check=False,
        capture_output=True,
    )
    if result.returncode == 1:
        raise ValueError(f"{label} has unstaged content and does not match the Git index: {relative}")
    if result.returncode != 0:
        raise ValueError(f"could not compare {label} with the Git index: {relative}")


def parse_float_fields(path: Path, line_number: int, fields: list[str], count: int) -> tuple[float, ...]:
    if len(fields) != count + 1:
        raise ValueError(f"{path}:{line_number}: expected {count} numeric values")
    values = tuple(float(value) for value in fields[1:])
    if not all(math.isfinite(value) for value in values):
        raise ValueError(f"{path}:{line_number}: non-finite numeric value")
    return values


def resolve_obj_index(token: str, count: int, path: Path, line_number: int, kind: str) -> int:
    try:
        index = int(token)
    except ValueError as error:
        raise ValueError(f"{path}:{line_number}: invalid {kind} index {token!r}") from error
    if index == 0:
        raise ValueError(f"{path}:{line_number}: OBJ {kind} index cannot be zero")
    resolved = index - 1 if index > 0 else count + index
    if resolved < 0 or resolved >= count:
        raise ValueError(f"{path}:{line_number}: {kind} index {index} is out of range")
    return resolved


def validate_mtl(path: Path) -> dict[str, tuple[float, float, float]]:
    materials: dict[str, tuple[float, float, float]] = {}
    current: str | None = None
    with path.open("r", encoding="utf-8", errors="strict") as stream:
        for line_number, raw in enumerate(stream, 1):
            fields = raw.strip().split()
            if not fields or fields[0].startswith("#"):
                continue
            if fields[0] == "newmtl":
                if len(fields) < 2:
                    raise ValueError(f"{path}:{line_number}: empty material name")
                current = " ".join(fields[1:])
                if current in materials:
                    raise ValueError(f"{path}:{line_number}: duplicate material {current!r}")
                materials[current] = (-1.0, -1.0, -1.0)
            elif fields[0] == "Kd":
                if current is None:
                    raise ValueError(f"{path}:{line_number}: Kd appears before newmtl")
                color = parse_float_fields(path, line_number, fields, 3)
                if any(value < 0.0 for value in color):
                    raise ValueError(f"{path}:{line_number}: negative diffuse color")
                materials[current] = color
    if not materials:
        raise ValueError(f"{path}: material library is empty")
    missing_colors = [name for name, color in materials.items() if color[0] < 0.0]
    if missing_colors:
        raise ValueError(f"{path}: material(s) missing Kd: {', '.join(missing_colors)}")
    return materials


def validate_obj(path: Path) -> dict[str, object]:
    vertices: list[tuple[float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    texcoords: list[tuple[float, ...]] = []
    triangles = 0
    materials: set[str] = set()
    mtllibs: list[str] = []

    with path.open("r", encoding="utf-8", errors="strict") as stream:
        for line_number, raw in enumerate(stream, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if fields[0] == "v":
                vertices.append(parse_float_fields(path, line_number, fields, 3))
            elif fields[0] == "vn":
                normal = parse_float_fields(path, line_number, fields, 3)
                if math.sqrt(sum(value * value for value in normal)) < 1e-8:
                    raise ValueError(f"{path}:{line_number}: zero-length normal")
                normals.append(normal)
            elif fields[0] == "vt":
                if len(fields) not in (3, 4):
                    raise ValueError(f"{path}:{line_number}: malformed texture coordinate")
                values = tuple(float(value) for value in fields[1:])
                if not all(math.isfinite(value) for value in values):
                    raise ValueError(f"{path}:{line_number}: non-finite texture coordinate")
                texcoords.append(values)
            elif fields[0] == "f":
                if len(fields) != 4:
                    raise ValueError(f"{path}:{line_number}: face is not triangulated")
                vertex_indices: list[int] = []
                for corner in fields[1:]:
                    parts = corner.split("/")
                    if len(parts) != 3 or not parts[0] or not parts[2]:
                        raise ValueError(f"{path}:{line_number}: face must include vertex and authored normal")
                    vertex_indices.append(resolve_obj_index(parts[0], len(vertices), path, line_number, "vertex"))
                    if parts[1]:
                        resolve_obj_index(parts[1], len(texcoords), path, line_number, "texture-coordinate")
                    resolve_obj_index(parts[2], len(normals), path, line_number, "normal")
                if len(set(vertex_indices)) != 3:
                    raise ValueError(f"{path}:{line_number}: degenerate triangle indices")
                a, b, c = (vertices[index] for index in vertex_indices)
                ab = tuple(b[i] - a[i] for i in range(3))
                ac = tuple(c[i] - a[i] for i in range(3))
                cross = (ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                         ab[0] * ac[1] - ab[1] * ac[0])
                if sum(value * value for value in cross) < 1e-16:
                    raise ValueError(f"{path}:{line_number}: zero-area triangle")
                triangles += 1
            elif fields[0] == "usemtl" and len(fields) > 1:
                materials.add(" ".join(fields[1:]))
            elif fields[0] == "mtllib" and len(fields) > 1:
                mtllibs.append(" ".join(fields[1:]))

    if not vertices or not triangles:
        raise ValueError(f"{path}: mesh is empty")
    if not normals or not materials or len(mtllibs) != 1:
        raise ValueError(f"{path}: expected normals, material assignments, and exactly one material library")
    mtl_path = confined_file(path.parent, mtllibs[0], label=str(path))
    declared_materials = validate_mtl(mtl_path)
    undefined = sorted(materials - declared_materials.keys())
    if undefined:
        raise ValueError(f"{path}: undefined material assignment(s): {', '.join(undefined)}")

    minimum = [min(vertex[axis] for vertex in vertices) for axis in range(3)]
    maximum = [max(vertex[axis] for vertex in vertices) for axis in range(3)]
    if minimum[1] < -0.001:
        raise ValueError(f"{path}: ground pivot contract violated (min Y {minimum[1]:.6f})")
    return {
        "vertices": len(vertices),
        "triangles": triangles,
        "normals": len(normals),
        "texcoords": len(texcoords),
        "materials": sorted(materials),
        "bounds": {"min": minimum, "max": maximum},
        "sha256": sha256(path),
        "mtlSha256": sha256(mtl_path),
    }


def validate_template_surfaces(
    root: Path, manifest: dict[str, object], tracked: set[str] | None
) -> tuple[int, list[str]]:
    failures: list[str] = []
    scene_references = 0
    lock_path = root / "Templates" / "assets.lock.json"
    require_index_match(root, lock_path, tracked, label="template asset lock")
    lock = json.loads(lock_path.read_text(encoding="utf-8"))["assets"]
    packs = {entry["pack"] for entry in manifest["assets"]}
    for pack, template_name in PACK_TEMPLATES.items():
        if pack not in packs:
            failures.append(f"generated manifest has no assets for {pack}")
            continue
        template_root = root / "Templates" / template_name
        asset_manifest_path = template_root / "Assets" / "manifest.json"
        require_index_match(root, asset_manifest_path, tracked, label=f"{template_name} asset manifest")
        asset_manifest = json.loads(asset_manifest_path.read_text(encoding="utf-8"))
        declared = {entry["path"]: entry["sha256"] for entry in asset_manifest["assets"]}
        for scene_path in sorted((template_root / "Scenes").glob("*.sparkscene")):
            require_index_match(root, scene_path, tracked, label=f"{template_name} scene")
            scene = json.loads(scene_path.read_text(encoding="utf-8"))
            for entity in scene.get("entities", []):
                for component in entity.get("components", []):
                    if component.get("type") != "MeshRenderer":
                        continue
                    mesh_path = str(component.get("fields", {}).get("meshPath", ""))
                    if not mesh_path or mesh_path.startswith("__spark_primitive_"):
                        continue
                    scene_references += 1
                    try:
                        resolved = confined_file(template_root, mesh_path, label=str(scene_path))
                        relative_asset = resolved.relative_to(template_root / "Assets").as_posix()
                        mtl_asset = str(Path(relative_asset).with_suffix(".mtl")).replace("\\", "/")
                        for asset_key in (relative_asset, mtl_asset):
                            asset = confined_file(template_root / "Assets", asset_key, label=str(scene_path))
                            repo_relative = asset.relative_to(root).as_posix()
                            require_index_match(root, asset, tracked, label=str(scene_path))
                            actual_hash = sha256(asset)
                            if declared.get(asset_key) != actual_hash:
                                raise ValueError(f"{scene_path}: undeclared or stale manifest asset {asset_key}")
                            lock_key = f"{template_name}/Assets/{asset_key}"
                            if lock.get(lock_key) != actual_hash:
                                raise ValueError(f"{scene_path}: unlocked or stale asset {lock_key}")
                    except (OSError, ValueError) as error:
                        failures.append(str(error))
    return scene_references, failures


def module_variant(name: str) -> tuple[str, str]:
    if name.endswith("_collision"):
        return name.removesuffix("_collision"), "collision"
    if name.endswith("_lod1"):
        return name.removesuffix("_lod1"), "lod1"
    return name, "lod0"


def validate_module_kits(
    root: Path, manifest: dict[str, object], validated: dict[str, dict[str, object]]
) -> list[str]:
    """Validate module-kit naming, coverage, pivot, and simplification contracts."""
    failures: list[str] = []
    assets = manifest.get("assets")
    if not isinstance(assets, list):
        return ["manifest assets must be a list"]

    module_entries = [entry for entry in assets if str(entry.get("pack", "")).startswith("module_")]
    declared_paths = [str(entry.get("path", "")) for entry in module_entries]
    duplicate_paths = sorted(path for path in set(declared_paths) if declared_paths.count(path) > 1)
    if duplicate_paths:
        failures.append(f"duplicate module manifest path(s): {', '.join(duplicate_paths)}")

    expected_previews = {
        f"docs/images/model-pipeline/{pack}.png" for pack in MODULE_PACK_DIRECTORIES
    }
    declared_previews = set(manifest.get("previews", []))
    missing_previews = sorted(expected_previews - declared_previews)
    if missing_previews:
        failures.append(f"module preview(s) absent from manifest: {', '.join(missing_previews)}")

    declared_module_files: set[str] = set()
    for pack, directory_name in MODULE_PACK_DIRECTORIES.items():
        pack_entries = [entry for entry in module_entries if entry.get("pack") == pack]
        if not pack_entries:
            failures.append(f"generated manifest has no assets for {pack}")
            continue

        expected_directory = root / "Assets" / "Models" / "ModuleKits" / directory_name
        groups: dict[str, dict[str, tuple[dict[str, object], dict[str, object]]]] = defaultdict(dict)
        for entry in pack_entries:
            name = str(entry.get("name", ""))
            relative_path = str(entry.get("path", ""))
            expected_path = (expected_directory / f"{name}.obj").relative_to(root).as_posix()
            if relative_path != expected_path:
                failures.append(
                    f"module asset path does not match its pack/name: {relative_path} (expected {expected_path})"
                )
            declared_module_files.add(relative_path)
            declared_module_files.add(str(Path(relative_path).with_suffix(".mtl")).replace("\\", "/"))
            base, variant = module_variant(name)
            if variant in groups[base]:
                failures.append(f"duplicate {pack}/{base} {variant} variant")
                continue
            actual = validated.get(relative_path)
            if actual is not None:
                groups[base][variant] = (entry, actual)

        for base, variants in sorted(groups.items()):
            missing = sorted({"lod0", "lod1", "collision"} - variants.keys())
            if missing:
                failures.append(f"{pack}/{base} missing variant(s): {', '.join(missing)}")
                continue
            lod0 = variants["lod0"][1]
            lod1 = variants["lod1"][1]
            collision = variants["collision"][1]
            if int(lod1["triangles"]) >= int(lod0["triangles"]):
                failures.append(f"{pack}/{base}: LOD1 is not simpler than LOD0")
            if int(collision["triangles"]) >= int(lod1["triangles"]):
                failures.append(f"{pack}/{base}: collision proxy is not simpler than LOD1")
            for variant, (_, actual) in variants.items():
                minimum_y = float(actual["bounds"]["min"][1])
                if abs(minimum_y) > MODULE_GROUND_TOLERANCE_METERS:
                    failures.append(
                        f"{pack}/{base} {variant}: ground pivot is {minimum_y:.6f} m "
                        f"(tolerance {MODULE_GROUND_TOLERANCE_METERS:.3f} m)"
                    )

    module_root = root / "Assets" / "Models" / "ModuleKits"
    actual_module_files = {
        path.relative_to(root).as_posix()
        for path in module_root.rglob("*")
        if path.is_file() and path.suffix.lower() in {".obj", ".mtl"}
    }
    undeclared = sorted(actual_module_files - declared_module_files)
    missing = sorted(declared_module_files - actual_module_files)
    if undeclared:
        failures.append(f"module model file(s) absent from manifest: {', '.join(undeclared)}")
    if missing:
        failures.append(f"manifest module model file(s) missing on disk: {', '.join(missing)}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument(
        "--allow-worktree",
        action="store_true",
        help="validate working-tree outputs without requiring them to match the Git index",
    )
    arguments = parser.parse_args()
    root = arguments.repo_root.resolve()
    try:
        manifest_path = confined_file(
            root, "Assets/Models/Generated/starter_assets_manifest.json", label="manifest"
        )
        tracked = None if arguments.allow_worktree else tracked_files(root)
        require_index_match(root, manifest_path, tracked, label="generated manifest")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        print(f"ERROR: {error}")
        print("FAILED: manifest could not be loaded for validation")
        return 1

    failures: list[str] = []
    checked = 0
    validated: dict[str, dict[str, object]] = {}
    for entry in manifest["assets"]:
        try:
            path = confined_file(root, entry["path"], label="generated asset")
            for generated_path in (path, path.with_suffix(".mtl")):
                require_index_match(root, generated_path, tracked, label="generated asset")
            actual = validate_obj(path)
            for field in ("sha256", "mtlSha256", "bounds"):
                if actual[field] != entry.get(field):
                    raise ValueError(f"manifest {field} mismatch for {entry['path']}")
            validated[entry["path"]] = actual
            checked += 1
        except (OSError, UnicodeError, ValueError) as error:
            failures.append(str(error))

    for preview in manifest["previews"]:
        try:
            preview_path = confined_file(root, preview, label="preview")
            require_index_match(root, preview_path, tracked, label="preview")
            data = preview_path.read_bytes()
            if len(data) < 1024 or not data.startswith(b"\x89PNG\r\n\x1a\n"):
                raise ValueError(f"preview is not a nontrivial PNG: {preview}")
        except (OSError, ValueError) as error:
            failures.append(str(error))

    try:
        scene_references, scene_failures = validate_template_surfaces(root, manifest, tracked)
        failures.extend(scene_failures)
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError) as error:
        scene_references = 0
        failures.append(str(error))

    failures.extend(validate_module_kits(root, manifest, validated))

    if failures:
        for failure in failures:
            print(f"ERROR: {failure}")
        print(f"FAILED: {len(failures)} issue(s), {checked} model(s) checked")
        return 1
    print(
        f"PASS: {checked} triangulated OBJ/MTL models, {len(manifest['previews'])} preview renders, "
        f"and {scene_references} confined scene references"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
