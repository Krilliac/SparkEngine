"""Author the SparkGame "Engine Showcase" prop kit with Blender 4.0.2; Spark Open License 1.0.

Art direction (owner-approved): a clean exhibition hall. Neutral graphite/steel/porcelain surfaces,
one warm Spark-amber accent per prop; chamfered rectangles, thin structural ribs and soft 45-degree
bevels (2-4 cm, one segment) on every hard edge; faceted flat shading; colour from MTL base colour plus
roughness/metalness only. Meters, ground-contact pivot, props face +Z in the engine (Blender +Y here,
see tools/blender/spark_kit.py for the axis mapping).

Run (xvfb-run supplies the GL context the Workbench preview needs on a host without EGL):
    PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
        --python tools/blender/author_showcase_kit.py -- --repo .
    python3 tools/blender/validate_kit.py Art/Blender/SparkGame/provenance.json
"""
import math
from pathlib import Path
import sys

import bpy

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spark_kit  # noqa: E402

# Exact art-direction base colours (hex / 255, written verbatim as MTL Kd); shades differ only by
# roughness and metalness.
GRAPHITE = (0x2B / 255, 0x2F / 255, 0x36 / 255)
STEEL = (0x9A / 255, 0xA3 / 255, 0xAD / 255)
AMBER = (0xF2 / 255, 0xA3 / 255, 0x3A / 255)
PORCELAIN = (0xE9 / 255, 0xE6 / 255, 0xDF / 255)
PALETTE = {
    "graphite": (GRAPHITE, 0.72, 0.0),        # matte powder-coated panels
    "graphite_metal": (GRAPHITE, 0.38, 0.8),  # anodised structural members
    "steel": (STEEL, 0.32, 0.9),              # brushed-steel ribs and trims
    "amber": (AMBER, 0.42, 0.0),              # the single brand accent
    "porcelain": (PORCELAIN, 0.55, 0.0),      # display faces
}
BEVEL = 0.03        # default 45-degree chamfer on hard edges (art direction: 2-4 cm)
THIN_BEVEL = 0.02   # thin ribs and inlays
OCTAGON = (0.0, 0.0, math.pi / 8)  # turn 8-sided prisms so their flats face the axes


def flat(obj):
    """Faceted flat shading (the library smooths cylinder sides)."""
    for polygon in obj.data.polygons:
        polygon.use_smooth = False
    return obj


def chamfered_octagon(kit, name, radius, bottom, top, mat, chamfer=BEVEL):
    """Octagonal prism from bottom to top with 45-degree chamfers on both cap edges."""
    body = top - bottom - 2 * chamfer
    flat(kit.cylinder(f"{name} lower chamfer", radius - chamfer, chamfer, (0, 0, bottom + chamfer / 2), mat,
                      segments=8, radius_top=radius, rotation=OCTAGON))
    flat(kit.cylinder(f"{name} body", radius, body, (0, 0, bottom + chamfer + body / 2), mat, segments=8,
                      rotation=OCTAGON))
    flat(kit.cylinder(f"{name} upper chamfer", radius, chamfer, (0, 0, top - chamfer / 2), mat, segments=8,
                      radius_top=radius - chamfer, rotation=OCTAGON))


def display_pedestal(kit):
    """1.1 m exhibition plinth with a 0.8 m top, rib-panelled porcelain body and an amber band."""
    kit.box("Pedestal foot", (0.86, 0.86, 0.08), (0, 0, 0.04), "graphite", bevel=BEVEL)
    kit.box("Pedestal body", (0.7, 0.7, 0.92), (0, 0, 0.54), "porcelain", bevel=BEVEL)
    kit.box("Pedestal accent band", (0.72, 0.72, 0.04), (0, 0, 0.92), "amber", bevel=THIN_BEVEL * 0.5)
    kit.box("Pedestal top slab", (0.8, 0.8, 0.09), (0, 0, 1.045), "graphite_metal", bevel=BEVEL)
    kit.box("Pedestal display inlay", (0.68, 0.68, 0.01), (0, 0, 1.095), "porcelain", bevel=0.004)
    # Two thin steel ribs per side frame each porcelain face.
    for side in range(4):
        angle = side * math.pi / 2
        for offset in (-0.2, 0.2):
            x = math.cos(angle) * 0.355 - math.sin(angle) * offset
            y = math.sin(angle) * 0.355 + math.cos(angle) * offset
            kit.box(f"Pedestal rib {side}{'L' if offset < 0 else 'R'}", (0.03, 0.02, 0.72), (x, y, 0.47), "steel",
                    bevel=THIN_BEVEL * 0.4, rotation=(0, 0, angle))


