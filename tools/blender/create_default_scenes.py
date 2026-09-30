"""Create VKR's measured starter worlds in the connected Blender instance.

Run as a Blender Python script, or execute its contents with ``__file__`` set
to its path. For live MCP execution without ``__file__``, pass ``output_dir``
explicitly to ``create_scenes``. The existing scene is preserved. Generated
scenes can be rebuilt safely because their ownership tag is checked first.
All authored dimensions are meters, Blender Z-up. Exported glTF is Y-up.
"""

import json
import math
from pathlib import Path

import bpy
from mathutils import Vector


GENERATOR = "vkr.default_scenes.v1"
PREFIX = "VKR_Template_"


def _material(name, color, metallic=0.0, roughness=0.75):
    key = PREFIX + "Material_" + name
    material = bpy.data.materials.get(key)
    if material is None:
        material = bpy.data.materials.new(key)
        material["vkr_generator"] = GENERATOR
    elif material.get("vkr_generator") != GENERATOR:
        raise RuntimeError("Unowned material name collision: " + key)
    material.use_nodes = True
    material.diffuse_color = (*color, 1.0)
    shader = material.node_tree.nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*color, 1.0)
    shader.inputs["Metallic"].default_value = metallic
    shader.inputs["Roughness"].default_value = roughness
    return material


def _palette():
    return {
        "floor": _material("Graphite", (0.115, 0.145, 0.17)),
        "concrete": _material("Concrete", (0.33, 0.39, 0.43)),
        "edge": _material("Edge", (0.56, 0.64, 0.68)),
        "dark": _material("Ink", (0.027, 0.043, 0.06)),
        "white": _material("Ivory", (0.88, 0.9, 0.84)),
        "yellow": _material("SafetyYellow", (1.0, 0.66, 0.055)),
        "orange": _material("SafetyOrange", (0.95, 0.24, 0.045)),
        "blue": _material("TeamBlue", (0.015, 0.45, 0.7)),
        "red": _material("TeamRed", (0.73, 0.065, 0.11)),
        "mint": _material("RouteMint", (0.12, 0.66, 0.49)),
        "stone": _material("Limestone", (0.48, 0.46, 0.36)),
        "stone_light": _material("PaleStone", (0.71, 0.69, 0.55)),
        "earth": _material("Courtyard", (0.16, 0.2, 0.16)),
        "clay": _material("Terracotta", (0.57, 0.265, 0.13)),
        "wood": _material("DarkOak", (0.18, 0.105, 0.055)),
        "grass": _material("Moss", (0.23, 0.33, 0.18)),
    }


