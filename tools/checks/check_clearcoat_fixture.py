#!/usr/bin/env python3
"""Validate the clearcoat fixture's independent material witnesses.

Usage:
  ./build_release/tools/bakery/vkr_bakery cook tests/fixtures/rendering/clearcoat
  python3 tools/checks/check_clearcoat_fixture.py <harness-snapshot-directory>

The material source textures are intentionally tracked as PNGs. Run the
packing command after a fresh build and before taking the snapshot so the
scene's adjacent `.vkt` payloads exist.

The script intentionally compares spatially separated pixels from a fixed scene
instead of reproducing the GGX implementation. It detects a dropped coat row,
a swapped coat-normal map, an inactive roughness input, or missing glass
transport.
"""
from __future__ import annotations

import json
import math
import struct
import sys
from pathlib import Path


PANEL_CENTERS = {
    "off": (104, 110),
    "smooth_aligned": (205, 110),
    "smooth_opposed": (307, 110),
    "rough": (408, 110),
}
GLASS_CENTER = (256, 212)


def capture_path(run: Path, channel: str) -> tuple[dict, Path]:
    report = json.loads((run / "report.json").read_text())
    captures = {item["channel"]: item for item in report["captures"]}
    item = captures.get(channel)
    if item is None:
        raise AssertionError(f"fixture did not capture {channel}")
    path = run / item["data_path"]
    if not path.is_file():
        raise AssertionError(f"missing {channel} payload: {path}")
    return item, path


def half_rgba(path: Path, width: int, height: int) -> list[tuple[float, float, float, float]]:
    values = list(struct.iter_unpack("<4e", path.read_bytes()))
    if len(values) != width * height:
        raise AssertionError(f"unexpected RGBA16F count for {path}: {len(values)}")
    if not all(math.isfinite(value) for pixel in values for value in pixel):
        raise AssertionError(f"non-finite HDR value in {path}")
    return values


def mean_patch(values: list[tuple[float, float, float, float]], width: int,
               height: int, center: tuple[int, int], radius: int = 4) -> tuple[float, float, float]:
    x0, y0 = center
    samples = []
    for y in range(y0 - radius, y0 + radius + 1):
        for x in range(x0 - radius, x0 + radius + 1):
            if not (0 <= x < width and 0 <= y < height):
                raise AssertionError(f"fixture sample outside capture: {(x, y)}")
            samples.append(values[y * width + x])
    return tuple(sum(pixel[i] for pixel in samples) / len(samples) for i in range(3))


def rgb_distance(a: tuple[float, float, float], b: tuple[float, float, float]) -> float:
    return max(abs(x - y) for x, y in zip(a, b))


def oct_decode(raw: bytes, index: int) -> tuple[float, float, float]:
    x, y = struct.unpack_from("<hh", raw, index * 4)
    nx = max(-1.0, min(1.0, x / 32767.0))
    ny = max(-1.0, min(1.0, y / 32767.0))
    nz = 1.0 - abs(nx) - abs(ny)
    if nz < 0.0:
        old_x = nx
        nx = (1.0 - abs(ny)) * (1.0 if old_x >= 0.0 else -1.0)
        ny = (1.0 - abs(old_x)) * (1.0 if ny >= 0.0 else -1.0)
    length = math.sqrt(nx * nx + ny * ny + nz * nz)
    if not math.isfinite(length) or length <= 1e-6:
        raise AssertionError("invalid decoded base normal")
    return nx / length, ny / length, nz / length


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__.strip())
    run = Path(sys.argv[1])
    hdr_item, hdr_path = capture_path(run, "hdr_pre_transmission")
    width, height = hdr_item["width"], hdr_item["height"]
    if (width, height) != (512, 384):
        raise AssertionError(f"fixture requires 512x384, got {(width, height)}")
    hdr = half_rgba(hdr_path, width, height)

    panels = {name: mean_patch(hdr, width, height, center)
              for name, center in PANEL_CENTERS.items()}
    for name, value in panels.items():
        if max(value) <= 0.01:
            raise AssertionError(f"unlit or missing {name} panel: {value}")
    if rgb_distance(panels["off"], panels["smooth_aligned"]) <= 0.002:
        raise AssertionError("factor-zero and factor-one panels are indistinguishable")
    if rgb_distance(panels["smooth_aligned"], panels["smooth_opposed"]) <= 0.002:
        raise AssertionError("opposed coat normal did not change the coat lobe")
    if rgb_distance(panels["smooth_aligned"], panels["rough"]) <= 0.002:
        raise AssertionError("clearcoat roughness limits did not change the lobe")

    _, normal_path = capture_path(run, "gbuffer_normal")
    normals = normal_path.read_bytes()
    if len(normals) != width * height * 4:
        raise AssertionError("gbuffer_normal is not canonical RG16_SNORM")
    for name, (x, y) in PANEL_CENTERS.items():
        normal = oct_decode(normals, y * width + x)
        if normal[0] <= 0.30:
            raise AssertionError(f"base normal map was not applied to {name}: {normal}")

    post_item, post_path = capture_path(run, "hdr_post_transmission")
    if (post_item["width"], post_item["height"]) != (width, height):
        raise AssertionError("post-transmission extent differs from the opaque HDR capture")
    post = half_rgba(post_path, width, height)
    glass = mean_patch(post, width, height, GLASS_CENTER)
    if not (glass[1] > glass[0] * 1.10 and glass[1] > glass[2] * 1.10):
        raise AssertionError(f"coated glass did not transmit the green backdrop: {glass}")

    print("PASS clearcoat fixture")
    print("panel HDR means", panels)
    print("glass HDR mean", glass)


if __name__ == "__main__":
    main()
