#!/usr/bin/env python3
"""Sparse coverage must change reflected coverage, never constant hit radiance.

The existing scene tests do not isolate confidence normalization. These analytic
inputs execute the shared production functions through Slang's CPU
backend, with expected radiance values independent of spatial loop layout.
Independent pixel boundaries expose path-dependent crossing times and incorrect
odd-extent coverage; SSGI must retain its existing half-resolution leaf.
An in-bounds old reflection must lose at least 15% of its RGB error per supported
frame at weight .85, even when fresh coverage is only .05. Without a current hit,
a mirror's known miss and glossy-lobe misses that the history's coverage made
unlikely drop history at once; unknown rays and plausible sparse misses halve
a glossy lobe's coverage, and the hold ends at the 1/16 floor within four
frames. A rough lobe (no reflection parallax) instead counts a frame without
hits as a zero-coverage sample at the ordinary retention, so sparse hits do
not collapse its coverage, even below the floor; refuting misses still end it
at the floor.
Planar mirror geometry independently distinguishes reflected-feature motion from
receiver motion. Known affine point/normal transforms, jittered projections and
history rejection cases exercise the production reprojection helpers directly.
Weighted RGB contributions independently identify the reflected object that
dominates incoming light, including dark coverage, black ties and disappearance.
Closed-form GGX visible-normal samples, a numerically integrated pdf, ray-record
round trips, ratio-estimator sums and reversed-depth (0.1 m / 20 km) planes and
reprojection check the lobe-sampling rework and the near-side unprojection.
The TAA effective sample count seeds the static integral. A Catmull-Rom
history stays inside its inner taps' range or yields to bilinear, and the
receiver-plane tolerance follows the plane's predicted separation, so a floor
seen past a counter's edge cannot widen its own tolerance.
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
            # Unknown outcome halves .5 coverage; decisive known misses drop it.
            4.8, 2.4, 1.2, 0.5, 8, 4, 2, .5 * .5, 0, 0, 0, 0,
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
            # An all-unknown hold ended within sixteen frames; rejected empty.
            0, 0, 0, 0, 0, 0, 0, 0,
            .95, .85, .90, .85, 0, .5,
            # Plane test: 8 cm off the plane rejects, 4 m along it keeps one.
            0, 1, .1, .5,
            10, 10, 10, .6, 23, 20, 25, .6,
            # Reuse footprint; roughness coverage fade from .45 to the .6 cutoff;
            # a decisive miss after a vanished reflection drops it at once.
            1, 0, .5, 1, 1, 1, .5, 0, 6, 4, 3, 2.5, 2.25, 0, 0, 2,
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
            .6090625, .49625, 2, 3, 3, 1,
            # RGB numerator .32 beats .0008; .1 loses to .8. Equal energy
            # favors covered weight, including black; exact ties preserve order.
            1, 0, 1, 0, 1, 0, 0, 0, 1,
            # Red .8 beats cyan .6; covered light .08 loses to .5;
            # blue .8 beats yellow .6.
            1, 0, 1,
            # Identity/tap: the lamp moves among taps, then loses all support.
            # A black neighborhood selects the widest covered current trace.
            17, 1, 17, 0, 17, 2, 23, 0, 31, 1]
expected.extend([7.25, 5, 6, 3, 7.25, 1, 0, 6, 4, 0])
# Independent normalized tangent-space vectors for strengths 0, .5, 1, 2.
# Unit strength cannot catch the former Z-after-strength reconstruction.
expected.extend([
    0, 0, 1,
    .35856858280031806, -.11952286093343936, .9258200997725515,
    .6, -.2, .7745966692414834,
    .8090398349558905, -.26967994498529685, .5222329678670935,
    0, 0, 1,
    1/math.sqrt(2), -1/math.sqrt(2), 0,
    .8090398349558905, -.26967994498529685, .5222329678670935,
])
# Short-segment restart: four distinct cells and no repeated cell. A minor-axis
# exit from cell (88, 244) through a badly rounded boundary crosses within 16
# steps (the former parameter epsilon needed 69) without skipping a major cell.
expected.extend([0, 4, 88244, 1, 0])
# A well-rounded boundary keeps the exact epsilon restart; boundary 963 of 1920
# restarts 1/64 pixel along the 3.84-pixel major axis.
expected.extend([1, 0.015625 / 3.84])
# Correspondence rules: static, surface candidate, hit identity only (receiver
# foreign / hit foreign), receiver identity only with a foreign hit.
expected.extend([1, 1, 1, 0, 1])
# Parallax: surface motion .65, half parallax .64 with virtual depth 2.5 now
# and before; the roughness blend's ends and midpoint.
expected.extend([.65, .64, 2.5, 2.5, 1, .5, 0])
# Plane y+z=-2 at v=.75, .25, the horizon row; a plane containing the ray.
expected.extend([4/3, 4, 2, 0])
# Reversed depth: previous u in 1280-pixel units, receiver and virtual
# depth, validity, the point at 100 m, the tilted plane and a 96 m floor.
expected.extend([19/30 * 1280, 2, 3, 1, 20, 10, -100, 4/3, 96])
# GGX lobe: isotropic sample, pdf, own ratio weight G2/G1; anisotropic sample;
# pdf integral; zero below the surface; capped reuse weight.
d_peak = 1 / (math.pi * .25 * 1.6**2)
expected.extend([.8, 0, .6, d_peak / 4, 1.2 / (.6 + math.sqrt(.52)),
                 0, 8/17, 15/17, 1, 0, 4])
# Ray records: hit state, UV within half a step, depth, pdf, visible row;
# unknown state, direction, pdf; miss state; empty record.
expected.extend([3, 1, 3.5, 2, 7, 2, 1, .5, 1, 0])
# Ratio-estimator sums: radiance, coverage over known weight, known share.
expected.extend([4, 2, 1, .25, .5])
# Unknown fill 9.4 at coverage 1; tight 3.7 and residual 6.7; clip [1, 3];
# storable NaN, overflow, finite.
expected.extend([9.4, 1, 3.7, 6.7, 1, 3, 0, 65504, 3])
# TAA effective samples: running mean n+1, constant .9 -> 19, .99 -> 127, none.
expected.extend([10, 5, 19, 127, 1])
# No current hit: a .2 lamp coverage halves (1.8 expected hits); coverage .5
# (4.5 expected) is refuted; .4 (3.6 expected) holds .4 * .5 * exp(-.6); a
# mirror's known miss drops and its unknown ray holds .5; an all-unknown full
# hold keeps RGB 8 at .125 after three frames and ends on the fourth.
expected.extend([8, .1, 0, .4 * .5 * math.exp(-.6), 0, .5, 8, .125, 0])
# Catmull-Rom history: the coverage-edge cubic yields to bilinear; ringing to
# 1.27 clamps to the inner radiance 1; coverage .63 clamps to the inner .5.
expected.extend([0, 1, 1, 1, .5])
# Plane test: the floor past a counter edge weighs nothing; a grazing-floor
# point .3 m off its plane keeps 1 - .3 / (.25 * its separation).
expected.extend([0, 1 - .3 / (.25 * math.sqrt(.05**2 + .3**2 + 5.4**2))])
# Rough lobe without hits, retention .85: all-unknown .85^3 and .85^4, the .2
# lamp .17, coverage .5 refuted to .5 * .85 * exp(-1.5), full coverage refuted
# below the floor (.85 * exp(-6) < 1/16); roughness .39 halves the lamp; a
# .07 sparse hit keeps .07 * .85 below the floor.
expected.extend([.85**3, .85**4, .17, .5 * .85 * math.exp(-1.5), 0, .1,
                 .07 * .85])
# Reversed-depth outputs are float32 extrapolations from points near the
# camera; their tolerances are 1e-3 pixel and 0.1 mm. Unprojecting the far
# plane instead misses them by 0.24 mm at 100 m and 1.3 mm on the tilted
# plane. The pdf integral is a 512x1024 midpoint sum; the iterated .9
# fixed point keeps float32 rounding.
tolerance = {344: 1e-3, 345: 1e-4, 346: 1e-4, 348: 1e-4, 349: 1e-4, 350: 1e-4,
             351: 1e-4, 352: 1e-4, 361: 2e-3, 390: 1e-4}
assert len(actual) == len(expected), (len(actual), len(expected))
for i, (got, want) in enumerate(zip(actual, expected)):
    assert math.isfinite(got) and abs(got - want) < tolerance.get(i, 1e-6), \
        (i, got, want)
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

print('SSR plane test separated/grazing, preserved SSGI, 2cm floor:', actual[112:116])

print("Incoming history, current shading, exact probe removal:", actual[116:124])

print("Full/half history bounds, reuse edge weight and footprint:", actual[124:128])

print("Roughness coverage fade at .3/.45/.525/.6:", actual[128:132])

print("Unsupported RGB step, decisive-miss drop, mirror clamp:", actual[132:140])
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
print("Radiance contribution, coverage ties and missing samples:", actual[268:280])
print("Representative identity/tap across lamp movement and disappearance:", actual[280:290])
print("Short-segment repeats/cells, minor-axis start/crossed/skipped:", actual[321:326])
print("Exact epsilon restart kept / rounded-boundary restart step:", actual[326:328])
print("Correspondence match rules:", actual[328:333])
print("Parallax reprojection and blend:", actual[333:340])
print("Receiver plane depth:", actual[340:344])
print("Reversed 0.1 m / 20 km depth:", actual[344:353])
print("GGX lobe samples, pdf, ratio, integral, horizon, cap:", actual[353:364])
print("Ray records:", actual[364:374])
print("Ratio-estimator sums:", actual[374:379])
print("Unknown fill, tight/residual, clip, storable:", actual[379:388])
print("TAA effective samples:", actual[388:393])
print("No-hit hold: lamp, refuted, partial, mirror miss/unknown, 3/4 frames:",
      actual[393:402])
print("Catmull-Rom edge fallback, ringing clamp, coverage clamp:", actual[402:407])
print("Plane test counter/floor, grazing tilt:", actual[407:409])
print("Rough hold three/four frames, lamp, refuted .5/1, glossy .39 lamp, sparse .07:",
      actual[409:416])
print(f"SSR production shared math: {len(actual)} outputs PASS")

print("Normal-RG strength 0/.5/1/2, disk overflow, zero fallback and ignored blue: PASS")
