"""Bakes a game asset's texture sets from its high-poly surfaces (Blender).

`bake_asset` takes a low-poly object (the game mesh, any number of material
slots) and its high-poly source (by default a copy of the low poly, whose
edges a Bevel shader rounds) and produces, per texture set:

- a unique UV atlas (UV map `atlas`, TEXCOORD_0 on export) at one texel
  density, every hard edge a seam;
- Cycles bakes of the high poly's Principled inputs and the masks the smart
  materials read (smart.py), composed into albedo, tangent normal, ORM
  (occlusion, roughness, metallic: VKR reads occlusion from the
  metallic-roughness texture's red channel) and emissive PNGs;
- one Principled material per set using them, ready for glTF export with
  MikkTSpace tangents, which VKR keeps (ADR-030).

Slots whose materials are in `keep` (screens that show an image chosen per
placement) keep their own material and their UVs.
"""
import json
import math
import os
import time

import bmesh
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

from . import detail_maps, smart

# Hard edges above this angle; atlas islands split just above it, so every
# hard edge is a UV seam (a baked normal map needs both to agree).
SMOOTH_ANGLE = 35.0
RAY_VISIBILITY = ("visible_camera", "visible_diffuse", "visible_glossy", "visible_transmission",
                  "visible_volume_scatter", "visible_shadow")


class Settings:
    """What a bake makes: texel density (px/m), atlas size limits, the
    density under which a model splits into several texture sets, samples
    and the Bevel shader's rounding radius (m)."""

    def __init__(self, density=1024.0, res_min=512, res_max=2048, min_group_density=640.0, max_groups=5,
                 samples=24, bevel_radius=0.0025, condition=1.0, keep=("display", "display_b", "glass_tint"),
                 remove_hidden=True, kinds=None, project_root=None):
        self.density = density
        self.res_min = res_min
        self.res_max = res_max
        self.min_group_density = min_group_density
        self.max_groups = max_groups
        self.samples = samples
        self.bevel_radius = bevel_radius
        self.condition = condition
        self.keep = tuple(keep)
        self.remove_hidden = remove_hidden
        self.kinds = dict(kinds or {})
        # The project folder whose assets/textures/vkr_detail holds the
        # detail maps the sets name; None writes no detail maps.
        self.project_root = project_root


def log(*args):
    print("[vkr-asset-kit]", *args, flush=True)


# ---------------------------------------------------------------------------
# Low-poly preparation

def select_only(objs, active):
    bpy.ops.object.select_all(action="DESELECT")
    for o in objs:
        o.select_set(True)
    bpy.context.view_layer.objects.active = active


def orient_outward(obj):
    """Turns every closed part whose faces point inward (negative signed
    volume) outward: a game mesh draws one side, so an inside-out part
    would vanish and the hidden-face pass would delete it. Open parts
    (planes, cards) keep their winding. Returns the parts flipped."""
    mesh = obj.data
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bm.faces.ensure_lookup_table()
    seen = set()
    flipped = 0
    for start in bm.faces:
        if start.index in seen:
            continue
        part = []
        stack = [start]
        seen.add(start.index)
        while stack:
            f = stack.pop()
            part.append(f)
            for e in f.edges:
                for g in e.link_faces:
                    if g.index not in seen:
                        seen.add(g.index)
                        stack.append(g)
        edges = {e for f in part for e in f.edges}
        if any(not e.is_manifold for e in edges):
            continue
        volume = 0.0
        for f in part:
            v = [l.vert.co for l in f.loops]
            for k in range(1, len(v) - 1):
                volume += v[0].dot(v[k].cross(v[k + 1]))
        if volume < 0.0:
            bmesh.ops.reverse_faces(bm, faces=part)
            flipped += 1
    if flipped:
        bm.to_mesh(mesh)
        mesh.update()
    bm.free()
    return flipped


