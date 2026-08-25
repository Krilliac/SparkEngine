"""Generate SparkEngine's first authored starter-model library in Blender."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from dataclasses import dataclass
from pathlib import Path

import bpy
from mathutils import Vector


PALETTE = {
    "navy": (0.035, 0.075, 0.14, 1.0),
    "gunmetal": (0.10, 0.16, 0.23, 1.0),
    "steel": (0.34, 0.44, 0.55, 1.0),
    "cyan": (0.03, 0.72, 1.0, 1.0),
    "azure": (0.08, 0.31, 0.92, 1.0),
    "orange": (1.0, 0.28, 0.045, 1.0),
    "red": (0.88, 0.055, 0.095, 1.0),
    "cream": (0.92, 0.86, 0.68, 1.0),
    "gold": (1.0, 0.58, 0.07, 1.0),
    "green": (0.05, 0.66, 0.28, 1.0),
    "purple": (0.43, 0.12, 0.86, 1.0),
    "brown": (0.28, 0.13, 0.065, 1.0),
    "wood": (0.50, 0.25, 0.09, 1.0),
    "stone": (0.38, 0.44, 0.50, 1.0),
    "grass": (0.13, 0.55, 0.19, 1.0),
    "black": (0.012, 0.016, 0.025, 1.0),
    "white": (0.92, 0.95, 1.0, 1.0),
}

PACK_TEMPLATES = {
    "fps_starter": "FPSStarter",
    "mmo_starter": "MMOStarter",
    "platformer_kit": "PlatformerKit",
    "rpg_starter": "RPGStarter",
}


@dataclass
class ModelRecord:
    pack: str
    name: str
    path: Path
    objects: list[bpy.types.Object]


def parse_arguments() -> argparse.Namespace:
    arguments = sys.argv[sys.argv.index("--") + 1 :] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    return parser.parse_args(arguments)


def clean_scene() -> None:
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    for collection in list(bpy.data.collections):
        if collection.name != "Collection":
            bpy.data.collections.remove(collection)


def make_material(name: str, color: tuple[float, float, float, float], metallic: float = 0.0,
                  roughness: float = 0.55, emission: float = 0.0) -> bpy.types.Material:
    material = bpy.data.materials.new(f"M_{name}")
    material.diffuse_color = color
    material.use_nodes = True
    principled = material.node_tree.nodes.get("Principled BSDF")
    principled.inputs["Base Color"].default_value = color
    metallic_input = principled.inputs.get("Metallic") or principled.inputs.get("Metallic IOR Level")
    if metallic_input is None:
        raise RuntimeError("Blender's Principled BSDF has no metallic input")
    metallic_input.default_value = metallic
    principled.inputs["Roughness"].default_value = roughness
    if emission > 0.0:
        principled.inputs["Emission Color"].default_value = color
        principled.inputs["Emission Strength"].default_value = emission
    return material


def assign_material(obj: bpy.types.Object, material: bpy.types.Material) -> None:
    obj.data.materials.append(material)


def apply_bevel(obj: bpy.types.Object, width: float, segments: int = 2) -> None:
    if width <= 0.0:
        return
    modifier = obj.modifiers.new("EdgeSoftening", "BEVEL")
    modifier.width = width
    modifier.segments = segments
    modifier.limit_method = "ANGLE"
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    obj.select_set(False)


def box(name: str, size: tuple[float, float, float], location: tuple[float, float, float],
        material: bpy.types.Material, bevel: float = 0.025, rotation: tuple[float, float, float] = (0, 0, 0)):
    bpy.ops.mesh.primitive_cube_add(location=location, rotation=rotation)
    obj = bpy.context.object
    obj.name = name
    obj.scale = tuple(value / 2.0 for value in size)
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    apply_bevel(obj, min(bevel, min(size) * 0.18))
    assign_material(obj, material)
    return obj


def cylinder(name: str, radius: float, depth: float, location: tuple[float, float, float],
             material: bpy.types.Material, vertices: int = 16,
             rotation: tuple[float, float, float] = (0, 0, 0), bevel: float = 0.015):
    bpy.ops.mesh.primitive_cylinder_add(vertices=vertices, radius=radius, depth=depth, location=location,
                                       rotation=rotation)
    obj = bpy.context.object
    obj.name = name
    apply_bevel(obj, min(bevel, radius * 0.15, depth * 0.1))
    assign_material(obj, material)
    return obj


def sphere(name: str, radius: float, location: tuple[float, float, float], material: bpy.types.Material,
           segments: int = 20, rings: int = 12, scale: tuple[float, float, float] = (1, 1, 1)):
    bpy.ops.mesh.primitive_uv_sphere_add(segments=segments, ring_count=rings, radius=radius, location=location)
    obj = bpy.context.object
    obj.name = name
    obj.scale = scale
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    assign_material(obj, material)
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    return obj


def cone(name: str, radius1: float, radius2: float, depth: float, location: tuple[float, float, float],
         material: bpy.types.Material, vertices: int = 12, rotation: tuple[float, float, float] = (0, 0, 0)):
    bpy.ops.mesh.primitive_cone_add(vertices=vertices, radius1=radius1, radius2=radius2, depth=depth,
                                   location=location, rotation=rotation)
    obj = bpy.context.object
    obj.name = name
    assign_material(obj, material)
    return obj


def torus(name: str, major_radius: float, minor_radius: float, location: tuple[float, float, float],
          material: bpy.types.Material, rotation: tuple[float, float, float] = (0, 0, 0)):
    bpy.ops.mesh.primitive_torus_add(major_radius=major_radius, minor_radius=minor_radius, major_segments=20,
                                    minor_segments=8, location=location, rotation=rotation)
    obj = bpy.context.object
    obj.name = name
    assign_material(obj, material)
    for polygon in obj.data.polygons:
        polygon.use_smooth = True
    return obj


def humanoid(prefix: str, primary, secondary, accent, *, robe: bool = False, broad: bool = False):
    objects = []
    shoulder = 0.31 if broad else 0.24
    objects.append(box(f"{prefix}_torso", (shoulder * 2, 0.23, 0.36), (0, 0, 0.56), primary, 0.045))
    if robe:
        objects.append(cone(f"{prefix}_robe", 0.29, 0.18, 0.48, (0, 0, 0.30), primary, 12))
    else:
        objects.extend([
            cylinder(f"{prefix}_leg_l", 0.075, 0.32, (-0.12, 0, 0.19), secondary, 10),
            cylinder(f"{prefix}_leg_r", 0.075, 0.32, (0.12, 0, 0.19), secondary, 10),
            box(f"{prefix}_boot_l", (0.16, 0.22, 0.10), (-0.12, -0.035, 0.05), secondary, 0.025),
            box(f"{prefix}_boot_r", (0.16, 0.22, 0.10), (0.12, -0.035, 0.05), secondary, 0.025),
        ])
    objects.extend([
        sphere(f"{prefix}_head", 0.15, (0, 0, 0.87), accent, 16, 10, (0.92, 0.86, 1.05)),
        cylinder(f"{prefix}_arm_l", 0.065, 0.34, (-shoulder - 0.045, 0, 0.54), secondary, 10,
                 (0, math.radians(10), 0)),
        cylinder(f"{prefix}_arm_r", 0.065, 0.34, (shoulder + 0.045, 0, 0.54), secondary, 10,
                 (0, math.radians(-10), 0)),
        box(f"{prefix}_belt", (shoulder * 1.95, 0.25, 0.075), (0, 0, 0.41), accent, 0.018),
    ])
    return objects


def build_primitives(materials):
    records = {}
    records["Cube"] = [box("Cube", (1, 1, 1), (0, 0, 0.5), materials["steel"], 0.04)]
    records["Plane"] = [box("Plane", (1, 1, 0.035), (0, 0, 0.0175), materials["stone"], 0.004)]
    records["Pyramid"] = [cone("Pyramid", 0.69, 0.0, 1.0, (0, 0, 0.5), materials["gold"], 4,
                                  (0, 0, math.radians(45)))]
    ramp_mesh = bpy.data.meshes.new("Ramp_mesh")
    ramp_mesh.from_pydata([(-.5,-.5,0),(.5,-.5,0),(-.5,.5,0),(.5,.5,0),(-.5,.5,1),(.5,.5,1)], [],
                          [(0,1,3,2),(2,3,5,4),(0,4,5,1),(0,2,4),(1,5,3)])
    ramp_mesh.update()
    ramp = bpy.data.objects.new("Ramp", ramp_mesh)
    bpy.context.collection.objects.link(ramp)
    assign_material(ramp, materials["orange"])
    records["Ramp"] = [ramp]
    records["Sphere"] = [sphere("Sphere", 0.5, (0, 0, 0.5), materials["cyan"], 24, 16)]
    records["Wall"] = [box("Wall", (1, 0.16, 1), (0, 0, 0.5), materials["gunmetal"], 0.035)]
    return records


def build_fps(materials):
    target = [
        cylinder("target_base", .34, .10, (0,0,.05), materials["gunmetal"], 20),
        cylinder("target_stem", .055, .42, (0,0,.30), materials["steel"], 12),
        box("target_plate", (.58,.10,.48), (0,0,.67), materials["cream"], .045),
        cylinder("target_ring_outer", .20, .025, (0,-.065,.67), materials["red"], 24,
                 (math.radians(90),0,0), .006),
        cylinder("target_ring_inner", .11, .03, (0,-.082,.67), materials["white"], 24,
                 (math.radians(90),0,0), .006),
        cylinder("target_bullseye", .052, .035, (0,-.10,.67), materials["red"], 20,
                 (math.radians(90),0,0), .006),
        box("target_sensor", (.17,.12,.10), (0,0,.96), materials["cyan"], .025),
    ]
    cover = [
        box("cover_foot", (.96,.42,.10), (0,0,.05), materials["black"], .025),
        box("cover_body", (.88,.28,.58), (0,.02,.39), materials["gunmetal"], .055,
            (math.radians(-5),0,0)),
        box("cover_panel", (.64,.035,.34), (0,-.165,.42), materials["steel"], .012,
            (math.radians(-5),0,0)),
        box("cover_stripe", (.62,.045,.075), (0,-.19,.48), materials["orange"], .012,
            (math.radians(-5),0,0)),
        box("cover_light_l", (.08,.05,.08), (-.34,-.19,.62), materials["cyan"], .015),
        box("cover_light_r", (.08,.05,.08), (.34,-.19,.62), materials["cyan"], .015),
    ]
    wall = [
        box("wall_core", (.82,.16,.72), (0,0,.36), materials["gunmetal"], .035),
        box("wall_pillar_l", (.14,.25,.92), (-.43,0,.46), materials["steel"], .03),
        box("wall_pillar_r", (.14,.25,.92), (.43,0,.46), materials["steel"], .03),
        box("wall_panel", (.58,.04,.42), (0,-.10,.42), materials["navy"], .012),
        box("wall_chevron_l", (.22,.055,.065), (-.14,-.13,.42), materials["orange"], .01,
            (0,math.radians(22),0)),
        box("wall_chevron_r", (.22,.055,.065), (.14,-.13,.42), materials["orange"], .01,
            (0,math.radians(-22),0)),
        box("wall_cap", (1.0,.28,.10), (0,0,.95), materials["black"], .025),
    ]
    rifle = [
        box("rifle_receiver", (.46,.16,.18), (0,-.04,.48), materials["gunmetal"], .035),
        box("rifle_stock", (.28,.13,.17), (-.34,.02,.48), materials["navy"], .035,
            (0,math.radians(-8),0)),
        cylinder("rifle_barrel", .035, .48, (.43,-.04,.50), materials["black"], 12,
                 (0,math.radians(90),0)),
        cylinder("rifle_muzzle", .060, .12, (.70,-.04,.50), materials["orange"], 12,
                 (0,math.radians(90),0)),
        box("rifle_magazine", (.13,.13,.25), (.06,-.01,.29), materials["steel"], .025,
            (0,math.radians(-10),0)),
        box("rifle_grip", (.11,.12,.23), (-.10,-.01,.28), materials["black"], .025,
            (0,math.radians(12),0)),
        cylinder("rifle_scope", .055, .25, (.02,-.03,.66), materials["cyan"], 12,
                 (0,math.radians(90),0)),
    ]
    return {"training_target": target, "cover_barrier": cover, "arena_wall": wall, "training_rifle": rifle}


def build_mmo(materials):
    astra = humanoid("astra", materials["azure"], materials["navy"], materials["cream"])
    astra.extend([
        box("astra_shoulder_l", (.22,.28,.14), (-.29,0,.68), materials["gold"], .04),
        box("astra_shoulder_r", (.22,.28,.14), (.29,0,.68), materials["gold"], .04),
        box("astra_visor", (.20,.035,.055), (0,-.14,.89), materials["cyan"], .012),
    ])
    bot = [
        torus("bot_hover_ring", .28,.045,(0,0,.17),materials["cyan"]),
        sphere("bot_body", .30,(0,0,.48),materials["gunmetal"],20,12,(1,.82,.78)),
        cylinder("bot_eye", .075,.055,(0,-.255,.52),materials["orange"],16,(math.radians(90),0,0)),
        box("bot_arm_l", (.28,.11,.12),(-.34,0,.47),materials["steel"],.035),
        box("bot_arm_r", (.28,.11,.12),(.34,0,.47),materials["steel"],.035),
        cone("bot_antenna", .045,0,.30,(0,0,.83),materials["orange"],10),
    ]
    beacon = [
        cylinder("beacon_base", .47,.11,(0,0,.055),materials["gunmetal"],24),
        torus("beacon_ring", .34,.045,(0,0,.17),materials["azure"]),
        *[box(f"beacon_pylon_{i}",(.08,.08,.52),(math.cos(i*math.pi/2)*.30,
              math.sin(i*math.pi/2)*.30,.34),materials["steel"],.018) for i in range(4)],
        cone("beacon_crystal_a",.17,.04,.58,(0,0,.47),materials["cyan"],6),
        cone("beacon_crystal_b",.04,.14,.23,(0,0,.875),materials["cyan"],6),
    ]
    relay = [
        cylinder("relay_base",.36,.12,(0,0,.06),materials["black"],20),
        cone("relay_tower",.25,.11,.58,(0,0,.39),materials["gunmetal"],8),
        torus("relay_ring_low",.25,.035,(0,0,.39),materials["azure"]),
        torus("relay_ring_high",.18,.03,(0,0,.66),materials["cyan"]),
        cylinder("relay_mast",.045,.34,(0,0,.82),materials["steel"],10),
        sphere("relay_node",.09,(0,0,.98),materials["orange"],16,10),
    ]
    return {"astra": astra, "training_bot": bot, "capture_beacon": beacon, "frontier_relay": relay}


def build_platformer(materials):
    runner = humanoid("runner", materials["red"], materials["navy"], materials["cream"])
    runner.extend([
        box("runner_hair",(.24,.16,.10),(0,.03,.99),materials["brown"],.03),
        box("runner_scarf",(.34,.08,.08),(-.12,.08,.66),materials["gold"],.025,
            (0,math.radians(18),0)),
    ])
    platform = [
        box("platform_rock",(.98,.76,.34),(0,0,.17),materials["stone"],.055),
        box("platform_grass",(1.0,.78,.12),(0,0,.40),materials["grass"],.035),
        box("platform_trim",(.82,.04,.08),(0,-.405,.34),materials["gold"],.012),
    ]
    coin = [
        torus("coin_rim",.34,.07,(0,0,.5),materials["gold"],(math.radians(90),0,0)),
        cylinder("coin_face",.28,.055,(0,0,.5),materials["orange"],24,(math.radians(90),0,0)),
        box("coin_mark_v",(.07,.045,.34),(0,-.055,.5),materials["cream"],.015),
        box("coin_mark_h",(.25,.045,.07),(0,-.06,.5),materials["cream"],.015),
    ]
    spikes = [box("spike_base",(.95,.72,.12),(0,0,.06),materials["gunmetal"],.025)]
    for x in (-.32,-.105,.105,.32):
        spikes.append(cone(f"spike_{x}",.105,0,.50,(x,0,.37),materials["red"],8))
    checkpoint = [
        cylinder("checkpoint_base",.22,.10,(0,0,.05),materials["gunmetal"],16),
        cylinder("checkpoint_pole",.045,.72,(0,0,.46),materials["steel"],10),
        torus("checkpoint_ring",.30,.045,(0,0,.70),materials["cyan"],(math.radians(90),0,0)),
        sphere("checkpoint_core",.10,(0,0,.70),materials["gold"],16,10),
    ]
    finish = [
        box("finish_post_l",(.13,.16,.86),(-.38,0,.43),materials["navy"],.035),
        box("finish_post_r",(.13,.16,.86),(.38,0,.43),materials["navy"],.035),
        box("finish_header",(.90,.18,.16),(0,0,.90),materials["red"],.035),
        *[box(f"finish_tile_{i}",(.10,.035,.10),(-.35+i*.10,-.11,.90),
              materials["white" if i%2==0 else "black"],.005) for i in range(8)],
    ]
    return {"runner": runner, "platform_straight": platform, "coin": coin,
            "spike_hazard": spikes, "checkpoint": checkpoint, "finish_gate": finish}


def build_rpg(materials):
    hero = humanoid("hero",materials["azure"],materials["brown"],materials["cream"])
    hero.extend([
        cone("hero_cape",.27,.18,.52,(0,.14,.48),materials["red"],12),
        box("hero_sword",(.055,.055,.60),(.34,-.03,.45),materials["steel"],.015,
            (0,math.radians(-10),0)),
        box("hero_sword_guard",(.22,.07,.055),(.29,-.03,.70),materials["gold"],.012),
    ])
    elder = humanoid("elder",materials["purple"],materials["brown"],materials["cream"],robe=True)
    elder.extend([
        cylinder("elder_staff",.035,.94,(.34,0,.47),materials["wood"],10),
        sphere("elder_staff_gem",.09,(.34,0,.96),materials["cyan"],16,10),
        cone("elder_hat",.23,.03,.24,(0,0,1.10),materials["purple"],16),
    ])
    warden = humanoid("warden",materials["gunmetal"],materials["steel"],materials["red"],broad=True)
    warden.extend([
        cylinder("warden_shield",.30,.08,(-.39,-.02,.52),materials["steel"],8,
                 (math.radians(90),0,0)),
        cylinder("warden_shield_badge",.13,.04,(-.39,-.08,.52),materials["red"],12,
                 (math.radians(90),0,0)),
        box("warden_visor",(.22,.04,.055),(0,-.14,.89),materials["orange"],.012),
    ])
    relic = [
        cylinder("relic_base",.38,.14,(0,0,.07),materials["stone"],16),
        cone("relic_crystal_low",.23,.10,.56,(0,0,.43),materials["purple"],6),
        cone("relic_crystal_high",.10,0,.30,(0,0,.86),materials["cyan"],6),
        torus("relic_ring",.31,.035,(0,0,.55),materials["gold"]),
    ]
    def house(prefix, roof_color, door_x):
        objects = [
            box(f"{prefix}_body",(.82,.72,.58),(0,0,.29),materials["cream"],.025),
            cone(f"{prefix}_roof",.69,0,.50,(0,0,.83),roof_color,4,(0,0,math.radians(45))),
            box(f"{prefix}_door",(.20,.035,.36),(door_x,-.375,.18),materials["brown"],.015),
            box(f"{prefix}_window",(.20,.035,.18),(-door_x,-.38,.36),materials["cyan"],.015),
        ]
        for x in (-.42,.42):
            objects.append(box(f"{prefix}_beam_{x}",(.055,.77,.62),(x,0,.31),materials["wood"],.01))
        objects.append(box(f"{prefix}_beam_top",(.88,.77,.055),(0,0,.57),materials["wood"],.01))
        return objects
    return {"hero": hero, "village_elder": elder, "training_warden": warden, "lost_relic": relic,
            "village_house_a": house("house_a",materials["red"],-.18),
            "village_house_b": house("house_b",materials["navy"],.18)}


def export_model(record: ModelRecord) -> None:
    record.path.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="DESELECT")
    for obj in record.objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = record.objects[0]
    bpy.ops.wm.obj_export(filepath=str(record.path), export_selected_objects=True, apply_modifiers=True,
                          apply_transform=True, export_uv=True, export_normals=True, export_materials=True,
                          export_triangulated_mesh=True, export_object_groups=True, export_material_groups=True,
                          forward_axis="Z", up_axis="Y", path_mode="RELATIVE")
    bpy.ops.object.select_all(action="DESELECT")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def obj_bounds(path: Path):
    vertices = []
    with path.open("r", encoding="utf-8") as stream:
        for line in stream:
            if line.startswith("v "):
                vertices.append(tuple(float(value) for value in line.split()[1:4]))
    return {"min": [min(vertex[i] for vertex in vertices) for i in range(3)],
            "max": [max(vertex[i] for vertex in vertices) for i in range(3)]}


def sync_template_asset_manifests(root: Path, records: list[ModelRecord]) -> None:
    """Declare every generated OBJ/MTL in template provenance and lock files."""
    lock_path = root / "Templates" / "assets.lock.json"
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    locked_assets = lock["assets"]

    for pack, template_name in PACK_TEMPLATES.items():
        manifest_path = root / "Templates" / template_name / "Assets" / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        # Replace the generated model surface atomically on each run while
        # preserving the independently-authored atlas/runtime-sheet entries.
        manifest["assets"] = [
            entry for entry in manifest["assets"]
            if not str(entry.get("path", "")).replace("\\", "/").startswith("Models/")
        ]
        prefix = f"{template_name}/Assets/Models/"
        for key in [key for key in locked_assets if key.startswith(prefix)]:
            del locked_assets[key]

        for record in sorted((item for item in records if item.pack == pack), key=lambda item: item.name):
            for asset_path, kind in (
                (record.path, "triangulated Wavefront OBJ model"),
                (record.path.with_suffix(".mtl"), "Wavefront material color library"),
            ):
                relative = asset_path.relative_to(manifest_path.parent).as_posix()
                digest = sha256(asset_path)
                manifest["assets"].append({
                    "path": relative,
                    "kind": kind,
                    "origin": "SparkEngine deterministic Blender 5.2.0 LTS model pipeline",
                    "sha256": digest,
                })
                lock_key = f"{template_name}/Assets/{relative}"
                locked_assets[lock_key] = digest

        manifest["enginePrimitives"] = ["__spark_primitive_ground__.obj"]
        manifest["notes"] = (
            "The reflected scene uses repository-original authored OBJ/MTL models; "
            "the built-in ground primitive remains for the world floor, and the atlas "
            "provides complementary UI and sprite art."
        )
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    lock["assets"] = dict(sorted(locked_assets.items()))
    lock_path.write_text(json.dumps(lock, indent=2) + "\n", encoding="utf-8")


def look_at(obj: bpy.types.Object, point: tuple[float, float, float]) -> None:
    obj.rotation_euler = (Vector(point) - obj.location).to_track_quat("-Z", "Y").to_euler()


def render_pack(pack: str, records: list[ModelRecord], output: Path, materials) -> None:
    for record in records:
        for obj in record.objects:
            obj.hide_render = record.pack != pack
            obj.hide_viewport = record.pack != pack

    current = [record for record in records if record.pack == pack]
    columns = 3
    spacing_x, spacing_y = 3.0, 2.65
    rows = math.ceil(len(current) / columns)
    preview_helpers = []
    for index, record in enumerate(current):
        row, column = divmod(index, columns)
        center_x = (column - (columns - 1) / 2.0) * spacing_x
        center_y = (row - (rows - 1) / 2.0) * spacing_y
        points = [obj.matrix_world @ Vector(corner) for obj in record.objects for corner in obj.bound_box]
        minimum = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
        maximum = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
        scale = 1.55 / max(maximum.x-minimum.x, maximum.y-minimum.y, maximum.z-minimum.z)
        pivot = bpy.data.objects.new(f"Preview_{record.name}", None)
        bpy.context.collection.objects.link(pivot)
        pivot.location = (center_x, center_y, 0)
        pivot.scale = (scale, scale, scale)
        for obj in record.objects:
            obj.parent = pivot
        preview_helpers.append(pivot)

        bpy.ops.object.text_add(location=(center_x - .72, center_y - .92, .04),
                                rotation=(0, 0, 0))
        label = bpy.context.object
        label.data.body = record.name.replace("_", " ").upper()
        label.data.align_x = "LEFT"
        label.data.size = .20
        label.data.extrude = .008
        label.data.materials.append(materials["white"])
        preview_helpers.append(label)

    bpy.ops.mesh.primitive_plane_add(size=30, location=(0, 0, -0.015))
    ground = bpy.context.object
    ground.name = "PreviewGround"
    ground.data.materials.append(materials["black"])
    preview_helpers.append(ground)

    camera = bpy.data.objects.get("PreviewCamera")
    camera.location = (6.8, -10.8, 8.8)
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = max(8.8, rows * 3.1)
    look_at(camera, (0, 0, .55))
    bpy.context.scene.camera = camera
    bpy.context.scene.render.filepath = str(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.render.render(write_still=True)

    for helper in preview_helpers:
        if helper and helper.name in bpy.data.objects:
            bpy.data.objects.remove(helper, do_unlink=True)
    for record in current:
        for obj in record.objects:
            obj.parent = None


def setup_render(materials) -> None:
    scene = bpy.context.scene
    # Blender 5.2's packaged API reports the Eevee engine as BLENDER_EEVEE;
    # older 4.x builds used BLENDER_EEVEE_NEXT.
    try:
        scene.render.engine = "BLENDER_EEVEE"
    except TypeError:
        scene.render.engine = "BLENDER_EEVEE_NEXT"
    scene.render.resolution_x = 1920
    scene.render.resolution_y = 1080
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = False
    scene.world.color = (0.008, 0.015, 0.028)
    scene.view_settings.look = "AgX - Medium High Contrast"

    camera_data = bpy.data.cameras.new("PreviewCamera")
    camera = bpy.data.objects.new("PreviewCamera", camera_data)
    bpy.context.collection.objects.link(camera)

    def area(name, location, energy, color, size):
        data = bpy.data.lights.new(name, "AREA")
        data.energy = energy
        data.color = color
        data.shape = "DISK"
        data.size = size
        obj = bpy.data.objects.new(name, data)
        obj.location = location
        bpy.context.collection.objects.link(obj)
        look_at(obj, (0, 0, .4))
    area("Key", (-5, -6, 9), 1450, (0.73, 0.87, 1.0), 6.0)
    area("Rim", (6, 2, 7), 1200, (0.15, 0.55, 1.0), 5.0)
    area("Fill", (0, -2, 5), 750, (1.0, 0.35, 0.12), 4.0)


def main() -> None:
    root = parse_arguments().repo_root.resolve()
    clean_scene()
    materials = {name: make_material(name, color, metallic=.55 if name in {"gunmetal","steel","gold"} else 0,
                                     roughness=.28 if name in {"cyan","orange","gold"} else .58,
                                     emission=.25 if name in {"cyan","orange"} else 0)
                 for name, color in PALETTE.items()}

    definitions = {
        "core_primitives": (build_primitives(materials), root / "Assets" / "Models"),
        "fps_starter": (build_fps(materials), root / "Templates" / "FPSStarter" / "Assets" / "Models"),
        "mmo_starter": (build_mmo(materials), root / "Templates" / "MMOStarter" / "Assets" / "Models"),
        "platformer_kit": (build_platformer(materials), root / "Templates" / "PlatformerKit" / "Assets" / "Models"),
        "rpg_starter": (build_rpg(materials), root / "Templates" / "RPGStarter" / "Assets" / "Models"),
    }
    records = []
    for pack, (models, directory) in definitions.items():
        for name, objects in models.items():
            records.append(ModelRecord(pack, name, directory / f"{name}.obj", objects))
    for record in records:
        export_model(record)
    sync_template_asset_manifests(root, records)

    setup_render(materials)
    previews = []
    for pack in definitions:
        output = root / "docs" / "images" / "model-pipeline" / f"{pack}.png"
        render_pack(pack, records, output, materials)
        previews.append(output.relative_to(root).as_posix())

    assets = []
    for record in records:
        assets.append({
            "pack": record.pack,
            "name": record.name,
            "path": record.path.relative_to(root).as_posix(),
            "sha256": sha256(record.path),
            "mtlSha256": sha256(record.path.with_suffix(".mtl")),
            "bounds": obj_bounds(record.path),
        })
    manifest = {
        "schema": 1,
        "generator": "Tools/model_pipeline/generate_starter_models.py",
        "coordinateSystem": "left-handed, Y-up, +Z-forward",
        "unit": "meter",
        "format": "triangulated OBJ/MTL",
        "blenderVersion": bpy.app.version_string,
        "assets": assets,
        "previews": previews,
    }
    manifest_path = root / "Assets" / "Models" / "Generated" / "starter_assets_manifest.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Generated {len(records)} models and {len(previews)} previews")


if __name__ == "__main__":
    main()