class _World:
    def __init__(self, key, title, size, materials):
        name = PREFIX + title
        previous = bpy.data.scenes.get(name)
        if previous is not None:
            if previous.get("vkr_generator") != GENERATOR:
                raise RuntimeError("Unowned scene name collision: " + name)
            owned_objects = list(previous.objects)
            owned_collections = list(previous.collection.children)
            owned_world = previous.world
            bpy.data.scenes.remove(previous)
            for obj in owned_objects:
                if obj.get("vkr_generator") == GENERATOR:
                    data = obj.data
                    bpy.data.objects.remove(obj, do_unlink=True)
                    if data is not None and data.users == 0:
                        if isinstance(data, bpy.types.Mesh):
                            bpy.data.meshes.remove(data)
                        elif isinstance(data, bpy.types.Curve):
                            bpy.data.curves.remove(data)
                        elif isinstance(data, bpy.types.Camera):
                            bpy.data.cameras.remove(data)
                        elif isinstance(data, bpy.types.Light):
                            bpy.data.lights.remove(data)
            for collection in owned_collections:
                if collection.users == 0:
                    bpy.data.collections.remove(collection)
            if owned_world is not None and owned_world.users == 0:
                bpy.data.worlds.remove(owned_world)
        self.scene = bpy.data.scenes.new(name)
        self.scene["vkr_generator"] = GENERATOR
        self.scene["vkr_template"] = key
        self.scene.unit_settings.system = "METRIC"
        self.scene.unit_settings.scale_length = 1.0
        self.scene.unit_settings.length_unit = "METERS"
        bpy.context.window.scene = self.scene
        self.collection = bpy.data.collections.new(name + "_World")
        self.scene.collection.children.link(self.collection)
        self.materials = materials
        self.key = key
        self.serial = 0
        self.structural = []
        self.visual = []
        self.spawns = []
        self.features = []
        self.size = size

    def object(self, name, data, location=(0, 0, 0), material=None, collision=True):
        self.serial += 1
        obj = bpy.data.objects.new(f"{self.key}.{name}.{self.serial:04d}", data)
        self.collection.objects.link(obj)
        obj.location = location
        obj["vkr_generator"] = GENERATOR
        obj["vkr_template"] = self.key
        obj["vkr_collision"] = bool(collision)
        if material is not None:
            obj.data.materials.append(self.materials[material])
        if collision:
            self.structural.append(obj)
        self.visual.append(obj)
        return obj

    def mesh(self, name, vertices, faces, material, location=(0, 0, 0), collision=True):
        mesh = bpy.data.meshes.new(self.key + "." + name + ".mesh")
        mesh.from_pydata(vertices, [], faces)
        mesh.update()
        return self.object(name, mesh, location, material, collision)

    def box(self, name, center, dimensions, material="concrete", bevel=0.025,
            collision=True, yaw=0.0):
        x, y, z = (value * 0.5 for value in dimensions)
        vertices = [(-x, -y, -z), (x, -y, -z), (x, y, -z), (-x, y, -z),
                    (-x, -y, z), (x, -y, z), (x, y, z), (-x, y, z)]
        faces = [(0, 3, 2, 1), (0, 1, 5, 4), (1, 2, 6, 5),
                 (2, 3, 7, 6), (3, 0, 4, 7), (4, 5, 6, 7)]
        obj = self.mesh(name, vertices, faces, material, center, collision)
        obj.rotation_euler.z = yaw
        if bevel > 0:
            modifier = obj.modifiers.new("Small edge bevel", "BEVEL")
            modifier.width = min(bevel, min(dimensions) * 0.22)
            modifier.segments = 1
        return obj

    def cylinder(self, name, center, radius, depth, material="concrete",
                 sides=16, collision=True):
        vertices = []
        for z in (-depth * 0.5, depth * 0.5):
            for index in range(sides):
                angle = math.tau * index / sides
                vertices.append((radius * math.cos(angle), radius * math.sin(angle), z))
        faces = [tuple(reversed(range(sides))), tuple(range(sides, sides * 2))]
        for index in range(sides):
            nxt = (index + 1) % sides
            faces.append((index, nxt, nxt + sides, index + sides))
        return self.mesh(name, vertices, faces, material, center, collision)

    def stripe(self, name, center, dimensions, material="yellow", yaw=0.0):
        return self.box(name, center, dimensions, material, bevel=0,
                        collision=False, yaw=yaw)

    def outline(self, name, center, dimensions, material="yellow", width=0.045):
        x, y, z = center
        dx, dy = dimensions
        for sign in (-1, 1):
            self.stripe(name + "_x", (x, y + sign * dy * 0.5, z), (dx, width, 0.012), material)
            self.stripe(name + "_y", (x + sign * dx * 0.5, y, z), (width, dy, 0.012), material)

    def label(self, text, location, size=0.35, material="white", vertical=False,
              yaw=0.0, align="CENTER"):
        curve = bpy.data.curves.new(self.key + ".label", "FONT")
        curve.body = text
        curve.size = size
        curve.align_x = align
        curve.align_y = "CENTER"
        curve.resolution_u = 2
        curve.extrude = 0.0005
        obj = self.object("label_" + text.replace("\n", "_"), curve,
                          location, material, collision=False)
        if vertical:
            obj.rotation_euler = (math.pi / 2, 0, yaw)
        else:
            obj.rotation_euler.z = yaw
        for selected in list(bpy.context.selected_objects):
            selected.select_set(False)
        obj.select_set(True)
        bpy.context.view_layer.objects.active = obj
        status = bpy.ops.object.convert(target="MESH")
        if "FINISHED" not in status:
            raise RuntimeError("Text conversion failed: " + text)
        # The converter replaces data in the existing object; capture active ref.
        converted = bpy.context.view_layer.objects.active
        converted["vkr_label"] = text
        converted.select_set(False)
        return converted

    def board(self, title, subtitle, center, width=7.0, accent="yellow"):
        x, y, z = center
        self.box("information_board", (x, y, z), (width, 0.14, 1.1), "dark",
                 collision=False)
        self.stripe("board_accent", (x, y - 0.074, z + 0.44), (width - 0.16, 0.02, 0.04), accent)
        self.label(title, (x, y - 0.085, z + 0.17), size=0.27, vertical=True)
        self.label(subtitle, (x, y - 0.086, z - 0.2), size=0.15,
                   material="edge", vertical=True)

    def floor(self, material="floor"):
        self.box("foundation", (0, 0, -0.25), (*self.size, 0.5), material, bevel=0.02)

    def feature(self, kind, name, position, **measurements):
        self.features.append({"kind": kind, "name": name,
                              "position_blender_m": list(position), **measurements})

    def spawn(self, name, position, forward=(0, 1, 0), material="mint"):
        x, y, z = position
        surface = z - 0.04
        self.cylinder("spawn_" + name + "_disk", (x, y, surface + 0.012), 0.7, 0.012,
                      material, sides=24, collision=False)
        self.cylinder("spawn_" + name + "_center", (x, y, surface + 0.022), 0.52, 0.008,
                      "dark", sides=24, collision=False)
        arrow = self.mesh("spawn_" + name + "_arrow",
                          [(-0.15, -0.2, 0), (0.15, -0.2, 0), (0, 0.32, 0)],
                          [(0, 1, 2)], material, (x, y, surface + 0.028), False)
        arrow.rotation_euler.z = math.atan2(-forward[0], forward[1])
        marker = self.object("spawn_" + name, None, position, collision=False)
        marker.empty_display_type = "ARROWS"
        marker.empty_display_size = 0.5
        marker["vkr_spawn_name"] = name
        marker["vkr_forward_blender"] = list(forward)
        marker["vkr_spawn_position_blender"] = list(position)
        self.spawns.append({"name": name, "object": marker.name,
                            "position_blender_m": list(position),
                            "forward_blender": list(forward),
                            "position_gltf_m": [x, z, -y],
                            "forward_gltf": [forward[0], forward[2], -forward[1]]})

    def ruler(self, name, start, length, axis="X", major=5, material="yellow"):
        x, y, z = start
        for index in range(length + 1):
            is_major = index % major == 0
            tick = 0.5 if is_major else 0.22
            px = x + (index if axis == "X" else 0)
            py = y + (index if axis == "Y" else 0)
            dimensions = (0.035, tick, 0.008) if axis == "X" else (tick, 0.035, 0.008)
            self.stripe(name + "_tick", (px, py, z), dimensions, material)
            if is_major:
                self.label(str(index) + "m", (px, py - 0.6, z + 0.01),
                           size=0.25, material=material)
        dimensions = (length, 0.018, 0.006) if axis == "X" else (0.018, length, 0.006)
        center = (x + (length / 2 if axis == "X" else 0),
                  y + (length / 2 if axis == "Y" else 0), z)
        self.stripe(name + "_baseline", center, dimensions, material)
        self.feature("ruler", name, start, axis=axis, length_m=length, tick_m=1, major_tick_m=major)

    def stairs(self, name, x, y_start, width, run, rise, base=0, material="concrete",
               top_overlap=0.0, edge_bevel=0.008):
        steps = round(rise / 0.2)
        actual_rise = rise / steps
        tread = run / steps
        for index in range(steps):
            height = (index + 1) * actual_rise
            overlap = top_overlap if index == steps - 1 else 0.0
            depth = tread + overlap
            y = y_start + (index + 0.5) * tread + overlap / 2
            self.box(name + "_step", (x, y, base + height / 2),
                     (width, depth, height), material, bevel=edge_bevel)
            self.stripe(name + "_nosing", (x, y - depth / 2 + 0.035, base + height + 0.008),
                        (width - 0.06, 0.055, 0.01), "yellow")
        self.label(f"{actual_rise:.2f}m RISE / {tread:.2f}m TREAD",
                   (x, y_start - 0.6, base + 0.025), 0.19, "yellow")
        self.feature("stairs", name, (x, y_start, base), width_m=width,
                     run_m=run, rise_m=rise, steps=steps, step_rise_m=actual_rise,
                     step_tread_m=tread, top_landing_overlap_m=top_overlap,
                     direction_blender=[0, 1, 0])

    def ramp(self, name, x, y_start, width, run, rise, base=0, material="concrete", yaw=0):
        w = width / 2
        vertices = [(-w, 0, 0), (w, 0, 0), (w, run, 0), (-w, run, 0),
                    (-w, 0, 0.015), (w, 0, 0.015), (w, run, rise), (-w, run, rise)]
        faces = [(0, 3, 2, 1), (0, 1, 5, 4), (1, 2, 6, 5),
                 (2, 3, 7, 6), (3, 0, 4, 7), (4, 5, 6, 7)]
        ramp = self.mesh(name, vertices, faces, material, (x, y_start, base))
        ramp.rotation_euler.z = yaw
        slope = math.degrees(math.atan2(rise - 0.015, run))
        forward = (-math.sin(yaw), math.cos(yaw), 0)
        across = (math.cos(yaw), math.sin(yaw), 0)
        for sign in (-1, 1):
            center = (x + forward[0] * run / 2 + across[0] * sign * (w - 0.045),
                      y_start + forward[1] * run / 2 + across[1] * sign * (w - 0.045),
                      base + (rise + 0.015) / 2 + 0.008)
            line = self.stripe(name + "_edge", center, (0.07, math.hypot(run, rise - 0.015), 0.008))
            line.rotation_euler.x = math.atan2(rise - 0.015, run)
            line.rotation_euler.z = yaw
        self.label(f"RAMP {slope:.1f} DEG / {rise:.1f}m",
                   (x - forward[0] * 0.7, y_start - forward[1] * 0.7, base + 0.02),
                   size=0.26, material="yellow")
        self.feature("ramp", name, (x, y_start, base), width_m=width,
                     run_m=run, rise_m=rise, entry_height_m=0.015,
                     slope_degrees=slope, direction_blender=list(forward))

    def platform(self, name, center_xy, size, height, material="concrete", thickness=0.3):
        x, y = center_xy
        self.box(name + "_deck", (x, y, height - thickness / 2), (*size, thickness), material)
        self.outline(name + "_rim", (x, y, height + 0.015),
                     (size[0] - 0.12, size[1] - 0.12), "yellow", 0.045)
        if height > 0.5:
            for sx in (-1, 1):
                for sy in (-1, 1):
                    px = x + sx * (size[0] / 2 - 0.3)
                    py = y + sy * (size[1] / 2 - 0.3)
                    self.box(name + "_pier", (px, py, (height - thickness) / 2),
                             (0.32, 0.32, height - thickness), "dark", bevel=0.01)
        self.feature("platform", name, (x, y, height), size_m=list(size), top_height_m=height)

    def gap(self, name, x, y, gap, height=0.8, material="concrete", width=2.5):
        length = 2.0
        center_offset = (gap + length) / 2
        for sign in (-1, 1):
            center = (x + sign * center_offset, y, height / 2)
            self.box(name + "_landing", center, (length, width, height), material)
            self.outline(name + "_landing_rim", (center[0], y, height + 0.012),
                         (length - 0.1, width - 0.1), "orange")
        self.stripe(name + "_gap_reference", (x, y - width / 2 - 0.25, 0.015), (gap, 0.045, 0.008))
        self.label(f"{gap:.0f}m GAP", (x, y - width / 2 - 0.75, 0.025), 0.3, "yellow")
        self.feature("jump_gap", name, (x, y, height), gap_m=gap,
                     landing_length_m=length, landing_width_m=width, deck_height_m=height)

    def gate(self, name, x, y, clear_width, clear_height, material="concrete", depth=1.0):
        post = 0.2
        for sign in (-1, 1):
            self.box(name + "_post", (x + sign * (clear_width + post) / 2, y, clear_height / 2),
                     (post, depth, clear_height), material, bevel=0.01)
        self.box(name + "_lintel", (x, y, clear_height + 0.12),
                 (clear_width + post * 2, depth, 0.24), material, bevel=0.01)
        self.stripe(name + "_header", (x, y - depth / 2 - 0.012, clear_height + 0.12),
                    (clear_width, 0.012, 0.06), "yellow")
        self.label(f"{clear_width:.1f}m W / {clear_height:.1f}m H",
                   (x, y - depth / 2 - 0.03, clear_height + 0.13),
                   0.17, "dark", vertical=True)
        self.feature("clearance", name, (x, y, 0), clear_width_m=clear_width,
                     clear_height_m=clear_height, depth_m=depth)

    def silhouette(self, name, x, y, yaw=0, material="orange"):
        # Human scale flat silhouette: 1.8m tall, 0.54m shoulders.
        outline = [(-0.09, 0.04), (-0.25, 0.04), (-0.2, 0.76), (-0.27, 1.02),
                   (-0.27, 1.39), (-0.16, 1.52), (-0.12, 1.62), (-0.12, 1.75),
                   (-0.06, 1.8), (0.06, 1.8), (0.12, 1.75), (0.12, 1.62),
                   (0.16, 1.52), (0.27, 1.39), (0.27, 1.02), (0.2, 0.76),
                   (0.25, 0.04), (0.09, 0.04), (0, 0.62)]
        vertices = [(px, py, pz) for py in (-0.04, 0.04) for px, pz in outline]
        count = len(outline)
        faces = [tuple(reversed(range(count))), tuple(range(count, count * 2))]
        for index in range(count):
            nxt = (index + 1) % count
            faces.append((index, nxt, nxt + count, index + count))
        obj = self.mesh(name, vertices, faces, material, (x, y, 0), False)
        obj.rotation_euler.z = yaw
        self.box(name + "_base", (x, y, 0.055), (0.9, 0.9, 0.1), "dark", collision=False)
        self.feature("target", name, (x, y, 0), height_m=1.8, shoulder_width_m=0.54)

    def presentation(self, camera_position, aim=(0, 0, 1.5), orthographic=64):
        data = bpy.data.cameras.new(self.key + ".overview_camera")
        camera = self.object("overview_camera", data, camera_position, collision=False)
        camera.rotation_euler = (Vector(aim) - camera.location).to_track_quat("-Z", "Y").to_euler()
        data.type = "ORTHO"
        data.ortho_scale = orthographic
        data.lens = 50
        data.clip_end = 500
        self.scene.camera = camera
        light_data = bpy.data.lights.new(self.key + ".sun", "SUN")
        light_data.energy = 2.2
        light_data.angle = math.radians(18)
        sun = self.object("sun", light_data, (0, 0, 25), collision=False)
        sun.rotation_euler = (math.radians(28), math.radians(-25), math.radians(-35))
        fill_data = bpy.data.lights.new(self.key + ".fill", "AREA")
        fill_data.energy = 1600
        fill_data.shape = "DISK"
        fill_data.size = 30
        fill = self.object("fill", fill_data, (0, -10, 25), collision=False)
        fill.rotation_euler = (Vector((0, 0, 0)) - fill.location).to_track_quat("-Z", "Y").to_euler()
        world = bpy.data.worlds.new(self.key + ".world")
        world.use_nodes = True
        world.node_tree.nodes["Background"].inputs["Color"].default_value = (0.12, 0.17, 0.23, 1)
        world.node_tree.nodes["Background"].inputs["Strength"].default_value = 0.45
        self.scene.world = world
        self.scene.render.engine = "CYCLES"
        self.scene.cycles.samples = 32
        self.scene.render.resolution_x = 1600
        self.scene.render.resolution_y = 1200
        self.scene.render.resolution_percentage = 100

    def export(self, directory, enabled=True):
        bpy.context.window.scene = self.scene
        bpy.context.view_layer.update()
        for obj in self.scene.objects:
            obj.select_set(False)
        dependencies = bpy.context.evaluated_depsgraph_get()
        triangle_count = 0
        for obj in self.visual:
            if obj.type == "MESH":
                mesh = obj.evaluated_get(dependencies).to_mesh()
                mesh.calc_loop_triangles()
                triangle_count += len(mesh.loop_triangles)
                obj.evaluated_get(dependencies).to_mesh_clear()
        if triangle_count >= 150000:
            raise RuntimeError(f"{self.key} exceeds triangle budget: {triangle_count}")
        paths = {}
        if enabled:
            for suffix, objects, modifiers, materials in (
                ("", [obj for obj in self.visual if obj.type in {"MESH", "EMPTY"}], True, "EXPORT"),
                ("_collision", self.structural, False, "NONE"),
            ):
                for selected in list(bpy.context.selected_objects):
                    selected.select_set(False)
                for obj in objects:
                    obj.select_set(True)
                path = directory / (self.key + suffix + ".glb")
                status = bpy.ops.export_scene.gltf(
                    filepath=str(path), export_format="GLB", check_existing=False,
                    use_selection=True, use_active_scene=True, export_yup=True,
                    export_apply=modifiers, export_materials=materials,
                    export_animations=False, export_skins=False, export_morph=False,
                    export_cameras=False, export_lights=False, export_extras=True,
                    export_texcoords=False, export_tangents=False,
                )
                if "FINISHED" not in status or not path.is_file() or path.stat().st_size == 0:
                    raise RuntimeError("Export did not finish: " + str(path))
                # Manifest references resolve beside measurements.json.
                paths["visual" if not suffix else "collision"] = path.name
        for selected in list(bpy.context.selected_objects):
            selected.select_set(False)
        # Calculate true triangulation for wedges and cylinders, independent of quads.
        collision_triangles = 0
        for obj in self.structural:
            obj.data.calc_loop_triangles()
            collision_triangles += len(obj.data.loop_triangles)
        return {"key": self.key, "scene": self.scene.name,
                "footprint_m": list(self.size), "visual_triangles": triangle_count,
                "collision_triangles": collision_triangles,
                "visual_meshes": sum(obj.type == "MESH" for obj in self.visual),
                "collision_meshes": len(self.structural), "exports": paths,
                "overview_camera": self.scene.camera.name,
                "spawns": self.spawns, "features": self.features}


