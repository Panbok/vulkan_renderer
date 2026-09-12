#!/usr/bin/env python3
"""Execute production fog math against HDR identity and geometric ray oracles.

Existing low-radiance captures cannot detect the scene cap. Analytic rays expose
far-plane clipping without depending on density integration or image sampling.
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
assert len(actual) == len(expected), (len(actual), len(expected))
for index, (value, oracle) in enumerate(zip(actual, expected)):
    assert math.isfinite(value) and math.isclose(value, oracle, rel_tol=2e-6, abs_tol=2e-5), (
        index, value, oracle)
print("PASS: 36 fog HDR identity, thin-medium, finite/infinite far-plane, box and absorption values")
