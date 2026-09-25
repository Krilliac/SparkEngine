"""Author the SparkGameFPS industrial training-arena kit; Spark Open License 1.0.

Art direction (owner-approved): poured concrete, painted hazard markings and cover that reads at a
glance; heavy slabs, chunky 2-4 cm chamfers, stencil-stripe bands, cover heights of 1.0 m (crouch).
Every MTL diffuse is one of four exact palette colours; shades differ only by roughness/metalness.

Props face Blender +Y, which the kit export (forward_axis='Z') writes as OBJ +Z, the direction the
art direction names. The pivot is the ground-contact centre (Blender z = 0).

Run (the Workbench preview needs an OpenGL context; on a display-less Linux host wrap Blender in xvfb-run):
  PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
    --python tools/blender/author_fps_kit.py -- --repo .
  python3 tools/blender/validate_kit.py Art/Blender/SparkGameFPS/provenance.json
"""
import math
from pathlib import Path
import sys

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spark_kit  # noqa: E402


def hex_rgb(value):
    """#RRGGBB as the 0-1 triple written verbatim to the MTL Kd line."""
    return tuple(round(int(value[i:i + 2], 16) / 255.0, 6) for i in (1, 3, 5))


CONCRETE = hex_rgb("#8C8A84")
HAZARD = hex_rgb("#E3B23C")
GUNMETAL = hex_rgb("#3A3F45")
SIGNAL = hex_rgb("#C8453A")

PALETTE = {
    "concrete": (CONCRETE, 0.92, 0.0),
    "hazard": (HAZARD, 0.62, 0.0),
    "gunmetal": (GUNMETAL, 0.38, 0.85),
    "gunmetal_paint": (GUNMETAL, 0.7, 0.15),
    "signal": (SIGNAL, 0.5, 0.0),
}

LOD_LIMIT = 0.40
COLLISION_CAP = 200
HALF_PI = math.pi / 2


def stripes(kit, name, x_min, x_max, z_min, height, depth, mat, width=0.1, pitch=0.24, y=0.0):
    """Diagonal stencil stripes (parallelograms in the XZ plane) through depth along Y, kept inside [x_min, x_max]."""
    x = x_min
    index = 0
    while x + width + height <= x_max + 1e-6:
        profile = [(x, z_min), (x + width, z_min), (x + width + height, z_min + height), (x + height, z_min + height)]
        kit.extrude(f"{name} {index + 1}", profile, depth, (0, y, 0), mat)
        x += pitch
        index += 1


def cover_barrier(kit):
    """2.0 x 1.0 x 0.5 m poured-concrete jersey slab at crouch-cover height with a hazard band."""
    profile = [(-0.24, 0.0), (0.24, 0.0), (0.24, 0.1), (0.17, 0.3), (0.13, 1.0),
               (-0.13, 1.0), (-0.17, 0.3), (-0.24, 0.1)]
    kit.extrude("Barrier slab", profile, 1.98, (0, 0, 0), "concrete", bevel=0.03, rotation=(0, 0, HALF_PI))
    kit.box("Hazard band", (2.0, 0.304, 0.16), (0, 0, 0.78), "hazard", bevel=0.02)
    stripes(kit, "Band stripe", -0.9, 0.9, 0.7, 0.16, 0.312, "gunmetal_paint", width=0.1, pitch=0.26)
    for side, x in (("L", -0.55), ("R", 0.55)):
        kit.box(f"Fork pocket {side}", (0.32, 0.5, 0.08), (x, 0, 0.05), "gunmetal", bevel=0.02)
    kit.export_asset("cover_barrier", triangle_budget=800, lod_ratio=0.36, collision="hull",
                     collision_cap=COLLISION_CAP)