def info_signpost(kit):
    """2.2 m two-sided info sign: graphite post, porcelain panel in a steel frame, amber 'i' glyph."""
    kit.box("Signpost foot", (0.6, 0.4, 0.06), (0, 0, 0.03), "graphite", bevel=THIN_BEVEL)
    kit.box("Signpost post", (0.12, 0.12, 1.36), (0, 0, 0.74), "graphite_metal", bevel=BEVEL)
    kit.box("Signpost frame lower", (1.06, 0.1, 0.04), (0, 0, 1.34), "steel", bevel=THIN_BEVEL * 0.5)
    kit.box("Signpost panel", (1.0, 0.08, 0.68), (0, 0, 1.7), "porcelain", bevel=BEVEL)
    kit.box("Signpost frame upper", (1.06, 0.1, 0.04), (0, 0, 2.06), "steel", bevel=THIN_BEVEL * 0.5)
    kit.box("Signpost header", (1.06, 0.12, 0.12), (0, 0, 2.14), "graphite", bevel=BEVEL)
    for face, y in (("front", 0.045), ("back", -0.045)):
        kit.box(f"Signpost glyph stem {face}", (0.08, 0.012, 0.26), (-0.3, y, 1.63), "amber", bevel=0.005)
        kit.box(f"Signpost glyph dot {face}", (0.08, 0.012, 0.08), (-0.3, y, 1.84), "amber", bevel=0.005)
        for row, z in enumerate((1.78, 1.64)):
            kit.box(f"Signpost text line {face}{row}", (0.46 - 0.1 * row, 0.012, 0.035), (0.1 - 0.05 * row, y, z),
                    "graphite", bevel=0.005)


def supply_crate(kit):
    """0.8 m cube crate: graphite shell, steel bands and corner posts, porcelain side panels, amber label."""
    kit.box("Crate shell", (0.76, 0.76, 0.76), (0, 0, 0.4), "graphite", bevel=BEVEL)
    kit.box("Crate band lower", (0.8, 0.8, 0.06), (0, 0, 0.03), "steel", bevel=THIN_BEVEL)
    kit.box("Crate band upper", (0.8, 0.8, 0.06), (0, 0, 0.77), "steel", bevel=THIN_BEVEL)
    for index, (x, y) in enumerate(((-0.36, -0.36), (0.36, -0.36), (0.36, 0.36), (-0.36, 0.36))):
        kit.box(f"Crate corner post {index}", (0.08, 0.08, 0.68), (x, y, 0.4), "steel", bevel=THIN_BEVEL)
    for side, x in (("left", -0.385), ("right", 0.385)):
        kit.box(f"Crate panel {side}", (0.02, 0.5, 0.5), (x, 0, 0.4), "porcelain", bevel=0.008)
    kit.box("Crate label", (0.34, 0.02, 0.16), (0, 0.385, 0.46), "amber", bevel=0.008)


def light_pylon(kit):
    """3.0 m exhibition light pylon: octagonal plinth, ribbed mast, porcelain head with an amber lamp."""
    chamfered_octagon(kit, "Pylon plinth", 0.34, 0.0, 0.14, "graphite")
    chamfered_octagon(kit, "Pylon collar", 0.15, 0.14, 0.24, "steel", chamfer=THIN_BEVEL)
    kit.box("Pylon mast", (0.16, 0.16, 2.44), (0, 0, 1.42), "graphite_metal", bevel=BEVEL)
    for index, (x, y) in enumerate(((0.0, 0.095), (0.0, -0.095), (0.095, 0.0), (-0.095, 0.0))):
        kit.box(f"Pylon rib {index}", (0.03, 0.03, 2.2), (x, y, 1.36), "steel", bevel=THIN_BEVEL * 0.4)
    chamfered_octagon(kit, "Pylon mid collar", 0.13, 1.5, 1.58, "steel", chamfer=THIN_BEVEL)
    kit.box("Pylon head", (0.56, 0.32, 0.32), (0, 0, 2.78), "porcelain", bevel=BEVEL)
    kit.box("Pylon lamp", (0.46, 0.02, 0.2), (0, 0.165, 2.77), "amber", bevel=0.008)
    kit.box("Pylon cap", (0.6, 0.36, 0.06), (0, 0, 2.97), "graphite", bevel=THIN_BEVEL)