def coplanar_overlaps(obj, tolerance=0.0002):
    """Pairs of faces from different parts that lie in one plane and
    overlap, facing the same way: the engine z-fights them and the bake
    darkens where they cross. Returns their face index pairs."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.faces.ensure_lookup_table()
    part = {}
    for start in bm.faces:
        if start.index in part:
            continue
        stack = [start]
        part[start.index] = start.index
        while stack:
            f = stack.pop()
            for e in f.edges:
                for g in e.link_faces:
                    if g.index not in part:
                        part[g.index] = start.index
                        stack.append(g)
    tree = BVHTree.FromBMesh(bm)
    found = []
    for a, b in tree.overlap(tree):
        if a >= b or part[a] == part[b]:
            continue
        fa, fb = bm.faces[a], bm.faces[b]
        if fa.normal.dot(fb.normal) < 0.999:
            continue
        if abs((fb.calc_center_median() - fa.calc_center_median()).dot(fa.normal)) > tolerance:
            continue
        found.append((a, b))
    bm.free()
    return found


def remove_hidden_faces(obj, keep=(), rays=48):
    """Deletes faces no viewer can see: every ray from points on the face
    into its hemisphere hits the asset (inside other parts, sandwiched
    between touching parts), or the face looks down from the floor. Frees
    atlas space for the faces that show. Returns the count removed."""
    mesh = obj.data
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bm.faces.ensure_lookup_table()
    keep_idx = {i for i, m in enumerate(mesh.materials) if m and m.name in keep}
    # Kept slots (screens, see-through glass) hide nothing: shelves behind
    # a glass door must stay.
    occluders = bm.copy()
    bmesh.ops.delete(occluders, geom=[f for f in occluders.faces if f.material_index in keep_idx],
                     context="FACES")
    bvh = BVHTree.FromBMesh(occluders)
    occluders.free()
    rng = np.random.default_rng(7)
    dirs = rng.normal(size=(rays, 3))
    dirs /= np.linalg.norm(dirs, axis=1)[:, None]
    zmin = min(v.co.z for v in bm.verts)
    doomed = []
    for f in bm.faces:
        if f.material_index in keep_idx:
            continue
        n = f.normal
        centre = f.calc_center_median()
        if n.z < -0.9 and centre.z < zmin + 0.002:
            doomed.append(f)
            continue
        seen = False
        for p in [centre] + [centre.lerp(v.co, 0.8) for v in f.verts]:
            origin = p + n * 0.0004
            for d in dirs:
                dv = Vector(d)
                if dv.dot(n) <= 0.05:
                    dv = dv - n * (2.0 * dv.dot(n))
                    if dv.dot(n) <= 0.05:
                        continue
                if bvh.ray_cast(origin, dv, 50.0)[0] is None:
                    # Rays toward the floor stop there: the floor hides.
                    if dv.z < 0 and origin.z - zmin < 0.002:
                        continue
                    seen = True
                    break
            if seen:
                break
        if not seen:
            doomed.append(f)
    if doomed:
        bmesh.ops.delete(bm, geom=doomed, context="FACES")
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()
    return len(doomed)


def triangulate(obj):
    """Triangles before the bake: the game mesh's tangent frames (MikkTSpace)
    must come from the same triangles the bake used, and glTF export
    computes tangents only for triangles and quads."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bmesh.ops.triangulate(bm, faces=bm.faces[:], quad_method="BEAUTY", ngon_method="BEAUTY")
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()


def resmooth(obj, angle=SMOOTH_ANGLE):
    """Hard edges above `angle`, weighted normals elsewhere."""
    select_only([obj], obj)
    bpy.ops.object.shade_smooth_by_angle(angle=math.radians(angle))
    mod = obj.modifiers.new("vkr_weighted_normals", "WEIGHTED_NORMAL")
    mod.keep_sharp = True
    bpy.ops.object.modifier_apply(modifier=mod.name)


def is_see_through(mat):
    """A Principled material with alpha below 1 or transmission."""
    if not mat.use_nodes:
        return mat.diffuse_color[3] < 0.99
    _, bsdf = _principled(mat.node_tree)
    if bsdf is None:
        return False
    alpha = bsdf.inputs.get("Alpha")
    transmission = bsdf.inputs.get("Transmission Weight")
    return bool((alpha is not None and not alpha.is_linked and alpha.default_value < 0.99) or
                (transmission is not None and not transmission.is_linked and transmission.default_value > 0.5))


def slot_areas(obj, keep=()):
    """Surface area per material slot, without `keep` slots."""
    areas = {}
    skip = {i for i, m in enumerate(obj.data.materials) if m and m.name in keep}
    for p in obj.data.polygons:
        if p.material_index not in skip:
            areas[p.material_index] = areas.get(p.material_index, 0.0) + p.area
    return areas


def atlas_resolution(area, settings, efficiency=0.62):
    """Pixels per side for `area` square metres at the target density,
    packed at `efficiency`, as a power of two within the limits."""
    need = math.sqrt(max(area, 1e-6) / efficiency) * settings.density
    res = settings.res_min
    # The next size up only when the need passes this one by a third:
    # packing usually beats the efficiency estimate.
    while res * 1.33 < need and res < settings.res_max:
        res *= 2
    return res


