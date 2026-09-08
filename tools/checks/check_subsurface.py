#!/usr/bin/env python3
"""Check local subsurface witness captures.

Usage: python3 tools/checks/check_subsurface.py <runs.json>

`runs.json` maps `enabled`, `off`, `zero`, `furnace`, and `mixed` to harness
snapshot directories. Capture the matching `subsurface_*_local` cases first.
The checks use source `subsurface_diffuse`, composed `subsurface_color`, final
HDR, and opaque visibility IDs. They do not reproduce the renderer's profile
kernel. The shadow witness is the right-hand shadow on the continuous surface;
the bounds below exclude the occluder at x=220 in this fixed 513x321 fixture.
"""

from __future__ import annotations

import json
import math
import struct
import sys
from pathlib import Path


def capture(run: Path, channel: str) -> tuple[dict, Path]:
    report = json.loads((run / "report.json").read_text())
    items = {item["channel"]: item for item in report["captures"]}
    item = items.get(channel)
    if item is None:
        raise AssertionError(f"{run}: missing capture {channel}")
    path = run / item["data_path"]
    if not path.is_file():
        raise AssertionError(f"{run}: missing payload {path}")
    return item, path


def encoded_bytes(run: Path, channel: str, encoding: str) -> tuple[int, int, bytes]:
    item, path = capture(run, channel)
    if item["canonical_encoding"] != encoding:
        raise AssertionError(
            f"{channel}: expected {encoding}, got {item['canonical_encoding']}")
    width, height = item["width"], item["height"]
    return width, height, path.read_bytes()


def rgba16(run: Path, channel: str) -> tuple[int, int, list[tuple[float, ...]]]:
    width, height, raw = encoded_bytes(run, channel, "RGBA16_FLOAT_LE")
    values = list(struct.iter_unpack("<4e", raw))
    if len(values) != width * height:
        raise AssertionError(f"{channel}: expected {width * height} RGBA16F pixels")
    if not all(math.isfinite(value) for pixel in values for value in pixel):
        raise AssertionError(f"{channel}: non-finite HDR payload")
    return width, height, values


def ids32(run: Path) -> tuple[int, int, list[int]]:
    width, height, raw = encoded_bytes(run, "visibility_ids", "R32_UINT_LE")
    if len(raw) != width * height * 4:
        raise AssertionError("visibility_ids is not canonical R32_UINT")
    return width, height, list(struct.unpack("<" + "I" * (width * height), raw))


def luma(pixel: tuple[float, ...]) -> float:
    return 0.2126 * pixel[0] + 0.7152 * pixel[1] + 0.0722 * pixel[2]


def dominant_opaque_id(ids: list[int]) -> int:
    coverage: dict[int, int] = {}
    for value in ids:
        if value != 0:
            coverage[value] = coverage.get(value, 0) + 1
    if not coverage:
        raise AssertionError("opaque visibility contains no surface")
    return max(coverage, key=coverage.__getitem__)


def fixture_shadow_edge(values: list[tuple[float, ...]], width: int, height: int,
                        ids: list[int], surface_id: int) -> tuple[int, int]:
    """Locate the known right-hand direct-light shadow on the skin surface."""
    x0, x1 = round(width * 0.52), round(width * 0.64)
    y0, y1 = round(height * 0.38), round(height * 0.62)
    best = (0.0, 0, 0)
    for y in range(y0, y1):
        row = values[y * width:(y + 1) * width]
        for x in range(x0, x1 - 1):
            if ids[y * width + x] != surface_id or ids[y * width + x + 1] != surface_id:
                continue
            gradient = abs(row[x + 1][0] - row[x][0])
            if gradient > best[0]:
                best = (gradient, x, y)
    gradient, edge_x, edge_y = best
    if gradient <= 1e-5:
        raise AssertionError("right-hand skin shadow was not found")
    return edge_x, edge_y


def transition_width(values: list[tuple[float, ...]], width: int, component: int,
                     edge_x: int, edge_y: int, ids: list[int], surface_id: int) -> float:
    """Return the 10--90% width at the fixed skin shadow sample."""
    row = values[edge_y * width:(edge_y + 1) * width]
    left = edge_x - 21
    right = edge_x + 21
    if left < 0 or right >= width:
        raise AssertionError("skin shadow sample exceeds capture extent")
    if any(ids[edge_y * width + x] != surface_id for x in range(left, right + 1)):
        raise AssertionError("skin shadow sample crossed an opaque-object boundary")
    low = row[left][component]
    high = row[right][component]
    if high < low:
        low, high = high, low
    span = high - low
    if span <= 1e-5:
        raise AssertionError(f"channel {component} has no measurable edge span")
    lo_level = low + 0.1 * span
    hi_level = low + 0.9 * span
    def crossing(level: float) -> float:
        for x in range(left, right):
            first, second = row[x][component], row[x + 1][component]
            if min(first, second) <= level <= max(first, second):
                if abs(second - first) <= 1e-8:
                    return float(x)
                return x + (level - first) / (second - first)
        raise AssertionError(f"channel {component} misses transition level {level}")

    return abs(crossing(hi_level) - crossing(lo_level))


