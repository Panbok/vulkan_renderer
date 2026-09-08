#!/usr/bin/env python3
"""Validate synthetic depth-of-field snapshots.

Argument: a JSON mapping `old_off`, `off`, `focus`, `resize`, `odd`, `taa`,
`spatial`, and `metalfx` to snapshot run directories. `old_off` is the retained pre-DoF capture of
local.dof.off. The checker derives card interiors from its HDR colors rather
than from fixed screen rectangles.
"""
from __future__ import annotations

import json
import math
import struct
import sys
from pathlib import Path


FOCUS_EXTENT = (512, 384)
EXPECTED_DEPTHS = {"near": 0.24, "focus": 0.49, "far": 1.59}


def report(run: Path) -> dict:
    return json.loads((run / "report.json").read_text())


def capture(run: Path, channel: str) -> tuple[dict, Path]:
    matches = [item for item in report(run)["captures"] if item["channel"] == channel]
    if len(matches) != 1:
        raise AssertionError(f"expected one {channel} capture in {run}, got {len(matches)}")
    path = run / matches[0]["data_path"]
    if not path.is_file():
        raise AssertionError(f"missing {channel} payload: {path}")
    return matches[0], path


def captures(run: Path, channel: str) -> list[tuple[dict, Path]]:
    matches = [(item, run / item["data_path"])
               for item in report(run)["captures"] if item["channel"] == channel]
    if not matches or not all(path.is_file() for _, path in matches):
        raise AssertionError(f"missing {channel} capture in {run}")
    return matches


def read_rgba16(item: dict, path: Path) -> list[tuple[float, float, float, float]]:
    if item["canonical_encoding"] != "RGBA16_FLOAT_LE":
        raise AssertionError(f"expected RGBA16_FLOAT_LE, got {item['canonical_encoding']}")
    values = list(struct.iter_unpack("<4e", path.read_bytes()))
    if len(values) != item["width"] * item["height"]:
        raise AssertionError("unexpected RGBA16F payload size")
    if not all(math.isfinite(value) for pixel in values for value in pixel):
        raise AssertionError("non-finite HDR value")
    return values


def read_rg16(item: dict, path: Path) -> list[tuple[float, float]]:
    if item["canonical_encoding"] != "RG16_FLOAT_LE":
        raise AssertionError(f"expected RG16_FLOAT_LE, got {item['canonical_encoding']}")
    values = list(struct.iter_unpack("<2e", path.read_bytes()))
    if len(values) != item["width"] * item["height"]:
        raise AssertionError("unexpected RG16F payload size")
    if not all(math.isfinite(value) for pixel in values for value in pixel):
        raise AssertionError("non-finite CoC payload")
    return values


def color_mask(pixels: list[tuple[float, float, float, float]], primary: int) -> list[bool]:
    return [pixel[primary] > 0.05 and pixel[primary] > 2.0 * max(
        pixel[(primary + 1) % 3], pixel[(primary + 2) % 3], 1e-4)
        for pixel in pixels]


def erode(mask: list[bool], width: int, height: int, radius: int = 2) -> list[bool]:
    output = [False] * len(mask)
    for y in range(radius, height - radius):
        for x in range(radius, width - radius):
            output[y * width + x] = all(
                mask[(y + dy) * width + x + dx]
                for dy in range(-radius, radius + 1)
                for dx in range(-radius, radius + 1))
    return output


