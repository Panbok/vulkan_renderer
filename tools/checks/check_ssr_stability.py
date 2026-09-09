#!/usr/bin/env python3
"""Sparse coverage must change reflected coverage, never constant hit radiance.

The existing scene tests do not isolate confidence normalization. These analytic
inputs execute the unchanged shared production functions through Slang's CPU
backend, with expected radiance values independent of spatial loop layout.
Independent pixel boundaries expose path-dependent crossing times and incorrect
odd-extent coverage; SSGI must retain its existing half-resolution leaf.
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
            4.8, 2.4, 1.2, 0.5, 8, 4, 2, 0.4, 8, 4, 2, 0.25,
            0, 0, 0, 0.5, 8, 4, 2, 0.5,
            .5025, .395, 1, 0, 0, 1, 6.5, 6.5, 6.5, .5, 100, 120, 3.2,
            # Rough reflection accumulation is continuous across support counts.
            # Coverage stays .85*1 + .15*.25; radiance uses the same weights.
            6, 6, 6, 6, .8875, .5, 8, 10, 4.3,
            # Empty front interval, true surface crossing, rear thickness slab.
            1, 0, 0, 1, .5, 1, 0, 0, 1, 1, 0, 0, 1,
            # SSGI keeps its existing symmetric slab and entry representative.
            1, 1, 0,
            # Earliest slab support: entry, or (10.25-10.125)/0.5.
            0, .25,
            # Full pixel, three-pixel half tail, five-pixel quarter tail,
            # and the ordinary two-pixel cell immediately before the tail.
            640/641, 480/481, 1, 1,
            638/641, 478/481, 1, 1,
            636/641, 476/481, 1, 1,
            636/641, 476/481, 638/641, 478/481,
            # Solve u(t)=0.1+0.8t at 253/641, and u(t)=0.9-0.8t
            # at 388/641. Different entries in each cell share the same exit.
            (253/641-.1)/.8, (253/641-.1)/.8,
            (.9-388/641)/.8, (.9-388/641)/.8,
            # The old SSGI leaf and SSR level one instead cross at 254/641.
            (254/641-.1)/.8, (254/641-.1)/.8,
            8, 4, 2, .5**16, 0, 0, 0, 0,
            .95, .85, .90, .85, 0, .5,
            0, (1-.01/.041)/2, .1, .5,
            5, 5, 5, .6, 21.2, 20, 22, .6,
            1, 0, 0, .25, .44625, .2325, 40.125, .1953125, 6, 4, 3, 2.5, 2.25, 1/32, 8, 2]
assert len(actual) == len(expected), actual
for i, (got, want) in enumerate(zip(actual, expected)):
    assert math.isfinite(got) and abs(got - want) < 1e-6, (i, got, want)
assert actual[92] == actual[93], ('positive path-dependent exit', actual[92:94])
assert actual[94] == actual[95], ('negative path-dependent exit', actual[94:96])
print('SSR production shared math: sparse, dense, empty and mixed-confidence radiance/coverage PASS')
print('sparse:', actual[:4], 'dense:', actual[4:8], 'mixed:', actual[12:16])
print('temporal stable:', actual[16:20], 'intermittent:', actual[20:24],
      'unsupported:', actual[24:28], 'black:', actual[28:32], 'rejected:', actual[32:36])

print('jitter UV:', actual[36:38], 'identity/depth acceptance:', actual[38:41],
      'bilinear affine radiance:', actual[42:46])
print('identity grid:', actual[46:48], 'partial support:', actual[48])
print('one/two-hit retention, three-hit continuity:', actual[49:52],
      'mixed coverage:', actual[52:54], 'empty/lower/rejected:', actual[54:57])
print('sparse history already within bounds:', actual[57])
print('front-empty/crossing/rear-slab decisions:', actual[58:65])
print('loaded depth acceptance (front/surface/rear/beyond/invalid/roundoff):', actual[65:71])
print('SSGI existing symmetric slab:', actual[71:74])
print('decreasing-depth rear-slab entry/crossing:', actual[74:76])
print('odd-extent full/half/quarter tail bounds:', actual[76:88],
      'half interior:', actual[88:92])
print('path-independent full-pixel exits, positive/negative:', actual[92:96])
print('SSGI half-resolution leaf / SSR level one:', actual[96:98])

print('16-frame empty fade / rejected empty:', actual[98:106])

print('motion-adaptive rough history, mirror, disabled, midpoint:', actual[106:112])

print('SSR separated/near receivers, preserved SSGI, 2cm floor:', actual[112:116])

print("Shaded history, exact probe removal, miss/full coverage:", actual[116:124])

print("Full/half history bounds and entering reconstruction taps:", actual[124:128])

print("Fractional cone offsets and subpixel mirror movement:", actual[128:132])

print("Unsupported RGB step, empty five-frame fade, mirror clamp:", actual[132:140])
