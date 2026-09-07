"""Author the original repository-licensed static box fixture with Blender 4.0.2.

Run: blender --background --factory-startup --python-exit-code 1 --python author.py -- --output-dir PATH
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

import bpy

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output-dir", type=Path, default=Path(__file__).resolve().parent)
args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
if bpy.app.version != (4, 0, 2):
    raise RuntimeError("Canonical fixture authoring requires Blender 4.0.2")
output = args.output_dir.resolve()
output.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
mesh = bpy.data.meshes.new("AuthoredBoxMesh")
mesh.from_pydata(
    [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1),
     (-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)], [],
    [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
     (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)],
)
mesh.update()
box = bpy.data.objects.new("AuthoredBox", mesh)
bpy.context.collection.objects.link(box)
bpy.context.view_layer.objects.active = box
box.select_set(True)
box.location = (1, 2, 3)
box.scale = (1, 2, 3)
bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
uv = mesh.uv_layers.new(name="FixtureUV")
for face in mesh.polygons:
    face.use_smooth = False
    for loop, coord in zip(face.loop_indices, [(0.125, 0.25), (0.875, 0.25), (0.875, 0.625), (0.125, 0.625)]):
        uv.data[loop].uv = coord
mesh.update()
bpy.context.preferences.filepaths.save_version = 0
blend = output / "authored_box.blend"
glb = output / "authored_box.glb"
bpy.ops.wm.save_as_mainfile(filepath=str(blend), compress=True)
export_options = dict(
    export_format="GLB", use_selection=True, export_apply=True, export_yup=True,
    export_normals=True, export_texcoords=True, export_tangents=False, export_colors=False,
    export_attributes=False, export_materials="NONE", export_animations=False, export_skins=False,
    export_morph=False, export_cameras=False, export_lights=False, export_extras=False,
    export_draco_mesh_compression_enable=False,
)
bpy.ops.export_scene.gltf(filepath=str(glb), **export_options)
manifest = {
    "authorship": "Original procedural fixture authored for SparkEngine with Blender; no third-party assets",
    "license": "Spark Open License 1.0", "license_file": "../../../../LICENSE", "blender_version": bpy.app.version_string,
    "blender_dimensions": [2, 4, 6], "blender_center": [1, 2, 3],
    "gltf_aabb_min": [0, 0, -4], "gltf_aabb_max": [2, 6, 0],
    "vertices": 24, "triangles": 12, "export_options": export_options,
    "sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
               for path in (Path(__file__).resolve(), blend, glb)},
}
(output / "provenance.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
