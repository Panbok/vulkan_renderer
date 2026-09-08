#!/usr/bin/env python3
"""Check thin-sheet native split, tint, shadows, area response and coverage.

Argument: JSON mapping back/front/off/black/shadow/rectangle/sun/cutout to
snapshot directories. Run the corresponding diffuse_sheet_*_local cases first.
The zero sheet measures the unchanged ambient term. Lambert and rectangle area
references are independent of shader helpers and LTC tables.
"""
from pathlib import Path
import json
import math
import struct
import sys
from check_clearcoat_fixture import capture_path, half_rgba, oct_decode

runs = {key: Path(value) for key, value in json.loads(Path(sys.argv[1]).read_text()).items()}
values = {}
for name in ('back', 'front', 'off', 'black', 'shadow', 'rectangle', 'sun', 'cutout'):
    item, path = capture_path(runs[name], 'hdr_pre_transmission')
    assert (item['width'], item['height']) == (129, 129)
    pixels = half_rgba(path, 129, 129)
    assert all(math.isfinite(v) for pixel in pixels for v in pixel), name
    values[name] = pixels[64*129+64][:3]
_, path = capture_path(runs['back'], 'gbuffer_normal')
normal = oct_decode(path.read_bytes(), 64*129+64)
assert normal == (0., 0., 1.) or normal == [0., 0., 1.], normal
ambient = [v*.25 for v in values['off']]
back = [v-a for v, a in zip(values['back'], ambient)]
front = [v-a for v, a in zip(values['front'], ambient)]
# Scene-authored point lights omit kind/attenuation, selecting the established
# legacy defaults: 1/(1 + .35*d + .44*d*d). Their center distance is two.
radiance = 8/(1+.35*2+.44*4)
expected_back = [radiance*.75*t/math.pi for t in (1,.5,.25)]
expected_front = radiance*.25/math.pi
errors = [abs(a-b) for a,b in zip(back, expected_back)]
errors += [abs(v-expected_front) for v in front]
assert max(errors) < .0007, (back, front, expected_back, expected_front)
assert max(abs(v) for v in values['black']) == 0, values['black']
assert max(abs(v-a) for v,a in zip(values['shadow'],ambient)) < .0002, values['shadow']

def rectangle_reference(count):
    total = 0.
    for y in range(count):
        py = -1+(y+.5)*2/count
        for x in range(count):
            px = -2+(x+.5)*4/count
            distance2 = px*px+py*py+4
            total += 4/(distance2*distance2)
    return total*8/(count*count)*.75/math.pi
coarse, rectangle = rectangle_reference(128), rectangle_reference(256)
assert abs(coarse-rectangle) < 1e-5, (coarse, rectangle)
rectangle_error = max(abs(v-a-rectangle*t) for v,a,t in zip(values['rectangle'],ambient,(1,.5,.25)))
assert rectangle_error < .0007, (values['rectangle'], rectangle, rectangle_error)
sun_error = max(abs(v-a-.75*t/math.pi) for v,a,t in zip(values['sun'],ambient,(1,.5,.25)))
assert sun_error < .0007, (values['sun'], sun_error)
item, path = capture_path(runs['cutout'], 'visibility_ids')
ids = list(struct.unpack('<'+'I'*(129*129), path.read_bytes()))
assert ids[64*129+24] == 0 and ids[64*129+104] > 0
assert max(abs(a-b) for a,b in zip(values['cutout'],values['back'])) < .0007
print(json.dumps(dict(status='pass',center_rgb=values,point_max_abs=max(errors),
    rectangle_reference=rectangle,rectangle_max_abs=rectangle_error,
    rectangle_quadrature_delta=abs(coarse-rectangle),sun_max_abs=sun_error,
    shadow='backlight blocked',cutout='left uncovered, right covered'),indent=2))
