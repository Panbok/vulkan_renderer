"""Checks the VKR Asset Kit bake end to end in Blender.

    Blender -b --factory-startup --python tools/blender/vkr_asset_kit/check_asset_kit.py

A 0.4 m plastic box with a box hidden inside it, a round knob on top, a
screen on its front, and a steel screw and a status light stamped on its
front bakes at 256 px/m. The check fails when: the hidden box survives (its
faces would waste atlas space); the export lacks MikkTSpace tangents (an
n-gon cap blocks them), has a second UV set or vertex colours, or does not
keep the screen's material apart from the baked one; an inside-out part
stays inside out (it would vanish); the screw does not land on the front face as
metal; the light does not write an emissive texture; or a model too large
for one atlas at the minimum density stays in one texture set. Through the
add-on (registered, as the panel runs it), Bake & Export of a box with an
Add Detail screw must leave the scene's object as it was, write into the
panel's project folder, name that project's detail maps in the material's
extras and stamp the screw where the empty stands.
"""
import json
import os
import shutil
import sys
import tempfile

import bmesh
import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import vkr_asset_kit  # noqa: E402
from vkr_asset_kit import bake  # noqa: E402


def paint(name, colour, metal=0.0, rough=0.5):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*colour, 1.0)
    bsdf.inputs["Metallic"].default_value = metal
    bsdf.inputs["Roughness"].default_value = rough
    return mat


def box(bm, size, centre):
    geom = bmesh.ops.create_cube(bm, size=1.0)
    verts = geom["verts"]
    bmesh.ops.scale(bm, vec=Vector(size), verts=verts)
    bmesh.ops.translate(bm, vec=Vector(centre), verts=verts)
    return verts


