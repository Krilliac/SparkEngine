"""Author the SparkGameRacing "Circuit Racing" kit with Blender 4.0.2; Spark Open License 1.0.

Art direction (owner-approved): a sunlit circuit of asphalt, kerb stripes and a single sponsor accent
for speed read. Long low extrusions, rounded safety profiles and a repeated stripe rhythm. Every MTL
diffuse is one of four exact palette colours (asphalt #2E2F31, kerb red #D63A2F, kerb white #F2F2EE,
sponsor cyan #1FB5C9); shades differ only by roughness and metalness. Faceted flat shading, 2-4 cm
chamfers on hard edges, 3-5 materials per prop, no texture maps.

Props face Blender +Y, which the kit export (forward_axis='Z') writes as OBJ +Z. The pivot is the
ground-contact centre (Blender z = 0). RacingTrackSystem dresses the active track with the kit (see
Art/Blender/SparkGameRacing/README.md).

Run (the Workbench preview needs an OpenGL context; on a display-less Linux host wrap Blender in xvfb-run):
  PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
    --python tools/blender/author_racing_kit.py -- --repo .
  python3 tools/blender/validate_kit.py Art/Blender/SparkGameRacing/provenance.json
"""
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


ASPHALT = hex_rgb("#2E2F31")
KERB_RED = hex_rgb("#D63A2F")
KERB_WHITE = hex_rgb("#F2F2EE")
SPONSOR_CYAN = hex_rgb("#1FB5C9")

PALETTE = {
    "asphalt": (ASPHALT, 0.92, 0.0),           # concrete plinths, ballast feet, light housings
    "asphalt_rubber": (ASPHALT, 0.8, 0.0),     # tyres, cone base
    "kerb_red": (KERB_RED, 0.55, 0.0),         # painted kerb stripes, start-light lenses
    "kerb_white": (KERB_WHITE, 0.5, 0.0),      # painted kerb stripes, reflective cone bands
    "kerb_white_steel": (KERB_WHITE, 0.35, 0.6),  # white-painted steel truss
    "sponsor_cyan": (SPONSOR_CYAN, 0.3, 0.0),  # the single sponsor accent
}

BEVEL = 0.03          # default chamfer on hard edges (art direction: 2-4 cm)
SMALL_BEVEL = 0.02    # slender members
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
    # clamp_overlap can collapse a narrow face (the 8 cm barrier crown) onto coincident vertices;
    # weld them and drop the zero-area triangles they would otherwise export.
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    bmesh.ops.dissolve_degenerate(bm, dist=1e-5, edges=bm.edges)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(obj.data)
    bm.free()
    return flat(obj)


def arc_band(kit, name, inner_radius, outer_radius, depth, start, end, steps, mat, location):
    """Partial annulus in the XZ plane from angle start to end (radians), depth along Y, chamfered."""
    bm = bmesh.new()
    columns = []
    for index in range(steps + 1):
        angle = start + (end - start) * index / steps
        cos_a, sin_a = math.cos(angle), math.sin(angle)
        columns.append([bm.verts.new((radius * cos_a, y, radius * sin_a))
                        for radius in (outer_radius, inner_radius) for y in (-depth / 2, depth / 2)])
    # column = [outer front, outer back, inner front, inner back]
    for a, b in zip(columns, columns[1:]):
        for i, j in ((0, 2), (3, 1), (1, 0), (2, 3)):
            bm.faces.new((a[i], b[i], b[j], a[j]))
    for column in (columns[0], columns[-1]):
        bm.faces.new((column[0], column[2], column[3], column[1]))
    return chamfer(kit._add(name, bm, mat, location, (0, 0, 0)))


def cone_radius(z):
    """Traffic-cone body radius at height z (0.15 m above the base plate, 0.035 m at the 0.7 m tip)."""
    return 0.15 - (z - 0.05) * (0.115 / 0.65)


