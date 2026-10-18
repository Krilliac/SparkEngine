"""Author the SparkGameRTS "Real-Time Strategy" kit with Blender 4.0.2; Spark Open License 1.0.

Art direction (owner-approved): readable from a high camera, neutral stone and iron with bold faction
colours. Stepped bases, big roof planes, faction-colour banners and trims. Every MTL diffuse is one of
the exact palette colours (Azure #2F6FD6 faction 1, Crimson #C8352F faction 2, Stone #8B8680); iron,
dressed stone and crystal are Stone shades that differ only by roughness and metalness. Faceted flat
shading, 3 cm chamfers on hard edges (flush trims under 2 cm and the 4 cm marker ring stay square),
3-5 materials per prop, no texture maps.

The faction-coloured props (command_center, barracks, rally_flag) are authored once per faction the
skirmish fields: the listed stem in Azure for the Human player (faction 1) and a *_crimson variant for
the Swarm opponent (faction 2, the red the RTS Battlefield panel already draws it in). The Verdant
faction 3 (Sentinel) never appears in the demo skirmish, so no variant is exported for it.
unit_marker is the player's selection ring and exists in Azure only.

Props face Blender +Y, which the kit export (forward_axis='Z') writes as OBJ +Z. The pivot is the
ground-contact centre (Blender z = 0). RTSDemoPresentation places the kit on the skirmish map at
2.5 m per grid cell, so a 4 x 4 cell building footprint is the command center's 10 x 10 m.

Run (the Workbench preview needs an OpenGL context; on a display-less Linux host wrap Blender in xvfb-run):
  PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
    --python tools/blender/author_rts_kit.py -- --repo .
  python3 tools/blender/validate_kit.py Art/Blender/SparkGameRTS/provenance.json
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


AZURE = hex_rgb("#2F6FD6")
CRIMSON = hex_rgb("#C8352F")
STONE = hex_rgb("#8B8680")

PALETTE = {
    "stone": (STONE, 0.9, 0.0),            # rough-hewn stepped bases, rocks
    "stone_dressed": (STONE, 0.62, 0.0),   # smoothed ashlar walls
    "iron": (STONE, 0.38, 0.85),           # doors, poles, bands, finials
    "crystal": (STONE, 0.12, 0.95),        # polished ore crystals (resources stay faction-neutral)
    "azure_cloth": (AZURE, 0.72, 0.0),     # faction 1 banners and flags
    "azure_trim": (AZURE, 0.34, 0.55),     # faction 1 lacquered roofs and trims
    "crimson_cloth": (CRIMSON, 0.72, 0.0),
    "crimson_trim": (CRIMSON, 0.34, 0.55),
}

FACTIONS = (("", "azure"), ("_crimson", "crimson"))  # (file suffix, palette prefix)
BEVEL = 0.03          # chamfer on hard edges (art direction: 2-4 cm)
LOD_RATIO = 0.36      # LOD1 must stay at or under 40% of its source
LOD_LIMIT = 0.40
COLLISION_CAP = 200
HALF_PI = math.pi / 2


def flat(obj):
    """Faceted flat shading (the library smooths cylinder and ring sides)."""
    for polygon in obj.data.polygons:
        polygon.use_smooth = False
    return obj


def chamfer(obj, width=BEVEL, min_angle=math.radians(50)):
    """Flat-shade and chamfer only the hard edges (face angle >= min_angle) of a built part."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    hard = [edge for edge in bm.edges if edge.is_manifold and edge.calc_face_angle(0.0) >= min_angle]
    bmesh.ops.bevel(bm, geom=hard, offset=width, segments=1, profile=0.5, affect="EDGES", clamp_overlap=True)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(obj.data)
    bm.free()
    return flat(obj)


