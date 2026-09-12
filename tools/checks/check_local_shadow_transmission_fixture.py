#!/usr/bin/env python3
"""Compare on/off/dark matrix captures against independent transmission ratios.

Usage:
  python3 tools/checks/check_local_shadow_transmission_fixture.py ON_RUN OFF_RUN DARK_RUN
Uses only the Python standard library and existing capture decoding helpers.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import sys


root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tools/checks"))
from check_clearcoat_fixture import capture_path, half_rgba, mean_patch


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("on", type=Path)
parser.add_argument("off", type=Path)
parser.add_argument("dark", type=Path)
args = parser.parse_args()
expected = json.loads((root / "tests/fixtures/rendering/local_shadow_transmission/expected.json").read_text())
width, height = expected["resolution"]
frames = {}
run_evidence = {}
for name in ("on", "off", "dark"):
    run = getattr(args, name).resolve()
    report_bytes = (run / "report.json").read_bytes()
    report = json.loads(report_bytes)
    if report.get("status") != "pass" or report.get("exit_code") != 0:
        raise AssertionError((name, "snapshot did not pass"))
    run_evidence[name] = {
        "path": str(run),
        "report_sha256": hashlib.sha256(report_bytes).hexdigest(),
        "status": report.get("status"),
        "exit_code": report.get("exit_code"),
        "provenance": report.get("provenance"),
    }
    item, path = capture_path(run, expected["channel"])
    if item["canonical_encoding"] != "RGBA16_FLOAT_LE":
        raise AssertionError((name, "unexpected HDR encoding"))
    if "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest() != item["data_sha256"]:
        raise AssertionError((name, "capture digest mismatch"))
    if (item["width"], item["height"]) != (width, height):
        raise AssertionError((name, item["width"], item["height"]))
    frames[name] = half_rgba(path, width, height)

camera = expected["camera_position"]
tangent = math.tan(math.radians(expected["camera_vertical_fov_degrees"]) / 2)
tolerance = expected["ratio_absolute_tolerance"]
results = []
for witness in expected["witnesses"]:
    x, y, z = witness["world_position"]
    view_depth = camera[2] - z
    center = (
        round((.5 + (x - camera[0]) / (2 * view_depth * tangent)) * width - .5),
        round((.5 - (y - camera[1]) / (2 * view_depth * tangent)) * height - .5),
    )
    samples = {
        name: mean_patch(frame, width, height, center,
                         expected["world_center_sample_radius_pixels"])
        for name, frame in frames.items()
    }
    direct = [value - dark for value, dark in zip(samples["off"], samples["dark"])]
    if min(direct) <= .02:
        raise AssertionError((witness["name"], "unlit or incorrect receiver sample", center, samples))
    ratio = [(value - dark) / reference
             for value, dark, reference in zip(samples["on"], samples["dark"], direct)]
    error = max(abs(value - reference)
                for value, reference in zip(ratio, witness["expected_rgb_ratio"]))
    results.append({"name": witness["name"], "pixel": center,
                    "ratio": ratio, "expected": witness["expected_rgb_ratio"],
                    "max_abs_error": error, "pass": error <= tolerance})

passed = all(result["pass"] for result in results)
print(json.dumps({"status": "pass" if passed else "fail",
                  "tolerance": tolerance, "runs": run_evidence,
                  "witnesses": results}, indent=2))
raise SystemExit(0 if passed else 1)
