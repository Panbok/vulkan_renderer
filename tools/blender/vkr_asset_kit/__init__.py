"""VKR Asset Kit: bake game-ready texture sets in Blender and send models to a
running VKR editor.

Install: zip this folder and use Blender Preferences > Add-ons > Install
from Disk, or link the folder into Blender's scripts/addons directory, then
enable "VKR Asset Kit". The panel is in the 3D Viewport sidebar, tab "VKR".
Headless: `Blender -b <file.blend> --python-expr` scripts import
`vkr_asset_kit.bake` and call `bake_asset` (see check_asset_kit.py).

Workflow:
1. Model the game mesh (any slots). Optionally model a high poly; without
   one, the bake rounds every edge with a Bevel shader.
2. Pick each material's smart kind (paint, metal, wood...) in Materials,
   or leave Auto (the name decides). Mark screens Keep.
3. Add details (screws, vents, seams, touch zones...) as empties placed on
   the surface: Add Detail drops one at the 3D cursor, Z along the surface
   normal.
4. Bake & Export writes <project>/<folder>/<name>/<name>.gltf with its own
   albedo, normal, ORM and emissive textures; Send to Editor also
   reimports every model of the open scene imported from that file, or
   places it at the 3D cursor when none is.
"""
import json
import os

import bpy
from bpy.props import BoolProperty, EnumProperty, FloatProperty, IntProperty, PointerProperty, StringProperty

from . import bake, link, smart

bl_info = {
    "name": "VKR Asset Kit",
    "author": "VKR",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "3D Viewport > Sidebar > VKR",
    "description": "Bake game-ready texture sets with smart wear and send models to the VKR editor",
    "category": "Import-Export",
}

KIND_ITEMS = [("AUTO", "Auto", "Pick by the material's name")] + [
    (k.upper(), k.capitalize(), f"Smart material: {k}") for k in smart.KINDS]

DETAIL_ITEMS = [
    ("screw", "Screw", "A screw head: radius, drive (phillips, slot, hex), finish"),
    ("rivet", "Rivet", "A domed rivet"),
    ("vent", "Vent", "Slots in a width x height panel"),
    ("perforation", "Perforation", "Round holes on a staggered grid"),
    ("ribs", "Ribs", "Parallel raised ribs (grips, treads)"),
    ("touch", "Touch", "Where hands wear the surface: smudged, polished"),
    ("wear", "Wear", "Scuffs and scratches (kick plates, edges)"),
    ("grime", "Grime", "Dirt collected in a sphere"),
    ("glow", "Glow", "A small emissive status light"),
]

DETAIL_DEFAULTS = {
    "screw": {"radius": 0.004, "drive": "phillips", "finish": "steel"},
    "rivet": {"radius": 0.005, "finish": "zinc"},
    "vent": {"width": 0.2, "height": 0.08, "slots": 6, "slot_width": 0.006},
    "perforation": {"width": 0.2, "height": 0.2, "pitch": 0.01, "hole_radius": 0.0025},
    "ribs": {"width": 0.05, "height": 0.1, "pitch": 0.004, "depth": 0.0006},
    "touch": {"radius": 0.1, "strength": 1.0},
    "wear": {"radius": 0.2, "strength": 1.0},
    "grime": {"radius": 0.15, "strength": 1.0},
    "glow": {"width": 0.004, "height": 0.004, "colour": [0.2, 1.0, 0.4], "strength": 4.0},
}