def plan_groups(areas, settings, efficiency=0.62):
    """Splits material slots into texture sets small enough for one
    largest atlas at the minimum density: largest slots first, each into
    the lightest set it fits."""
    limit = (settings.res_max / settings.min_group_density) ** 2 * efficiency
    if sum(areas.values()) <= limit:
        return [sorted(areas)]
    groups = []
    for slot, area in sorted(areas.items(), key=lambda kv: -kv[1]):
        fits = [g for g in groups if g[0] + area <= limit]
        if fits:
            g = min(fits, key=lambda g: g[0])
        elif len(groups) < settings.max_groups:
            g = [0.0, []]
            groups.append(g)
        else:
            g = min(groups, key=lambda g: g[0])
        g[0] += area
        g[1].append(slot)
    return [sorted(g[1]) for g in groups]


def unwrap_atlas(obj, groups, resolutions, keep=(), source_uv=None):
    """Adds UV map `atlas`: per texture set, Smart UV islands at one texel
    density packed into 0..1 with a margin of about 6 texels; `keep` faces
    copy `source_uv` (0..1 per screen). Returns each set's density."""
    mesh = obj.data
    source_uv = source_uv or mesh.uv_layers.active.name if mesh.uv_layers else None
    mesh.uv_layers.new(name="atlas")
    mesh.uv_layers.active = mesh.uv_layers["atlas"]
    keep_idx = {i for i, m in enumerate(mesh.materials) if m and m.name in keep}
    select_only([obj], obj)
    # Without sync the UV operators see only the faces selected in the mesh.
    bpy.context.scene.tool_settings.use_uv_select_sync = False
    for slots, res in zip(groups, resolutions):
        members = set(slots)
        bpy.ops.object.mode_set(mode="EDIT")
        # Face mode, everything cleared first: in vertex mode a face whose
        # vertices stay selected counts as selected again.
        bpy.ops.mesh.select_mode(type="FACE")
        bm = bmesh.from_edit_mesh(mesh)
        for element in list(bm.verts) + list(bm.edges) + list(bm.faces):
            element.select = False
        for f in bm.faces:
            f.select = f.material_index in members
        bm.select_flush_mode()
        bmesh.update_edit_mesh(mesh)
        margin = 6.0 / res
        bpy.ops.uv.smart_project(angle_limit=math.radians(SMOOTH_ANGLE + 1.0), island_margin=margin,
                                 area_weight=0.0, correct_aspect=True, scale_to_bounds=False,
                                 margin_method="FRACTION")
        bpy.ops.uv.select_all(action="SELECT")
        bpy.ops.uv.average_islands_scale()
        bpy.ops.uv.pack_islands(rotate=True, rotate_method="ANY", scale=True, margin_method="FRACTION",
                                margin=margin, shape_method="CONCAVE")
        bpy.ops.object.mode_set(mode="OBJECT")
    # Edit mode rebuilt the layers: look them up again.
    atlas = mesh.uv_layers["atlas"]
    if source_uv and source_uv in mesh.uv_layers:
        src = mesh.uv_layers[source_uv]
        for poly in mesh.polygons:
            if poly.material_index in keep_idx:
                for li in poly.loop_indices:
                    atlas.data[li].uv = src.data[li].uv
    densities = []
    for slots, res in zip(groups, resolutions):
        members = set(slots)
        uv_area = 0.0
        world_area = 0.0
        for poly in mesh.polygons:
            if poly.material_index not in members:
                continue
            uvs = [atlas.data[li].uv for li in poly.loop_indices]
            for k in range(1, len(uvs) - 1):
                e1 = uvs[k] - uvs[0]
                e2 = uvs[k + 1] - uvs[0]
                uv_area += abs(e1.x * e2.y - e1.y * e2.x) / 2
            world_area += poly.area
        densities.append(res * math.sqrt(uv_area / max(world_area, 1e-9)))
    return densities


# ---------------------------------------------------------------------------
# High-poly routing: every channel the composer reads, from any Principled
# material, selected by route(channel).

CHANNELS = ("nmap", "albedo", "data", "position", "normal_ws", "masks", "occlusion", "emission")


def _principled(nt):
    out = next((n for n in nt.nodes if n.type == "OUTPUT_MATERIAL" and n.is_active_output), None)
    if out and out.inputs["Surface"].is_linked:
        node = out.inputs["Surface"].links[0].from_node
        if node.type == "BSDF_PRINCIPLED":
            return out, node
    return out, next((n for n in nt.nodes if n.type == "BSDF_PRINCIPLED"), None)


