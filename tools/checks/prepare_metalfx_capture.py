#!/usr/bin/env python3
"""Validate the retained raw snapshot and describe its ordered eight-phase replay."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct

p = argparse.ArgumentParser()
p.add_argument("report", type=Path)
p.add_argument("output", type=Path)
p.add_argument("--warmup", type=int, default=128)
p.add_argument("--fixed-exposure", type=float, default=1.0)
a = p.parse_args()
report = a.report.resolve()
d = json.loads(report.read_text())
assert d["exit_code"] == 0 and d["status"] == "pass", "Need a completed snapshot"
config = d["effective_config"]
assert config["upscaler"] == "metalfx_temporal" and config["taa_enabled"]
assert not config["dynamic_resolution"], "Replay requires a fixed source extent"
assert config["camera_script_version"] == 5, "Only fixed eight-phase MetalFX snapshots are supported"
assert math.isfinite(a.fixed_exposure) and 0 < a.fixed_exposure <= 65504
required = {
    "hdr_post_transmission": ("color", "RGBA16_FLOAT_LE", 8),
    "depth": ("depth", "R32_FLOAT_LE", 4),
    "motion_vectors": ("motion", "RG16_FLOAT_LE", 4),
}
groups = {}
for c in d["captures"]:
    if c["channel"] in required:
        group = groups.setdefault(c["checkpoint_frame"], {})
        assert c["channel"] not in group, "Duplicate checkpoint/channel"
        group[c["channel"]] = c
assert len(groups) == 8, "Expected exactly eight captured checkpoints"
assert a.warmup >= 128 and a.warmup <= 8192 and a.warmup % 8 == 0

def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]

def jitter(index, base):
    # Mirrors vkr_temporal_halton's float32 operations, including subtraction.
    index = index % 8 + 1
    fraction, result = 1.0, 0.0
    while index:
        fraction = f32(fraction / base)
        result = f32(result + f32(fraction * (index % base)))
        index //= base
    return f32(result - 0.5)

frames = []
extent = None
for checkpoint, channels in sorted(groups.items()):
    assert set(channels) == set(required), "Missing raw channel"
    indices = {c["source_frame_index"] for c in channels.values()}
    assert len(indices) == 1, "Channels have different producers"
    source_frame = indices.pop()
    frame = {"checkpoint": checkpoint, "source_frame_index": source_frame,
             "jitter": [jitter(source_frame, 2), jitter(source_frame, 3)],
             "input_sha256": {}}
    for channel, (key, encoding, bpp) in required.items():
        c = channels[channel]
        assert c["canonical_encoding"] == encoding and c["origin"] == "top_left"
        size = (c["width"], c["height"])
        if extent is None:
            extent = size
        assert size == extent, "Replay requires a fixed source extent"
        path = (report.parent / c["data_path"]).resolve()
        raw = path.read_bytes()
        assert len(raw) == size[0] * size[1] * bpp, "Canonical raw must be tightly packed"
        sha = "sha256:" + hashlib.sha256(raw).hexdigest()
        assert sha == c["data_sha256"], "Capture digest mismatch"
        frame[key] = str(path)
        frame["input_sha256"][key] = sha
    frames.append(frame)
phases = [f["source_frame_index"] % 8 for f in frames]
assert all(phases[i] == (phases[0] + i) % 8 for i in range(8)), "Nonconsecutive raster phases"
out = {"width": extent[0], "height": extent[1],
       "output_width": d["effective_config"]["resolution"][0],
       "output_height": d["effective_config"]["resolution"][1],
       "warmup_frames": a.warmup, "fixed_exposure": a.fixed_exposure,
       "source_report": str(report),
       "source_report_sha256": hashlib.sha256(report.read_bytes()).hexdigest(),
       "frames": frames,
       "limitation": "Ordered raster phases from separate captures, not consecutive scene submissions; replay repeats frozen source radiance."}
a.output.parent.mkdir(parents=True, exist_ok=True)
with a.output.open("x") as f:
    json.dump(out, f, indent=2)
    f.write("\n")
print(f"{a.output}: active{extent}, phases{phases}, {a.warmup}+8 frames; all input sizes and digests checked")
