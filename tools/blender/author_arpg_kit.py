"""Author the SparkGameARPG "Action RPG Dungeon" kit with Blender 4.0.2; Spark Open License 1.0.

Art direction (owner-approved): obsidian crypts lit by arcane violet and embers, where loot pops against
dark ground. Jagged spires, cracked slabs, rune-cut bands and glowing inlays. Every MTL diffuse is one of
four exact palette colours (obsidian #1E1B24, bone #D8CFB8, arcane violet #7B4FD1, ember #D9502B); shades
differ only by roughness and metalness. Faceted flat shading, 2-4 cm chamfers on hard edges (flush inlays
under 2 cm thick stay square), 3-5 materials per prop, no texture maps.

Props face Blender +Y, which the kit export (forward_axis='Z') writes as OBJ +Z. The pivot is the
ground-contact centre (Blender z = 0). ARPGDungeonSystem places the kit in the crypt entry room (see
Art/Blender/SparkGameARPG/README.md).

Run (the Workbench preview needs an OpenGL context; on a display-less Linux host wrap Blender in xvfb-run):
  PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
    --python tools/blender/author_arpg_kit.py -- --repo .
  python3 tools/blender/validate_kit.py Art/Blender/SparkGameARPG/provenance.json
"""
import math
from pathlib import Path
import random
import sys

import bmesh
import bpy
from mathutils import Euler, Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
import spark_kit  # noqa: E402


def hex_rgb(value):
    """#RRGGBB as the 0-1 triple written verbatim to the MTL Kd line."""
    return tuple(round(int(value[i:i + 2], 16) / 255.0, 6) for i in (1, 3, 5))


OBSIDIAN = hex_rgb("#1E1B24")
BONE = hex_rgb("#D8CFB8")
ARCANE = hex_rgb("#7B4FD1")
EMBER = hex_rgb("#D9502B")

PALETTE = {
    "obsidian": (OBSIDIAN, 0.32, 0.05),       # polished volcanic glass: pillars, spires, urn foot
    "obsidian_slab": (OBSIDIAN, 0.9, 0.0),    # cracked, matte floor slabs
    "obsidian_iron": (OBSIDIAN, 0.42, 0.85),  # blackened iron: frames, rune-cut bands, blades
    "bone": (BONE, 0.74, 0.0),                # bleached bone: urn body, spike tips
    "bone_gilt": (BONE, 0.26, 0.92),          # pale gilt: coins, goblet, crossguard
    "arcane": (ARCANE, 0.16, 0.0),            # glowing arcane inlays, crystals, portal membrane
    "ember": (EMBER, 0.28, 0.0),              # glowing ember cracks, gems, keystone
}

BEVEL = 0.03          # default chamfer on hard edges (art direction: 2-4 cm)
SMALL_BEVEL = 0.02    # slender members and small caps
LOD_RATIO = 0.36      # LOD1 must stay at or under 40% of its source
LOD_LIMIT = 0.40
COLLISION_CAP = 200
HALF_PI = math.pi / 2


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


def along(base, rotation, distance):
    """Point distance up the local Z axis of a part rotated by rotation and rooted at base.

    Multi-part props (spike sleeve + tip, sword blade + hilt) rotate about one shared pivot this way.
    """
    return tuple(Vector(base) + Euler(rotation).to_matrix() @ Vector((0.0, 0.0, distance)))


def lathe(kit, name, profile, location, mat, segments=10, jitter=None):
    """Faceted solid of revolution about Z from (radius, z) points, capped at both ends.

    jitter, when given, is a random.Random that nudges each ring vertex (lumpy heaps).
    """
    bm = bmesh.new()
    loops = []
    for radius, z in profile:
        loop = []
        for index in range(segments):
            angle = 2 * math.pi * index / segments
            r, dz = radius, 0.0
            if jitter is not None and z > 0.0:  # the base loop stays flat on the ground
                r += jitter.uniform(-0.025, 0.025)
                dz = jitter.uniform(-0.012, 0.012)
            loop.append(bm.verts.new((r * math.cos(angle), r * math.sin(angle), z + dz)))
        loops.append(loop)
    for lower, upper in zip(loops, loops[1:]):
        for index in range(segments):
            following = (index + 1) % segments
            bm.faces.new((lower[index], lower[following], upper[following], upper[index]))
    bm.faces.new(loops[0][::-1])
    bm.faces.new(loops[-1])
    return flat(kit._add(name, bm, mat, location, (0, 0, 0)))


