#!/usr/bin/env python3
"""Compare before/after ssr_static_jitter snapshots on the same Metal host.

The raw trace must remain unchanged within FP16 noise while temporal filtering
reduces static-scene variation by at least 25%. This detects the native history
selection failure that shared shader arithmetic and build checks cannot reveal.
"""
import json
import math
from pathlib import Path
import struct
import sys

from check_screen_motion import motion


def read(run):
    report = json.loads((run / 'report.json').read_text())
    assert report['exit_code'] == 0, report['status']
    channels = {}
    for channel in ('ssr_raw', 'ssr_reflection'):
        captures = sorted((c for c in report['captures'] if c['channel'] == channel),
                          key=lambda c: c['checkpoint_frame'])
        assert len(captures) >= 3, 'Need at least three static checkpoints'
        frames = {}
        for c in captures:
            assert c['canonical_encoding'] == 'RGBA16_FLOAT_LE'
            values = list(struct.iter_unpack('<4e', (run / c['data_path']).read_bytes()))
            assert len(values) == c['width'] * c['height']
            assert all(math.isfinite(v) for p in values for v in p)
            key = (c['checkpoint_frame'], c['width'], c['height'], c['capture_version'])
            assert key not in frames
            frames[key] = values
        channels[channel] = frames
    return report['effective_config'], channels


def variation(frames):
    # Mean RGB multiplied by coverage is the reflected signal before material
    # weighting. The static camera makes consecutive pixel comparisons valid.
    signals = [[sum(p[:3]) * p[3] / 3 for p in values] for values in frames.values()]
    deltas = sorted(abs(a - b) for x, y in zip(signals, signals[1:]) for a, b in zip(x, y))
    return (sum(deltas) / len(deltas), deltas[int(0.95 * len(deltas))],
            sum(map(sum, signals)) / sum(map(len, signals)))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: check_ssr_history.py BEFORE_RUN AFTER_RUN')
    before_run, after_run = map(Path, sys.argv[1:])
    before_config, before = read(before_run)
    after_config, after = read(after_run)
    assert before_config == after_config, 'Configurations differ'
    assert before_config['ssr_enabled'] and before_config['taa_enabled']
    assert not before_config['dynamic_resolution']
    for channel in before:
        assert before[channel].keys() == after[channel].keys(), 'Capture extents/checkpoints/semantics differ'
    old_motion, new_motion = motion(before_run), motion(after_run)
    assert old_motion.keys() == new_motion.keys()
    for frame in old_motion:
        old_extent, old_data = old_motion[frame]
        new_extent, new_data = new_motion[frame]
        assert old_extent == new_extent
        assert list(struct.iter_unpack('<2e', old_data)) == list(struct.iter_unpack('<2e', new_data))
    raw_delta = [abs(a - b) for key in before['ssr_raw']
                 for x, y in zip(before['ssr_raw'][key], after['ssr_raw'][key])
                 for a, b in zip(x, y)]
    raw_mae = sum(raw_delta) / len(raw_delta)
    assert raw_mae < 1e-5, ('Raw trace changed', raw_mae)
    old_mean, old_p95, old_energy = variation(before['ssr_reflection'])
    new_mean, new_p95, new_energy = variation(after['ssr_reflection'])
    print(f'raw trace MAE: {raw_mae:.9g}; motion unchanged')
    print(f'covered radiance mean: {old_energy:.9g} -> {new_energy:.9g}')
    print(f'variation mean: {old_mean:.9g} -> {new_mean:.9g}')
    print(f'variation p95: {old_p95:.9g} -> {new_p95:.9g}')
    assert old_mean > 0 and new_mean < old_mean * 0.75, 'History did not reduce flicker'
    print(f'PASS: mean static reflection variation reduced by {100 * (1 - new_mean / old_mean):.2f}%')
