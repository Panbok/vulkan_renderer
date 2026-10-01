"""Blender objects for the mannequin: the skinned mesh, its armature, UV
layout, bone-heat skin weights and glTF export. Runs inside Blender; the
surface and joints come from the numpy modules."""

from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Vector

import mannequin_base as mb


GENERATOR = "vkr.mannequin.v2"
MAX_INFLUENCES = 4

# Bones whose roll aligns their local Z to the character's forward (+Y);
# fingers and hands align it to their back-of-hand direction instead.
_HAND_PREFIXES = ("hand_", "thumb_", "index_", "middle_", "ring_", "pinky_")


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = 60
    scene.unit_settings.system = "METRIC"
    return scene


# =============================================================================
# Mesh, skin and armature
# =============================================================================

def create_mesh(body, name="SK_Mannequin"):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(body.positions.tolist(), [], [list(f) for f in body.faces])
    mesh.update()
    uv_layer = mesh.uv_layers.new(name="UVMap")
    flat = np.zeros((len(mesh.loops), 2))
    for polygon, uvs in zip(mesh.polygons, body.face_uvs):
        for offset, loop in enumerate(polygon.loop_indices):
            flat[loop] = uvs[offset]
    uv_layer.data.foreach_set("uv", flat.ravel())
    for polygon in mesh.polygons:
        polygon.use_smooth = True
    # Region weights ride along subdivision for the shell design.
    for region, weight in body.regions.items():
        attribute = mesh.attributes.new("region_" + region, "FLOAT", "POINT")
        attribute.data.foreach_set("value", weight.astype(np.float32))
    # The helmet is the focal point: twice the texel density of the body.
    texel = mesh.attributes.new("texel_scale", "FLOAT", "FACE")
    texel.data.foreach_set("value", np.where(np.asarray(body.face_parts) == mb.PART_HEAD, 2.0, 1.0)
                           .astype(np.float32))
    obj = bpy.data.objects.new(name, mesh)
    obj["vkr_generator"] = GENERATOR
    bpy.context.scene.collection.objects.link(obj)
    # The part patches keep a consistent outward winding with the body.
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()
    return obj


def skin(mesh_obj, armature_obj, rig, body):
    """Bone-heat weights, then rules: the helmet rides the head, each boot
    splits between foot and ball, at most MAX_INFLUENCES per vertex."""
    bpy.ops.object.select_all(action="DESELECT")
    mesh_obj.select_set(True)
    armature_obj.select_set(True)
    bpy.context.view_layer.objects.active = armature_obj
    bpy.ops.object.parent_set(type="ARMATURE_AUTO")
    names = rig.deform_names()
    column = {name: i for i, name in enumerate(names)}
    count = len(mesh_obj.data.vertices)
    dense = np.zeros((count, len(names)))
    group_bone = {group.index: group.name for group in mesh_obj.vertex_groups}
    for vertex in mesh_obj.data.vertices:
        for element in vertex.groups:
            name = group_bone[element.group]
            if name in column:
                dense[vertex.index, column[name]] = element.weight
    positions = body.positions
    vertex_part = np.zeros(count, dtype=np.int64)
    for face, part in zip(body.faces, body.face_parts):
        vertex_part[list(face)] = np.maximum(vertex_part[list(face)], part)

    # The helmet, down to its underside along the jaw line, is rigid on the
    # head; the neck bends below it.
    rim = (positions - mb.RIM_POINT) @ mb.RIM_NORMAL
    helmet = np.clip((rim + 0.010) / 0.008, 0.0, 1.0)
    helmet = helmet * helmet * (3.0 - 2.0 * helmet)
    head_row = np.zeros(len(names))
    head_row[column["head"]] = 1.0
    dense = dense * (1.0 - helmet[:, None]) + head_row[None] * helmet[:, None]
    # Boots: foot behind the ball line, ball in front of it.
    for side in ("l", "r"):
        ball = rig.bones[f"ball_{side}"].head
        foot = rig.bones[f"foot_{side}"].head
        axis = ball - foot
        axis[2] = 0.0
        axis /= np.linalg.norm(axis)
        sign = -1.0 if side == "l" else 1.0
        boot = (vertex_part == mb.PART_BOOT) & (positions[:, 0] * sign > 0.0)
        low = boot & (positions[:, 2] < foot[2] + 0.02)
        share = np.clip(((positions - ball) @ axis + 0.012) / 0.024, 0.0, 1.0)
        share = share * share * (3.0 - 2.0 * share)
        row = np.zeros((count, len(names)))
        row[:, column[f"ball_{side}"]] = share
        row[:, column[f"foot_{side}"]] = 1.0 - share
        dense[low] = row[low]

    order = np.argsort(-dense, axis=1)[:, :MAX_INFLUENCES]
    top = np.take_along_axis(dense, order, axis=1)
    total = top.sum(axis=1, keepdims=True)
    missing = np.flatnonzero(total[:, 0] <= 0.0)
    if len(missing):
        raise ValueError(f"{len(missing)} vertices without skin weights, e.g. {positions[missing[:3]]}")
    top /= total
    for group in list(mesh_obj.vertex_groups):
        mesh_obj.vertex_groups.remove(group)
    groups = [mesh_obj.vertex_groups.new(name=name) for name in names]
    for vertex in range(count):
        for slot in range(MAX_INFLUENCES):
            weight = float(top[vertex, slot])
            if weight > 0.0:
                groups[order[vertex, slot]].add([vertex], weight, "REPLACE")
    return names, order, top


