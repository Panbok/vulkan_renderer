#!/usr/bin/env python3
"""Sparse coverage must change reflected coverage, never constant hit radiance.

The existing scene tests do not isolate confidence normalization. These analytic
inputs execute the unchanged shared production functions through Slang's CPU
backend, with expected radiance values independent of spatial loop layout.
"""
import math
import pathlib
import subprocess
import tempfile
root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='vkr-ssr-check-') as directory:
    output = pathlib.Path(directory)
    subprocess.run([
        'slangc', '-target', 'cpp', '-entry', 'ssr_stability_execute', '-stage', 'compute',
        '-I', str(root / 'renderer/src/shaders/shared'),
        str(root / 'tools/checks/ssr_stability.slang'),
        '-o', str(output / 'ssr_stability_generated.cpp')], check=True)
    subprocess.run([
        'c++', '-std=c++17', '-O0', '-I', str(output),
        str(root / 'tools/checks/ssr_stability_main.cpp'),
        '-o', str(output / 'check')], check=True)
    actual = list(map(float, subprocess.check_output(
        [str(output / 'check')], text=True).split()))
expected = [8, 4, 2, 1/9, 8, 4, 2, 1, 0, 0, 0, 0, 5, 2.5, 5, 2/3,
            4.8, 2.4, 1.2, 0.5, 8, 4, 2, 0.4, 0, 0, 0, 0,
            0, 0, 0, 0.5, 8, 4, 2, 0.5]
assert len(actual) == len(expected), actual
for i, (got, want) in enumerate(zip(actual, expected)):
    assert math.isfinite(got) and abs(got - want) < 1e-6, (i, got, want)
print('SSR production shared math: sparse, dense, empty and mixed-confidence radiance/coverage PASS')
print('sparse:', actual[:4], 'dense:', actual[4:8], 'mixed:', actual[12:16])
print('temporal stable:', actual[16:20], 'intermittent:', actual[20:24],
      'unsupported:', actual[24:28], 'black:', actual[28:32], 'rejected:', actual[32:36])