def hip_roof(kit, name, width, depth, height, ridge, z, mat, location=(0.0, 0.0)):
    """Hipped roof: a width x depth eave rectangle at z rising to a ridge of length ridge along X."""
    bm = bmesh.new()
    w, d = width / 2, depth / 2
    base = [bm.verts.new(co) for co in ((-w, -d, 0), (w, -d, 0), (w, d, 0), (-w, d, 0))]
    left, right = bm.verts.new((-ridge / 2, 0, height)), bm.verts.new((ridge / 2, 0, height))
    bm.faces.new(base[::-1])
    bm.faces.new((base[0], base[1], right, left))
    bm.faces.new((base[1], base[2], right))
    bm.faces.new((base[2], base[3], left, right))
    bm.faces.new((base[3], base[0], left))
    return chamfer(kit._add(name, bm, mat, (location[0], location[1], z), (0, 0, 0)), min_angle=math.radians(30))


def gable_roof(kit, name, length, span, height, z, mat, eave=0.22):
    """Solid gable roof with its ridge along X: span across Y, eave-thick edges, bevelled."""
    s = span / 2
    profile = [(-s, 0.0), (s, 0.0), (s, eave), (0.0, height), (-s, eave)]
    # extrude() profiles lie in local XZ and extrude along local Y; a quarter turn about Z runs the ridge along X.
    return flat(kit.extrude(name, profile, length, (0, 0, z), mat, bevel=BEVEL, rotation=(0, 0, HALF_PI)))


def stepped_base(kit, prefix, steps, mat="stone"):
    """Stacked, bevelled plinths [(width, depth, height), ...] from the ground up; returns the top z."""
    z = 0.0
    for index, (width, depth, height) in enumerate(steps):
        kit.box(f"{prefix} step {index + 1}", (width, depth, height), (0, 0, z + height / 2), mat, bevel=BEVEL)
        z += height
    return z


def banner(kit, name, width, height, top, x, y, faction, yaw=0.0):
    """Hanging cloth banner with a notched foot and an iron hanging rod, front face toward +Y."""
    w = width / 2
    profile = [(-w, 0.0), (w, 0.0), (w, -height), (0.0, -height + width * 0.45), (-w, -height)]
    kit.extrude(f"{name} cloth", profile, 0.05, (x, y, top), f"{faction}_cloth", bevel=0.02, rotation=(0, 0, yaw))
    flat(kit.cylinder(f"{name} rod", 0.035, width + 0.24, (x, y, top + 0.03), "iron", segments=6,
                      rotation=(0, HALF_PI, yaw)))


def crystal(kit, name, radius, height, root, rotation):
    """Six-sided crystal: a prism with a pyramidal point, rooted at root and height long along local Z."""
    bm = bmesh.new()
    shaft = height * 0.72
    rings = [[bm.verts.new((radius * math.cos(math.pi * i / 3), radius * math.sin(math.pi * i / 3), z))
              for i in range(6)] for z in (0.0, shaft)]
    point = bm.verts.new((0.0, 0.0, height))
    bm.faces.new(rings[0][::-1])
    for i in range(6):
        j = (i + 1) % 6
        bm.faces.new((rings[0][i], rings[0][j], rings[1][j], rings[1][i]))
        bm.faces.new((rings[1][i], rings[1][j], point))
    return flat(kit._add(name, bm, "crystal", root, rotation))


