#!/usr/bin/env python3
"""End-to-end check of agent federation over a collaborative session.

Starts a headless host editor and a headless guest editor on Bistro, joins
them in a session over loopback, and drives one agent on each through
`vkr_mcp`: `mason` on the host and `painter` on the guest. The check
passes when the task board, the claims and the change feed are shared:

- a task the guest's agent adds is taken by the host's agent and finished,
  and of two tasks that require a pipeline class, the host's agent gets the
  one its machine draws;
- a claim of the guest's agent refuses the host's agent, and the reverse;
- each editor's change feed names the other editor's agent batch, which
  also waits for review there until the guest accepts it;
- both editors end with the same scene digest.

    python tools/checks/check_agent_federation.py \
        --editor build_debug/editor/vkr_editor.exe \
        --mcp build_debug/tools/vkr_mcp.exe
"""

import argparse
import json
import pathlib
import re
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_editor_session import ROOT, launch, read  # noqa: E402

MCP_VERSION = "2026-07-28"


class Agent:
    """One vkr_mcp process speaking for agent `name` to one editor."""

    def __init__(self, mcp: str, socket: str, name: str, log: pathlib.Path):
        self.name = name
        self.next_id = 0
        self.log = open(log, "w", encoding="utf-8")
        self.process = subprocess.Popen(
            [mcp, "--socket", socket, "--agent", name],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.log, text=True, encoding="utf-8")

    def call(self, op: str, arguments: dict) -> dict:
        """Calls `op`; answers {"ok": bool, "result" or "error": ...}."""
        self.next_id += 1
        request = {
            "jsonrpc": "2.0", "id": self.next_id, "method": "tools/call",
            "params": {
                "name": "vkr_" + op.replace(".", "_"),
                "arguments": arguments,
                "_meta": {"io.modelcontextprotocol/protocolVersion":
                          MCP_VERSION}}}
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        self.log.write(f"> {op} {json.dumps(arguments)}\n< {line}")
        self.log.flush()
        response = json.loads(line)
        result = response.get("result", {})
        if result.get("isError") or "error" in response:
            text = "".join(c.get("text", "") for c in result.get("content", []))
            return {"ok": False,
                    "error": text or json.dumps(response.get("error"))}
        return {"ok": True, "result": result.get("structuredContent", {})}

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
        self.log.close()


