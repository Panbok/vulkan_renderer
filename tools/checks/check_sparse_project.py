#!/usr/bin/env python3
"""End-to-end check of sparse project sharing (ADR-123): a Get that makes a
sparse working copy, its project opened in the editor with Bakery jobs that
read the copy's unfetched files, and a commit and sync.

1. The repository workspace's project that holds a scene named Bistro is
   cloned (APFS, copy on write) into a scratch workspace with Bistro as its
   only scene, and Bakery's `share_project` copies it with every file that
   scene uses into a shared folder, as the editor's Share does.
2. The shared folder becomes a working copy of a loopback depot: `main`
   holds the project and `main.cooked.<family>` its cooked textures.
3. Two headless editors open the shared project and then Bistro by name,
   one after the other, and capture one view `--settle` seconds after the
   scene is ready.
4. A second headless editor, with a home of its own, starts on the shared
   project with no scene open (its Cmd queue runs once a project is open),
   Gets the project with `depot.clone` and `sparse`, opens Bistro by name,
   so its Bakery preparation and its loads read files as they arrive,
   captures the same view after the same time, creates and saves one
   object and commits and syncs.

The check passes when the sparse run showed the scene with cooked textures
fetched from the side branch and first drawn from previews, some tracked
files stayed unfetched, the
The commit carried changes to Bistro alone, left no local change and added only
a few objects to the server, a second sparse clone lists the new commit's
files, and the sparse capture differs from the first local one by at most
1.5 times what the two local ones differ by, with a floor of 0.5 levels of
255.

    python tools/checks/check_sparse_project.py \\
        --editor build_release/editor/vkr_editor \\
        --depot build_release/tools/depot/vkr_depot \\
        --bakery build_release/tools/bakery/vkr_bakery
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

from check_asset_stream import (LOCAL_POLL, ROOT, VIEW, captures, cmd_values,
                                compare, depot, stream_statuses)
from check_editor_session import editor_env

# Dynamic resolution follows recent GPU frame times, which the sparse run's
# texture reloads raise; a fixed render scale keeps the captures comparable.
LOCAL_SCRIPT = f"""wait 5
gfx.dynamic = false
scene.open {{scene}}
wait.scene
stats.pending_textures
{{polls}}
op view.capture {{{{{VIEW}}}}}
wait 3
echo capture-done
"""

SPARSE_SCRIPT = f"""wait 5
op depot.clone {{{{"address":"{{address}}","key":"{{key}}","name":"{{name}}","destination":"{{destination}}","sparse":true}}}}
wait 30
op depot.status
gfx.dynamic = false
scene.open {{scene}}
wait.scene
op stream.status
stats.pending_textures
{{polls}}
op stream.status
op view.capture {{{{{VIEW}}}}}
wait 3
create cube
sel.name = "SparseCube"
scene.save
wait 2
op depot.commit {{{{"message":"sparse edit","sync":true}}}}
wait 30
op depot.status
op stream.status
echo capture-done
"""
SPARSE_POLL = "wait 2\nop stream.status\nstats.pending_textures"


def bistro_project(workspace: pathlib.Path):
    """(id, project folder, Bistro scene id) of the workspace's project that
    holds a scene named Bistro."""
    for manifest in sorted((workspace / ".vkreditor" / "projects").glob(
            "*/project.json")):
        data = json.loads(manifest.read_text(encoding="utf-8"))
        for scene in data.get("scenes", []):
            if scene.get("name") == "Bistro":
                scene_id = pathlib.PurePosixPath(scene["path"]).parts[1]
                return manifest.parent.name, manifest.parent, scene_id
    raise RuntimeError(f"no project of the workspace {workspace} has a scene "
                       "named Bistro")


def run_editor(editor: str, work: pathlib.Path, script: str,
               log: pathlib.Path, args: list, timeout: float) -> str:
    work.mkdir(parents=True, exist_ok=True)
    env = editor_env(work, autoclose=int(timeout))
    with open(log, "w", encoding="utf-8", errors="replace") as out:
        process = subprocess.Popen(
            [editor, "--headless", "--no-agent-socket", *args, "--exec",
             script], cwd=ROOT, env=env, stdout=out, stderr=subprocess.STDOUT)
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    return log.read_text(encoding="utf-8", errors="replace")


def disk_files(root: pathlib.Path) -> list:
    """Files below `root` outside its `.vkrdepot` state, '/'-separated."""
    return sorted(p.relative_to(root).as_posix() for p in root.rglob("*")
                  if p.is_file() and
                  not p.relative_to(root).as_posix().startswith(".vkrdepot/"))


def ignore_rules(path: pathlib.Path) -> list:
    """The patterns of a `.vkrdepotignore` file."""
    return [line.strip() for line in path.read_text(encoding="utf-8")
            .splitlines() if line.strip() and not line.startswith("#")]


def ignored(path: str, rules: list) -> bool:
    """Whether a '/'-separated path matches one of the rules: `dir/` as a
    folder anywhere (or the root's when it holds a '/'), `*.ext` as a name's
    end, anything else as a whole path."""
    parts = path.split("/")
    for rule in rules:
        if rule.endswith("/"):
            folder = rule[:-1]
            if "/" in folder:
                if path.startswith(rule):
                    return True
            elif folder in parts[:-1]:
                return True
        elif rule.startswith("*"):
            if parts[-1].endswith(rule[1:]):
                return True
        elif path == rule:
            return True
    return False


def tree_bytes(root: pathlib.Path) -> int:
    return sum(p.stat().st_size for p in root.rglob("*") if p.is_file())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--depot", required=True)
    parser.add_argument("--bakery", required=True)
    parser.add_argument("--port", type=int, default=7358)
    parser.add_argument("--settle", type=int, default=60,
                        help="seconds each run waits after its scene is "
                        "ready before its capture")
    # Outside the repository: a share refuses a destination inside it.
    parser.add_argument("--work", default=str(
        pathlib.Path(tempfile.gettempdir()) / "vkr-sparse-project"))
    # Projects live in their own folders (ADR-110): the workspace that
    # holds the Bistro project, such as `~/VKR Projects/Testbed`.
    parser.add_argument("--workspace", default=str(ROOT),
                        help="workspace folder whose project has a Bistro "
                        "scene (default: the repository's own)")
    parser.add_argument("--timeout", type=float, default=1200.0)
    parser.add_argument("--keep", action="store_true")
    args = parser.parse_args()
    editor = str(pathlib.Path(args.editor).resolve())
    depot_exe = str(pathlib.Path(args.depot).resolve())
    bakery = str(pathlib.Path(args.bakery).resolve())
    work = pathlib.Path(args.work).resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    failures = []
    server = None
    made = []
    log = open(work / "depot.log", "w", encoding="utf-8")
    env = dict(os.environ)
    env["VKR_DEPOT_IDENTITY"] = str(work / "identity.key")
    try:
        # 1. A workspace whose project holds Bistro alone, then its share.
        project_id, project_dir, scene_id = bistro_project(
            pathlib.Path(args.workspace).resolve())
        source = work / "source"
        projects = source / ".vkreditor" / "projects"
        projects.mkdir(parents=True)
        subprocess.run(["cp", "-c", "-R", str(project_dir),
                        str(projects / project_id)], check=True)
        manifest = projects / project_id / "project.json"
        data = json.loads(manifest.read_text(encoding="utf-8"))
        for scene in data.get("scenes", []):
            if scene.get("name") != "Bistro":
                folder = projects / project_id / pathlib.PurePosixPath(
                    scene["path"]).parent
                shutil.rmtree(folder, ignore_errors=True)
        data["scenes"] = [s for s in data.get("scenes", [])
                          if s.get("name") == "Bistro"]
        manifest.write_text(json.dumps(data, indent=2), encoding="utf-8")
        # Clouds drift with the time since the editor started, which the
        # sparse run's Get and streamed load lengthen; still clouds keep
        # the captures comparable.
        world = projects / project_id / "world.scene.json"
        if world.exists():
            document = json.loads(world.read_text(encoding="utf-8"))
            for entity in document.get("entities", []):
                clouds = entity.get("components", {}).get("clouds")
                if clouds is not None:
                    clouds["wind_mps"] = [0, 0]
            world.write_text(json.dumps(document, indent=2),
                             encoding="utf-8")
        shared = work / "shared"
        request = work / "share.request.json"
        result = work / "share.result.json"
        request.write_text(json.dumps({
            "version": 1, "read_only": False,
            "workspace_root": str(source / ".vkreditor"),
            "operation": "share_project", "project_path": str(manifest),
            "destination": str(shared),
            "legacy_root": str(ROOT) + os.sep}), encoding="utf-8")
        shared_run = subprocess.run(
            [bakery, "project", "--request", str(request), "--result",
             str(result)], cwd=ROOT, env=env, capture_output=True, text=True)
        log.write(shared_run.stdout + shared_run.stderr)
        if shared_run.returncode != 0:
            raise RuntimeError(f"share_project failed: {result.read_text()}"
                               if result.exists() else "share_project failed")

        # 2. The depot.
        depot([depot_exe, "keygen"], env, log)
        store = work / "store"
        output = depot([depot_exe, "init", str(store)], env, log)
        key = re.search(r"server key ([0-9a-f]{64})", output).group(1)
        address = f"127.0.0.1:{args.port}"
        server = subprocess.Popen(
            [depot_exe, "serve", str(store), "--bind", address, "--open"],
            env=env, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
        time.sleep(1.0)
        depot([depot_exe, "clone", address, key, str(shared)], env, log)
        depot([depot_exe, "commit", str(shared), "-m", "Bistro project",
               "--push"], env, log)
        published = depot([depot_exe, "cooked", str(shared)], env, log)
        tracked = len(disk_files(shared))
        store_before = tree_bytes(store)

        # 3. The local run, from the shared folder.
        # Each run opens Bistro by name after the project, as the sparse
        # run does, so the view adapts from the same start.
        local_script = LOCAL_SCRIPT.format(
            scene="Bistro", polls="\n".join([LOCAL_POLL] * args.settle))
        local_text = run_editor(
            editor, work / "local", local_script, work / "local.log",
            ["--workspace", str(shared), "--project", project_id],
            args.timeout)
        again_text = run_editor(
            editor, work / "again", local_script, work / "again.log",
            ["--workspace", str(shared), "--project", project_id],
            args.timeout)
        local_captures = captures(local_text)
        again_captures = captures(again_text)
        made += local_captures + again_captures
        if not local_captures or not again_captures:
            raise RuntimeError(f"a local run did not capture; see {work}")

        # 4. The sparse Get, Bistro opened from it, an edit, a commit, a sync.
        sparse = work / "sparse"
        script = SPARSE_SCRIPT.format(
            address=address, key=key, name=data.get("name", "Project"),
            destination=str(sparse), scene="Bistro",
            polls="\n".join([SPARSE_POLL] * (args.settle // 2)))
        # The Cmd queue runs once a project is open, so the shared project
        # holds the editor, with no scene open, until the Get switches to
        # the sparse copy. Bistro stays closed until then, since materials
        # are cached by file name and a second open would reuse the
        # first's.
        sparse_text = run_editor(
            editor, work / "sparse-run", script, work / "sparse.log",
            ["--workspace", str(shared), "--project", project_id],
            args.timeout)
        sparse_captures = captures(sparse_text)
        made += sparse_captures
        statuses = [s for s in stream_statuses(sparse_text) if s.get("sparse")]
        if "capture-done" not in sparse_text or not sparse_captures:
            raise RuntimeError("the sparse run did not capture; see "
                               f"{work / 'sparse.log'}")
        if not statuses:
            raise RuntimeError("the sparse run had no sparse working copy; "
                               f"see {work / 'sparse.log'}")
        status = depot([depot_exe, "status", str(sparse)], env, log)
        counts = re.search(r"(\d+) changes on \S+ at \w+; (\d+) of (\d+) "
                           r"files not fetched", status)
        if not counts:
            raise RuntimeError(f"unexpected status: {status.strip()}")
        changes, unfetched, listed = (int(counts.group(i)) for i in (1, 2, 3))
        if changes:
            failures.append(f"{changes} local changes remain after the "
                            "commit and sync")
        on_disk = len(disk_files(sparse))
        if not unfetched:
            failures.append(f"every one of {listed} tracked files was "
                            "fetched")
        cooked = [p for p in disk_files(sparse) if p.endswith(".vkt")]
        if not cooked:
            failures.append("the sparse Bistro fetched no cooked texture")
        # What the commit carried: with no change left, every tracked file
        # on disk is the new commit's, so those that differ from the shared
        # folder's are the commit's; they must be the edit to Bistro alone.
        rules = ignore_rules(shared / ".vkrdepotignore")
        submitted = sorted(
            path for path in disk_files(sparse)
            if not ignored(path, rules) and
            (not (shared / path).exists() or
             (shared / path).read_bytes() != (sparse / path).read_bytes()))
        scene_folder = f"/scenes/{scene_id}/"
        if not submitted:
            failures.append("the commit carried no change")
        strays = [path for path in submitted if scene_folder not in path]
        if strays:
            failures.append(f"the commit carried changes outside Bistro: "
                            f"{strays}")
        added = tree_bytes(store) - store_before
        if added > 2_000_000:
            failures.append(f"the commit added {added} bytes to the server")
        second = work / "sparse-second"
        recloned = depot([depot_exe, "clone", address, key, str(second),
                          "--sparse"], env, log)
        relisted = re.search(r": (\d+) files", recloned)
        if not relisted or int(relisted.group(1)) < listed:
            failures.append(f"the commit lists {recloned.strip()}, "
                            f"against {listed} files before")
        noise, noise_share = compare(local_captures[0], again_captures[0])
        mean, share = compare(local_captures[0], sparse_captures[0])
        if mean is None or noise is None:
            failures.append("the captures differ in size")
        elif mean > max(1.5 * noise, 0.5):
            failures.append(f"the sparse capture differs by {mean:.3f} "
                            f"against {noise:.3f} between local runs")
        last = statuses[-1]
        if not last.get("reads", 0):
            failures.append("the sparse Bistro read no texture preview")
        pending = cmd_values(sparse_text, "stats.pending_textures")
        print(f"shared: {tracked} files in the project's working copy; "
              f"{published.strip()}")
        print(f"sparse: {on_disk} files on disk after the run "
              f"({len(cooked)} cooked textures), {unfetched} of "
              f"{listed} tracked files not fetched; the remote fetched "
              f"{last.get('fetched', 0)} files and read "
              f"{last.get('reads', 0)} previews; "
              f"{pending[-1] if pending else '?'} textures pending before the "
              f"capture")
        print(f"submit: {submitted}; "
              f"{added} bytes added to the server store; "
              f"{recloned.strip()}")
        if mean is not None and noise is not None:
            print(f"capture: sparse against local, mean difference "
                  f"{mean:.3f} ({share:.3%} of pixels by more than 2); "
                  f"local against local {noise:.3f} ({noise_share:.3%})")
    except (RuntimeError, subprocess.CalledProcessError) as error:
        failures.append(str(error))
    finally:
        if server:
            server.kill()
            server.wait()
        log.close()
        for capture in made if not failures else []:
            capture.unlink(missing_ok=True)
        if not args.keep and not failures:
            for name in ("source", "shared", "store", "sparse",
                         "sparse-second"):
                shutil.rmtree(work / name, ignore_errors=True)
    for failure in failures:
        print("FAIL:", failure)
    print("logs:", work)
    if failures:
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