def assert_source_owned_by(source: list[tuple[float, ...]], ids: list[int],
                           surface_id: int, label: str) -> None:
    lit_ids = {object_id for pixel, object_id in zip(source, ids)
               if luma(pixel) > 1e-4}
    if lit_ids != {surface_id}:
        raise AssertionError(f"{label} source leaked outside active skin ID {surface_id}: {lit_ids}")


def assert_unchanged_outside_owner(before: list[tuple[float, ...]],
                                   after: list[tuple[float, ...]], ids: list[int],
                                   surface_id: int) -> None:
    for index, object_id in enumerate(ids):
        if object_id != surface_id and before[index] != after[index]:
            raise AssertionError(f"subsurface composite changed non-skin opaque pixel {index}")


def ordered_opaque_ids(ids: list[int], width: int) -> list[int]:
    bounds: dict[int, list[int]] = {}
    for index, object_id in enumerate(ids):
        if object_id == 0:
            continue
        x, y = index % width, index // width
        if object_id not in bounds:
            bounds[object_id] = [x, x, y, y]
        else:
            extent = bounds[object_id]
            extent[0] = min(extent[0], x)
            extent[1] = max(extent[1], x)
            extent[2] = min(extent[2], y)
            extent[3] = max(extent[3], y)
    return sorted(bounds, key=lambda object_id: bounds[object_id][0] + bounds[object_id][1])


