#!/usr/bin/env python3
"""Execute production fog math against HDR identity and geometric ray oracles.

Existing low-radiance captures cannot detect the scene cap. Analytic rays expose
far-plane clipping without depending on density integration or image sampling.
Sky-lit fog adds Henyey-Greenstein normalization and mean-cosine oracles, the
constant-colour fallback, the SH sky average against numerical integration of
the production evaluator, and the froxel source scale and cap.
This CPU execution does not establish native GPU correctness or parity.
Run with slangc and c++ on PATH: python3 tools/checks/check_froxel_regression.py
"""
import math
import pathlib
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="vkr-froxel-check-") as directory:
    output = pathlib.Path(directory)
    subprocess.run([
        "slangc", "-target", "cpp", "-entry", "froxel_regression_execute",
        "-stage", "compute", "-I", str(root / "renderer/src/shaders/shared"),
        str(root / "tools/checks/froxel_regression.slang"),
        "-o", str(output / "froxel_regression_generated.cpp")], check=True)
    subprocess.run([
        "c++", "-std=c++17", "-O0", "-I", str(output),
        str(root / "tools/checks/froxel_regression_main.cpp"),
        "-o", str(output / "check")], check=True)
    actual = list(map(float, subprocess.check_output(
        [str(output / "check")], text=True).split()))

expected = []
for radiance, scatter, weight in zip([1000, 10000, 60000], [.25, .5, 1], [.2, .5, .9]):
    expected.extend([radiance, radiance, radiance * .999 + scatter,
                     radiance * .999 + scatter * (1 - weight)])
# A unit-focal ray has x/depth=.5 and y/depth=-.5 regardless of far plane.
center_depth = .1 * 2000 ** (63.5 / 64)
for _ in range(3):
    expected.extend([100, -100, -200, center_depth / 2, -center_depth / 2,
                     -center_depth, .03, math.exp(-.01 * 199.9)])
tolerances = [(2e-6, 2e-5)] * len(expected)


def hg(g, cosine):
    return (1 - g * g) / (4 * math.pi * (1 + g * g - 2 * g * cosine) ** 1.5)


# Numerical sphere integrals: total 1 and mean cosine g for every anisotropy.
for g in [-0.6, 0.0, 0.5, 0.9]:
    expected.extend([1.0, g, (1 + g) / (4 * math.pi * (1 - g) ** 2)])
    tolerances.extend([(5e-4, 5e-4), (5e-4, 5e-4), (2e-6, 2e-5)])
expected.append(1 / (4 * math.pi))
tolerances.append((2e-6, 2e-5))

sun = [2.0, 1.5, 1.0]
ambient = [0.1, 0.2, 0.3]
for channel, colour in enumerate([0.5, 0.6, 0.7]):
    expected.extend([colour,
                     0.8 * (ambient[channel] + sun[channel] * hg(0.5, 1.0)),
                     0.8 * (ambient[channel] + sun[channel] * hg(0.5, 0.0))])
    tolerances.extend([(2e-6, 2e-5)] * 3)

# The SH average must equal the evaluator's spherical mean; compare the pair.
sh_pairs = []
for channel in range(3):
    sh_pairs.append(len(expected))
    expected.extend([None, None])
    tolerances.extend([(1e-4, 1e-4)] * 2)

# Unit-peak capping keeps hue while limiting the peak to 5,000.
for channel, (albedo, scattered, big) in enumerate(
        zip([0.9, 0.8, 0.7], [1.0, 2.0, 3.0], [1.0e4, 2.0e4, 4.0e4])):
    peak = 100.0 * 0.7 * 4.0e4
    expected.extend([0.02 * albedo * scattered,
                     100.0 * albedo * big * min(1.0, 5000.0 / peak)])
    tolerances.extend([(2e-6, 2e-5), (2e-5, 2e-4)])

assert len(actual) == len(expected), (len(actual), len(expected))
for index in sh_pairs:
    expected[index] = actual[index + 1]
    expected[index + 1] = actual[index]
for index, (value, oracle, (relative, absolute)) in enumerate(
        zip(actual, expected, tolerances)):
    assert math.isfinite(value) and math.isclose(
        value, oracle, rel_tol=relative, abs_tol=absolute), (index, value, oracle)
print(f"PASS: {len(expected)} fog HDR identity, thin-medium, far-plane, box, "
      "absorption, phase, sky-lit in-scatter, SH average and source values")