class VkrKitSettings(bpy.types.PropertyGroup):
    project: StringProperty(name="Project", subtype="DIR_PATH",
                            default="",
                            description="The VKR project folder (its assets/ tree is content), such as "
                                        "~/VKR Projects/<name>")
    folder: StringProperty(name="Folder", default="assets/models/props",
                           description="Content folder the model goes to, inside the project")
    asset_name: StringProperty(name="Name", default="",
                               description="File and material name; empty uses the object's name")
    high_poly: PointerProperty(name="High poly", type=bpy.types.Collection,
                               description="Collection of high-poly objects; empty bakes the game mesh with "
                                           "rounded edges")
    density: FloatProperty(name="Texel density", default=1024.0, min=64.0, max=4096.0,
                           description="Pixels per metre the atlas aims for")
    max_resolution: EnumProperty(name="Max atlas", default="2048",
                                 items=[(v, v, f"{v} pixels") for v in ("1024", "2048", "4096")])
    condition: FloatProperty(name="Condition", default=1.0, min=0.0, max=3.0,
                             description="Wear, grime and dust: 0 new, 1 used, 2 worn out")
    samples: IntProperty(name="Samples", default=24, min=4, max=512)
    bevel_radius: FloatProperty(name="Edge rounding", default=0.0025, min=0.0, max=0.05, unit="LENGTH",
                                description="Radius the bake rounds every edge by")
    lightmap_uvs: BoolProperty(name="Lightmap UVs", default=True,
                               description="Cook the reimported model with lightmap UVs for baked lighting")
    socket: StringProperty(name="Editor socket", default="",
                           description="The editor's agent socket; empty finds the default one")
    detail_kind: EnumProperty(name="Detail", items=DETAIL_ITEMS)
    last_report: StringProperty(default="")


def _settings(context):
    return context.scene.vkr_asset_kit


def _asset_name(context, obj):
    name = _settings(context).asset_name.strip() or obj.name
    return "".join(c if c.isalnum() or c in "_-" else "_" for c in name).lower()


def _content_path(context, obj):
    s = _settings(context)
    name = _asset_name(context, obj)
    return f"{s.folder.strip('/')}/{name}/{name}.gltf"


def detail_specs(obj):
    """The object's details: its `vkr_details` JSON (scripted builders) and
    every empty parented to it with a `vkr_detail` kind. An empty's Z axis
    is the surface normal, its Y axis up, its location the centre."""
    specs = list(json.loads(obj.get("vkr_details", "[]")))
    for child in obj.children_recursive:
        kind = child.get("vkr_detail")
        if child.type != "EMPTY" or not kind:
            continue
        m = child.matrix_world
        spec = {"kind": str(kind), "center": tuple(m.translation), "point": tuple(m.translation),
                "normal": tuple(m.to_3x3().col[2].normalized()), "up": tuple(m.to_3x3().col[1].normalized())}
        for key, value in DETAIL_DEFAULTS.get(str(kind), {}).items():
            raw = child.get(key, value)
            spec[key] = list(raw) if hasattr(raw, "__len__") and not isinstance(raw, str) else raw
        scale = max(child.matrix_world.to_scale())
        if "radius" in spec and "radius" not in child.keys():
            spec["radius"] = child.empty_display_size * scale
        specs.append(spec)
    return specs


def bake_object(context, obj, send=False):
    """Bakes a copy of `obj` (the scene's object stays as it is) and exports
    it into the project; with `send`, reimports or places it in the editor."""
    s = _settings(context)
    name = _asset_name(context, obj)
    kinds = {}
    keep = []
    for mat in obj.data.materials:
        if not mat:
            continue
        if mat.get("vkr_keep"):
            keep.append(mat.name)
        if mat.get("vkr_smart") and str(mat["vkr_smart"]).lower() in smart.KINDS:
            kinds[mat.name] = str(mat["vkr_smart"]).lower()
    if not s.project.strip():
        raise ValueError("Set Project in the VKR panel to the VKR project folder")
    project_root = bpy.path.abspath(s.project)
    # Screens and tinted glass stay kept by name; Keep adds the artist's own.
    keep = tuple(bake.Settings().keep) + tuple(keep)
    settings = bake.Settings(density=s.density, res_max=int(s.max_resolution), samples=s.samples,
                             bevel_radius=s.bevel_radius, condition=s.condition, keep=keep, kinds=kinds,
                             project_root=project_root)
    out_dir = os.path.join(project_root, s.folder.strip("/"))
    details = detail_specs(obj)
    work = obj.copy()
    work.data = obj.data.copy()
    for modifier in list(work.modifiers):
        work.modifiers.remove(modifier)
    context.scene.collection.objects.link(work)
    depsgraph = context.evaluated_depsgraph_get()
    evaluated = obj.evaluated_get(depsgraph)
    work.data = bpy.data.meshes.new_from_object(evaluated)
    work.matrix_world = obj.matrix_world.copy()
    hp_objects = [o for o in (s.high_poly.all_objects if s.high_poly else []) if o.type == "MESH"]
    try:
        report = bake.bake_asset(work, name, out_dir, settings, hp_objects=hp_objects, details=details)
    finally:
        bpy.data.objects.remove(work)
    s.last_report = json.dumps(report)
    if send:
        source = _content_path(context, obj)
        with link.EditorLink(s.socket or None) as editor:
            models = editor.call("model.list", {"origin": source}).get("models", [])
            models = [m for m in models if m.get("origin") == source]
            if models:
                for model in models:
                    link.reimport(editor, model["asset"], source, s.lightmap_uvs)
                report["reimported"] = len(models)
            else:
                cursor = context.scene.cursor.location
                # Blender Z up to VKR Y up.
                editor.call("model.place", {"models": [{"source": source, "name": name,
                                                        "position": [cursor.x, cursor.z, -cursor.y]}],
                                            "lightmap_uvs": s.lightmap_uvs})
                editor.wait_jobs()
                report["placed"] = 1
        s.last_report = json.dumps(report)
    return report


