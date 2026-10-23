#!/usr/bin/env python3
"""End-to-end check of view streaming (editor_remote_view.c, ADR-131).

1. A headless server editor opens Bistro, renders at a fixed scale with
   its light icons off and serves its Scene (remote_view.serve).
2. A headless client editor, which opens no scene of its own (its --scene
   names no file), connects with the server's key, flies its Scene camera
   across the square for six seconds and then holds the plaza view. The
   server applies every pose the client sends, so its camera ends where
   the client's did. The client reports remote_view.status and captures
   its Scene panel, which shows the decoded remote image.
3. The server captures its own Scene at the pose the client left it in.

`--role both` (the default) runs both editors on this machine over
loopback. It passes when the client showed frames with no decode failure,
its pose-to-display latency was measured, and its capture matches the
server's within the codec's loss (PSNR of at least --min-psnr dB over the
rows between the Scene toolbar and the bottom overlays). It prints the
latency p50 and p95 and their parts, and removes its captures and logs
unless --keep is given. `--metal-validation server` or `client` runs that
side with Metal API validation, on the spatial upscaler, and fails on a
report.

Between two computers each runs one half. The server prints its address
and key and serves for --hold seconds, then captures:

    python tools/checks/check_view_stream.py --editor <editor> \\
        --role server --bind 0.0.0.0:7340 --allow <client identity key>

The client connects within that time, flies, captures and reports:

    python tools/checks/check_view_stream.py --editor <editor> \\
        --role client --address <server ip>:7340 --key <server key>

Each half keeps its capture as <work>/server.png or <work>/client.png;
`--compare CLIENT.png SERVER.png` measures them once one is copied across.
Both halves need the same headless window, so the Scene panels match.

    python tools/checks/check_view_stream.py \\
        --editor build_release/editor/vkr_editor
"""

import argparse
import json
import math
import pathlib
import re
import shutil
import subprocess
import sys
import time

from check_asset_stream import agent_results, captures, png_pixels
from check_editor_session import ROOT, SCENE, editor_env, read

CAPTURE = {"area": "scene", "max_width": 960}

SERVER_SCRIPT = """wait.scene
gfx.dynamic = false
labels off
op remote_view.serve {serve}
echo serving
{polls}
op remote_view.status
op view.capture {capture}
wait 3
echo server-done
quit discard
"""
SERVER_POLL = "wait 2\nop remote_view.status"

# The client flies across the square for six seconds and holds the plaza
# view the tiled Bistro plaza cases use (eye 10, 4.2, 27.8, yaw 19, pitch
# -22).
FINAL = ((10.0, 4.2, 27.8), (18.77, 0.45, 30.82))
START = ((-24.0, 5.0, -14.0), (0.0, 3.0, 0.0))
PATH = [tuple(tuple(a + (b - a) * i / 23.0 for a, b in zip(start, final))
              for start, final in zip(START, FINAL))
        for i in range(24)]


def glide(pose) -> str:
    (ex, ey, ez), (tx, ty, tz) = pose
    return ("op view.camera {"
            f'"eye":[{ex},{ey},{ez}],"target":[{tx},{ty},{tz}],'
            '"glide":true}')


CLIENT_SCRIPT = """wait 2
op remote_view.connect {connect}
wait 5
op remote_view.status
{motion}
{final}
wait 8
op remote_view.status
op view.capture {capture}
wait 3
op remote_view.status
echo client-done
quit discard
"""


def statuses(text: str) -> list:
    found = []
    for answer in agent_results(text):
        result = answer.get("result")
        if isinstance(result, dict) and "role" in result:
            found.append(result)
    return found


def psnr(a: pathlib.Path, b: pathlib.Path):
    """PSNR over RGB in dB and the mean absolute difference (0-255), or
    None when the images differ in size. The Scene toolbar along the top
    and the orientation gizmo and frame time along the bottom are editor
    UI that differs between the editors, so only the rows between them
    count."""
    wa, ha, ca, rows_a = png_pixels(a)
    wb, hb, cb, rows_b = png_pixels(b)
    if (wa, ha) != (wb, hb):
        return None, None, (wa, ha, wb, hb)
    first, last = int(ha * 0.08), int(ha * 0.86)
    squared = 0
    absolute = 0
    for row_a, row_b in zip(rows_a[first:last], rows_b[first:last]):
        for x in range(wa):
            for c in range(3):
                d = row_a[x * ca + c] - row_b[x * cb + c]
                squared += d * d
                absolute += abs(d)
    count = wa * (last - first) * 3
    mse = squared / count
    value = 99.0 if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)
    return value, absolute / count, (wa, ha, wb, hb)


