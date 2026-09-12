#!/usr/bin/env python3
"""Compare the five froxel_bistro_regression snapshots in raw scene-linear HDR.

Usage: python3 tools/checks/check_froxel_bistro_regression.py OFF EMPTY THIN FAR50 FAR500
No exposure/tonemapped values enter these independent identity/range comparisons.
"""
import json
import math
from pathlib import Path
import struct
import sys


def capture(run, channel):
    run = Path(run)
    for report_path in [run / "report.json", *sorted(run.glob("captures/*/report.json"))]:
        if not report_path.exists():
            continue
        report = json.loads(report_path.read_text())
        for item in report.get("captures", []):
            if item["channel"] == channel:
                assert report.get("status") == "pass", report_path
                assert (item["width"], item["height"]) == (640, 360), item
                data = (report_path.parent / item["data_path"]).read_bytes()
                layout = {"depth": "<f", "visibility_ids": "<I",
                          "visibility_primitives": "<I", "gbuffer_normal": "<2h"}.get(channel, "<4e")
                pixels = list(struct.iter_unpack(layout, data))
                assert len(pixels) == 640 * 360, (channel, len(pixels))
                return pixels
    raise AssertionError((run, channel, "missing successful capture"))


def main():
    assert len(sys.argv) == 6, __doc__
    off, empty, thin, far50, far500 = [capture(run, "hdr_pre_bloom") for run in sys.argv[1:]]
    assert all(math.isfinite(c) for frame in [off, empty, thin, far50, far500]
               for pixel in frame for c in pixel), "nonfinite HDR"
    # Distinct scene instances can rasterize different coplanar Bistro
    # primitives at an exactly equal depth. Identify that changed input from
    # primitive/normal/depth captures before applying the unchanged RGB oracle.
    # Visible-row indices are transient and may globally permute between runs.
    off_primitives = capture(sys.argv[1], "visibility_primitives")
    empty_primitives = capture(sys.argv[2], "visibility_primitives")
    off_normals = capture(sys.argv[1], "gbuffer_normal")
    empty_normals = capture(sys.argv[2], "gbuffer_normal")
    off_depth = capture(sys.argv[1], "depth")
    empty_depth = capture(sys.argv[2], "depth")
    assert off_depth == empty_depth, "camera/coverage depth differs; identity inputs do not match"
    changed_primitives = {i for i, (a, b) in enumerate(zip(off_primitives, empty_primitives)) if a != b}
    changed_normals = {i for i, (a, b) in enumerate(zip(off_normals, empty_normals)) if a != b}
    assert changed_primitives == changed_normals, "unclassified geometry/normal mismatch; inspect inputs"
    geometry_mismatches = [{"pixel": [i % 640, i // 640],
                            "off_primitive": off_primitives[i][0],
                            "empty_primitive": empty_primitives[i][0],
                            "off_normal": off_normals[i], "empty_normal": empty_normals[i],
                            "equal_device_depth": off_depth[i][0]}
                           for i in sorted(changed_primitives)]
    # The same RGB tolerance applies to every pixel with matching geometry.
    # Report changed inputs separately; never claim whole-frame identity for
    # frames that supplied different surfaces to the lighting/fog stages.
    max_relative = 0
    for index, (before, after) in enumerate(zip(off, empty)):
        if index in changed_primitives:
            continue
        for source, result in zip(before[:3], after[:3]):
            error = abs(source - result)
            assert error <= max(.01, abs(source) * .002), (index % 640, index // 640, source, result)
            max_relative = max(max_relative, error / max(abs(source), 1))
    # Project the authored 30,000 cards independently from the camera basis.
    # Fixed screen coordinates would hide a fixture/camera placement regression.
    root = Path(__file__).resolve().parents[2]
    case = json.loads((root / "tools/cases/local/froxel_bistro_regression_off.case.json").read_text())
    scene = json.loads((root / case["scene"]).read_text())
    camera = case["camera"]
    yaw, pitch = map(math.radians, [camera["yaw"], camera["pitch"]])
    forward = [math.cos(yaw) * math.cos(pitch), math.sin(pitch), math.sin(yaw) * math.cos(pitch)]
    right = [-math.sin(yaw), 0, math.cos(yaw)]
    up = [-math.cos(yaw) * math.sin(pitch), math.cos(pitch), -math.sin(yaw) * math.sin(pitch)]
    focal = 180 / math.tan(math.radians(camera["vertical_fov_degrees"]) / 2)
    witnesses = []
    for row in ["opaque", "blend", "transmission"]:
        entity = next(e for e in scene["entities"] if e["name"] == f"froxel_hdr_{row}_30000")
        delta = [point - eye for point, eye in zip(entity["transform"]["pos"], camera["position"])]
        depth = sum(a * b for a, b in zip(delta, forward))
        x = round(320 + focal * sum(a * b for a, b in zip(delta, right)) / depth)
        y = round(180 - focal * sum(a * b for a, b in zip(delta, up)) / depth)
        assert 2 <= x < 638 and 2 <= y < 358, (row, x, y, "witness outside frame")
        indices = [(y + dy) * 640 + x + dx for dy in range(-2, 3) for dx in range(-2, 3)]
        assert not any(i in changed_primitives for i in indices), (row, "bright witness geometry changed")
        before = sum(off[i][0] for i in indices) / len(indices)
        clear = sum(empty[i][0] for i in indices) / len(indices)
        sparse = sum(thin[i][0] for i in indices) / len(indices)
        assert before > 5000, (row, before, "non-degenerate bright witness required")
        assert abs(clear / before - 1) < .002, (row, before, clear)
        assert .99 <= sparse / before <= 1.002, (row, before, sparse)
        witnesses.append({"row": row, "pixel": [x, y], "off": before, "empty": clear, "thin": sparse})
    depth50 = capture(sys.argv[4], "depth")
    depth500 = capture(sys.argv[5], "depth")
    sky_count = 0
    max_sky_error = 0
    for index, (d50, d500) in enumerate(zip(depth50, depth500)):
        if d50[0] < 1 or d500[0] < 1:
            continue
        sky_count += 1
        for a, b in zip(far50[index][:3], far500[index][:3]):
            error = abs(a - b)
            assert error <= max(.002, abs(b) * .004), (index, a, b)
            max_sky_error = max(max_sky_error, error)
    assert sky_count >= 100, (sky_count, "common sky coverage required")
    print(json.dumps({"status": "pass", "witnesses": witnesses,
                      "empty_max_relative": max_relative,
                      "identity_pixels_tested": len(off) - len(changed_primitives),
                      "full_frame_identity": not geometry_mismatches,
                      "changed_coplanar_geometry_inputs": geometry_mismatches,
                      "sky_pixels": sky_count,
                      "far_plane_max_sky_absolute": max_sky_error}, indent=2))


if __name__ == "__main__":
    main()
