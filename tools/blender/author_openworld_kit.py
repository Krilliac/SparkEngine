"""Author the SparkGameOpenWorld "Open World Landmarks" kit with Blender 4.0.2; Spark Open License 1.0.

Art direction (owner-approved): a weathered frontier of moss-stained stone and pine timber you can navigate
by from a distance. Irregular stacked stone, broken silhouettes, tall vertical landmarks. Every MTL diffuse
is one of four exact palette colours (weathered stone #7D7A70, moss #4E6B3A, pine #2F4A2A, sunset clay
#B8643C); shades differ only by roughness and metalness. Faceted flat shading, 2-4 cm chamfers on hard
edges, no texture maps.

Props face Blender +Y, which the kit export (forward_axis='Z') writes as OBJ +Z. The pivot is the
ground-contact centre (Blender z = 0). OWWorldSetup streams the kit with every region's manifest (see
Art/Blender/SparkGameOpenWorld/README.md).

Run (the Workbench preview needs an OpenGL context; on a display-less Linux host wrap Blender in xvfb-run):
  PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
    --python tools/blender/author_openworld_kit.py -- --repo .
  python3 tools/blender/validate_kit.py Art/Blender/SparkGameOpenWorld/provenance.json
"""
import math
from pathlib import Path
import random
import sys

import bmesh
import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spark_kit  # noqa: E402


def hex_rgb(value):
    """#RRGGBB as the 0-1 triple written verbatim to the MTL Kd line."""
    return tuple(round(int(value[i:i + 2], 16) / 255.0, 6) for i in (1, 3, 5))


STONE = hex_rgb("#7D7A70")
MOSS = hex_rgb("#4E6B3A")
PINE = hex_rgb("#2F4A2A")
CLAY = hex_rgb("#B8643C")

PALETTE = {
    "stone": (STONE, 0.94, 0.0),          # rough, weathered field stone
    "stone_dressed": (STONE, 0.72, 0.0),  # worn quoins, lintels, imposts and coping
    "moss": (MOSS, 0.98, 0.0),            # moss mats on ledges and footings
    "pine": (PINE, 0.82, 0.0),            # tarred pine timber
    "clay": (CLAY, 0.86, 0.0),            # sunset-clay roof, caps and wayfinding keystone
}

BEVEL = 0.03          # default chamfer on hard edges (art direction: 2-4 cm)
SMALL_BEVEL = 0.02    # slender members, caps and moss mats
LOD_RATIO = 0.36      # LOD1 must stay at or under 40% of its source
LOD_LIMIT = 0.40
COLLISION_CAP = 200


def flat(obj):
    """Faceted flat shading (the library smooths cylinder and ring sides)."""
    for polygon in obj.data.polygons:
        polygon.use_smooth = False
    return obj


