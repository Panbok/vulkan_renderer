#!/usr/bin/env python3
"""Run a level plan against the VKR editor and print a short report.

A plan is JSON:

  {"agent": "north-wing",
   "steps": [
     {"op": "claims.set", "args": {"region": {...}, "name": "north wing"}},
     {"op": "batch", "args": {"label": "rooms", "ops": [...]}},
     {"op": "query.raycast", "args": {"origin": [...], "direction": [0, -1, 0]},
      "expect": [{"path": "position.1", "between": [2.9, 3.1]}]},
     {"op": "level.map", "args": {"region": {...}}}]}

Each step runs one editor operation (`ops.list` names them) through
`vkr_mcp`. An expectation reads a value from the step's result by a dotted
path (keys and list indices, such as `issues.0.kind`) and compares it:
`equals`, `between` [low, high], `at_least`, `at_most`, `count` (the length
of a list or string) or `contains` (an item of a list, or a substring).
The run stops at the first failed step unless that step sets
`"continue": true`.

Usage: level_run.py [--socket PATH] [--mcp PATH] [--agent NAME]
                    [--max-chars N] plan.json
Exit: 0 when every step and expectation passed, 1 when one failed, 2 for a
usage, plan or connection error.
"""

import argparse
import json
import os
import pathlib
import subprocess
import sys

META = {"io.modelcontextprotocol/protocolVersion": "2026-07-28"}
ROOT = pathlib.Path(__file__).resolve().parents[4]


def default_mcp():
    name = "vkr_mcp.exe" if os.name == "nt" else "vkr_mcp"
    return ROOT / "build_release" / "tools" / name


class Mcp:
    """One vkr_mcp process: one editor connection and author."""

    def __init__(self, mcp, socket, agent):
        args = [str(mcp)]
        if socket:
            args += ["--socket", socket]
        if agent:
            args += ["--agent", agent]
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL)
        self.next_id = 0

    def call(self, op, arguments):
        """Returns (ok, result or error text)."""
        self.next_id += 1
        request = {"jsonrpc": "2.0", "id": self.next_id, "method": "tools/call",
                   "params": {"name": "vkr_" + op.replace(".", "_"),
                              "arguments": arguments, "_meta": META}}
        self.proc.stdin.write((json.dumps(request) + "\n").encode("utf-8"))
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise ConnectionError("vkr_mcp closed; is the editor running?")
        response = json.loads(line)
        if "error" in response:
            return False, response["error"].get("message", "MCP error")
        result = response["result"]
        if result.get("isError"):
            return False, result["content"][0]["text"]
        return True, result.get("structuredContent")

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=30)


def read_path(value, path):
    for key in path.split(".") if path else []:
        if isinstance(value, list):
            value = value[int(key)]
        elif isinstance(value, dict):
            value = value[key]
        else:
            raise KeyError(key)
    return value


def check(result, expect):
    """Returns (passed, text) for one expectation."""
    path = expect.get("path", "")
    try:
        value = read_path(result, path)
    except (KeyError, IndexError, ValueError, TypeError):
        return False, f"{path or 'result'} is missing"
    shown = json.dumps(value)[:80]
    if "equals" in expect:
        return value == expect["equals"], f"{path}={shown} (want {expect['equals']})"
    if "between" in expect:
        low, high = expect["between"]
        ok = isinstance(value, (int, float)) and low <= value <= high
        return ok, f"{path}={shown} (want {low}..{high})"
    if "at_least" in expect:
        ok = isinstance(value, (int, float)) and value >= expect["at_least"]
        return ok, f"{path}={shown} (want >= {expect['at_least']})"
    if "at_most" in expect:
        ok = isinstance(value, (int, float)) and value <= expect["at_most"]
        return ok, f"{path}={shown} (want <= {expect['at_most']})"
    if "count" in expect:
        count = len(value) if isinstance(value, (list, str)) else None
        return count == expect["count"], f"{path} count={count} (want {expect['count']})"
    if "contains" in expect:
        return expect["contains"] in value, f"{path} contains {expect['contains']!r}"
    return False, f"unknown expectation {json.dumps(expect)}"


def summary(op, result, max_chars):
    """A short view of a result: map rows in full, captures by path."""
    if op == "level.map" and isinstance(result, dict) and "rows" in result:
        head = {k: result[k] for k in ("cell", "first", "columns", "walkable",
                                       "reachable", "start") if k in result}
        return json.dumps(head) + "\n" + "\n".join(
            "    |" + row + "|" for row in result["rows"])
    text = json.dumps(result, separators=(",", ":"))
    return text if len(text) <= max_chars else text[:max_chars] + "..."


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("plan")
    parser.add_argument("--socket", default=os.environ.get(
        "VKR_EDITOR_AGENT_SOCKET"))
    parser.add_argument("--mcp", default=str(default_mcp()))
    parser.add_argument("--agent", default=None)
    parser.add_argument("--max-chars", type=int, default=400)
    options = parser.parse_args()
    try:
        plan = json.loads(pathlib.Path(options.plan).read_text(encoding="utf-8"))
        steps = plan["steps"]
    except (OSError, ValueError, KeyError) as error:
        print(f"error: the plan did not load: {error}", file=sys.stderr)
        return 2
    if not pathlib.Path(options.mcp).exists():
        print(f"error: no vkr_mcp at {options.mcp}; build the editor first",
              file=sys.stderr)
        return 2
    mcp = Mcp(options.mcp, options.socket, options.agent or plan.get("agent"))
    failed = 0
    try:
        for index, step in enumerate(steps, 1):
            op = step["op"]
            ok, result = mcp.call(op, step.get("args", {}))
            title = f"{index} {op}" + (f" ({step['name']})" if "name" in step else "")
            if not ok:
                print(f"FAIL {title}: {result}")
                failed += 1
                if not step.get("continue"):
                    break
                continue
            notes = [check(result, expect) for expect in step.get("expect", [])]
            passed = all(note_ok for note_ok, _ in notes)
            print(f"{'PASS' if passed else 'FAIL'} {title}")
            for note_ok, text in notes:
                print(f"    {'ok ' if note_ok else 'BAD'} {text}")
            if isinstance(result, dict) and result.get("settled") is False:
                print("    note: settled false; the scene was still rebuilding")
            print("    " + summary(op, result, options.max_chars))
            if not passed:
                failed += 1
                if not step.get("continue"):
                    break
    except ConnectionError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    finally:
        mcp.close()
    print(f"{len(steps)} steps, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