def textured_material(mesh_obj, textures, name="MI_Mannequin"):
    """One PBR material from the baked textures: base colour, normal, and
    occlusion-roughness-metallic packed as the glTF exporter expects."""
    material = bpy.data.materials.new(name)
    material["vkr_generator"] = GENERATOR
    # A closed surface: exported single sided.
    material.use_backface_culling = True
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    shader = nodes["Principled BSDF"]

    def image_node(path, data):
        node = nodes.new("ShaderNodeTexImage")
        node.image = bpy.data.images.load(str(Path(path).resolve()))
        node.image.colorspace_settings.name = "Non-Color" if data else "sRGB"
        return node

    base = image_node(textures["base"], False)
    links.new(base.outputs["Color"], shader.inputs["Base Color"])
    orm = image_node(textures["orm"], True)
    split = nodes.new("ShaderNodeSeparateColor")
    links.new(orm.outputs["Color"], split.inputs["Color"])
    links.new(split.outputs["Green"], shader.inputs["Roughness"])
    links.new(split.outputs["Blue"], shader.inputs["Metallic"])
    normal = image_node(textures["normal"], True)
    normal_map = nodes.new("ShaderNodeNormalMap")
    links.new(normal.outputs["Color"], normal_map.inputs["Color"])
    links.new(normal_map.outputs["Normal"], shader.inputs["Normal"])
    # The exporter takes occlusion from a group named glTF Material Output.
    group = bpy.data.node_groups.get("glTF Material Output")
    if group is None:
        group = bpy.data.node_groups.new("glTF Material Output", "ShaderNodeTree")
        group.interface.new_socket("Occlusion", in_out="INPUT", socket_type="NodeSocketFloat")
    settings = nodes.new("ShaderNodeGroup")
    settings.node_tree = group
    links.new(split.outputs["Red"], settings.inputs["Occlusion"])
    mesh_obj.data.materials.clear()
    mesh_obj.data.materials.append(material)
    return material


def _roll_reference(name):
    if name.startswith(_HAND_PREFIXES):
        return Vector((0.0, 0.0, 1.0))
    return Vector((0.0, 1.0, 0.0))


def create_armature(rig, name="Mannequin"):
    armature = bpy.data.armatures.new(name)
    obj = bpy.data.objects.new(name, armature)
    obj["vkr_generator"] = GENERATOR
    bpy.context.scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    edit = armature.edit_bones
    for bone_name in rig.order:
        bone = rig.bones[bone_name]
        eb = edit.new(bone_name)
        eb.head = Vector(bone.head)
        tail = Vector(bone.tail)
        if (tail - eb.head).length < 1e-4:
            tail = eb.head + Vector((0.0, 0.0, 0.05))
        eb.tail = tail
        eb.use_deform = bone.deform
        if bone.parent:
            eb.parent = edit[bone.parent]
        eb.use_connect = False
        eb.align_roll(_roll_reference(bone_name))
    bpy.ops.object.mode_set(mode="OBJECT")
    return obj


def subdivide(mesh_obj, levels=1):
    """Apply Catmull-Clark subdivision ahead of the armature; vertex groups
    and UVs interpolate."""
    bpy.context.view_layer.objects.active = mesh_obj
    modifier = mesh_obj.modifiers.new("Subdivision", "SUBSURF")
    modifier.levels = levels
    modifier.render_levels = levels
    modifier.uv_smooth = "PRESERVE_BOUNDARIES"
    bpy.ops.object.modifier_move_to_index(modifier=modifier.name, index=0)
    bpy.ops.object.modifier_apply(modifier=modifier.name)


def pack_uvs(mesh_obj, margin=0.003):
    """Equalize texel density across islands, raise it where the mesh's
    `texel_scale` face attribute asks (whole islands only), and pack the
    islands into [0, 1]."""
    bpy.context.view_layer.objects.active = mesh_obj
    mesh_obj.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.select_all(action="SELECT")
    bpy.ops.uv.average_islands_scale()
    bpy.ops.object.mode_set(mode="OBJECT")
    mesh = mesh_obj.data
    scale = mesh.attributes.get("texel_scale")
    if scale is not None:
        face_scale = np.zeros(len(mesh.polygons), dtype=np.float32)
        scale.data.foreach_get("value", face_scale)
        totals = np.zeros(len(mesh.polygons), dtype=np.int64)
        mesh.polygons.foreach_get("loop_total", totals)
        loop_scale = np.repeat(face_scale, totals)
        uv = np.zeros(len(mesh.loops) * 2, dtype=np.float32)
        mesh.uv_layers.active.data.foreach_get("uv", uv)
        uv = uv.reshape(-1, 2) * loop_scale[:, None]
        mesh.uv_layers.active.data.foreach_set("uv", uv.ravel())
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.select_all(action="SELECT")
    bpy.ops.uv.pack_islands(rotate=True, margin=margin)
    bpy.ops.object.mode_set(mode="OBJECT")


def export_gltf(path):
    """glTF with separate buffers and PNG textures under textures/: the mesh
    cooker reads image files, not images embedded in a GLB."""
    bpy.ops.export_scene.gltf(
        filepath=str(path),
        export_format="GLTF_SEPARATE",
        export_texture_dir="textures",
        export_image_format="AUTO",
        export_yup=True,
        export_texcoords=True,
        export_normals=True,
        # The cooker derives tangents after deduplication; exported ones would
        # only add bytes (and Blender's differ in the last digit run to run).
        export_tangents=False,
        export_materials="EXPORT",
        export_skins=True,
        export_influence_nb=4,
        export_all_influences=False,
        export_def_bones=False,
        export_animations=True,
        export_animation_mode="ACTIONS",
        export_force_sampling=True,
        export_frame_step=1,
        export_optimize_animation_size=True,
        export_anim_slide_to_zero=True,
        export_reset_pose_bones=True,
        export_rest_position_armature=True,
        export_extras=False,
        export_lights=False,
        export_cameras=False,
    )