def _blank(materials):
    world = _World("blank", "Blank", (20, 20), materials)
    world.floor()
    world.outline("perimeter", (0, 0, 0.014), (19.8, 19.8), "edge", 0.035)
    for sign in (-1, 1):
        world.stripe("origin_cross", (0, 0, 0.017), (0.035, 1.5, 0.008), "mint", yaw=(0 if sign < 0 else math.pi / 2))
    world.ruler("one_meter_reference", (-5, -9, 0.017), 10)
    world.label("20 x 20m", (0, 8.8, 0.02), 0.42, "edge")
    world.label("+Y / FORWARD", (0, 6.8, 0.02), 0.23, "edge")
    world.label("ORIGIN", (0, -1.2, 0.02), 0.22, "mint")
    world.spawn("main", (0, -6, 0.04))
    world.feature("origin", "world_origin", (0, 0, 0))
    world.presentation((22, -28, 27), orthographic=29)
    return world


def _fps(materials):
    world = _World("fps_arena", "FPS_Arena", (48, 40), materials)
    world.floor()
    for sign in (-1, 1):
        world.box("side_boundary", (sign * 23.75, 0, 0.65), (0.5, 40, 1.3), "dark")
        world.box("end_boundary", (0, sign * 19.75, 0.65), (47, 0.5, 1.3), "dark")
        world.stripe("boundary_warning", (sign * 23.37, 0, 0.025), (0.12, 39.4, 0.01))
        team = "blue" if sign < 0 else "red"
        world.stripe("spawn_zone", (sign * 19, -7, 0.01), (6, 6, 0.012), team)
        world.outline("spawn_zone_boundary", (sign * 19, -7, 0.025), (5.9, 5.9), "white")
        world.label("BLUE" if sign < 0 else "RED", (sign * 19, -9, 0.035), 0.55)
        world.spawn(team, (sign * 19, -7, 0.04), (-sign, 0, 0), team)
        world.platform(team + "_deck_3m", (sign * 14, 5), (8, 6), 3)
        # The final tread overlaps the landing instead of relying on beveled edges.
        world.stairs(team + "_stairs_0_to_3", sign * 13.5, -4, 3, 6, 3,
                     top_overlap=0.15, edge_bevel=0)
        # A clear 1.5m approach remains on the existing +3m deck before this flight.
        upper_x = sign * 16.8
        world.outline(team + "_upper_stair_bottom_landing", (upper_x, 2.75, 3.02),
                      (2.3, 1.2), team, 0.04)
        world.label("TO +6m", (upper_x, 2.75, 3.03), 0.22, team)
        world.feature("landing", team + "_upper_stair_bottom_landing", (upper_x, 2.75, 3),
                      width_m=2.0, length_m=1.5, top_height_m=3,
                      supporting_platform=team + "_deck_3m")
        world.stairs(team + "_stairs_3_to_6", upper_x, 3.5, 2, 4.5, 3, base=3,
                     top_overlap=0.15, edge_bevel=0)
        world.platform(team + "_lookout_6m", (sign * 17, 9.5), (3.6, 3), 6)
        world.feature("landing", team + "_upper_stair_top_landing", (upper_x, 8.55, 6),
                      width_m=2.0, length_m=1.1, top_height_m=6,
                      stair_overlap_m=0.15, supporting_platform=team + "_lookout_6m")
        world.label("+3m", (sign * 14, 5.5, 3.025), 0.55, team)
        world.label("+6m", (sign * 17, 9.5, 6.025), 0.42, team)
        world.box(team + "_overlook_cover", (sign * 14, 7.5, 3.5), (3, 0.6, 1), team)
    world.platform("center_bridge_3m", (0, 5), (20, 3), 3, thickness=0.25)
    # Extend each end 0.2m into its lookout so both bevels retain a flat overlap.
    world.platform("sky_bridge_6m", (0, 9.5), (30.8, 2), 6, thickness=0.25)
    world.feature("bridge_connection", "sky_bridge_lookout_joins", (0, 9.5, 6),
                  end_overlap_m=0.2, width_m=2.0, top_height_m=6)
    world.ramp("central_ramp_16deg", 0, -7, 4, 10.5, 3)
    world.label("UPPER LOOP", (0, 5, 3.026), 0.36, "yellow")
    world.label("SKY BRIDGE", (0, 9.5, 6.025), 0.32, "yellow")
    # Tall obstacles are confined to separated lanes, leaving 2m+ side routes.
    for x, height in ((-7, 0.5), (-4, 1), (4, 1.5), (7, 2)):
        world.box("cover_" + str(height), (x, -4, height / 2), (2, 2, height))
        world.stripe("cover_height_band", (x, -5.014, height - 0.08), (1.8, 0.014, 0.06))
        world.label(f"{height:.1f}m COVER", (x, -5.7, 0.025), 0.24, "yellow")
        world.feature("cover", f"cover_{height:.1f}m", (x, -4, 0), height_m=height, size_m=[2, 2])
    world.gate("crouch_1_2m", -7.5, -9.5, 2, 1.2, depth=2)
    world.gate("crouch_1_5m", 7.5, -9.5, 2, 1.5, depth=2)
    for gap, x in ((1, -12), (2, -2), (3, 10)):
        world.gap("jump_" + str(gap) + "m", x, -16, gap)
    world.spawn("main", (0, -12.5, 0.04))
    world.ruler("south_distance_ruler", (-20, -12.5, 0.018), 40)
    # A separated 40m range, with shooter x=-20 and exact target distances.
    world.stripe("range_lane", (0, 16, 0.013), (46, 6, 0.014), "dark")
    world.outline("range_outline", (0, 16, 0.027), (45.9, 5.9), "edge")
    world.ruler("range_40m", (-20, 13.8, 0.029), 40, material="yellow")
    world.spawn("range", (-20, 16, 0.04), (1, 0, 0), "yellow")
    for distance in (10, 20, 30, 40):
        x = -20 + distance
        world.silhouette("target_" + str(distance) + "m", x, 16, yaw=-math.pi / 2)
        world.label(str(distance) + "m", (x, 17.5, 0.038), 0.38, "yellow")
        world.features[-1]["distance_from_range_spawn_m"] = distance
    world.board("FPS / ARENA 01", "48 x 40m  |  LOOP +3m / +6m  |  METERS", (0, -19.38, 1.3), 8, "blue")
    world.board("RANGE / 40m", "ORANGE TARGETS 1.8m  |  10 / 20 / 30 / 40m", (0, 19.35, 1.4), 9)
    world.presentation((52, -62, 57), aim=(0, 0, 2), orthographic=67)
    return world