def launch(editor: str, work: pathlib.Path, script: str, log: pathlib.Path,
           autoclose: int, scene: str, validation: bool, identity: str = ""):
    """A headless editor on `scene` that closes after `autoclose` seconds,
    with Metal API validation when `validation` is set. `identity` names
    the identity key file it connects with; without it the editor makes a
    fresh one in `work`."""
    work.mkdir(parents=True, exist_ok=True)
    out = open(log, "w", encoding="utf-8", errors="replace")
    env = editor_env(work, autoclose)
    if identity:
        env["VKR_DEPOT_IDENTITY"] = identity
    if validation:
        env["MTL_DEBUG_LAYER"] = "1"
        # MetalFX's temporal scaler aborts under the debug layer.
        (work / "graphics.json").write_text(
            '{"version": 1, "upscaler": "spatial"}\n')
    process = subprocess.Popen(
        [editor, "--headless", "--no-agent-socket", "--scene", scene,
         "--exec", script],
        cwd=ROOT, env=env, stdout=out, stderr=subprocess.STDOUT)
    return process, out


def stop(process) -> None:
    if process and process.poll() is None:
        process.kill()
        process.wait()


def wait_for_key(process, log: pathlib.Path, deadline: float):
    """The server key the server editor logged, or None."""
    while time.time() < deadline and process.poll() is None:
        match = re.search(r"Serving the Scene on \S+, key ([0-9a-f]{64})",
                          read(log))
        if match:
            return match.group(1)
        time.sleep(1.0)
    return None


def finish(processes, outs, deadline: float) -> None:
    """Waits for every editor until the deadline, then closes them."""
    try:
        for process in processes:
            try:
                process.wait(timeout=max(1.0, deadline - time.time()))
            except subprocess.TimeoutExpired:
                stop(process)
    finally:
        for process in processes:
            stop(process)
        for out in outs:
            out.close()


def server_script(bind: str, allow: list, hold: float) -> str:
    serve = {"bind": bind}
    if allow:
        serve["allow"] = allow
    polls = "\n".join([SERVER_POLL] * max(1, int(hold / 2)))
    return SERVER_SCRIPT.format(serve=json.dumps(serve), polls=polls,
                                capture=json.dumps(CAPTURE))


def client_script(address: str, key: str) -> str:
    motion = "\n".join(f"{glide(pose)}\nwait 0.25" for pose in PATH)
    return CLIENT_SCRIPT.format(
        connect=json.dumps({"address": address, "key": key}), motion=motion,
        final=glide(FINAL), capture=json.dumps(dict(CAPTURE, settle=False)))


def client_report(text: str, failures: list) -> dict:
    """Checks and prints the client's half; returns its last status."""
    every = statuses(text)
    found = [s for s in every if s.get("role") == "client"]
    if "client-done" not in text:
        failures.append("the client script did not finish")
    # A view that ended reports role none, with the reason it ended.
    if every and every[-1].get("error"):
        failures.append(f"the client stopped: {every[-1]['error']}")
    final = found[-1] if found else {}
    if not final:
        failures.append("the client reported no remote_view.status")
        return final
    if final.get("frames_shown", 0) < 30:
        failures.append(f"the client showed {final.get('frames_shown')} "
                        "frames")
    if final.get("decode_failures", 0):
        failures.append(f"{final['decode_failures']} frames failed to "
                        "decode")
    if not final.get("samples"):
        failures.append("no pose-to-display latency was measured")
    print(f"client frames received {final.get('frames_received')} "
          f"decoded {final.get('frames_decoded')} shown "
          f"{final.get('frames_shown')} dropped "
          f"{final.get('frames_dropped')} (late {final.get('frames_late')}, "
          f"reordered {final.get('frames_reordered')}, gaps "
          f"{final.get('frame_gaps')}) refreshes sent "
          f"{final.get('refreshes_sent')}")
    print("pose-to-display latency ms: p50 %.1f p95 %.1f over %d frames"
          % (final.get("latency_ms_p50", 0), final.get("latency_ms_p95", 0),
             final.get("samples", 0)))
    print("  client (arrival to display) p50 %.1f p95 %.1f; decode p50 "
          "%.1f p95 %.1f; server render %.1f encode %.1f (last frame)"
          % (final.get("client_ms_p50", 0), final.get("client_ms_p95", 0),
             final.get("decode_ms_p50", 0), final.get("decode_ms_p95", 0),
             final.get("server_render_ms", 0),
             final.get("server_encode_ms", 0)))
    return final


