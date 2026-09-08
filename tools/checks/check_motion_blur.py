#!/usr/bin/env python3
"""Validate local motion-blur snapshot directories.

Argument: a JSON mapping `disabled`, `zero`, `static`, `mb180`, `mb360`, and
`history` to snapshot run directories.
"""
from __future__ import annotations

import json
import math
import struct
import sys
from pathlib import Path


EXTENT = (512, 384)
TILE_EXTENT = (32, 24)
MAX_RADIUS_PIXELS = 16.0


def report(run: Path) -> dict:
    return json.loads((run / "report.json").read_text())


def captures(run: Path, channel: str) -> list[tuple[dict, Path]]:
    found = [(item, run / item["data_path"])
             for item in report(run)["captures"] if item["channel"] == channel]
    if not found or not all(path.is_file() for _, path in found):
        raise AssertionError(f"missing {channel} capture in {run}")
    return found


def capture(run: Path, channel: str) -> tuple[dict, Path]:
    found = captures(run, channel)
    if len(found) != 1:
        raise AssertionError(f"expected one {channel} capture in {run}, got {len(found)}")
    return found[0]


def read_rgba16(item: dict, path: Path) -> list[tuple[float, float, float, float]]:
    if item["canonical_encoding"] != "RGBA16_FLOAT_LE":
        raise AssertionError(f"expected RGBA16_FLOAT_LE, got {item['canonical_encoding']}")
    values = list(struct.iter_unpack("<4e", path.read_bytes()))
    if len(values) != item["width"] * item["height"]:
        raise AssertionError("unexpected RGBA16F payload size")
    if not all(math.isfinite(value) for pixel in values for value in pixel):
        raise AssertionError("non-finite color value")
    return values


def read_rg16(item: dict, path: Path) -> list[tuple[float, float]]:
    if item["canonical_encoding"] != "RG16_FLOAT_LE":
        raise AssertionError(f"expected RG16_FLOAT_LE, got {item['canonical_encoding']}")
    values = list(struct.iter_unpack("<2e", path.read_bytes()))
    if len(values) != item["width"] * item["height"]:
        raise AssertionError("unexpected RG16F payload size")
    if not all(math.isfinite(value) for pixel in values for value in pixel):
        raise AssertionError("non-finite motion value")
    return values


def extent(item: dict) -> tuple[int, int]:
    return item["width"], item["height"]


def primary_mask(pixels: list[tuple[float, float, float, float]], primary: int) -> list[bool]:
    return [pixel[primary] > 0.05 and pixel[primary] > 2.0 * max(
        pixel[(primary + 1) % 3], pixel[(primary + 2) % 3], 1e-4)
        for pixel in pixels]