def _village_hall(world, name, x, y, color):
    width = 8
    depth = 9
    world.box(name + "_foundation", (x, y, 0.12), (width + 0.4, depth + 0.4, 0.24), "stone")
    for sign in (-1, 1):
        world.box(name + "_side", (x + sign * (width / 2 - 0.2), y, 1.5), (0.4, depth, 3), color)
        world.box(name + "_entry_flank", (x + sign * 2.65, y - depth / 2 + 0.2, 1.5),
                  (2.3, 0.4, 3), color)
        for offset in (-depth / 2, depth / 2):
            world.box(name + "_corner_pier", (x + sign * (width / 2), y + offset, 1.55),
                      (0.65, 0.65, 3.1), "stone_light")
    world.box(name + "_entry_lintel", (x, y - depth / 2 + 0.2, 2.75), (3, 0.4, 0.5), "stone_light")
    world.box(name + "_rear", (x, y + depth / 2 - 0.2, 1.15), (width, 0.4, 2.3), color)
    world.platform(name + "_roof", (x, y), (width + 0.6, depth + 0.6), 3.25, "wood", thickness=0.25)
    stair_x = x + (3 if x > 0 else -3)
    world.stairs(name + "_roof_stairs", stair_x, y - depth / 2 - 6.5,
                 2.6, 6.5, 3.25, material="stone")
    world.feature("doorway", name + "_entry", (x, y - depth / 2, 0.24), clear_width_m=3.0,
                  clear_height_m=2.26, room_size_m=[7.2, 8.2])


