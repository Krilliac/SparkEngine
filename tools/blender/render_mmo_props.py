"""Render exported MMO models in a consistent Blender CPU review setup.

Uses the runtime's diffuse-color material subset; this is not a D3D11 capture.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

import bpy
from mathutils import Vector


def look_at(obj, point):
    obj.rotation_euler = (point - obj.location).to_track_quat('-Z', 'Y').to_euler()


def render(path, destination):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.wm.obj_import(filepath=str(path), forward_axis='NEGATIVE_Z', up_axis='Y')
    objects = [obj for obj in bpy.context.scene.objects if obj.type == 'MESH']
    points = [obj.matrix_world @ vertex.co for obj in objects for vertex in obj.data.vertices]
    if not points:
        raise RuntimeError('Imported model contains no geometry')
    minimum = Vector([min(v[axis] for v in points) for axis in range(3)])
    maximum = Vector([max(v[axis] for v in points) for axis in range(3)])
    center = (minimum + maximum) * .5
    size = max(maximum - minimum)
    for material in bpy.data.materials:
        color = tuple(material.diffuse_color)
        material.use_nodes = True
        node = material.node_tree.nodes.get('Principled BSDF')
        node.inputs['Base Color'].default_value = color
        node.inputs['Metallic'].default_value = 0
        node.inputs['Roughness'].default_value = .65
    bpy.ops.mesh.primitive_plane_add(size=size * 200, location=(center.x, center.y, minimum.z - size * .01))
    floor = bpy.data.materials.new('Review floor')
    floor.diffuse_color = (.045, .055, .075, 1)
    floor.use_nodes = True
    floor.node_tree.nodes['Principled BSDF'].inputs['Base Color'].default_value = floor.diffuse_color
    floor.node_tree.nodes['Principled BSDF'].inputs['Roughness'].default_value = .8
    bpy.context.object.data.materials.append(floor)
    bpy.ops.object.camera_add(location=center + Vector((1.6, -2.5, 1.5)) * size)
    camera = bpy.context.object
    camera.data.type = 'ORTHO'
    camera.data.ortho_scale = size * 1.75
    look_at(camera, center)
    scene = bpy.context.scene
    scene.camera = camera
    for location, energy, scale in [((-3, -4, 5), 250, 3), ((3, -1, 3), 125, 3), ((0, 3, 4), 220, 2)]:
        bpy.ops.object.light_add(type='AREA', location=center + Vector(location) * size)
        light = bpy.context.object
        light.data.energy = energy * size * size
        light.data.shape = 'DISK'
        light.data.size = scale * size
        look_at(light, center)
    scene.world = bpy.data.worlds.new('Review world')
    scene.world.use_nodes = True
    scene.world.node_tree.nodes['Background'].inputs['Color'].default_value = (.12, .15, .20, 1)
    scene.world.node_tree.nodes['Background'].inputs['Strength'].default_value = .4
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = 24
    scene.cycles.use_denoising = False
    scene.cycles.max_bounces = 4
    scene.render.threads_mode = 'FIXED'
    scene.render.threads = 4
    scene.render.resolution_x = 360
    scene.render.resolution_y = 360
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.render.filepath = str(destination)
    bpy.ops.render.render(write_still=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--names', nargs='+', required=True)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    repo = args.repo.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        for name in args.names:
            if Path(name).name != name or not name.replace('_', '').isalnum():
                raise ValueError('Expected a model name, not a path')
            relative = 'Assets/Models/MMO/' + name + '.obj'
            original = Path(temporary) / (name + '.obj')
            original.write_bytes(subprocess.check_output(
                ['git', '-C', str(repo), 'show', 'ff951c19a5e12be6ec3724696af6fe030070e8c4:' + relative]))
            render(original, args.output / (name + '-before.png'))
            render(repo / relative, args.output / (name + '-candidate.png'))


if __name__ == '__main__':
    main()
