"""Author the SparkGameMMO "Fantasy MMO Hub" town-square kit with Blender 4.0.2; Spark Open License 1.0.

Art direction (owner-approved): a warm market town matching the existing MMO props. Carved oak, brass
fittings and teal guild banners; timber frames with pegged joints, brass corner caps and gently curved
roofs. Every MTL diffuse is one of four exact palette colours (oak #6B3A1E, brass #9E6A2A, guild teal
#1F5C55, cream plaster #D1B98A); shades differ only by roughness and metalness. Faceted flat shading,
2-4 cm chamfers on hard edges, no texture maps.

Props face Blender +Y, which the kit export (forward_axis='Z') writes as OBJ +Z, the direction the art
direction names. The pivot is the ground-contact centre (Blender z = 0). MMOEngineSystems places the
kit in the TownSquare area (see Art/Blender/SparkGameMMO/README.md).

Run (the Workbench preview needs an OpenGL context; on a display-less Linux host wrap Blender in xvfb-run):
  PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
    --python tools/blender/author_mmo_kit.py -- --repo .
  python3 tools/blender/validate_kit.py Art/Blender/SparkGameMMO/provenance.json
"""
import json
import math
from pathlib import Path
import sys

import bmesh
import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spark_kit  # noqa: E402


def hex_rgb(value):
    """#RRGGBB as the 0-1 triple written verbatim to the MTL Kd line."""
    return tuple(round(int(value[i:i + 2], 16) / 255.0, 6) for i in (1, 3, 5))


OAK = hex_rgb("#6B3A1E")
BRASS = hex_rgb("#9E6A2A")
TEAL = hex_rgb("#1F5C55")
PLASTER = hex_rgb("#D1B98A")

PALETTE = {
    "oak": (OAK, 0.8, 0.0),               # carved, oiled timber frames
    "oak_polished": (OAK, 0.42, 0.0),     # hand-worn pegs, countertops and trims
    "brass": (BRASS, 0.34, 0.9),          # corner caps, collars, fittings
    "teal": (TEAL, 0.88, 0.0),            # guild cloth: banners, awnings, notice felt
    "teal_glass": (TEAL, 0.12, 0.25),     # glossy portal membrane and rune gems
    "plaster": (PLASTER, 0.9, 0.0),       # cream plaster, parchment notices
}

BEVEL = 0.03          # default chamfer on hard edges (art direction: 2-4 cm)
SMALL_BEVEL = 0.02    # slender members and caps
LOD_RATIO = 0.36      # LOD1 must stay at or under 40% of its source
LOD_LIMIT = 0.40
COLLISION_CAP = 200
HALF_PI = math.pi / 2
FACE_FRONT = (HALF_PI, 0, 0)  # turn a local-Z primitive (disc, ring, peg) to face Blender +Y


def flat(obj):
    """Faceted flat shading (the library smooths cylinder and ring sides)."""
    for polygon in obj.data.polygons:
        polygon.use_smooth = False
    return obj


def chamfer(obj, width=SMALL_BEVEL, min_angle=math.radians(50)):
    """Flat-shade and chamfer only the hard edges (face angle >= min_angle) of a built part.

    Cylinders and rings keep their facet edges sharp and get the 2-4 cm bevel on cap and lip edges,
    which is what the art direction asks for without multiplying every facet.
    """
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    hard = [edge for edge in bm.edges if edge.is_manifold and edge.calc_face_angle(0.0) >= min_angle]
    bmesh.ops.bevel(bm, geom=hard, offset=width, segments=1, profile=0.5, affect="EDGES", clamp_overlap=True)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(obj.data)
    bm.free()
    return flat(obj)