def bounds(mask: list[bool], width: int, height: int) -> tuple[int, int, int, int]:
    points = [(index % width, index // width) for index, covered in enumerate(mask) if covered]
    if len(points) < 16:
        raise AssertionError("card mask has too little interior coverage")
    xs, ys = zip(*points)
    return min(xs), max(xs) + 1, min(ys), max(ys) + 1


def mean_component(values: list[tuple[float, ...]], mask: list[bool], component: int) -> float:
    samples = [pixel[component] for pixel, covered in zip(values, mask) if covered]
    if not samples:
        raise AssertionError("empty card interior")
    return sum(samples) / len(samples)


def edge_energy(pixels: list[tuple[float, float, float, float]], width: int,
                height: int, component: int, box: tuple[int, int, int, int]) -> float:
    x0, x1, y0, y1 = box
    x0, x1 = max(1, x0 - 16), min(width - 1, x1 + 16)
    y0, y1 = max(1, y0 - 16), min(height - 1, y1 + 16)
    values = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            center = pixels[y * width + x][component]
            values.append(abs(center - pixels[y * width + x + 1][component]))
            values.append(abs(center - pixels[(y + 1) * width + x][component]))
    return sum(values) / len(values)


def edge_peak(pixels: list[tuple[float, float, float, float]], width: int,
              height: int, component: int, box: tuple[int, int, int, int]) -> float:
    x0, x1, y0, y1 = box
    x0, x1 = max(1, x0 - 16), min(width - 1, x1 + 16)
    y0, y1 = max(1, y0 - 16), min(height - 1, y1 + 16)
    return max(
        abs(pixels[y * width + x][component] - pixels[y * width + x + 1][component])
        for y in range(y0, y1) for x in range(x0, x1))


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__.strip())
    runs = {name: Path(path) for name, path in json.loads(Path(sys.argv[1]).read_text()).items()}
    if set(runs) != {"old_off", "off", "focus", "resize", "odd", "taa", "spatial", "metalfx"}:
        raise AssertionError("mapping must contain old_off, off, focus, resize, odd, taa, spatial, and metalfx")

    for channel in ("final_color", "hdr_pre_bloom"):
        before_item, before_path = capture(runs["old_off"], channel)
        off_item, off_path = capture(runs["off"], channel)
        if (before_item["width"], before_item["height"]) != (
                off_item["width"], off_item["height"]):
            raise AssertionError(f"disabled DoF changed {channel} extent")
        if before_path.read_bytes() != off_path.read_bytes():
            raise AssertionError(f"disabled DoF changed {channel} bytes")

    before_item, before_path = capture(runs["old_off"], "hdr_pre_bloom")
    if (before_item["width"], before_item["height"]) != FOCUS_EXTENT:
        raise AssertionError("baseline extent changed")
    baseline = read_rgba16(before_item, before_path)
    focus_item, focus_path = capture(runs["focus"], "dof_color")
    focus_pixels = read_rgba16(focus_item, focus_path)
    if (focus_item["width"], focus_item["height"]) != FOCUS_EXTENT:
        raise AssertionError("DoF color extent changed")
    coc_item, coc_path = capture(runs["focus"], "dof_coc")
    coc = read_rg16(coc_item, coc_path)
    if (coc_item["width"], coc_item["height"]) != FOCUS_EXTENT:
        raise AssertionError("DoF CoC extent changed")

    masks = {name: erode(color_mask(baseline, primary), *FOCUS_EXTENT)
             for name, primary in (("near", 0), ("focus", 1), ("far", 2))}
    boxes = {name: bounds(mask, *FOCUS_EXTENT) for name, mask in masks.items()}
    coc_values = {name: mean_component(coc, mask, 0) for name, mask in masks.items()}
    depths = {name: mean_component(coc, mask, 1) for name, mask in masks.items()}
    for name, expected in EXPECTED_DEPTHS.items():
        if abs(depths[name] - expected) > 0.08:
            raise AssertionError(f"{name} card depth {depths[name]:.4f} differs from {expected:.2f} m")
    if coc_values["near"] * coc_values["far"] >= 0.0:
        raise AssertionError(f"near and far CoC must have opposite signs: {coc_values}")
    if abs(coc_values["focus"]) >= 0.25 * min(abs(coc_values["near"]), abs(coc_values["far"])):
        raise AssertionError(f"focus-plane CoC is not near zero: {coc_values}")

    focus_baseline = mean_component(baseline, masks["focus"], 1)
    focus_output = mean_component(focus_pixels, masks["focus"], 1)
    if focus_output < 0.85 * focus_baseline:
        raise AssertionError("focus-plane interior was not retained")
    edge = {name: {
        "before": edge_energy(baseline, *FOCUS_EXTENT, primary, boxes[name]),
        "dof": edge_energy(focus_pixels, *FOCUS_EXTENT, primary, boxes[name]),
        "peak_before": edge_peak(baseline, *FOCUS_EXTENT, primary, boxes[name]),
        "peak_dof": edge_peak(focus_pixels, *FOCUS_EXTENT, primary, boxes[name]),
    } for name, primary in (("near", 0), ("focus", 1), ("far", 2))}
    for name in ("near", "far"):
        if edge[name]["peak_dof"] >= 0.8 * edge[name]["peak_before"]:
            raise AssertionError(f"{name} retained a one-pixel hard edge: {edge[name]}")
    if edge["focus"]["dof"] < 0.6 * edge["focus"]["before"]:
        raise AssertionError(f"focus edge was blurred too much: {edge['focus']}")

    resize_color, resize_color_path = capture(runs["resize"], "dof_color")
    resize_coc, resize_coc_path = capture(runs["resize"], "dof_coc")
    resize_report = report(runs["resize"])
    resize_config = resize_report["effective_config"]
    resize_scale = resize_config["content_scale"]
    resize_authored = resize_config["resize_round_trip"]
    resize_extent = tuple(round(value * resize_scale) for value in resize_authored)
    if (resize_color["width"], resize_color["height"]) != resize_extent or (
            resize_coc["width"], resize_coc["height"]) != resize_extent:
        raise AssertionError("DoF output does not match outbound drawable extent")
    read_rgba16(resize_color, resize_color_path)
    read_rg16(resize_coc, resize_coc_path)
    restored_extent = tuple(resize_config["resolution"])
    resize_log = runs["resize"] / "captures/0/stdout.log"
    resize_pass = (f"VKR_HARNESS_RESIZE_ROUND_TRIP_PASS outbound={resize_extent[0]}x"
                   f"{resize_extent[1]} restored={restored_extent[0]}x{restored_extent[1]}")
    if not resize_log.is_file() or resize_pass not in resize_log.read_text():
        raise AssertionError("resize round trip did not reach the reported drawable extent")
    odd_color, odd_color_path = capture(runs["odd"], "dof_color")
    odd_coc, odd_coc_path = capture(runs["odd"], "dof_coc")
    odd_extent = (odd_color["width"], odd_color["height"])
    if odd_extent != (257, 193) or odd_extent != (odd_coc["width"], odd_coc["height"]):
        raise AssertionError("offscreen odd extent did not survive DoF")
    read_rgba16(odd_color, odd_color_path)
    read_rg16(odd_coc, odd_coc_path)
    if report(runs["taa"])["effective_config"]["taa_enabled"] is not True:
        raise AssertionError("temporal DoF case did not enable TAA")
    if len(captures(runs["taa"], "dof_color")) != 2 or len(captures(runs["taa"], "dof_coc")) != 2:
        raise AssertionError("temporal DoF case requires two color and CoC frames")
    for name, upscaler, scale in (("spatial", "spatial", 0.666666667),
                                  ("metalfx", "metalfx_temporal", 0.8)):
        config = report(runs[name])["effective_config"]
        if config["upscaler"] != upscaler or abs(config["render_scale"] - scale) > 1e-5:
            raise AssertionError(f"{name} DoF reconstruction mode was not realized")
        color, color_path = capture(runs[name], "dof_color")
        coc_item, coc_path = capture(runs[name], "dof_coc")
        if (color["width"], color["height"]) != (coc_item["width"], coc_item["height"]):
            raise AssertionError(f"{name} DoF capture extents differ")
        read_rgba16(color, color_path)
        read_rg16(coc_item, coc_path)

    print(json.dumps({"status": "pass", "coc": coc_values, "depth_m": depths,
                      "edge_energy": edge}, indent=2))


if __name__ == "__main__":
    main()
