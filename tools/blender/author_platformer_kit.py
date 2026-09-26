"""Author the SparkGamePlatformer "Level 0" kit with Blender 4.0.2; Spark Open License 1.0.

Art direction (owner-approved): a bright toybox. Saturated primaries, soft rounded forms and
hazards nobody can miss; pill and rounded-box shapes, chunky outlines from bevels, exaggerated
proportions. Every MTL diffuse is one of the four palette colours (Sky #6CC3F0, Grass #5BBF4A,
Coin gold #F5C542, Brick #C4633A); shades differ only by roughness and metalness. Faceted flat
shading, 2-4 cm chamfers on hard edges (larger radii where a form is meant to read as rounded),
3-5 materials per prop, no texture maps.

Props face Blender +Y, which the kit export (forward_axis='Z') writes as OBJ +Z; the pivot is the
ground-contact centre (Blender z = 0). The platformer's levels run along world X with the follow
camera on the -Z side, so front-facing props (goal flag, coin) are placed turned 180 degrees about
Y. Sizes match the level data in metres: floating_platform is a 3 x 3 m pad (PlatformerLevelSystem's
3 m stepping stones; PlatformerLevelFlow stretches it over wider platforms), spike_hazard is a 1 x 1 m
tile PlatformerHazardSystem lays across spike pits, spring_pad is scaled up to be each Bouncy platform,
goal_flag stands on the goal platform, and coin is for the Coin collectibles (not placed yet; see
Art/Blender/SparkGamePlatformer/README.md).

Run (the Workbench preview needs an OpenGL context; on a display-less Linux host wrap Blender in xvfb-run):
  PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
    --python tools/blender/author_platformer_kit.py -- --repo .
  python3 tools/blender/validate_kit.py Art/Blender/SparkGamePlatformer/provenance.json
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


SKY = hex_rgb("#6CC3F0")
GRASS = hex_rgb("#5BBF4A")
GOLD = hex_rgb("#F5C542")
BRICK = hex_rgb("#C4633A")

PALETTE = {
    "grass": (GRASS, 0.82, 0.0),         # turf caps, the goal pennant
    "brick": (BRICK, 0.86, 0.0),         # platform bodies, bases
    "brick_glazed": (BRICK, 0.38, 0.0),  # glazed toy-plastic trims and pads
    "gold": (GOLD, 0.28, 1.0),           # polished coin rims, studs, finials, spike tips
    "gold_satin": (GOLD, 0.55, 0.35),    # coin faces
    "gold_mirror": (GOLD, 0.12, 1.0),    # the coin's raised star, the brightest thing in the kit
    "sky_metal": (SKY, 0.3, 0.85),       # painted steel: spikes, coil, pole
    "sky": (SKY, 0.5, 0.0),              # toy-plastic accents
}

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


def lathe(kit, name, profile, segments, location, mat, rotation=(0, 0, 0)):
    """Revolve an open (radius, z) profile about local Z; profile points at radius 0 close into fans."""
    bm = bmesh.new()
    rings = []
    for radius, z in profile:
        if radius == 0.0:
            rings.append([bm.verts.new((0.0, 0.0, z))] * segments)
        else:
            rings.append([bm.verts.new((radius * math.cos(2 * math.pi * i / segments),
                                        radius * math.sin(2 * math.pi * i / segments), z))
                          for i in range(segments)])
    for lower, upper in zip(rings, rings[1:]):
        for i in range(segments):
            j = (i + 1) % segments
            corners = [lower[i], lower[j], upper[j], upper[i]]
            unique = list(dict.fromkeys(corners))
            if len(unique) >= 3:
                bm.faces.new(unique)
    return flat(kit._add(name, bm, mat, location, rotation))


def star_profile(outer, inner, points=5):
    """(x, z) outline of a star with its first point straight up."""
    outline = []
    for i in range(points * 2):
        radius = outer if i % 2 == 0 else inner
        angle = HALF_PI + math.pi * i / points
        outline.append((radius * math.cos(angle), radius * math.sin(angle)))
    return outline[::-1]  # clockwise in XZ so extrude() caps face outward


def floating_platform(kit):
    """3 x 3 x 1 m sky island: a rounded turf cap over a brick block, grass drips, gold studs and a keel."""
    # Turf cap: a soft rounded box 0.38 m thick whose top is the walkable surface (z = 1.0).
    flat(kit.box("Turf cap", (3.0, 3.0, 0.38), (0, 0, 0.81), "grass", bevel=0.11, bevel_segments=3))
    # Grass drips spill over the middle of each edge, exaggerated and chunky.
    for index, (x, y, sx, sy) in enumerate(((0.0, 1.47, 0.9, 0.2), (1.47, 0.35, 0.2, 0.8),
                                            (-0.3, -1.47, 0.8, 0.2), (-1.47, -0.4, 0.2, 0.7))):
        flat(kit.box(f"Grass drip {index + 1}", (sx, sy, 0.34), (x, y, 0.56), "grass", bevel=0.07))
    # Brick block with a glazed course, tucked 15 cm inside the cap.
    flat(kit.box("Brick block", (2.7, 2.7, 0.5), (0, 0, 0.42), "brick", bevel=BEVEL))
    flat(kit.box("Glazed course", (2.78, 2.78, 0.09), (0, 0, 0.36), "brick_glazed", bevel=0.02))
    # Keel: an inverted, chamfered frustum so the island reads as floating from below.
    chamfer(kit.cylinder("Keel", 0.55, 0.2, (0, 0, 0.1), "brick_glazed", segments=8, radius_top=1.15,
                         rotation=(0, 0, math.pi / 8)))
    # One gold stud per side of the brick block (fronts toward +Y first).
    for index, angle in enumerate((HALF_PI, 0.0, -HALF_PI, math.pi)):
        x, y = 1.37 * math.cos(angle), 1.37 * math.sin(angle)
        flat(kit.box(f"Gold stud {index + 1}", (0.22, 0.22, 0.22), (x, y, 0.45), "gold", bevel=0.04,
                     rotation=(0, 0, angle)))
    kit.export_asset("floating_platform", triangle_budget=900, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def coin(kit):
    """0.5 m coin standing on its rim: chamfered gold rim, satin faces and a mirror-gold star raised on both sides."""
    radius, face, rim = 0.25, 0.028, 0.045
    # Coin axis along Blender Y (the prop's front), centred 0.25 m up so the rim touches the ground.
    lathe(kit, "Coin face front", [(0.0, face), (0.19, face), (0.205, rim)], 16, (0, 0, radius), "gold_satin",
          rotation=(-HALF_PI, 0, 0))
    lathe(kit, "Coin rim", [(0.205, rim), (0.232, rim), (radius, rim - 0.02), (radius, -rim + 0.02),
                            (0.232, -rim), (0.205, -rim)], 16, (0, 0, radius), "gold", rotation=(-HALF_PI, 0, 0))
    lathe(kit, "Coin face back", [(0.205, -rim), (0.19, -face), (0.0, -face)], 16, (0, 0, radius), "gold_satin",
          rotation=(-HALF_PI, 0, 0))
    # Raised star through the coin: it stands 1.2 cm proud of both faces.
    kit.extrude("Coin star", star_profile(0.13, 0.055), 2 * face + 0.024, (0, 0, radius), "gold_mirror")
    kit.export_asset("coin", triangle_budget=300, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def spring_pad(kit):
    """1.0 m spring pad: rounded brick base, a chunky sky-steel coil and a glazed launch cap with a gold rim."""
    chamfer(kit.cylinder("Base", 0.5, 0.14, (0, 0, 0.07), "brick", segments=12), width=0.04)
    for index in range(4):
        flat(kit.box(f"Base bolt {index + 1}", (0.1, 0.1, 0.06),
                     (0.4 * math.cos(math.pi / 4 + index * HALF_PI), 0.4 * math.sin(math.pi / 4 + index * HALF_PI),
                      0.16), "gold", bevel=0.02, rotation=(0, 0, math.pi / 4 + index * HALF_PI)))
    # Coil: three fat hoops, alternately tilted so they read as a spring rather than stacked washers.
    for index, z in enumerate((0.21, 0.31, 0.41)):
        tilt = 0.09 if index % 2 == 0 else -0.09
        flat(kit.ring(f"Coil {index + 1}", 0.29, 0.21, 0.07, (0, 0, z), "sky_metal", segments=10,
                      rotation=(tilt, 0, 0)))
    flat(kit.cylinder("Coil core", 0.12, 0.3, (0, 0, 0.3), "sky_metal", segments=6))
    # Launch cap: a squat pill-like puck whose top is 0.56 m.
    chamfer(kit.cylinder("Launch cap", 0.44, 0.1, (0, 0, 0.5), "brick_glazed", segments=14), width=0.035)
    flat(kit.ring("Cap rim", 0.46, 0.4, 0.04, (0, 0, 0.47), "gold", segments=14))
    flat(kit.cylinder("Cap button", 0.16, 0.03, (0, 0, 0.565), "sky", segments=6))
    kit.export_asset("spring_pad", triangle_budget=800, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def spike_hazard(kit):
    """1 x 1 m spike tile: a brick plate with a glazed rim and nine sky-steel spikes with gold tips."""
    flat(kit.box("Plate", (1.0, 1.0, 0.1), (0, 0, 0.05), "brick", bevel=BEVEL))
    flat(kit.box("Glazed rim", (0.9, 0.9, 0.04), (0, 0, 0.12), "brick_glazed", bevel=0.02))
    for row in range(3):
        for column in range(3):
            x, y = (column - 1) * 0.3, (row - 1) * 0.3
            height = 0.5 if (row, column) == (1, 1) else 0.42
            flat(kit.cylinder(f"Spike {row * 3 + column + 1}", 0.12, height * 0.8, (x, y, 0.14 + height * 0.4),
                              "sky_metal", segments=6, radius_top=0.04))
            flat(kit.cylinder(f"Spike tip {row * 3 + column + 1}", 0.04, height * 0.2,
                              (x, y, 0.14 + height * 0.9), "gold", segments=6, radius_top=0.004))
    kit.export_asset("spike_hazard", triangle_budget=500, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def goal_flag(kit):
    """3.5 m goal flag: stepped brick plinth, sky-steel pole, gold ball finial and a grass pennant with a gold star."""
    flat(kit.box("Plinth", (0.9, 0.9, 0.2), (0, 0, 0.1), "brick", bevel=0.04))
    flat(kit.box("Plinth step", (0.6, 0.6, 0.14), (0, 0, 0.27), "brick_glazed", bevel=BEVEL))
    pole_bottom, pole_top = 0.34, 3.26
    flat(kit.cylinder("Pole", 0.06, pole_top - pole_bottom, (0, 0, (pole_bottom + pole_top) / 2), "sky_metal",
                      segments=8))
    flat(kit.cylinder("Pole collar", 0.1, 0.12, (0, 0, pole_bottom + 0.06), "gold", segments=8))
    # Ball finial: a faceted sphere whose top is exactly 3.5 m.
    # Lathed rather than bmesh.ops.create_uvsphere, whose pole faces export in a run-dependent order.
    sphere = [(0.12 * math.cos(math.pi * (step / 6 - 0.5)), 0.12 * math.sin(math.pi * (step / 6 - 0.5)))
              for step in range(7)]
    sphere[0], sphere[-1] = (0.0, -0.12), (0.0, 0.12)
    lathe(kit, "Finial", sphere, 10, (0, 0, 3.38), "gold")
    # Pennant flies toward +X; its faces point along +Y (the prop's front). The tail is notched.
    pennant = [(0.06, 0.0), (1.35, -0.1), (1.05, -0.45), (1.35, -0.8), (0.06, -0.9)]
    kit.extrude("Pennant", pennant, 0.05, (0, 0, 3.2), "grass", bevel=0.02)
    kit.extrude("Pennant star", star_profile(0.2, 0.085), 0.07, (0.58, 0, 2.75), "gold")
    kit.export_asset("goal_flag", triangle_budget=600, lod_ratio=LOD_RATIO, collision="box",
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

    The camera looks down -Y at the props' fronts, so +X is screen left: the goal flag stands at the right
    with its pennant flying back over the floating platform.
    """
    layout = {"goal_flag": (-4.9, -0.3), "floating_platform": (-1.3, 0.0), "spring_pad": (1.5, 0.4),
              "spike_hazard": (3.2, 0.4), "coin": (4.6, 0.6)}
    for name, (dx, dy) in layout.items():
        for obj in bpy.data.collections[name].objects:
            obj.location.x += dx
            obj.location.y += dy
    for mat in bpy.data.materials:
        mat.diffuse_color = (*(srgb_to_linear(c) for c in mat.diffuse_color[:3]), 1.0)

    ground_mesh = bpy.data.meshes.new("Preview ground")
    ground_mesh.from_pydata([(-40, -30, 0), (40, -30, 0), (40, 30, 0), (-40, 30, 0)], [], [(0, 1, 2, 3)])
    ground = bpy.data.objects.new("Preview ground", ground_mesh)
    ground_mat = bpy.data.materials.new("Preview_ground")
    ground_mat.diffuse_color = (0.34, 0.34, 0.34, 1.0)
    ground_mesh.materials.append(ground_mat)
    bpy.context.scene.collection.objects.link(ground)

    camera_data = bpy.data.cameras.new("Preview camera")
    camera_data.lens = 40
    camera = bpy.data.objects.new("Preview camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = (-0.2, 13.5, 5.2)
    camera.rotation_euler = (math.radians(75), 0.0, math.radians(180))

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
    kit = spark_kit.Kit(args.repo, "SparkGamePlatformer", __file__, PALETTE, short="Platformer")
    kit.blend_path = kit.art_dir / "platformer_kit.blend"
    floating_platform(kit)
    coin(kit)
    spring_pad(kit)
    spike_hazard(kit)
    goal_flag(kit)
    check_lod_limits(kit)
    kit.save_blend()
    kit.write_provenance()
    render_preview(kit.art_dir / "preview.png")


main()
