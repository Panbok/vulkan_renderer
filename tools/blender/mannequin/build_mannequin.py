"""Build VKR's default mannequin character and export it as glTF.

Run headless from the repository root:

    Blender --background --factory-startup \\
        --python tools/blender/mannequin/build_mannequin.py -- \\
        --sources <dir> --out <dir>

`--sources` holds the pinned third-party inputs listed in sources.json
(fetched by fetch_sources.py); `--out` receives mannequin.gltf with its
buffer and textures/. Metres, Blender Z up and facing +Y, exported as glTF
Y up facing -Z (the engine's actor forward). `--clips none` skips the
animations for look development.
"""

import argparse
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import bpy  # noqa: E402
import numpy as np  # noqa: E402

import mannequin_bake as mbk  # noqa: E402
import mannequin_base as mb  # noqa: E402
import mannequin_blender as mbl  # noqa: E402
import mannequin_clips as mc  # noqa: E402
import mannequin_design as md  # noqa: E402
import mannequin_rig as mr  # noqa: E402


def _arguments():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sources", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--texture-size", type=int, default=2048)
    parser.add_argument("--detail-levels", type=int, default=2,
                        help="subdivision levels of the baked detail mesh")
    parser.add_argument("--blend", action="store_true", help="also save mannequin.blend")
    parser.add_argument("--clips", default="", help="comma-separated clip names, 'none', or all by default")
    return parser.parse_args(argv)


def _frame_cuts(path):
    """100STYLE Frame_Cuts.csv: take stem to (start, stop) frame indices."""
    cuts = {}
    lines = Path(path).read_text(encoding="utf-8").splitlines()
    header = lines[0].split(",")
    for line in lines[1:]:
        fields = line.split(",")
        for column, name in enumerate(header):
            if name.endswith("_START") and fields[column] != "N/A":
                kind = name[:-6]
                cuts[f"{fields[0]}_{kind}"] = (int(fields[column]), int(fields[column + 1]))
    return cuts


def main():
    args = _arguments()
    started = time.time()
    args.out.mkdir(parents=True, exist_ok=True)
    mbl.reset_scene()
    body = mb.build(args.sources)
    rig = mr.Rig(body)
    mesh = mbl.create_mesh(body)
    mbl.pack_uvs(mesh)
    armature = mbl.create_armature(rig)
    names, _, _ = mbl.skin(mesh, armature, rig, body)
    print(f"body {len(body.positions)} vertices, {len(body.faces)} faces, "
          f"{len(names)} deform bones ({time.time() - started:.1f} s)")

    # Weights come from the 30 k-vertex base; the game mesh is one level
    # finer so plates keep real steps; the detail mesh adds the fine design.
    mbl.subdivide(mesh, 1)
    design = md.layout(rig)
    source = bpy.data.objects.new("SK_Mannequin_Source", mesh.data.copy())
    bpy.context.scene.collection.objects.link(source)
    mbk.displace(mesh, design, md.BASE_TONE)
    details = mbk.detail_meshes(source, design, args.detail_levels, md.BASE_TONE)
    cage = mbk.cage(source)
    bpy.data.objects.remove(source, do_unlink=True)
    print(f"game {len(mesh.data.vertices)} vertices, detail "
          f"{sum(len(obj.data.vertices) for obj in details)} vertices ({time.time() - started:.1f} s)")
    baked = mbk.bake(mesh, details, cage, args.texture_size)
    bpy.data.objects.remove(cage, do_unlink=True)
    base, orm, normal = mbk.compose(baked)
    textures_dir = args.out / "textures"
    textures_dir.mkdir(exist_ok=True)
    textures = {"base": textures_dir / "mannequin_basecolor.png",
                "orm": textures_dir / "mannequin_orm.png",
                "normal": textures_dir / "mannequin_normal.png"}
    mbk.save_png(base, textures["base"], srgb=True)
    mbk.save_png(orm, textures["orm"], srgb=False)
    mbk.save_png(normal, textures["normal"], srgb=False)
    for obj in details:
        bpy.data.objects.remove(obj, do_unlink=True)
    mbl.textured_material(mesh, textures)
    print(f"baked {args.texture_size}px textures ({time.time() - started:.1f} s)")

    if args.clips == "none":
        _export(args, mesh, started)
        return
    target = mc.target_from_armature(armature)
    cuts = _frame_cuts(args.sources / "100style" / "Frame_Cuts.csv")
    wanted = {name for name in args.clips.split(",") if name}
    for spec in mc.CLIPS:
        if wanted and spec.name not in wanted:
            continue
        clip_started = time.time()
        result = mc.build_clip(spec, args.sources / "100style", cuts, target, args.sources / "cmu")
        action = mc.add_action(armature, spec.name, target, result["quaternions"], result["translation"],
                               result["clip"].loop)
        action["vkr_speed"] = float(result["speed"])
        action["vkr_travel"] = [float(v) for v in result["clip"].travel[:2]]
        airborne = result["clip"].airborne
        action["vkr_airborne"] = [int(f) for f in np.flatnonzero(airborne)] if airborne is not None else []
        action["vkr_window"] = [int(f) for f in result["window"]]
        action["vkr_source"] = result["source"]
        print(f"clip {spec.name}: {result['source']} frames {len(result['quaternions'])}, window {result['window']}, "
              f"speed {result['speed']:.3f} m/s ({time.time() - clip_started:.1f} s)")

    _export(args, mesh, started)


def _export(args, mesh, started):
    # Clip metadata for controllers: loop speed and direction in the glTF
    # frame (Y up, -Z forward), where Blender (x, y) maps to glTF (x, -z).
    clips = []
    for action in bpy.data.actions:
        travel = action.get("vkr_travel", [0.0, 1.0])
        start, end = action.frame_range
        clips.append({"name": action.name,
                      "loop": bool(action.get("vkr_loop", True)),
                      "duration": round((end - start) / bpy.context.scene.render.fps, 6),
                      "speed": round(float(action.get("vkr_speed", 0.0)), 4),
                      "travel": [round(float(travel[0]), 4), round(-float(travel[1]), 4)],
                      "source": action.get("vkr_source", "")})
    clips.sort(key=lambda clip: clip["name"])
    (args.out / "mannequin.clips.json").write_text(
        json.dumps({"version": 1, "fps": bpy.context.scene.render.fps, "clips": clips}, indent=2) + "\n",
        encoding="utf-8")
    if args.blend:
        bpy.ops.wm.save_as_mainfile(filepath=str(args.out / "mannequin.blend"))
    mbl.export_gltf(args.out / "mannequin.gltf")
    print(f"exported {args.out / 'mannequin.gltf'} ({time.time() - started:.1f} s, "
          f"{len(mesh.data.vertices)} vertices)")


main()
