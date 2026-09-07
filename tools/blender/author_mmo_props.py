#!/usr/bin/env python3
"""Author original MMO props using Blender 4.0; Spark Open License 1.0.
Run: PYTHONHOME=/usr blender -b --factory-startup --python tools/blender/author_mmo_props.py -- --repo .
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import random
import subprocess
import sys

import bpy
from mathutils import Vector

PALETTE = {
    'oak': ((0.30, 0.13, 0.045), 0.78, 0.0),
    'honey': ((0.53, 0.29, 0.10), 0.72, 0.0),
    'darkwood': ((0.11, 0.045, 0.018), 0.88, 0.0),
    'iron': ((0.105, 0.125, 0.14), 0.4, 0.75),
    'iron_edge': ((0.26, 0.29, 0.31), 0.36, 0.65),
    'brass': ((0.62, 0.39, 0.10), 0.32, 0.72),
    'stone': ((0.41, 0.43, 0.40), 0.9, 0.0),
    'limestone': ((0.67, 0.65, 0.53), 0.86, 0.0),
    'plaster': ((0.78, 0.69, 0.51), 0.9, 0.0),
    'teal': ((0.055, 0.25, 0.23), 0.65, 0.0),
    'teal_light': ((0.12, 0.37, 0.32), 0.68, 0.0),
    'cream': ((0.82, 0.73, 0.49), 0.87, 0.0),
    'red': ((0.53, 0.10, 0.045), 0.76, 0.0),
    'coal': ((0.045, 0.032, 0.028), 0.92, 0.0),
    'ember': ((0.90, 0.19, 0.02), 0.5, 0.0),
    'flame': ((1.0, 0.59, 0.035), 0.4, 0.0),
    'pine': ((0.08, 0.23, 0.095), 0.9, 0.0),
    'pine_light': ((0.18, 0.34, 0.13), 0.9, 0.0),
    'water': ((0.06, 0.42, 0.44), 0.2, 0.1),
    'purple': ((0.38, 0.08, 0.41), 0.25, 0.15),
}
MATERIALS = {}
PARTS = []


def material(name):
    if name not in MATERIALS:
        color, roughness, metal = PALETTE[name]
        mat = bpy.data.materials.new('MMO_' + name)
        mat.diffuse_color = (*color, 1)
        mat.use_nodes = True
        node = mat.node_tree.nodes.get('Principled BSDF')
        node.inputs['Base Color'].default_value = (*color, 1)
        node.inputs['Roughness'].default_value = roughness
        node.inputs['Metallic'].default_value = metal
        MATERIALS[name] = mat
    return MATERIALS[name]


def finish(obj, name, mat, bevel=0):
    obj.name = name
    obj.data.materials.append(material(mat))
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bevel:
        mod = obj.modifiers.new('Crafted edge chamfer', 'BEVEL')
        mod.width = min(bevel, min(obj.dimensions)*0.45)
        mod.segments = 1
        bpy.ops.object.modifier_apply(modifier=mod.name)
    PARTS.append(obj)
    return obj


def box(name, location, size, mat='oak', bevel=0.015):
    bpy.ops.mesh.primitive_cube_add(size=1, location=location)
    obj = bpy.context.object
    obj.scale = size
    return finish(obj, name, mat, bevel)


def cylinder(name, location, radius, depth, mat='iron', vertices=16, radius_top=None):
    bpy.ops.mesh.primitive_cone_add(vertices=vertices, radius1=radius,
                                   radius2=radius if radius_top is None else radius_top,
                                   depth=depth, location=location)
    obj = finish(bpy.context.object, name, mat)
    for poly in obj.data.polygons:
        poly.use_smooth = len(poly.vertices) == 4
    return obj


def beam(name, start, end, radius=0.035, mat='iron', vertices=8):
    start, end = Vector(start), Vector(end)
    obj = cylinder(name, (start + end) / 2, radius, (end - start).length, mat, vertices)
    obj.rotation_euler = (end - start).to_track_quat('Z', 'Y').to_euler()
    return obj


def torus(name, location, radius, tube=0.025, mat='iron', rotation=(0, 0, 0)):
    bpy.ops.mesh.primitive_torus_add(major_radius=radius, minor_radius=tube,
                                    major_segments=20, minor_segments=6,
                                    location=location, rotation=rotation)
    obj = finish(bpy.context.object, name, mat)
    for poly in obj.data.polygons:
        poly.use_smooth = True
    return obj


def mesh(name, vertices, faces, mat='stone', bevel=0):
    data = bpy.data.meshes.new(name)
    data.from_pydata(vertices, [], faces)
    data.update()
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    obj = finish(obj, name, mat, bevel)
    if name in {'Striped canvas awning', 'Heavy canvas slope', 'Open entrance drape', 'Swallowtail woven banner'}:
        # Cloth needs a back surface in the runtime's back-face-culled renderer.
        modifier = obj.modifiers.new('Woven fabric thickness', 'SOLIDIFY')
        modifier.thickness = 0.012
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    return obj


def prism(name, profile, depth, mat='stone', bevel=0.01, y=0):
    count = len(profile)
    vertices = [(x, y - depth/2, z) for x, z in profile] + [(x, y + depth/2, z) for x, z in profile]
    faces = [tuple(range(count-1, -1, -1)), tuple(range(count, 2*count))]
    faces += [(i, (i+1)%count, (i+1)%count+count, i+count) for i in range(count)]
    return mesh(name, vertices, faces, mat, bevel)


def lathe(name, profile, mat='iron', segments=20, location=(0, 0, 0)):
    vertices = [(r*math.cos(2*math.pi*i/segments)+location[0],
                 r*math.sin(2*math.pi*i/segments)+location[1], z+location[2])
                for z, r in profile for i in range(segments)]
    faces = [tuple(range(segments-1, -1, -1))]
    for j in range(len(profile)-1):
        for i in range(segments):
            k = j*segments+i
            n = j*segments+(i+1)%segments
            faces.append((k, n, n+segments, k+segments))
    faces.append(tuple(range((len(profile)-1)*segments, len(profile)*segments)))
    obj = mesh(name, vertices, faces, mat)
    for poly in obj.data.polygons:
        poly.use_smooth = len(poly.vertices) == 4
    return obj


def ico(name, location, scale, mat='stone', subdivisions=1):
    bpy.ops.mesh.primitive_ico_sphere_add(subdivisions=subdivisions, radius=1, location=location)
    obj = bpy.context.object
    obj.scale = scale
    return finish(obj, name, mat)


def roof(width, depth, base, rise):
    prism('Gable plaster', [(-width/2, base), (width/2, base), (0, base+rise)], depth*0.88, 'plaster', 0)
    slope = math.atan2(rise, width/2)
    for side in (-1, 1):
        for row in range(3):
            t = (row+0.5)/3
            for col in range(4):
                obj = box('Overlapping roof shingle',
                          (side*width*t/2, (col-1.5)*depth/4, base+rise*(1-t)+0.04),
                          (math.hypot(width/2, rise)/3+0.055, depth/4-0.018, 0.06),
                          'teal' if (row+col)%2 else 'teal_light', 0.008)
                obj.rotation_euler.y = side*slope
    beam('Roof ridge', (0, -depth/2-0.06, base+rise+0.055),
         (0, depth/2+0.06, base+rise+0.055), 0.065, 'darkwood')


def chest():
    box('Chest dark interior', (0, 0, 0.4), (1.6, 1.1, 0.75), 'darkwood')
    for i in range(6):
        for y in (-0.565, 0.565):
            box('Individual chest board', ((i-2.5)*0.255, y, 0.42), (0.245, 0.065, 0.70), 'honey' if i%2 else 'oak')
    for x in (-0.79, 0.79):
        box('End panel', (x, 0, 0.4), (0.075, 1.06, 0.7), 'oak')
    for i in range(10):
        a, b = i*math.pi/10, (i+1)*math.pi/10
        profile = [(0.57*math.cos(a), 0.77+0.43*math.sin(a)),
                   (0.57*math.cos(b), 0.77+0.43*math.sin(b)),
                   (0.53*math.cos(b), 0.77+0.39*math.sin(b)),
                   (0.53*math.cos(a), 0.77+0.39*math.sin(a))]
        obj = prism('Curved lid plank', profile, 1.6, 'honey' if i%2 else 'oak', 0.004)
        obj.rotation_euler.z = math.pi/2
    for side in (-1, 1):
        profile = [(-0.55, 0.77), (0.55, 0.77)]
        profile += [(0.55*math.cos(i*math.pi/12), 0.77+0.41*math.sin(i*math.pi/12))
                    for i in range(1, 12)]
        panel = prism('Arched wooden lid end', profile, 0.05, 'oak', 0.006, side*0.78)
        panel.rotation_euler.z = math.pi/2
    for x in (-0.59, 0.59):
        for y in (-0.61, 0.61):
            box('Iron strap', (x, y, 0.42), (0.12, 0.035, 0.77), 'iron', 0.005)
        for i in range(10):
            a, b = i*math.pi/10, (i+1)*math.pi/10
            beam('Curved lid iron band', (x, 0.58*math.cos(a), 0.78+0.44*math.sin(a)),
                 (x, 0.58*math.cos(b), 0.78+0.44*math.sin(b)), 0.033, 'iron', 6)
    box('Latch plate', (0, -0.64, 0.68), (0.23, 0.05, 0.27), 'brass', 0.02)
    torus('Latch ring', (0, -0.68, 0.60), 0.065, 0.015, 'iron', (math.pi/2, 0, 0))
    for x in (-0.66, 0.66):
        for y in (-0.43, 0.43):
            box('Chest foot', (x, y, 0.025), (0.22, 0.22, 0.15), 'darkwood')


def anvil():
    cylinder('Oak stump', (0, 0, 0.22), 0.53, 0.44, 'oak', 12)
    torus('Stump iron hoop', (0, 0, 0.13), 0.52, 0.025)
    prism('Forged waist', [(-0.55, 0.44), (0.55, 0.44), (0.35, 0.6),
                         (0.22, 0.82), (0.62, 1.03), (-0.67, 1.03),
                         (-0.30, 0.82), (-0.33, 0.6)], 0.5, 'iron', 0.025)
    box('Hardened face', (0, 0, 1.07), (1.45, 0.58, 0.14), 'iron_edge', 0.025)
    horn = cylinder('Tapered horn', (0.96, 0, 1.02), 0.25, 0.65, 'iron_edge', 12, 0.025)
    horn.rotation_euler.y = math.pi/2
    box('Heel', (-0.84, 0, 1.00), (0.28, 0.4, 0.16), 'iron', 0.02)
    for x in (-0.38, 0.38):
        for y in (-0.26, 0.26):
            cylinder('Mounting bolt', (x, y, 0.46), 0.055, 0.06, 'iron_edge', 6)


def forge():
    box('Stone hearth', (0, 0, 0.12), (1.6, 1.25, 0.24), 'stone', 0.04)
    for side in (-1, 1):
        for layer in range(3):
            box('Hearth masonry', (side*0.64, 0, 0.35+layer*0.26), (0.31, 1.12, 0.245),
                'stone' if layer%2 else 'limestone', 0.025)
    box('Soot firebox back', (0, 0.45, 0.59), (1.02, 0.12, 0.78), 'coal', 0)
    box('Lintel', (0, 0, 1.02), (1.58, 1.18, 0.23), 'limestone', 0.035)
    for layer in range(4):
        box('Chimney course', (0.12, 0.3, 1.24+layer*0.24), (0.65, 0.6, 0.225), 'stone', 0.025)
    box('Chimney crown', (0.12, 0.3, 2.13), (0.82, 0.74, 0.12), 'limestone')
    box('Chimney dark opening', (0.12, 0.3, 2.2), (0.45, 0.37, 0.018), 'coal', 0)
    for i in range(6):
        beam('Hearth grate', (-0.46+i*0.185, -0.52, 0.28), (-0.46+i*0.185, 0.37, 0.28), 0.025)
        ico('Hot coal', (-0.4+i*0.16, 0, 0.34), (0.14, 0.18, 0.10), 'ember')
    for i in range(4):
        ico('Fire tongue', (-0.32+i*0.21, 0.08, 0.48), (0.095, 0.11, 0.28 if i%2 else 0.19), 'flame')
    beam('Tool rail', (-0.85, -0.2, 0.75), (-0.85, 0.4, 0.75), 0.025)


def guild_hall():
    box('Stone foundation', (0, 0, 0.13), (2.4, 1.8, 0.26), 'stone', 0.04)
    box('Lime plaster walls', (0, 0, 0.86), (2.13, 1.55, 1.20), 'plaster', 0.02)
    for x in (-1.08, 0, 1.08):
        for y in (-0.8, 0.8):
            box('Timber upright', (x, y, 0.87), (0.13, 0.13, 1.23), 'darkwood')
    for z in (0.3, 1.45):
        for y in (-0.81, 0.81):
            box('Timber sill', (0, y, z), (2.22, 0.13, 0.14), 'oak')
    for x in (-0.73, 0.73):
        box('Window recess', (x, -0.81, 1.03), (0.4, 0.06, 0.42), 'coal', 0.02)
        box('Window amber pane', (x, -0.85, 1.03), (0.33, 0.02, 0.34), 'brass', 0)
        box('Window mullion', (x, -0.875, 1.03), (0.045, 0.03, 0.38), 'darkwood', 0)
        box('Window transom', (x, -0.875, 1.03), (0.36, 0.03, 0.045), 'darkwood', 0)
    box('Door shadow', (0, -0.84, 0.7), (0.49, 0.08, 0.83), 'darkwood')
    for i in range(4):
        box('Door plank', ((i-1.5)*0.105, -0.90, 0.69), (0.095, 0.05, 0.75), 'honey', 0.007)
    box('Doorstep', (0, -0.98, 0.25), (0.7, 0.42, 0.16), 'limestone')
    roof(2.55, 1.95, 1.49, 0.8)
    box('Guild crest shield', (0, -0.91, 1.68), (0.3, 0.08, 0.31), 'teal', 0.04)
    box('Guild crest brass mark', (0, -0.96, 1.68), (0.06, 0.025, 0.2), 'brass', 0.004)



def alchemy_shop():
    box('Apothecary foundation', (0, 0, .1), (1.65, 1.3, .2), 'stone', .03)
    box('Apothecary walls', (0, 0, .83), (1.4, 1.1, 1.35), 'plaster')
    for x in (-.73, .73):
        for y in (-.57, .57):
            box('Frame post', (x, y, .82), (.11, .11, 1.38), 'darkwood')
    roof(1.8, 1.5, 1.5, .64)
    box('Shop door', (-.34, -.58, .63), (.44, .08, .95), 'oak')
    for z in (.36, .87):
        box('Door hinge', (-.46, -.635, z), (.18, .03, .055), 'iron', .008)
    box('Bottle display recess', (.37, -.57, .91), (.54, .06, .72), 'darkwood')
    for z in (.62, .91, 1.17):
        box('Display shelf', (.37, -.74, z), (.65, .4, .05), 'honey', .008)
    for row in range(2):
        for col in range(3):
            x = .14+col*.22
            z = .70+row*.29
            ico('Potion flask', (x, -.75, z), (.075, .075, .08), ('teal_light','purple','ember')[col], 2)
            cylinder('Bottle neck', (x, -.75, z+.09), .026, .08, 'brass', 8)
    beam('Shop sign bracket', (.76, -.3, 1.40), (1.02, -.3, 1.4), .028)
    sign = cylinder('Round apothecary sign', (.98, -.3, 1.12), .18, .055, 'honey', 16)
    sign.rotation_euler.x = math.pi/2
    ico('Raised potion symbol', (.98, -.34, 1.10), (.07, .022, .08), 'purple', 1)
    box('Sign bottle neck', (.98, -.34, 1.19), (.042, .022, .09), 'purple', .007)


def market_stall():
    for x in (-.92, .92):
        for y in (-.58, .58):
            box('Canopy post', (x,y,.82), (.095,.095,1.64), 'oak')
    box('Counter body', (0,-.27,.44), (1.85,.83,.8), 'darkwood')
    for i in range(8):
        box('Counter plank', ((i-3.5)*.225,-.715,.45), (.213,.045,.72), 'honey' if i%2 else 'oak', .008)
    for y in (-.61,-.36,-.11,.14):
        box('Counter top board', (0,y,.86), (1.95,.24,.065), 'honey', .009)
    for stripe in range(8):
        x0,x1=(stripe-4)*.25,(stripe-3)*.25
        verts=[]
        for y,z in [(-.82,1.53),(-.42,1.72),(.2,1.77),(.76,1.55)]:
            verts.extend([(x0,y,z),(x1-.005,y,z)])
        mesh('Striped canvas awning',verts,[(0,1,3,2),(2,3,5,4),(4,5,7,6)],'cream' if stripe%2 else 'teal')
        prism('Scalloped valance', [(x0,1.53),(x1-.005,1.53),(x1-.02,1.40),((x0+x1)/2,1.35),(x0+.02,1.4)], .03,
              'cream' if stripe%2 else 'teal', 0, -.83)
    for group,x in enumerate((-.6,0,.6)):
        box('Produce tray', (x,-.2,.92), (.5,.52,.08), 'oak', .01)
        for j in range(6):
            ico('Produce', (x+(j%3-1)*.13,-.30+(j//3)*.17,1.02), (.083,.077,.087),
                ('red','flame','pine_light')[group])
    beam('Canopy ridge',(-1,.2,1.81),(1,.2,1.81),.04,'darkwood')


def gate():
    for side in (-1,1):
        for row in range(5):
            box('Gate pier stone',(side*.83,0,.15+row*.29),(.43,.55,.275),'stone' if row%2 else 'limestone',.025)
        box('Pier plinth',(side*.83,0,.06),(.59,.72,.12),'stone',.025)
    for i in range(9):
        a,b=i*math.pi/9+.007,(i+1)*math.pi/9-.007
        profile=[(.64*math.cos(a),1.43+.64*math.sin(a)),
                 (.97*math.cos(a),1.43+.97*math.sin(a)),
                 (.97*math.cos(b),1.43+.97*math.sin(b)),
                 (.64*math.cos(b),1.43+.64*math.sin(b))]
        prism('Arch voussoir',profile,.56,'limestone' if i%2 else 'stone',.012)
    for i in range(7):
        x=(i-3)*.18
        top=1.40+math.sqrt(max(0,.62**2-x*x))
        beam('Portcullis iron upright',(x,.17,.14),(x,.17,top),.026,'iron',8)
    for z in (.48,.97,1.43):
        box('Portcullis rail',(0,.17,z),(1.3,.06,.06),'iron',.008)


def gravestone():
    profile=[(-.44,.16),(.44,.16),(.44,.95)]
    profile += [(.44*math.cos(i*math.pi/12),.95+.44*math.sin(i*math.pi/12)) for i in range(1,13)]
    prism('Rounded memorial',profile,.24,'stone',.025)
    box('Memorial base',(0,0,.10),(1.03,.55,.20),'limestone',.025)
    box('Raised cross stem',(0,-.145,.98),(.07,.05,.4),'limestone',.012)
    box('Raised cross arm',(0,-.145,1.06),(.28,.05,.07),'limestone',.012)
    for i,width in enumerate((.43,.34,.39)):
        box('Inscribed memorial line',(0,-.13,.64-i*.095),(width,.022,.025),'coal',.004)
    for i in range(3):
        ico('Grave edge stone',(-.35+i*.35,-.3,.085),(.17,.16,.10),'stone',1)


def rock_large():
    rng=random.Random(9127)
    obj=ico('Fractured granite core',(0,0,.62),(1.0,.82,.85),'stone',2)
    for v in obj.data.vertices:
        v.co*=rng.uniform(.88,1.12)
        if v.co.z>.4:
            v.co.z=.58+(v.co.z-.4)*.5
    obj.data.materials.append(material('limestone'))
    obj.data.materials.append(material('iron_edge'))
    for face in obj.data.polygons:
        face.material_index=rng.choices((0,1,2),(7,2,1))[0]
    for loc,scale in [((.72,.24,.18),(.45,.45,.3)),((-.66,-.31,.16),(.4,.32,.27)),((.29,-.68,.1),(.31,.22,.2))]:
        ico('Broken outcrop',loc,scale,'stone',1)


def tree_pine():
    cylinder('Pine trunk',(0,0,.72),.14,1.44,'oak',10,.10)
    rng=random.Random(5512)
    for layer in range(5):
        bottom=.65+layer*.54
        radius=1.0-layer*.15
        verts=[]
        for ring in range(3):
            for i in range(16):
                angle=2*math.pi*i/16
                r=radius*((1 if i%2 else .86) if ring==0 else (.67 if ring==1 else .045))
                z=bottom+(rng.uniform(-.08,.08) if ring==0 else (.27 if ring==1 else .97))
                verts.append((r*math.cos(angle),r*math.sin(angle),z))
        faces=[tuple(range(15,-1,-1))]
        for ring in range(2):
            for i in range(16):
                j=(i+1)%16
                faces.append((ring*16+i,ring*16+j,(ring+1)*16+j,(ring+1)*16+i))
        faces.append(tuple(range(32,48)))
        obj=mesh('Layered needle branches',verts,faces,'pine')
        obj.data.materials.append(material('pine_light'))
        for face in obj.data.polygons:
            face.material_index=1 if rng.random()<.24 else 0


def tent():
    for side in (-1,1):
        verts=[(side*1.22,-1.15,0),(0,-1.15,1.68),(0,1.15,1.68),(side*1.22,1.15,0)]
        mesh('Heavy canvas slope',verts,[(0,1,2,3)],'cream')
        beam('Tent hem',(side*1.22,-1.15,.025),(side*1.22,1.15,.025),.035,'oak')
    prism('Canvas closed back',[(-1.22,0),(1.22,0),(0,1.68)],.025,'cream',0,1.15)
    for side in (-1,1):
        mesh('Open entrance drape',[(side*1.21,-1.18,0),(side*.40,-1.25,.10),(side*.24,-1.25,1.27),(0,-1.18,1.68)],
             [(0,1,2,3)],'teal')
        for y in (-1,1):
            beam('Guy rope',(side*.86,y*.93,.47),(side*1.48,y*1.37,.055),.014,'honey',6)
            beam('Wooden ground stake',(side*1.48,y*1.37,0),(side*1.43,y*1.37,.24),.028,'darkwood',6)
    beam('Ridge pole',(0,-1.35,1.71),(0,1.35,1.71),.045,'darkwood')
    for y in (-1.15,1.15):
        beam('Ridge support',(0,y,0),(0,y,1.76),.035,'darkwood')


def banner():
    cylinder('Banner pole',(-.54,0,1.10),.045,2.2,'oak',12)
    ico('Spear finial',(-.54,0,2.31),(.08,.055,.18),'brass',1)
    beam('Banner crossarm',(-.7,0,2.12),(.71,0,2.12),.038,'iron')
    for angle in (0,2*math.pi/3,4*math.pi/3):
        beam('Tripod foot',(-.54,0,.24),(-.54+.46*math.cos(angle),.46*math.sin(angle),.04),.065,'iron')
    verts=[]
    for row in range(7):
        for col in range(9):
            x=-.48+col*.135
            y=.09*math.sin(col*.85+row*.33)
            z=2.05-row*.19
            if row==6:
                z+=.24*(1-abs(col-4)/4)
            verts.append((x,y,z))
    faces=[(r*9+c,r*9+c+1,(r+1)*9+c+1,(r+1)*9+c) for r in range(6) for c in range(8)]
    mesh('Swallowtail woven banner',verts,faces,'teal')
    prism('Raised heraldic diamond',[(.05,1.36),(.27,1.61),(.05,1.91),(-.17,1.61)],.027,'brass',0,-.12)
    for x in (-.44,.60):
        torus('Banner hanging ring',(x,0,2.09),.054,.011,'iron',(math.pi/2,0,0))


def pillar():
    cylinder('Octagonal footing',(0,0,.13),.36,.26,'stone',8)
    cylinder('Lower capital',(0,0,.34),.30,.16,'limestone',12)
    cylinder('Column shaft',(0,0,1.5),.23,2.20,'stone',16,.215)
    for i in range(8):
        a=2*math.pi*i/8
        beam('Raised flute',(.222*math.cos(a),.222*math.sin(a),.5),(.208*math.cos(a),.208*math.sin(a),2.5),.027,'limestone',6)
    for z in (.49,2.53,2.69):
        cylinder('Molded collar',(0,0,z),.28,.12,'limestone',16)
    cylinder('Upper abacus',(0,0,2.88),.36,.23,'stone',8)


def torch():
    cylinder('Torch stave',(0,0,.43),.045,.86,'oak',10)
    for z in (.72,.82):
        torus('Iron binding',(0,0,z),.06,.012)
    cylinder('Brazier cup',(0,0,.88),.075,.17,'iron',12,.11)
    for i in range(6):
        a=i*math.pi/3
        beam('Basket tine',(.07*math.cos(a),.07*math.sin(a),.80),(.10*math.cos(a),.10*math.sin(a),1.01),.012,'iron',6)
    ico('Amber flame',(0,0,1.04),(.09,.08,.22),'ember',1)
    ico('Gold flame core',(.014,-.02,1.06),(.048,.047,.17),'flame',1)


def cauldron():
    lathe('Hollow iron pot',[(.10,.22),(.16,.40),(.38,.49),(.60,.46),(.69,.40),(.69,.35),(.60,.40),(.38,.42),(.18,.26)],'iron',24)
    torus('Rolled lip',(0,0,.69),.38,.03,'iron_edge')
    for x in (-.49,.49):
        torus('Cast side handle',(x,0,.49),.14,.029,'iron',(0,math.pi/2,0))
    for angle in (0,2*math.pi/3,4*math.pi/3):
        beam('Cauldron foot',(.25*math.cos(angle),.25*math.sin(angle),.17),(.34*math.cos(angle),.34*math.sin(angle),0),.055,'iron',8)
    cylinder('Potion surface',(0,0,.57),.36,.015,'purple',24)
    for i in range(3):
        ico('Potion bubble',(-.14+i*.14,.06,.59),(.04,.04,.026),'purple',2)


def fountain():
    lathe('Carved fountain base',[(0,.95),(.13,1.06),(.25,.98),(.32,.84)],'stone',24)
    lathe('Lower basin',[(.24,.66),(.37,1.06),(.59,1.13),(.69,1.08),(.69,.96),(.55,.91),(.37,.60)],'limestone',24)
    cylinder('Lower water',(0,0,.56),.93,.022,'water',24)
    lathe('Turned center pedestal',[(.35,.30),(.48,.32),(.61,.20),(1.04,.16),(1.12,.32)],'stone',20)
    lathe('Upper spill bowl',[(1.04,.20),(1.12,.45),(1.33,.52),(1.4,.49),(1.4,.42),(1.26,.36),(1.13,.19)],'limestone',20)
    cylinder('Upper water',(0,0,1.30),.39,.02,'water',24)
    cylinder('Center nozzle',(0,0,1.48),.075,.37,'brass',12,.04)
    ico('Finial water bead',(0,0,1.72),(.10,.10,.12),'water',2)
    for i in range(4):
        a=i*math.pi/2
        points=[(.43*math.cos(a),.43*math.sin(a),1.33),(.58*math.cos(a),.58*math.sin(a),1.12),(.66*math.cos(a),.66*math.sin(a),.63)]
        for j in range(2):
            beam('Sculpted water spill',points[j],points[j+1],.035,'water',8)


BUILDERS = {name: globals()[name] for name in ('chest','anvil','forge','guild_hall','alchemy_shop','market_stall','gate',
            'gravestone','rock_large','tree_pine','tent','banner','pillar','torch','cauldron','fountain')}



def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def original_record(repo, commit, name):
    path = 'Assets/Models/MMO/' + name + '.obj'
    data = subprocess.check_output(['git', '-C', str(repo), 'show', commit+':'+path])
    vertices = [list(map(float, line.split()[1:4])) for line in data.decode().splitlines() if line.startswith('v ')]
    return {'name': name, 'obj_path': path, 'mtl_path': path[:-4]+'.mtl',
            'source_before_sha256': hashlib.sha256(data).hexdigest(),
            'original_bounds': {'min': [min(v[i] for v in vertices) for i in range(3)],
                                'max': [max(v[i] for v in vertices) for i in range(3)]}}


def fit_parts(bounds):
    points = [obj.matrix_world @ vertex.co for obj in PARTS for vertex in obj.data.vertices]
    lo = Vector([min(v[i] for v in points) for i in range(3)])
    hi = Vector([max(v[i] for v in points) for i in range(3)])
    target_lo = Vector((bounds['min'][0], -bounds['max'][2], bounds['min'][1]))
    target_hi = Vector((bounds['max'][0], -bounds['min'][2], bounds['max'][1]))
    for obj in PARTS:
        transform = obj.matrix_world.copy()
        for vertex in obj.data.vertices:
            p = transform @ vertex.co
            vertex.co = Vector([target_lo[i] + (p[i]-lo[i])*(target_hi[i]-target_lo[i])/(hi[i]-lo[i]) for i in range(3)])
        obj.matrix_world.identity()


def export_asset(repo, record):
    bpy.ops.object.select_all(action='DESELECT')
    duplicates = []
    for source in PARTS:
        obj = source.copy()
        obj.data = source.data.copy()
        bpy.context.collection.objects.link(obj)
        obj.select_set(True)
        duplicates.append(obj)
    bpy.context.view_layer.objects.active = duplicates[0]
    bpy.ops.object.join()
    obj = bpy.context.object
    obj.name = record['name']
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.normals_make_consistent(inside=False)
    bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.008, scale_to_bounds=True)
    bpy.ops.object.mode_set(mode='OBJECT')
    path = repo / record['obj_path']
    bpy.ops.wm.obj_export(filepath=str(path), export_selected_objects=True,
                          export_triangulated_mesh=True, export_uv=True,
                          export_normals=True, export_materials=True,
                          forward_axis='NEGATIVE_Z', up_axis='Y', path_mode='STRIP')
    bpy.data.objects.remove(obj, do_unlink=True)
    record['triangle_count'] = sum(line.startswith('f ') for line in path.read_text().splitlines())
    record['obj_sha256'] = sha(path)
    record['mtl_sha256'] = sha(repo / record['mtl_path'])
    if record['triangle_count'] > 5000:
        raise RuntimeError(f"Triangle budget exceeded: {record['name']} {record['triangle_count']}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--repo', type=Path, required=True)
    parser.add_argument('--only', nargs='*')
    args = parser.parse_args(sys.argv[sys.argv.index('--')+1:])
    if bpy.app.version_string != '4.0.2':
        raise RuntimeError('Authoring is pinned to Blender 4.0.2')
    bpy.context.preferences.filepaths.save_version = 0
    repo = args.repo.resolve()
    art = repo/'Art/Blender/MMO'
    art.mkdir(parents=True, exist_ok=True)
    manifest_path = art/'provenance.json'
    previous = json.loads(manifest_path.read_text()) if manifest_path.exists() else {}
    commit = previous.get('source_commit') or subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip()
    bpy.ops.object.select_all(action='SELECT')
    bpy.ops.object.delete(use_global=False)
    records = []
    for index, name in enumerate(args.only or BUILDERS):
        PARTS.clear()
        BUILDERS[name]()
        record = original_record(repo, commit, name)
        fit_parts(record['original_bounds'])
        export_asset(repo, record)
        collection = bpy.data.collections.new(name)
        bpy.context.scene.collection.children.link(collection)
        for obj in PARTS:
            for old in list(obj.users_collection):
                old.objects.unlink(obj)
            collection.objects.link(obj)
            obj.location.x += (index%4)*3
            obj.location.y += (index//4)*3
        collection['runtime_bounds_json'] = json.dumps(record['original_bounds'])
        records.append(record)
        print('AUTHORED', name, record['triangle_count'], flush=True)
    blend = art/'props.blend'
    bpy.ops.wm.save_as_mainfile(filepath=str(blend), compress=True)
    license_path = repo/'LICENSE'
    manifest = {'schema_version': 1, 'blender_version': bpy.app.version_string,
                'source_commit': commit, 'author_script': {'path': 'tools/blender/author_mmo_props.py', 'sha256': sha(__file__)},
                'blend': {'path': str(blend.relative_to(repo)), 'sha256': sha(blend)},
                'license': {'name': 'Spark Open License 1.0', 'path': str(license_path.relative_to(repo)), 'sha256': sha(license_path)},
                'assets': records}
    manifest_path.write_text(json.dumps(manifest, indent=2)+'\n')


if __name__ == '__main__':
    main()
