"""Studio previews of exported models: the glTF imported into an empty
scene (so the check covers the export) and rendered in Cycles under an
interior studio light and a key light, from the front-left and the
back-right, side by side in one PNG."""
import glob
import math
import os

import bpy
import numpy as np
from mathutils import Vector

from .bake import pixels, setup_cycles


def _hdri():
    """Blender's bundled interior studio light."""
    base = os.path.dirname(bpy.app.binary_path)
    for root in (os.path.join(base, "..", "Resources"), base):
        for path in sorted(glob.glob(os.path.join(root, "*", "datafiles", "studiolights", "world", "interior.exr"))):
            return path
    return None


def render_preview(gltf_path, path, size=(900, 900)):
    """Imports the exported glTF into an empty scene (so the check covers the
    export) and renders it in Cycles under an interior HDRI with a key
    light, from the front-left and the back-right; one image, two views."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=gltf_path)
    objs = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    scene = bpy.context.scene
    setup_cycles(64)
    scene.cycles.samples = 64
    scene.cycles.use_denoising = True
    scene.render.resolution_x, scene.render.resolution_y = size
    scene.view_settings.view_transform = "AgX"
    scene.view_settings.look = "AgX - Medium High Contrast"
    nt = scene.world.node_tree
    hdri = _hdri()
    if hdri:
        env = nt.nodes.new("ShaderNodeTexEnvironment")
        env.image = bpy.data.images.load(hdri)
        nt.links.new(env.outputs["Color"], nt.nodes["Background"].inputs["Color"])
    nt.nodes["Background"].inputs["Strength"].default_value = 0.8
    lo = Vector((1e9, 1e9, 1e9))
    hi = -lo
    for o in objs:
        for c in o.bound_box:
            w = o.matrix_world @ Vector(c)
            lo = Vector((min(lo.x, w.x), min(lo.y, w.y), min(lo.z, w.z)))
            hi = Vector((max(hi.x, w.x), max(hi.y, w.y), max(hi.z, w.z)))
    centre = (lo + hi) / 2
    radius = max((hi - lo).length / 2, 0.05)
    bpy.ops.mesh.primitive_plane_add(size=radius * 40, location=(0, 0, lo.z))
    floor = bpy.context.active_object
    fm = bpy.data.materials.new("floor")
    fm.use_nodes = True
    fb = fm.node_tree.nodes["Principled BSDF"]
    fb.inputs["Base Color"].default_value = (0.11, 0.11, 0.115, 1)
    fb.inputs["Roughness"].default_value = 0.55
    floor.data.materials.append(fm)
    key = bpy.data.lights.new("key", "AREA")
    key.energy = 260.0 * radius * radius
    key.size = radius * 1.2
    key_obj = bpy.data.objects.new("key", key)
    key_obj.location = centre + Vector((-1.4, -1.8, 2.2)) * radius * 1.6
    key_obj.rotation_euler = (centre - key_obj.location).to_track_quat("-Z", "Y").to_euler()
    scene.collection.objects.link(key_obj)
    cam_data = bpy.data.cameras.new("cam")
    cam_data.lens = 50
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    scene.camera = cam
    half_fov = math.atan(cam_data.sensor_width / 2 / cam_data.lens)
    dist = radius / math.sin(half_fov) * 1.02
    frames = []
    for k, (angle, elevation) in enumerate(((-35.0, 22.0), (145.0, 18.0))):
        a, e = math.radians(-90 + angle), math.radians(elevation)
        cam.location = centre + Vector((math.cos(a) * math.cos(e), math.sin(a) * math.cos(e), math.sin(e))) * dist
        cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
        tmp = path + f".{k}.png"
        scene.render.filepath = tmp
        bpy.ops.render.render(write_still=True)
        img = bpy.data.images.load(tmp)
        frames.append(pixels(img).copy())
        bpy.data.images.remove(img)
        os.remove(tmp)
    sheet = np.concatenate(frames, axis=1)
    h, w = sheet.shape[:2]
    out = bpy.data.images.new("sheet", w, h, alpha=False)
    out.pixels.foreach_set(sheet.ravel())
    out.filepath_raw = path
    out.file_format = "PNG"
    out.save()
