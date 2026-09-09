#!/usr/bin/env python3
"""Sparse coverage must change reflected coverage, never constant hit radiance.

The existing scene tests do not isolate confidence normalization. These analytic
inputs execute the shared production functions through Slang's CPU
backend, with expected radiance values independent of spatial loop layout.
Independent pixel boundaries expose path-dependent crossing times and incorrect
odd-extent coverage; SSGI must retain its existing half-resolution leaf.
An in-bounds old reflection must lose at least 15% of its RGB error per supported
frame at weight .85, even when fresh coverage is only .05.
Planar mirror geometry independently distinguishes reflected-feature motion from
receiver motion. Known affine point/normal transforms, jittered projections and
history rejection cases exercise the production reprojection helpers directly.
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
            4.8, 2.4, 1.2, 0.5, 8, 4, 2, 0.25, 8, 4, 2, 0.25,
            0, 0, 0, 0.5, 8, 4, 2, 0.5,
            .5025, .395, 1, 0, 0, 1, 6.5, 6.5, 6.5, .5, 100, 120, 3.2,
            # Rough reflection accumulation is continuous across support counts.
            # Coverage stays .85*1 + .15*.25; unsupported RGB residual is capped.
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
            10, 10, 10, .6, 23, 20, 25, .6,
            1, 0, 0, .25, .44625, .2325, 40.125, .1953125, 6, 4, 3, 2.5, 2.25, 1/32, 8, 2,
            # A 15% fresh RGB share gives this independent geometric error bound.
            *[2 + 6 * .85**frame for frame in range(1, 6)], .8575,
            8, 4, 2, .8575, 3.324675324675325, .1925, 0, 0, 0, 0,
            # Mirror image (.6,0,-3) viewed from camera x=-.2 is at u=19/30.
            # The current receiver (.4,0,-2) instead projects to u=.65.
            19/30, .5, 2, 3, 3, 1, .65, 1/60,
            # Full-resolution offsets from a trace receiver survive reprojection.
            .6078125, .49375, 2, 3, 3, 1,
            .6078125 + 1/30, .49375, 2, 3, 3, 1,
            # Raster jitter changes UV without moving the tilted receiver plane.
            .5025, .495, 2, 3, 3, 1,
            # A reflected emitter moves from x=.6 to .4 in previous object pose.
            17/30, .5, 2, 3, 3, 1,
            # Prior receiver plane z=-2.5 mirrors the emitter to depth 4.
            .575, .5, 2.5, 4, 3, 1,
            # Object point (1,2,-1) under the independently specified prior model.
            -7, 7, 6.5, 1, -7, 7, 6.5, 1,
            -3/math.sqrt(157), 2/math.sqrt(157), 12/math.sqrt(157),
            # Singular current/prior models, NaN point, zero normal; invalid rays.
            0, 0, 0, 0, 0, 0, 0, 0,
            .85, 1,
            # Each identity word; receiver/virtual depth; normal; history absent;
            # invalid hit; offscreen; missing hit/receiver; zero virtual depth.
            *([0] * 13),
            # Signed octahedral encoding has a known positive and folded branch.
            6/13, 4/13, 6/math.sqrt(61), 4/math.sqrt(61), 3/math.sqrt(61),
            -9/13, 7/13, -6/math.sqrt(61), 4/math.sqrt(61), -3/math.sqrt(61),
            .4, .2, -2, .2, .1, -2,
            # Orthographic translation is independent of reflected-hit depth.
            .9, .5, 2, 3, 3, 1,
            # One incoming history value receives only current material weights.
            2, 4, 8, .6,
            # A mirrored current affine transform preserves the same object data.
            -7, 7, 6.5, 1,
            -3/math.sqrt(157), 2/math.sqrt(157), 12/math.sqrt(157), 1,
            # Current and previous raster jitter are both nonzero and unequal.
            .6090625, .49625, 2, 3, 3, 1]
assert len(actual) == len(expected), actual
for i, (got, want) in enumerate(zip(actual, expected)):
    assert math.isfinite(got) and abs(got - want) < 1e-6, (i, got, want)
assert actual[92] == actual[93], ('positive path-dependent exit', actual[92:94])
assert actual[94] == actual[95], ('negative path-dependent exit', actual[94:96])
previous_error = 6.0
for value in actual[140:145]:
    error = value - 2.0
    assert 0 <= error <= .85 * previous_error + 1e-6, ('old RGB retention', error, previous_error)
    previous_error = error
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

print("Incoming history, current shading, exact probe removal:", actual[116:124])

print("Full/half history bounds and entering reconstruction taps:", actual[124:128])

print("Fractional cone offsets and subpixel mirror movement:", actual[128:132])

print("Unsupported RGB step, empty five-frame fade, mirror clamp:", actual[132:140])
print("Sparse in-bounds old-image step / coverage:", actual[140:146])
print("Constant sparse radiance / weak history / rejected miss:", actual[146:156])
print("Planar reflection reprojection / receiver-motion error:", actual[156:164])
print("Full-resolution stationary/translated offsets:", actual[164:176])
print("Jitter with tilted receiver:", actual[176:182])
print("Moving emitter / moving receiver plane:", actual[182:194])
print("Affine point/surface/normal transport:", actual[194:205])
print("Invalid transforms and ray-plane intersections:", actual[205:213])
print("Reflected correspondence acceptance/rejection:", actual[213:228])
print("Signed octahedral normal branches:", actual[228:238])
print("Perspective/orthographic depth reconstruction:", actual[238:244])
print("Orthographic reflected motion:", actual[244:250])
print("Current receiver response:", actual[250:254])
print("Mirrored affine transport:", actual[254:262])
print("Unequal current/previous raster jitter:", actual[262:268])
print(f"SSR production shared math: {len(actual)} outputs PASS")