def test_asset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bm = bmesh.new()
    box(bm, (0.4, 0.4, 0.4), (0.0, 0.0, 0.2))
    first_inner = len(bm.faces)
    box(bm, (0.2, 0.2, 0.2), (0.0, 0.0, 0.2))
    bm.faces.ensure_lookup_table()
    for f in bm.faces[first_inner:]:
        f.material_index = 1
    # A knob with n-gon caps (export computes tangents only for triangles
    # and quads), standing on the box so its bottom cap hides.
    first_knob = len(bm.faces)
    knob = bmesh.ops.create_cone(bm, cap_ends=True, segments=24, radius1=0.05, radius2=0.05, depth=0.04)
    bmesh.ops.translate(bm, vec=Vector((0.0, 0.0, 0.42)), verts=knob["verts"])
    # A screen proud of the front face, kept with its own material.
    first_screen = len(bm.faces)
    box(bm, (0.2, 0.002, 0.12), (0.0, -0.201, 0.25))
    bm.faces.ensure_lookup_table()
    for f in bm.faces[first_screen:]:
        f.material_index = 2
    mesh = bpy.data.meshes.new("kit_box")
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(paint("kit_paint", (0.1, 0.2, 0.5)))
    mesh.materials.append(paint("kit_inner", (0.9, 0.1, 0.1)))
    mesh.materials.append(paint("display", (0.0, 0.0, 0.0)))
    mesh.materials[0]["vkr_smart"] = "plastic"
    obj = bpy.data.objects.new("kit_box", mesh)
    bpy.context.scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project()
    bpy.ops.object.mode_set(mode="OBJECT")
    details = [{"kind": "screw", "center": (0.12, -0.2, 0.12), "normal": (0, -1, 0), "radius": 0.02,
                "finish": "steel"},
               {"kind": "glow", "center": (0.1, -0.2, 0.33), "normal": (0, -1, 0), "width": 0.02,
                "colour": (0.2, 1.0, 0.4)}]
    out = tempfile.mkdtemp(prefix="vkr-asset-kit-")
    settings = bake.Settings(density=256.0, res_min=128, res_max=512, samples=4, keep=("display",))
    report = bake.bake_asset(obj, "kit_box", out, settings, details=details)

    # The inner box, the outer box's floor face and the knob's bottom cap
    # are gone; the hidden material takes no atlas space.
    assert report["hidden_faces"] >= 8, report
    assert [t["slots"] for t in report["sets"]] == [["kit_paint"]], report

    gltf_path = report["path"]
    gltf = json.load(open(gltf_path))
    assert [m["name"] for m in gltf["materials"]] == ["kit_box", "display"], gltf["materials"]
    prims = [p for m in gltf["meshes"] for p in m["primitives"]]
    assert sorted(p["material"] for p in prims) == [0, 1], prims
    for p in prims:
        attributes = p["attributes"]
        assert "TANGENT" in attributes and "TEXCOORD_0" in attributes, attributes
        assert "TEXCOORD_1" not in attributes and "COLOR_0" not in attributes, attributes
    material = gltf["materials"][0]
    mr = material["pbrMetallicRoughness"]["metallicRoughnessTexture"]["index"]
    assert material["occlusionTexture"]["index"] == mr, "occlusion must read the ORM's red channel"
    assert "emissiveTexture" in material, "the status light writes an emissive texture"

    # The screw: metal where the front face's centre bakes, paint around it.
    tex_dir = os.path.join(os.path.dirname(gltf_path), "textures")
    orm = bpy.data.images.load(os.path.join(tex_dir, "kit_box_orm.png"))
    px = bake.pixels(orm)
    metal = px[..., 2]
    metal_texels = int((metal > 0.8).sum())
    density = report["sets"][0]["density"]
    expected = np.pi * 0.02 ** 2 * density ** 2
    assert 0.4 * expected < metal_texels < 2.5 * expected, (metal_texels, expected)

    # Each texture set packs its own faces into its whole atlas: a set that
    # shares the pack with the others reaches a fraction of its density.
    split_settings = bake.Settings(density=256.0, res_min=128, res_max=256, min_group_density=200.0,
                                   samples=4, keep=("display",))
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bm = bmesh.new()
    box(bm, (1.0, 1.0, 1.0), (0.0, 0.0, 0.5))
    first_small = len(bm.faces)
    box(bm, (0.2, 0.2, 0.2), (0.0, 0.0, 1.1))
    bm.faces.ensure_lookup_table()
    for f in bm.faces[first_small:]:
        f.material_index = 1
    mesh = bpy.data.meshes.new("kit_split")
    bm.to_mesh(mesh)
    bm.free()
    mesh.materials.append(paint("kit_big", (0.5, 0.5, 0.5)))
    mesh.materials.append(paint("kit_small", (0.9, 0.5, 0.1)))
    split = bpy.data.objects.new("kit_split", mesh)
    bpy.context.scene.collection.objects.link(split)
    bpy.context.view_layer.objects.active = split
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project()
    bpy.ops.object.mode_set(mode="OBJECT")
    split_report = bake.bake_asset(split, "kit_split", out, split_settings)
    sets = split_report["sets"]
    assert len(sets) == 2, sets
    small = next(s for s in sets if s["slots"] == ["kit_small"])
    big = next(s for s in sets if s["slots"] == ["kit_big"])
    assert small["density"] > 2.0 * big["density"], sets

    # Too much surface for one atlas at the minimum density splits.
    big = {0: 30.0, 1: 4.0, 2: 3.0}
    groups = bake.plan_groups(big, bake.Settings())
    assert len(groups) >= 2 and sorted(s for g in groups for s in g) == [0, 1, 2], groups
    small = bake.plan_groups({0: 1.0, 1: 0.5}, bake.Settings())
    assert small == [[0, 1]], small
    shutil.rmtree(out)