def slab(kit, name, outline, z_bottom, thickness, mat, bevel=BEVEL):
    """Flat slab from an (x, y) floor outline, thickness up from z_bottom, with chamfered edges.

    extrude() lays its profile in local XZ and extrudes along local Y; turning +90 degrees about X
    maps local (x, y, z) onto Blender (x, -z, y), so profile (x, -y) is the outline and the
    extrusion runs up Blender Z.
    """
    profile = [(x, -y) for x, y in outline]
    obj = kit.extrude(name, profile, thickness, (0, 0, z_bottom + thickness / 2), mat, bevel=bevel,
                      rotation=(HALF_PI, 0, 0))
    # An outline edge shorter than two chamfers (the 4 cm crack ends) bevels down to zero length;
    # dissolve those collapsed edges so the export carries no zero-area triangles.
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.dissolve_degenerate(bm, dist=1e-5, edges=bm.edges)
    bm.to_mesh(obj.data)
    bm.free()
    return obj


def spire(kit, name, radius, height, location, mat, segments=4, tilt=(0.0, 0.0), yaw=0.0, tip=0.006):
    """Jagged faceted spire (narrow frustum) rooted at location, optionally leaning about its root."""
    rotation = (tilt[0], tilt[1], yaw)
    return flat(kit.cylinder(name, radius, height, along(location, rotation, height / 2), mat, segments=segments,
                             radius_top=tip, rotation=rotation))


def inlay(kit, name, size, location, mat="arcane", yaw=0.0):
    """Flush glowing inlay strip; thinner than the 2 cm chamfer, so its edges stay square."""
    return kit.box(name, size, location, mat, rotation=(0, 0, yaw))


