#!/usr/bin/env python3
"""Check the moving-emitter SSR fixture's disappearance and material contract.

The backend-neutral ssr_reflected_hit_motion case translates SSRRedEmitter behind
foreground coverage while its floor receiver stays fixed. In the left floor
region, red trace support exists at frame 3 and is absent at frame 8. History
must not retain a red reflection after that support disappears. The independent
shared-math oracle covers exact reprojection coordinates; this checks native
bindings, incoming-radiance payloads and the missing-correspondence path.
"""
import hashlib
import json
import math
from pathlib import Path
import struct
import sys


def check(run):
    report = json.loads((run / 'report.json').read_text())
    assert report['status'] == 'pass' and report['exit_code'] == 0
    assert report['case']['id'] == 'local.ssr.reflected_hit_motion'
    assert not report['effective_config']['taa_enabled']
    rows = []
    for capture in report['captures']:
        channel = capture['channel']
        if channel not in ('ssr_raw', 'ssr_reflection', 'motion_vectors'):
            continue
        if channel == 'ssr_reflection':
            assert capture['capture_version'] == 5, 'Expected incoming radiance'
        data = (run / capture['data_path']).read_bytes()
        assert 'sha256:' + hashlib.sha256(data).hexdigest() == capture['data_sha256']
        width, height = capture['width'], capture['height']
        motion = channel == 'motion_vectors'
        assert len(data) == width * height * (4 if motion else 8)
        red_count = covered_count = 0
        red_weight = red_x = max_motion = 0.0
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
                red_count += 1
                weight = pixel[0] * pixel[3]
                red_weight += weight
                red_x += x * weight
        rows.append({'channel': channel, 'frame': capture['checkpoint_frame'],
                     'covered_pixels': covered_count, 'red_pixels': red_count,
                     'red_centroid_x': red_x / red_weight if red_weight else None,
                     'max_motion_pixels': max_motion, 'sha256': capture['data_sha256']})
    for channel in ('ssr_raw', 'ssr_reflection'):
        samples = {row['frame']: row for row in rows if row['channel'] == channel}
        assert set(samples) == {3, 4, 8}, (channel, samples.keys())
        assert samples[3]['red_pixels'] > 0, 'Fixture did not produce red reflection'
        assert samples[4]['red_centroid_x'] > samples[3]['red_centroid_x'], \
            'Reflected emitter did not move right'
        assert samples[8]['red_pixels'] == 0, (channel, 'Lingering red reflection')
        assert samples[8]['covered_pixels'] > 0, 'SSR was disabled instead of rejecting the hit'
    assert max(row['max_motion_pixels'] for row in rows) > 1, 'Rigid motion was not exercised'
    print(json.dumps({'status': 'pass', 'run': str(run), 'samples': rows}, indent=2))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: check_ssr_reflected_hit.py SNAPSHOT_RUN')
    check(Path(sys.argv[1]))
