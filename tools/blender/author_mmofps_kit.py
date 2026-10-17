"""Author the SparkGameMMOFPS (TERRAFRONT) frontline-logistics kit; Spark Open License 1.0.

Art direction (owner-approved, "TERRAFRONT"): near-future frontline logistics -- olive composites,
orange safety accents, night-ops blue; angular armour plates, recessed panel lines, stencilled IDs,
modular 0.5 m grid. Every MTL diffuse is one of four exact palette colours; shades differ only by
roughness/metalness. Hard edges carry 2-4 cm bevels (thin trim is clamped by spark_kit to 45% of
its smallest side); round parts (mast, dish, beacon) stay faceted cylinders.

Props face Blender +Y, which the kit export (forward_axis='Z') writes as OBJ +Z, the direction the
art direction names. The pivot is the ground-contact centre (Blender z = 0).

Run (the Workbench preview needs an OpenGL context; on a display-less Linux host wrap Blender in xvfb-run):
  PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
    --python tools/blender/author_mmofps_kit.py -- --repo .
  python3 tools/blender/validate_kit.py Art/Blender/SparkGameMMOFPS/provenance.json
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


OLIVE = hex_rgb("#4B5320")
COMPOSITE = hex_rgb("#5E6468")
ORANGE = hex_rgb("#D9822B")
NIGHT = hex_rgb("#1D2733")

PALETTE = {
    "olive": (OLIVE, 0.72, 0.0),          # olive-drab composite armour
    "composite": (COMPOSITE, 0.66, 0.05),  # grey composite frame / deck
    "steel": (COMPOSITE, 0.36, 0.82),      # bare metal hardware (same grey, metallic)
    "orange": (ORANGE, 0.52, 0.0),         # safety accents and stencils
    "night": (NIGHT, 0.44, 0.3),           # night-ops blue panels, panel-line inlays
}

LOD_LIMIT = 0.40
COLLISION_CAP = 200
HALF_PI = math.pi / 2
BEVEL = 0.03


def octagon(width):
    """Flat-sided octagon profile (faces on +/-X and +/-Y) of the given across-flats width."""
    radius = width / 2 / math.cos(math.pi / 8)
    return [(radius * math.cos(math.pi / 8 + i * math.pi / 4), radius * math.sin(math.pi / 8 + i * math.pi / 4))
            for i in range(8)]


def prism(kit, name, width, z_min, z_max, mat, bevel=BEVEL):
    """Vertical octagonal armour prism from z_min to z_max (extruded profile turned upright)."""
    kit.extrude(name, octagon(width), z_max - z_min, (0, 0, (z_min + z_max) / 2), mat, bevel=bevel,
                rotation=(HALF_PI, 0, 0))


def stencil_id(kit, name, x, y, z, mat, scale=1.0):
    """Stencilled unit ID on a +Y face: a bar-code of short blocks (reads as painted numerals at range)."""
    widths = (0.04, 0.02, 0.04, 0.04, 0.02)
    offset = -0.13 * scale
    for index, width in enumerate(widths):
        kit.box(f"{name} {index + 1}", (width * scale, 0.012, 0.12 * scale), (x + offset + width * scale / 2, y, z), mat)
        offset += (width + 0.025) * scale


def supply_drop_pod(kit):
    """1.4 m wide, 2.4 m tall octagonal orbital drop pod: skirt, olive armour, orange band, night hatch."""
    prism(kit, "Landing skirt", 1.4, 0.0, 0.28, "composite")
    prism(kit, "Hull armour", 1.24, 0.28, 1.86, "olive")
    prism(kit, "Safety band", 1.28, 1.36, 1.5, "orange", bevel=0.02)
    prism(kit, "Panel line lower", 1.26, 0.72, 0.76, "night", bevel=0.01)
    prism(kit, "Nose plate", 1.0, 1.86, 2.1, "night")
    prism(kit, "Nose cap", 0.64, 2.1, 2.26, "composite")
    kit.box("Beacon housing", (0.2, 0.2, 0.1), (0, 0, 2.31), "steel", bevel=0.02)
    kit.cylinder("Beacon", 0.07, 0.04, (0, 0, 2.38), "orange", segments=8)
    for index in range(4):
        angle = math.pi / 4 + index * HALF_PI
        fin = [(0.0, 0.0), (0.26, 0.0), (0.06, 0.9), (0.0, 0.9)]
        kit.extrude(f"Landing fin {index + 1}", [(x + 0.54, z) for x, z in fin], 0.06, (0, 0, 0), "steel",
                    bevel=0.02, rotation=(0, 0, angle))
    # Front (+Y) cargo hatch with orange grab bar and stencilled ID.
    kit.box("Cargo hatch", (0.56, 0.04, 0.9), (0, 0.62, 1.0), "night", bevel=0.02)
    kit.box("Hatch grab bar", (0.3, 0.04, 0.05), (0, 0.65, 0.76), "orange", bevel=0.015)
    stencil_id(kit, "Hatch ID", 0.0, 0.648, 1.28, "orange")
    for side, x in (("L", -0.3), ("R", 0.3)):
        kit.box(f"Hatch hinge {side}", (0.05, 0.05, 0.18), (x, 0.63, 1.0), "steel", bevel=0.015)
    kit.export_asset("supply_drop_pod", triangle_budget=2200, lod_ratio=0.34, collision="hull",
                     collision_cap=COLLISION_CAP)


def deployable_barricade(kit):
    """2.4 x 1.2 m folding barricade: three olive armour plates on a grey frame, orange crest, night ID plate."""
    plate = [(-0.16, 0.08), (0.2, 0.08), (0.2, 0.2), (0.06, 1.14), (-0.06, 1.14), (-0.16, 0.9)]
    for index, x in enumerate((-0.8, 0.0, 0.8)):
        kit.extrude(f"Armour plate {index + 1}", plate, 0.76, (x, 0, 0), "olive", bevel=BEVEL,
                    rotation=(0, 0, HALF_PI))
    # Frame behind the plates: base rail, two uprights at the panel seams, top crest bar.
    kit.box("Base rail", (2.4, 0.5, 0.08), (0, -0.02, 0.04), "composite", bevel=0.025)
    for side, x in (("L", -0.4), ("R", 0.4)):
        kit.box(f"Seam post {side}", (0.05, 0.14, 1.06), (x, -0.06, 0.61), "night", bevel=0.02)
    kit.box("Crest bar", (2.4, 0.14, 0.06), (0, -0.0, 1.17), "orange", bevel=0.02)
    for side, x in (("L", -1.0), ("R", 1.0)):
        kit.box(f"Rear strut {side}", (0.06, 0.62, 0.06), (x, -0.36, 0.37), "steel", bevel=0.02,
                rotation=(0.9, 0, 0))
        kit.box(f"Foot pad {side}", (0.22, 0.16, 0.04), (x, -0.56, 0.02), "steel", bevel=0.015)
    # Front stencil plate on the centre panel (face leans back, so the plate follows its slope).
    kit.box("ID plate", (0.36, 0.03, 0.16), (0, 0.155, 0.42), "night", bevel=0.01, rotation=(0.15, 0, 0))
    stencil_id(kit, "Plate ID", 0.0, 0.18, 0.42, "orange", scale=0.8)
    for side, x in (("L", -1.12), ("R", 1.12)):
        kit.box(f"Reflector {side}", (0.1, 0.03, 0.06), (x, 0.19, 0.16), "orange", bevel=0.01)
    kit.export_asset("deployable_barricade", triangle_budget=1200, lod_ratio=0.34, collision="hull",
                     collision_cap=COLLISION_CAP)


def comms_relay(kit):
    """3.5 m field comms relay: olive equipment case, tripod, steel mast, night dish, orange tips and beacon."""
    kit.box("Equipment case", (0.8, 0.56, 0.5), (0, 0, 0.29), "olive", bevel=BEVEL)
    kit.box("Case skid", (0.9, 0.62, 0.04), (0, 0, 0.02), "composite", bevel=0.015)
    kit.box("Front panel", (0.56, 0.03, 0.3), (0, 0.285, 0.3), "night", bevel=0.01)
    kit.box("Status strip", (0.4, 0.035, 0.04), (0, 0.29, 0.48), "orange", bevel=0.008)
    stencil_id(kit, "Case ID", -0.02, 0.302, 0.28, "orange", scale=0.9)
    kit.box("Mast collar", (0.2, 0.2, 0.14), (0, 0, 0.6), "steel", bevel=0.02)
    kit.cylinder("Mast lower", 0.055, 1.5, (0, 0, 1.42), "steel", segments=8)
    kit.cylinder("Mast upper", 0.04, 1.3, (0, 0, 2.8), "steel", segments=8)
    for index in range(3):
        angle = HALF_PI + index * 2 * math.pi / 3
        foot = (0.95 * math.cos(angle), 0.95 * math.sin(angle))
        mid = (foot[0] / 2, foot[1] / 2)
        length = math.hypot(0.95, 1.1)
        tilt = math.atan2(0.95, 1.1)
        kit.box(f"Tripod leg {index + 1}", (0.05, 0.05, length), (mid[0], mid[1], 0.565), "composite",
                bevel=0.015, rotation=(tilt, 0, angle - HALF_PI))
        kit.box(f"Leg foot {index + 1}", (0.14, 0.14, 0.03), (foot[0], foot[1], 0.015), "steel", bevel=0.01)
    kit.box("Antenna crossbar", (1.1, 0.05, 0.05), (0, 0, 2.95), "composite", bevel=0.015)
    for side, x in (("L", -0.52), ("R", 0.52)):
        kit.cylinder(f"Whip {side}", 0.012, 0.5, (x, 0, 3.22), "steel", segments=6)
        kit.box(f"Whip tip {side}", (0.05, 0.05, 0.05), (x, 0, 3.48), "orange", bevel=0.01)
    kit.cylinder("Dish", 0.2, 0.08, (0, 0.14, 2.3), "night", segments=12, radius_top=0.34,
                 rotation=(-HALF_PI + 0.25, 0, 0))
    kit.cylinder("Dish feed", 0.02, 0.2, (0, 0.28, 2.33), "steel", segments=6, rotation=(-HALF_PI + 0.25, 0, 0))
    kit.box("Beacon", (0.08, 0.08, 0.1), (0, 0, 3.45), "orange", bevel=0.015)
    kit.export_asset("comms_relay", triangle_budget=1800, lod_ratio=0.34, collision="hull",
                     collision_cap=COLLISION_CAP)


def vehicle_pad(kit):
    """6 x 6 m vehicle spawn pad: composite slab, olive deck with night grid inlays, orange corner chevrons."""
    kit.box("Pad slab", (6.0, 6.0, 0.12), (0, 0, 0.06), "composite", bevel=0.04)
    kit.box("Deck plate", (5.4, 5.4, 0.04), (0, 0, 0.14), "olive", bevel=0.02)
    # Recessed panel lines on the 1.5 m grid (a multiple of the 0.5 m module).
    for index, offset in enumerate((-1.5, 0.0, 1.5)):
        kit.box(f"Grid line X {index + 1}", (0.05, 5.4, 0.012), (offset, 0, 0.158), "night")
        kit.box(f"Grid line Y {index + 1}", (5.4, 0.05, 0.012), (0, offset, 0.158), "night")
    # Each L starts at an outer deck corner with its arms running inward along the edges.
    for index, (sx, sy) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        chevron = [(0.0, 0.0), (0.9, 0.0), (0.9, 0.16), (0.16, 0.16), (0.16, 0.9), (0.0, 0.9)]
        kit.extrude(f"Corner chevron {index + 1}", chevron, 0.03, (2.6 * sx, 2.6 * sy, 0.155), "orange",
                    rotation=(HALF_PI, 0, (index - 1) * HALF_PI))
    # Drive-on arrow pointing +Y (the pad front) and a stencilled bay ID.
    arrow = [(0.0, 1.1), (0.55, 0.45), (0.2, 0.45), (0.2, -0.6), (-0.2, -0.6), (-0.2, 0.45), (-0.55, 0.45)]
    kit.extrude("Drive arrow", [(x, -y) for x, y in arrow], 0.02, (0, 0.6, 0.165), "orange",
                rotation=(HALF_PI, 0, 0))
    kit.box("Bay ID plate", (0.8, 0.3, 0.02), (0, -1.9, 0.165), "night")
    kit.export_asset("vehicle_pad", triangle_budget=900, lod_ratio=0.34, collision="box",
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
    offsets = {"supply_drop_pod": 6.0, "deployable_barricade": 3.6, "comms_relay": 1.0, "vehicle_pad": -3.6}
    for name, offset in offsets.items():
        for obj in bpy.data.collections[name].objects:
            obj.location.x += offset
    # Show the MTL colours as authored sRGB in the viewport shading.
    for mat in bpy.data.materials:
        mat.diffuse_color = (*(srgb_to_linear(c) for c in mat.diffuse_color[:3]), 1.0)

    kit.box("Preview ground", (120.0, 90.0, 0.02), (0, 0, -0.01), "composite")
    ground = kit._parts.pop()
    ground_mat = bpy.data.materials.new("Preview_ground")
    ground_mat.diffuse_color = (0.42, 0.42, 0.42, 1.0)
    ground.data.materials[0] = ground_mat

    camera_data = bpy.data.cameras.new("Preview camera")
    camera_data.lens = 35
    camera = bpy.data.objects.new("Preview camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = (0.0, 15.5, 7.0)
    camera.rotation_euler = (math.radians(69), 0.0, math.radians(180))

    scene = bpy.context.scene
    scene.camera = camera
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x, scene.render.resolution_y = 1600, 900
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.image_settings.compression = 100
    scene.render.use_stamp = False
    # use_stamp only stops burn-in; each enabled field is still written as PNG metadata, and the
    # Date and RenderTime fields change every run, so turn them all off for a byte-stable preview.
    for prop in scene.render.bl_rna.properties:
        if prop.identifier.startswith("use_stamp_") and not prop.is_readonly:
            setattr(scene.render, prop.identifier, False)
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
    kit = spark_kit.Kit(args.repo, "SparkGameMMOFPS", __file__, PALETTE, short="MMOFPS")
    kit.blend_path = kit.art_dir / "mmofps_kit.blend"
    for build in (supply_drop_pod, deployable_barricade, comms_relay, vehicle_pad):
        build(kit)
    check_lod_limits(kit)
    kit.save_blend()
    kit.write_provenance()
    render_preview(kit, kit.art_dir / "preview.png")


main()
