#!/usr/bin/env python3
"""End-to-end check of collaborative editing (editor_session.c).

Starts a headless host editor on Bistro, which hosts a session on loopback
and edits before anyone joins, then a headless guest editor that joins with
the host's key, replays that history and edits through the host. Both print
`session.status`; the check passes when both editors end at the same session
sequence with the same scene digest, the guest saw the host's edits, the
host saw the guest's, and the guest's refused edit did not apply.

    python tools/checks/check_editor_session.py --editor build_debug/editor/vkr_editor.exe
"""

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCENE = "assets/scenes/bistro.scene.json"

HOST_SCRIPT = """wait.scene
op session.host {{"name":"alpha","bind":"127.0.0.1:{port}"}}
create cube
wait 2
sel.name = "HostCube"
wait 1
sel.position = (1, 2, 3)
wait 1
echo host-edited
{polls}
select GuestCube
sel.position
op session.status
"""

GUEST_SCRIPT = """wait.scene
op session.join {{"address":"127.0.0.1:{port}","key":"{key}","name":"beta"}}
wait 10
op session.status
select HostCube
sel.position
create cube
wait 3
sel.name = "GuestCube"
wait 2
sel.position = (4, 5, 6)
wait 2
sel.position = (7, 8, 9)
wait 2
undo
wait 3
sel.position
scene.entities
duplicate
wait 3
scene.entities
wait 5
op session.status
echo guest-done
"""


def editor_env(work: pathlib.Path) -> dict:
    env = dict(os.environ)
    for name in ("MTL_DEBUG_LAYER", "MTL_SHADER_VALIDATION",
                 "VK_INSTANCE_LAYERS"):
        env.pop(name, None)
    home = work / "home"
    home.mkdir(parents=True, exist_ok=True)
    env["HOME"] = str(home)
    env["USERPROFILE"] = str(home)
    env["VKR_EDITOR_LAYOUT_PATH"] = str(work / "layout.json")
    env["VKR_GRAPHICS_SETTINGS_PATH"] = str(work / "graphics.json")
    env["VKR_AUTOCLOSE_SECONDS"] = "600"
    return env


def launch(editor: str, work: pathlib.Path, script: str, log: pathlib.Path):
    work.mkdir(parents=True, exist_ok=True)
    out = open(log, "w", encoding="utf-8", errors="replace")
    process = subprocess.Popen(
        [editor, "--headless", "--no-agent-socket", "--scene", SCENE,
         "--exec", script],
        cwd=ROOT, env=editor_env(work), stdout=out, stderr=subprocess.STDOUT)
    return process, out


def read(log: pathlib.Path) -> str:
    return log.read_text(encoding="utf-8", errors="replace")


def statuses(text: str) -> list:
    """The session.status results the log printed, in order."""
    found = []
    for match in re.finditer(r"\[agent\] (\{.*\})", text):
        try:
            response = json.loads(match.group(1))
        except json.JSONDecodeError:
            continue
        result = response.get("result")
        if isinstance(result, dict) and "digest" in result:
            found.append(result)
    return found


def results(text: str) -> list:
    return re.findall(r"\[cmd\] ([^\r\n\[]*)", text)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--port", type=int, default=7341)
    parser.add_argument("--work", default=str(ROOT / ".scratch" /
                                              "editor-session"))
    parser.add_argument("--timeout", type=float, default=900.0)
    args = parser.parse_args()
    args.editor = str(pathlib.Path(args.editor).resolve())
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    host_log = work / "host.log"
    guest_log = work / "guest.log"
    # The host keeps the session open while the guest loads Bistro, joins
    # and edits; a cold Debug start takes minutes.
    polls = "\n".join(["wait 8", "op session.status"] * 40)
    host, host_out = launch(args.editor, work / "host",
                            HOST_SCRIPT.format(port=args.port, polls=polls),
                            host_log)
    deadline = time.time() + args.timeout
    key = None
    while time.time() < deadline and host.poll() is None:
        text = read(host_log)
        if "host-edited" in text:
            for status in statuses(text):
                if status.get("mode") == "host" and status.get("key"):
                    key = status["key"]
            match = re.search(r"Hosting a collaborative session on \S+, key "
                              r"([0-9a-f]{64})", text)
            if match:
                key = match.group(1)
            if key:
                break
        time.sleep(1.0)
    if not key:
        host.kill()
        print("FAIL: the host did not start a session; see", host_log)
        return 1
    guest, guest_out = launch(args.editor, work / "guest",
                              GUEST_SCRIPT.format(port=args.port, key=key),
                              guest_log)
    for process in (guest, host):
        remaining = max(1.0, deadline - time.time())
        try:
            process.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            process.kill()
    host_out.close()
    guest_out.close()

    host_text = read(host_log)
    guest_text = read(guest_log)
    failures = []
    guest_statuses = statuses(guest_text)
    host_statuses = statuses(host_text)
    if "guest-done" not in guest_text:
        failures.append("the guest script did not finish")
    if not guest_statuses or not host_statuses:
        failures.append("missing session.status results")
    else:
        final_guest = guest_statuses[-1]
        final_host = host_statuses[-1]
        if not any(status.get("joined") for status in guest_statuses):
            failures.append("the guest did not join: "
                            f"{guest_statuses[-1].get('error')}")
        if final_guest.get("digest") != final_host.get("digest"):
            failures.append("the scene digests differ: host "
                            f"{final_host.get('digest')} guest "
                            f"{final_guest.get('digest')}")
        if final_guest.get("sequence") != final_host.get("sequence"):
            failures.append("the session sequences differ: host "
                            f"{final_host.get('sequence')} guest "
                            f"{final_guest.get('sequence')}")
        if not any(peer.get("name") == "beta"
                   for status in host_statuses
                   for peer in status.get("peers", [])):
            failures.append("the host never saw the guest's presence")
    guest_results = results(guest_text)
    host_results = results(host_text)
    if not any(r.startswith("(1, 2, 3)") or r.startswith("(1.0") or
               "1, 2, 3" in r for r in guest_results):
        failures.append("the guest did not see HostCube at (1, 2, 3)")
    if not any("4, 5, 6" in r for r in host_results):
        failures.append("the host did not see GuestCube at (4, 5, 6) after "
                        "the guest's undo")
    entity_counts = [r for r in guest_results if re.fullmatch(r"\d+\s*", r)]
    if len(entity_counts) >= 2 and entity_counts[-1] != entity_counts[-2]:
        failures.append("the guest's duplicate applied: "
                        f"{entity_counts[-2]} -> {entity_counts[-1]}")
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