def barrier_segment(kit):
    """4.0 m concrete safety barrier: rounded Jersey profile in alternating kerb-painted 1 m blocks."""
    kit.box("Plinth", (4.0, 0.6, 0.08), (0, 0, 0.04), "asphalt", bevel=SMALL_BEVEL)
    # Rounded safety profile (x across the barrier = Blender Y, z up), laid in local XZ and extruded
    # along local Y; turning +90 degrees about Z runs the extrusion along Blender X.
    profile = [(-0.29, 0.08), (0.29, 0.08), (0.29, 0.18), (0.14, 0.34), (0.1, 0.78), (0.08, 0.86),
               (0.04, 0.9), (-0.04, 0.9), (-0.08, 0.86), (-0.1, 0.78), (-0.14, 0.34), (-0.29, 0.18)]
    for index, x in enumerate((-1.5, -0.5, 0.5, 1.5)):
        stripe = "kerb_red" if index % 2 == 0 else "kerb_white"
        block = kit.extrude(f"Barrier block {index + 1}", profile, 1.0, (x, 0, 0), stripe,
                            rotation=(0, 0, HALF_PI))
        chamfer(block, min_angle=math.radians(25))
    # Sponsor band across the middle two blocks, leaning back with the upper front face.
    lean = math.atan2(0.04, 0.44)
    kit.box("Sponsor band", (1.8, 0.03, 0.22), (0, 0.137, 0.56), "sponsor_cyan", bevel=0.012,
            rotation=(lean, 0, 0))
    kit.export_asset("barrier_segment", triangle_budget=700, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def traffic_cone(kit):
    """0.7 m traffic cone: square rubber base, red body with two reflective white bands."""
    kit.box("Base plate", (0.42, 0.42, 0.05), (0, 0, 0.025), "asphalt_rubber", bevel=SMALL_BEVEL)
    bands = [(0.05, 0.32, "kerb_red"), (0.32, 0.44, "kerb_white"), (0.44, 0.52, "kerb_red"),
             (0.52, 0.6, "kerb_white"), (0.6, 0.7, "kerb_red")]
    for index, (bottom, top, mat) in enumerate(bands):
        band = kit.cylinder(f"Cone band {index + 1}", cone_radius(bottom), top - bottom,
                            (0, 0, (bottom + top) / 2), mat, segments=10, radius_top=cone_radius(top))
        if top >= 0.7:
            chamfer(band, width=0.01)  # the tip rim is only 3.5 cm wide
        else:
            flat(band)
    kit.export_asset("traffic_cone", triangle_budget=300, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def checkpoint_arch(kit):
    """12 x 6 m checkpoint arch: red/white striped semicircle on ballast feet with a sponsor banner."""
    bands = 10
    for index in range(bands):
        start, end = math.pi * index / bands, math.pi * (index + 1) / bands
        arc_band(kit, f"Arch stripe {index + 1}", 5.2, 6.0, 0.9, start, end, 3,
                 "kerb_red" if index % 2 == 0 else "kerb_white", (0, 0, 0))
    for side in (-1, 1):
        kit.box(f"Ballast foot {'left' if side > 0 else 'right'}", (0.8, 1.4, 0.45), (side * 5.6, 0, 0.225),
                "asphalt", bevel=BEVEL)
        kit.box(f"Banner hanger {'left' if side > 0 else 'right'}", (0.06, 0.06, 0.5), (side * 2.0, 0, 4.62),
                "asphalt", bevel=SMALL_BEVEL)
    kit.box("Sponsor banner", (5.0, 0.12, 1.0), (0, 0, 3.9), "sponsor_cyan", bevel=BEVEL)
    kit.export_asset("checkpoint_arch", triangle_budget=1800, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def start_gantry(kit):
    """14 x 7 m start/finish gantry: striped towers, white truss beam, sponsor board, five start lights."""
    for side in (-1, 1):
        label = "left" if side > 0 else "right"
        x = side * 6.5
        kit.box(f"Footing {label}", (1.0, 1.2, 0.4), (x, 0, 0.2), "asphalt", bevel=BEVEL)
        for index in range(8):  # 0.65 m kerb stripes from the footing to the beam
            bottom = 0.4 + 0.65 * index
            kit.box(f"Tower {label} stripe {index + 1}", (0.8, 0.8, 0.65), (x, 0, bottom + 0.325),
                    "kerb_red" if index % 2 == 0 else "kerb_white", bevel=BEVEL)
    # Truss beam: two chords, verticals every 2 m and alternating diagonals between them.
    kit.box("Bottom chord", (14.0, 0.7, 0.2), (0, 0, 5.7), "kerb_white_steel", bevel=BEVEL)
    kit.box("Top chord", (14.0, 0.7, 0.2), (0, 0, 6.9), "kerb_white_steel", bevel=BEVEL)
    for index in range(7):
        x = -6.0 + 2.0 * index
        kit.box(f"Truss vertical {index + 1}", (0.12, 0.12, 1.0), (x, 0, 6.3), "kerb_white_steel",
                bevel=SMALL_BEVEL)
        if index < 6:
            tilt = math.atan2(2.0, 1.0) * (1 if index % 2 == 0 else -1)
            kit.box(f"Truss diagonal {index + 1}", (0.1, 0.1, math.hypot(2.0, 1.0)), (x + 1.0, 0, 6.3),
                    "kerb_white_steel", bevel=SMALL_BEVEL, rotation=(0, tilt, 0))
    kit.box("Sponsor board", (7.0, 0.08, 0.9), (0, 0.4, 6.3), "sponsor_cyan", bevel=SMALL_BEVEL)
    # Start lights hang under the beam centre, lenses facing the grid (+Y).
    for index in range(5):
        x = -2.0 + 1.0 * index
        kit.box(f"Light housing {index + 1}", (0.5, 0.35, 0.9), (x, 0, 5.15), "asphalt", bevel=BEVEL)
        for row, z in enumerate((5.36, 4.94)):
            flat(kit.cylinder(f"Light lens {index + 1}{'ab'[row]}", 0.14, 0.06, (x, 0.2, z), "kerb_red",
                              segments=10, rotation=(-HALF_PI, 0, 0)))
    kit.export_asset("start_gantry", triangle_budget=2600, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def tyre_stack(kit):
    """1.2 m stack of four tyres (rubber / white / rubber / red) with a sponsor strap on the front."""
    for index, mat in enumerate(("asphalt_rubber", "kerb_white", "asphalt_rubber", "kerb_red")):
        tyre = kit.ring(f"Tyre {index + 1}", 0.32, 0.16, 0.3, (0, 0, 0.15 + 0.3 * index), mat, segments=14)
        chamfer(tyre)
    kit.box("Sponsor strap", (0.2, 0.03, 1.1), (0, 0.33, 0.6), "sponsor_cyan", bevel=0.012)
    kit.export_asset("tyre_stack", triangle_budget=1200, lod_ratio=LOD_RATIO, collision="hull",
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
    offsets = {"start_gantry": 9.0, "checkpoint_arch": -5.5, "barrier_segment": -14.5, "tyre_stack": -17.6,
               "traffic_cone": -19.0}
    for name, offset in offsets.items():
        for obj in bpy.data.collections[name].objects:
            obj.location.x += offset
    # Show the MTL colours as authored sRGB in the viewport shading.
    for mat in bpy.data.materials:
        mat.diffuse_color = (*(srgb_to_linear(c) for c in mat.diffuse_color[:3]), 1.0)

    ground_mesh = bpy.data.meshes.new("Preview ground")
    ground_mesh.from_pydata([(-400, -400, 0), (400, -400, 0), (400, 400, 0), (-400, 400, 0)], [], [(0, 1, 2, 3)])
    ground = bpy.data.objects.new("Preview ground", ground_mesh)
    ground_mat = bpy.data.materials.new("Preview_ground")
    ground_mat.diffuse_color = (0.42, 0.42, 0.42, 1.0)
    ground_mesh.materials.append(ground_mat)
    bpy.context.scene.collection.objects.link(ground)

    camera_data = bpy.data.cameras.new("Preview camera")
    camera_data.lens = 38
    camera = bpy.data.objects.new("Preview camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = (-1.5, 42.0, 9.0)
    camera.rotation_euler = (math.radians(82.5), 0.0, math.radians(180))

    scene = bpy.context.scene
    scene.camera = camera
    if scene.world is not None:
        scene.world.color = (0.62, 0.64, 0.66)
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
    kit = spark_kit.Kit(args.repo, "SparkGameRacing", __file__, PALETTE, short="Racing")
    kit.blend_path = kit.art_dir / "racing_kit.blend"
    for build in (barrier_segment, traffic_cone, checkpoint_arch, start_gantry, tyre_stack):
        build(kit)
    check_lod_limits(kit)
    kit.save_blend()
    kit.write_provenance()
    render_preview(kit.art_dir / "preview.png")


main()
