#!/usr/bin/env python3
"""Check the fixed sheen witness after capturing local.sheen.layers.

Pack tests/fixtures/rendering/sheen with tools/pack_vkt_textures.sh first.
This checks visible roughness response, transmitted background and retained coat
SSR. Channel decoding and numerical energy use separate CPU/furnace checks.
"""
import json
from pathlib import Path
import sys
from check_clearcoat_fixture import (
    SSR_FLOOR_Y_MIN, capture_path, half_rgba, mean_patch, rgb_distance,
)

run = Path(sys.argv[1])
item, path = capture_path(run, 'hdr_pre_transmission')
width, height = item['width'], item['height']
assert (width, height) == (512, 384)
pixels = half_rgba(path, width, height)
panels = {name: mean_patch(pixels, width, height, (x, 110))
          for name, x in [('off', 104), ('smooth', 205), ('rough', 307), ('textured', 408)]}
assert all(max(rgb) > .01 for rgb in panels.values()), panels
assert rgb_distance(panels['smooth'], panels['rough']) > .002, panels
assert rgb_distance(panels['off'], panels['rough']) > .002, panels
item, path = capture_path(run, 'hdr_post_transmission')
post = half_rgba(path, width, height)
glass = mean_patch(post, width, height, (256, 212))
assert glass[1] > 1.1 * max(glass[0], glass[2]), glass
item, path = capture_path(run, 'ssr_raw')
raw = half_rgba(path, item['width'], item['height'])
floor_y_min = (SSR_FLOOR_Y_MIN * item['height'] + height - 1) // height
hits = [p for i, p in enumerate(raw) if i // item['width'] >= floor_y_min and p[3] > .01]
red = [p for p in hits if p[0] > 2 * max(p[1], p[2], 1e-5)]
assert len(hits) >= 8 and len(red) >= 4, (len(hits), len(red))
print(json.dumps({'status': 'pass', 'panels': panels, 'glass': glass,
                  'floor_ssr_hits': len(hits), 'red_hits': len(red)}, indent=2))