def _rpg(materials):
    world = _World("rpg_grounds", "RPG_Grounds", (64, 64), materials)
    world.floor("earth")
    world.stripe("main_stone_path", (0, -7, 0.012), (6, 49, 0.014), "stone")
    world.stripe("cross_stone_path", (0, -4, 0.012), (58, 5, 0.014), "stone")
    world.stripe("courtyard_paving", (0, 3, 0.012), (19, 14, 0.014), "floor")
    world.outline("courtyard_meter_border", (0, 3, 0.028), (18.7, 13.7), "stone_light", 0.05)
    for sign in (-1, 1):
        world.box("edge_retaining_wall", (sign * 31.75, 0, 0.55), (0.5, 64, 1.1), "stone")
        world.box("end_retaining_wall", (0, sign * 31.75, 0.55), (63, 0.5, 1.1), "stone")
        for y in (-24, -12, 0, 12, 24):
            world.box("perimeter_pier", (sign * 31.4, y, 0.85), (0.9, 1, 1.7), "stone_light")
        # The low plinths sit outside the traversable central loop.
        world.box("moss_planter", (sign * 13, -13, 0.35), (3, 5, 0.7), "stone")
        world.stripe("moss_planter_surface", (sign * 13, -13, 0.705), (2.65, 4.65, 0.02), "grass")
        world.platform("courtyard_wallwalk", (sign * 10.5, 9), (3, 14), 3, "stone")
        for y in (4, 8, 12, 15):
            world.box("wallwalk_outer_merlon", (sign * 11.8, y, 3.4), (0.4, 1.2, 0.8), "stone_light")
        _village_hall(world, "west_hall" if sign < 0 else "east_hall", sign * 21.5, -1, "clay")
    world.platform("north_terrace_3m", (0, 13), (24, 6), 3, "stone")
    world.stairs("courtyard_stairs_0_to_3", -7, 4, 3, 6, 3, material="stone")
    world.ramp("courtyard_ramp_14deg", 7, -2, 4, 12, 3, material="stone")
    world.platform("tower_deck_6m", (0, 23), (10, 8), 6, "stone_light")
    world.stairs("tower_stairs_3_to_6", 0, 13, 3, 6, 3, base=3, material="stone")
    # Keep the tower arrival open; three tall faces create a distinct landmark.
    world.box("tower_rear_wall", (0, 26.7, 7.3), (10, 0.6, 2.6), "stone")
    for sign in (-1, 1):
        world.box("tower_side_wall", (sign * 4.7, 23, 7.2), (0.6, 8, 2.4), "stone")
        for y in (19.2, 26.6):
            world.box("tower_corner_buttress", (sign * 4.7, y, 7.6), (0.95, 0.95, 3.2), "stone_light")
    world.platform("east_gallery_bridge_3_25m", (17.35, 4), (8.3, 2.8), 3.25, "wood", thickness=0.25)
    # A short sideways ramp leaves the 3m wallwalk and meets the 3.25m gallery.
    world.ramp("gallery_roof_transition", 12, 4, 2.8, 1.2, 0.25, base=3,
               material="wood", yaw=-math.pi / 2)
    world.gate("main_courtyard_gate", 0, -17, 8, 3.3, "stone", depth=1.5)
    world.label("COURTYARD", (0, -10, 0.035), 0.65, "stone_light")
    world.label("+3m TERRACE", (0, 12, 3.03), 0.48, "dark")
    world.label("+6m LOOKOUT", (0, 23, 6.03), 0.48, "dark")
    world.spawn("main", (0, -23, 0.04), material="mint")
    world.spawn("courtyard", (0, 1, 0.04), material="mint")
    # Explicit upper spawn includes its floor elevation in all marker geometry.
    world.spawn("tower", (0, 22, 6.04), material="mint")
    world.ruler("approach_20m", (-3.8, -26, 0.025), 20, axis="Y", material="stone_light")
    world.ruler("cross_path_50m", (-25, -7, 0.025), 50, material="stone_light")
    # A south-west clearance laboratory, with roomy approach lanes.
    for x, width, height in ((-25, 0.8, 1.8), (-20, 1.0, 2.0), (-15, 1.2, 2.4)):
        world.gate(f"door_{width:.1f}_{height:.1f}", x, -19, width, height, "stone", depth=1.2)
    world.board("CLEARANCE COURT", "0.8 / 1.0 / 1.2m WIDTH  |  1.8 / 2.0 / 2.4m HEIGHT", (-20, -24, 1.45), 10)
    # South-east three jump lanes, with drops to a safe ground level.
    for gap, y in ((1, -16), (2, -21), (3, -26)):
        world.gap("rpg_jump_" + str(gap) + "m", 21, y, gap, height=1, material="stone", width=2.6)
    world.board("TRAVERSAL YARD", "1 / 2 / 3m GAPS  |  1m DECK  |  GROUND RECOVERY", (20, -30, 1.45), 10, "mint")
    # Low ruins and broken colonnades provide readable, varied silhouettes.
    for side in (-1, 1):
        for index, y in enumerate((13, 18, 23)):
            height = (2.8, 1.6, 3.8)[index]
            x = side * 23
            world.cylinder("ruined_column_foot", (x, y, 0.12), 0.7, 0.24, "stone_light", sides=12)
            world.cylinder("ruined_column", (x, y, height / 2), 0.36, height, "stone", sides=12)
            world.cylinder("ruined_capital", (x, y, height), 0.55, 0.22, "stone_light", sides=12)
        world.box("broken_ruin_wall", (side * 26.5, 18, 0.75), (1, 12, 1.5), "stone", yaw=side * 0.14)
        world.box("ruin_wall_fragment", (side * 27.5, 24, 1.3), (5, 0.8, 2.6), "stone")
        for xoffset in (-1, 1):
            world.box("ruin_block", (side * 20 + xoffset, 17, 0.3), (0.8, 0.9, 0.6), "stone_light", yaw=xoffset * 0.3)
    world.silhouette("landmark_target_west", -24, 9, material="blue")
    world.silhouette("landmark_target_east", 24, 9, material="red")
    world.board("RPG / GROUNDS 01", "64 x 64m  |  COURTYARD / VILLAGE / TOWER  |  METERS", (0, -30.8, 1.5), 12, "mint")
    world.presentation((78, -94, 84), aim=(0, 1, 2), orthographic=88)
    return world


