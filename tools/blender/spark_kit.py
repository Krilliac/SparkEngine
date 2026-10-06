"""Shared Blender 4.0.2 helpers for per-module asset kits; Spark Open License 1.0.

Per-module authoring scripts (tools/blender/author_<module>_kit.py) put this
directory on sys.path, build their props from the primitives below, and let the
Kit export each prop as <asset>.obj, <asset>_lod1.obj and <asset>_collision.obj
under Assets/Models/<Short>/Kit/ plus Art/Blender/<Module>/provenance.json.

Conventions follow Assets/Models/ModuleKits (Tools/model_pipeline/generate_starter_models.py,
"left-handed, Y-up, +Z-forward"): meters, ground-level pivot (z = 0 in Blender), author Z-up
with the prop's front facing Blender -Y, export forward_axis='Z', up_axis='Y'. Blender (x, y, z)
lands in the OBJ as (-x, z, y), so the front faces -Z, toward an engine camera looking down +Z.
OBJStaticMeshLoader only flips V. (The MMO props use forward_axis='NEGATIVE_Z', which is the
same kit turned 180 degrees about Y.) Triangles stay counter-clockwise around their normals.

Output is byte-deterministic: no randomness here (scripts seed their own random.Random),
fixed authoring order, canonicalized OBJ UV/face order and no timestamps. Blender 4.0 embeds
memory addresses in .blend files, so save_blend() keeps an existing source whose recorded
content digest (provenance blend.scene_sha256) is unchanged instead of rewriting it.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import sys

import bmesh
import bpy
from mathutils import Euler, Matrix, Vector

BLENDER_VERSION = "4.0.2"
LICENSE_NAME = "Spark Open License 1.0"
EXPORT_AXES = {"forward_axis": "Z", "up_axis": "Y"}
COLLISION_COLOR = ((1.0, 1.0, 1.0), 1.0, 0.0)


def parse_args():
    """Parse the arguments after Blender's '--' separator (only --repo)."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    return parser.parse_args(argv)


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def reset_scene():
    """Empty the factory scene so every run starts from identical data-blocks."""
    if bpy.app.version_string != BLENDER_VERSION:
        raise RuntimeError(f"Kit authoring is pinned to Blender {BLENDER_VERSION}")
    bpy.context.preferences.filepaths.save_version = 0
    for collection in (bpy.data.objects, bpy.data.meshes, bpy.data.materials, bpy.data.cameras,
                       bpy.data.lights, bpy.data.collections, bpy.data.images):
        for block in list(collection):
            collection.remove(block)
    bpy.context.scene.unit_settings.system = "METRIC"
    bpy.context.scene.unit_settings.scale_length = 1.0


def obj_stats(path):
    """Triangle count and vertex bounds of an exported (triangulated) OBJ."""
    positions, triangles = [], 0
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        if line.startswith("v "):
            positions.append([float(value) for value in line.split()[1:4]])
        elif line.startswith("f "):
            triangles += 1
    bounds = {"min": [min(p[axis] for p in positions) for axis in range(3)],
              "max": [max(p[axis] for p in positions) for axis in range(3)]}
    return triangles, bounds


def canonicalize_obj(path):
    """Sort/deduplicate UVs and sort faces per block; geometry is unchanged.

    Blender may emit the same UV loops and polygons in a different order between runs,
    the same reason generate_starter_models.canonicalize_obj exists.
    """
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    emitted = []
    for line in lines:
        if line.startswith("vt "):
            values = [0.0 if abs(float(token)) < 0.000005 else float(token) for token in line.split()[1:]]
            emitted.append("vt " + " ".join(f"{value:.5f}" for value in values))
    canonical = sorted(set(emitted), key=lambda uv: (tuple(float(v) for v in uv.split()[1:]), uv))
    new_index = {uv: index + 1 for index, uv in enumerate(canonical)}
    remap = [new_index[uv] for uv in emitted]

    output, faces, uvs_written = [], [], False

    def flush_faces():
        output.extend(sorted(faces))
        faces.clear()

    for line in lines:
        if line.startswith("vt "):
            if not uvs_written:
                output.extend(canonical)
                uvs_written = True
            continue
        if line.startswith("f "):
            corners = []
            for corner in line.split()[1:]:
                parts = corner.split("/")
                if len(parts) >= 2 and parts[1]:
                    parts[1] = str(remap[int(parts[1]) - 1])
                corners.append("/".join(parts))
            faces.append("f " + " ".join(corners))
            continue
        flush_faces()
        output.append(line)
    flush_faces()
    Path(path).write_text("\n".join(output) + "\n", encoding="utf-8", newline="\n")


