"""Texture bake for the mannequin: carve the shell design into a dense copy
of the game mesh and bake it back as normal, occlusion and mask maps, then
compose the colour and occlusion-roughness-metallic textures. Runs inside
Blender (Cycles on the CPU).
"""

from pathlib import Path

import bmesh
import bpy
import numpy as np

import mannequin_shell as shell


# Linear colours: a warm off-white shell, a dark carbon suit and a pale
# steel-blue accent.
SHELL = np.array([0.600, 0.590, 0.555])
UNDERLAYER = np.array([0.040, 0.046, 0.054])
ACCENT = np.array([0.280, 0.400, 0.520])
SHELL_ROUGHNESS = 0.32
UNDERLAYER_ROUGHNESS = 0.56
ACCENT_ROUGHNESS = 0.30
GROOVE_ROUGHNESS = 0.62
# The suit's carbon weave: tow width in texels, its tone swing and normal
# tilt.
WEAVE_TOW = 5
WEAVE_TONE = 0.45
WEAVE_TILT = 0.18


def _positions_normals(mesh):
    count = len(mesh.vertices)
    p = np.zeros(count * 3)
    mesh.vertices.foreach_get("co", p)
    n = np.zeros(count * 3)
    mesh.vertex_normals.foreach_get("vector", n)
    return p.reshape(-1, 3), n.reshape(-1, 3)


def regions(mesh):
    """Region weights stored on the mesh by mannequin_blender.create_mesh."""
    out = {}
    for attribute in mesh.attributes:
        if attribute.name.startswith("region_"):
            values = np.zeros(len(mesh.vertices), dtype=np.float32)
            attribute.data.foreach_get("value", values)
            out[attribute.name[len("region_"):]] = values.astype(np.float64)
    return out


def displace(obj, design, base):
    """The game mesh's own shape: the design's coarse entries only."""
    p, n = _positions_normals(obj.data)
    result = shell.evaluate(p, n, design, regions(obj.data), fine=False, base=base)
    obj.data.vertices.foreach_set("co", (p + n * result.displacement[:, None]).ravel())
    obj.data.update()


def smooth_normals(mesh, n, iterations=16):
    """Vertex normals relaxed over their neighbours. A subdivision surface's
    normals ripple faintly at the scale of its control mesh; displacing a few
    millimetres along them would shift plate edges sideways with that ripple,
    so the design displaces along the relaxed normals instead."""
    count = len(mesh.vertices)
    edges = np.zeros(len(mesh.edges) * 2, dtype=np.int64)
    mesh.edges.foreach_get("vertices", edges)
    edges = edges.reshape(-1, 2)
    a = np.concatenate([edges[:, 0], edges[:, 1]])
    b = np.concatenate([edges[:, 1], edges[:, 0]])
    for _ in range(iterations):
        total = n.copy()
        np.add.at(total, a, n[b])
        n = total / np.linalg.norm(total, axis=1, keepdims=True)
    return n


def wave_noise(p, frequency, seed, count=24):
    """Smooth 3D noise in [0, 1]: a sum of plane waves in random directions
    around `frequency` (cycles per metre), continuous across UV seams."""
    rng = np.random.default_rng(seed)
    directions = rng.normal(size=(count, 3))
    directions /= np.linalg.norm(directions, axis=1, keepdims=True)
    frequencies = frequency * np.exp(rng.uniform(-0.4, 0.4, count))
    phases = rng.uniform(0.0, 2.0 * np.pi, count)
    total = np.zeros(len(p))
    for direction, f, phase in zip(directions, frequencies, phases):
        total += np.sin(2.0 * np.pi * f * (p @ direction) + phase)
    return np.clip(0.5 + total / (2.2 * np.sqrt(count)), 0.0, 1.0)


