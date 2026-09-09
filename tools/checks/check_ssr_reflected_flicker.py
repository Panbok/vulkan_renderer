#!/usr/bin/env python3
"""Measure the reported Bistro bar view before stationary accumulation.

Usage: python3 tools/checks/check_ssr_reflected_flicker.py SNAPSHOT_RUN [...]

Use ssr_reflected_hit_flicker.case.json. Checkpoint replays sample raster-jitter
phases, not a continuous clip. Report regional RGB/coverage ranges and retained
light so darkening cannot masquerade as stability. These are measurements, not
a flicker-free verdict or an approved cross-backend tolerance.
"""
import array
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


HALF = [struct.unpack("<e", struct.pack("<H", value))[0] for value in range(65536)]
REGIONS = {
    "bar_lip": (680, 746, 1280, 835),
    "bar_panel": (780, 845, 1210, 960),
    "counter": (680, 665, 1370, 736),
    "upper_gloss": (1230, 30, 1720, 260),
}
CHANNELS = ("ssr_raw", "ssr_reflection", "hdr_pre_transmission",
            "hdr_post_transmission", "hdr_pre_bloom")


def measure(run):
    report_bytes = (run / "report.json").read_bytes()
    report = json.loads(report_bytes)
    assert report["status"] == "pass", run
    assert report["effective_config"]["resolution"] == [1784, 1093], run
    result = {"run": str(run), "report_sha256": hashlib.sha256(report_bytes).hexdigest(),
              "channels": {}}
    for channel in CHANNELS:
        rows = sorted((r for r in report["captures"] if r["channel"] == channel),
                      key=lambda r: r["checkpoint_frame"])
        assert [r["checkpoint_frame"] for r in rows] == [0, 1, 4, 8], channel
        frames = []
        width, height = rows[0]["width"], rows[0]["height"]
        for row in rows:
            assert row["canonical_encoding"] == "RGBA16_FLOAT_LE", row
            assert [row["width"], row["height"]] == [width, height], row
            if channel == "ssr_reflection":
                assert row["capture_version"] == 5, row
            data = (run / row["data_path"]).read_bytes()
            assert "sha256:" + hashlib.sha256(data).hexdigest() == row["data_sha256"], row
            frame = array.array("H")
            frame.frombytes(data)
            if sys.byteorder != "little":
                frame.byteswap()
            assert len(frame) == width * height * 4, row
            frames.append(frame)
        statistics = {}
        for region, box in REGIONS.items():
            scale = (width / 1784, height / 1093, width / 1784, height / 1093)
            x0, y0, x1, y1 = [round(v * s) for v, s in zip(box, scale)]
            count = (x1 - x0) * (y1 - y0)
            rgb_sum = coverage_sum = covered_sum = energy = 0.0
            above = 0
            worst = []
            for y in range(y0, y1):
                for x in range(x0, x1):
                    index = 4 * (y * width + x)
                    values = [[HALF[frame[index + k]] for k in range(4)] for frame in frames]
                    assert all(math.isfinite(v) for value in values for v in value), (channel, x, y)
                    channels = list(zip(*values))
                    assert min(channels[3]) >= 0 and max(channels[3]) <= 1, (channel, x, y)
                    delta = max(max(c) - min(c) for c in channels[:3])
                    coverage = max(channels[3]) - min(channels[3])
                    signal = [max(c[:3]) * c[3] for c in values]
                    covered = max(signal) - min(signal)
                    rgb_sum += delta
                    coverage_sum += coverage
                    covered_sum += covered
                    energy += sum(signal) / len(signal)
                    above += covered > .001
                    worst.append((covered, x, y, values))
            worst.sort(reverse=True, key=lambda row: row[0])
            statistics[region] = {
                "box": [x0, y0, x1, y1], "pixels": count,
                "mean_rgb_range": rgb_sum / count,
                "mean_coverage_range": coverage_sum / count,
                "mean_covered_range": covered_sum / count,
                "mean_covered_energy": energy / count,
                "range_gt_0.001": above, "worst": worst[:3],
            }
        result["channels"][channel] = {"extent": [width, height], "regions": statistics}
    return result


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    print(json.dumps([measure(Path(name)) for name in sys.argv[1:]], indent=2))