class VKR_OT_bake_export(bpy.types.Operator):
    bl_idname = "vkr.bake_export"
    bl_label = "Bake & Export"
    bl_description = ("Bake the active mesh's texture sets from its high poly with smart wear and details, and "
                      "export the glTF into the project")
    send: BoolProperty(default=False, options={"SKIP_SAVE"})

    @classmethod
    def poll(cls, context):
        return context.active_object is not None and context.active_object.type == "MESH"

    def execute(self, context):
        obj = context.active_object
        mode = obj.mode
        if mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        try:
            report = bake_object(context, obj, send=self.send)
        except (ConnectionError, RuntimeError, OSError, ValueError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        finally:
            context.view_layer.objects.active = obj
            obj.select_set(True)
        sets = ", ".join(f"{t['resolution']}px {t['density']}px/m" for t in report["sets"])
        verb = (f"reimported {report['reimported']}" if report.get("reimported") else
                "placed in the editor" if report.get("placed") else "exported")
        self.report({"INFO"}, f"{report['name']}: {report['triangles']} tris, {sets}, {verb} "
                              f"({report['seconds']} s)")
        return {"FINISHED"}


class VKR_OT_add_detail(bpy.types.Operator):
    bl_idname = "vkr.add_detail"
    bl_label = "Add Detail"
    bl_description = ("Add a detail empty at the 3D cursor, parented to the active mesh: Z along the surface "
                      "normal (snap the cursor to the surface with Shift+right click, orientation Geometry)")
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        return context.active_object is not None and context.active_object.type == "MESH"

    def execute(self, context):
        s = _settings(context)
        target = context.active_object
        kind = s.detail_kind
        empty = bpy.data.objects.new(f"vkr_{kind}", None)
        empty.empty_display_type = "CIRCLE" if kind in ("screw", "rivet", "glow") else (
            "SPHERE" if kind in ("touch", "wear", "grime") else "CUBE")
        defaults = DETAIL_DEFAULTS[kind]
        empty.empty_display_size = defaults.get("radius", max(defaults.get("width", 0.05),
                                                               defaults.get("height", 0.05)) / 2)
        empty["vkr_detail"] = kind
        for key, value in defaults.items():
            if key != "radius":
                empty[key] = value
        context.scene.collection.objects.link(empty)
        empty.matrix_world = context.scene.cursor.matrix.copy()
        # The circle empty draws in its XY plane: the surface's.
        empty.parent = target
        empty.matrix_parent_inverse = target.matrix_world.inverted()
        for o in context.selected_objects:
            o.select_set(False)
        empty.select_set(True)
        context.view_layer.objects.active = empty
        return {"FINISHED"}


class VKR_PT_asset_kit(bpy.types.Panel):
    bl_label = "VKR Asset Kit"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "VKR"

    def draw(self, context):
        s = _settings(context)
        layout = self.layout
        obj = context.active_object
        col = layout.column(align=True)
        col.prop(s, "project")
        col.prop(s, "folder")
        col.prop(s, "asset_name", placeholder=obj.name if obj else "")
        if obj and obj.type == "MESH":
            layout.label(text=_content_path(context, obj), icon="FILE_3D")
        col = layout.column(align=True)
        col.prop(s, "high_poly")
        col.prop(s, "density")
        col.prop(s, "max_resolution")
        col.prop(s, "condition", slider=True)
        col.prop(s, "bevel_radius")
        col.prop(s, "samples")
        col = layout.column(align=True)
        col.prop(s, "lightmap_uvs")
        col.prop(s, "socket")
        row = layout.row(align=True)
        row.scale_y = 1.4
        row.operator(VKR_OT_bake_export.bl_idname, icon="RENDER_STILL").send = False
        op = row.operator(VKR_OT_bake_export.bl_idname, text="Send to Editor", icon="EXPORT")
        op.send = True
        if s.last_report:
            try:
                report = json.loads(s.last_report)
                box = layout.box()
                box.label(text=f"{report['name']}: {report['triangles']} triangles, {report['seconds']} s")
                for item in report["sets"]:
                    box.label(text=f"{item['resolution']} px, {item['density']} px/m: {', '.join(item['slots'])}")
            except (ValueError, KeyError):
                pass


class VKR_PT_asset_kit_materials(bpy.types.Panel):
    bl_label = "Materials"
    bl_parent_id = "VKR_PT_asset_kit"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "VKR"

    @classmethod
    def poll(cls, context):
        return context.active_object is not None and context.active_object.type == "MESH"

    def draw(self, context):
        layout = self.layout
        for slot in context.active_object.material_slots:
            mat = slot.material
            if not mat:
                continue
            row = layout.row(align=True)
            row.label(text=mat.name, icon="MATERIAL")
            row.prop(mat, "vkr_smart_kind", text="")
            row.prop(mat, "vkr_keep_slot", text="", icon="IMAGE_DATA")


class VKR_PT_asset_kit_details(bpy.types.Panel):
    bl_label = "Details"
    bl_parent_id = "VKR_PT_asset_kit"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "VKR"

    def draw(self, context):
        s = _settings(context)
        layout = self.layout
        row = layout.row(align=True)
        row.prop(s, "detail_kind", text="")
        row.operator(VKR_OT_add_detail.bl_idname, icon="EMPTY_AXIS")
        obj = context.active_object
        target = obj.parent if obj and obj.get("vkr_detail") else obj
        if not target or target.type != "MESH":
            return
        empties = [c for c in target.children_recursive if c.type == "EMPTY" and c.get("vkr_detail")]
        scripted = len(json.loads(target.get("vkr_details", "[]")))
        layout.label(text=f"{len(empties)} detail empties, {scripted} scripted details")
        if obj and obj.get("vkr_detail"):
            box = layout.box()
            box.label(text=f"{obj.name} ({obj['vkr_detail']})")
            box.prop(obj, "empty_display_size", text="Size / radius")
            for key in DETAIL_DEFAULTS.get(str(obj["vkr_detail"]), {}):
                if key in obj.keys():
                    box.prop(obj, f'["{key}"]', text=key.replace("_", " ").capitalize())


def _kind_get(mat):
    value = str(mat.get("vkr_smart", "")).upper()
    names = [item[0] for item in KIND_ITEMS]
    return names.index(value) if value in names else 0


def _kind_set(mat, index):
    value = KIND_ITEMS[index][0]
    if value == "AUTO":
        if "vkr_smart" in mat.keys():
            del mat["vkr_smart"]
    else:
        mat["vkr_smart"] = value.lower()


CLASSES = (VkrKitSettings, VKR_OT_bake_export, VKR_OT_add_detail, VKR_PT_asset_kit, VKR_PT_asset_kit_materials,
           VKR_PT_asset_kit_details)


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.vkr_asset_kit = PointerProperty(type=VkrKitSettings)
    bpy.types.Material.vkr_smart_kind = EnumProperty(name="Smart kind", items=KIND_ITEMS, get=_kind_get,
                                                     set=_kind_set,
                                                     description="The wear, grime and dust recipe the bake layers")
    bpy.types.Material.vkr_keep_slot = BoolProperty(
        name="Keep", get=lambda m: bool(m.get("vkr_keep", False)),
        set=lambda m, v: m.__setitem__("vkr_keep", bool(v)),
        description="Keep this material and its UVs (a screen whose image each placement picks)")


def unregister():
    del bpy.types.Material.vkr_keep_slot
    del bpy.types.Material.vkr_smart_kind
    del bpy.types.Scene.vkr_asset_kit
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)