def detail_meshes(source_obj, design, levels, base):
    """Subdivided copies of the undisplaced game mesh (rest pose, same UVs)
    carrying the whole design, coarse and fine: the body `levels` finer and
    the helmet (faces with texel_scale above 1) one level finer still.
    `Masks` holds the groove distance over GROOVE_REACH (red), underlayer
    (green) and accent (blue); `Grit` holds plate rims (red), broad and
    medium 3D noise (green, blue) for wear and grime; `Place` holds the
    height over 1.9 m (red) and fine 3D noise (green)."""
    objects = []
    for part, extra in (("body", 0), ("head", 1)):
        mesh = source_obj.data.copy()
        bm = bmesh.new()
        bm.from_mesh(mesh)
        layer = bm.faces.layers.float.get("texel_scale")
        doomed = [face for face in bm.faces if (face[layer] > 1.5) != (part == "head")]
        bmesh.ops.delete(bm, geom=doomed, context="FACES")
        bm.to_mesh(mesh)
        bm.free()
        obj = bpy.data.objects.new(f"{source_obj.name}_Detail_{part}", mesh)
        bpy.context.scene.collection.objects.link(obj)
        modifier = obj.modifiers.new("Subdivision", "SUBSURF")
        modifier.levels = levels + extra
        modifier.render_levels = levels + extra
        modifier.uv_smooth = "PRESERVE_BOUNDARIES"
        depsgraph = bpy.context.evaluated_depsgraph_get()
        dense = bpy.data.meshes.new_from_object(obj.evaluated_get(depsgraph), depsgraph=depsgraph)
        obj.modifiers.clear()
        obj.data = dense
        bpy.data.meshes.remove(mesh)
        p, n = _positions_normals(dense)
        n = smooth_normals(dense, n)
        result = shell.evaluate(p, n, design, regions(dense), fine=True, base=base)
        dense.vertices.foreach_set("co", (p + n * result.displacement[:, None]).ravel())
        count = len(p)
        layers = {"Masks": (result.groove_distance / shell.GROOVE_REACH, result.under, result.accent),
                  "Grit": (result.edge, wave_noise(p, 2.5, 11), wave_noise(p, 14.0, 12)),
                  "Place": (np.clip(p[:, 2] / 1.9, 0.0, 1.0), wave_noise(p, 40.0, 13), np.zeros(count))}
        for name, channels in layers.items():
            colors = np.stack(list(channels) + [np.ones(count)], axis=1)
            attribute = dense.color_attributes.new(name, "FLOAT_COLOR", "POINT")
            attribute.data.foreach_set("color", colors.ravel())
        dense.update()
        objects.append(obj)
    return objects


def _bake_image(name, size, is_data):
    image = bpy.data.images.new(name, size, size, alpha=False, float_buffer=True)
    image.colorspace_settings.name = "Non-Color" if is_data else "sRGB"
    return image


def _target_material(game_obj, image):
    """A throwaway material whose active image node receives the bake."""
    material = bpy.data.materials.new("BakeTarget")
    material.use_nodes = True
    node = material.node_tree.nodes.new("ShaderNodeTexImage")
    node.image = image
    material.node_tree.nodes.active = node
    game_obj.data.materials.clear()
    game_obj.data.materials.append(material)
    return material


def _emission_material(detail_objects, layer):
    material = bpy.data.materials.new("MaskEmission_" + layer)
    material.use_nodes = True
    nodes = material.node_tree.nodes
    links = material.node_tree.links
    nodes.clear()
    output = nodes.new("ShaderNodeOutputMaterial")
    emission = nodes.new("ShaderNodeEmission")
    color = nodes.new("ShaderNodeVertexColor")
    color.layer_name = layer
    links.new(color.outputs["Color"], emission.inputs["Color"])
    links.new(emission.outputs["Emission"], output.inputs["Surface"])
    for obj in detail_objects:
        obj.data.materials.clear()
        obj.data.materials.append(material)


def _pixels(image):
    return np.array(image.pixels[:], dtype=np.float32).reshape(image.size[1], image.size[0], 4)


# The cage sits this far out along the smooth surface's normals, above the
# tallest stack of plates.
CAGE_OFFSET = 0.012


def cage(source_obj):
    """The bake cage: the undisplaced game mesh pushed out along its smooth
    normals. Rays then run along the smooth surface's normals rather than
    the stepped game mesh's, so plate edges bake without wobble."""
    obj = bpy.data.objects.new(source_obj.name + "_Cage", source_obj.data.copy())
    bpy.context.scene.collection.objects.link(obj)
    p, n = _positions_normals(obj.data)
    obj.data.vertices.foreach_set("co", (p + n * CAGE_OFFSET).ravel())
    obj.data.update()
    return obj