class Kit:
    """One module's prop kit: palette materials, primitive builders, exports and provenance.

    Builders add editable parts to the prop in progress; export_asset() consumes them.
    palette maps a name to ((r, g, b), roughness, metallic); material names become
    '<Short>_<name>' in the MTL files.
    """

    def __init__(self, repo, module, script, palette, short=None):
        reset_scene()
        self.repo = Path(repo).resolve()
        self.module = module
        self.short = short or module
        self.script = Path(script).resolve()
        self.palette = dict(palette)
        self.art_dir = self.repo / "Art/Blender" / module
        self.export_dir = self.repo / "Assets/Models" / self.short / "Kit"
        self.blend_path = self.art_dir / f"{module.lower()}_kit.blend"
        self.records = []
        self._materials = {}
        self._parts = []
        self._scene_sha256 = None

    # ---- materials ------------------------------------------------------------------

    def material(self, name):
        """Principled material for a palette entry (created once, named '<Short>_<name>')."""
        if name not in self._materials:
            color, roughness, metallic = COLLISION_COLOR if name == "collision" else self.palette[name]
            mat = bpy.data.materials.new(f"{self.short}_{name}")
            mat.diffuse_color = (*color, 1.0)
            mat.use_nodes = True
            node = mat.node_tree.nodes.get("Principled BSDF")
            node.inputs["Base Color"].default_value = (*color, 1.0)
            node.inputs["Roughness"].default_value = roughness
            node.inputs["Metallic"].default_value = metallic
            self._materials[name] = mat
        return self._materials[name]

    # ---- primitives (all dimensions in meters; location/rotation are object transforms) ----

    def _add(self, name, bm, mat, location, rotation, smooth_quads=False):
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        data = bpy.data.meshes.new(name)
        bm.to_mesh(data)
        bm.free()
        for polygon in data.polygons:
            polygon.use_smooth = smooth_quads and len(polygon.vertices) == 4
        obj = bpy.data.objects.new(name, data)
        obj.data.materials.append(self.material(mat))
        bpy.context.scene.collection.objects.link(obj)
        obj.matrix_world = Matrix.LocRotScale(Vector(location), Euler(rotation), None)
        self._parts.append(obj)
        return obj

    def box(self, name, size, location, mat, bevel=0.0, bevel_segments=1, rotation=(0, 0, 0)):
        """Axis-aligned box of full size (x, y, z) centred on location, optionally edge-bevelled."""
        bm = bmesh.new()
        bmesh.ops.create_cube(bm, size=1.0)
        bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
        width = min(bevel, min(size) * 0.45)
        if width > 0:
            bmesh.ops.bevel(bm, geom=list(bm.edges), offset=width, segments=bevel_segments,
                            profile=0.5, affect="EDGES", clamp_overlap=True)
        return self._add(name, bm, mat, location, rotation)

    def cylinder(self, name, radius, depth, location, mat, segments=16, radius_top=None, rotation=(0, 0, 0)):
        """Capped cylinder (or frustum/cone with radius_top) along local Z, centred on location."""
        bm = bmesh.new()
        bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segments, radius1=radius,
                              radius2=radius if radius_top is None else radius_top, depth=depth)
        return self._add(name, bm, mat, location, rotation, smooth_quads=True)

    def ring(self, name, outer_radius, inner_radius, height, location, mat, segments=24, rotation=(0, 0, 0)):
        """Flat annulus (washer, hoop, rim) of the given height along local Z."""
        profile = [(outer_radius, -height / 2), (outer_radius, height / 2),
                   (inner_radius, height / 2), (inner_radius, -height / 2)]
        bm = bmesh.new()
        rings = [[bm.verts.new((r * math.cos(2 * math.pi * i / segments), r * math.sin(2 * math.pi * i / segments), z))
                  for r, z in profile] for i in range(segments)]
        for i in range(segments):
            a, b = rings[i], rings[(i + 1) % segments]
            for k in range(4):
                bm.faces.new((a[k], b[k], b[(k + 1) % 4], a[(k + 1) % 4]))
        return self._add(name, bm, mat, location, rotation, smooth_quads=True)

    def arch(self, name, inner_radius, outer_radius, depth, location, mat, segments=12, leg_height=0.0,
             rotation=(0, 0, 0)):
        """Semicircular arch in the local XZ plane, extruded depth along Y; legs drop leg_height below the springing."""
        pairs = []
        if leg_height > 0:
            pairs.append(((outer_radius, -leg_height), (inner_radius, -leg_height)))
        for i in range(segments + 1):
            angle = math.pi * i / segments
            pairs.append(((outer_radius * math.cos(angle), outer_radius * math.sin(angle)),
                          (inner_radius * math.cos(angle), inner_radius * math.sin(angle))))
        if leg_height > 0:
            pairs.append(((-outer_radius, -leg_height), (-inner_radius, -leg_height)))
        bm = bmesh.new()
        columns = [[bm.verts.new((x, y, z)) for x, z in pair for y in (-depth / 2, depth / 2)] for pair in pairs]
        # column = [outer front, outer back, inner front, inner back]
        for a, b in zip(columns, columns[1:]):
            for i, j in ((0, 2), (3, 1), (1, 0), (2, 3)):
                bm.faces.new((a[i], b[i], b[j], a[j]))
        for column, reverse in ((columns[0], False), (columns[-1], True)):
            cap = [column[0], column[2], column[3], column[1]]
            bm.faces.new(cap[::-1] if reverse else cap)
        return self._add(name, bm, mat, location, rotation)

    def extrude(self, name, profile, depth, location, mat, bevel=0.0, rotation=(0, 0, 0)):
        """Simple polygon (x, z) profile in the local XZ plane extruded depth along Y."""
        bm = bmesh.new()
        front = [bm.verts.new((x, -depth / 2, z)) for x, z in profile]
        back = [bm.verts.new((x, depth / 2, z)) for x, z in profile]
        bm.faces.new(front)
        bm.faces.new(back[::-1])
        count = len(profile)
        for i in range(count):
            j = (i + 1) % count
            bm.faces.new((front[i], front[j], back[j], back[i]))
        if bevel > 0:
            bmesh.ops.bevel(bm, geom=list(bm.edges), offset=bevel, segments=1, profile=0.5,
                            affect="EDGES", clamp_overlap=True)
        return self._add(name, bm, mat, location, rotation)

    # ---- export ---------------------------------------------------------------------

    def _joined_copy(self, name):
        copies = []
        for part in self._parts:
            copy = part.copy()
            copy.data = part.data.copy()
            copy.data.transform(part.matrix_world)
            copy.matrix_world = Matrix()
            bpy.context.scene.collection.objects.link(copy)
            copies.append(copy)
        bpy.ops.object.select_all(action="DESELECT")
        for copy in copies:
            copy.select_set(True)
        bpy.context.view_layer.objects.active = copies[0]
        if len(copies) > 1:
            bpy.ops.object.join()
        joined = bpy.context.view_layer.objects.active
        joined.name = name
        joined.data.name = name
        return joined

    def _unwrap(self, obj):
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.008, scale_to_bounds=True)
        bpy.ops.object.mode_set(mode="OBJECT")

    def _decimate(self, obj, ratio):
        modifier = obj.modifiers.new("LOD decimate", "DECIMATE")
        modifier.decimate_type = "COLLAPSE"
        modifier.ratio = ratio
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.modifier_apply(modifier=modifier.name)

    def _hull_object(self, name, source, kind, cap):
        bm = bmesh.new()
        if kind == "hull":
            points = [Vector(v.co) for v in source.data.vertices]
            for _ in range(8):
                bm.clear()
                verts = [bm.verts.new(p) for p in points]
                result = bmesh.ops.convex_hull(bm, input=verts)
                leftovers = {vert for vert in result["geom_interior"] + result["geom_unused"]
                             if isinstance(vert, bmesh.types.BMVert)}
                bmesh.ops.delete(bm, geom=[vert for vert in bm.verts if vert in leftovers], context="VERTS")
                bmesh.ops.triangulate(bm, faces=bm.faces)
                if len(bm.faces) <= cap:
                    break
                # Merge hull points toward a coarser shape, then re-hull the survivors.
                data = bpy.data.meshes.new(name + "_reduce")
                bm.to_mesh(data)
                reduced = bpy.data.objects.new(name + "_reduce", data)
                bpy.context.scene.collection.objects.link(reduced)
                self._decimate(reduced, cap / len(bm.faces) * 0.9)
                points = [Vector(v.co) for v in reduced.data.vertices]
                bpy.data.objects.remove(reduced, do_unlink=True)
                bpy.data.meshes.remove(data)
            else:
                kind = "box"
        if kind == "box":
            corners = [Vector(v.co) for v in source.data.vertices]
            lo = Vector([min(c[i] for c in corners) for i in range(3)])
            hi = Vector([max(c[i] for c in corners) for i in range(3)])
            bm.clear()
            bmesh.ops.create_cube(bm, size=1.0)
            bmesh.ops.scale(bm, vec=hi - lo, verts=bm.verts)
            bmesh.ops.translate(bm, vec=(lo + hi) / 2, verts=bm.verts)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        data = bpy.data.meshes.new(name)
        bm.to_mesh(data)
        bm.free()
        obj = bpy.data.objects.new(name, data)
        obj.data.materials.append(self.material("collision"))
        bpy.context.scene.collection.objects.link(obj)
        return obj, kind

    def _export(self, obj):
        path = self.export_dir / f"{obj.name}.obj"
        bpy.ops.object.select_all(action="DESELECT")
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.wm.obj_export(filepath=str(path), export_selected_objects=True, apply_modifiers=True,
                              export_triangulated_mesh=True, export_uv=True, export_normals=True,
                              export_materials=True, path_mode="STRIP", **EXPORT_AXES)
        canonicalize_obj(path)
        triangles, bounds = obj_stats(path)
        return {"obj_path": path.relative_to(self.repo).as_posix(),
                "mtl_path": path.with_suffix(".mtl").relative_to(self.repo).as_posix(),
                "bounds": bounds, "triangle_count": triangles,
                "obj_sha256": sha256(path), "mtl_sha256": sha256(path.with_suffix(".mtl"))}

    def export_asset(self, name, triangle_budget=5000, lod_ratio=0.5, collision="hull", collision_cap=64):
        """Export the parts built since the last export as name/.../_lod1/_collision and record them.

        lod_ratio is the Decimate (collapse) ratio for LOD1; collision is 'hull' (convex hull,
        reduced until it fits collision_cap triangles, falling back to a box) or 'box'.
        """
        if not self._parts:
            raise RuntimeError(f"{name}: no parts were built")
        self.export_dir.mkdir(parents=True, exist_ok=True)
        joined = self._joined_copy(name)
        self._unwrap(joined)
        record = {"name": name, **self._export(joined), "triangle_budget": triangle_budget}
        if record["triangle_count"] > triangle_budget:
            raise RuntimeError(f"{name}: {record['triangle_count']} triangles exceed budget {triangle_budget}")

        lod = joined.copy()
        lod.data = joined.data.copy()
        lod.name = lod.data.name = name + "_lod1"
        bpy.context.scene.collection.objects.link(lod)
        self._decimate(lod, lod_ratio)
        lod_record = {**self._export(lod), "decimate_ratio": lod_ratio}
        if lod_record["triangle_count"] >= record["triangle_count"]:
            raise RuntimeError(f"{name}: LOD1 is not smaller than the source mesh")

        hull, kind = self._hull_object(name + "_collision", joined, collision, collision_cap)
        self._unwrap(hull)
        hull_record = {**self._export(hull), "kind": kind, "triangle_cap": collision_cap}
        if hull_record["triangle_count"] > collision_cap:
            raise RuntimeError(f"{name}: collision exceeds {collision_cap} triangles")

        for temporary in (joined, lod, hull):
            mesh = temporary.data
            bpy.data.objects.remove(temporary, do_unlink=True)
            bpy.data.meshes.remove(mesh)

        # Keep the editable parts in a per-asset collection of the .blend.
        collection = bpy.data.collections.new(name)
        bpy.context.scene.collection.children.link(collection)
        for part in self._parts:
            for old in list(part.users_collection):
                old.objects.unlink(part)
            collection.objects.link(part)
        self._parts = []

        record["variants"] = {"lod1": lod_record, "collision": hull_record}
        self.records.append(record)
        print("AUTHORED", name, record["triangle_count"], lod_record["triangle_count"],
              hull_record["triangle_count"], flush=True)
        return record

    # ---- source + provenance --------------------------------------------------------

    def scene_digest(self):
        """sha256 of the authored content (objects, transforms, geometry, materials).

        Blender 4.0 writes run-time memory addresses into .blend files, so identical scenes
        never save to identical bytes; this digest is the byte-stable identity of the source.
        """
        def rounded(values):
            return [round(value, 6) + 0.0 for value in values]

        content = {"collections": [[c.name, sorted(o.name for o in c.objects)] for c in bpy.data.collections],
                   "materials": [], "objects": []}
        for mat in sorted(bpy.data.materials, key=lambda m: m.name):
            node = mat.node_tree.nodes.get("Principled BSDF") if mat.node_tree else None
            inputs = [rounded(node.inputs[key].default_value) if key == "Base Color"
                      else round(node.inputs[key].default_value, 6)
                      for key in ("Base Color", "Roughness", "Metallic")] if node else []
            content["materials"].append([mat.name, rounded(mat.diffuse_color), inputs])
        for obj in sorted(bpy.data.objects, key=lambda o: o.name):
            mesh = obj.data
            content["objects"].append([
                obj.name, [rounded(row) for row in obj.matrix_world],
                [mat.name for mat in mesh.materials],
                [rounded(vertex.co) for vertex in mesh.vertices],
                [[list(p.vertices), p.material_index, p.use_smooth] for p in mesh.polygons]])
        return hashlib.sha256(json.dumps(content, separators=(",", ":")).encode()).hexdigest()

    def save_blend(self):
        """Save the editable source (compressed, no .blend1 backups).

        An existing .blend whose recorded scene digest and bytes still match is kept untouched,
        so an unchanged rerun leaves the source and provenance byte-identical.
        """
        self.art_dir.mkdir(parents=True, exist_ok=True)
        self._scene_sha256 = self.scene_digest()
        previous_path = self.art_dir / "provenance.json"
        if previous_path.is_file() and self.blend_path.is_file():
            previous = json.loads(previous_path.read_text(encoding="utf-8")).get("blend", {})
            if (previous.get("scene_sha256") == self._scene_sha256
                    and previous.get("sha256") == sha256(self.blend_path)):
                return
        bpy.ops.wm.save_as_mainfile(filepath=str(self.blend_path), compress=True)

    def write_provenance(self):
        """Write Art/Blender/<Module>/provenance.json (schema of Art/Blender/MMO/provenance.json)."""
        def entry(path):
            path = Path(path).resolve()
            return {"path": path.relative_to(self.repo).as_posix(), "sha256": sha256(path)}

        if self._scene_sha256 is None:
            raise RuntimeError("save_blend() must run before write_provenance()")
        license_path = self.repo / "LICENSE"
        manifest = {
            "schema_version": 1,
            "blender_version": bpy.app.version_string,
            "module": self.module,
            "author_script": entry(self.script),
            "library": entry(__file__),
            "blend": {**entry(self.blend_path), "scene_sha256": self._scene_sha256},
            "license": {"name": LICENSE_NAME, **entry(license_path)},
            "export_convention": "meters, ground pivot, Y-up, forward_axis=Z (left-handed +Z-forward)",
            "assets": self.records,
        }
        path = self.art_dir / "provenance.json"
        path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n")
        return path