def _source(nt, socket, fallback):
    """The output feeding `socket`, or a constant node with its value."""
    if socket is not None and socket.is_linked:
        return socket.links[0].from_socket
    value = socket.default_value if socket is not None else fallback
    if hasattr(value, "__len__"):
        node = nt.nodes.new("ShaderNodeRGB")
        node.outputs[0].default_value = tuple(value)[:3] + (1.0,)
    else:
        node = nt.nodes.new("ShaderNodeValue")
        node.outputs[0].default_value = float(value)
    return node.outputs[0]


class Routing:
    """A copy of one slot's material with an emission switch."""

    def __init__(self, src, slot, settings):
        mat = src.copy() if src else bpy.data.materials.new("vkr_empty")
        mat.use_nodes = True
        mat.name = f"vkr_hp_{slot}_{src.name if src else 'empty'}"
        nt = mat.node_tree
        nodes, links = nt.nodes, nt.links
        out, bsdf = _principled(nt)
        if out is None:
            out = nodes.new("ShaderNodeOutputMaterial")
        geo = nodes.new("ShaderNodeNewGeometry")
        get = (lambda name: bsdf.inputs.get(name)) if bsdf else (lambda name: None)
        albedo = _source(nt, get("Base Color"), (0.5, 0.5, 0.5))
        rough = _source(nt, get("Roughness"), 0.5)
        metal = _source(nt, get("Metallic"), 0.0)
        data = nodes.new("ShaderNodeCombineColor")
        links.new(rough, data.inputs["Red"])
        links.new(metal, data.inputs["Green"])
        data.inputs["Blue"].default_value = (slot + 0.5) / 32.0
        emit_colour = _source(nt, get("Emission Color"), (0.0, 0.0, 0.0))
        emit_strength = _source(nt, get("Emission Strength"), 0.0)
        emit = nodes.new("ShaderNodeVectorMath")
        emit.operation = "SCALE"
        links.new(emit_colour, emit.inputs[0])
        links.new(emit_strength, emit.inputs["Scale"])
        # Rounded edges: the Bevel shader over the material's own normal.
        normal_in = get("Normal")
        detail = normal_in.links[0].from_socket if normal_in is not None and normal_in.is_linked else geo.outputs["Normal"]
        bevel = nodes.new("ShaderNodeBevel")
        bevel.samples = 16
        bevel.inputs["Radius"].default_value = settings.bevel_radius
        links.new(detail, bevel.inputs["Normal"])
        if bsdf is not None:
            links.new(bevel.outputs["Normal"], bsdf.inputs["Normal"])
            shaded = bsdf.outputs[0]
        else:
            diffuse = nodes.new("ShaderNodeBsdfDiffuse")
            links.new(bevel.outputs["Normal"], diffuse.inputs["Normal"])
            shaded = diffuse.outputs[0]
        # Masks: R edges (a 6 mm bevel against the smooth normal), G local
        # cavity (occlusion by the asset within 3 cm), B 1.
        edge_bevel = nodes.new("ShaderNodeBevel")
        edge_bevel.samples = 16
        edge_bevel.inputs["Radius"].default_value = 0.006
        dot = nodes.new("ShaderNodeVectorMath")
        dot.operation = "DOT_PRODUCT"
        links.new(edge_bevel.outputs["Normal"], dot.inputs[0])
        links.new(geo.outputs["Normal"], dot.inputs[1])
        edge = nodes.new("ShaderNodeMath")
        edge.operation = "SUBTRACT"
        edge.inputs[0].default_value = 1.0
        links.new(dot.outputs["Value"], edge.inputs[1])
        cavity = nodes.new("ShaderNodeAmbientOcclusion")
        cavity.samples = 16
        cavity.only_local = True
        cavity.inputs["Distance"].default_value = 0.03
        masks = nodes.new("ShaderNodeCombineColor")
        links.new(edge.outputs[0], masks.inputs["Red"])
        links.new(cavity.outputs["AO"], masks.inputs["Green"])
        masks.inputs["Blue"].default_value = 1.0
        occlusion = nodes.new("ShaderNodeAmbientOcclusion")
        occlusion.samples = 32
        occlusion.inputs["Distance"].default_value = 0.35
        self.emission = nodes.new("ShaderNodeEmission")
        self.emission.inputs["Strength"].default_value = 1.0
        self.material = mat
        self.out = out
        self.shaded = shaded
        self.sockets = {"albedo": albedo, "data": data.outputs[0], "position": geo.outputs["Position"],
                        "normal_ws": geo.outputs["Normal"], "masks": masks.outputs[0],
                        "occlusion": occlusion.outputs["AO"], "emission": emit.outputs[0]}

    def route(self, channel):
        links = self.material.node_tree.links
        for link in list(self.out.inputs["Surface"].links):
            links.remove(link)
        for link in list(self.emission.inputs["Color"].links):
            links.remove(link)
        if channel == "nmap":
            links.new(self.shaded, self.out.inputs["Surface"])
        else:
            links.new(self.sockets[channel], self.emission.inputs["Color"])
            links.new(self.emission.outputs["Emission"], self.out.inputs["Surface"])