def create_scenes(output_dir=None, export=True):
    if output_dir is None:
        source_path = globals().get("__file__")
        if not source_path:
            raise ValueError("Pass output_dir when executing without __file__")
        output_dir = Path(source_path).resolve().parents[2] / "assets" / "templates"
    """Build three owned scenes, export worlds/collision, write measurement manifest.

    Does not save the user's .blend or render. ``export=False`` performs authored
    scene creation and geometry checks only. Returns JSON-serializable evidence.
    Existing unrelated scenes, objects and data are never deleted.
    """
    if bpy.context.mode != "OBJECT":
        raise RuntimeError("Create default scenes in Object Mode; preserve active edit first")
    if bpy.context.window is None:
        raise RuntimeError("This entry point requires the connected Blender window")
    directory = Path(output_dir).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    manifest_path = directory / "measurements.json"
    expected_paths = [directory / (key + suffix + ".glb")
                      for key in ("blank", "fps_arena", "rpg_grounds")
                      for suffix in ("", "_collision")]
    if export and any(path.exists() for path in expected_paths):
        if not manifest_path.exists():
            raise RuntimeError("Existing unowned exports without measurement manifest")
        existing_manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if existing_manifest.get("generator") != GENERATOR:
            raise RuntimeError("Existing unowned measurement manifest")
    original_scene = bpy.context.window.scene
    original_scene_name = original_scene.name
    report = {"generator": GENERATOR, "unit": "meter", "authoring_up": "+Z",
              "export_up": "+Y", "coordinate_map": {
                  "blender_to_gltf": "[x, y, z] -> [x, z, -y]",
                  "gltf_to_blender": "[x, y, z] -> [x, -z, y]",
                  "blender_forward": "+Y", "gltf_forward": "-Z"},
              "collision": "Structural meshes only; no guides, labels, targets, or spawn geometry",
              "preserved_scene": original_scene_name, "scenes": []}
    try:
        materials = _palette()
        for builder in (_blank, _fps, _rpg):
            world = builder(materials)
            report["scenes"].append(world.export(directory, enabled=export))
        if export:
            manifest_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            report["manifest"] = str(manifest_path)
    finally:
        preserved = bpy.data.scenes.get(original_scene_name)
        if preserved is not None:
            bpy.context.window.scene = preserved
    return report
