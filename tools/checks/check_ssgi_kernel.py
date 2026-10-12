#!/usr/bin/env python3
"""Execute the production SSGI kernel through Slang's CPU backend.

Each expected value below follows from the stated geometry, not from the
kernel's code. A failure names the defect it detects:

- sampling: a short window of the 256-phase sequence leaves strata empty (the
  former linear first dimension put 16 frames inside one sixteenth of the
  elevation range);
- firefly: a lone bright sample keeps a share of its own radiance (its
  bound includes itself) or adds more than the display share to an
  isolated receiver, dense bright bounce, dim sparse bounce or bounce its
  history holds is scaled, the hue changes, or the display share ignores
  the exposure;
- plane weight: neighbours on one grazing far floor are rejected, or a
  foreground step, a turned surface or a floor seen past a counter's edge is
  accepted (the floor's own separation once widened its tolerance);
- history: a tap on a grazing floor is compared with the receiver's own
  previous depth instead of the plane's depth at that tap;
- thickness: far hits keep the fixed near-field slab;
- finite: NaN or infinity reaches a half-float image;
- all-miss: fully supported history (static or panning) loses history, a
  half-disoccluded footprint keeps full retention, or an unanswered receiver
  drops its history or keeps it unchanged;
- trace start: the trace starts at the hierarchy top or cannot ascend to it;
- placement: the history placement range probes one side only, or counts a
  probe past the plane's horizon as no change.
"""
import math
import pathlib
import subprocess
import tempfile

MAIN = """#include "ssgi_kernel_generated.cpp"
#include <cstdio>
#include <limits>
int main()
{
    float results[38];
    for (float &value : results) value = std::numeric_limits<float>::quiet_NaN();
    GlobalParams_0 globals = {};
    globals.g_results_0.data = results;
    globals.g_results_0.count = sizeof(results) / sizeof(results[0]);
    ComputeVaryingInput input = {};
    input.startGroupID = uint3(0u, 0u, 0u);
    input.endGroupID = uint3(1u, 1u, 1u);
    ssgi_kernel_execute(&input, nullptr, &globals);
    for (float value : results) std::printf("%.9g\\n", value);
}
"""

root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='vkr-ssgi-check-') as directory:
    output = pathlib.Path(directory)
    (output / 'main.cpp').write_text(MAIN)
    subprocess.run([
        'slangc', '-target', 'cpp', '-entry', 'ssgi_kernel_execute', '-stage', 'compute',
        '-I', str(root / 'renderer/src/shaders/shared'),
        str(root / 'tools/checks/ssgi_kernel_check.slang'),
        '-o', str(output / 'ssgi_kernel_generated.cpp')], check=True)
    subprocess.run([
        'c++', '-std=c++17', '-O0', '-I', str(output),
        str(output / 'main.cpp'), '-o', str(output / 'check')], check=True)
    actual = list(map(float, subprocess.check_output(
        [str(output / 'check')], text=True).split()))

expected = [
    # sampling: 16 frames fill sixteen elevation strata from any start,
    # including one across the period's end; 9 frames fill nine azimuth
    # strata; 6 frames fill the 2 x 3 cells; the period is 256.
    1, 1, 1, 1,
    # firefly: a lone 1,830 bulb hit among eight .04 samples keeps
    # .25 * 9 / .853, the radiance whose ninth adds a quarter of displayed
    # luminance (the old bound, four times a mean that includes the hit, kept
    # 813); ten among nine tens keeps 10 (bound 40); a lone .5 stays under
    # the share; a red hit of luminance 9 * .2126 alone against reference .2
    # scales to luminance .8 (above the quarter share).
    .25 * 9 / .853, 10, .5, .8 / .2126,
    # plane weight: a floor point 5.4 m further along the same plane keeps
    # the spatial term 1 / (1 + 1); a facing step of 5 m and a normal turned
    # 60 degrees weigh nothing.
    .5, 0, 0,
    # history: the ray through u = .75 at depth 4 has x = 2. Rays at v = .6
    # and v = .55 slope down by .2 and .1, so the floor y = -1 lies at depths
    # 5 and 10. The receiver's previous depth 5.5 scales to 5.5 * 10 / 5.
    2, 5, 11,
    # accepted within 5 %, foreign identity, 2 m off, 2 m off with a 2 m
    # placement allowance.
    1, 0, 0, 1,
    # thickness: 4 footprints of 2 / 1080 per metre: the 10 cm floor at 2 m,
    # 800 / 1080 m at 100 m.
    .1, 800 / 1080,
    # finite: NaN and infinity become zero; 1e6 saturates at the half maximum.
    0, 0, 65504,
    # all-miss: full support keeps .9 * 8, half support keeps
    # .9 * (.6 + .4 * .5) * 8; no answered ray keeps .95 * 8 with full alpha.
    7.2, 5.76, 7.6, 1,
    # trace start: active, at the leaf, ascending to the top of six levels.
    1, 0, 5,
    # plane weight: the floor 1 m below the counter plane weighs nothing.
    0,
    # placement: depth 100 at v = .505 caps at 100 (one-sided: 66.7); depth
    # 5 at v = .6 changes .5 / .09 - 5 toward the horizon (one-sided: .4545).
    100, .5 / .09 - 5,
    # firefly, continued: the red hit has no green; a sample alone has no
    # other mean; an isolated receiver keeps a bulb hit to .25 / .853 over
    # history .04 and passes bounce 1 over history .3 (bound 1.2); at display
    # scale 8 a lone hit among misses keeps .25 / 8.
    0, 0, .25 / .853, 1, .25 / 8,
]
assert len(actual) == len(expected), (len(actual), len(expected))
for i, (got, want) in enumerate(zip(actual, expected)):
    assert math.isfinite(got) and abs(got - want) < 1e-5 * max(1.0, abs(want)), (i, got, want)
print('SSGI sampling strata:', actual[0:4])
print('SSGI firefly bound:', actual[4:8], actual[33:38])
print('SSGI plane weights grazing/step/turned:', actual[8:11])
print('SSGI ray, plane depth, history tap depth:', actual[11:14])
print('SSGI history taps accepted/foreign/off/allowed:', actual[14:18])
print('SSGI thickness near/far:', actual[18:20])
print('SSGI finite guards:', actual[20:23])
print('SSGI all-miss supported/disoccluded/unanswered:', actual[23:27])
print('SSGI trace start active/mip/max:', actual[27:30])
print('SSGI counter/floor plane weight:', actual[30])
print('SSGI placement range near horizon / receding side:', actual[31:33])
print(f'SSGI production kernel math: {len(actual)} outputs PASS')