PROPS = (
    # name, builder, triangle budget (art direction), LOD1 decimate ratio, collision kind, collision cap
    ("display_pedestal", display_pedestal, 900, 0.36, "box", 200),
    ("info_signpost", info_signpost, 700, 0.36, "hull", 64),
    ("supply_crate", supply_crate, 600, 0.36, "box", 200),
    ("light_pylon", light_pylon, 1200, 0.36, "hull", 64),
)


def render_preview(kit, path):
    """Workbench preview of the whole kit side by side on a neutral ground; not part of provenance."""
    spacing = 1.9
    for index, (name, *_rest) in enumerate(PROPS):
        for obj in bpy.data.collections[name].objects:
            # The camera looks down -Y, so decreasing Blender X runs left to right in the image.
            obj.location.x -= (index - (len(PROPS) - 1) / 2) * spacing
    ground_mesh = bpy.data.meshes.new("Preview ground")
    ground_mesh.from_pydata([(-80, -80, 0), (80, -80, 0), (80, 80, 0), (-80, 80, 0)], [], [(0, 1, 2, 3)])
    ground = bpy.data.objects.new("Preview ground", ground_mesh)
    ground_material = bpy.data.materials.new("Preview ground")
    ground_material.diffuse_color = (0.55, 0.55, 0.55, 1.0)
    ground_mesh.materials.append(ground_material)
    bpy.context.scene.collection.objects.link(ground)

    camera = bpy.data.objects.new("Preview camera", bpy.data.cameras.new("Preview camera"))
    camera.data.lens = 42
    camera.location = (0.0, 10.5, 3.2)
    camera.rotation_euler = (math.radians(81), 0.0, math.radians(180))
    bpy.context.scene.collection.objects.link(camera)

    scene = bpy.context.scene
    scene.camera = camera
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x, scene.render.resolution_y = 1600, 900
    scene.render.resolution_percentage = 100
    scene.render.film_transparent = False
    scene.render.dither_intensity = 0.0  # no noise: keeps the flat-shaded PNG small
    scene.render.image_settings.file_format = "PNG"
    scene.render.image_settings.color_mode = "RGB"
    scene.render.image_settings.compression = 100
    scene.view_settings.view_transform = "Standard"
    shading = scene.display.shading
    shading.light = "STUDIO"
    shading.studio_light = "Default"
    shading.color_type = "MATERIAL"
    shading.show_shadows = True
    shading.shadow_intensity = 0.35
    shading.background_type = "VIEWPORT"
    shading.background_color = (0.82, 0.82, 0.82)
    scene.display.light_direction = (0.45, 0.35, 0.8)
    scene.display.render_aa = "5"
    scene.render.filepath = str(path)
    path.unlink(missing_ok=True)
    bpy.ops.render.render(write_still=True)
    if not path.is_file():
        raise RuntimeError(f"Workbench preview was not written to {path}")


def main():
    args = spark_kit.parse_args()
    kit = spark_kit.Kit(args.repo, "SparkGame", __file__, PALETTE, short="Showcase")
    kit.blend_path = kit.art_dir / "showcase_kit.blend"
    for name, builder, budget, lod_ratio, collision, cap in PROPS:
        builder(kit)
        kit.export_asset(name, triangle_budget=budget, lod_ratio=lod_ratio, collision=collision, collision_cap=cap)
    kit.save_blend()
    kit.write_provenance()
    render_preview(kit, kit.art_dir / "preview.png")


main()
