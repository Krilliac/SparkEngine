"""Author the SparkGameRPG "Village & Dungeon" kit with Blender 4.0.2; Spark Open License 1.0.

Art direction (owner-approved): a storybook village by day and a torch-lit dungeon by night. Rounded
timber, thatch overhangs, iron bands and hand-hewn stone. Every MTL diffuse is one of four exact palette
colours (village timber #7A4A28, thatch #C9A55A, slate #4A5058, torch flame #E8752A); shades differ only by
roughness and metalness (iron and brass are metallic slate and flame). Faceted flat shading, 2-4 cm
chamfers on hard edges, no texture maps.

Props face Blender +Y, which the kit export (forward_axis='Z') writes as OBJ +Z. The pivot is the
ground-contact centre (Blender z = 0); the wall sconce's pivot is the bottom of its wall plate, with the
wall at Blender y = 0 (OBJ z = 0) and the sconce reaching out along +Z. RPGWorldSetup streams the kit with
the areas where each prop belongs (see Art/Blender/SparkGameRPG/README.md).

Run (the Workbench preview needs an OpenGL context; on a display-less Linux host wrap Blender in xvfb-run):
  PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
    --python tools/blender/author_rpg_kit.py -- --repo .
  python3 tools/blender/validate_kit.py Art/Blender/SparkGameRPG/provenance.json
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


TIMBER = hex_rgb("#7A4A28")
THATCH = hex_rgb("#C9A55A")
SLATE = hex_rgb("#4A5058")
FLAME = hex_rgb("#E8752A")

PALETTE = {
    "timber": (TIMBER, 0.78, 0.0),        # rounded village timber
    "endgrain": (TIMBER, 0.92, 0.0),      # sawn end grain and rough planks
    "thatch": (THATCH, 0.96, 0.0),        # thatch, rope, parchment
    "stone": (SLATE, 0.9, 0.0),           # hand-hewn slate stone
    "water": (SLATE, 0.08, 0.2),          # still well water
    "iron": (SLATE, 0.42, 0.8),           # blackened iron bands and brackets
    "brass": (FLAME, 0.35, 0.85),         # chest lock and fittings
    "flame": (FLAME, 1.0, 0.0),           # torch flame and wax seal
}

BEVEL = 0.03          # default chamfer on hard edges (art direction: 2-4 cm)
SMALL_BEVEL = 0.02    # slender members, boards and bands
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


def gable_roof(kit, prefix, span, length, ridge_z, pitch, thickness, cap_radius, eave_radius=0.0, mat="thatch",
               bevel=0.04):
    """Two rounded-edge slabs meeting over a ridge along X, closed by a round ridge roll of cap_radius.

    eave_radius > 0 adds a round thatch roll along each eave, the thick storybook overhang.

    Each slab covers span/2 of the plan depth (Y); the roll tops out at ridge_z + thickness / 2 + cap_radius.
    """
    half = span / 2
    slope = half / math.cos(pitch)
    for side, sign in (("front", 1), ("back", -1)):
        # Slab centre sits halfway down the slope, pushed out along its normal by half the thickness.
        normal_y, normal_z = sign * math.sin(pitch), math.cos(pitch)
        centre = (0.0, sign * half / 2 + normal_y * thickness / 2,
                  ridge_z - half / 2 * math.tan(pitch) + normal_z * thickness / 2)
        kit.box(f"{prefix} {side}", (length, slope, thickness), centre, mat, bevel=bevel, bevel_segments=2,
                rotation=(-sign * pitch, 0, 0))
    flat(kit.cylinder(f"{prefix} ridge roll", cap_radius, length + 0.04, (0, 0, ridge_z + thickness / 2), mat,
                      segments=6, rotation=(0, math.pi / 2, 0)))
    if eave_radius > 0:
        for side, sign in (("front", 1), ("back", -1)):
            flat(kit.cylinder(f"{prefix} eave roll {side}", eave_radius, length + 0.02,
                              (0, sign * half, ridge_z - half * math.tan(pitch) + thickness / 2), mat, segments=6,
                              rotation=(0, math.pi / 2, 0)))


def village_well(kit):
    """Village well: 1.6 m hand-hewn stone ring, timber windlass and a thatched gable roof topping out at 2.4 m."""
    rng = random.Random(5101)
    courses, blocks, course_height, radius = 2, 9, 0.33, 0.68
    for course in range(courses):
        for index in range(blocks):
            angle = 2 * math.pi * (index + 0.5 * course) / blocks + rng.uniform(-0.03, 0.03)
            width = 2 * math.pi * radius / blocks - 0.03
            z = course_height * course + (course_height - 0.02) / 2
            kit.box(f"Wall stone {course + 1}.{index + 1}", (width, 0.2 + rng.uniform(-0.01, 0.02),
                                                               course_height - 0.02),
                    (radius * math.cos(angle), radius * math.sin(angle), z),
                    "stone", bevel=BEVEL, rotation=(0, 0, angle - math.pi / 2 + rng.uniform(-0.04, 0.04)))
    chamfer(kit.ring("Coping", 0.8, 0.56, 0.1, (0, 0, 0.71), "stone", segments=10))
    flat(kit.cylinder("Water", 0.58, 0.04, (0, 0, 0.42), "water", segments=10))

    # Timber uprights on the coping carry a ridge beam and the windlass.
    for side, sx in (("L", -1), ("R", 1)):
        chamfer(kit.cylinder(f"Post {side}", 0.07, 1.62, (sx * 0.68, 0, 1.47), "timber", segments=8))
        kit.extrude(f"Gable {side}", [(-0.5, 0.0), (0.5, 0.0), (0.0, 0.42)], 0.06, (sx * 0.68, 0, 1.86), "timber",
                    bevel=SMALL_BEVEL, rotation=(0, 0, math.pi / 2))
    kit.box("Ridge beam", (1.9, 0.12, 0.12), (0, 0, 2.2), "timber", bevel=BEVEL)
    flat(kit.cylinder("Windlass axle", 0.045, 1.46, (0.04, 0, 1.4), "timber", segments=8,
                      rotation=(0, math.pi / 2, 0)))
    flat(kit.cylinder("Rope drum", 0.09, 0.42, (0, 0, 1.4), "thatch", segments=8, rotation=(0, math.pi / 2, 0)))
    kit.box("Crank arm", (0.04, 0.05, 0.26), (0.79, 0, 1.3), "iron", bevel=SMALL_BEVEL)
    flat(kit.cylinder("Crank handle", 0.025, 0.16, (0.85, 0, 1.19), "timber", segments=6,
                      rotation=(0, math.pi / 2, 0)))
    flat(kit.cylinder("Rope", 0.014, 0.38, (0, -0.06, 1.13), "thatch", segments=6))

    # Bucket hanging just above the coping, bound with an iron band.
    chamfer(kit.cylinder("Bucket", 0.12, 0.2, (0, -0.06, 0.84), "timber", segments=10, radius_top=0.145))
    flat(kit.ring("Bucket band", 0.148, 0.12, 0.03, (0, -0.06, 0.9), "iron", segments=10))

    # Thatch gable roof with rounded overhangs, ridge along X.
    gable_roof(kit, "Thatch", span=1.9, length=2.0, ridge_z=2.29, pitch=math.radians(38), thickness=0.12,
               cap_radius=0.06, eave_radius=0.08)
    kit.export_asset("village_well", triangle_budget=2200, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def quest_signpost(kit):
    """2.1 m quest signpost: stone footing, iron-banded timber post, three arrow boards, notice and thatch cap."""
    kit.box("Footing", (0.4, 0.4, 0.18), (0, 0, 0.09), "stone", bevel=BEVEL)
    chamfer(kit.cylinder("Post", 0.07, 1.86, (0, 0, 1.11), "timber", segments=8))
    flat(kit.ring("Post band", 0.078, 0.066, 0.05, (0, 0, 0.24), "iron", segments=8))

    arrow = [(-0.34, -0.08), (0.24, -0.08), (0.36, 0.0), (0.24, 0.08), (-0.34, 0.08)]
    for index, (height, yaw, flip) in enumerate(((1.82, 0.25, 1), (1.6, -0.35, -1), (1.38, 0.9, 1))):
        board = [(flip * x, z) for x, z in arrow]
        if flip < 0:
            board.reverse()
        kit.extrude(f"Arrow board {index + 1}", board, 0.05,
                    (flip * 0.3 * math.cos(yaw), flip * 0.3 * math.sin(yaw), height), "timber",
                    bevel=SMALL_BEVEL, rotation=(0, 0, yaw))

    # Quest notice facing the road (+Y): timber board, parchment and a flame-orange wax seal.
    kit.box("Notice board", (0.38, 0.04, 0.3), (0, 0.09, 1.02), "timber", bevel=SMALL_BEVEL)
    kit.box("Parchment", (0.28, 0.012, 0.22), (0, 0.114, 1.02), "thatch")
    flat(kit.cylinder("Wax seal", 0.03, 0.02, (0.07, 0.124, 0.95), "flame", segments=8,
                      rotation=(math.pi / 2, 0, 0)))

    gable_roof(kit, "Cap", span=0.32, length=0.26, ridge_z=2.02, pitch=math.radians(35), thickness=0.06,
               cap_radius=0.03, bevel=SMALL_BEVEL)
    kit.export_asset("quest_signpost", triangle_budget=700, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def treasure_chest(kit):
    """0.9 x 0.6 x 0.6 m treasure chest: plank body, rounded lid, iron bands and handles, brass lock."""
    kit.box("Body", (0.84, 0.56, 0.3), (0, 0, 0.15), "timber", bevel=BEVEL)
    lid = [(0.28 * math.cos(math.pi * i / 8), 0.28 * math.sin(math.pi * i / 8)) for i in range(9)]
    chamfer(kit.extrude("Lid", lid, 0.84, (0, 0, 0.3), "endgrain", rotation=(0, 0, math.pi / 2)))
    kit.box("Lid seam", (0.86, 0.58, 0.03), (0, 0, 0.3), "iron", bevel=0.01)

    for side, x in (("L", -0.27), ("R", 0.27)):
        chamfer(kit.arch(f"Band {side}", 0.28, 0.3, 0.05, (x, 0, 0.3), "iron", segments=8, leg_height=0.29,
                         rotation=(0, 0, math.pi / 2)), width=0.008)
        flat(kit.ring(f"Handle {side}", 0.06, 0.045, 0.018, (math.copysign(0.435, x), 0, 0.2), "iron", segments=8,
                      rotation=(0, math.pi / 2, 0)))
    for index, (sx, sy) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        kit.box(f"Foot {index + 1}", (0.1, 0.1, 0.03), (sx * 0.37, sy * 0.23, 0.015), "iron", bevel=0.01)

    kit.box("Lock plate", (0.13, 0.024, 0.15), (0, 0.29, 0.29), "brass", bevel=SMALL_BEVEL)
    flat(kit.cylinder("Keyhole boss", 0.022, 0.012, (0, 0.305, 0.27), "iron", segments=8,
                      rotation=(math.pi / 2, 0, 0)))
    kit.export_asset("treasure_chest", triangle_budget=1500, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def wall_sconce(kit):
    """0.6 m wall torch sconce: iron wall plate and bracket, cup, pitch-wrapped timber torch, flame."""
    kit.box("Wall plate", (0.12, 0.02, 0.28), (0, 0.01, 0.18), "iron", bevel=SMALL_BEVEL)
    chamfer(kit.cylinder("Plate finial", 0.03, 0.06, (0, 0.025, 0.03), "iron", segments=6, radius_top=0.012),
            width=0.006)
    kit.box("Bracket arm", (0.04, 0.2, 0.04), (0, 0.11, 0.2), "iron", bevel=0.012,
            rotation=(math.radians(18), 0, 0))
    chamfer(kit.cylinder("Cup", 0.04, 0.09, (0, 0.2, 0.27), "iron", segments=8, radius_top=0.068), width=0.01)
    flat(kit.ring("Cup collar", 0.074, 0.06, 0.02, (0, 0.2, 0.31), "iron", segments=8))
    chamfer(kit.cylinder("Torch", 0.026, 0.34, (0, 0.21, 0.34), "timber", segments=6,
                         rotation=(math.radians(-8), 0, 0)), width=0.008)
    flat(kit.cylinder("Pitch wrap", 0.04, 0.07, (0, 0.225, 0.49), "thatch", segments=6, radius_top=0.046,
                      rotation=(math.radians(-8), 0, 0)))
    flat(kit.cylinder("Flame", 0.052, 0.12, (0, 0.23, 0.54), "flame", segments=6, radius_top=0.0))
    flat(kit.cylinder("Flame tongue", 0.03, 0.08, (0.02, 0.24, 0.52), "flame", segments=5, radius_top=0.0,
                      rotation=(0.2, -0.35, 0.4)))
    kit.export_asset("wall_sconce", triangle_budget=500, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def barrel(kit):
    """0.9 m bellied barrel: faceted staves in three rings, iron hoops, end-grain head with a bung."""
    flat(kit.cylinder("Staves low", 0.3, 0.3, (0, 0, 0.15), "timber", segments=12, radius_top=0.36))
    flat(kit.cylinder("Staves belly", 0.36, 0.3, (0, 0, 0.45), "timber", segments=12))
    flat(kit.cylinder("Staves high", 0.36, 0.26, (0, 0, 0.73), "timber", segments=12, radius_top=0.3))
    for index, (z, inner) in enumerate(((0.07, 0.3), (0.27, 0.35), (0.63, 0.35), (0.83, 0.3))):
        flat(kit.ring(f"Hoop {index + 1}", inner + 0.022, inner, 0.035, (0, 0, z), "iron", segments=12))
    # The head stands 2.5 cm proud of the stave tops (0.86 m) and the bung 1 cm proud of the head, so
    # neither is buried in the staves nor coplanar with another top face.
    chamfer(kit.cylinder("Head", 0.285, 0.03, (0, 0, 0.87), "endgrain", segments=12), width=0.01)
    flat(kit.cylinder("Bung", 0.03, 0.02, (0.14, 0.08, 0.885), "endgrain", segments=6))
    kit.export_asset("barrel", triangle_budget=800, lod_ratio=LOD_RATIO, collision="hull",
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
    offsets = {"quest_signpost": 2.9, "village_well": 1.15, "treasure_chest": -0.55, "barrel": -1.75,
               "wall_sconce": -2.7}
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
    camera_data.lens = 32
    camera = bpy.data.objects.new("Preview camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = (0.3, 7.2, 3.3)
    camera.rotation_euler = (math.radians(73), 0.0, math.radians(180))

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
    kit = spark_kit.Kit(args.repo, "SparkGameRPG", __file__, PALETTE, short="RPG")
    kit.blend_path = kit.art_dir / "rpg_kit.blend"
    for build in (village_well, quest_signpost, treasure_chest, wall_sconce, barrel):
        build(kit)
    check_lod_limits(kit)
    kit.save_blend()
    kit.write_provenance()
    render_preview(kit.art_dir / "preview.png")


main()
