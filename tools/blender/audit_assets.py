"""Inspect shipped visual assets with Blender without modifying their sources.

blender --background --factory-startup --python-exit-code 1 --python audit_assets.py -- --root REPO --output REPORT
"""
import argparse
from array import array
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import sys
import wave

import bmesh
import bpy


def inspect_model(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.wm.obj_import(filepath=str(path), forward_axis="NEGATIVE_Z", up_axis="Y")
    objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    counts = Counter()
    for obj in objects:
        mesh = obj.data
        mesh.calc_loop_triangles()
        counts.update(vertices=len(mesh.vertices), triangles=len(mesh.loop_triangles),
                      polygons=len(mesh.polygons), uv_layers=len(mesh.uv_layers))
        counts["near_zero_area_faces"] += sum(face.area <= 1e-12 for face in mesh.polygons)
        counts["nonfinite_vertices"] += sum(not all(math.isfinite(v) for v in vertex.co)
                                            for vertex in mesh.vertices)
        topology = bmesh.new()
        topology.from_mesh(mesh)
        counts["boundary_edges"] += sum(edge.is_boundary for edge in topology.edges)
        counts["nonmanifold_edges"] += sum(not edge.is_manifold for edge in topology.edges)
        topology.free()
    missing_images = [bpy.path.abspath(image.filepath) for image in bpy.data.images
                      if image.source == "FILE" and not Path(bpy.path.abspath(image.filepath)).is_file()]
    return {"objects": len(objects), **dict(counts), "has_geometry": bool(counts["triangles"]),
            "face_area_threshold": 1e-12, "missing_images": sorted(missing_images),
            "materials": sorted(material.name for material in bpy.data.materials)}


def inspect_image(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        width, height = image.size
        channels = image.channels
        samples = set()
        if width and height and channels:
            # RNA indexed pixel access copies the full buffer on each lookup.
            # Read it once so large atlases do not multiply that cost 256 times.
            pixels = array("f", [0.0]) * (width * height * channels)
            image.pixels.foreach_get(pixels)
            for y in range(8):
                for x in range(8):
                    offset = ((y * (height - 1) // 7) * width + x * (width - 1) // 7) * channels
                    samples.add(tuple(round(pixels[offset + c], 5) for c in range(channels)))
        return {"width": width, "height": height, "channels": channels,
                "sampled_distinct_colors": len(samples)}
    finally:
        bpy.data.images.remove(image)


def inspect_audio(path):
    sound = bpy.data.sounds.load(str(path), check_existing=False)
    try:
        with wave.open(str(path), "rb") as source:
            return {"channels": source.getnchannels(), "sample_rate": source.getframerate(),
                    "sample_width": source.getsampwidth(), "frames": source.getnframes()}
    finally:
        bpy.data.sounds.remove(sound)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    root = args.root.resolve()
    handlers = {".obj": inspect_model, ".png": inspect_image, ".wav": inspect_audio}
    entries = []
    other_files = []
    other_extension_counts = Counter()
    other_media = {".svg", ".gif", ".mp4", ".webm", ".glb", ".gltf", ".fbx", ".blend",
                   ".jpg", ".jpeg", ".tga", ".dds", ".ogg", ".mp3", ".exr", ".hdr", ".ktx"}
    for directory in ("Assets", "Templates", "GameModules"):
        for path in sorted((root / directory).rglob("*")):
            if not path.is_file():
                continue
            if path.suffix.lower() not in {*handlers, ".mtl"}:
                other_extension_counts[path.suffix.lower()] += 1
                if path.suffix.lower() in other_media:
                    other_files.append({"path": path.relative_to(root).as_posix(),
                                        "extension": path.suffix.lower(), "bytes": path.stat().st_size})
                continue
            row = {"path": path.relative_to(root).as_posix(), "bytes": path.stat().st_size,
                   "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            try:
                if path.suffix.lower() == ".mtl":
                    row["material_definitions"] = sum(line.startswith("newmtl ")
                                                      for line in path.read_text().splitlines())
                else:
                    row.update(handlers[path.suffix.lower()](path))
                row["inspected"] = True
            except Exception as error:
                row.update(inspected=False, error=str(error))
            entries.append(row)
            print("SPARK_ASSET_AUDIT", row["path"], row["inspected"], flush=True)
    report = {"schema_version": 1, "blender_version": bpy.app.version_string,
              "scope": ["Assets", "Templates", "GameModules"],
              "inspected_extensions": sorted([*handlers, ".mtl"]),
              "other_files": other_files,
              "uninspected_extension_counts": dict(sorted(other_extension_counts.items())),
              "note": "Inspection is not engine rendering, artistic approval, or release qualification. "
                      "Open meshes and flat textures can be intentional; review before changing them. "
                      "Blender can synthesize missing attributes; source correctness needs separate validation. "
                      "Other files are inventoried but not inspected by this script.",
              "entries": entries}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if any(not entry["inspected"] for entry in entries):
        raise RuntimeError("One or more assets failed inspection; see the report")


if __name__ == "__main__":
    main()
