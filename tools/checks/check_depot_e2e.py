#!/usr/bin/env python3
"""End-to-end check of vkr_depot over loopback UDP (ADR-105).

Runs a server and three users through init, access, clone, commit, a second
clone, an edit, a pull that keeps a local change, a refused stale push,
locks and a refused key. Every step checks the files or the command's
result; any mismatch exits nonzero.

Example:
  python3 tools/checks/check_depot_e2e.py build_release/tools/depot/vkr_depot
"""

import argparse
import hashlib
import os
import pathlib
import random
import shutil
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("depot", help="path to the vkr_depot executable")
    parser.add_argument("--port", type=int, default=7341)
    parser.add_argument("--keep", action="store_true",
                        help="keep the temporary directory")
    args = parser.parse_args()
    exe = str(pathlib.Path(args.depot).resolve())
    root = pathlib.Path(tempfile.mkdtemp(prefix="vkr_depot_e2e_"))
    address = f"127.0.0.1:{args.port}"
    identity = {name: dict(os.environ,
                           VKR_DEPOT_IDENTITY=str(root / f"{name}.key"))
                for name in ("alice", "bob", "carol")}

    def run(command, user="alice", ok=True):
        result = subprocess.run([exe] + command, env=identity[user],
                                capture_output=True, text=True, timeout=120)
        output = (result.stdout + result.stderr).strip()
        print(f"$ vkr_depot {' '.join(command)}\n{output}")
        if ok != (result.returncode == 0):
            sys.exit(f"unexpected result for: {command}")
        return output

    def digest(directory):
        files = {}
        for path in sorted(pathlib.Path(directory).rglob("*")):
            if ".vkrdepot" in path.parts or not path.is_file():
                continue
            files[path.relative_to(directory).as_posix()] = hashlib.sha256(
                path.read_bytes()).hexdigest()
        return files

    store = root / "store"
    server_key = run(["init", str(store)]).split("server key ")[1].split()[0]
    alice = run(["keygen"]).split("identity ")[1].split()[0]
    bob = run(["keygen"], "bob").split("identity ")[1].split()[0]
    run(["access", str(store), alice, "write"])
    run(["access", str(store), bob, "write"])

    server = subprocess.Popen([exe, "serve", str(store), "--bind", address],
                              stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)
    time.sleep(1.0)
    try:
        work_a = root / "alice"
        run(["clone", address, server_key, str(work_a)])
        rng = random.Random(7)
        (work_a / "textures").mkdir()
        (work_a / "textures" / "big.bin").write_bytes(rng.randbytes(3_500_000))
        (work_a / "textures" / "zeros.bin").write_bytes(bytes(2_000_000))
        (work_a / "scene.json").write_bytes(b'{"entities": []}\n')
        (work_a / "deep" / "a" / "b").mkdir(parents=True)
        (work_a / "deep" / "a" / "b" / "c.txt").write_bytes(b"nested\n")
        (work_a / "empty.dat").write_bytes(b"")
        run(["status", str(work_a)])
        run(["commit", str(work_a), "-m", "first assets"])

        work_b = root / "bob"
        run(["clone", address, server_key, str(work_b)], "bob")
        if digest(work_a) != digest(work_b):
            sys.exit("the clone differs from the pushed tree")

        # A 10-byte edit inside a large file: few chunks travel.
        data = bytearray((work_b / "textures" / "big.bin").read_bytes())
        data[1_700_000:1_700_010] = b"0123456789"
        (work_b / "textures" / "big.bin").write_bytes(bytes(data))
        (work_b / "scene.json").unlink()
        (work_b / "new.txt").write_bytes(b"bob was here\n")
        run(["commit", str(work_b), "-m", "bob edits"], "bob")

        (work_a / "deep" / "a" / "b" / "c.txt").write_bytes(b"alice local\n")
        run(["pull", str(work_a)])
        expected = digest(work_b)
        expected["deep/a/b/c.txt"] = hashlib.sha256(b"alice local\n").hexdigest()
        if digest(work_a) != expected:
            sys.exit("the pull result differs")

        (work_b / "new.txt").write_bytes(b"bob again\n")
        run(["commit", str(work_b), "-m", "bob again"], "bob")
        run(["commit", str(work_a), "-m", "alice stale"], ok=False)

        run(["pull", str(work_a)])
        run(["lock", "textures/zeros.bin", "--dir", str(work_b)], "bob")
        run(["locks", str(work_a)])
        (work_a / "textures" / "zeros.bin").write_bytes(bytes(10))
        run(["commit", str(work_a), "-m", "touch locked"], ok=False)
        run(["unlock", "textures/zeros.bin", "--dir", str(work_a)], ok=False)
        run(["unlock", "textures/zeros.bin", "--dir", str(work_b)], "bob")
        run(["commit", str(work_a), "-m", "after unlock"])
        run(["log", str(work_a)])
        run(["refs", str(work_a)])

        run(["keygen"], "carol")
        run(["clone", address, server_key, str(root / "carol")], "carol",
            ok=False)
        print("depot end-to-end check passed")
    finally:
        server.terminate()
        try:
            server.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
        if not args.keep:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    main()