def bake(game_obj, detail_objects, cage_obj, size, samples=48):
    """Normal (tangent space), occlusion and the mask layers, as (h, w, 4)
    arrays keyed NORMAL, AO, EMIT (Masks), GRIT and PLACE."""
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = samples
    scene.world = scene.world or bpy.data.worlds.new("World")
    bake_settings = scene.render.bake
    bake_settings.use_selected_to_active = True
    bake_settings.use_cage = True
    bake_settings.cage_object = cage_obj
    bake_settings.max_ray_distance = 2.0 * CAGE_OFFSET
    bake_settings.margin = 12
    bake_settings.margin_type = "EXTEND"
    results = {}
    saved = list(game_obj.data.materials)
    armature = [m for m in game_obj.modifiers if m.type == "ARMATURE"]
    for modifier in armature:
        modifier.show_render = False
        modifier.show_viewport = False
    try:
        # Normals and masks are deterministic per ray; a few samples
        # antialias them. Occlusion needs the full count.
        passes = (("NORMAL", "NORMAL", None, 8), ("AO", "AO", None, samples),
                  ("EMIT", "EMIT", "Masks", 8), ("GRIT", "EMIT", "Grit", 8), ("PLACE", "EMIT", "Place", 4))
        for key, kind, layer, count in passes:
            scene.cycles.samples = count
            if layer:
                _emission_material(detail_objects, layer)
            image = _bake_image(f"bake_{key.lower()}", size, True)
            _target_material(game_obj, image)
            bpy.ops.object.select_all(action="DESELECT")
            for obj in detail_objects:
                obj.select_set(True)
            game_obj.select_set(True)
            bpy.context.view_layer.objects.active = game_obj
            options = {"use_selected_to_active": True, "use_cage": True, "cage_object": cage_obj.name}
            if kind == "NORMAL":
                bpy.ops.object.bake(type="NORMAL", normal_space="TANGENT", **options)
            elif kind == "AO":
                scene.world.light_settings.distance = 0.03
                bpy.ops.object.bake(type="AO", **options)
            else:
                bpy.ops.object.bake(type="EMIT", **options)
            results[key] = _pixels(image)
    finally:
        for modifier in armature:
            modifier.show_render = True
            modifier.show_viewport = True
        game_obj.data.materials.clear()
        for material in saved:
            game_obj.data.materials.append(material)
    return results


def _weave(shape):
    """A basket-weave carbon texture in texture space: square cells of
    WEAVE_TOW texels whose tows alternate direction, each tow shaded round
    across its width. Returns tone (0..1) and a tangent-space normal
    offset (x, y)."""
    h, w = shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    cell = (np.floor(xx / WEAVE_TOW) + np.floor(yy / WEAVE_TOW)).astype(np.int64) % 2
    across = np.where(cell == 0, xx, yy) / WEAVE_TOW * 2.0
    phase = (across % 1.0) * np.pi
    tone = np.sin(phase)
    slope = np.cos(phase) * WEAVE_TILT
    normal = np.stack([np.where(cell == 0, slope, 0.0), np.where(cell == 0, 0.0, slope)], axis=-1)
    return tone, normal