def ammo_crate(kit):
    """0.6 x 0.4 x 0.35 m steel ammo crate with stencil band, signal latch and end handles."""
    for side, y in (("F", 0.13), ("B", -0.13)):
        kit.box(f"Skid {side}", (0.56, 0.06, 0.03), (0, y, 0.015), "gunmetal", bevel=0.01)
    kit.box("Crate body", (0.56, 0.38, 0.27), (0, 0, 0.155), "gunmetal_paint", bevel=0.025)
    kit.box("Crate lid", (0.6, 0.4, 0.07), (0, 0, 0.315), "gunmetal", bevel=0.025, bevel_segments=2)
    kit.box("Stencil band", (0.44, 0.39, 0.08), (0, 0, 0.16), "hazard", bevel=0.005)
    stripes(kit, "Stencil stripe", -0.21, 0.21, 0.12, 0.08, 0.396, "gunmetal_paint", width=0.035, pitch=0.1)
    kit.box("Signal latch", (0.09, 0.02, 0.06), (0, 0.19, 0.29), "signal", bevel=0.008)
    for side, x in (("L", -0.29), ("R", 0.29)):
        kit.box(f"Handle {side}", (0.02, 0.16, 0.035), (x, 0, 0.21), "gunmetal", bevel=0.008)
    kit.export_asset("ammo_crate", triangle_budget=700, lod_ratio=0.36, collision="box",
                     collision_cap=COLLISION_CAP)


def spawn_pad(kit):
    """2.0 m concrete spawn disc with a striped hazard ring, gunmetal emitter and +Z facing arrow."""
    kit.cylinder("Pad base", 1.0, 0.1, (0, 0, 0.05), "concrete", segments=32)
    kit.cylinder("Pad chamfer", 1.0, 0.03, (0, 0, 0.115), "concrete", segments=32, radius_top=0.97)
    kit.ring("Stripe track", 0.9, 0.76, 0.02, (0, 0, 0.135), "gunmetal_paint", segments=32)
    segments = 16
    for index in range(segments):
        angle = 2 * math.pi * (index + 0.5) / segments
        kit.box(f"Hazard block {index + 1}", (0.17, 0.13, 0.024), (0.83 * math.cos(angle), 0.83 * math.sin(angle), 0.137),
                "hazard", rotation=(0, 0, angle + HALF_PI))
    kit.cylinder("Emitter", 0.42, 0.04, (0, 0, 0.14), "gunmetal", segments=24)
    kit.ring("Signal ring", 0.3, 0.24, 0.016, (0, 0, 0.166), "signal", segments=24)
    arrow = [(0.0, 0.62), (0.16, 0.48), (0.07, 0.48), (0.07, 0.36), (-0.07, 0.36), (-0.07, 0.48), (-0.16, 0.48)]
    kit.extrude("Facing arrow", [(x, -y) for x, y in arrow], 0.02, (0, 0, 0.14), "signal",
                rotation=(HALF_PI, 0, 0))
    kit.export_asset("spawn_pad", triangle_budget=1100, lod_ratio=0.34, collision="hull",
                     collision_cap=COLLISION_CAP)


def target_dummy(kit):
    """1.8 m pop-up training dummy: concrete weight, gunmetal post, hazard torso and signal target rings."""
    kit.cylinder("Weight", 0.34, 0.08, (0, 0, 0.04), "concrete", segments=20)
    kit.cylinder("Weight chamfer", 0.34, 0.03, (0, 0, 0.095), "concrete", segments=20, radius_top=0.31)
    kit.cylinder("Post", 0.045, 0.82, (0, 0, 0.51), "gunmetal", segments=10)
    kit.box("Hip clamp", (0.16, 0.1, 0.08), (0, 0, 0.88), "gunmetal", bevel=0.02)
    torso = [(-0.2, 0.9), (0.2, 0.9), (0.26, 1.3), (0.24, 1.42), (0.12, 1.47),
             (-0.12, 1.47), (-0.24, 1.42), (-0.26, 1.3)]
    kit.extrude("Torso plate", torso, 0.08, (0, 0, 0), "hazard", bevel=0.02)
    stripes(kit, "Waist stripe", -0.18, 0.18, 0.93, 0.07, 0.084, "gunmetal_paint", width=0.035, pitch=0.09)
    for side, x, tilt in (("L", -0.3, -0.12), ("R", 0.3, 0.12)):
        kit.box(f"Arm {side}", (0.08, 0.07, 0.42), (x, 0, 1.17), "gunmetal_paint", bevel=0.02, rotation=(0, tilt, 0))
    kit.cylinder("Neck", 0.04, 0.12, (0, 0, 1.51), "gunmetal", segments=10)
    kit.box("Head plate", (0.22, 0.08, 0.24), (0, 0, 1.68), "hazard", bevel=0.03)
    kit.box("Visor", (0.16, 0.086, 0.04), (0, 0, 1.7), "gunmetal", bevel=0.01)
    kit.ring("Chest ring", 0.12, 0.085, 0.012, (0, 0.044, 1.2), "signal", segments=20, rotation=(HALF_PI, 0, 0))
    kit.cylinder("Chest bullseye", 0.04, 0.012, (0, 0.044, 1.2), "signal", segments=12, rotation=(HALF_PI, 0, 0))
    kit.export_asset("target_dummy", triangle_budget=1600, lod_ratio=0.36, collision="hull",
                     collision_cap=COLLISION_CAP)


