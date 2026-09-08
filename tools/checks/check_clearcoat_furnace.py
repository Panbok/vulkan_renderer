#!/usr/bin/env python3
"""Check unit-environment energy on coated opaque, blend and transmission rows.

Prepare after ./build_release.sh:
  ./build_release/tools/vkr_mesh_cooker --input tests/fixtures/rendering/clearcoat_furnace.gltf --output tests/fixtures/rendering/clearcoat_furnace.vkb
Capture tools/cases/local/clearcoat_furnace_local.case.json with
  tools/profiles/local-brdf-display-validation.json
Then: python3 tools/checks/check_clearcoat_furnace.py <snapshot-directory>
The fixture uses the existing assets/textures/brdf_white_local.hdr environment.
"""
import json
import math
from pathlib import Path
import struct
import sys

run = Path(sys.argv[1])
report = json.loads((run / 'report.json').read_text())
capture = next(x for x in report['captures'] if x['channel'] == 'hdr_pre_bloom')
width, height = capture['width'], capture['height']
pixels = list(struct.iter_unpack('<4e', (run / capture['data_path']).read_bytes()))
assert len(pixels) == width * height
assert all(math.isfinite(x) for pixel in pixels for x in pixel)
scene = json.loads(Path('tests/fixtures/rendering/clearcoat_furnace.gltf').read_text())
focal = height / (2 * math.tan(math.radians(45) / 2))
values = []
for node in scene['nodes']:
    x, y, z = node['translation']
    px = round(width / 2 + x * focal / (7 - z))
    py = round(height / 2 - y * focal / (7 - z))
    rgb = pixels[py * width + px][:3]
    error = max(abs(channel - 1) for channel in rgb)
    values.append({'material': node['name'], 'pixel': [px, py], 'rgb': rgb, 'error': error})
    assert error <= 0.001, values[-1]
assert len(values) == 36
print(json.dumps({'status': 'pass', 'samples': values, 'max_error': max(x['error'] for x in values)}, indent=2))
