#!/usr/bin/env python3
"""Inspect captured MetalFX static-accumulation output.

This checker reads snapshot artifacts only. It reports bar-region RGB variation
and mean signal separately, validates private HDR alpha ages, and checks that
presentation restores final PNG alpha to 255. Snapshot checkpoints are isolated
replays, so a cross-checkpoint age-128 RGB comparison is reported as
inconclusive rather than as a continuous-history freeze verdict.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import zlib


BAR_POLYGON = ((0, 253), (1107, 249), (1080, 283), (0, 378))
STATIC_FRAMES = (3, 4, 7, 10)
RESET_FRAMES = (0, 3, 6, 7, 134, 266, 274)
VALIDITY_FRAMES = (0, 3)
VALIDITY_EXTENT = (512, 384)
VALIDITY_TRANSPARENT_ROI = (430, 450, 175, 205)
VALIDITY_OPAQUE_ROI = (240, 270, 180, 205)


def fail(message: str) -> None:
    raise AssertionError(message)


def sha256(path: Path) -> str:
    return "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()


def load_report(run: Path) -> dict:
    path = run / "report.json"
    if not path.is_file():
        fail(f"missing snapshot report: {path}")
    report = json.loads(path.read_text())
    if report.get("status") != "pass" or report.get("exit_code") != 0:
        fail(f"snapshot did not pass: {path}")
    if not isinstance(report.get("provenance"), dict):
        fail(f"missing provenance: {path}")
    return report


def run_digest(run: Path) -> str:
    return sha256(run / "report.json")


def captures(run: Path, report: dict, channel: str) -> dict[int, tuple[dict, Path]]:
    output: dict[int, tuple[dict, Path]] = {}
    for item in report.get("captures", []):
        if item.get("channel") != channel:
            continue
        frame = item.get("checkpoint_frame")
        path = run / item.get("data_path", "")
        if not isinstance(frame, int) or frame in output or not path.is_file():
            fail(f"invalid {channel} capture in {run}")
        if sha256(path) != item.get("data_sha256"):
            fail(f"{channel} frame {frame}: data digest differs from report")
        metadata_path = run / item.get("metadata_path", "")
        if item.get("metadata_path"):
            if not metadata_path.is_file() or sha256(metadata_path) != item.get("metadata_sha256"):
                fail(f"{channel} frame {frame}: metadata digest differs from report")
        output[frame] = item, path
    return output


def require_frames(items: dict[int, tuple[dict, Path]], frames: tuple[int, ...],
                   channel: str) -> list[tuple[dict, Path]]:
    if set(items) != set(frames):
        fail(f"{channel}: expected checkpoints {list(frames)}, got {sorted(items)}")
    return [items[frame] for frame in frames]


def replay_provenance(run: Path, report: dict,
                      items: list[tuple[dict, Path]]) -> dict:
    """Prove each capture came from a separately hashed snapshot child."""
    references = {entry.get("index"): entry for entry in report.get("auxiliary_runs", [])}
    observations = []
    for item, _ in items:
        parts = Path(item["data_path"]).parts
        if len(parts) < 3 or parts[0] != "captures":
            fail(f"{item['data_path']}: not rooted in a snapshot child")
        try:
            child_index = int(parts[1])
        except ValueError as error:
            raise AssertionError(f"{item['data_path']}: invalid snapshot child") from error
        reference = references.get(child_index)
        if not reference:
            fail(f"{item['data_path']}: no auxiliary child reference")
        child_report = run / reference.get("report", "")
        if not child_report.is_file() or sha256(child_report) != reference.get("sha256"):
            fail(f"{item['data_path']}: child report digest differs from aggregate")
        observations.append({
            "checkpoint_frame": item["checkpoint_frame"],
            "child_index": child_index,
            "child_report_sha256": reference["sha256"],
            "source_frame_index": item["source_frame_index"],
            "submit_serial": item["submit_serial"],
        })
    child_indices = [observation["child_index"] for observation in observations]
    if len(set(child_indices)) != len(child_indices):
        fail("expected each snapshot checkpoint to have a distinct child replay")
    return {"independent_child_replays": True, "captures": observations}


def read_rgba16(item: dict, path: Path) -> list[tuple[float, float, float, float]]:
    if item.get("canonical_encoding") != "RGBA16_FLOAT_LE":
        fail(f"{path}: expected RGBA16_FLOAT_LE, got {item.get('canonical_encoding')}")
    width, height = item.get("width"), item.get("height")
    if not isinstance(width, int) or not isinstance(height, int) or width <= 0 or height <= 0:
        fail(f"{path}: invalid extent")
    values = list(struct.iter_unpack("<4e", path.read_bytes()))
    if len(values) != width * height:
        fail(f"{path}: expected {width * height} RGBA16F pixels, got {len(values)}")
    if not all(math.isfinite(component) for pixel in values for component in pixel):
        fail(f"{path}: non-finite HDR component")
    return values


def read_png_rgba8(path: Path, width: int, height: int) -> bytes:
    """Decode the canonical non-interlaced RGBA8 PNG without a third-party module."""
    source = path.read_bytes()
    if source[:8] != b"\x89PNG\r\n\x1a\n":
        fail(f"{path}: not a PNG")
    position, idat, seen_ihdr = 8, bytearray(), False
    while position < len(source):
        if position + 12 > len(source):
            fail(f"{path}: truncated PNG chunk")
        size = struct.unpack_from(">I", source, position)[0]
        kind = source[position + 4:position + 8]
        start, end = position + 8, position + 8 + size
        if end + 4 > len(source):
            fail(f"{path}: truncated PNG payload")
        payload = source[start:end]
        if kind == b"IHDR":
            if seen_ihdr or size != 13:
                fail(f"{path}: malformed IHDR")
            actual_width, actual_height, depth, color, compression, filtering, interlace = (
                struct.unpack(">IIBBBBB", payload))
            if (actual_width, actual_height, depth, color, compression, filtering, interlace) != (
                    width, height, 8, 6, 0, 0, 0):
                fail(f"{path}: expected non-interlaced RGBA8 {width}x{height}")
            seen_ihdr = True
        elif kind == b"IDAT":
            idat.extend(payload)
        elif kind == b"IEND":
            break
        position = end + 4
    if not seen_ihdr or not idat:
        fail(f"{path}: incomplete PNG")
    filtered = zlib.decompress(idat)
    stride, cursor, prior, output = width * 4, 0, bytearray(width * 4), bytearray()
    for _ in range(height):
        if cursor + stride + 1 > len(filtered):
            fail(f"{path}: truncated PNG scanline")
        filter_kind = filtered[cursor]
        row = bytearray(filtered[cursor + 1:cursor + 1 + stride])
        cursor += stride + 1
        for i, value in enumerate(row):
            left = row[i - 4] if i >= 4 else 0
            above = prior[i]
            upper_left = prior[i - 4] if i >= 4 else 0
            if filter_kind == 0:
                reconstructed = value
            elif filter_kind == 1:
                reconstructed = value + left
            elif filter_kind == 2:
                reconstructed = value + above
            elif filter_kind == 3:
                reconstructed = value + ((left + above) >> 1)
            elif filter_kind == 4:
                predictor = left + above - upper_left
                distances = (abs(predictor - left), abs(predictor - above),
                             abs(predictor - upper_left))
                reconstructed = value + (left if distances[0] <= distances[1] and
                                          distances[0] <= distances[2] else
                                         above if distances[1] <= distances[2] else upper_left)
            else:
                fail(f"{path}: unsupported PNG filter {filter_kind}")
            row[i] = reconstructed & 0xff
        output.extend(row)
        prior = row
    if cursor != len(filtered):
        fail(f"{path}: unexpected PNG tail")
    return bytes(output)


def point_in_bar(x: float, y: float) -> bool:
    signs = []
    for start, end in zip(BAR_POLYGON, BAR_POLYGON[1:] + BAR_POLYGON[:1]):
        signs.append((end[0] - start[0]) * (y - start[1]) -
                     (end[1] - start[1]) * (x - start[0]))
    return all(value >= 0.0 for value in signs) or all(value <= 0.0 for value in signs)


def bar_indices(width: int, height: int) -> list[int]:
    if (width, height) != (1280, 720):
        fail(f"bar metric requires 1280x720 HDR, got {width}x{height}")
    result = [y * width + x for y in range(249, 378) for x in range(1107)
              if point_in_bar(x + 0.5, y + 0.5)]
    if not result:
        fail("bar polygon contains no pixels")
    return result


def rgb_metrics(frames: list[list[tuple[float, float, float, float]]],
                indices: list[int]) -> dict:
    per_frame_means = []
    for frame in frames:
        per_frame_means.append([sum(frame[index][channel] for index in indices) / len(indices)
                                for channel in range(3)])
    ranges = []
    for index in indices:
        ranges.append(max(max(frame[index][channel] for frame in frames) -
                          min(frame[index][channel] for frame in frames)
                          for channel in range(3)))
    return {
        "bar_pixels": len(indices),
        "mean_rgb_by_capture": per_frame_means,
        "mean_rgb_delta_from_first": [[mean[channel] - per_frame_means[0][channel]
                                        for channel in range(3)]
                                       for mean in per_frame_means],
        "mean_max_rgb_range": sum(ranges) / len(ranges),
        "max_rgb_range": max(ranges),
        "pixels_rgb_range_gt_0_001": sum(value > 0.001 for value in ranges),
    }


def age(value: float, path: Path) -> int:
    rounded = round(value)
    if abs(value - rounded) > 1e-6 or rounded < 0 or rounded > 128:
        fail(f"{path}: HDR alpha is not an integer private age in [0, 128]: {value}")
    return rounded


def ages(frames: list[list[tuple[float, float, float, float]]],
         paths: list[Path]) -> list[list[int]]:
    return [[age(pixel[3], path) for pixel in frame] for frame, path in zip(frames, paths)]


def cross_replay_age128_rgb(frames: list[list[tuple[float, float, float, float]]],
                            age_frames: list[list[int]],
                            required_frames: tuple[int, ...]) -> dict:
    frozen = [index for index in range(len(frames[0]))
              if all(age_frames[frame][index] == 128 for frame in required_frames)]
    changed = 0
    changed_by_capture = []
    for index in frozen:
        reference = frames[0][index][:3]
        if any(frame[index][:3] != reference for frame in frames[1:]):
            changed += 1
    for frame in frames[1:]:
        changed_by_capture.append(sum(frame[index][:3] != frames[0][index][:3]
                                      for index in frozen))
    return {
        "verdict": "inconclusive: checkpoints are independent MetalFX replays",
        "persistent_age128_pixels": len(frozen),
        "changed_rgb_pixels": changed,
        "changed_rgb_pixels_by_later_capture": changed_by_capture,
    }


def final_metrics(run: Path, report: dict, frames: tuple[int, ...],
                  require_bar: bool) -> dict:
    final = require_frames(captures(run, report, "final_color"), frames, "final_color")
    opaque = 0
    rgb_frames = []
    for item, path in final:
        if item.get("canonical_encoding") != "RGBA8_SRGB_PNG":
            fail(f"{path}: final color is not canonical PNG")
        rgba = read_png_rgba8(path, item["width"], item["height"])
        alpha = rgba[3::4]
        if any(value != 255 for value in alpha):
            fail(f"{path}: final PNG alpha is not opaque")
        opaque += len(alpha)
        rgb_frames.append([tuple(component / 255.0 for component in rgba[index:index + 4])
                           for index in range(0, len(rgba), 4)])
    result = {"opaque_final_png_pixels": opaque}
    if require_bar:
        width, height = final[0][0]["width"], final[0][0]["height"]
        result["bar_srgb_normalized"] = rgb_metrics(rgb_frames, bar_indices(width, height))
    return result


def describe_run(run: Path, report: dict) -> dict:
    provenance = report["provenance"]
    config = report.get("effective_config", {})
    return {
        "report_sha256": run_digest(run),
        "git_sha": provenance.get("git_sha"),
        "binary_sha256": provenance.get("binary_sha256"),
        "dirty": provenance.get("dirty"),
        "backend": config.get("renderer_backend"),
        "resolution": config.get("resolution"),
        "render_resolution": config.get("render_resolution"),
        "upscaler": config.get("upscaler"),
        "ssr_enabled": config.get("ssr_enabled"),
    }


def static_run(run: Path) -> tuple[dict, list[list[tuple[float, float, float, float]]],
                                   list[list[int]]]:
    report = load_report(run)
    hdr = require_frames(captures(run, report, "hdr_pre_bloom"), STATIC_FRAMES,
                         "hdr_pre_bloom")
    post = require_frames(captures(run, report, "hdr_post_transmission"), STATIC_FRAMES,
                          "hdr_post_transmission")
    hdr_frames = [read_rgba16(item, path) for item, path in hdr]
    for item, path in post:
        read_rgba16(item, path)
    width, height = hdr[0][0]["width"], hdr[0][0]["height"]
    if any((item["width"], item["height"]) != (width, height) for item, _ in hdr):
        fail("hdr_pre_bloom capture extent changed")
    result = {
        "provenance": describe_run(run, report),
        "hdr_capture_version": hdr[0][0].get("capture_version"),
        "hdr_capture_digests": [item["data_sha256"] for item, _ in hdr],
        "replay_provenance": replay_provenance(run, report, hdr),
        "bar": rgb_metrics(hdr_frames, bar_indices(width, height)),
        "final": final_metrics(run, report, STATIC_FRAMES, True),
    }
    if result["hdr_capture_version"] == 3:
        alpha = ages(hdr_frames, [path for _, path in hdr])
        result["age128_pixels_by_capture"] = [sum(value == 128 for value in frame)
                                                for frame in alpha]
        result["cross_replay_age128_rgb"] = cross_replay_age128_rgb(
            hdr_frames, alpha, tuple(range(len(alpha))))
    else:
        alpha = []
        result["age_validation"] = "not_applicable: capture_version lacks private age metadata"
    return result, hdr_frames, alpha


def reset_run(run: Path) -> dict:
    report = load_report(run)
    hdr = require_frames(captures(run, report, "hdr_pre_bloom"), RESET_FRAMES,
                         "hdr_pre_bloom")
    scene = require_frames(captures(run, report, "scene_color"), RESET_FRAMES,
                           "scene_color")
    hdr_frames = [read_rgba16(item, path) for item, path in hdr]
    for item, path in scene:
        if item.get("canonical_encoding") != "RGBA8_SRGB_PNG":
            fail(f"{path}: expected canonical scene-color PNG")
        read_png_rgba8(path, item["width"], item["height"])
    if hdr[0][0].get("capture_version") != 3:
        fail("reset HDR capture lacks private age metadata")
    alpha = ages(hdr_frames, [path for _, path in hdr])
    for frame_index in (3, 6, 7, 134):
        index = RESET_FRAMES.index(frame_index)
        if any(value != 0 for value in alpha[index]):
            fail(f"reset frame {frame_index}: expected age zero")
    late_indices = tuple(RESET_FRAMES.index(frame) for frame in (266, 274))
    cross_replay = cross_replay_age128_rgb(
        [hdr_frames[index] for index in late_indices],
        [alpha[index] for index in late_indices], (0, 1))
    return {
        "provenance": describe_run(run, report),
        "hdr_capture_version": hdr[0][0].get("capture_version"),
        "hdr_capture_digests": [item["data_sha256"] for item, _ in hdr],
        "scene_capture_digests": [item["data_sha256"] for item, _ in scene],
        "replay_provenance": replay_provenance(run, report, hdr),
        "age128_pixels_by_capture": dict(zip(RESET_FRAMES,
                                               (sum(value == 128 for value in frame)
                                                for frame in alpha))),
        "cross_replay_age128_rgb_late": cross_replay,
        "final": final_metrics(run, report, RESET_FRAMES, False),
    }


def roi_indices(extent: tuple[int, int], roi: tuple[int, int, int, int]) -> list[int]:
    width, height = extent
    x0, x1, y0, y1 = roi
    if not (0 <= x0 < x1 <= width and 0 <= y0 < y1 <= height):
        fail(f"ROI {roi} is outside {width}x{height}")
    return [y * width + x for y in range(y0, y1) for x in range(x0, x1)]


def opaque_png_pixels(items: list[tuple[dict, Path]], channel: str) -> int:
    opaque = 0
    for item, path in items:
        if item.get("canonical_encoding") != "RGBA8_SRGB_PNG":
            fail(f"{path}: {channel} is not canonical PNG")
        rgba = read_png_rgba8(path, item["width"], item["height"])
        alpha = rgba[3::4]
        if any(value != 255 for value in alpha):
            fail(f"{path}: {channel} PNG alpha is not opaque")
        opaque += len(alpha)
    return opaque


def validity_run(run: Path) -> dict:
    """Check the transparent/opaque and scene-change age contracts."""
    report = load_report(run)
    hdr = require_frames(captures(run, report, "hdr_pre_bloom"), VALIDITY_FRAMES,
                         "hdr_pre_bloom")
    scene = require_frames(captures(run, report, "scene_color"), VALIDITY_FRAMES,
                           "scene_color")
    hdr_frames = [read_rgba16(item, path) for item, path in hdr]
    if any((item["width"], item["height"]) != VALIDITY_EXTENT for item, _ in hdr):
        fail(f"validity HDR extent must remain {VALIDITY_EXTENT}")
    if any(item.get("capture_version") != 3 for item, _ in hdr):
        fail("validity HDR capture lacks private age metadata")
    alpha = ages(hdr_frames, [path for _, path in hdr])
    transparent = roi_indices(VALIDITY_EXTENT, VALIDITY_TRANSPARENT_ROI)
    opaque = roi_indices(VALIDITY_EXTENT, VALIDITY_OPAQUE_ROI)
    if any(alpha[0][index] != 0 for index in transparent):
        fail("validity frame 0 transparent ROI must have age zero")
    if any(alpha[0][index] != 128 for index in opaque):
        fail("validity frame 0 opaque ROI must have age 128")
    if any(value != 0 for value in alpha[1]):
        fail("validity frame 3 scene change must reset every age to zero")
    return {
        "provenance": describe_run(run, report),
        "hdr_capture_version": hdr[0][0].get("capture_version"),
        "hdr_capture_digests": [item["data_sha256"] for item, _ in hdr],
        "scene_capture_digests": [item["data_sha256"] for item, _ in scene],
        "replay_provenance": replay_provenance(run, report, hdr),
        "frame0_transparent_age0_pixels": len(transparent),
        "frame0_opaque_age128_pixels": len(opaque),
        "frame3_age0_pixels": len(alpha[1]),
        "scene_png_opaque_pixels": opaque_png_pixels(scene, "scene color"),
        "final": final_metrics(run, report, VALIDITY_FRAMES, False),
    }


def static_comparison(before: dict, after: dict) -> dict:
    same_binary = before["provenance"]["binary_sha256"] == after["provenance"]["binary_sha256"]
    same_capture_version = before["hdr_capture_version"] == after["hdr_capture_version"]
    reasons = []
    if not same_binary:
        reasons.append("binary_sha256 differs")
    if not same_capture_version:
        reasons.append("hdr capture_version differs; alpha metadata differs")
    return {
        "authoritative_ab_comparison": not reasons,
        "non_authoritative_reasons": reasons,
        "mean_max_rgb_range": {
            "before": before["bar"]["mean_max_rgb_range"],
            "after": after["bar"]["mean_max_rgb_range"],
        },
        "mean_rgb_by_capture": {
            "before": before["bar"]["mean_rgb_by_capture"],
            "after": after["bar"]["mean_rgb_by_capture"],
        },
        "final_srgb_mean_max_rgb_range": {
            "before": before["final"]["bar_srgb_normalized"]["mean_max_rgb_range"],
            "after": after["final"]["bar_srgb_normalized"]["mean_max_rgb_range"],
        },
        "final_srgb_mean_rgb_by_capture": {
            "before": before["final"]["bar_srgb_normalized"]["mean_rgb_by_capture"],
            "after": after["final"]["bar_srgb_normalized"]["mean_rgb_by_capture"],
        },
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--static-before", type=Path)
    parser.add_argument("--static-after", type=Path)
    parser.add_argument("--reset", type=Path)
    parser.add_argument("--validity-run", type=Path)
    args = parser.parse_args()
    if not args.static_before and not args.static_after and not args.reset and not args.validity_run:
        parser.error("provide --static-before/--static-after, --reset, and/or --validity-run")
    if bool(args.static_before) != bool(args.static_after):
        parser.error("--static-before and --static-after must be supplied together")

    output: dict = {"status": "pass"}
    if args.static_before:
        before, _, _ = static_run(args.static_before)
        after, _, _ = static_run(args.static_after)
        output["static_before"] = before
        output["static_after"] = after
        output["static_comparison"] = static_comparison(before, after)
    if args.reset:
        output["reset"] = reset_run(args.reset)
    if args.validity_run:
        output["validity"] = validity_run(args.validity_run)
    print(json.dumps(output, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
