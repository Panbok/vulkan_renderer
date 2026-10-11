#!/usr/bin/env python3
"""Stage one editor project scene's live files into a depot working copy.

Copies the project file and the project's World and game documents
(`world.scene.json`, `world.editor.json`, `game.json`, whichever exist), the
scene file, its live edit overlay and inventory, the builds that inventory
names, and every repository asset the overlays reach through meshes and
materials that git does not track, at workspace-relative paths. Without the
World, the receiving editor makes a default one and the scene renders under
another sun, sky and grading. Older overlays and builds in the scene folder are
left out. On macOS each copy is an APFS clone (`cp -c`); elsewhere a plain
copy.

    python3 -I stage_scene.py --project ID --scene ID WORK [--workspace DIR]
                              [--dry-run]

WORK must already be a depot working copy (`vkr_depot clone`). WORKSPACE is
the editor's `--workspace` and the repository root (default: the current
directory). Exits 1 when a referenced asset is missing.
"""

import argparse
import json
import pathlib
import platform
import re
import shutil
import subprocess
import sys

ASSET_PATH = re.compile(
    rb"assets/[A-Za-z0-9_./\-]+?\.(?:vkrhf|vkb|vkt|gltf|bin|mtg|mt|png)")


def normalize(path: str) -> str:
    parts = []
    for part in path.split("/"):
        if part == "..":
            if parts:
                parts.pop()
        elif part not in ("", "."):
            parts.append(part)
    return "/".join(parts)


def references(root: pathlib.Path, location: pathlib.Path) -> set:
    """`assets/...` paths a file names; a material's relative texture paths
    are resolved against its folder. Cooked `.vkt` files are left out: they
    are per platform, and `.vkrdepotignore` drops them."""
    data = (root / location).read_bytes()
    found = {normalize(m.decode()) for m in ASSET_PATH.findall(data)}
    if location.suffix == ".mt":
        for line in data.decode(errors="replace").splitlines():
            key, _, value = line.partition("=")
            if not key.endswith("_texture") or not value:
                continue
            relative = value.split("?", 1)[0]
            if relative.startswith("assets/"):
                found.add(normalize(relative))
            else:
                found.add(normalize((location.parent / relative).as_posix()))
    return {path for path in found if not path.endswith(".vkt")}


def asset_location(root: pathlib.Path, path: str):
    """An `assets/...` path resolves against the repository first, then the
    workspace's `.vkreditor/` (terrain heightfields live there)."""
    for candidate in (pathlib.Path(path), pathlib.Path(".vkreditor") / path):
        if (root / candidate).is_file():
            return candidate
    return None


def git_tracked(root: pathlib.Path, paths: list) -> set:
    if not paths:
        return set()
    result = subprocess.run(["git", "ls-files", "-z", "--", *paths], cwd=root,
                            capture_output=True, check=True)
    return {p for p in result.stdout.decode().split("\0") if p}


def copy(source: pathlib.Path, target: pathlib.Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    if platform.system() == "Darwin":
        subprocess.run(["cp", "-c", str(source), str(target)], check=True)
    else:
        shutil.copy2(source, target)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("work")
    parser.add_argument("--project", required=True)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--workspace", default=".")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    root = pathlib.Path(args.workspace).resolve()
    work = pathlib.Path(args.work).expanduser().resolve()
    if not (work / ".vkrdepot").is_dir():
        print(f"{work} is not a depot working copy")
        return 1
    project = pathlib.Path(".vkreditor/projects") / args.project
    scene_dir = project / "scenes" / args.scene
    scene_file = root / scene_dir / "scene.json"
    if not scene_file.is_file():
        print(f"no scene file at {scene_file}")
        return 1

    scene = json.loads(scene_file.read_text())
    files = [project / "project.json", scene_dir / "scene.json"]
    # Project documents every scene of the project renders under.
    documents = [project / name for name in
                 ("world.scene.json", "world.editor.json", "game.json")
                 if (root / project / name).is_file()]
    files.extend(documents)
    overlay = scene.get("edit_overlay")
    if overlay:
        files.append(scene_dir / overlay)
    if scene.get("inventory"):
        files.append(scene_dir / scene["inventory"])
        inventory = json.loads((root / scene_dir / scene["inventory"]).read_text())
        for asset in inventory.get("assets", []):
            for artifact in asset.get("artifacts", []):
                files.append(scene_dir / artifact["path"])

    seen = set()
    pending = list(references(root, scene_dir / overlay)) if overlay else []
    for document in documents:
        pending.extend(references(root, document))
    missing = []
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        location = asset_location(root, path)
        if location is None:
            missing.append(path)
            continue
        files.append(location)
        if location.suffix in (".vkb", ".gltf", ".mt", ".mtg"):
            pending.extend(references(root, location) - seen)

    tracked = git_tracked(root, [f.as_posix() for f in files
                                 if f.parts[0] == "assets"])
    files = sorted({f for f in files if f.as_posix() not in tracked})
    total = 0
    for f in files:
        if not (root / f).is_file():
            missing.append(f.as_posix())
            continue
        size = (root / f).stat().st_size
        total += size
        print(f"{size:>14,}  {f.as_posix()}")
        if not args.dry_run:
            copy(root / f, work / f)
    print(f"{len(files)} files, {total / 1e6:.1f} MB; "
          f"{len(tracked)} tracked assets left to git")
    for path in sorted(missing):
        print(f"missing: {path}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
