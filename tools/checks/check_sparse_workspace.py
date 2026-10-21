#!/usr/bin/env python3
"""End-to-end check of a sparse depot working copy (vkr_depot_workspace.h,
ADR-123, docs/proposals/network-protocol.md, "Sparse working copy").

1. Two headless editors open Bistro from this checkout and capture one view
   `--settle` seconds after the scene is ready; the first records every
   content read (VKR_VFS_RECORD).
2. The files below `assets/` that run read, and the files of folders
   Bistro never opens (UNREAD), are committed to `main` of a loopback depot
   and the cooked `.vkt` textures to `main.cooked.<family>`, as
   check_asset_stream.py does.
3. `vkr_depot clone --sparse` makes a working copy that holds only its root
   `.vkrdepotignore`, and `vkr_depot status` must report no change.
4. A headless editor starts with `--stream-workspace` on that copy, opens
   Bistro, so each file arrives when a loader opens it, captures the same
   view after `--settle` seconds and makes one edit: `material.create`
   copies one of Bistro's materials to a new file in the working copy.
5. `vkr_depot status` must then list the new material and no deletion,
   no file of the unread folders may be on disk, and every file on disk
   must be one the local run read; `vkr_depot commit` must upload a few
   objects only. A second sparse clone lists every file of the new
   commit, and the new material read back from the depot equals the
   working copy's.

The check passes when all of that holds and the sparse capture differs from
the first local one by no more than the second local one does (with a floor
of 0.5 levels of 255). It removes its depot and working copies afterwards.

    python tools/checks/check_sparse_workspace.py \\
        --editor build_release/editor/vkr_editor \\
        --depot build_release/tools/depot/vkr_depot
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

from check_asset_stream import (LOCAL_POLL, LOCAL_SCRIPT, ROOT, VIEW,
                                captures, cmd_values, compare, depot,
                                run_editor, scene_seconds, stream_statuses)

ADDED = "assets/materials/zz_sparse_check.mt"
# Folders of this checkout that Bistro never opens: their files must stay
# unfetched in the sparse working copy.
UNREAD = ("assets/characters", "assets/models/alps")

SPARSE_SCRIPT = f"""wait.scene
op stream.status
stats.pending_textures
{{polls}}
op view.capture {{{{{VIEW}}}}}
wait 3
op material.create {{{{"path":"{ADDED}","kind":"definition","from":"{{source}}"}}}}
wait 2
op stream.status
echo capture-done
"""
SPARSE_POLL = "wait 2\nop stream.status\nstats.pending_textures"


def disk_files(root: pathlib.Path) -> list:
    """Files below `root` outside its `.vkrdepot` state, '/'-separated."""
    found = []
    for path in root.rglob("*"):
        relative = path.relative_to(root).as_posix()
        if path.is_file() and not relative.startswith(".vkrdepot/"):
            found.append(relative)
    return sorted(found)


def status_counts(text: str):
    """(changes, unfetched, tracked) from `vkr_depot status` of a sparse
    working copy, and its change lines as (code, path)."""
    match = re.search(r"(\d+) changes on \S+ at \w+; (\d+) of (\d+) files not "
                      r"fetched", text)
    if not match:
        raise RuntimeError(f"unexpected status output: {text.strip()}")
    lines = [(line[0], line[2:].strip()) for line in text.splitlines()
             if len(line) > 2 and line[0] in "AMD" and line[1] == " "]
    return (int(match.group(1)), int(match.group(2)), int(match.group(3)),
            lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--depot", required=True)
    parser.add_argument("--port", type=int, default=7357)
    parser.add_argument("--settle", type=int, default=60,
                        help="seconds every run waits after its scene is "
                        "ready before its capture")
    parser.add_argument("--work", default=str(ROOT / ".scratch" /
                                              "sparse-workspace"))
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--keep", action="store_true",
                        help="keep the depot and working copies")
    args = parser.parse_args()
    editor = str(pathlib.Path(args.editor).resolve())
    depot_exe = str(pathlib.Path(args.depot).resolve())
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
        # 1. Two local runs, the first recording its reads.
        reads = work / "reads.txt"
        local_script = LOCAL_SCRIPT.format(
            polls="\n".join([LOCAL_POLL] * args.settle))
        local_text = run_editor(
            editor, work / "local", local_script, work / "local.log", [],
            {"VKR_VFS_RECORD": str(reads)}, args.timeout)
        again_text = run_editor(editor, work / "again", local_script,
                                work / "again.log", [], {}, args.timeout)
        local_captures = captures(local_text)
        again_captures = captures(again_text)
        made += local_captures + again_captures
        if not local_captures or not again_captures:
            raise RuntimeError(f"a local run did not capture; see {work}")
        identities = sorted(
            {line.strip()
             for line in reads.read_text(encoding="utf-8",
                                         errors="replace").splitlines()
             if line.startswith("assets/")})
        files = [i for i in identities if (ROOT / i).is_file()]
        source = next((i for i in files if i.endswith(".mt")), None)
        if not source:
            raise RuntimeError("the local run read no material")
        read = set(files)
        unread = sorted(
            path.relative_to(ROOT).as_posix()
            for folder in UNREAD for path in (ROOT / folder).rglob("*")
            if path.is_file() and path.relative_to(ROOT).as_posix() not in read)
        if not unread:
            raise RuntimeError(f"no unread files under {UNREAD}")

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
        seed = work / "seed"
        depot([depot_exe, "clone", address, key, str(seed)], env, log)
        for identity in files + unread:
            target = seed / identity
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / identity, target)
        (seed / ".vkrdepotignore").write_text("*.vkt\n", encoding="utf-8")
        depot([depot_exe, "commit", str(seed), "-m", "Bistro"], env, log)
        depot([depot_exe, "cooked", str(seed)], env, log)

        # 3. The sparse clone holds the ignore rules alone.
        sparse = work / "sparse"
        cloned = depot([depot_exe, "clone", address, key, str(sparse),
                        "--sparse"], env, log)
        on_disk = disk_files(sparse)
        if on_disk != [".vkrdepotignore"]:
            failures.append(f"the sparse clone wrote {len(on_disk)} files, "
                            f"such as {on_disk[:3]}")
        changes, unfetched, tracked, _ = status_counts(
            depot([depot_exe, "status", str(sparse)], env, log))
        if changes:
            failures.append(f"a fresh sparse clone shows {changes} changes")

        # 4. Bistro from the sparse working copy, then one edit.
        polls = "\n".join([SPARSE_POLL] * (args.settle // 2))
        sparse_text = run_editor(
            editor, work / "sparse-run",
            SPARSE_SCRIPT.format(polls=polls, source=source),
            work / "sparse.log", ["--stream-workspace", str(sparse)], {},
            args.timeout)
        statuses = stream_statuses(sparse_text)
        sparse_captures = captures(sparse_text)
        made += sparse_captures
        if "capture-done" not in sparse_text or not sparse_captures:
            raise RuntimeError("the sparse run did not capture; see "
                               f"{work / 'sparse.log'}")
        if not statuses or not statuses[0].get("streaming"):
            raise RuntimeError("the editor did not stream the working copy; "
                               f"see {work / 'sparse.log'}")
        if not (sparse / ADDED).is_file():
            failures.append(f"material.create wrote no {ADDED} in the "
                            "working copy")

        # 5. Status, commit, and the new commit read back.
        after = depot([depot_exe, "status", str(sparse)], env, log)
        changes_after, unfetched_after, tracked_after, lines = \
            status_counts(after)
        if ("A", ADDED) not in lines:
            failures.append("status does not list the new material")
        deleted = [path for code, path in lines if code == "D"]
        if deleted:
            failures.append(f"status lists {len(deleted)} deletions, such as "
                            f"{deleted[0]}")
        fetched = tracked_after - unfetched_after
        on_disk_after = set(disk_files(sparse))
        unread_fetched = [path for path in unread if path in on_disk_after]
        if unread_fetched:
            failures.append(f"{len(unread_fetched)} files the scene never "
                            f"opens arrived, such as {unread_fetched[0]}")
        stray = sorted(path for path in on_disk_after
                       if path not in read and path not in
                       (".vkrdepotignore", ADDED))
        if stray:
            failures.append(f"{len(stray)} files on disk the local run never "
                            f"read, such as {stray[0]}")
        pushed = depot([depot_exe, "commit", str(sparse), "-m",
                        "sparse edit"], env, log)
        upload = re.search(r"(\d+) objects, (\d+) bytes uploaded", pushed)
        if not upload:
            raise RuntimeError(f"unexpected commit output: {pushed.strip()}")
        uploaded_objects, uploaded_bytes = int(upload.group(1)), int(
            upload.group(2))
        if uploaded_bytes > 1_000_000:
            failures.append(f"the commit uploaded {uploaded_bytes} bytes")
        second = work / "sparse-second"
        recloned = depot([depot_exe, "clone", address, key, str(second),
                          "--sparse"], env, log)
        listed = re.search(r": (\d+) files", recloned)
        expected = tracked + sum(1 for code, _ in lines if code == "A")
        if not listed or int(listed.group(1)) != expected:
            failures.append(f"the new commit lists {recloned.strip()}, "
                            f"expected {expected} files")
        stream = work / "stream"
        depot([depot_exe, "stream", address, key, str(stream), ADDED], env,
              log)
        if (stream / ADDED).read_bytes() != (sparse / ADDED).read_bytes():
            failures.append("the committed material differs from the working "
                            "copy's")

        noise_mean, noise_share = compare(local_captures[0],
                                          again_captures[0])
        final_mean, final_share = compare(local_captures[0],
                                          sparse_captures[0])
        if final_mean is None or noise_mean is None:
            failures.append("the captures differ in size")
        elif final_mean > max(1.5 * noise_mean, 0.5):
            failures.append(f"the sparse capture differs by {final_mean:.3f} "
                            f"against {noise_mean:.3f} between local runs")
        first, last = statuses[0], statuses[-1]
        pending = cmd_values(sparse_text, "stats.pending_textures")
        settled = next((statuses[i].get("since_open_ms", 0)
                        for i in range(min(len(statuses), len(pending)))
                        if pending[i] == 0), None)
        print(f"seed: {len(files)} files read by the local run; "
              f"{cloned.strip()}")
        print(f"sparse clone: {len(on_disk)} file on disk, {unfetched} of "
              f"{tracked} not fetched")
        print(f"editor: scene ready {first.get('since_open_ms', 0):.0f} ms "
              f"after the stream opened (local "
              f"{scene_seconds(local_text) or 0:.1f} s after its load "
              f"began); no texture pending from "
              f"{'never' if settled is None else f'{settled:.0f} ms'}; "
              f"{last.get('fetched', 0)} files fetched, "
              f"{last.get('reads', 0)} previews read, "
              f"{last.get('failed', 0)} failed")
        unread_tracked = sum(1 for path in unread
                             if not path.endswith(".vkt"))
        print(f"after the edit: {changes_after} changes "
              f"({', '.join(f'{c} {p}' for c, p in lines[:5])}); "
              f"{fetched} of {tracked_after} files fetched; none of the "
              f"{unread_tracked} tracked files of {', '.join(UNREAD)}")
        print(f"commit: {uploaded_objects} objects, {uploaded_bytes} bytes "
              f"uploaded; {recloned.strip()}")
        if final_mean is not None and noise_mean is not None:
            print(f"capture: sparse against local, mean difference "
                  f"{final_mean:.3f} ({final_share:.3%} of pixels by more "
                  f"than 2); local against local {noise_mean:.3f} "
                  f"({noise_share:.3%})")
    except RuntimeError as error:
        failures.append(str(error))
    finally:
        if server:
            server.kill()
            server.wait()
        log.close()
        for capture in made if not failures else []:
            capture.unlink(missing_ok=True)
        if not args.keep and not failures:
            for name in ("store", "seed", "sparse", "sparse-second",
                         "stream"):
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
