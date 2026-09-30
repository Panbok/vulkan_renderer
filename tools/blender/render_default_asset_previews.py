"""Render authored default assets in Blender, without changing export content.

Run after create_default_scenes.py in the same
Blender file. Images are visual review evidence, not renderer performance data.
For live MCP execution without __file__, pass output_dir explicitly.
"""

import bpy
from pathlib import Path


def render_scene(scene_name, output, resolution=(1280, 800)):
    scene = bpy.data.scenes[scene_name]
    assert scene.camera, f'{scene_name} needs an authored overview camera'
    bpy.context.window.scene = scene
    scene.render.engine = 'BLENDER_EEVEE'
    scene.render.resolution_x, scene.render.resolution_y = resolution
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'PNG'
    scene.render.filepath = str(output)
    bpy.ops.render.render(write_still=True)
    return str(output)


def render_overviews(output_dir=None):
    if output_dir is None:
        source_path = globals().get('__file__')
        if not source_path:
            raise ValueError('Pass output_dir when executing without __file__')
        output_dir = Path(source_path).resolve().parents[2] / 'assets' / 'templates' / 'previews'
    directory = Path(output_dir)
    directory.mkdir(parents=True, exist_ok=True)
    results = []
    for name, filename in [('VKR_Template_Blank', 'blank.png'),
                           ('VKR_Template_FPS_Arena', 'fps_arena.png'),
                           ('VKR_Template_RPG_Grounds', 'rpg_grounds.png')]:
        results.append(render_scene(name, directory/filename))
    return results
