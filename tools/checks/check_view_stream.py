#!/usr/bin/env python3
"""End-to-end check of view streaming (editor_remote_view.c, ADR-131).

1. A headless server editor opens Bistro, renders at a fixed scale with
   its light icons off and serves its Scene on loopback
   (remote_view.serve).
2. A headless client editor, which opens no scene of its own (its --scene
   names no file), connects with the server's key, flies its Scene camera
   across the square for six seconds and then holds the plaza view. The
   server applies every pose the client sends, so its camera ends where
   the client's did. The client reports remote_view.status and captures
   its Scene panel, which shows the decoded remote image.
3. The server captures its own Scene at the pose the client left it in.

The check passes when the client showed frames with no decode failure, its
pose-to-display latency was measured, and its capture matches the server's
within the codec's loss (PSNR of at least --min-psnr dB over the rows
between the Scene toolbar and the bottom overlays). It prints the latency
p50 and p95 and their parts, and removes its captures and logs unless
--keep is given. `--metal-validation server` or `client` runs that side
with Metal API validation, on the spatial upscaler, and fails on a report.

    python tools/checks/check_view_stream.py \\
        --editor build_release/editor/vkr_editor
"""

import argparse
import math
import pathlib
import re
import shutil
import subprocess
import sys
import time

from check_asset_stream import agent_results, captures, png_pixels
from check_editor_session import ROOT, SCENE, editor_env, read

VIEW = '"area":"scene","max_width":960'

