"""Validate authored MMO OBJ exports independently of Blender's importer.

Checks source geometry/material contracts, not renderer or artistic quality.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path


NAMES = (
    "alchemy_shop", "anvil", "banner", "cauldron", "chest", "forge", "fountain", "gate",
    "gravestone", "guild_hall", "market_stall", "pillar", "rock_large", "tent", "torch", "tree_pine",
)
# Immutable original source bounds at ff951c1, independently extracted before
# replacement. A candidate-generated manifest must not redefine gameplay scale.
SPECIAL_BOUNDS = {
    "cauldron": ((-.5, -.4, -.5), (.5, .4, .5)),
    "fountain": ((-1.5, -1, -1.5), (1.5, 1, 1.5)),
    "pillar": ((-.3, -1.5, -.3), (.3, 1.5, .3)),
    "tent": ((-1.5, 0, -1.5), (1.5, 2, 1.5)),
    "torch": ((-.1, -.5, -.1), (.1, .5, .1)),
    "tree_pine": ((-1, 0, -1), (1, 4, 1)),
}


SOURCE_HASHES = {
    "alchemy_shop": "6c68bff57d18e3e66e80095bca48bcecd6c764ab1e2dec56e9ade6078571f96a",
    "anvil": "6c68bff57d18e3e66e80095bca48bcecd6c764ab1e2dec56e9ade6078571f96a",
    "banner": "6c68bff57d18e3e66e80095bca48bcecd6c764ab1e2dec56e9ade6078571f96a",
    "cauldron": "305d6b4307d87decb11a0bbce795b60eb41ca3827d590bdd90249a4c5fdebc60",
    "chest": "6c68bff57d18e3e66e80095bca48bcecd6c764ab1e2dec56e9ade6078571f96a",
    "forge": "6c68bff57d18e3e66e80095bca48bcecd6c764ab1e2dec56e9ade6078571f96a",
    "fountain": "d76b3e92ed9e40a213372feb72ccf5321f979f3c7bf67c613430d11f0865fd6f",
    "gate": "6c68bff57d18e3e66e80095bca48bcecd6c764ab1e2dec56e9ade6078571f96a",
    "gravestone": "6c68bff57d18e3e66e80095bca48bcecd6c764ab1e2dec56e9ade6078571f96a",
    "guild_hall": "6c68bff57d18e3e66e80095bca48bcecd6c764ab1e2dec56e9ade6078571f96a",
    "market_stall": "6c68bff57d18e3e66e80095bca48bcecd6c764ab1e2dec56e9ade6078571f96a",
    "pillar": "b5002f382c18a8dc6df2f7d2f5601c91f7c2cf67928c6d0b9f1a7fcf5328d373",
    "rock_large": "6c68bff57d18e3e66e80095bca48bcecd6c764ab1e2dec56e9ade6078571f96a",
    "tent": "aa6e46fabcd9f827107d3ce57a774b54ef831cc4603a26fb0a7869ac1ca5d492",
    "torch": "7caa2906ea7a6b6b3f51cacdafddc8c394095cf6e0fd08ffe55d89a2d44c696e",
    "tree_pine": "74bfb70cd56f441b2c1ac94cd691aa6dd7c92c486f9f31e84458aaa630474f93"
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def values(fields, count, label):
    require(len(fields) >= count, f"Incomplete {label}")
    result = tuple(float(value) for value in fields[:count])
    require(all(math.isfinite(value) for value in result), f"Nonfinite {label}")
    return result


def read_materials(path):
    require(path.is_file(), f"Missing material library: {path.name}")
    materials = {}
    current = None
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        if fields[0] == "newmtl":
            require(len(fields) == 2 and fields[1] not in materials, "Invalid or duplicate material name")
            current = fields[1]
            materials[current] = None
        elif fields[0] == "Kd":
            require(current is not None, "Diffuse color precedes its material")
            color = values(fields[1:], 3, "diffuse color")
            require(all(0 <= channel <= 1 for channel in color), "Diffuse color outside [0,1]")
            materials[current] = color
        elif fields[0].lower().startswith("map_") or fields[0].lower() in {"bump", "disp", "decal", "refl"}:
            # This authored kit intentionally exports a self-contained Kd palette.
            # Unexpected texture dependencies require an explicit contract update.
            raise ValueError("Unexpected texture dependency in diffuse-color-only MMO kit")
    require(materials and all(color is not None for color in materials.values()), "Material lacks diffuse color")
    return materials


def validate_model(path):
    positions, normals, uvs, faces, libraries = [], [], [], [], []
    material = None
    for line in path.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if not fields or fields[0].startswith("#"):
            continue
        kind, data = fields[0], fields[1:]
        if kind == "v":
            positions.append(values(data, 3, "position"))
        elif kind == "vn":
            normal = values(data, 3, "normal")
            require(abs(sum(v * v for v in normal) - 1) <= 1e-3, "Normal is not unit length")
            normals.append(normal)
        elif kind == "vt":
            uv = values(data, 2, "UV")
            require(all(-1e-5 <= v <= 1 + 1e-5 for v in uv), "UV outside authored [0,1] range")
            uvs.append(uv)
        elif kind == "mtllib":
            libraries.extend(data)
        elif kind == "usemtl":
            require(len(data) == 1, "Invalid material assignment")
            material = data[0]
        elif kind == "f":
            require(len(data) == 3, "Export must contain triangles only")
            corners = []
            for corner in data:
                parts = corner.split("/")
                require(len(parts) == 3 and all(parts), "Face lacks authored UV or normal reference")
                indices = tuple(int(part) - 1 for part in parts)
                require(all(index >= 0 for index in indices), "Export must use positive OBJ indices")
                corners.append(indices)
            faces.append((corners, material))
    require(positions and normals and uvs and faces, "Empty geometry or missing authored attributes")
    require(len(faces) <= 5000, "Model exceeds 5000-triangle budget")
    require(libraries == [path.with_suffix(".mtl").name], "Unexpected material-library dependency")
    materials = read_materials(path.with_suffix(".mtl"))
    referenced = set()
    for corners, material in faces:
        require(material in materials, "Face references undefined material")
        for vertex, uv, normal in corners:
            require(vertex < len(positions) and uv < len(uvs) and normal < len(normals), "Invalid face index")
            referenced.add(vertex)
        a, b, c = (positions[corner[0]] for corner in corners)
        ab = tuple(b[i] - a[i] for i in range(3))
        ac = tuple(c[i] - a[i] for i in range(3))
        cross = (ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                 ab[0] * ac[1] - ab[1] * ac[0])
        magnitude = math.sqrt(sum(v * v for v in cross))
        require(magnitude > 1e-10, "Degenerate triangle")
        for _, _, normal in corners:
            require(sum(cross[i] * normals[normal][i] for i in range(3)) / magnitude > 1e-5,
                    "Triangle winding conflicts with authored normals")
    # Unreferenced extrema must not conceal a change in rendered/collision size.
    minimum = tuple(min(positions[index][axis] for index in referenced) for axis in range(3))
    maximum = tuple(max(positions[index][axis] for index in referenced) for axis in range(3))
    expected = SPECIAL_BOUNDS.get(path.stem, ((-.5, -.5, -.5), (.5, .5, .5)))
    require(all(abs(actual - original) <= 2e-5 for bounds, original_bounds in zip((minimum, maximum), expected)
                for actual, original in zip(bounds, original_bounds)), "Original gameplay bounds changed")
    return {"path": path.name, "vertices": len(positions), "triangles": len(faces),
            "materials": len(materials), "minimum": minimum, "maximum": maximum}


def validate_exports(root):
    directory = root / "Assets/Models/MMO"
    require({path.stem for path in directory.glob("*.obj")} == set(NAMES), "MMO model filename set changed")
    return [validate_model(directory / f"{name}.obj") for name in NAMES]


def validate_provenance(root, models):
    manifest = json.loads((root / "Art/Blender/MMO/provenance.json").read_text(encoding="utf-8"))
    require(manifest.get("schema_version") == 1, "Unknown provenance schema")
    require(manifest.get("blender_version") == "4.0.2", "Unexpected Blender authoring version")
    require(manifest.get("source_commit") == "ff951c19a5e12be6ec3724696af6fe030070e8c4",
            "Original asset commit changed")

    def check_file(record, expected_path):
        require(isinstance(record, dict) and record.get("path") == expected_path, "Provenance path changed")
        path = root / expected_path
        require(path.is_file() and not path.is_symlink(), "Provenance file is missing or a symlink")
        require(hashlib.sha256(path.read_bytes()).hexdigest() == record.get("sha256"),
                "Provenance hash mismatch: " + expected_path)

    check_file(manifest.get("author_script"), "tools/blender/author_mmo_props.py")
    check_file(manifest.get("blend"), "Art/Blender/MMO/props.blend")
    check_file(manifest.get("license"), "LICENSE")
    require(manifest["license"].get("name") == "Spark Open License 1.0", "License attribution changed")
    assets = manifest.get("assets")
    require(isinstance(assets, list) and len(assets) == len(NAMES), "Incomplete provenance model set")
    by_name = {}
    for asset in assets:
        require(isinstance(asset, dict) and asset.get("name") in NAMES and asset["name"] not in by_name,
                "Unknown or duplicate provenance model")
        by_name[asset["name"]] = asset
    for model in models:
        name = Path(model["path"]).stem
        asset = by_name[name]
        require(asset.get("source_before_sha256") == SOURCE_HASHES[name], "Original model hash changed")
        expected = SPECIAL_BOUNDS.get(name, ((-.5, -.5, -.5), (.5, .5, .5)))
        require(asset.get("original_bounds") == {"min": list(expected[0]), "max": list(expected[1])},
                "Original model bounds changed in provenance")
        require(asset.get("triangle_count") == model["triangles"], "Provenance triangle count mismatch")
        for extension in ("obj", "mtl"):
            path = f"Assets/Models/MMO/{name}.{extension}"
            check_file({"path": asset.get(extension + "_path"), "sha256": asset.get(extension + "_sha256")}, path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.root.resolve()
    models = validate_exports(root)
    validate_provenance(root, models)
    print(json.dumps({"models": models, "scope": "OBJ source contract and authored-file hashes"}, indent=2))


if __name__ == "__main__":
    main()