def setup_cycles(samples):
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    prefs = bpy.context.preferences.addons["cycles"].preferences
    for device_type in ("METAL", "OPTIX", "CUDA", "HIP", "ONEAPI"):
        try:
            prefs.compute_device_type = device_type
            prefs.get_devices()
            if any(d.type == device_type for d in prefs.devices):
                for d in prefs.devices:
                    d.use = d.type == device_type
                scene.cycles.device = "GPU"
                break
        except TypeError:
            continue
    else:
        scene.cycles.device = "CPU"
    scene.cycles.samples = samples
    scene.cycles.use_denoising = False
    scene.render.bake.margin_type = "EXTEND"
    if scene.world is None:
        scene.world = bpy.data.worlds.new("vkr_bake")
    scene.world.use_nodes = True
    bg = scene.world.node_tree.nodes.get("Background")
    if bg:
        bg.inputs["Color"].default_value = (1.0, 1.0, 1.0, 1.0)
        bg.inputs["Strength"].default_value = 1.0


def pixels(image):
    arr = np.empty(image.size[0] * image.size[1] * 4, dtype=np.float32)
    image.pixels.foreach_get(arr)
    return arr.reshape(image.size[1], image.size[0], 4)


def bake_channels(hp_objects, lp, routings, groups, resolutions, settings):
    """Bakes every channel from the high poly to the low poly's texture
    sets; returns one dict of numpy arrays per set."""
    targets = []
    owner = {}
    for g, (slots, res) in enumerate(zip(groups, resolutions)):
        image = bpy.data.images.new(f"vkr_bake{g}", res, res, alpha=False, float_buffer=True)
        image.colorspace_settings.name = "Non-Color"
        targets.append(image)
        for slot in slots:
            owner[slot] = image
    dummy = bpy.data.images.new("vkr_bake_none", 16, 16, alpha=False, float_buffer=True)
    for i, mat in enumerate(lp.data.materials):
        nt = mat.node_tree
        node = nt.nodes.get("vkr_bake_target") or nt.nodes.new("ShaderNodeTexImage")
        node.name = "vkr_bake_target"
        node.image = owner.get(i, dummy)
        nt.nodes.active = node
    margin = max(8, max(resolutions) // 128)
    out = [{} for _ in groups]
    plan = [("nmap", "NORMAL", settings.samples), ("albedo", "EMIT", 16), ("data", "EMIT", 4),
            ("position", "EMIT", 1), ("normal_ws", "EMIT", 1), ("masks", "EMIT", settings.samples),
            ("occlusion", "EMIT", settings.samples * 2), ("emission", "EMIT", 4)]
    for channel, kind, spp in plan:
        for routing in routings:
            routing.route(channel)
        bpy.context.scene.cycles.samples = spp
        select_only(list(hp_objects) + [lp], lp)
        t0 = time.time()
        kwargs = dict(type=kind, use_selected_to_active=True, cage_extrusion=0.006, max_ray_distance=0.014,
                      margin=margin, margin_type="EXTEND", target="IMAGE_TEXTURES", use_clear=True,
                      uv_layer="atlas")
        if kind == "NORMAL":
            kwargs.update(normal_space="TANGENT", normal_r="POS_X", normal_g="POS_Y", normal_b="POS_Z")
        bpy.ops.object.bake(**kwargs)
        for g, image in enumerate(targets):
            out[g][channel] = pixels(image).copy()
        log(f"bake {channel} {resolutions} {spp}spp {time.time() - t0:.1f}s")
    for image in targets + [dummy]:
        bpy.data.images.remove(image)
    for mat in lp.data.materials:
        node = mat.node_tree.nodes.get("vkr_bake_target")
        if node:
            mat.node_tree.nodes.remove(node)
    return out


# ---------------------------------------------------------------------------
# Output

def linear_to_srgb(x):
    x = np.clip(x, 0.0, 1.0)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1.0 / 2.4) - 0.055)


