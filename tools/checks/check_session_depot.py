#!/usr/bin/env python3
"""End-to-end check of a collaborative session on a depot (ADR-106).

Serves a depot, commits the Bistro scene and its edit overlay to `main`,
clones it twice, and opens a headless editor on each working copy. The host
editor creates an object and leaves it unsaved, then hosts with a 4 KiB
history: it saves and commits a session base on a branch of its own. It
then creates objects until its history passes the budget, which makes a
new base for joiners, and one more after it. The guest editor joins from
`main`: it checks the newest base out, reloads its scene, is welcomed and
replays only the edits after that base. The host then writes a file into
its working copy and publishes it; the guest fetches it. The check passes
when the guest holds the host's unsaved object and the objects from before
and after the newest base, the published file arrives in the guest's
working copy with the same bytes, and both editors end at the same depot
commit and scene digest.

    python tools/checks/check_session_depot.py \
        --editor build_debug/editor/vkr_editor.exe \
        --mcp build_debug/tools/vkr_mcp.exe \
        --depot build_debug/tools/depot/vkr_depot.exe
"""

import argparse
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_agent_federation import Agent, wait_for  # noqa: E402
from check_editor_session import ROOT, editor_env  # noqa: E402


def run(command: list, env: dict, log) -> str:
    log.write("$ " + " ".join(command) + "\n")
    result = subprocess.run(command, env=env, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", cwd=ROOT)
    log.write(result.stdout + result.stderr)
    log.flush()
    if result.returncode != 0:
        raise RuntimeError(f"{command[1]} failed: {result.stderr.strip()}")
    return result.stdout


def launch(editor: str, work: pathlib.Path, scene: pathlib.Path,
           socket: str, log: pathlib.Path, env_extra: dict):
    work.mkdir(parents=True, exist_ok=True)
    env = editor_env(work, autoclose=1800)
    env.update(env_extra)
    out = open(log, "w", encoding="utf-8", errors="replace")
    process = subprocess.Popen(
        [editor, "--headless", "--agent-socket", socket, "--scene",
         str(scene), "--exec", "wait.scene\nwait 900\n"],
        cwd=ROOT, env=env, stdout=out, stderr=subprocess.STDOUT)
    return process, out


def poll(agent: Agent, test, deadline: float, process,
         interval: float = 2.0):
    """Calls session.status until `test(status)` holds, the deadline passes
    or the editor exits; the last status."""
    status = {}
    while time.time() < deadline and process.poll() is None:
        answer = agent.call("session.status", {})
        status = answer.get("result", {})
        if test(status):
            return status
        time.sleep(interval)
    return status


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--mcp", required=True)
    parser.add_argument("--depot", required=True)
    parser.add_argument("--port", type=int, default=7346)
    parser.add_argument("--depot-port", type=int, default=7345)
    parser.add_argument("--work", default=str(ROOT / ".scratch" /
                                              "session-depot"))
    parser.add_argument("--timeout", type=float, default=1500.0)
    args = parser.parse_args()
    editor = str(pathlib.Path(args.editor).resolve())
    mcp = str(pathlib.Path(args.mcp).resolve())
    depot = str(pathlib.Path(args.depot).resolve())
    work = pathlib.Path(args.work)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    log = open(work / "depot.log", "w", encoding="utf-8")
    env = dict(os.environ)
    env["VKR_DEPOT_IDENTITY"] = str(work / "identity.key")
    deadline = time.time() + args.timeout
    processes = []
    outputs = []
    agents = []
    failures = []
    try:
        run([depot, "keygen"], env, log)
        store = work / "store"
        output = run([depot, "init", str(store)], env, log)
        key = re.search(r"server key ([0-9a-f]{64})", output).group(1)
        server = subprocess.Popen(
            [depot, "serve", str(store), "--bind",
             f"127.0.0.1:{args.depot_port}", "--open"],
            env=env, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
        processes.append(server)
        time.sleep(1.0)
        address = f"127.0.0.1:{args.depot_port}"
        seed = work / "seed"
        run([depot, "clone", address, key, str(seed)], env, log)
        (seed / "scenes").mkdir()
        for name in ("bistro.scene.json", "bistro.scene.json.editor.json"):
            shutil.copy(ROOT / "assets" / "scenes" / name, seed / "scenes" /
                        name)
        run([depot, "commit", str(seed), "-m", "Bistro"], env, log)
        host_copy = work / "host"
        guest_copy = work / "guest"
        run([depot, "clone", address, key, str(host_copy)], env, log)
        run([depot, "clone", address, key, str(guest_copy)], env, log)
        editor_env_extra = {"VKR_DEPOT_IDENTITY": env["VKR_DEPOT_IDENTITY"]}

        host_socket = str(work / "host.sock")
        guest_socket = str(work / "guest.sock")
        host, host_out = launch(editor, work / "host-editor",
                                host_copy / "scenes" / "bistro.scene.json",
                                host_socket, work / "host.log",
                                editor_env_extra)
        processes.append(host)
        outputs.append(host_out)
        if not wait_for(work / "host.log", r"\[cmd\] > wait 900", host,
                        deadline):
            print("FAIL: the host did not load its scene")
            return 1
        mason = Agent(mcp, host_socket, "mason", work / "mason.log")
        agents.append(mason)
        mason.call("entity.create", {"name": "PreHost",
                                     "position": [2, 1, 2],
                                     "review": False})
        time.sleep(2.0)
        answer = mason.call("session.host",
                            {"name": "alpha",
                             "bind": f"127.0.0.1:{args.port}",
                             "history": 4096})
        session_key = answer.get("result", {}).get("key", "")
        hosted = poll(mason, lambda s: (s.get("depot") or {}).get("head", "")
                      .strip("0") != "", deadline, host)
        base = (hosted.get("depot") or {}).get("head", "")
        if not base.strip("0"):
            failures.append(f"the host made no depot base: {hosted}")
            raise RuntimeError("no base")

        # Past the 4 KiB history the host makes a newer base for joiners.
        bulk = 0
        while bulk < 200 and time.time() < deadline:
            mason.call("entity.create", {"name": f"Bulk {bulk}",
                                         "position": [bulk * 0.5, 1, -4],
                                         "review": False})
            bulk += 1
            if re.search(r"New editors join from base",
                         (work / "host.log").read_text(
                             encoding="utf-8", errors="replace")):
                break
        if not wait_for(work / "host.log", r"New editors join from base "
                        r"[0-9a-f]+ at edit (\d+)", host, deadline):
            failures.append(f"the host made no newer base after {bulk} "
                            "objects")
            raise RuntimeError("no newer base")
        mason.call("entity.create", {"name": "AfterBase",
                                     "position": [0, 1, -8],
                                     "review": False})
        time.sleep(2.0)

        guest, guest_out = launch(editor, work / "guest-editor",
                                  guest_copy / "scenes" / "bistro.scene.json",
                                  guest_socket, work / "guest.log",
                                  editor_env_extra)
        processes.append(guest)
        outputs.append(guest_out)
        if not wait_for(work / "guest.log", r"\[cmd\] > wait 900", guest,
                        deadline):
            print("FAIL: the guest did not load its scene")
            return 1
        painter = Agent(mcp, guest_socket, "painter", work / "painter.log")
        agents.append(painter)
        painter.call("session.join", {"address": f"127.0.0.1:{args.port}",
                                      "key": session_key, "name": "beta"})
        joined = poll(painter, lambda s: s.get("joined") or s.get("error"),
                      deadline, guest)
        if not joined.get("joined"):
            failures.append(f"the guest did not join: {joined.get('error')}")
            raise RuntimeError("no join")
        for name in ("PreHost", "Bulk 0", "AfterBase"):
            found = painter.call("entity.get", {"entity": name})
            if not found.get("ok"):
                failures.append(f"the guest lacks the host's {name}: "
                                f"{found.get('error')}")
        replay = re.search(r"replaying (\d+) edits",
                           (work / "guest.log").read_text(
                               encoding="utf-8", errors="replace"))
        if not replay or not 0 < int(replay.group(1)) < bulk:
            failures.append("the guest did not replay from the newer base: "
                            f"{replay.group(0) if replay else None}")

        notes = host_copy / "scenes" / "notes.txt"
        notes.write_text("published through the session depot\n",
                         encoding="utf-8")
        published = mason.call("session.publish", {"message": "Notes"})
        if not published.get("ok"):
            failures.append(f"publish failed: {published.get('error')}")
        host_state = poll(
            mason, lambda s: (s.get("depot") or {}).get("head") not in
            ("", base) and not (s.get("depot") or {}).get("busy"), deadline,
            host)
        head = (host_state.get("depot") or {}).get("head", "")
        guest_state = poll(
            painter, lambda s: (s.get("depot") or {}).get("head") == head and
            not (s.get("depot") or {}).get("busy"), deadline, guest)
        arrived = guest_copy / "scenes" / "notes.txt"
        if not arrived.exists() or arrived.read_bytes() != notes.read_bytes():
            failures.append("the published file did not reach the guest")
        if (guest_state.get("depot") or {}).get("head") != head:
            failures.append(f"the depot heads differ: host {head} guest "
                            f"{(guest_state.get('depot') or {}).get('head')}")
        if guest_state.get("digest") != host_state.get("digest"):
            failures.append("the scene digests differ")
        summary = (f"base {base[:12]} head {head[:12]}, newer base after "
                   f"{bulk} objects, guest {replay.group(0) if replay else ''}")
    except RuntimeError as error:
        failures.append(str(error))
        summary = ""
    finally:
        for agent in agents:
            agent.close()
        for process in processes:
            process.kill()
            process.wait(timeout=30)
        for out in outputs:
            out.close()
        log.close()
    for failure in failures:
        print("FAIL:", failure)
    print("logs:", work)
    if failures:
        return 1
    print("PASS:", summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
