"""Render review images of a built mannequin.blend (visual evidence only).

    Blender --background <out>/mannequin.blend --python review_mannequin.py -- \\
        --out <dir> [--clip Walk_Fwd --frames 8 --view side] [--turntable]

A clip strip carries the armature forward at the clip's recorded speed, so
planted feet must stay still against the ground grid; a skating foot shows
as a smear across the strip's frames.
"""

import argparse
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector


def _arguments():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--clip", default="")
    parser.add_argument("--frames", type=int, default=8)
    parser.add_argument("--view", default="side", choices=["side", "front", "threequarter"])
    parser.add_argument("--turntable", action="store_true")
    parser.add_argument("--size", type=int, default=520)
    parser.add_argument("--tag", default="")
    return parser.parse_args(argv)


def _clay(obj):
    material = bpy.data.materials.get("ReviewClay") or bpy.data.materials.new("ReviewClay")
    material.use_nodes = True
    shader = material.node_tree.nodes["Principled BSDF"]
    shader.inputs["Base Color"].default_value = (0.36, 0.37, 0.39, 1.0)
    shader.inputs["Roughness"].default_value = 0.5
    if not obj.data.materials:
        obj.data.materials.append(material)


def _stage(scene):
    world = bpy.data.worlds.new("ReviewWorld")
    scene.world = world
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.32, 0.33, 0.35, 1.0)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.6
    for name, location, energy in (("Key", (2.5, 3.5, 3.5), 700.0), ("Fill", (-3.0, 1.5, 2.0), 250.0),
                                   ("Rim", (0.5, -3.5, 3.0), 500.0)):
        light = bpy.data.lights.new(name, "AREA")
        light.energy = energy
        light.size = 2.5
        obj = bpy.data.objects.new(name, light)
        obj.location = location
        obj.rotation_euler = (Vector((0.0, 0.0, 1.0)) - Vector(location)).to_track_quat("-Z", "Y").to_euler()
        scene.collection.objects.link(obj)
    # Ground grid: 0.25 m tiles make foot slide visible.
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=64, y_subdivisions=64, size=16.0)
    grid = bpy.context.active_object
    grid.name = "ReviewGround"
    material = bpy.data.materials.new("ReviewGround")
    material.use_nodes = True
    nodes = material.node_tree.nodes
    shader = nodes["Principled BSDF"]
    checker = nodes.new("ShaderNodeTexChecker")
    checker.inputs["Scale"].default_value = 32.0
    checker.inputs["Color1"].default_value = (0.20, 0.21, 0.22, 1.0)
    checker.inputs["Color2"].default_value = (0.26, 0.27, 0.28, 1.0)
    material.node_tree.links.new(checker.outputs["Color"], shader.inputs["Base Color"])
    grid.data.materials.append(material)


def _camera(scene, view, target, distance=4.2, height=2.2, scale=2.3):
    data = bpy.data.cameras.new("ReviewCamera")
    data.type = "ORTHO"
    data.ortho_scale = scale
    camera = bpy.data.objects.new("ReviewCamera", data)
    scene.collection.objects.link(camera)
    scene.camera = camera
    offsets = {"side": Vector((distance, 0.0, 0.0)), "front": Vector((0.0, distance, 0.0)),
               "threequarter": Vector((distance * 0.7, distance * 0.7, 0.4))}
    camera.location = Vector(target) + offsets[view] + Vector((0.0, 0.0, height - target[2]))
    camera.rotation_euler = (Vector(target) - camera.location).to_track_quat("-Z", "Y").to_euler()
    return camera


def _render(scene, path, size):
    scene.render.engine = "BLENDER_EEVEE"
    scene.render.resolution_x = size
    scene.render.resolution_y = int(size * 1.25)
    scene.render.image_settings.file_format = "PNG"
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)


def main():
    args = _arguments()
    args.out.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    armature = next(o for o in scene.objects if o.type == "ARMATURE")
    mesh = next(o for o in scene.objects if o.type == "MESH")
    _clay(mesh)
    _stage(scene)
    tag = args.tag or args.clip or "rest"
    if args.clip:
        action = bpy.data.actions[args.clip]
        armature.animation_data_create()
        armature.animation_data.action = action
        if action.slots:
            armature.animation_data.action_slot = action.slots[0]
        speed = float(action.get("vkr_speed", 0.0))
        travel = Vector((*action.get("vkr_travel", (0.0, 1.0)), 0.0))
        start, end = action.frame_range
        camera = _camera(scene, args.view, (0.0, 0.0, 0.95))
        base = camera.location.copy()
        for i in range(args.frames):
            frame = start + (end - start) * i / args.frames
            scene.frame_set(int(math.floor(frame)), subframe=frame - math.floor(frame))
            # Carry the character forward at its clip speed with the camera
            # following, so the ground and any planted foot move together.
            seconds = (frame - start) / scene.render.fps
            armature.location = travel * (speed * seconds)
            camera.location = base + travel * (speed * seconds)
            _render(scene, args.out / f"{tag}_{args.view}_{i:02d}.png", args.size)
    elif args.turntable:
        _camera(scene, "front", (0.0, 0.0, 0.95))
        camera = scene.camera
        for i, angle in enumerate((0, 45, 90, 135, 180, 270)):
            radians = math.radians(angle)
            camera.location = (4.2 * math.sin(radians), 4.2 * math.cos(radians), 1.0)
            camera.rotation_euler = (Vector((0.0, 0.0, 0.95)) - camera.location).to_track_quat("-Z", "Y").to_euler()
            _render(scene, args.out / f"{tag}_turn_{angle:03d}.png", args.size)


main()