def server_report(text: str, failures: list) -> dict:
    """Checks and prints the server's half; returns its last status."""
    found = [s for s in statuses(text) if s.get("role") == "server"]
    if "server-done" not in text:
        failures.append("the server script did not finish")
    final = found[-1] if found else {}
    sent = max((s.get("frames_sent", 0) for s in found), default=0)
    if not sent:
        failures.append("the server sent no frames")
    print(f"stream {final.get('width')}x{final.get('height')} frames sent "
          f"{sent}, key frames {final.get('key_frames')}, refreshes "
          f"{final.get('refreshes')}, send failures "
          f"{final.get('send_failures')}, bitrate {final.get('bitrate')}")
    if final:
        print("  server render p50 %.1f p95 %.1f; encode p50 %.1f p95 %.1f"
              % (final.get("render_ms_p50", 0), final.get("render_ms_p95", 0),
                 final.get("encode_ms_p50", 0), final.get("encode_ms_p95", 0)))
    return final


def compare(client: pathlib.Path, server: pathlib.Path, min_psnr: float,
            failures: list) -> None:
    value, mean, sizes = psnr(client, server)
    if value is None:
        failures.append("the captures differ in size: client %dx%d, "
                        "server %dx%d" % sizes)
        return
    print(f"client vs server capture: PSNR {value:.1f} dB, mean difference "
          f"{mean:.2f} of 255")
    if value < min_psnr:
        failures.append(f"the client's image is {value:.1f} dB from the "
                        f"server's (< {min_psnr} dB)")


def validation_report(text: str, side: str, failures: list) -> None:
    reports = [line for line in text.splitlines()
               if "validation" in line.lower() and
               ("error" in line.lower() or "failed" in line.lower())]
    print(f"Metal validation on the {side}: {len(reports)} reports")
    for line in reports[:10]:
        print("  ", line.strip()[:200])
    if reports:
        failures.append("Metal validation reported errors")


def keep_capture(text: str, path: pathlib.Path, failures: list):
    """Moves the run's last capture to `path`; None without one."""
    found = captures(text)
    if not found:
        failures.append("a capture is missing")
        return None
    shutil.copyfile(found[-1], path)
    for made in found:
        made.unlink(missing_ok=True)
    return path


def run_both(args, work: pathlib.Path, failures: list) -> list:
    server_log = work / "server.log"
    client_log = work / "client.log"
    address = f"127.0.0.1:{args.port}"
    deadline = time.time() + args.timeout
    server, server_out = launch(
        args.editor, work / "server", server_script(address, [], 90.0),
        server_log, 900, SCENE, args.metal_validation == "server")
    processes, outs = [server], [server_out]
    try:
        key = wait_for_key(server, server_log, deadline)
        if key:
            client, client_out = launch(
                args.editor, work / "client", client_script(address, key),
                client_log, 300, str(work / "client" / "no.scene.json"),
                args.metal_validation == "client")
            processes.insert(0, client)
            outs.append(client_out)
    finally:
        finish(processes, outs, deadline)
    server_text = read(server_log)
    if len(processes) == 1:
        failures.append("the server did not serve")
        return [server_log]
    client_text = read(client_log)
    server_report(server_text, failures)
    client_report(client_text, failures)
    client_png = keep_capture(client_text, work / "client.png", failures)
    server_png = keep_capture(server_text, work / "server.png", failures)
    if client_png and server_png:
        compare(client_png, server_png, args.min_psnr, failures)
    if args.metal_validation:
        validation_report(server_text if args.metal_validation == "server"
                          else client_text, args.metal_validation, failures)
    return [server_log, client_log]