def _noise(shape, cell, seed):
    """Smooth value noise in [0, 1] with `cell` texel features."""
    rng = np.random.default_rng(seed)
    h, w = shape
    grid = rng.random((h // cell + 2, w // cell + 2)).astype(np.float32)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32) / cell
    x0, y0 = xx.astype(np.int64), yy.astype(np.int64)
    fx, fy = xx - x0, yy - y0
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    top = grid[y0, x0] * (1 - fx) + grid[y0, x0 + 1] * fx
    bottom = grid[y0 + 1, x0] * (1 - fx) + grid[y0 + 1, x0 + 1] * fx
    return top * (1 - fy) + bottom * fy


def _scratches(shape, count, seed):
    """Thin straight scratches of random length and direction, 0..1."""
    rng = np.random.default_rng(seed)
    h, w = shape
    mask = np.zeros(shape, dtype=np.float32)
    starts = rng.uniform(0.0, 1.0, (count, 2)) * np.array([w, h])
    angles = rng.uniform(0.0, np.pi, count)
    lengths = rng.uniform(6.0, 42.0, count) * (w / 2048.0)
    strengths = rng.uniform(0.3, 1.0, count)
    for start, angle, length, strength in zip(starts, angles, lengths, strengths):
        steps = max(2, int(length * 2))
        t = np.linspace(0.0, length, steps)
        xs = np.clip((start[0] + np.cos(angle) * t).astype(np.int64), 0, w - 1)
        ys = np.clip((start[1] + np.sin(angle) * t).astype(np.int64), 0, h - 1)
        # Fade in and out along the stroke.
        fade = np.sin(np.pi * t / length) * strength
        np.maximum.at(mask, (ys, xs), fade.astype(np.float32))
    return mask


def compose(baked):
    """Base colour (linear), ORM and normal arrays from the bake.

    The shell wears: rims of raised plates are a touch brighter and chipped
    in places, grime settles into occluded corners and groove floors, the
    surface carries faint scratches and broad tint and roughness variation.
    """
    masks = baked["EMIT"]
    grit = baked["GRIT"]
    groove, floor = shell.groove_shade(np.clip(masks[..., 0], 0.0, 1.0) * shell.GROOVE_REACH)
    under = np.maximum(np.clip(masks[..., 1], 0.0, 1.0), floor)
    accent = np.clip(masks[..., 2], 0.0, 1.0) * (1.0 - floor)
    shell_share = np.clip(1.0 - under - accent, 0.0, 1.0)
    edge = np.clip(grit[..., 0], 0.0, 1.0)
    broad = np.clip(grit[..., 1], 0.0, 1.0)
    medium = np.clip(grit[..., 2], 0.0, 1.0)
    ao = np.clip(baked["AO"][..., 0], 0.0, 1.0)
    height = np.clip(baked["PLACE"][..., 0], 0.0, 1.0) * 1.9
    fine = np.clip(baked["PLACE"][..., 1], 0.0, 1.0)
    shape = groove.shape
    speck = _noise(shape, 3, 5)
    scratches = _scratches(shape, 2600, 9)

    tint = 1.0 + 0.05 * (broad - 0.5) + 0.03 * (medium - 0.5)
    shell_color = SHELL[None, None] * tint[..., None] * (1.0 + 0.07 * edge[..., None])
    chips = edge * np.clip((speck - 0.80) / 0.06, 0.0, 1.0) * np.clip((medium - 0.40) / 0.2, 0.0, 1.0)
    shell_color = shell_color * (1.0 - chips[..., None]) + (UNDERLAYER * 2.5)[None, None] * chips[..., None]
    shell_color = shell_color * (1.0 + 0.06 * scratches[..., None])
    weave, weave_normal = _weave(shape)
    suit_color = UNDERLAYER[None, None] * (1.0 + WEAVE_TONE * (weave[..., None] - 0.5)) * (1.0 - floor[..., None] * 0.4)
    base = (shell_color * shell_share[..., None] + suit_color * under[..., None]
            + ACCENT[None, None] * accent[..., None])
    # Grime settles in occluded corners, and dust climbs the boots and shins.
    dust = (1.0 - np.clip(height / 0.55, 0.0, 1.0)) ** 2 * (0.4 + 0.6 * fine)
    grime = np.clip(np.clip((1.0 - ao) * 1.6, 0.0, 1.0) * (0.55 + 0.45 * medium) + 0.35 * dust, 0.0, 1.0)
    base = base * (1.0 - 0.38 * grime[..., None])

    grain = _noise(shape, 24, 7) - 0.5
    roughness = (SHELL_ROUGHNESS + 0.10 * (broad - 0.5) + 0.06 * grain - 0.06 * edge + 0.10 * scratches
                 + 0.12 * chips) * shell_share
    roughness = roughness + UNDERLAYER_ROUGHNESS * (1.0 + 0.1 * (medium - 0.5) - 0.25 * (weave - 0.5)) * under
    roughness = roughness + ACCENT_ROUGHNESS * accent
    roughness = roughness + (GROOVE_ROUGHNESS - roughness) * groove + 0.10 * grime
    orm = np.stack([0.30 + 0.70 * ao, np.clip(roughness, 0.05, 1.0), np.zeros_like(ao)], axis=-1)
    normal = baked["NORMAL"][..., :3] * 2.0 - 1.0
    normal[..., :2] += weave_normal * under[..., None] * (1.0 - floor[..., None])
    normal /= np.linalg.norm(normal, axis=-1, keepdims=True)
    return base, orm, normal * 0.5 + 0.5


def save_png(array, path, srgb):
    """Save an (h, w, 3) float array; `srgb` encodes linear colour."""
    h, w, _ = array.shape
    data = np.clip(array, 0.0, 1.0)
    if srgb:
        data = np.where(data <= 0.0031308, data * 12.92, 1.055 * np.power(data, 1.0 / 2.4) - 0.055)
    image = bpy.data.images.new(Path(path).stem, w, h, alpha=False)
    image.colorspace_settings.name = "Non-Color"
    image.pixels[:] = np.concatenate([data, np.ones((h, w, 1), dtype=np.float32)], axis=-1).ravel()
    image.filepath_raw = str(path)
    image.file_format = "PNG"
    image.save()
    return image
