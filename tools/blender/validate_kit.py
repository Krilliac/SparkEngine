"""Validate a per-module Blender kit from its provenance.json (stdlib only).

Checks the recorded source/export contract written by tools/blender/spark_kit.py:
hashes of every recorded file, OBJ geometry attributes, bounds, triangle budgets,
LOD1 reduction, collision caps and MTL material resolution. It does not judge art
quality or engine rendering.

Usage: python3 tools/blender/validate_kit.py Art/Blender/<Module>/provenance.json [--root .]
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

BOUNDS_TOLERANCE = 1e-5


def require(condition, message):
    if not condition:
        raise ValueError(message)


def check_file(root, record, label):
    """A recorded {path, sha256} pair must name a regular file inside root with that hash."""
    require(isinstance(record, dict), f"{label}: missing record")
    relative = record.get("path")
    require(isinstance(relative, str) and relative and not Path(relative).is_absolute()
            and ".." not in Path(relative).parts, f"{label}: invalid path {relative!r}")
    path = root / relative
    require(path.is_file() and not path.is_symlink(), f"{label}: missing file {relative}")
    require(hashlib.sha256(path.read_bytes()).hexdigest() == record.get("sha256"),
            f"{label}: sha256 mismatch for {relative}")
    return path


def read_material_names(path):
    require(path.is_file(), f"Missing material library: {path.name}")
    names = set()
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if fields and fields[0] == "newmtl":
            require(len(fields) == 2, f"{path.name}: invalid newmtl line")
            names.add(fields[1])
    require(names, f"{path.name}: defines no materials")
    return names


def parse_obj(path):
    """Return triangle count, bounds and material usage of a triangulated OBJ with UVs and normals."""
    counts = {"v": 0, "vt": 0, "vn": 0}
    minimum, maximum = [math.inf] * 3, [-math.inf] * 3
    libraries, used, triangles = [], set(), 0
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        kind, data = fields[0], fields[1:]
        if kind in counts:
            size = 2 if kind == "vt" else 3
            require(len(data) >= size, f"{path.name}:{number}: incomplete {kind}")
            values = [float(value) for value in data[:size]]
            require(all(math.isfinite(value) for value in values), f"{path.name}:{number}: nonfinite {kind}")
            counts[kind] += 1
            if kind == "v":
                minimum = [min(a, b) for a, b in zip(minimum, values)]
                maximum = [max(a, b) for a, b in zip(maximum, values)]
        elif kind == "mtllib":
            libraries.extend(data)
        elif kind == "usemtl":
            require(len(data) == 1, f"{path.name}:{number}: invalid usemtl")
            used.add(data[0])
        elif kind == "f":
            require(len(data) == 3, f"{path.name}:{number}: face is not a triangle")
            for corner in data:
                parts = corner.split("/")
                require(len(parts) >= 2 and parts[1], f"{path.name}:{number}: face lacks a UV reference")
                require(len(parts) == 3 and parts[2], f"{path.name}:{number}: face lacks a normal reference")
                for part, kind_name in zip(parts, ("v", "vt", "vn")):
                    index = int(part)
                    require(1 <= index <= counts[kind_name], f"{path.name}:{number}: {kind_name} index out of range")
            triangles += 1
    require(triangles > 0, f"{path.name}: no faces")
    require(counts["vn"] > 0, f"{path.name}: no normals")
    require(counts["vt"] > 0, f"{path.name}: no UVs")
    require(libraries == [path.with_suffix(".mtl").name], f"{path.name}: unexpected mtllib {libraries}")
    undefined = used - read_material_names(path.with_suffix(".mtl"))
    require(not undefined, f"{path.name}: materials missing from MTL: {sorted(undefined)}")
    return triangles, {"min": minimum, "max": maximum}


def check_mesh(root, record, label):
    """Hash-check one exported mesh and compare its geometry to the record; returns its triangle count."""
    obj = check_file(root, {"path": record.get("obj_path"), "sha256": record.get("obj_sha256")}, label + " obj")
    mtl = check_file(root, {"path": record.get("mtl_path"), "sha256": record.get("mtl_sha256")}, label + " mtl")
    require(mtl == obj.with_suffix(".mtl"), f"{label}: MTL is not beside its OBJ")
    triangles, bounds = parse_obj(obj)
    require(record.get("triangle_count") == triangles,
            f"{label}: recorded triangle_count {record.get('triangle_count')} != {triangles}")
    recorded = record.get("bounds") or {}
    for key in ("min", "max"):
        values = recorded.get(key)
        require(isinstance(values, list) and len(values) == 3 and all(math.isfinite(float(v)) for v in values),
                f"{label}: invalid recorded bounds")
        require(all(abs(float(a) - b) <= BOUNDS_TOLERANCE for a, b in zip(values, bounds[key])),
                f"{label}: bounds differ from record")
    return triangles


def validate_kit(provenance_path, root=None):
    provenance_path = Path(provenance_path).resolve()
    # Art/Blender/<Module>/provenance.json -> repository root
    root = Path(root).resolve() if root else provenance_path.parents[3]
    manifest = json.loads(provenance_path.read_text(encoding="utf-8"))
    require(manifest.get("schema_version") == 1, "Unknown provenance schema")
    for key in ("author_script", "library", "blend", "license"):
        check_file(root, manifest.get(key), key)
    require(manifest["license"].get("name") == "Spark Open License 1.0", "Unexpected license name")
    assets = manifest.get("assets")
    require(isinstance(assets, list) and assets, "No assets recorded")
    names = [asset.get("name") for asset in assets]
    require(len(set(names)) == len(names), "Duplicate asset names")

    summary = []
    for asset in assets:
        name = asset["name"]
        triangles = check_mesh(root, asset, name)
        budget = asset.get("triangle_budget")
        require(isinstance(budget, int) and triangles <= budget, f"{name}: {triangles} triangles exceed budget {budget}")
        variants = asset.get("variants") or {}
        require({"lod1", "collision"} <= set(variants), f"{name}: missing lod1/collision variants")
        lod = check_mesh(root, variants["lod1"], name + "_lod1")
        require(lod < triangles, f"{name}: LOD1 ({lod}) is not smaller than the source ({triangles})")
        collision = check_mesh(root, variants["collision"], name + "_collision")
        cap = variants["collision"].get("triangle_cap")
        require(isinstance(cap, int) and collision <= cap, f"{name}: collision {collision} triangles exceed cap {cap}")
        summary.append({"name": name, "triangles": triangles, "lod1": lod, "collision": collision})
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("provenance", type=Path)
    parser.add_argument("--root", type=Path, help="repository root (default: derived from the provenance path)")
    args = parser.parse_args()
    print(json.dumps({"assets": validate_kit(args.provenance, args.root)}, indent=2))


if __name__ == "__main__":
    main()
