#!/usr/bin/env python3
"""Compare matched screen_motion_off and screen_motion_ssr/ssgi snapshots.

Screen effects may change lighting, but must not change MetalFX camera motion.
Run the paired case manifests with the same binary and local-offscreen profile.
"""
import json
import math
from pathlib import Path
import struct
import sys


def motion(run):
    report = json.loads((run / 'report.json').read_text())
    assert report['exit_code'] == 0, report['status']
    result = {}
    for item in report['captures']:
        if item['channel'] != 'motion_vectors':
            continue
        assert item['canonical_encoding'] == 'RG16_FLOAT_LE'
        data = (run / item['data_path']).read_bytes()
        extent = (item['width'], item['height'])
        assert len(data) == extent[0] * extent[1] * 4
        assert all(math.isfinite(v) for p in struct.iter_unpack('<2e', data) for v in p)
        frame = item['checkpoint_frame']
        assert frame not in result, frame
        result[frame] = extent, data
    assert result, 'No motion captures'
    return result


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: check_screen_motion.py OFF_RUN EFFECT_RUN')
    reference, effect = map(lambda value: motion(Path(value)), sys.argv[1:])
    assert reference.keys() == effect.keys(), 'Different checkpoints'
    pixels = 0
    for frame in sorted(reference):
        extent, expected = reference[frame]
        actual_extent, actual = effect[frame]
        assert actual_extent == extent, (frame, extent, actual_extent)
        different = sum(a != b for a, b in zip(struct.iter_unpack('<2e', expected),
                                               struct.iter_unpack('<2e', actual)))
        print(f'frame {frame}: {different} changed motion pixels / {extent[0] * extent[1]}')
        assert actual == expected, 'Screen effect changed the shared motion predecessor'
        pixels += extent[0] * extent[1]
    print(f'PASS: {pixels} byte-identical motion pixels')