SERVER_SCRIPT = f"""wait.scene
gfx.dynamic = false
labels off
op remote_view.serve {{{{"bind":"127.0.0.1:{{port}}"}}}}
echo serving
{{polls}}
op remote_view.status
op view.capture {{{{{VIEW}}}}}
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
op remote_view.connect {{"address":"127.0.0.1:{port}","key":"{key}"}}
wait 5
op remote_view.status
{motion}
{final}
wait 8
op remote_view.status
op view.capture {{""" + VIEW + ""","settle":false}}
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
           autoclose: int, scene: str, validation: bool):
    """A headless editor on `scene` that closes after `autoclose` seconds,
    with Metal API validation when `validation` is set."""
    work.mkdir(parents=True, exist_ok=True)
    out = open(log, "w", encoding="utf-8", errors="replace")
    env = editor_env(work, autoclose)
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--editor", required=True)
    parser.add_argument("--port", type=int, default=7342)
    parser.add_argument("--work", default=str(ROOT / ".scratch" /
                                              "view-stream-check"))
    parser.add_argument("--min-psnr", type=float, default=30.0)
    parser.add_argument("--timeout", type=float, default=600.0)
    parser.add_argument("--keep", action="store_true")
    # One Metal validation process at a time: the server's export path or
    # the client's import path.
    parser.add_argument("--metal-validation", choices=("server", "client"))
    args = parser.parse_args()
    args.editor = str(pathlib.Path(args.editor).resolve())
    work = pathlib.Path(args.work)
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    server_log = work / "server.log"
    client_log = work / "client.log"
    server = client = None
    server_out = client_out = None
    made = []
    try:
        polls = "\n".join([SERVER_POLL] * 45)
        server, server_out = launch(
            args.editor, work / "server",
            SERVER_SCRIPT.format(port=args.port, polls=polls), server_log,
            900, SCENE, args.metal_validation == "server")
        deadline = time.time() + args.timeout
        key = None
        while time.time() < deadline and server.poll() is None and not key:
            match = re.search(r"Serving the Scene on \S+, key ([0-9a-f]{64})",
                              read(server_log))
            key = match.group(1) if match else None
            time.sleep(1.0)
        if not key:
            print("FAIL: the server did not serve; see", server_log)
            return 1
        motion = "\n".join(f"{glide(pose)}\nwait 0.25" for pose in PATH)
        client, client_out = launch(
            args.editor, work / "client",
            CLIENT_SCRIPT.format(port=args.port, key=key, motion=motion,
                                 final=glide(FINAL)),
            client_log, 300, str(work / "client" / "no.scene.json"),
            args.metal_validation == "client")
        for process in (client, server):
            try:
                process.wait(timeout=max(1.0, deadline - time.time()))
            except subprocess.TimeoutExpired:
                stop(process)
    finally:
        stop(client)
        stop(server)
        for out in (client_out, server_out):
            if out:
                out.close()

    server_text = read(server_log)
    client_text = read(client_log)
    failures = []
    client_statuses = [s for s in statuses(client_text)
                       if s.get("role") == "client"]
    server_statuses = [s for s in statuses(server_text)
                       if s.get("role") == "server"]
    client_captures = captures(client_text)
    server_captures = captures(server_text)
    made += client_captures + server_captures
    if "client-done" not in client_text:
        failures.append("the client script did not finish")
    if "server-done" not in server_text:
        failures.append("the server script did not finish")
    final = client_statuses[-1] if client_statuses else {}
    if not final:
        failures.append("the client reported no remote_view.status")
    else:
        if final.get("frames_shown", 0) < 30:
            failures.append(f"the client showed {final.get('frames_shown')} "
                            "frames")
        if final.get("decode_failures", 0):
            failures.append(f"{final['decode_failures']} frames failed to "
                            "decode")
        if not final.get("samples"):
            failures.append("no pose-to-display latency was measured")
    sent = max((s.get("frames_sent", 0) for s in server_statuses), default=0)
    if not sent:
        failures.append("the server sent no frames")
    if not client_captures or not server_captures:
        failures.append("a capture is missing")
        quality = None
    else:
        quality = psnr(client_captures[-1], server_captures[-1])
        if quality[0] is None:
            failures.append("the captures differ in size: client %dx%d, "
                            "server %dx%d" % quality[2])
        elif quality[0] < args.min_psnr:
            failures.append(f"the client's image is {quality[0]:.1f} dB from "
                            f"the server's (< {args.min_psnr} dB)")

    server_final = server_statuses[-1] if server_statuses else {}
    print(f"stream {server_final.get('width')}x{server_final.get('height')} "
          f"frames sent {sent}, key frames {server_final.get('key_frames')}, "
          f"refreshes {server_final.get('refreshes')}, send failures "
          f"{server_final.get('send_failures')}, bitrate "
          f"{server_final.get('bitrate')}")
    if final:
        print(f"client frames received {final.get('frames_received')} "
              f"decoded {final.get('frames_decoded')} shown "
              f"{final.get('frames_shown')} dropped "
              f"{final.get('frames_dropped')} (late "
              f"{final.get('frames_late')}, gaps {final.get('frame_gaps')}) "
              f"refreshes sent {final.get('refreshes_sent')}")
        print("pose-to-display latency ms: p50 %.1f p95 %.1f over %d frames"
              % (final.get("latency_ms_p50", 0), final.get("latency_ms_p95", 0),
                 final.get("samples", 0)))
        print("  client (arrival to display) p50 %.1f p95 %.1f; decode p50 "
              "%.1f p95 %.1f; server render %.1f encode %.1f (last frame)"
              % (final.get("client_ms_p50", 0), final.get("client_ms_p95", 0),
                 final.get("decode_ms_p50", 0), final.get("decode_ms_p95", 0),
                 final.get("server_render_ms", 0),
                 final.get("server_encode_ms", 0)))
    if server_final:
        print("  server render p50 %.1f p95 %.1f; encode p50 %.1f p95 %.1f"
              % (server_final.get("render_ms_p50", 0),
                 server_final.get("render_ms_p95", 0),
                 server_final.get("encode_ms_p50", 0),
                 server_final.get("encode_ms_p95", 0)))
    if quality and quality[0] is not None:
        print(f"client vs server capture: PSNR {quality[0]:.1f} dB, mean "
              f"difference {quality[1]:.2f} of 255")
    if args.metal_validation:
        log = server_text if args.metal_validation == "server" else client_text
        reports = [line for line in log.splitlines()
                   if "validation" in line.lower() and
                   ("error" in line.lower() or "failed" in line.lower())]
        print(f"Metal validation on the {args.metal_validation}: "
              f"{len(reports)} reports")
        for line in reports[:10]:
            print("  ", line.strip()[:200])
        if reports:
            failures.append("Metal validation reported errors")
    for failure in failures:
        print("FAIL:", failure)
    print("server log:", server_log)
    print("client log:", client_log)
    if not args.keep and not failures:
        for path in made:
            path.unlink(missing_ok=True)
        shutil.rmtree(work, ignore_errors=True)
    if failures:
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