def run_server(args, work: pathlib.Path, failures: list) -> list:
    log = work / "server.log"
    deadline = time.time() + args.timeout + args.hold
    server, out = launch(args.editor, work / "server",
                         server_script(args.bind, args.allow, args.hold), log,
                         int(args.timeout + args.hold), SCENE,
                         args.metal_validation == "server")
    key = None
    try:
        key = wait_for_key(server, log, deadline)
        if key:
            served = re.search(r"Serving the Scene on (\S+),", read(log))
            print(f"serving on {served.group(1) if served else args.bind} "
                  f"for {args.hold:.0f} s, key {key}", flush=True)
    finally:
        finish([server], [out], deadline)
    text = read(log)
    if not key:
        failures.append("the server did not serve")
        return [log]
    final = server_report(text, failures)
    if not any(s.get("client") for s in statuses(text)):
        failures.append("no client opened the stream")
    elif final.get("client"):
        print("note: the client was still connected at the capture")
    png = keep_capture(text, work / "server.png", failures)
    if png:
        print("server capture:", png)
    if args.metal_validation == "server":
        validation_report(text, "server", failures)
    return [log]


def run_client(args, work: pathlib.Path, failures: list) -> list:
    log = work / "client.log"
    deadline = time.time() + args.timeout
    client, out = launch(args.editor, work / "client",
                         client_script(args.address, args.key), log, 300,
                         str(work / "client" / "no.scene.json"),
                         args.metal_validation == "client", args.identity)
    finish([client], [out], deadline)
    text = read(log)
    client_report(text, failures)
    png = keep_capture(text, work / "client.png", failures)
    if png:
        print("client capture:", png)
    if args.metal_validation == "client":
        validation_report(text, "client", failures)
    return [log]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor")
    parser.add_argument("--role", choices=("both", "server", "client"),
                        default="both")
    parser.add_argument("--port", type=int, default=7342,
                        help="loopback port of --role both")
    parser.add_argument("--bind", default="0.0.0.0:7340",
                        help="server address of --role server")
    parser.add_argument("--allow", action="append", default=[],
                        help="identity key a --role server admits")
    parser.add_argument("--hold", type=float, default=180.0,
                        help="seconds a --role server serves before its "
                        "capture")
    parser.add_argument("--address", help="server of --role client")
    parser.add_argument("--key", help="server key of --role client")
    parser.add_argument("--identity", default="",
                        help="identity key file a --role client connects "
                        "with, such as the machine's, so a server's "
                        "--allow admits it")
    parser.add_argument("--compare", nargs=2, metavar=("CLIENT", "SERVER"),
                        help="only measure two captures")
    parser.add_argument("--work", default=str(ROOT / ".scratch" /
                                              "view-stream-check"))
    parser.add_argument("--min-psnr", type=float, default=30.0)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--keep", action="store_true")
    # One Metal validation process at a time: the server's export path or
    # the client's import path.
    parser.add_argument("--metal-validation", choices=("server", "client"))
    args = parser.parse_args()
    failures = []
    if args.compare:
        compare(pathlib.Path(args.compare[0]), pathlib.Path(args.compare[1]),
                args.min_psnr, failures)
    else:
        if not args.editor or (args.role == "client" and
                               not (args.address and args.key)):
            parser.error("--editor is required, and --role client needs "
                         "--address and --key")
        args.editor = str(pathlib.Path(args.editor).resolve())
        work = pathlib.Path(args.work)
        if work.exists():
            shutil.rmtree(work)
        work.mkdir(parents=True)
        logs = {"both": run_both, "server": run_server,
                "client": run_client}[args.role](args, work, failures)
        if failures or args.keep:
            for log in logs:
                print("log:", log)
        else:
            # A passing run leaves only a split half's capture, for
            # --compare.
            shutil.rmtree(work / "server", ignore_errors=True)
            shutil.rmtree(work / "client", ignore_errors=True)
            for log in logs:
                log.unlink(missing_ok=True)
            if args.role == "both":
                shutil.rmtree(work, ignore_errors=True)
    for failure in failures:
        print("FAIL:", failure)
    if failures:
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
