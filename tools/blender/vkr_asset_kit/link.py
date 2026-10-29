"""Live link to a running VKR editor through its agent channel (ADR-084).

The editor listens on a local socket: `--agent-socket <path>`, else
`<temp>/vkr/editor-<uid>.sock` (a second editor adds `-<pid>`). Each request
is one JSON line `{"v": 1, "id": n, "op": ..., "args": {...}, "agent": ...}`
and each answer one JSON line with `ok` and `result` or `error`.
"""
import glob
import json
import os
import socket
import tempfile
import time

AGENT = "blender-asset-kit"


def default_sockets():
    """Editor sockets on this machine, the default one first."""
    directory = os.path.join(os.environ.get("TMPDIR", tempfile.gettempdir()), "vkr")
    uid = os.getuid() if hasattr(os, "getuid") else 0
    first = os.path.join(directory, f"editor-{uid}.sock")
    others = sorted(p for p in glob.glob(os.path.join(directory, f"editor-{uid}-*.sock")) if p != first)
    return [p for p in [first] + others if os.path.exists(p)]


class EditorLink:
    """One connection to an editor's agent channel."""

    def __init__(self, path=None, timeout=10.0):
        candidates = [path] if path else default_sockets()
        if not candidates:
            raise ConnectionError("No VKR editor is listening: start the editor (its agent channel is on by "
                                  "default) or set Editor socket in the VKR panel.")
        last = None
        for candidate in candidates:
            try:
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self.sock.settimeout(timeout)
                self.sock.connect(candidate)
                self.path = candidate
                break
            except OSError as error:
                last = error
                self.sock = None
        if self.sock is None:
            raise ConnectionError(f"Cannot connect to {candidates[0]}: {last}")
        self.reader = self.sock.makefile("r", encoding="utf-8")
        self.next_id = 0

    def call(self, op, args=None, timeout=120.0):
        """Runs one operation; returns its result or raises with the
        editor's message."""
        self.next_id += 1
        request = {"v": 1, "id": self.next_id, "op": op, "args": args or {}, "agent": AGENT}
        self.sock.settimeout(timeout)
        self.sock.sendall((json.dumps(request) + "\n").encode("utf-8"))
        line = self.reader.readline()
        if not line:
            raise ConnectionError("The editor closed the agent channel")
        answer = json.loads(line)
        if not answer.get("ok"):
            error = answer.get("error", {})
            raise RuntimeError(f"{op}: {error.get('message', error)}")
        return answer.get("result")

    def wait_jobs(self, timeout=900.0):
        """Waits until the editor's jobs (imports, cooks) settle."""
        self.call("cmd", {"line": "wait.scene"}, timeout=timeout)

    def close(self):
        try:
            self.reader.close()
            self.sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def reimport(link, asset, source, lightmap_uvs=False, attempts=600):
    """Reimports `asset` from `source`, retrying while another job runs."""
    for _ in range(attempts):
        try:
            link.call("model.reimport", {"asset": asset, "source": source, "lightmap_uvs": lightmap_uvs})
            link.wait_jobs()
            return
        except RuntimeError as error:
            if "wait for its jobs" not in str(error):
                raise
            time.sleep(1.0)
    raise TimeoutError(f"The editor stayed busy; {asset} was not reimported")