def panel_mean(values: list[tuple[float, ...]], ids: list[int], width: int,
               object_id: int) -> float:
    points = [index for index, value in enumerate(ids) if value == object_id]
    if not points:
        raise AssertionError(f"missing furnace opaque ID {object_id}")
    xs = [index % width for index in points]
    ys = [index // width for index in points]
    center_x = (min(xs) + max(xs)) // 2
    center_y = (min(ys) + max(ys)) // 2
    samples = [luma(values[y * width + x])
               for y in range(center_y - 8, center_y + 9)
               for x in range(center_x - 8, center_x + 9)
               if ids[y * width + x] == object_id]
    if len(samples) != 17 * 17:
        raise AssertionError(f"furnace core for ID {object_id} crosses an object boundary")
    return sum(samples) / len(samples)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(__doc__.strip())
    runs = {key: Path(value) for key, value in json.loads(Path(sys.argv[1]).read_text()).items()}
    required = {"enabled", "off", "zero", "furnace", "mixed"}
    if set(runs) != required:
        raise AssertionError(f"runs must contain exactly {sorted(required)}")

    width, height, source = rgba16(runs["enabled"], "subsurface_diffuse")
    color_width, color_height, color = rgba16(runs["enabled"], "subsurface_color")
    pre_width, pre_height, pre = rgba16(runs["enabled"], "hdr_pre_transmission")
    ids_width, ids_height, ids = ids32(runs["enabled"])
    if (color_width, color_height) != (width, height) or (pre_width, pre_height) != (width, height):
        raise AssertionError("enabled source, composed color, and HDR extents differ")
    if (ids_width, ids_height) != (width, height):
        raise AssertionError("enabled opaque IDs and subsurface extent differ")
    skin_id = dominant_opaque_id(ids)
    assert_source_owned_by(source, ids, skin_id, "enabled")
    if sum(luma(pixel) > 1e-4 for pixel in source) < width * height // 100:
        raise AssertionError("subsurface diffuse source has no lit continuous-surface coverage")
    assert_unchanged_outside_owner(pre, color, ids, skin_id)

    off_hdr_width, off_hdr_height, off_hdr = rgba16(runs["off"], "hdr_pre_transmission")
    off_ids_width, off_ids_height, off_ids = ids32(runs["off"])
    if (off_hdr_width, off_hdr_height) != (width, height):
        raise AssertionError("off HDR and composed subsurface extents differ")
    if (off_ids_width, off_ids_height) != (width, height):
        raise AssertionError("off opaque IDs and HDR extent differ")
    off_skin_id = dominant_opaque_id(off_ids)
    edge_x, edge_y = fixture_shadow_edge(off_hdr, width, height, off_ids, off_skin_id)
    composed_widths = [transition_width(color, width, channel, edge_x, edge_y, ids, skin_id)
                       for channel in range(3)]
    off_widths = [transition_width(off_hdr, width, channel, edge_x, edge_y, off_ids, off_skin_id)
                  for channel in range(3)]
    if not (composed_widths[0] > composed_widths[1] > composed_widths[2]):
        raise AssertionError(f"RGB profile widths are not red > green > blue: {composed_widths}")
    if not all(composed > off for composed, off in zip(composed_widths, off_widths)):
        raise AssertionError(
            f"composed skin shadow did not soften every off-HDR channel: {off_widths} -> {composed_widths}")

    off_w, off_h, off_final = encoded_bytes(runs["off"], "final_color", "RGBA8_SRGB_PNG")
    zero_w, zero_h, zero_final = encoded_bytes(runs["zero"], "final_color", "RGBA8_SRGB_PNG")
    if (off_w, off_h) != (zero_w, zero_h):
        raise AssertionError("off and zero witness extents differ")
    if off_final != zero_final:
        raise AssertionError("zero strength final RGBA8 PNG differs from absent scene binding")
    off_w, off_h, off_post = encoded_bytes(
        runs["off"], "hdr_post_transmission", "RGBA16_FLOAT_LE")
    zero_w, zero_h, zero_post = encoded_bytes(
        runs["zero"], "hdr_post_transmission", "RGBA16_FLOAT_LE")
    if (off_w, off_h) != (zero_w, zero_h):
        raise AssertionError("off and zero post-transmission extents differ")
    if off_post != zero_post:
        raise AssertionError("zero strength post-transmission HDR differs from absent scene binding")

    furnace_w, furnace_h, furnace = rgba16(runs["furnace"], "subsurface_color")
    source_w, source_h, furnace_source = rgba16(runs["furnace"], "subsurface_diffuse")
    ids_w, ids_h, furnace_ids = ids32(runs["furnace"])
    if (furnace_w, furnace_h) != (480, 240):
        raise AssertionError(f"unexpected furnace extent {(furnace_w, furnace_h)}")
    if (source_w, source_h) != (furnace_w, furnace_h) or (ids_w, ids_h) != (furnace_w, furnace_h):
        raise AssertionError("furnace source, composite, and opaque IDs differ in extent")
    furnace_ids_left_to_right = ordered_opaque_ids(furnace_ids, furnace_w)
    if len(furnace_ids_left_to_right) != 4:
        raise AssertionError(f"furnace needs four opaque strength panels: {furnace_ids_left_to_right}")
    energy = [panel_mean(furnace, furnace_ids, furnace_w, object_id)
              for object_id in furnace_ids_left_to_right]
    source_energy = [panel_mean(furnace_source, furnace_ids, furnace_w, object_id)
                     for object_id in furnace_ids_left_to_right]
    if source_energy[0] != 0.0 or any(value <= 1e-4 for value in source_energy[1:]):
        raise AssertionError(f"furnace strength source routing is invalid: {source_energy}")
    if min(energy) <= 1e-4 or max(energy) - min(energy) > 2.0 ** -10:
        raise AssertionError(f"heterogeneous-strength furnace violates conserved uniform energy: {energy}")

    mixed_w, mixed_h, mixed_ids = ids32(runs["mixed"])
    _, _, mixed_source = rgba16(runs["mixed"], "subsurface_diffuse")
    color_w, color_h, mixed_color = rgba16(runs["mixed"], "subsurface_color")
    if (color_w, color_h) != (mixed_w, mixed_h):
        raise AssertionError("mixed composed color and visibility IDs differ in extent")
    if len(mixed_source) != len(mixed_ids):
        raise AssertionError("mixed source and visibility IDs differ in extent")
    visible = {value for value in mixed_ids if value != 0}
    if len(visible) != 3:
        raise AssertionError(f"mixed opaque visibility needs skin, coat, and emission IDs: {visible}")
    mixed_skin_id = dominant_opaque_id(mixed_ids)
    assert_source_owned_by(mixed_source, mixed_ids, mixed_skin_id, "mixed")

    print(json.dumps({
        "status": "pass",
        "skin_shadow_sample": [edge_x, edge_y],
        "off_hdr_transition_width": off_widths,
        "composed_transition_width": composed_widths,
        "zero_vs_off_final_exact": True,
        "zero_vs_off_post_transmission_exact": True,
        "furnace_luminance": energy,
        "furnace_source_luminance": source_energy,
        "mixed_visible_id_count": len(visible),
    }, indent=2))


if __name__ == "__main__":
    main()
