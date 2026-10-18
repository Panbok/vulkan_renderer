#!/usr/bin/env python3
"""End-to-end check of world partition cells in a collaborative session
(editor_session.c, ADR-106).

Each editor streams cells around its own camera, so an edit can name an
object in a cell another editor has not loaded. The check copies Bistro to
an ignored scratch scene beside it, gives the copy a world partition and
saves a cube 2 km away into its cell document. A host then opens the copy,
loads that cell, hosts and renames the cube; a guest whose camera stays at
Bistro joins and replays the rename. The check passes when the guest
loaded the cell for the edit, has the renamed cube, and both editors end at
the same session sequence and digest. It removes the scratch scene, its
sidecar and its cells afterwards.

    python tools/checks/check_session_partition.py --editor build_debug/editor/vkr_editor.exe
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import time

from check_editor_session import ROOT, SCENE, launch, read, results, statuses

COPY = "assets/scenes/bistro_partition_check.scene.json"
# 2 km out in both axes: cell 15,15 at the default 128 m cell size, far past
# the 384 m load radius around Bistro's default camera.
FAR = 2000
CELL = FAR // 128

PREP_SCRIPT = f"""wait.scene
create cube
wait 2
sel.name = "Partition"
wait 1
component.add world_partition
wait 2
create cube
wait 2
sel.name = "FarCube"
wait 1
sel.position = ({FAR}, 0, {FAR})
wait 2
scene.save
wait 3
echo prep-done
"""

HOST_SCRIPT = """wait.scene
partition.load {cell} {cell} {cell} {cell}
wait 3
op session.host {{"name":"gamma","bind":"127.0.0.1:{port}"}}
wait 3
select FarCube
wait 1
sel.name = "FarCubeRenamed"
wait 2
echo host-edited
{polls}
op session.status
"""

GUEST_SCRIPT = """wait.scene
op session.join {{"address":"127.0.0.1:{port}","key":"{key}","name":"delta"}}
wait 20
op session.status
select FarCubeRenamed
sel.name
wait 2
op session.status
echo guest-done
"""


def scratch_paths() -> list:
    scene = ROOT / COPY
    return [scene, pathlib.Path(str(scene) + ".editor.json"),
            scene.with_name(scene.name[:-len(".json")] + ".cells")]


def remove_scratch() -> None:
    for path in scratch_paths():
        if path.is_dir():
            shutil.rmtree(path)
        elif path.exists():
            path.unlink()


def wait_for(process, deadline: float) -> None:
    try:
        process.wait(timeout=max(1.0, deadline - time.time()))
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--port", type=int, default=7343)
    parser.add_argument("--work", default=str(ROOT / ".scratch" /
                                              "session-partition"))
    parser.add_argument("--timeout", type=float, default=1200.0)
    args = parser.parse_args()
    args.editor = str(pathlib.Path(args.editor).resolve())
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    deadline = time.time() + args.timeout
    failures = []

    remove_scratch()
    shutil.copyfile(ROOT / SCENE, ROOT / COPY)
    try:
        prep_log = work / "prep.log"
        prep, prep_out = launch(args.editor, work / "prep", PREP_SCRIPT,
                                prep_log, autoclose=240, scene=COPY)
        while time.time() < deadline and prep.poll() is None:
            if "prep-done" in read(prep_log):
                break
            time.sleep(1.0)
        prep.kill()
        prep.wait()
        prep_out.close()
        cell = scratch_paths()[2] / f"{CELL}_{CELL}.json"
        if not cell.exists() or "FarCube" not in cell.read_text(
                encoding="utf-8", errors="replace"):
            print("FAIL: the scratch scene has no cell document", cell,
                  "holding FarCube; see", prep_log)
            return 1

        host_log = work / "host.log"
        guest_log = work / "guest.log"
        polls = "\n".join(["wait 8", "op session.status"] * 40)
        host, host_out = launch(
            args.editor, work / "host",
            HOST_SCRIPT.format(cell=CELL, port=args.port, polls=polls),
            host_log, scene=COPY)
        key = None
        while time.time() < deadline and host.poll() is None:
            text = read(host_log)
            if "host-edited" in text:
                match = re.search(r"Hosting a collaborative session on \S+, "
                                  r"key ([0-9a-f]{64})", text)
                if match:
                    key = match.group(1)
                    break
            time.sleep(1.0)
        if not key:
            host.kill()
            host.wait()
            host_out.close()
            print("FAIL: the host did not start a session; see", host_log)
            return 1
        guest, guest_out = launch(args.editor, work / "guest",
                                  GUEST_SCRIPT.format(port=args.port, key=key),
                                  guest_log, scene=COPY)
        wait_for(guest, deadline)
        wait_for(host, deadline)
        host_out.close()
        guest_out.close()
    finally:
        remove_scratch()

    host_text = read(host_log)
    guest_text = read(guest_log)
    host_statuses = statuses(host_text)
    guest_statuses = statuses(guest_text)
    if "Session edit" in guest_text and "does not apply here" in guest_text:
        failures.append("the rename did not apply on the guest")
    if f"Loading cell {CELL},{CELL} for a session edit" not in guest_text:
        failures.append(f"the guest did not load cell {CELL},{CELL} for the "
                        "edit")
    if not any("FarCubeRenamed" in r for r in results(guest_text)):
        failures.append("the guest has no FarCubeRenamed")
    if "guest-done" not in guest_text:
        failures.append("the guest script did not finish")
    if not host_statuses or not guest_statuses:
        failures.append("missing session.status results")
    else:
        final_host = host_statuses[-1]
        final_guest = guest_statuses[-1]
        if not any(status.get("joined") for status in guest_statuses):
            failures.append("the guest did not join: "
                            f"{final_guest.get('error')}")
        if final_guest.get("digest") != final_host.get("digest"):
            failures.append("the scene digests differ: host "
                            f"{final_host.get('digest')} guest "
                            f"{final_guest.get('digest')}")
        if final_guest.get("sequence") != final_host.get("sequence"):
            failures.append("the session sequences differ: host "
                            f"{final_host.get('sequence')} guest "
                            f"{final_guest.get('sequence')}")
    for failure in failures:
        print("FAIL:", failure)
    print("host log:", host_log)
    print("guest log:", guest_log)
    if failures:
        return 1
    print("PASS: sequence", guest_statuses[-1].get("sequence"), "digest",
          guest_statuses[-1].get("digest"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
