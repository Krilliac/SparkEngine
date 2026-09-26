"""Author the SparkGameVisualScript "Blueprint lab" kit with Blender 4.0.2; Spark Open License 1.0.

Art direction (owner-approved): a blueprint lab where every interactive piece shows its state in
colour and shape. Clear mechanical affordances: handles, rails, plates and lamps with an on/off
state. Every MTL diffuse is one of the four palette colours (Blueprint navy #1B2A4A, Node cyan
#34C6D3, Signal green #58C26B, Off grey #6B7280); shades differ only by roughness and metalness.
Faceted flat shading, 2-4 cm chamfers on hard edges, 3-5 materials per prop, no texture maps.

State language: Signal green is always "on/armed", Off grey is always "off/idle", Node cyan marks
the part a player touches or a script drives (lever grip, door handle, plate rim, lamp junction).

Props face Blender +Y, which the kit export (forward_axis='Z') writes as OBJ +Z; the pivot is the
ground-contact centre (Blender z = 0). Sizes are metres and match the demo world in
GameModules/SparkGameVisualScript/Source/Core/VisualScriptDemoWorld.cpp, which places the lever and
pressure plate at the player spawn and the sliding door flanked by two signal lamps behind the
coin row:
  lever           1.2 m tall floor lever, handle thrown forward (the "on" position)
  sliding_door    2 x 3 m framed door on a top rail, closed, green "unlocked" lamp on the header
  pressure_plate  1.2 m square floor plate, grey pad (idle) with green armed LEDs and a cyan rim
  signal_lamp     2.4 m post with a green (on) and a grey (off) lens

Run (the Workbench preview needs an OpenGL context; on a display-less Linux host wrap Blender in xvfb-run):
  PYTHONHOME=/usr xvfb-run -a blender -b --factory-startup --python-exit-code 1 \
    --python tools/blender/author_visualscript_kit.py -- --repo .
  python3 tools/blender/validate_kit.py Art/Blender/SparkGameVisualScript/provenance.json
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


NAVY = hex_rgb("#1B2A4A")
CYAN = hex_rgb("#34C6D3")
GREEN = hex_rgb("#58C26B")
GREY = hex_rgb("#6B7280")

PALETTE = {
    "navy": (NAVY, 0.72, 0.0),         # painted housings, plinths, door panel
    "navy_steel": (NAVY, 0.38, 0.75),  # blued steel frames
    "cyan": (CYAN, 0.35, 0.0),         # node accents: grips, handles, rims, junctions
    "green": (GREEN, 0.3, 0.0),        # "on" lenses and LEDs
    "grey": (GREY, 0.62, 0.0),         # "off" lenses and idle pads
    "grey_steel": (GREY, 0.3, 0.9),    # bare steel: arms, rails, rollers, posts
}

BEVEL = 0.03          # chamfer on hard edges (art direction: 2-4 cm)
SMALL_BEVEL = 0.02    # chamfer for parts thinner than about 8 cm
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


def lens(kit, name, radius, y, z, mat, x=0.0):
    """Round lamp lens facing +Y (the prop's front), chamfered so it reads as a domed signal."""
    return chamfer(kit.cylinder(name, radius, 0.05, (x, y, z), mat, segments=10, rotation=(HALF_PI, 0, 0)))


def lever(kit):
    """1.2 m floor lever: navy base and housing, steel arm thrown forward, cyan grip, on/off lamps."""
    flat(kit.box("Base plate", (0.62, 0.46, 0.08), (0, 0, 0.04), "navy", bevel=BEVEL))
    flat(kit.box("Housing", (0.34, 0.26, 0.26), (0, -0.02, 0.21), "navy", bevel=BEVEL))
    # Two steel cheeks carry the pivot pin; the arm swings between them along Y.
    for side, x in (("left", -0.13), ("right", 0.13)):
        flat(kit.box(f"Cheek {side}", (0.05, 0.22, 0.2), (x, 0.0, 0.42), "grey_steel", bevel=SMALL_BEVEL))
    pivot_z = 0.46
    chamfer(kit.cylinder("Pivot pin", 0.045, 0.34, (0, 0, pivot_z), "grey_steel", segments=8,
                         rotation=(0, HALF_PI, 0)))
    # Arm leans 22 degrees toward the front: the thrown ("on") position.
    lean = math.radians(22)
    direction = (0.0, math.sin(lean), math.cos(lean))
    arm_length = 0.5
    arm_centre = arm_length / 2
    flat(kit.box("Arm", (0.06, 0.06, arm_length),
                 (0, direction[1] * arm_centre, pivot_z + direction[2] * arm_centre), "grey_steel",
                 bevel=SMALL_BEVEL, rotation=(-lean, 0, 0)))
    grip_length = 0.24
    grip_centre = arm_length + grip_length / 2
    chamfer(kit.cylinder("Grip", 0.05, grip_length,
                         (0, direction[1] * grip_centre, pivot_z + direction[2] * grip_centre), "cyan",
                         segments=8, rotation=(-lean, 0, 0)))
    # Knob: a faceted ball whose top is 1.2 m.
    knob_distance = arm_length + grip_length + 0.02
    knob_z = pivot_z + direction[2] * knob_distance
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=8, v_segments=5, radius=1.2 - knob_z)
    # create_uvsphere emits its faces in a run-dependent order (the vertices are stable), which
    # leaks into the .blend digest and the OBJ normal order; sort them by vertex indices.
    bm.verts.index_update()
    face_keys = {face: tuple(vertex.index for vertex in face.verts) for face in bm.faces}
    face_rank = {key: rank for rank, key in enumerate(sorted(face_keys.values()))}
    bm.faces.sort(key=lambda face: face_rank[face_keys[face]])  # BMesh sort keys must be numbers
    flat(kit._add("Knob", bm, "cyan", (0, direction[1] * knob_distance, knob_z), (0, 0, 0)))
    # State lamps on the housing front: green "on" lit, grey "off" dark.
    lens(kit, "Lamp on", 0.045, 0.12, 0.24, "green", x=0.08)
    lens(kit, "Lamp off", 0.045, 0.12, 0.24, "grey", x=-0.08)
    kit.export_asset("lever", triangle_budget=600, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def sliding_door(kit):
    """2 x 3 m sliding door: blued steel frame, navy panel on a top rail, cyan handle band, green lamp."""
    for side, x in (("left", -0.9), ("right", 0.9)):
        flat(kit.box(f"Jamb {side}", (0.2, 0.3, 2.75), (x, 0, 1.375), "navy_steel", bevel=BEVEL))
    flat(kit.box("Header", (2.0, 0.3, 0.25), (0, 0, 2.875), "navy_steel", bevel=BEVEL))
    flat(kit.box("Top rail", (1.96, 0.08, 0.08), (0, 0.19, 2.66), "grey_steel", bevel=SMALL_BEVEL))
    flat(kit.box("Floor guide", (1.6, 0.1, 0.04), (0, 0.2, 0.02), "grey_steel", bevel=SMALL_BEVEL))
    # Panel hangs in front of the frame from two rollers on the rail.
    flat(kit.box("Panel", (1.64, 0.08, 2.52), (0, 0.2, 1.33), "navy", bevel=BEVEL))
    for side, x in (("left", -0.55), ("right", 0.55)):
        chamfer(kit.cylinder(f"Roller {side}", 0.07, 0.06, (x, 0.2, 2.66), "grey_steel", segments=8,
                             rotation=(HALF_PI, 0, 0)))
        flat(kit.box(f"Hanger {side}", (0.08, 0.04, 0.14), (x, 0.2, 2.56), "grey_steel", bevel=SMALL_BEVEL))
    # Cyan node band across the panel and a vertical pull handle at the leading edge.
    flat(kit.box("Node band", (1.5, 0.03, 0.12), (0, 0.25, 1.9), "cyan", bevel=SMALL_BEVEL))
    flat(kit.box("Pull handle", (0.07, 0.07, 0.7), (0.6, 0.28, 1.1), "cyan", bevel=SMALL_BEVEL))
    for index, z in enumerate((0.78, 1.42)):
        flat(kit.box(f"Handle standoff {index + 1}", (0.05, 0.06, 0.05), (0.6, 0.245, z), "grey_steel",
                     bevel=0.015))
    lens(kit, "Unlocked lamp", 0.07, 0.17, 2.875, "green")
    kit.export_asset("sliding_door", triangle_budget=900, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def pressure_plate(kit):
    """1.2 m square plate: blued steel frame, cyan rim, raised grey pad with a cyan chevron, green LEDs."""
    flat(kit.box("Frame", (1.2, 1.2, 0.08), (0, 0, 0.04), "navy_steel", bevel=BEVEL))
    flat(kit.box("Node rim", (1.04, 1.04, 0.04), (0, 0, 0.09), "cyan", bevel=SMALL_BEVEL))
    flat(kit.box("Pad", (0.94, 0.94, 0.06), (0, 0, 0.12), "grey", bevel=BEVEL))
    # Chevron points to the prop's front (+Y) so the plate's orientation reads from above.
    chevron = [(-0.22, -0.06), (0.0, 0.14), (0.22, -0.06), (0.22, -0.16), (0.0, 0.04), (-0.22, -0.16)]
    kit.extrude("Chevron", chevron[::-1], 0.02, (0, 0, 0.16), "cyan", rotation=(-HALF_PI, 0, 0))
    for index, (x, y) in enumerate(((-0.53, -0.53), (0.53, -0.53), (0.53, 0.53), (-0.53, 0.53))):
        # Chamfer only the cap rims (the 60-degree hexagon side edges stay sharp) to hold the 400-tri budget.
        chamfer(kit.cylinder(f"Armed LED {index + 1}", 0.028, 0.03, (x, y, 0.09), "green", segments=6),
                width=0.008, min_angle=math.radians(75))
    kit.export_asset("pressure_plate", triangle_budget=400, lod_ratio=LOD_RATIO, collision="box",
                     collision_cap=COLLISION_CAP)


def signal_lamp(kit):
    """2.4 m signal post: navy plinth, steel post, cyan node junction, navy head with green/grey lenses."""
    flat(kit.box("Plinth", (0.5, 0.5, 0.12), (0, 0, 0.06), "navy", bevel=BEVEL))
    chamfer(kit.cylinder("Plinth collar", 0.12, 0.1, (0, 0, 0.17), "grey_steel", segments=8))
    flat(kit.cylinder("Post", 0.06, 1.62, (0, 0, 1.03), "grey_steel", segments=8))
    # Node junction box halfway up: where the script "wire" enters the lamp.
    flat(kit.box("Node junction", (0.2, 0.2, 0.22), (0, 0, 1.05), "cyan", bevel=BEVEL))
    # Lamp head: green "on" lens above the grey "off" lens, each under a steel visor.
    flat(kit.box("Head", (0.3, 0.24, 0.54), (0, 0, 2.11), "navy", bevel=BEVEL))
    flat(kit.box("Head cap", (0.36, 0.3, 0.04), (0, 0, 2.38), "navy", bevel=SMALL_BEVEL))
    for label, z, mat in (("on", 2.22, "green"), ("off", 1.98, "grey")):
        lens(kit, f"Lens {label}", 0.09, 0.14, z, mat)
        flat(kit.box(f"Visor {label}", (0.24, 0.1, 0.03), (0, 0.17, z + 0.11), "grey_steel", bevel=0.012))
    kit.export_asset("signal_lamp", triangle_budget=700, lod_ratio=LOD_RATIO, collision="hull",
                     collision_cap=COLLISION_CAP)


def check_lod_limits(kit):
    for record in kit.records:
        lod = record["variants"]["lod1"]["triangle_count"]
        if lod > LOD_LIMIT * record["triangle_count"]:
            raise RuntimeError(f"{record['name']}: LOD1 {lod} exceeds {LOD_LIMIT:.0%} of {record['triangle_count']}")


def canonicalize_vertex_order(kit):
    """Renumber each exported OBJ's positions and normals in sorted order and refresh its hashes.

    The Decimate collapse behind LOD1 leaves the same vertices in a run-dependent order on the lever's
    symmetric knob, which spark_kit.canonicalize_obj (UVs and face order only) does not absorb. Equal
    position or normal lines merge; every face corner keeps its own position, UV and normal values.
    """
    def exports(record):
        yield record
        yield from record["variants"].values()

    for export in (entry for record in kit.records for entry in exports(record)):
        path = kit.repo / export["obj_path"]
        lines = path.read_text(encoding="utf-8").splitlines()
        emitted = {"v": [], "vn": []}
        for line in lines:
            tag = line.split(" ", 1)[0]
            if tag in emitted:
                emitted[tag].append(line)
        remap = {}
        for tag, tag_lines in emitted.items():
            canonical = sorted(set(tag_lines), key=lambda text: ([float(v) for v in text.split()[1:]], text))
            rank = {text: index + 1 for index, text in enumerate(canonical)}
            remap[tag] = ([rank[text] for text in tag_lines], canonical)

        output, faces, written = [], [], set()
        for line in lines + [""]:
            tag = line.split(" ", 1)[0]
            if tag in remap:
                if tag not in written:
                    output.extend(remap[tag][1])
                    written.add(tag)
                continue
            if tag == "f":
                corners = []
                for corner in line.split()[1:]:
                    position, uv, normal = corner.split("/")
                    corners.append(f"{remap['v'][0][int(position) - 1]}/{uv}/{remap['vn'][0][int(normal) - 1]}")
                faces.append("f " + " ".join(corners))
                continue
            output.extend(sorted(faces))
            faces.clear()
            if line:
                output.append(line)
        path.write_text("\n".join(output) + "\n", encoding="utf-8", newline="\n")
        export["obj_sha256"] = spark_kit.sha256(path)


def srgb_to_linear(channel):
    return channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4


def render_preview(path):
    """Workbench preview of the kit (after the .blend is saved; never written back to it).

    The camera looks down -Y at the props' fronts, so +X is screen left.
    """
    layout = {"signal_lamp": (-2.8, 0.0), "sliding_door": (-0.4, -0.4), "lever": (1.8, 0.6),
              "pressure_plate": (3.4, 0.6)}
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
    camera.location = (0.35, 10.5, 3.2)
    camera.rotation_euler = (math.radians(81), 0.0, math.radians(180))

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
    kit = spark_kit.Kit(args.repo, "SparkGameVisualScript", __file__, PALETTE, short="VisualScript")
    kit.blend_path = kit.art_dir / "visualscript_kit.blend"
    lever(kit)
    sliding_door(kit)
    pressure_plate(kit)
    signal_lamp(kit)
    check_lod_limits(kit)
    canonicalize_vertex_order(kit)
    kit.save_blend()
    kit.write_provenance()
    render_preview(kit.art_dir / "preview.png")


main()