def chamfer(obj, width=SMALL_BEVEL, min_angle=math.radians(50)):
    """Flat-shade and chamfer only the hard edges (face angle >= min_angle) of a built part."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    hard = [edge for edge in bm.edges if edge.is_manifold and edge.calc_face_angle(0.0) >= min_angle]
    bmesh.ops.bevel(bm, geom=hard, offset=width, segments=1, profile=0.5, affect="EDGES", clamp_overlap=True)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(obj.data)
    bm.free()
    return flat(obj)


def stone(kit, rng, name, size, location, mat="stone", yaw=0.05, shift=0.03):
    """One chamfered stone block with a small seeded yaw and offset, so stacked courses read as irregular."""
    x, y, z = location
    return kit.box(name, size, (x + rng.uniform(-shift, shift), y + rng.uniform(-shift, shift), z), mat,
                   bevel=BEVEL, rotation=(0, 0, rng.uniform(-yaw, yaw)))


def moss(kit, rng, name, size, location, yaw=0.3):
    """A low moss mat draped on a ledge or footing."""
    return kit.box(name, size, location, "moss", bevel=SMALL_BEVEL, rotation=(0, 0, rng.uniform(-yaw, yaw)))


def watchtower(kit):
    """8.0 m frontier watchtower: tapering stacked-stone keep, pine lookout stage, sunset-clay pyramid roof."""
    rng = random.Random(4101)
    course_height = 0.7
    widths = [3.0 - 0.1 * index for index in range(6)]  # 5 cm ledge per course, top of stone at 4.2 m
    for index, width in enumerate(widths):
        z = course_height * index + course_height / 2
        stone(kit, rng, f"Course {index + 1}", (width, width, course_height - 0.02), (0, 0, z), yaw=0.05)
        # Quoins jut past alternating corners, so the silhouette never reads as a clean box.
        for corner, (sx, sy) in enumerate(((1, 1), (-1, -1)) if index % 2 == 0 else ((-1, 1), (1, -1))):
            quoin = (rng.uniform(0.6, 0.8), rng.uniform(0.45, 0.6), course_height * 0.9)
            offset = width / 2 - 0.22
            stone(kit, rng, f"Quoin {index + 1}{'ab'[corner]}", quoin, (sx * offset, sy * offset, z),
                  mat="stone_dressed", yaw=0.08, shift=0.02)
        if index in (0, 2, 4):
            side = rng.choice((-1, 1))
            moss(kit, rng, f"Ledge moss {index + 1}", (width * 0.55, 0.16, 0.05),
                 (rng.uniform(-0.3, 0.3), side * (width / 2 - 0.04), course_height * (index + 1) + 0.02), yaw=0.04)
    for index, (x, y, yaw) in enumerate(((1.75, 0.9, 0.4), (-1.7, -0.6, -0.3), (0.9, -1.72, 0.1))):
        moss(kit, rng, f"Footing moss {index + 1}", (0.7, 0.45, 0.12), (x, y, 0.06), yaw=yaw)

    # Door, lintel and arrow slit on the front (+Y) face.
    front = widths[0] / 2
    kit.box("Door", (0.9, 0.08, 1.6), (0, front + 0.02, 0.8), "pine", bevel=SMALL_BEVEL)
    kit.box("Door lintel", (1.3, 0.22, 0.24), (0, front + 0.06, 1.74), "clay", bevel=BEVEL)
    kit.box("Arrow slit shutter", (0.28, 0.06, 0.6), (0.4, widths[3] / 2 + 0.02, 2.75), "pine", bevel=SMALL_BEVEL)

    # Pine lookout stage on knee braces.
    kit.box("Stage floor", (3.3, 3.3, 0.2), (0, 0, 4.3), "pine", bevel=BEVEL)
    brace_tilt = math.atan2(0.35, 0.7)
    for side, (x, y, rotation) in {"front": (0, 1.42, (-brace_tilt, 0, 0)), "back": (0, -1.42, (brace_tilt, 0, 0)),
                                   "right": (1.42, 0, (0, brace_tilt, 0)),
                                   "left": (-1.42, 0, (0, -brace_tilt, 0))}.items():
        kit.box(f"Knee brace {side}", (0.12, 0.12, 0.8), (x, y, 3.85), "pine", bevel=SMALL_BEVEL, rotation=rotation)
    for index, (sx, sy) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        kit.box(f"Stage post {index + 1}", (0.18, 0.18, 2.0), (sx * 1.5, sy * 1.5, 5.4), "pine", bevel=BEVEL)
    for side, (x, y, along_x) in {"front": (0, 1.52, True), "back": (0, -1.52, True),
                                  "right": (1.52, 0, False), "left": (-1.52, 0, False)}.items():
        board = (2.82, 0.06, 0.6) if along_x else (0.06, 2.82, 0.6)
        rail = (2.86, 0.1, 0.1) if along_x else (0.1, 2.86, 0.1)
        kit.box(f"Parapet {side}", board, (x, y, 4.7), "pine", bevel=SMALL_BEVEL)
        kit.box(f"Top rail {side}", rail, (x, y, 5.05), "pine", bevel=SMALL_BEVEL)

    # Sunset-clay pyramid roof (4-sided frustum turned 45 degrees) and a pine finial topping out at 8.0 m.
    chamfer(kit.cylinder("Roof", 2.62, 1.3, (0, 0, 7.05), "clay", segments=4, radius_top=0.2,
                         rotation=(0, 0, math.pi / 4)), width=BEVEL)
    chamfer(kit.cylinder("Finial", 0.07, 0.4, (0, 0, 7.8), "pine", segments=6))
    kit.export_asset("watchtower", triangle_budget=2800, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def bridge_segment(kit):
    """6.0 x 2.4 m bridge segment: stacked-stone abutments, pine trestle, stringers, planks and clay-capped rails."""
    rng = random.Random(4102)
    for side, sx in (("L", -1), ("R", 1)):
        kit.box(f"Abutment base {side}", (1.0, 2.4, 0.5), (sx * 2.5, 0, 0.25), "stone", bevel=BEVEL)
        stone(kit, rng, f"Abutment course {side}", (0.88, 2.2, 0.46), (sx * 2.52, 0, 0.73), yaw=0.02)
        stone(kit, rng, f"Abutment coping {side}", (0.5, 1.9, 0.14), (sx * 2.45, 0, 1.03),
              mat="stone_dressed", yaw=0.03, shift=0.01)
        moss(kit, rng, f"Abutment moss {side}", (0.8, 0.6, 0.1), (sx * 2.45, -0.6 * sx, 0.05), yaw=0.2)
    moss(kit, rng, "Coping moss", (0.36, 0.7, 0.05), (-2.62, 0.55, 0.52), yaw=0.05)

    # Mid-span pine trestle on a stone footing.
    kit.box("Trestle footing", (0.5, 2.0, 0.2), (0, 0, 0.1), "stone", bevel=BEVEL)
    for side, y in (("L", -0.8), ("R", 0.8)):
        kit.box(f"Trestle post {side}", (0.18, 0.18, 0.78), (0, y, 0.59), "pine", bevel=BEVEL)
    kit.box("Trestle cross brace", (0.1, 1.8, 0.12), (0, 0, 0.59), "pine", bevel=SMALL_BEVEL,
            rotation=(math.atan2(0.6, 1.6), 0, 0))

    for side, y in (("L", -0.8), ("R", 0.8)):
        kit.box(f"Stringer {side}", (5.9, 0.2, 0.26), (0, y, 1.11), "pine", bevel=BEVEL)
    for index in range(12):  # 12 x 0.5 m planks span exactly 6.0 m
        x = -2.75 + 0.5 * index
        kit.box(f"Plank {index + 1:02d}", (0.46, 2.2, 0.08), (x, rng.uniform(-0.03, 0.03), 1.28 + rng.uniform(-0.01, 0.01)),
                "pine", bevel=SMALL_BEVEL, rotation=(0, 0, rng.uniform(-0.02, 0.02)))

    for side, y in (("L", -1.1), ("R", 1.1)):
        for index, x in enumerate((-2.6, 0.0, 2.6)):
            kit.box(f"Rail post {side}{index + 1}", (0.14, 0.14, 1.0), (x, y, 1.82), "pine", bevel=BEVEL)
            kit.box(f"Post cap {side}{index + 1}", (0.2, 0.2, 0.08), (x, y, 2.36), "clay", bevel=SMALL_BEVEL)
        kit.box(f"Top rail {side}", (5.34, 0.1, 0.1), (0, y, 2.22), "pine", bevel=SMALL_BEVEL)
        kit.box(f"Mid rail {side}", (5.34, 0.08, 0.08), (0, y, 1.8), "pine", bevel=SMALL_BEVEL)
    kit.export_asset("bridge_segment", triangle_budget=2000, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def ruined_arch(kit):
    """5.0 m ruined arch: stacked-stone piers, wedge voussoirs with a clay keystone, a broken wall stub, shoring."""
    rng = random.Random(4103)
    springing = 2.5
    inner_radius, outer_radius = 1.05, 1.9
    for side, sx in (("L", -1), ("R", 1)):
        kit.box(f"Plinth {side}", (1.1, 1.2, 0.35), (sx * 1.5, 0, 0.175), "stone_dressed", bevel=BEVEL)
        course = (springing - 0.16 - 0.35) / 3
        for index in range(3):
            stone(kit, rng, f"Pier {side} course {index + 1}", (0.86, 0.96, course - 0.02),
                  (sx * 1.5, 0, 0.35 + course * index + course / 2), yaw=0.06)
        kit.box(f"Impost {side}", (1.0, 1.08, 0.16), (sx * 1.5, 0, springing - 0.08), "stone_dressed", bevel=BEVEL)
        moss(kit, rng, f"Plinth moss {side}", (0.9, 0.3, 0.06), (sx * 1.5, 0.48, 0.37), yaw=0.05)

    # Nine wedge voussoirs about the springing centre; the keystone is sunset clay and stands proud.
    gap = 0.012
    for index in range(9):
        a0 = math.pi * index / 9 + gap
        a1 = math.pi * (index + 1) / 9 - gap
        keystone = index == 4
        outer = outer_radius + (0.1 if keystone else rng.uniform(-0.05, 0.03))
        profile = [(inner_radius * math.cos(a0), inner_radius * math.sin(a0)),
                   (outer * math.cos(a0), outer * math.sin(a0)),
                   (outer * math.cos(a1), outer * math.sin(a1)),
                   (inner_radius * math.cos(a1), inner_radius * math.sin(a1))]
        kit.extrude("Keystone" if keystone else f"Voussoir {index + 1}", profile, 1.0 if keystone else 0.9,
                    (0, 0, springing), "clay" if keystone else "stone", bevel=BEVEL,
                    rotation=(0, 0 if keystone else rng.uniform(-0.015, 0.015), 0))

    # Broken spandrel wall: a stepped stub on the left rising to 5.0 m, a single low stone on the right.
    stone(kit, rng, "Wall stub 1", (0.7, 0.88, 0.5), (-1.6, 0, 3.8), yaw=0.05, shift=0.02)
    stone(kit, rng, "Wall stub 2", (0.62, 0.84, 0.5), (-1.62, 0, 4.3), yaw=0.07, shift=0.02)
    stone(kit, rng, "Wall stub 3", (0.46, 0.78, 0.45), (-1.7, 0, 4.775), yaw=0.1, shift=0.02)
    stone(kit, rng, "Wall fill", (0.5, 0.86, 0.4), (-1.12, 0, 4.2), yaw=0.08, shift=0.02)
    stone(kit, rng, "Right spandrel", (0.56, 0.86, 0.46), (1.6, 0, 3.7), yaw=0.08, shift=0.02)
    moss(kit, rng, "Stub moss", (0.4, 0.6, 0.05), (-1.68, 0.05, 5.0 - 0.025 - 0.01), yaw=0.05)
    moss(kit, rng, "Crown moss", (0.5, 0.7, 0.05), (0.28, -0.05, springing + outer_radius - 0.03), yaw=0.25)

    # Pine shoring under the right haunch.
    kit.box("Shoring post", (0.16, 0.16, 3.0), (0.86, 0, 1.5), "pine", bevel=BEVEL)
    kit.box("Shoring head", (0.36, 0.5, 0.1), (0.86, 0, 3.05), "pine", bevel=SMALL_BEVEL)
    kit.box("Shoring kicker", (0.12, 0.12, 1.0), (1.12, 0, 0.72), "pine", bevel=SMALL_BEVEL,
            rotation=(0, math.radians(-24), 0))

    # Fallen blocks in front of the arch.
    for index, (x, y, size, yaw) in enumerate((((-0.55, 1.05, (0.6, 0.45, 0.4), 0.5)),
                                              ((0.3, 1.3, (0.45, 0.4, 0.32), -0.4)),
                                              ((2.2, 0.9, (0.5, 0.42, 0.36), 0.9)))):
        kit.box(f"Fallen block {index + 1}", size, (x, y, size[2] / 2), "stone", bevel=BEVEL, rotation=(0, 0, yaw))
    moss(kit, rng, "Fallen moss", (0.4, 0.3, 0.05), (-0.55, 1.05, 0.42), yaw=0.5)
    kit.export_asset("ruined_arch", triangle_budget=1800, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def check_lod_limits(kit):
    for record in kit.records:
        lod = record["variants"]["lod1"]["triangle_count"]
        if lod > LOD_LIMIT * record["triangle_count"]:
            raise RuntimeError(f"{record['name']}: LOD1 {lod} exceeds {LOD_LIMIT:.0%} of {record['triangle_count']}")


def srgb_to_linear(channel):
    return channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4


def render_preview(path):
    """Workbench preview of the kit side by side (after the .blend is saved; never written back to it)."""
    # The camera looks down -Y at the props' fronts, so +X is screen left: list them left to right.
    offsets = {"watchtower": 6.2, "ruined_arch": 0.6, "bridge_segment": -5.6}
    for name, offset in offsets.items():
        for obj in bpy.data.collections[name].objects:
            obj.location.x += offset
    # Show the MTL colours as authored sRGB in the viewport shading.
    for mat in bpy.data.materials:
        mat.diffuse_color = (*(srgb_to_linear(c) for c in mat.diffuse_color[:3]), 1.0)

    ground_mesh = bpy.data.meshes.new("Preview ground")
    ground_mesh.from_pydata([(-60, -40, 0), (60, -40, 0), (60, 40, 0), (-60, 40, 0)], [], [(0, 1, 2, 3)])
    ground = bpy.data.objects.new("Preview ground", ground_mesh)
    ground_mat = bpy.data.materials.new("Preview_ground")
    ground_mat.diffuse_color = (0.42, 0.42, 0.42, 1.0)
    ground_mesh.materials.append(ground_mat)
    bpy.context.scene.collection.objects.link(ground)

    camera_data = bpy.data.cameras.new("Preview camera")
    camera_data.lens = 35
    camera = bpy.data.objects.new("Preview camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = (0.0, 19.0, 5.2)
    camera.rotation_euler = (math.radians(83), 0.0, math.radians(180))

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
    kit = spark_kit.Kit(args.repo, "SparkGameOpenWorld", __file__, PALETTE, short="OpenWorld")
    kit.blend_path = kit.art_dir / "openworld_kit.blend"
    for build in (watchtower, bridge_segment, ruined_arch):
        build(kit)
    check_lod_limits(kit)
    kit.save_blend()
    kit.write_provenance()
    render_preview(kit.art_dir / "preview.png")


main()
