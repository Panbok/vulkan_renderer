#!/usr/bin/env python3
"""Compare eight-frame Metal4FX replays of the 1280x720 Bistro bar capture.

The fixed bar polygon measures unexposed HDR variation. Lower variation alone
does not establish better imagery. --require-identical checks the independent
padding control over every output RGB pixel, not just the bar.
"""
import argparse
from array import array
import json
import math
from pathlib import Path
import struct
import sys

HALF = struct.unpack("<65536e", struct.pack("<65536H", *range(65536)))
POLYGON = [(0, 253), (1107, 249), (1080, 283), (0, 378)]


def inside(x, y):
    edges = zip(POLYGON, POLYGON[1:] + POLYGON[:1])
    signs = [(b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])
             for a, b in edges]
    return all(v >= 0 for v in signs) or all(v <= 0 for v in signs)


def read(run):
    report = json.loads((run / "report.json").read_text())
    assert report["mechanism"] == "MTL4FXTemporalScaler"
    assert (report["width"], report["height"]) == (1280, 720)
    assert report["encoding"] == "RGBA16_FLOAT_LE" and report["origin"] == "top_left"
    assert len(report["frames"]) == 8
    frames = []
    for frame in report["frames"]:
        values = array("H")
        values.frombytes((run / frame["file"]).read_bytes())
        if sys.byteorder != "little":
            values.byteswap()
        assert len(values) == 1280 * 720 * 4
        assert all(math.isfinite(HALF[v]) for v in values), "Nonfinite HDR"
        frames.append(values)
    return report, frames


def variation(frames):
    ranges, means = [], [0.0, 0.0, 0.0]
    for y in range(249, 378):
        for x in range(1107):
            if not inside(x + 0.5, y + 0.5):
                continue
            index = (y * 1280 + x) * 4
            channels = [[HALF[f[index + c]] for f in frames] for c in range(3)]
            ranges.append(max(max(ch) - min(ch) for ch in channels))
            for c, ch in enumerate(channels):
                means[c] += sum(ch) / len(ch)
    return {"bar_pixels": len(ranges),
            "mean_rgb": [v / len(ranges) for v in means],
            "max_rgb_range": max(ranges),
            "mean_max_rgb_range": sum(ranges) / len(ranges),
            "pixels_range_gt_0.001": sum(v > 0.001 for v in ranges)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--require-identical", action="store_true")
    args = parser.parse_args()
    before_report, before = read(args.before)
    after_report, after = read(args.after)
    for key in ("device", "active_width", "active_height", "warmup_frames"):
        assert before_report[key] == after_report[key], ("Replay differs", key)
    assert before_report["frames"] == after_report["frames"], "Phase order differs"
    count, maximum, total = 0, 0.0, 0.0
    for a, b in zip(before, after):
        for i in range(0, len(a), 4):
            delta = max(abs(HALF[a[i + c]] - HALF[b[i + c]]) for c in range(3))
            count += delta != 0
            maximum = max(maximum, delta)
            total += delta
    print(json.dumps({"before": variation(before), "after": variation(after),
                      "nonzero_rgb_pixels": count, "max_rgb_delta": maximum,
                      "mean_max_rgb_delta": total / (8 * 1280 * 720),
                      "finite_rgba_components_per_run": 8 * 1280 * 720 * 4},
                     indent=2), flush=True)
    if args.require_identical:
        assert count == 0, "Padding changed reconstructed RGB"


if __name__ == "__main__":
    main()