def save_png(path, rgb, srgb):
    """Writes HxWx3 `rgb` (0..1, rows bottom-up as Blender keeps them) as an
    8-bit PNG; `srgb` encodes linear values first."""
    h, w = rgb.shape[:2]
    data = linear_to_srgb(rgb) if srgb else np.clip(rgb, 0.0, 1.0)
    image = bpy.data.images.new(os.path.basename(path), w, h, alpha=False, float_buffer=False)
    image.colorspace_settings.name = "sRGB" if srgb else "Non-Color"
    px = np.ones((h, w, 4), dtype=np.float32)
    px[..., :3] = data
    image.pixels.foreach_set(px.ravel())
    image.filepath_raw = path
    image.file_format = "PNG"
    image.save()
    bpy.data.images.remove(image)


def _gltf_output_group():
    group = bpy.data.node_groups.get("glTF Material Output")
    if group:
        return group
    group = bpy.data.node_groups.new("glTF Material Output", "ShaderNodeTree")
    group.interface.new_socket("Occlusion", in_out="INPUT", socket_type="NodeSocketFloat")
    return group


def texture_set_material(name, tex_dir, emission_peak):
    """A Principled material on one texture set's PNGs; the ORM's red
    channel is also glTF occlusion."""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    mat.use_backface_culling = True
    nt = mat.node_tree
    nodes, links = nt.nodes, nt.links
    bsdf = nodes["Principled BSDF"]

    def image(suffix, data):
        node = nodes.new("ShaderNodeTexImage")
        node.image = bpy.data.images.load(os.path.join(tex_dir, f"{name}_{suffix}.png"), check_existing=False)
        if data:
            node.image.colorspace_settings.name = "Non-Color"
        return node

    links.new(image("albedo", False).outputs["Color"], bsdf.inputs["Base Color"])
    sep = nodes.new("ShaderNodeSeparateColor")
    links.new(image("orm", True).outputs["Color"], sep.inputs["Color"])
    links.new(sep.outputs["Green"], bsdf.inputs["Roughness"])
    links.new(sep.outputs["Blue"], bsdf.inputs["Metallic"])
    group = nodes.new("ShaderNodeGroup")
    group.node_tree = _gltf_output_group()
    links.new(sep.outputs["Red"], group.inputs["Occlusion"])
    nmap = nodes.new("ShaderNodeNormalMap")
    nmap.uv_map = "atlas"
    links.new(image("normal", True).outputs["Color"], nmap.inputs["Color"])
    links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    if emission_peak > 1e-4:
        links.new(image("emissive", False).outputs["Color"], bsdf.inputs["Emission Color"])
        bsdf.inputs["Emission Strength"].default_value = max(1.0, emission_peak)
    return mat