def weapon_rack(kit):
    """1.6 x 1.2 m weapon rack on a concrete plinth: gunmetal frame, hazard comb bar and signal tags."""
    kit.box("Plinth", (1.6, 0.48, 0.14), (0, 0, 0.07), "concrete", bevel=0.03)
    stripes(kit, "Plinth stripe", -0.76, 0.76, 0.03, 0.08, 0.49, "hazard", width=0.06, pitch=0.18)
    for side, x in (("L", -0.76), ("R", 0.76)):
        kit.box(f"Upright {side}", (0.08, 0.1, 1.02), (x, 0, 0.65), "gunmetal", bevel=0.02)
        kit.box(f"Signal tag {side}", (0.05, 0.02, 0.05), (x, 0.06, 1.05), "signal", bevel=0.006)
    kit.box("Top rail", (1.6, 0.12, 0.06), (0, 0, 1.17), "gunmetal", bevel=0.02)
    kit.box("Back panel", (1.44, 0.03, 0.86), (0, -0.08, 0.63), "gunmetal_paint", bevel=0.01)
    kit.box("Butt tray", (1.44, 0.26, 0.05), (0, 0.02, 0.165), "gunmetal", bevel=0.02)
    kit.box("Comb bar", (1.44, 0.08, 0.05), (0, 0.02, 0.88), "hazard", bevel=0.02)
    for index in range(9):
        x = -0.64 + index * 0.16
        kit.box(f"Comb tooth {index + 1}", (0.03, 0.1, 0.08), (x, 0.03, 0.93), "gunmetal", bevel=0.008)
    kit.export_asset("weapon_rack", triangle_budget=1400, lod_ratio=0.36, collision="box",
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
    offsets = {"cover_barrier": 3.9, "ammo_crate": 2.05, "spawn_pad": 0.15, "target_dummy": -1.75, "weapon_rack": -3.6}
    for name, offset in offsets.items():
        for obj in bpy.data.collections[name].objects:
            obj.location.x += offset
    # Show the MTL colours as authored sRGB in the viewport shading.
    for mat in bpy.data.materials:
        mat.diffuse_color = (*(srgb_to_linear(c) for c in mat.diffuse_color[:3]), 1.0)

    kit.box("Preview ground", (40.0, 30.0, 0.02), (0, 0, -0.01), "gunmetal_paint")
    ground = kit._parts.pop()
    ground_mat = bpy.data.materials.new("Preview_ground")
    ground_mat.diffuse_color = (0.42, 0.42, 0.42, 1.0)
    ground.data.materials[0] = ground_mat

    camera_data = bpy.data.cameras.new("Preview camera")
    camera_data.lens = 35
    camera = bpy.data.objects.new("Preview camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = (0.15, 9.4, 3.3)
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


def main():
    args = spark_kit.parse_args()
    kit = spark_kit.Kit(args.repo, "SparkGameFPS", __file__, PALETTE, short="FPS")
    kit.blend_path = kit.art_dir / "fps_kit.blend"
    for build in (cover_barrier, ammo_crate, spawn_pad, target_dummy, weapon_rack):
        build(kit)
    check_lod_limits(kit)
    kit.save_blend()
    kit.write_provenance()
    render_preview(kit, kit.art_dir / "preview.png")


main()