def bounds(mask: list[bool], width: int, height: int) -> tuple[int, int, int, int]:
    points = [(index % width, index // width) for index, hit in enumerate(mask) if hit]
    if len(points) < 16:
        raise AssertionError("fixture color mask has too little coverage")
    xs, ys = zip(*points)
    return min(xs), max(xs) + 1, min(ys), max(ys) + 1


def edge_peak(pixels: list[tuple[float, float, float, float]], width: int,
              height: int, component: int, box: tuple[int, int, int, int]) -> float:
    x0, x1, y0, y1 = box
    x0, x1 = max(1, x0 - 16), min(width - 1, x1 + 16)
    y0, y1 = max(1, y0 - 16), min(height - 1, y1 + 16)
    return max(abs(pixels[y * width + x][component] -
                   pixels[y * width + x + 1][component])
               for y in range(y0, y1) for x in range(x0, x1))


def max_radius(values: list[tuple[float, float]]) -> float:
    return max(math.hypot(value[0], value[1]) for value in values)


def max_vector_pixels(values: list[tuple[float, float]], width: int,
                      height: int) -> float:
    return max(math.hypot(value[0] * width, value[1] * height)
               for value in values)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__.strip())
    runs = {name: Path(path) for name, path in json.loads(Path(sys.argv[1]).read_text()).items()}
    expected = {"disabled", "zero", "static", "mb180", "mb360", "history"}
    if set(runs) != expected:
        raise AssertionError("mapping must contain disabled, zero, static, mb180, mb360, and history")

    for name in ("disabled", "zero"):
        config = report(runs[name])["effective_config"]
        if config["taa_enabled"] or config["ssr_enabled"] or config["ssgi_enabled"]:
            raise AssertionError(f"{name} enabled a temporal/reflection feature")
    for channel in ("final_color", "hdr_pre_bloom"):
        disabled_item, disabled_path = capture(runs["disabled"], channel)
        zero_item, zero_path = capture(runs["zero"], channel)
        if extent(disabled_item) != EXTENT or extent(zero_item) != EXTENT:
            raise AssertionError(f"{channel} extent changed for a disabled shutter")
        if disabled_path.read_bytes() != zero_path.read_bytes():
            raise AssertionError(f"disabled and zero-shutter {channel} bytes differ")

    static_hdr_item, static_hdr_path = capture(runs["static"], "hdr_pre_bloom")
    static_color_item, static_color_path = capture(runs["static"], "motion_blur_color")
    if extent(static_hdr_item) != EXTENT or extent(static_color_item) != EXTENT:
        raise AssertionError("static motion-blur extent changed")
    if static_hdr_path.read_bytes() != static_color_path.read_bytes():
        raise AssertionError("static motion blur changed HDR bytes")
    static_tiles_item, static_tiles_path = capture(runs["static"], "motion_blur_tiles")
    static_vectors_item, static_vectors_path = capture(runs["static"], "motion_vectors")
    static_tiles = read_rg16(static_tiles_item, static_tiles_path)
    static_vectors = read_rg16(static_vectors_item, static_vectors_path)
    if extent(static_tiles_item) != TILE_EXTENT or extent(static_vectors_item) != EXTENT:
        raise AssertionError("static motion metadata extent is invalid")
    if max_radius(static_tiles) > 0.01 or max_radius(static_vectors) > 0.01:
        raise AssertionError("static workload has nonzero motion metadata")

    disabled_hdr_item, disabled_hdr_path = capture(runs["disabled"], "hdr_pre_bloom")
    disabled_hdr = read_rgba16(disabled_hdr_item, disabled_hdr_path)
    red_box = bounds(primary_mask(disabled_hdr, 0), *EXTENT)
    green_box = bounds(primary_mask(disabled_hdr, 1), *EXTENT)
    active = {}
    for name, shutter in (("mb180", 180.0), ("mb360", 360.0)):
        config = report(runs[name])["effective_config"]
        if (not config["motion_blur_enabled"] or
                abs(config["motion_blur_shutter_angle"] - shutter) > 1e-5 or
                config["taa_enabled"] or config["ssr_enabled"] or config["ssgi_enabled"]):
            raise AssertionError(f"{name} did not realize its authored motion-blur workload")
        hdr_item, hdr_path = capture(runs[name], "hdr_pre_bloom")
        color_item, color_path = capture(runs[name], "motion_blur_color")
        tiles_item, tiles_path = capture(runs[name], "motion_blur_tiles")
        vectors_item, vectors_path = capture(runs[name], "motion_vectors")
        if extent(hdr_item) != EXTENT or extent(color_item) != EXTENT or \
                extent(vectors_item) != EXTENT or extent(tiles_item) != TILE_EXTENT:
            raise AssertionError(f"{name} motion capture extent is invalid")
        if hdr_path.read_bytes() != disabled_hdr_path.read_bytes():
            raise AssertionError(f"{name} changed the raw hdr_pre_bloom source")
        hdr = read_rgba16(hdr_item, hdr_path)
        color = read_rgba16(color_item, color_path)
        tiles = read_rg16(tiles_item, tiles_path)
        vectors = read_rg16(vectors_item, vectors_path)
        tile_radius = max_radius(tiles)
        vector_pixels = max_vector_pixels(vectors, *EXTENT)
        if tile_radius <= 0.5 or vector_pixels <= 0.5:
            raise AssertionError(f"{name} did not preserve motion history")
        if tile_radius > MAX_RADIUS_PIXELS + 0.05:
            raise AssertionError(f"{name} exceeded the 16 pixel half-extent")
        active[name] = {"hdr": hdr, "color": color, "tile_radius": tile_radius,
                        "vector_pixels": vector_pixels}

    red_before = edge_peak(disabled_hdr, *EXTENT, 0, red_box)
    red_180 = edge_peak(active["mb180"]["color"], *EXTENT, 0, red_box)
    red_360 = edge_peak(active["mb360"]["color"], *EXTENT, 0, red_box)
    if red_180 >= 0.9 * red_before or red_360 >= 0.9 * red_180:
        raise AssertionError("shutter angle did not broaden the moving opaque edge")
    green_before = edge_peak(disabled_hdr, *EXTENT, 1, green_box)
    green_360 = edge_peak(active["mb360"]["color"], *EXTENT, 1, green_box)
    if green_360 < 0.9 * green_before:
        raise AssertionError("transparent coverage was motion blurred")
    if active["mb360"]["tile_radius"] + 0.05 < active["mb180"]["tile_radius"]:
        raise AssertionError("360 degree shutter reduced tile velocity")

    history_color = captures(runs["history"], "motion_blur_color")
    history_tiles = captures(runs["history"], "motion_blur_tiles")
    history_vectors = captures(runs["history"], "motion_vectors")
    if not (len(history_color) == len(history_tiles) == len(history_vectors) == 2):
        raise AssertionError("history case requires early and late motion captures")
    frames = [item["source_frame_index"] for item, _ in history_color]
    if frames[1] <= frames[0]:
        raise AssertionError("later motion-history capture did not advance source frames")
    history_tile_radius = [max_radius(read_rg16(item, path))
                           for item, path in history_tiles]
    history_vector_pixels = [max_vector_pixels(read_rg16(item, path), *EXTENT)
                             for item, path in history_vectors]
    if history_tile_radius[0] > 0.01 or history_vector_pixels[0] > 0.01:
        raise AssertionError("first motion frame unexpectedly used old history")
    # Camera translation is 0.03 m/frame. At the fixture backdrop depth,
    # 463.529 px focal length and a 180 degree shutter predict 1.849 px.
    if abs(history_tile_radius[1] - 1.849) > 0.1 or \
            abs(history_vector_pixels[1] - 22.19) > 0.4:
        raise AssertionError("actual predecessor interval produced an unexpected velocity")
    for item, path in history_tiles + history_vectors:
        read_rg16(item, path)

    print(json.dumps({"status": "pass", "red_peak": {"before": red_before,
                     "180": red_180, "360": red_360},
                     "max_radius": {name: {"tiles": values["tile_radius"],
                                     "vector_pixels": values["vector_pixels"]}
                                    for name, values in active.items()},
                     "history": {"tiles": history_tile_radius,
                                 "vector_pixels": history_vector_pixels}}, indent=2))


if __name__ == "__main__":
    main()