def export_gltf(obj, path):
    """glTF (separate files) of `obj` alone: atlas UVs, MikkTSpace tangents,
    no vertex colours."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    select_only([obj], obj)
    bpy.ops.export_scene.gltf(filepath=path, export_format="GLTF_SEPARATE", export_texture_dir="textures",
                              export_image_format="AUTO", export_yup=True, export_texcoords=True,
                              export_normals=True, export_tangents=True, export_materials="EXPORT",
                              export_vertex_color="NONE", use_selection=True, export_extras=True,
                              export_lights=False, export_cameras=False)


DETAIL_FOLDER = "assets/textures/vkr_detail"


def ensure_detail_maps(project_root, family):
    """Writes the family's tileable detail normal and overlay into the
    project once; returns their content paths."""
    folder = os.path.join(project_root, DETAIL_FOLDER)
    normal = f"{DETAIL_FOLDER}/{family}_normal.png"
    # "mask" in the name makes `vkr_bakery cook` encode it as data, as the
    # detail albedo key needs (ADR-136); a material reading the file
    # directly, not through a glTF import, takes that artifact.
    overlay = f"{DETAIL_FOLDER}/{family}_overlay_mask.png"
    if not (os.path.exists(os.path.join(project_root, normal)) and
            os.path.exists(os.path.join(project_root, overlay))):
        os.makedirs(folder, exist_ok=True)
        n, o = detail_maps.normal_and_overlay(family)
        save_png(os.path.join(project_root, normal), n, False)
        save_png(os.path.join(project_root, overlay), o, False)
    return normal, overlay


def set_detail(mat, set_slots, areas, names_kinds, resolution, density, project_root):
    """Gives a texture set's material the detail maps of the kind that
    covers most of it, as glTF extras the importer turns into detail keys,
    weaker when other kinds share the set. Returns the family or None."""
    by_family = {}
    total = 0.0
    for slot in set_slots:
        kind = names_kinds(slot)
        family = detail_maps.FAMILY.get(kind)
        area = areas.get(slot, 0.0)
        total += area
        if family:
            by_family[family] = by_family.get(family, 0.0) + area
    if not by_family or total <= 0.0:
        return None
    family, area = max(by_family.items(), key=lambda kv: kv[1])
    share = area / total
    weight = min(1.0, max(0.3, (share - 0.4) / 0.5))
    normal, overlay = ensure_detail_maps(project_root, family)
    metres_per_uv = resolution / max(density, 1.0)
    mat["vkr_detail_normal_texture"] = f"{normal}?tc=normal_rg"
    mat["vkr_detail_normal_scale"] = round(detail_maps.STRENGTH[family] * weight, 3)
    mat["vkr_detail_uv_scale"] = round(min(1024.0, max(1.0, metres_per_uv / detail_maps.TILE[family])), 2)
    mat["vkr_detail_albedo_texture"] = f"{overlay}?tc=data_mask"
    mat["vkr_detail_albedo_strength"] = round(detail_maps.ALBEDO_STRENGTH[family] * weight, 3)
    return family


# ---------------------------------------------------------------------------
# Driver

def bake_asset(lp, name, out_dir, settings=None, hp_objects=None, details=(), debug_dir=None):
    """Bakes `lp` (a mesh object, modified in place into the game mesh) from
    `hp_objects` (default: a copy of `lp`) and writes
    `<out_dir>/<name>/<name>.gltf` with its textures. Returns a report."""
    settings = settings or Settings()
    t_start = time.time()
    lp.name = name
    slots = [m.name if m else "" for m in lp.data.materials]
    # See-through materials cannot bake into an opaque set: they keep their
    # own material, as screens do.
    see_through = [m.name for m in lp.data.materials if m and is_see_through(m)]
    if see_through:
        settings = Settings(**{**settings.__dict__, "keep": tuple(settings.keep) + tuple(see_through)})
    kinds = dict(settings.kinds)
    for m in lp.data.materials:
        if m and m.get("vkr_smart"):
            kinds[m.name] = str(m["vkr_smart"])
    tris_before = sum(len(p.vertices) - 2 for p in lp.data.polygons)
    flipped = orient_outward(lp)
    if flipped:
        log(f"{name}: turned {flipped} inside-out parts outward")
    overlaps = coplanar_overlaps(lp)
    if overlaps:
        log(f"{name}: {len(overlaps)} coplanar face pairs overlap (z-fighting); offset or split those parts")
    removed = remove_hidden_faces(lp, settings.keep) if settings.remove_hidden else 0
    triangulate(lp)
    resmooth(lp)
    tris = sum(len(p.vertices) - 2 for p in lp.data.polygons)
    areas = slot_areas(lp, settings.keep)
    groups = plan_groups(areas, settings)
    resolutions = [atlas_resolution(sum(areas[s] for s in g), settings) for g in groups]
    log(f"{name}: {tris_before} -> {tris} tris ({removed} hidden faces), {sum(areas.values()):.2f} m2, "
        f"sets {[[slots[i] for i in g] for g in groups]} at {resolutions}")
    own_hp = not hp_objects
    if own_hp:
        hp = lp.copy()
        hp.data = lp.data.copy()
        hp.name = name + "_hp"
        bpy.context.scene.collection.objects.link(hp)
        hp_objects = [hp]
    # Each high-poly slot routes its own material's channels; the bake's
    # slot id names the material the composer layers by its kind.
    routings = []
    names = []
    for hp in hp_objects:
        for i, mat in enumerate(hp.data.materials):
            key = mat.name if mat else f"slot{i}"
            if key not in names:
                names.append(key)
            if mat and mat.get("vkr_smart"):
                kinds[key] = str(mat["vkr_smart"])
            routing = Routing(mat, names.index(key), settings)
            hp.data.materials[i] = routing.material
            routings.append(routing)
    if len(names) > 32:
        raise ValueError(f"{name}: the high poly has {len(names)} materials; the bake tells 32 apart")
    uv_source = lp.data.uv_layers.active.name if lp.data.uv_layers else None
    densities = unwrap_atlas(lp, groups, resolutions, settings.keep, uv_source)
    log(f"{name}: texel density {[round(d) for d in densities]} px/m")
    saved_visibility = {attr: getattr(lp, attr) for attr in RAY_VISIBILITY}
    for attr in RAY_VISIBILITY:
        setattr(lp, attr, False)
    bpy.ops.mesh.primitive_plane_add(size=60.0, location=(0, 0, min((lp.matrix_world @ Vector(c)).z
                                                                    for c in lp.bound_box) - 0.0005))
    floor = bpy.context.active_object
    setup_cycles(settings.samples)
    sets = bake_channels(hp_objects, lp, routings, groups, resolutions, settings)
    bpy.data.objects.remove(floor)
    tex_dir = os.path.join(out_dir, name, "textures")
    os.makedirs(tex_dir, exist_ok=True)
    materials = []
    unknown = []
    details_used = []
    for g, ch in enumerate(sets):
        set_name = name if g == 0 else f"{name}_{g}"
        debug = {} if debug_dir else None
        albedo, rough, metal, ao, emission, height = smart.compose(
            ch, names, details, densities[g], settings.condition, kinds=kinds, keep=settings.keep, debug=debug,
            unknown=unknown)
        nmap = smart.apply_height(ch["nmap"], height, 1.0 / densities[g])
        save_png(os.path.join(tex_dir, f"{set_name}_albedo.png"), albedo, True)
        save_png(os.path.join(tex_dir, f"{set_name}_normal.png"), nmap, False)
        save_png(os.path.join(tex_dir, f"{set_name}_orm.png"), np.stack([ao, rough, metal], -1), False)
        peak = float(emission.max())
        if peak > 1e-4:
            save_png(os.path.join(tex_dir, f"{set_name}_emissive.png"), emission / max(peak, 1.0), True)
        if debug_dir:
            os.makedirs(debug_dir, exist_ok=True)
            for key, layer in debug.items():
                save_png(os.path.join(debug_dir, f"{set_name}_{key}.png"), np.repeat(layer[..., None], 3, -1),
                         False)
        mat = texture_set_material(set_name, tex_dir, peak)
        if settings.project_root:
            family = set_detail(mat, groups[g], areas, lambda i: smart.kind_of(slots[i], kinds.get(slots[i])),
                                resolutions[g], densities[g], settings.project_root)
            details_used.append(family)
        materials.append(mat)
    if unknown:
        log(f"{name}: unknown details {sorted(set(unknown))}")
    # The game mesh: one baked material per texture set, then kept slots.
    keep = [s for s in slots if s in settings.keep]
    group_of = {slot: g for g, members in enumerate(groups) for slot in members}
    remap = {i: (len(groups) + keep.index(s) if s in keep else group_of.get(i, 0)) for i, s in enumerate(slots)}
    mesh = lp.data
    indices = [remap[p.material_index] for p in mesh.polygons]
    kept_materials = [mesh.materials[slots.index(s)] for s in keep]
    # Clearing the slots resets every face's index: assign after.
    mesh.materials.clear()
    for mat in materials + kept_materials:
        mesh.materials.append(mat)
    mesh.polygons.foreach_set("material_index", indices)
    mesh.update()
    for layer_name in [uv.name for uv in mesh.uv_layers if uv.name != "atlas"]:
        mesh.uv_layers.remove(mesh.uv_layers[layer_name])
    mesh.uv_layers.active = mesh.uv_layers["atlas"]
    mesh.uv_layers["atlas"].active_render = True
    for attribute_name in [a.name for a in mesh.color_attributes]:
        mesh.color_attributes.remove(mesh.color_attributes[attribute_name])
    for attr, value in saved_visibility.items():
        setattr(lp, attr, value)
    if own_hp:
        bpy.data.objects.remove(hp_objects[0])
    path = os.path.join(out_dir, name, f"{name}.gltf")
    export_gltf(lp, path)
    report = {"name": name, "path": path, "triangles": tris, "hidden_faces": removed, "flipped_parts": flipped,
              "coplanar_overlaps": len(overlaps),
              "sets": [{"slots": [slots[i] for i in g], "resolution": r, "density": round(d),
                        "detail": details_used[k] if k < len(details_used) else None}
                       for k, (g, r, d) in enumerate(zip(groups, resolutions, densities))],
              "seconds": round(time.time() - t_start, 1)}
    log(json.dumps(report))
    return report
