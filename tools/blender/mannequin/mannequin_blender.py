"""Blender objects for the mannequin: the skinned mesh, its armature, UV
layout and glTF export. Runs inside Blender; geometry and weights come from
the numpy modules."""

import bmesh
import bpy
import numpy as np
from mathutils import Vector

import mannequin_body as mb


GENERATOR = "vkr.mannequin.v1"

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
# UV layout
# =============================================================================

def face_corner_uvs(body):
    """UV per face corner. MakeHuman faces keep their atlas coordinates; the
    new parts' islands are in metres of surface. Packing rescales both."""
    return body.face_uvs


# =============================================================================
# Mesh and armature
# =============================================================================

def create_mesh(body, weights, name="SK_Mannequin"):
    names, bone_index, bone_weight = weights
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(body.positions.tolist(), [], [list(f) for f in body.faces])
    mesh.update()
    uv_layer = mesh.uv_layers.new(name="UVMap")
    corner_uvs = face_corner_uvs(body)
    flat = np.zeros((len(mesh.loops), 2))
    for polygon, uvs in zip(mesh.polygons, corner_uvs):
        for offset, loop in enumerate(polygon.loop_indices):
            flat[loop] = uvs[offset]
    uv_layer.data.foreach_set("uv", flat.ravel())
    for polygon in mesh.polygons:
        polygon.use_smooth = True

    obj = bpy.data.objects.new(name, mesh)
    obj["vkr_generator"] = GENERATOR
    bpy.context.scene.collection.objects.link(obj)
    groups = [obj.vertex_groups.new(name=bone) for bone in names]
    for vertex in range(len(body.positions)):
        for slot in range(bone_index.shape[1]):
            weight = float(bone_weight[vertex, slot])
            if weight > 0.0:
                groups[bone_index[vertex, slot]].add([vertex], weight, "REPLACE")
    # Closed surfaces from the part patches keep a consistent outward winding.
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    bm.to_mesh(mesh)
    bm.free()
    return obj


# Part materials: linear base colour, roughness, clear coat weight and its
# roughness. The shell is a satin plastic; the boots a graphite rubber.
MATERIALS = (
    ("MI_Mannequin_Shell", (0.42, 0.43, 0.45), 0.42, 0.35, 0.18),
    ("MI_Mannequin_Visor", (0.36, 0.37, 0.39), 0.30, 0.55, 0.10),
    ("MI_Mannequin_Boot", (0.035, 0.036, 0.040), 0.62, 0.0, 0.5),
)


def assign_materials(mesh_obj, face_parts):
    """One material per part label (body, head, boot), in MATERIALS order."""
    mesh = mesh_obj.data
    for name, color, roughness, coat, coat_roughness in MATERIALS:
        material = bpy.data.materials.new(name)
        material["vkr_generator"] = GENERATOR
        material.use_nodes = True
        shader = material.node_tree.nodes["Principled BSDF"]
        shader.inputs["Base Color"].default_value = (*color, 1.0)
        shader.inputs["Roughness"].default_value = roughness
        shader.inputs["Metallic"].default_value = 0.0
        shader.inputs["Coat Weight"].default_value = coat
        shader.inputs["Coat Roughness"].default_value = coat_roughness
        mesh.materials.append(material)
    indices = np.asarray(face_parts, dtype=np.int32)
    mesh.polygons.foreach_set("material_index", indices)
    mesh.update()


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


def bind(mesh_obj, armature_obj):
    mesh_obj.parent = armature_obj
    modifier = mesh_obj.modifiers.new("Armature", "ARMATURE")
    modifier.object = armature_obj


def subdivide(mesh_obj, levels=1):
    """Apply Catmull-Clark subdivision; vertex groups and UVs interpolate."""
    bpy.context.view_layer.objects.active = mesh_obj
    modifier = mesh_obj.modifiers.new("Subdivision", "SUBSURF")
    modifier.levels = levels
    modifier.render_levels = levels
    modifier.uv_smooth = "PRESERVE_BOUNDARIES"
    bpy.ops.object.modifier_apply(modifier=modifier.name)


def pack_uvs(mesh_obj, margin=0.003):
    """Equalize texel density across islands and pack them into [0, 1]."""
    bpy.context.view_layer.objects.active = mesh_obj
    mesh_obj.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.select_all(action="SELECT")
    bpy.ops.uv.average_islands_scale()
    bpy.ops.uv.pack_islands(rotate=True, margin=margin)
    bpy.ops.object.mode_set(mode="OBJECT")


def export_glb(path):
    bpy.ops.export_scene.gltf(
        filepath=str(path),
        export_format="GLB",
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