def wait_for(log: pathlib.Path, pattern: str, process, deadline: float):
    while time.time() < deadline and process.poll() is None:
        match = re.search(pattern, read(log))
        if match:
            return match
        time.sleep(1.0)
    return None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--mcp", required=True)
    parser.add_argument("--port", type=int, default=7342)
    parser.add_argument("--work", default=str(ROOT / ".scratch" /
                                              "agent-federation"))
    parser.add_argument("--timeout", type=float, default=1200.0)
    args = parser.parse_args()
    editor = str(pathlib.Path(args.editor).resolve())
    mcp = str(pathlib.Path(args.mcp).resolve())
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    deadline = time.time() + args.timeout
    host_log = work / "host.log"
    guest_log = work / "guest.log"
    host_socket = str(work / "host.sock")
    guest_socket = str(work / "guest.sock")
    host, host_out = launch(
        editor, work / "host",
        "wait.scene\n"
        f'op session.host {{"name":"alpha","bind":"127.0.0.1:{args.port}"}}\n'
        "wait 600\n", host_log, host_socket)
    processes = [host]
    outputs = [host_out]
    agents = []
    failures = []
    try:
        match = wait_for(host_log, r"Hosting a collaborative session on \S+, "
                         r"key ([0-9a-f]{64})", host, deadline)
        if not match:
            print("FAIL: the host did not start a session; see", host_log)
            return 1
        guest, guest_out = launch(
            editor, work / "guest",
            "wait.scene\n"
            f'op session.join {{"address":"127.0.0.1:{args.port}",'
            f'"key":"{match.group(1)}","name":"beta"}}\n'
            "wait 600\n", guest_log, guest_socket)
        processes.append(guest)
        outputs.append(guest_out)
        if not wait_for(guest_log, r"Joined the session as participant",
                        guest, deadline):
            print("FAIL: the guest did not join; see", guest_log)
            return 1
        time.sleep(3.0)
        mason = Agent(mcp, host_socket, "mason", work / "mason.log")
        painter = Agent(mcp, guest_socket, "painter", work / "painter.log")
        agents = [mason, painter]

        def expect(agent, op, arguments, ok=True, contains=""):
            answer = agent.call(op, arguments)
            text = json.dumps(answer)
            if answer["ok"] != ok or (contains and contains not in text):
                failures.append(f"{agent.name} {op}: {text[:400]}")
            return answer.get("result", {})

        # The task board: the guest's agent adds, the host's takes it.
        added = expect(painter, "task.add",
                       {"kind": "layout", "title": "Build the plaza"})
        task_id = (added.get("task") or {}).get("task")
        taken = expect(mason, "task.next", {"kinds": ["layout"]},
                       contains="mason@alpha")
        if (taken.get("task") or {}).get("task") != task_id:
            failures.append(f"mason took {taken} instead of task {task_id}")
        expect(painter, "task.next", {"kinds": ["layout"]},
               contains='"task": null')

        # Capabilities: a task for the other pipeline class waits; the one
        # for this machine's class (ADR-087) goes to the host's agent.
        expect(painter, "task.add", {"kind": "material",
                                     "title": "Tune the tiled path",
                                     "requires": ["tiled"]})
        expect(painter, "task.add", {"kind": "material",
                                     "title": "Tune the desktop path",
                                     "requires": ["desktop"]})
        lookdev = Agent(mcp, host_socket, "lookdev", work / "lookdev.log")
        agents.append(lookdev)
        own = "tiled" if sys.platform == "darwin" else "desktop"
        expect(lookdev, "task.next", {"kinds": ["material"]},
               contains=f"Tune the {own} path")

        # Claims hold across editors, both ways.
        expect(painter, "claims.set",
               {"name": "east", "region": {"min": [-40, -5, -20],
                                           "max": [-30, 10, -10]}},
               contains="painter@beta")
        expect(mason, "claims.set",
               {"name": "overlap", "region": {"min": [-35, -5, -15],
                                              "max": [-25, 10, -5]}},
               ok=False, contains="VKR-AGENT-0010")
        expect(mason, "claims.set",
               {"name": "west", "region": {"min": [10, -5, -20],
                                           "max": [20, 10, -10]}},
               contains="mason@alpha")
        time.sleep(2.0)
        expect(painter, "claims.list", {}, contains="mason@alpha")
        expect(mason, "entity.create",
               {"name": "MasonBox", "position": [15, 1, -15]})
        painter_box = expect(painter, "entity.create",
                             {"name": "PainterBox", "position": [-35, 1, -15]})
        expect(mason, "entity.create",
               {"name": "Intruder", "position": [-36, 1, -14]},
               ok=False, contains="VKR-AGENT-0010")
        expect(painter, "entity.create",
               {"name": "Intruder2", "position": [14, 1, -14]},
               ok=False, contains="VKR-AGENT-0010")
        time.sleep(3.0)

        # Each feed names the other editor's agent batch and its object.
        for agent, author, name in ((mason, "painter@beta", "PainterBox"),
                                    (painter, "mason@alpha", "MasonBox")):
            feed = expect(agent, "changes.feed", {"after": 0, "limit": 128})
            if not any(event.get("kind") == "applied" and
                       event.get("author") == author and
                       any(entity.get("name") == name
                           for entity in event.get("entities", []))
                       for event in feed.get("events", [])):
                failures.append(f"{agent.name}'s feed lacks {author}'s "
                                f"{name}")

        # Reviews span the editors: the guest's batch waits for review on the
        # host too, and accepting it on the guest clears it there.
        expect(mason, "changes.list", {}, contains="painter@beta")
        expect(painter, "changes.accept", {"change": painter_box.get("change")})
        time.sleep(3.0)
        pending = expect(mason, "changes.list", {})
        if "painter@beta" in json.dumps(pending):
            failures.append("the guest's accepted change still waits on the "
                            "host")

        expect(mason, "task.done", {"task": task_id, "note": "plaza built"})
        time.sleep(2.0)
        expect(painter, "task.list", {"state": "done"},
               contains="plaza built")
        host_status = expect(mason, "session.status", {})
        guest_status = expect(painter, "session.status", {})
        if host_status.get("digest") != guest_status.get("digest"):
            failures.append("the scene digests differ: host "
                            f"{host_status.get('digest')} guest "
                            f"{guest_status.get('digest')}")
        if host_status.get("sequence") != guest_status.get("sequence"):
            failures.append("the session sequences differ")
        summary = (f"sequence {guest_status.get('sequence')} digest "
                   f"{guest_status.get('digest')}")
    finally:
        for agent in agents:
            agent.close()
        for process in processes:
            process.kill()
            process.wait(timeout=30)
        for out in outputs:
            out.close()
    for failure in failures:
        print("FAIL:", failure)
    print("logs:", work)
    if failures:
        return 1
    print("PASS:", summary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