def spike_trap(kit):
    """2 x 2 m spike trap: iron rune-cut frame, cracked obsidian slabs over an ember bed, bone-tipped spikes."""
    rng = random.Random(5201)
    kit.box("Ember bed", (1.76, 1.76, 0.06), (0, 0, 0.04), "ember")
    for side, (x, y, size) in {"front": (0, 0.92, (2.0, 0.16, 0.14)), "back": (0, -0.92, (2.0, 0.16, 0.14)),
                               "right": (0.92, 0, (0.16, 1.68, 0.14)),
                               "left": (-0.92, 0, (0.16, 1.68, 0.14))}.items():
        kit.box(f"Frame {side}", size, (x, y, 0.07), "obsidian_iron", bevel=BEVEL)
        along_x = size[0] > size[1]
        for index, offset in enumerate((-0.42, 0.42)):
            position = (offset, y, 0.1445) if along_x else (x, offset, 0.1445)
            inlay(kit, f"Frame rune {side} {index + 1}", (0.3, 0.05, 0.01), position,
                  yaw=0.0 if along_x else HALF_PI)

    # Four floor slabs split by a cross-shaped gap; the front-right and back-left quadrants are cracked
    # into two pieces along a jagged line, so the ember bed glows through.
    for qx, qy in ((1, 1), (-1, 1), (-1, -1), (1, -1)):
        x0, x1 = sorted((qx * 0.03, qx * 0.82))
        y0, y1 = sorted((qy * 0.03, qy * 0.82))
        z = 0.05 + rng.uniform(0.0, 0.008)
        label = f"{'front' if qy > 0 else 'back'} {'right' if qx > 0 else 'left'}"
        if qx == qy:
            # The crack runs from the slab's top edge to its right edge; piece b sits one gap across it.
            crack = [(x0 + 0.04, y1), (x0 + 0.34, y0 + 0.48), (x0 + 0.44, y0 + 0.36), (x1, y0 + 0.04)]
            gap = 0.035
            piece_a = [(x0, y0), (x1, y0), crack[3], crack[2], crack[1], crack[0], (x0, y1)]
            piece_b = [(crack[0][0] + gap, y1), (crack[1][0] + gap, crack[1][1] + gap),
                       (crack[2][0] + gap, crack[2][1] + gap), (x1, crack[3][1] + gap), (x1, y1)]
            slab(kit, f"Slab {label} a", piece_a, z, 0.08, "obsidian_slab")
            slab(kit, f"Slab {label} b", piece_b, z - 0.006, 0.08, "obsidian_slab")
        else:
            slab(kit, f"Slab {label}", [(x0, y0), (x1, y0), (x1, y1), (x0, y1)], z, 0.08, "obsidian_slab")

    # Spikes rise through the gaps: an iron sleeve and a bone tip, each with a small seeded lean.
    spots = [(0, 0)] + [(0, s) for s in (-0.62, -0.22, 0.22, 0.62)] + [(s, 0) for s in (-0.62, -0.22, 0.22, 0.62)]
    spots += [(0.5, 0.52), (-0.5, -0.52), (0.55, -0.45), (-0.45, 0.55)]
    for index, (x, y) in enumerate(spots):
        tilt = (rng.uniform(-0.12, 0.12), rng.uniform(-0.12, 0.12))
        height = rng.uniform(0.24, 0.34) + (0.08 if index == 0 else 0.0)
        root = (x, y, 0.02)
        flat(kit.cylinder(f"Spike sleeve {index + 1:02d}", 0.055, 0.16, along(root, (*tilt, 0), 0.08), "obsidian_iron",
                          segments=6, radius_top=0.045, rotation=(*tilt, 0)))
        spire(kit, f"Spike tip {index + 1:02d}", 0.045, height, along(root, (*tilt, 0), 0.15), "bone", segments=6,
              tilt=tilt)

    for index, (sx, sy) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        spire(kit, f"Corner spire {index + 1}", 0.075, 0.22, (sx * 0.92, sy * 0.92, 0.14), "obsidian_iron",
              yaw=math.pi / 4, tilt=(-sy * 0.12, sx * 0.12))
    kit.export_asset("spike_trap", triangle_budget=1400, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def destructible_urn(kit):
    """0.9 m bone funerary urn: obsidian foot, rune-cut band with arcane glyphs, iron handles, ember mouth."""
    body = [(0.15, 0.06), (0.25, 0.17), (0.3, 0.33), (0.3, 0.46), (0.24, 0.61), (0.14, 0.71), (0.12, 0.79),
            (0.17, 0.85), (0.17, 0.9), (0.12, 0.9), (0.12, 0.84)]
    chamfer(lathe(kit, "Urn body", body, (0, 0, 0), "bone", segments=10), width=SMALL_BEVEL)
    chamfer(kit.cylinder("Urn foot", 0.17, 0.06, (0, 0, 0.03), "obsidian", segments=10), width=SMALL_BEVEL)
    flat(kit.cylinder("Ember embers", 0.115, 0.02, (0, 0, 0.845), "ember", segments=10))
    flat(kit.ring("Rune band", 0.318, 0.27, 0.09, (0, 0, 0.4), "obsidian", segments=10))
    # Glyphs sit on facet centres of the 10-sided band (every other facet), so they rest on the apothem.
    radius = 0.318 * math.cos(math.pi / 10) + 0.004
    for index in range(5):
        angle = HALF_PI + 2 * math.pi * index / 5  # the first glyph faces the front (+Y)
        inlay(kit, f"Glyph {index + 1}", (0.07, 0.01, 0.05),
              (radius * math.cos(angle), radius * math.sin(angle), 0.4), yaw=angle + HALF_PI)
    for side, sx in (("left", -1), ("right", 1)):
        flat(kit.ring(f"Handle {side}", 0.085, 0.055, 0.03, (sx * 0.235, 0, 0.64), "obsidian_iron", segments=8,
                      rotation=(HALF_PI, 0, 0)))
    kit.export_asset("destructible_urn", triangle_budget=900, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


MOUND = [(0.5, 0.0), (0.44, 0.08), (0.33, 0.17), (0.2, 0.25), (0.07, 0.3)]


def mound_height(x, y):
    """Height of the (unjittered) coin heap at floor point (x, y), so loose loot rests on its surface."""
    radius = math.hypot(x, y)
    if radius >= MOUND[0][0]:
        return 0.0
    for (r0, z0), (r1, z1) in zip(MOUND, MOUND[1:]):
        if r1 <= radius <= r0:
            return z0 + (z1 - z0) * (r0 - radius) / (r0 - r1)
    return MOUND[-1][1]


def loot_pile(kit):
    """1.2 m heap of gilt coins with ember gems, arcane crystals, a goblet and a sword driven into it."""
    rng = random.Random(5203)
    lathe(kit, "Coin heap", MOUND, (0, 0, 0), "bone_gilt", segments=9, jitter=rng)
    # Loose coins spill out to the 1.2 m footprint.
    for index in range(12):
        angle = 2 * math.pi * index / 12 + rng.uniform(-0.2, 0.2)
        radius = 0.555 if index % 3 == 0 else rng.uniform(0.47, 0.53)
        x, y = radius * math.cos(angle), radius * math.sin(angle)
        flat(kit.cylinder(f"Coin {index + 1:02d}", 0.045, 0.012, (x, y, mound_height(x, y) + 0.02), "bone_gilt",
                          segments=7, rotation=(rng.uniform(-0.3, 0.3), rng.uniform(-0.3, 0.3), 0)))
    for index, (x, y, count) in enumerate(((0.36, 0.28, 4), (-0.3, 0.36, 3), (0.1, -0.42, 5))):
        height = 0.016 * count
        chamfer(kit.cylinder(f"Coin stack {index + 1}", 0.05, height, (x, y, mound_height(x, y) - 0.01 + height / 2),
                             "bone_gilt", segments=7), width=0.004)
    # Ember gems and arcane crystals catch the eye against the dark crypt floor.
    for index, (x, y) in enumerate(((0.2, 0.18), (-0.18, 0.22), (0.3, -0.1), (-0.34, -0.18), (0.02, 0.36))):
        flat(kit.cylinder(f"Ember gem {index + 1}", 0.05, 0.07, (x, y, mound_height(x, y) + 0.01), "ember", segments=5,
                          radius_top=0.02,
                          rotation=(rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5), rng.uniform(0, 1))))
    for index, (x, y, height) in enumerate(((-0.22, -0.02, 0.36), (-0.3, 0.06, 0.26), (-0.14, -0.12, 0.22),
                                            (0.24, -0.26, 0.2))):
        spire(kit, f"Arcane crystal {index + 1}", 0.05, height, (x, y, mound_height(x, y) - 0.03), "arcane",
              segments=5, tilt=(rng.uniform(-0.35, 0.35), rng.uniform(-0.35, 0.35)))

    # Gilt goblet toppled on the floor at the heap's front.
    goblet_tilt = (0.0, 1.25, 0.4)
    flat(kit.cylinder("Goblet cup", 0.07, 0.12, (0.3, 0.42, 0.087), "bone_gilt", segments=8, radius_top=0.05,
                      rotation=goblet_tilt))
    flat(kit.cylinder("Goblet stem", 0.018, 0.1, (0.19, 0.46, 0.05), "bone_gilt", segments=6, rotation=goblet_tilt))

    # Blackened sword driven into the heap: jagged blade, gilt crossguard, obsidian grip, ember pommel.
    # Every part is placed along the blade axis, which leans about the buried tip.
    lean = (0.14, -0.1, 0.35)
    tip = (0.04, 0.02, 0.02)
    blade = [(-0.04, 0.6), (0.04, 0.6), (0.035, 0.26), (0.045, 0.2), (0.03, 0.14), (0.0, 0.0), (-0.03, 0.14),
             (-0.045, 0.2), (-0.035, 0.26)]
    kit.extrude("Sword blade", blade, 0.022, tip, "obsidian_iron", bevel=0.006, rotation=lean)
    kit.box("Sword crossguard", (0.24, 0.05, 0.045), along(tip, lean, 0.62), "bone_gilt", bevel=SMALL_BEVEL,
            rotation=lean)
    flat(kit.cylinder("Sword grip", 0.02, 0.16, along(tip, lean, 0.72), "obsidian", segments=6, rotation=lean))
    flat(kit.cylinder("Sword pommel", 0.035, 0.05, along(tip, lean, 0.82), "ember", segments=6, rotation=lean))
    kit.export_asset("loot_pile", triangle_budget=1600, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def portal_gate(kit):
    """4.0 m portal gate: jagged obsidian pillars with rune-cut bands, an arcane-inlaid arch and membrane."""
    rng = random.Random(5204)
    springing, inner_radius, outer_radius = 2.0, 1.15, 1.7

    kit.box("Dais", (3.8, 1.6, 0.16), (0, 0, 0.08), "obsidian_slab", bevel=BEVEL)
    kit.box("Front step", (2.4, 0.42, 0.08), (0, 1.0, 0.04), "obsidian_slab", bevel=BEVEL)
    for index, (x, y, length, yaw) in enumerate(((-0.55, 0.45, 0.7, 0.35), (0.35, 0.55, 0.55, -0.5),
                                                 (0.95, -0.4, 0.45, 0.9))):
        inlay(kit, f"Dais crack {index + 1}", (length, 0.05, 0.01), (x, y, 0.1645), mat="ember", yaw=yaw)

    for side, sx in (("left", -1), ("right", 1)):
        z = 0.16
        for index, (width, depth, height) in enumerate(((0.8, 0.9, 0.7), (0.74, 0.84, 0.65), (0.68, 0.8, 0.49))):
            kit.box(f"Pillar {side} {index + 1}", (width, depth, height - 0.01),
                    (sx * 1.575 + rng.uniform(-0.015, 0.015), 0, z + height / 2), "obsidian", bevel=BEVEL,
                    rotation=(0, 0, rng.uniform(-0.05, 0.05)))
            z += height
        for level, band_z in (("low", 0.86), ("capital", 1.95)):
            kit.box(f"Rune band {side} {level}", (0.86, 0.96, 0.14), (sx * 1.575, 0, band_z), "obsidian_iron",
                    bevel=BEVEL)
            inlay(kit, f"Band rune {side} {level}", (0.46, 0.01, 0.05), (sx * 1.575, 0.4845, band_z))
        # Jagged spires crown each pillar's outer shoulder.
        spire(kit, f"Spire {side} tall", 0.16, 1.2, (sx * 1.78, 0.1, 2.0), "obsidian", yaw=0.3,
              tilt=(0.05, sx * 0.14))
        spire(kit, f"Spire {side} short", 0.12, 0.75, (sx * 1.7, -0.22, 2.0), "obsidian", yaw=-0.2,
              tilt=(-0.1, sx * 0.22))
        spire(kit, f"Floor shard {side}", 0.1, 0.5, (sx * 1.62, 0.62, 0.16), "obsidian", yaw=0.5,
              tilt=(0.12, sx * 0.18))

    chamfer(kit.arch("Arch", inner_radius, outer_radius, 0.7, (0, 0, springing), "obsidian", segments=10),
            width=BEVEL)
    kit.arch("Arch rune inlay", 1.34, 1.44, 0.72, (0, 0, springing), "arcane", segments=10)
    keystone = [(-0.2, 1.1), (0.2, 1.1), (0.27, 1.7), (0.0, 1.8), (-0.27, 1.7)]
    kit.extrude("Keystone", keystone, 0.8, (0, 0, springing), "ember", bevel=SMALL_BEVEL)
    spire(kit, "Crown spire", 0.13, 0.28, (0, 0, 3.72), "obsidian", yaw=math.pi / 4)  # tip at exactly 4.0 m

    # The glowing membrane fills the opening, from the dais to the arch intrados.
    membrane = [(-inner_radius, 0.16 - springing), (inner_radius, 0.16 - springing)]
    membrane += [(inner_radius * math.cos(math.pi * i / 10), inner_radius * math.sin(math.pi * i / 10))
                 for i in range(11)]
    kit.extrude("Portal membrane", membrane, 0.04, (0, 0, springing), "arcane")
    kit.export_asset("portal_gate", triangle_budget=2600, lod_ratio=LOD_RATIO, collision="hull",
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
    offsets = {"portal_gate": 3.3, "spike_trap": 0.0, "loot_pile": -2.1, "destructible_urn": -3.6}
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
    camera_data.lens = 42
    camera = bpy.data.objects.new("Preview camera", camera_data)
    bpy.context.scene.collection.objects.link(camera)
    camera.location = (0.7, 12.0, 4.5)
    camera.rotation_euler = (math.radians(77.3), 0.0, math.radians(180))

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
    kit = spark_kit.Kit(args.repo, "SparkGameARPG", __file__, PALETTE, short="ARPG")
    kit.blend_path = kit.art_dir / "arpg_kit.blend"
    for build in (spike_trap, destructible_urn, loot_pile, portal_gate):
        build(kit)
    check_lod_limits(kit)
    kit.save_blend()
    kit.write_provenance()
    render_preview(kit.art_dir / "preview.png")


main()