def test_orient():
    """An inside-out closed part turns outward; an open card keeps its
    winding; a correct part stays."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bm = bmesh.new()
    box(bm, (0.2, 0.2, 0.2), (0.0, 0.0, 0.1))
    first_inverted = len(bm.faces)
    box(bm, (0.2, 0.2, 0.2), (0.5, 0.0, 0.1))
    bm.faces.ensure_lookup_table()
    inverted = bm.faces[first_inverted:]
    bmesh.ops.reverse_faces(bm, faces=inverted)
    card = bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=0.1)
    bmesh.ops.translate(bm, vec=Vector((1.0, 0.0, 0.1)), verts=card["verts"])
    bmesh.ops.reverse_faces(bm, faces=list({f for v in card["verts"] for f in v.link_faces}))
    mesh = bpy.data.meshes.new("kit_orient")
    bm.to_mesh(mesh)
    bm.free()
    obj = bpy.data.objects.new("kit_orient", mesh)
    bpy.context.scene.collection.objects.link(obj)
    assert bake.orient_outward(obj) == 1
    for poly in mesh.polygons:
        centre = Vector(poly.center)
        if centre.x > 0.9:
            assert poly.normal.z < -0.99, "the open card keeps its winding"
        else:
            middle = Vector((0.5 if centre.x > 0.25 else 0.0, 0.0, 0.1))
            assert (centre - middle).dot(poly.normal) > 0.0, "every box face points out"


def test_addon():
    """The panel's path: settings from the scene, a detail from an empty."""
    bpy.ops.wm.read_factory_settings(use_empty=True)
    vkr_asset_kit.register()
    try:
        bm = bmesh.new()
        box(bm, (0.4, 0.4, 0.4), (0.0, 0.0, 0.2))
        mesh = bpy.data.meshes.new("kit_panel")
        bm.to_mesh(mesh)
        bm.free()
        mesh.materials.append(paint("plastic_grey", (0.4, 0.4, 0.4)))
        obj = bpy.data.objects.new("kit_panel", mesh)
        bpy.context.scene.collection.objects.link(obj)
        bpy.context.view_layer.objects.active = obj
        obj.select_set(True)
        vertices = len(mesh.vertices)

        # A screw empty on the front face's centre, Z out of the face.
        cursor = bpy.context.scene.cursor
        cursor.location = (0.0, -0.2, 0.2)
        cursor.rotation_mode = "XYZ"
        cursor.rotation_euler = (np.pi / 2.0, 0.0, 0.0)
        settings = bpy.context.scene.vkr_asset_kit
        settings.detail_kind = "screw"
        assert bpy.ops.vkr.add_detail() == {"FINISHED"}
        empty = bpy.context.active_object
        empty.empty_display_size = 0.02
        bpy.context.view_layer.objects.active = obj

        project = tempfile.mkdtemp(prefix="vkr-asset-kit-project-")
        settings.project = project
        settings.folder = "assets/models/check"
        settings.density = 256.0
        settings.max_resolution = "1024"
        settings.samples = 4
        objects = len(bpy.data.objects)
        report = vkr_asset_kit.bake_object(bpy.context, obj)

        assert len(bpy.data.objects) == objects, "the bake's working copy stays behind"
        assert len(obj.data.vertices) == vertices, "the bake changed the scene's mesh"
        gltf_path = os.path.join(project, "assets/models/check/kit_panel/kit_panel.gltf")
        assert os.path.samefile(report["path"], gltf_path), report["path"]
        extras = json.load(open(gltf_path))["materials"][0].get("extras", {})
        normal = extras.get("vkr_detail_normal_texture", "").split("?")[0]
        assert normal and os.path.isfile(os.path.join(project, normal)), extras

        orm = bpy.data.images.load(os.path.join(os.path.dirname(gltf_path), "textures", "kit_panel_orm.png"))
        metal_texels = int((bake.pixels(orm)[..., 2] > 0.8).sum())
        density = report["sets"][0]["density"]
        expected = np.pi * 0.02 ** 2 * density ** 2
        assert 0.4 * expected < metal_texels < 2.5 * expected, (metal_texels, expected)
        shutil.rmtree(project)
    finally:
        vkr_asset_kit.unregister()


if __name__ == "__main__":
    try:
        test_orient()
        test_asset()
        test_addon()
    except AssertionError as error:
        print("check_asset_kit: FAILED", error)
        sys.exit(1)
    print("check_asset_kit: all checks passed")