def command_center(kit, suffix, faction):
    """10 x 10 m keep: two-step stone base, ashlar hall, four towers, a faction hip roof and banners."""
    top = stepped_base(kit, "Base", ((10.0, 10.0, 0.45), (9.0, 9.0, 0.4)))
    hall_height = 3.4
    kit.box("Hall", (7.0, 7.0, hall_height), (0, 0, top + hall_height / 2), "stone_dressed", bevel=BEVEL)
    eave = top + hall_height
    kit.box("Eave trim", (7.4, 7.4, 0.26), (0, 0, eave + 0.13), f"{faction}_trim", bevel=BEVEL)
    hip_roof(kit, "Great roof", 7.8, 7.8, 2.6, 2.2, eave + 0.26, f"{faction}_trim")
    flat(kit.cylinder("Roof finial", 0.12, 1.3, (0, 0, eave + 0.26 + 2.6 + 0.6), "iron", segments=6,
                      radius_top=0.03))

    for index, (sx, sy) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        x, y = sx * 3.65, sy * 3.65
        tower_height = 5.0
        chamfer(kit.cylinder(f"Tower {index + 1}", 0.95, tower_height, (x, y, top + tower_height / 2), "stone",
                             segments=8))
        chamfer(kit.cylinder(f"Tower band {index + 1}", 1.05, 0.3, (x, y, top + tower_height - 0.15),
                             f"{faction}_trim", segments=8))
        flat(kit.cylinder(f"Tower roof {index + 1}", 1.15, 1.9, (x, y, top + tower_height + 0.95), f"{faction}_trim",
                          segments=8, radius_top=0.04))

    # Front (+Y): gate arch with iron doors, three steps up to the base and two faction banners.
    front = 3.5
    chamfer(kit.arch("Gate arch", 0.95, 1.35, 0.5, (0, front + 0.05, top + 1.4), "stone", segments=8,
                     leg_height=1.4))
    kit.box("Gate doors", (1.9, 0.12, 1.4), (0, front + 0.02, top + 0.7), "iron", bevel=BEVEL)
    kit.extrude("Gate doors crown", [(-0.95, 0.0), (0.95, 0.0), (0.0, 0.9)], 0.12, (0, front + 0.02, top + 1.4),
                "iron", bevel=0.02)
    kit.box("Front stair", (2.6, 0.5, 0.2), (0, 4.75, 0.55), "stone", bevel=BEVEL)
    for side, sx in (("left", -1), ("right", 1)):
        banner(kit, f"Banner {side}", 1.0, 2.2, eave - 0.3, sx * 2.15, front + 0.05, faction)
    kit.export_asset(f"command_center{suffix}", triangle_budget=3000, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def barracks(kit, suffix, faction):
    """8 x 6 m drill hall: stepped base, long ashlar hall under a faction gable roof, gate and banners."""
    top = stepped_base(kit, "Base", ((8.0, 6.0, 0.35), (7.4, 5.4, 0.3)))
    hall_height = 2.7
    kit.box("Hall", (6.8, 4.4, hall_height), (0, 0, top + hall_height / 2), "stone_dressed", bevel=BEVEL)
    for index, sx in enumerate((-1, 1)):
        kit.box(f"Buttress {index + 1}", (0.7, 5.0, hall_height - 0.3), (sx * 3.2, 0, top + (hall_height - 0.3) / 2),
                "stone", bevel=BEVEL)
    eave = top + hall_height
    kit.box("Eave trim", (7.0, 4.6, 0.22), (0, 0, eave + 0.11), f"{faction}_trim", bevel=BEVEL)
    gable_roof(kit, "Gable roof", 7.6, 5.6, 2.0, eave + 0.22, f"{faction}_trim")
    flat(kit.cylinder("Ridge beam", 0.1, 7.7, (0, 0, eave + 0.22 + 2.0), "iron", segments=6,
                      rotation=(0, HALF_PI, 0)))
    for index, sx in enumerate((-1, 1)):
        chamfer(kit.box(f"Chimney {index + 1}", (0.55, 0.55, 1.6), (sx * 2.4, -1.0, eave + 1.6), "stone"))

    # Front (+Y): wide iron gate under a stone arch, a step, and banners on the buttresses.
    front = 2.2
    chamfer(kit.arch("Gate arch", 0.85, 1.2, 0.4, (0, front + 0.05, top + 1.15), "stone", segments=8,
                     leg_height=1.15))
    kit.box("Gate doors", (1.7, 0.1, 1.15), (0, front + 0.02, top + 0.575), "iron", bevel=BEVEL)
    kit.extrude("Gate doors crown", [(-0.85, 0.0), (0.85, 0.0), (0.0, 0.8)], 0.1, (0, front + 0.02, top + 1.15),
                "iron", bevel=0.02)
    kit.box("Front stair", (2.4, 0.4, 0.3), (0, 2.8, 0.15), "stone", bevel=BEVEL)
    for side, sx in (("left", -1), ("right", 1)):
        banner(kit, f"Banner {side}", 0.6, 1.8, eave - 0.2, sx * 3.2, 2.55, faction)
    kit.export_asset(f"barracks{suffix}", triangle_budget=2400, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def resource_node(kit):
    """3 m ore cluster: a rock bed of faceted boulders pierced by tall polished crystals (tallest 3.0 m)."""
    rng = random.Random(7303)
    for index, (x, y, radius, height) in enumerate(((0.0, 0.0, 1.25, 0.55), (0.9, -0.55, 0.6, 0.45),
                                                    (-0.95, 0.45, 0.55, 0.4), (-0.35, -0.95, 0.45, 0.35))):
        chamfer(kit.cylinder(f"Boulder {index + 1}", radius, height, (x, y, height / 2), "stone", segments=7,
                             radius_top=radius * 0.7, rotation=(0, 0, rng.uniform(0, math.pi))))
    for index, (x, y, height, radius) in enumerate(((0.0, 0.05, 3.0, 0.34), (0.55, 0.35, 2.1, 0.26),
                                                    (-0.5, 0.25, 1.9, 0.25), (0.25, -0.5, 1.6, 0.22),
                                                    (-0.3, -0.35, 1.35, 0.2), (0.85, -0.2, 1.1, 0.18),
                                                    (-0.9, -0.1, 0.95, 0.17))):
        tilt = (0.0, 0.0) if index == 0 else (rng.uniform(-0.3, 0.3), rng.uniform(-0.3, 0.3))
        # Roots sit 12 cm up inside the rock bed so tilted crystals never dip below the ground.
        crystal(kit, f"Crystal {index + 1}", radius, height - 0.12, (x, y, 0.12), (*tilt, rng.uniform(0, math.pi)))
    for index, (x, y) in enumerate(((1.1, 0.6), (-1.15, -0.55), (0.3, 1.2))):
        chamfer(kit.box(f"Shard {index + 1}", (0.3, 0.22, 0.26), (x, y, 0.13), "stone_dressed",
                        rotation=(0, 0, rng.uniform(0, math.pi))))
    kit.export_asset("resource_node", triangle_budget=1600, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def rally_flag(kit, suffix, faction):
    """3.0 m rally flag: stepped stone foot, iron pole with a faction band, swallowtail flag, spear finial."""
    top = stepped_base(kit, "Foot", ((0.7, 0.7, 0.16), (0.46, 0.46, 0.14)))
    pole = 2.44  # foot 0.30 + pole + 0.26 finial = 3.0 m
    flat(kit.cylinder("Pole", 0.045, pole, (0, 0, top + pole / 2), "iron", segments=6))
    flat(kit.cylinder("Pole band", 0.07, 0.12, (0, 0, top + 0.5), f"{faction}_trim", segments=6))
    flat(kit.cylinder("Finial", 0.07, 0.26, (0, 0, top + pole + 0.13), "iron", segments=4, radius_top=0.005))
    # Swallowtail flag flies toward +X from the pole top; its faces point along +Y (the prop's front).
    flag = [(0.04, 0.0), (1.05, 0.0), (0.78, -0.33), (1.05, -0.66), (0.04, -0.66)]
    kit.extrude("Flag", flag, 0.03, (0, 0, top + pole - 0.08), f"{faction}_cloth", bevel=0.01)
    kit.export_asset(f"rally_flag{suffix}", triangle_budget=400, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def unit_marker(kit):
    """1.0 m selection ring: a faction band, four inward chevrons and iron studs, 4 cm tall."""
    flat(kit.ring("Ring", 0.5, 0.43, 0.04, (0, 0, 0.02), "azure_trim", segments=20))
    for index in range(4):
        angle = HALF_PI + index * HALF_PI  # the first chevron marks the front (+Y)
        radius = 0.36
        # A +90 degree turn about X lays the extruded profile flat (outline (x, -z)); the yaw points it inward.
        kit.extrude(f"Chevron {index + 1}", [(-0.07, 0.0), (0.07, 0.0), (0.0, -0.09)], 0.03,
                    (radius * math.cos(angle), radius * math.sin(angle), 0.015), "azure_cloth",
                    rotation=(HALF_PI, 0, angle + HALF_PI))
        stud = angle + math.pi / 4
        kit.box(f"Stud {index + 1}", (0.05, 0.05, 0.05), (0.465 * math.cos(stud), 0.465 * math.sin(stud), 0.025),
                "iron", rotation=(0, 0, stud))
    kit.export_asset("unit_marker", triangle_budget=300, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def check_lod_limits(kit):
    for record in kit.records:
        lod = record["variants"]["lod1"]["triangle_count"]
        if lod > LOD_LIMIT * record["triangle_count"]:
            raise RuntimeError(f"{record['name']}: LOD1 {lod} exceeds {LOD_LIMIT:.0%} of {record['triangle_count']}")


def srgb_to_linear(channel):
    return channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4


def render_preview(path):
    """Workbench preview of the kit (after the .blend is saved; never written back to it).

    Front row: the Azure (faction 1) props and the neutral resource node; back row: the Crimson variants.
    The camera looks down -Y at the props' fronts from an RTS-like height, so +X is screen left.
    """
    layout = {"command_center": (9.5, 0.0), "barracks": (-0.5, 0.0), "resource_node": (-7.0, 0.0),
              "rally_flag": (-10.5, 0.0), "unit_marker": (-12.8, 0.8),
              "command_center_crimson": (9.5, -16.0), "barracks_crimson": (-0.5, -16.0),
              "rally_flag_crimson": (-10.5, -16.0)}
    for name, (dx, dy) in layout.items():
        for obj in bpy.data.collections[name].objects:
            obj.location.x += dx
            obj.location.y += dy
    for mat in bpy.data.materials:
        mat.diffuse_color = (*(srgb_to_linear(c) for c in mat.diffuse_color[:3]), 1.0)

    ground_mesh = bpy.data.meshes.new("Preview ground")
    ground_mesh.from_pydata([(-80, -60, 0), (80, -60, 0), (80, 40, 0), (-80, 40, 0)], [], [(0, 1, 2, 3)])
    ground = bpy.data.objects.new("Preview ground", ground_mesh)
    ground_mat = bpy.data.materials.new("Preview_ground")
    ground_mat.diffuse_color = (0.2, 0.2, 0.2, 1.0)
    ground_mesh.materials.append(ground_mat)
    bpy.context.scene.collection.objects.link(ground)

    camera_data = bpy.data.cameras.new("Preview camera")
    camera_data.lens = 38
    camera = bpy.data.objects.new("Preview camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = (0.8, 29.0, 27.0)
    camera.rotation_euler = (math.radians(54.5), 0.0, math.radians(180))

    scene = bpy.context.scene
    scene.camera = camera
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x, scene.render.resolution_y = 1600, 900
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.image_settings.compression = 100
    scene.render.use_stamp = False
    scene.render.dither_intensity = 0.0  # dither noise inflates the PNG
    scene.display.render_aa = "8"
    scene.display_settings.display_device = "sRGB"
    scene.view_settings.view_transform = "Standard"
    shading = scene.display.shading
    shading.light = "STUDIO"
    shading.studiolight_rotate_z = 0.0
    shading.color_type = "MATERIAL"
    shading.show_shadows = True
    shading.shadow_intensity = 0.35
    shading.show_cavity = False  # screen-space cavity adds per-pixel noise to the PNG
    shading.background_type = "VIEWPORT"
    shading.background_color = (0.62, 0.64, 0.66)
    scene.display.light_direction = (0.45, 0.35, 0.82)
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    if not path.is_file():
        raise RuntimeError(f"Workbench preview was not written to {path}")


def main():
    args = spark_kit.parse_args()
    kit = spark_kit.Kit(args.repo, "SparkGameRTS", __file__, PALETTE, short="RTS")
    kit.blend_path = kit.art_dir / "rts_kit.blend"
    for suffix, faction in FACTIONS:
        command_center(kit, suffix, faction)
        barracks(kit, suffix, faction)
        rally_flag(kit, suffix, faction)
    resource_node(kit)
    unit_marker(kit)
    check_lod_limits(kit)
    kit.save_blend()
    kit.write_provenance()
    render_preview(kit.art_dir / "preview.png")


main()