def curved_roof(kit, name, width, depth, rise, thickness, base_z, mat, segments=6, y=0.0):
    """Gently curved roof shell: an arc across depth (Blender Y), extruded along the width (X)."""
    outer, inner = [], []
    for index in range(segments + 1):
        t = -1.0 + 2.0 * index / segments
        u = t * depth / 2
        z = base_z + rise * (1.0 - t * t)
        outer.append((u, z + thickness))
        inner.append((u, z))
    profile = outer + inner[::-1]
    # extrude() lays the profile in local XZ and extrudes along local Y; a +90 degree turn about Z maps
    # local X onto Blender Y (the arc runs front to back) and the extrusion onto the width.
    return chamfer(kit.extrude(name, profile, width, (0, y, 0), mat, rotation=(0, 0, HALF_PI)))


def peg(kit, name, x, y, z, radius=0.024, length=0.04):
    """Protruding round oak peg head of a pegged timber joint, facing +Y."""
    return flat(kit.cylinder(name, radius, length, (x, y + length / 2, z), "oak_polished", segments=8,
                             rotation=FACE_FRONT))


def quest_board(kit):
    """2.0 x 2.2 m guild quest board: pegged oak frame, teal notice felt, parchment notices, curved roof."""
    for side, x in (("L", -0.8), ("R", 0.8)):
        kit.box(f"Sole plate {side}", (0.34, 0.56, 0.08), (x, 0, 0.04), "oak", bevel=SMALL_BEVEL)
        kit.box(f"Post {side}", (0.14, 0.14, 1.94), (x, 0, 1.05), "oak", bevel=BEVEL)
        kit.box(f"Post foot cap {side}", (0.18, 0.18, 0.1), (x, 0, 0.13), "brass", bevel=SMALL_BEVEL)
        for rail, z in (("top", 1.78), ("bottom", 0.62)):
            peg(kit, f"Peg {rail} {side}", x, 0.07, z)
    kit.box("Rail top", (1.46, 0.1, 0.12), (0, 0, 1.78), "oak", bevel=BEVEL)
    kit.box("Rail bottom", (1.46, 0.1, 0.12), (0, 0, 0.62), "oak", bevel=BEVEL)
    kit.box("Notice felt", (1.46, 0.05, 1.04), (0, 0, 1.2), "teal", bevel=SMALL_BEVEL)
    for index, (x, z) in enumerate(((-0.69, 1.68), (0.69, 1.68), (0.69, 0.72), (-0.69, 0.72))):
        kit.box(f"Felt corner cap {index + 1}", (0.1, 0.03, 0.1), (x, 0.03, z), "brass", bevel=0.01)
    notices = (  # x, z, width, height, tilt about Y (radians)
        (-0.42, 1.36, 0.36, 0.44, 0.05), (0.02, 1.42, 0.32, 0.38, -0.04), (0.44, 1.3, 0.34, 0.5, 0.03),
        (-0.3, 0.9, 0.42, 0.3, -0.03), (0.26, 0.92, 0.3, 0.34, 0.06),
    )
    for index, (x, z, width, height, tilt) in enumerate(notices):
        kit.box(f"Notice {index + 1}", (width, 0.012, height), (x, 0.031, z), "plaster", rotation=(0, tilt, 0))
        flat(kit.cylinder(f"Notice pin {index + 1}", 0.018, 0.02, (x, 0.047, z + height / 2 - 0.05), "brass",
                          segments=6, rotation=FACE_FRONT))
    kit.box("Lintel", (1.9, 0.16, 0.14), (0, 0, 2.0), "oak", bevel=BEVEL)
    for side, x in (("L", -0.8), ("R", 0.8)):
        peg(kit, f"Peg lintel {side}", x, 0.08, 2.0)
    curved_roof(kit, "Roof shell", 2.0, 0.66, 0.08, 0.06, 2.06, "oak")
    kit.box("Roof ridge cap", (2.0, 0.08, 0.04), (0, 0, 2.18), "brass", bevel=0.01)
    kit.box("Roof valance", (1.86, 0.03, 0.12), (0, 0.3, 2.02), "teal", bevel=0.01)
    kit.export_asset("quest_board", triangle_budget=1400, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def vendor_stall(kit):
    """3.0 x 2.6 m market stall: oak deck and pegged frame, plaster counter, curved teal-striped awning."""
    kit.box("Deck", (2.9, 1.9, 0.12), (0, 0, 0.06), "oak", bevel=BEVEL)
    kit.box("Counter body", (2.6, 0.56, 0.84), (0, 0.52, 0.54), "plaster", bevel=BEVEL)
    kit.box("Counter top", (2.8, 0.72, 0.08), (0, 0.52, 1.0), "oak_polished", bevel=BEVEL)
    for index, x in enumerate((-1.36, 1.36)):
        for row, y in enumerate((0.84, 0.2)):
            kit.box(f"Counter corner cap {index}{row}", (0.12, 0.12, 0.1), (x, y, 1.0), "brass", bevel=SMALL_BEVEL)
    kit.box("Counter kick rail", (2.64, 0.06, 0.12), (0, 0.8, 0.2), "oak", bevel=SMALL_BEVEL)
    for side, x in (("L", -0.44), ("R", 0.44)):
        kit.box(f"Counter panel stile {side}", (0.08, 0.04, 0.62), (x, 0.8, 0.58), "oak", bevel=SMALL_BEVEL)

    for side, x in (("L", -1.36), ("R", 1.36)):
        for row, y in (("front", 0.86), ("back", -0.82)):
            kit.box(f"Post {side} {row}", (0.14, 0.14, 2.2), (x, y, 1.22), "oak", bevel=BEVEL)
            kit.box(f"Post foot cap {side} {row}", (0.18, 0.18, 0.1), (x, y, 0.17), "brass", bevel=SMALL_BEVEL)
        kit.box(f"Side beam {side}", (0.14, 1.82, 0.16), (x, 0.02, 2.24), "oak", bevel=BEVEL)
    for row, y in (("front", 0.86), ("back", -0.82)):
        kit.box(f"Beam {row}", (2.94, 0.14, 0.16), (0, y, 2.24), "oak", bevel=BEVEL)
    for side, x in (("L", -1.36), ("R", 1.36)):
        peg(kit, f"Peg beam {side}", x, 0.93, 2.24)
        peg(kit, f"Peg counter {side}", x, 0.93, 0.96)

    curved_roof(kit, "Awning", 3.0, 2.0, 0.22, 0.05, 2.33, "teal", segments=8, y=0.02)
    kit.box("Awning ridge rod", (3.0, 0.06, 0.06), (0, 0.02, 2.57), "brass", bevel=0.01)
    for index in range(7):  # alternating guild-teal and cream valance tabs along the front edge
        x = -1.29 + index * 0.43
        mat = "teal" if index % 2 == 0 else "plaster"
        kit.box(f"Valance tab {index + 1}", (0.4, 0.03, 0.2), (x, 1.0, 2.26), mat, bevel=0.01)

    kit.box("Back shelf", (2.6, 0.34, 0.06), (0, -0.66, 1.3), "oak_polished", bevel=SMALL_BEVEL)
    kit.box("Crate left", (0.44, 0.4, 0.4), (-1.0, 0.44, 1.24), "oak", bevel=BEVEL)
    kit.box("Crate left band", (0.46, 0.42, 0.05), (-1.0, 0.44, 1.24), "brass", bevel=0.01)
    flat(kit.cylinder("Cloth bolt", 0.1, 0.6, (0.1, 0.4, 1.14), "teal", segments=8, rotation=(0, HALF_PI, 0)))
    flat(kit.cylinder("Cloth bolt 2", 0.1, 0.6, (0.1, 0.62, 1.14), "plaster", segments=8,
                      rotation=(0, HALF_PI, 0)))
    for index, x in enumerate((0.72, 1.0)):
        chamfer(kit.cylinder(f"Brass urn {index + 1}", 0.13, 0.3, (x, 0.5, 1.19), "brass", segments=10,
                             radius_top=0.08), width=0.015)
    for index, x in enumerate((-0.9, -0.3, 0.3, 0.9)):
        chamfer(kit.cylinder(f"Shelf jar {index + 1}", 0.08, 0.2, (x, -0.66, 1.43), "brass", segments=8))
    kit.box("Guild sign", (0.9, 0.05, 0.26), (0, 0.95, 1.98), "oak_polished", bevel=SMALL_BEVEL)
    chamfer(kit.cylinder("Guild sign emblem", 0.1, 0.03, (0, 0.985, 1.98), "teal", segments=12,
                         rotation=FACE_FRONT), width=0.01)
    chamfer(kit.cylinder("Lantern", 0.09, 0.22, (1.2, 0.95, 1.94), "brass", segments=8, radius_top=0.06),
            width=0.015)
    kit.export_asset("vendor_stall", triangle_budget=2600, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def portal_ring(kit):
    """3.2 m diameter guild portal: brass ring with oak lining, glossy teal membrane, plaster dais."""
    ring_z = 1.84  # ring centre; the 1.6 m outer radius beds 4 cm into the upper step
    kit.box("Dais lower step", (3.2, 1.8, 0.16), (0, 0, 0.08), "plaster", bevel=BEVEL)
    kit.box("Dais upper step", (2.6, 1.3, 0.12), (0, 0, 0.22), "oak", bevel=BEVEL)
    for index, (x, y) in enumerate(((-1.51, 0.81), (1.51, 0.81), (1.51, -0.81), (-1.51, -0.81))):
        kit.box(f"Dais corner cap {index + 1}", (0.18, 0.18, 0.18), (x, y, 0.09), "brass", bevel=SMALL_BEVEL)

    chamfer(kit.ring("Portal ring", 1.6, 1.32, 0.3, (0, 0, ring_z), "brass", segments=32, rotation=FACE_FRONT),
            width=BEVEL)
    chamfer(kit.ring("Portal lining", 1.33, 1.2, 0.22, (0, 0, ring_z), "oak", segments=24, rotation=FACE_FRONT))
    flat(kit.cylinder("Portal membrane", 1.21, 0.04, (0, 0, ring_z), "teal_glass", segments=24,
                      rotation=FACE_FRONT))
    for index in range(8):  # rune gems set into the front face of the ring
        angle = 2 * math.pi * (index + 0.5) / 8
        kit.box(f"Rune gem {index + 1}", (0.12, 0.05, 0.16),
                (1.46 * math.cos(angle), 0.16, ring_z + 1.46 * math.sin(angle)), "teal_glass", bevel=SMALL_BEVEL,
                rotation=(0, -angle + HALF_PI, 0))
    kit.box("Keystone", (0.36, 0.4, 0.3), (0, 0, ring_z + 1.58), "oak", bevel=BEVEL)
    kit.box("Keystone cap", (0.4, 0.44, 0.06), (0, 0, ring_z + 1.76), "brass", bevel=SMALL_BEVEL)

    for side, sign in (("L", -1), ("R", 1)):
        cradle = [(0.72, 0.28), (1.52, 0.28), (1.34, 0.9), (0.96, 0.9)]
        kit.extrude(f"Cradle {side}", [(sign * x, z) for x, z in cradle], 0.42, (0, 0, 0), "oak", bevel=BEVEL)
        kit.box(f"Cradle cap {side}", (0.44, 0.46, 0.06), (sign * 1.15, 0, 0.93), "brass", bevel=SMALL_BEVEL)
        peg(kit, f"Peg cradle {side}", sign * 1.15, 0.21, 0.6, radius=0.03)
    kit.export_asset("portal_ring", triangle_budget=2400, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def check_lod_limits(kit):
    for record in kit.records:
        lod = record["variants"]["lod1"]["triangle_count"]
        if lod > LOD_LIMIT * record["triangle_count"]:
            raise RuntimeError(f"{record['name']}: LOD1 {lod} exceeds {LOD_LIMIT:.0%} of {record['triangle_count']}")


def srgb_to_linear(channel):
    return channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4


def render_preview(kit, path):
    """Workbench preview of the kit side by side (after the .blend is saved; never written back to it)."""
    # The camera looks down -Y at the props' fronts, so +X is screen left: list them right to left.
    offsets = {"quest_board": 3.4, "vendor_stall": 0.0, "portal_ring": -3.9}
    for name, offset in offsets.items():
        for obj in bpy.data.collections[name].objects:
            obj.location.x += offset
    # Show the MTL colours as authored sRGB in the viewport shading.
    for mat in bpy.data.materials:
        mat.diffuse_color = (*(srgb_to_linear(c) for c in mat.diffuse_color[:3]), 1.0)

    ground_mesh = bpy.data.meshes.new("Preview ground")
    ground_mesh.from_pydata([(-40, -30, 0), (40, -30, 0), (40, 30, 0), (-40, 30, 0)], [], [(0, 1, 2, 3)])
    ground = bpy.data.objects.new("Preview ground", ground_mesh)
    ground_mat = bpy.data.materials.new("Preview_ground")
    ground_mat.diffuse_color = (0.42, 0.42, 0.42, 1.0)
    ground_mesh.materials.append(ground_mat)
    bpy.context.scene.collection.objects.link(ground)

    camera_data = bpy.data.cameras.new("Preview camera")
    camera_data.lens = 35
    camera = bpy.data.objects.new("Preview camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = (-0.2, 11.2, 3.6)
    camera.rotation_euler = (math.radians(79), 0.0, math.radians(180))

    scene = bpy.context.scene
    scene.camera = camera
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x, scene.render.resolution_y = 1600, 900
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.image_settings.compression = 100
    scene.render.use_stamp = False
    scene.render.dither_intensity = 0.0  # dither noise would triple the PNG size
    scene.display.render_aa = "8"
    scene.display_settings.display_device = "sRGB"
    scene.view_settings.view_transform = "Standard"
    shading = scene.display.shading
    shading.light = "STUDIO"
    shading.studiolight_rotate_z = 0.0
    shading.color_type = "MATERIAL"
    shading.show_shadows = True
    shading.shadow_intensity = 0.35
    shading.show_cavity = False  # screen-space cavity adds per-pixel noise and doubles the PNG size
    shading.background_type = "VIEWPORT"
    shading.background_color = (0.62, 0.64, 0.66)
    scene.display.light_direction = (0.45, 0.35, 0.82)
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    if not path.is_file():
        raise RuntimeError(f"Workbench preview was not written to {path}")


def main():
    args = spark_kit.parse_args()
    kit = spark_kit.Kit(args.repo, "SparkGameMMO", __file__, PALETTE, short="MMO")
    kit.blend_path = kit.art_dir / "mmo_kit.blend"
    provenance_path = kit.art_dir / "provenance.json"
    previous = json.loads(provenance_path.read_text(encoding="utf-8")) if provenance_path.is_file() else {}
    for build in (quest_board, vendor_stall, portal_ring):
        build(kit)
    check_lod_limits(kit)
    kit.save_blend()
    current = json.loads(kit.write_provenance().read_text(encoding="utf-8"))
    # Workbench output under a software GL context differs by a few pixels from run to run, so an unchanged
    # kit (same scene digest and same authoring script) keeps its preview and a rerun stays byte-identical.
    preview = kit.art_dir / "preview.png"
    unchanged = all(previous.get(key, {}).get(field) == current[key][field]
                    for key, field in (("blend", "scene_sha256"), ("author_script", "sha256")))
    if not (unchanged and preview.is_file()):
        render_preview(kit, preview)


main()
