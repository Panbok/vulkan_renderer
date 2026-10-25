#!/usr/bin/env python3
"""End-to-end check of asset streaming from a depot (vkr_depot_remote.h,
docs/proposals/network-protocol.md, "Asset streaming").

1. A headless editor opens Bistro from this checkout with VKR_VFS_RECORD
   set, waits and captures one view; a second local run captures it again,
   which measures how much two identical loads differ.
2. Every file below `assets/` that run read is copied into a depot working
   copy in the repository's layout and committed to `main` of a loopback
   depot; the cooked `.vkt` textures, which the ignore rules keep out of the
   history, go to `main.cooked.<family>` with `vkr_depot cooked`.
3. A second headless editor starts with `--stream` into an empty directory
   and opens the same scene: every read the branch lists arrives from the
   depot, and a material's texture larger than 1 MiB first draws from the
   smallest mips at the start of its file (a preview), then, once a frame
   draws it, from the whole file (ADR-123). It reports `stream.status` and
   its pending and preview textures when the scene is ready and every 2 s
   for `--settle` seconds, then captures the same view.

With `--hide-after SECONDS`, the streamed run hides Bistro's geometry that
many seconds after the scene is ready instead of capturing, so no frame
draws its materials (texture demand follows the world's visible sources,
not the camera), and the check passes when queued downloads were
cancelled, none failed, every written file equals the local one and
nothing arrived in the last 15 s.

With `--remote ADDRESS KEY`, step 2 commits to a depot another machine
serves at ADDRESS with server key KEY instead of a loopback one, and the
streamed run reads from it over the network. Serve a fresh store there
(`vkr_depot serve <store> --bind 0.0.0.0:<port> --open`): the check pushes
`main` and this platform's cooked branch, since each platform streams only
its own texture family (`vkr_depot_cooked_branch`), so the files still
compare byte for byte with the local ones.

Both local runs report their pending textures every second, so the first
gives the local times to the scene and to every texture loaded. Every run
captures `--settle` seconds after its scene is ready: Bistro's final colour
drifts while it runs (0.46 levels of 255 between 30 s and 100 s of one
local run on the Mac), so captures taken at other times would differ by
more than streaming changes.

The check passes when the streamed editor read previews and fetched files
and none failed, every file it wrote equals the local file byte for byte,
no file arrived in the 15 s before its capture (it had reached full detail),
and its capture differs from the first local one by no more than the second
local one does (with a floor of 0.5 levels of 255, since final colour varies
between identical runs). It prints the times to the scene, to the last
preview read and to the last fetched file against the local times, and
removes its depot and stream directory afterwards.

    python tools/checks/check_asset_stream.py \\
        --editor build_release/editor/vkr_editor.exe \\
        --depot build_release/tools/depot/vkr_depot.exe
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib

from check_editor_session import ROOT, SCENE, editor_env

VIEW = '"eye":[-24,5,-14],"target":[0,3,0],"max_width":960'

# Dynamic resolution follows recent GPU frame times, which a streamed run's
# reloads raise, so every run renders at the fixed scale.
# A local run counts its pending textures every second, then captures.
LOCAL_SCRIPT = f"""wait.scene
gfx.dynamic = false
stats.pending_textures
{{polls}}
op view.capture {{{{{VIEW}}}}}
wait 3
echo capture-done
"""
LOCAL_POLL = "wait 1\nstats.pending_textures"

# The streamed run reports every 2 s while its files arrive, then captures.
STREAM_SCRIPT = f"""wait.scene
gfx.dynamic = false
op stream.status
stats.pending_textures
stats.preview_textures
{{polls}}
op stream.status
op view.capture {{{{{VIEW}}}}}
wait 3
op stream.status
stats.preview_textures
echo capture-done
"""
STREAM_POLL = ("wait 2\nop stream.status\nstats.pending_textures\n"
               "stats.preview_textures")

# The streamed run hides Bistro while its files arrive.
HIDE_SCRIPT = """wait.scene
op stream.status
wait {after}
op stream.status
op entity.set {{"entity":"Bistro Lights","visible":false}}
{polls}
op stream.status
echo capture-done
quit discard
"""


def png_pixels(path: pathlib.Path):
    """Decodes an 8-bit, non-interlaced RGB or RGBA PNG: (width, height,
    channels, rows of bytes)."""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    offset = 8
    width = height = channels = 0
    idat = b""
    while offset < len(data):
        length, kind = struct.unpack(">I4s", data[offset:offset + 8])
        body = data[offset + 8:offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(
                ">IIBBBBB", body)
            assert depth == 8 and interlace == 0 and color in (2, 6), path
            channels = 3 if color == 2 else 4
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        row = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if kind == 1:
                row[x] = (row[x] + left) & 255
            elif kind == 2:
                row[x] = (row[x] + up) & 255
            elif kind == 3:
                row[x] = (row[x] + ((left + up) >> 1)) & 255
            elif kind == 4:
                p = left + up - corner
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - corner)
                predictor = left if pa <= pb and pa <= pc else (
                    up if pb <= pc else corner)
                row[x] = (row[x] + predictor) & 255
        rows.append(bytes(row))
        previous = row
    return width, height, channels, rows


def compare(a: pathlib.Path, b: pathlib.Path):
    """Mean absolute difference per channel (0-255) and the share of pixels
    that differ by more than 2 in any channel."""
    wa, ha, ca, rows_a = png_pixels(a)
    wb, hb, cb, rows_b = png_pixels(b)
    if (wa, ha, ca) != (wb, hb, cb):
        return None, None
    total = 0
    differing = 0
    for row_a, row_b in zip(rows_a, rows_b):
        for x in range(0, len(row_a), ca):
            worst = 0
            for c in range(3):
                d = abs(row_a[x + c] - row_b[x + c])
                total += d
                worst = max(worst, d)
            differing += worst > 2
    return total / (wa * ha * 3), differing / (wa * ha)


def agent_results(text: str) -> list:
    found = []
    for match in re.finditer(r"\[agent\] (\{.*\})", text):
        try:
            found.append(json.loads(match.group(1)))
        except json.JSONDecodeError:
            pass
    return found


def captures(text: str) -> list:
    paths = []
    for answer in agent_results(text):
        result = answer.get("result")
        if isinstance(result, dict) and result.get("path", "").endswith(
                ".png"):
            paths.append(pathlib.Path(result["path"]))
    return paths


def cmd_values(text: str, statement: str) -> list:
    """The numbers an `--exec` expression printed, in order: each
    `[cmd] > statement` echo followed by its `[cmd] <value>` line."""
    values = []
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.rstrip().endswith(f"[cmd] > {statement}"):
            for following in lines[i + 1:i + 40]:
                if "[cmd] >" in following or "[cmd] error" in following:
                    break
                match = re.search(r"\[cmd\] (-?[0-9.]+)\s*$", following)
                if match:
                    values.append(float(match.group(1)))
                    break
    return values


def scene_seconds(text: str):
    """The scene load time the editor logged, or None."""
    match = re.search(r"SCENE_LOAD_TIME result=ready seconds=([0-9.]+)", text)
    return float(match.group(1)) if match else None


def stream_statuses(text: str) -> list:
    return [answer["result"] for answer in agent_results(text)
            if isinstance(answer.get("result"), dict) and
            "streaming" in answer["result"]]


def run_editor(editor: str, work: pathlib.Path, script: str, log: pathlib.Path,
               extra_args: list, env_extra: dict, timeout: float) -> str:
    work.mkdir(parents=True, exist_ok=True)
    env = editor_env(work, autoclose=int(timeout))
    env.update(env_extra)
    with open(log, "w", encoding="utf-8", errors="replace") as out:
        process = subprocess.Popen(
            [editor, "--headless", "--no-agent-socket", *extra_args,
             "--scene", SCENE, "--exec", script],
            cwd=ROOT, env=env, stdout=out, stderr=subprocess.STDOUT)
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
    return log.read_text(encoding="utf-8", errors="replace")


def depot(command: list, env: dict, log) -> str:
    log.write("$ " + " ".join(command) + "\n")
    result = subprocess.run(command, env=env, capture_output=True, text=True,
                            encoding="utf-8", errors="replace", cwd=ROOT)
    log.write(result.stdout + result.stderr)
    log.flush()
    if result.returncode != 0:
        raise RuntimeError(f"{command[1]} failed: {result.stderr.strip()}")
    return result.stdout


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--depot", required=True)
    parser.add_argument("--port", type=int, default=7353)
    parser.add_argument("--settle", type=int, default=120,
                        help="seconds every run waits after its scene is "
                        "ready before its capture")
    parser.add_argument("--hide-after", type=int, default=0,
                        help="seconds after the scene is ready at which the "
                        "streamed run hides Bistro instead of capturing, to "
                        "check that queued downloads stop")
    parser.add_argument("--work", default=str(ROOT / ".scratch" /
                                              "asset-stream"))
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--keep", action="store_true",
                        help="keep the depot and stream directory")
    parser.add_argument("--remote", nargs=2, metavar=("ADDRESS", "KEY"),
                        help="commit to and stream from a fresh store "
                        "another machine serves at ADDRESS with server KEY "
                        "instead of a loopback depot")
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
        # 1. The local run and its reads.
        reads = work / "reads.txt"
        local_script = LOCAL_SCRIPT.format(
            polls="\n".join([LOCAL_POLL] * args.settle))
        local_text = run_editor(
            editor, work / "local", local_script, work / "local.log", [],
            {"VKR_VFS_RECORD": str(reads)}, args.timeout)
        local_captures = captures(local_text)
        made += local_captures
        if "capture-done" not in local_text or not local_captures:
            raise RuntimeError("the local run did not capture; see "
                               f"{work / 'local.log'}")
        again_text = "" if args.hide_after else run_editor(
            editor, work / "again", local_script, work / "again.log", [], {},
            args.timeout)
        again_captures = captures(again_text)
        made += again_captures
        if not args.hide_after and ("capture-done" not in again_text or
                                not again_captures):
            raise RuntimeError("the second local run did not capture; see "
                               f"{work / 'again.log'}")
        identities = sorted(
            {line.strip()
             for line in reads.read_text(encoding="utf-8",
                                         errors="replace").splitlines()
             if line.startswith("assets/")})
        files = [i for i in identities if (ROOT / i).is_file()]

        # 2. The depot: main holds the files, the side branch the cooked.
        depot([depot_exe, "keygen"], env, log)
        if args.remote:
            address, key = args.remote
        else:
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
        cooked = 0
        seed_bytes = 0
        for identity in files:
            target = seed / identity
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / identity, target)
            cooked += identity.endswith(".vkt")
            seed_bytes += target.stat().st_size
        (seed / ".vkrdepotignore").write_text("*.vkt\n", encoding="utf-8")
        push_start = time.monotonic()
        depot([depot_exe, "commit", str(seed), "-m", "Bistro", "--push"], env,
              log)
        published = depot([depot_exe, "cooked", str(seed)], env, log)
        push_seconds = time.monotonic() - push_start

        # 3. The streamed run.
        stream_dir = work / "stream"
        polls = "\n".join(["wait 2", "op stream.status"] *
                           (args.settle // 2)) if args.hide_after else "\n".join(
                               [STREAM_POLL] * (args.settle // 2))
        script = HIDE_SCRIPT.format(
            after=args.hide_after,
            polls=polls) if args.hide_after else STREAM_SCRIPT.format(
                polls=polls)
        stream_text = run_editor(
            editor, work / "streamed", script, work / "stream.log",
            ["--stream", address, key, str(stream_dir)], {}, args.timeout)
        statuses = stream_statuses(stream_text)
        stream_captures = captures(stream_text)
        made += stream_captures
        if "capture-done" not in stream_text or (not args.hide_after and
                                                 not stream_captures):
            raise RuntimeError("the streamed run did not capture; see "
                               f"{work / 'stream.log'}")
        if not statuses or not statuses[0].get("streaming"):
            raise RuntimeError("the editor did not stream; see "
                               f"{work / 'stream.log'}")
        first, last = statuses[0], statuses[-1]
        # The status just before the capture: nothing may have arrived in
        # the 15 s before it.
        before = statuses[-2] if len(statuses) >= 2 else last
        if before.get("since_open_ms", 0) - before.get("last_fetch_ms", 0) < \
                15000.0:
            failures.append("files still arrived within 15 s of the capture; "
                            "raise --settle")
        if last.get("fetched", 0) == 0:
            failures.append("the streamed editor fetched no file")
        if last.get("reads", 0) == 0:
            failures.append("the streamed editor read no preview")
        if args.hide_after and last.get("cancelled", 0) == 0:
            failures.append("hiding Bistro cancelled no queued download")
        if last.get("failed", 0):
            failures.append(f"{last['failed']} fetches failed")
        # Every streamed file holds the local file's bytes.
        mismatched = []
        written = 0
        for path in stream_dir.rglob("*"):
            relative = path.relative_to(stream_dir).as_posix()
            if not path.is_file() or relative.startswith(".vkrdepot"):
                continue
            written += 1
            if path.read_bytes() != (ROOT / relative).read_bytes():
                mismatched.append(relative)
        if mismatched:
            failures.append(f"{len(mismatched)} streamed files differ from "
                            f"the local ones, such as {mismatched[0]}")
        noise_mean = final_mean = None
        if not args.hide_after:
            noise_mean, noise_share = compare(local_captures[0],
                                              again_captures[0])
            final_mean, final_share = compare(local_captures[0],
                                              stream_captures[0])
            if final_mean is None or noise_mean is None:
                failures.append("the captures differ in size")
            elif final_mean > max(1.5 * noise_mean, 0.5):
                failures.append(f"the streamed capture differs by "
                                f"{final_mean:.3f} against {noise_mean:.3f} "
                                f"between local runs")
        if args.hide_after and len(statuses) >= 2:
            at_switch = statuses[1]
            print(f"hidden at "
                  f"{at_switch.get('since_open_ms', 0):.0f} ms: "
                  f"{at_switch.get('fetched', 0)} files fetched, "
                  f"{at_switch.get('prefetched', 0)} prefetched by then; at "
                  f"the end {last.get('fetched', 0)} of "
                  f"{last.get('listed', 0)} fetched, "
                  f"{last.get('prefetched', 0)} prefetched, "
                  f"{last.get('cancelled', 0)} cancelled")
        print(f"seed: {len(files)} files read by the local run, {cooked} "
              f"cooked, {seed_bytes / 1e6:.1f} MB, committed and pushed to "
              f"{address} in {push_seconds:.1f} s; {published.strip()}")
        print(f"stream: {last.get('listed')} listed, {last.get('fetched')} "
              f"fetched, {last.get('failed')} failed, "
              f"{last.get('stored_bytes', 0) / 1e6:.1f} MB stored; branch "
              f"listed in {first.get('open_ms', 0):.0f} ms")
        print(f"previews: {last.get('reads', 0)} reads brought "
              f"{last.get('read_bytes', 0) / 1e6:.1f} MB; "
              f"{last.get('prefetched', 0)} whole files queued after them")
        print(f"times since the stream opened: scene ready "
              f"{first.get('since_open_ms', 0):.0f} ms, last preview read "
              f"{last.get('last_read_ms', 0):.0f} ms, last file "
              f"{last.get('last_fetch_ms', 0):.0f} ms; loaders waited "
              f"{last.get('wait_ms', 0):.0f} ms in all")
        previews = cmd_values(stream_text, "stats.preview_textures")
        pending = cmd_values(stream_text, "stats.pending_textures")
        if previews:
            print(f"preview textures: {previews[0]:.0f} at the scene, "
                  f"{max(previews):.0f} at most, {previews[-1]:.0f} at the "
                  f"capture (a preview no frame draws keeps its small mips)")
        if pending:
            # Each poll reads stream.status, then the pending textures.
            settled = next((statuses[i].get("since_open_ms", 0)
                            for i in range(min(len(statuses), len(pending)))
                            if pending[i] == 0), None)
            print(f"streamed pending textures: {pending[0]:.0f} at the "
                  f"scene, {pending[-1]:.0f} before the capture; none "
                  f"pending from "
                  f"{'never' if settled is None else f'{settled:.0f} ms'} "
                  f"after the stream opened")
        local_pending = cmd_values(local_text, "stats.pending_textures")
        local_scene = scene_seconds(local_text)
        loaded = next((i for i, count in enumerate(local_pending)
                       if count == 0), None)
        print(f"local: scene ready {local_scene or 0:.1f} s after its load "
              f"began; every texture loaded about "
              f"{'never' if loaded is None else f'{loaded} s'} after the "
              f"scene")
        print(f"files: {written} written under the stream directory, "
              f"{len(mismatched)} differ from the local ones")
        if final_mean is not None and noise_mean is not None:
            print(f"capture: streamed against local, mean difference "
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
            for name in ("store", "seed", "stream"):
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
