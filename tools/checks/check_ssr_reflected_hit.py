#!/usr/bin/env python3
"""Check the moving-emitter SSR fixture's disappearance and material contract.

The backend-neutral ssr_reflected_hit_motion case translates SSRRedEmitter behind
foreground coverage while its floor receiver stays fixed. In the left floor
region, red trace support (ssr_raw) exists at frame 3 and is absent at frame 8.
History (ssr_reflection) must not keep a red reflection after that support
disappears. A mirror or glossy receiver's hold ends within four frames; a rough
receiver (no reflection parallax) counts a frame without hits as a sample of
zero coverage, so faint red may remain there while it fades over about twenty
frames (ADR-055). Frame 8 therefore bounds the red light history keeps instead
of its red pixel count:

- The coverage-weighted red energy, the sum of red radiance times coverage over
  the region's red pixels, measured in fully covered pixels of the traced
  reflection (the median red radiance of ssr_raw's red pixels at frame 3), is
  at most four. Four is one row as wide as the traced reflection (about 27
  pixels) at 15 % coverage. A stale opaque copy of the smallest traced
  reflection seen (7 pixels) exceeds it, as did the 2026-10-11 hold that kept
  full-strength radiance (16 pixels) and a zero-sample rule for every lobe (30
  to 60 pixels).
- No red pixel keeps more than half coverage, which only an opaque stale
  reflection holds.

The independent shared-math oracle covers exact reprojection coordinates; this
checks native bindings, incoming-radiance payloads and the
missing-correspondence path.
"""
import hashlib
import json
import math
from pathlib import Path
import statistics
import struct
import sys

# Red light the history may keep at frame 8, in fully covered pixels of the
# traced reflection, and the coverage above which a red pixel is opaque.
LINGERING_RED_PIXELS_MAX = 4.0
OPAQUE_COVERAGE = 0.5


def capture_rows(run, report):
    rows = []
    for capture in report['captures']:
        channel = capture['channel']
        if channel not in ('ssr_raw', 'ssr_reflection', 'motion_vectors'):
            continue
        if channel == 'ssr_reflection':
            assert capture['capture_version'] == 6, 'Expected incoming radiance'
        data = (run / capture['data_path']).read_bytes()
        assert 'sha256:' + hashlib.sha256(data).hexdigest() == capture['data_sha256']
        width, height = capture['width'], capture['height']
        motion = channel == 'motion_vectors'
        assert len(data) == width * height * (4 if motion else 8)
        covered_count = 0
        red_radiance = []
        red_energy = red_x = red_coverage_max = max_motion = 0.0
        for index, pixel in enumerate(struct.iter_unpack('<2e' if motion else '<4e', data)):
            assert all(math.isfinite(value) for value in pixel), (channel, index, pixel)
            if motion:
                max_motion = max(max_motion, abs(pixel[0]) * width,
                                 abs(pixel[1]) * height)
                continue
            assert 0 <= pixel[3] <= 1, (channel, index, pixel[3])
            covered_count += pixel[3] > 0
            x, y = index % width, index // width
            if (x < width // 2 and height * .52 < y < height * .735 and
                    pixel[0] > 2 * max(pixel[1], pixel[2]) and pixel[0] > .1):
                red_radiance.append(pixel[0])
                weight = pixel[0] * pixel[3]
                red_energy += weight
                red_x += x * weight
                red_coverage_max = max(red_coverage_max, pixel[3])
        rows.append({'channel': channel, 'frame': capture['checkpoint_frame'],
                     'covered_pixels': covered_count,
                     'red_pixels': len(red_radiance),
                     'red_centroid_x': red_x / red_energy if red_energy else None,
                     'red_energy': red_energy,
                     'red_radiance_median':
                         statistics.median(red_radiance) if red_radiance else None,
                     'red_coverage_max': red_coverage_max,
                     'max_motion_pixels': max_motion, 'sha256': capture['data_sha256']})
    return rows


def lingering_red_pixels(rows):
    """Frame-8 history red energy in fully covered traced-reflection pixels."""
    raw = {row['frame']: row for row in rows if row['channel'] == 'ssr_raw'}
    history = {row['frame']: row for row in rows if row['channel'] == 'ssr_reflection'}
    return history[8]['red_energy'] / raw[3]['red_radiance_median']


def check(run):
    report = json.loads((run / 'report.json').read_text())
    assert report['status'] == 'pass' and report['exit_code'] == 0
    assert report['case']['id'] == 'local.ssr.reflected_hit_motion'
    assert not report['effective_config']['taa_enabled']
    rows = capture_rows(run, report)
    samples = {}
    for channel in ('ssr_raw', 'ssr_reflection'):
        samples[channel] = {row['frame']: row for row in rows if row['channel'] == channel}
        frames = samples[channel]
        assert set(frames) == {3, 4, 8}, (channel, frames.keys())
        assert frames[3]['red_pixels'] > 0, 'Fixture did not produce red reflection'
        assert frames[4]['red_centroid_x'] is not None and \
            frames[4]['red_centroid_x'] > frames[3]['red_centroid_x'], \
            'Reflected emitter did not move right'
        assert frames[8]['covered_pixels'] > 0, 'SSR was disabled instead of rejecting the hit'
    assert samples['ssr_raw'][8]['red_pixels'] == 0, \
        ('ssr_raw', 'Red trace support remains at frame 8')
    lingering = lingering_red_pixels(rows)
    assert lingering <= LINGERING_RED_PIXELS_MAX, \
        ('ssr_reflection', 'Lingering red reflection', lingering)
    opaque = samples['ssr_reflection'][8]['red_coverage_max']
    assert opaque <= OPAQUE_COVERAGE, ('ssr_reflection', 'Opaque red history', opaque)
    assert max(row['max_motion_pixels'] for row in rows) > 1, 'Rigid motion was not exercised'
    print(json.dumps({'status': 'pass', 'run': str(run),
                      'lingering_red_pixels': lingering, 'samples': rows}, indent=2))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: check_ssr_reflected_hit.py SNAPSHOT_RUN')
    check(Path(sys.argv[1]))
