#!/usr/bin/env python3
"""Check isolated native sheen area-light output against independent quadrature.

Capture sheen_rectangle_oracle_local.case.json with the BRDF display profile.
The black metallic substrate isolates sheen. Its central front-face sample has
known geometry; integration uses the analytic Charlie model, not LTC matrices.
"""
from pathlib import Path
import json
import math
import re
import struct
import sys
from check_clearcoat_fixture import capture_path, half_rgba, oct_decode

run = Path(sys.argv[1])
item, path = capture_path(run, 'hdr_pre_transmission')
assert (item['width'], item['height']) == (65, 65)
measured = half_rgba(path, 65, 65)[32 * 65 + 32][:3]
_, path = capture_path(run, 'gbuffer_normal')
normal = oct_decode(path.read_bytes(), 32 * 65 + 32)
assert normal[2] > .9999, normal
# The center camera ray intersects the cube's z=.06 front at x=.36.
view_x, view_z = 6 / math.sqrt(37), 1 / math.sqrt(37)
# Evaluate the same resolved normal the shader consumed; octahedral storage
# slightly tilts an authored +Z normal after quantization.
no_v = normal[0] * view_x + normal[2] * view_z
roughness = round(.6 * 255) / 255  # Resolve's RGBA8 material contract.
alpha = roughness * roughness
# Only the common per-roughness normalization comes from the cooked payload.
source = Path('renderer/src/vkr_sheen_lut_data.inc').read_text()
tables = source.split('#elif defined(VKR_SHEEN_LUT_EMIT_LTC)')[-1]
if tables == source:
    tables = source.split('#if defined(VKR_SHEEN_LUT_EMIT_LTC)')[-1]
assert tables != source, 'unrecognized generated table section'
bits = [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{4})', tables)]
assert len(bits) == 4 * 64 * 64 * 4, len(bits)
def half(i):
    return struct.unpack('<e', struct.pack('<H', bits[i]))[0]
x = (roughness - .04) / .96 * 63
lo = int(x)
normalization = half(64 * 64 * 4 + lo * 4 + 3) * (1 - x + lo) + half(64 * 64 * 4 + (lo + 1) * 4 + 3) * (x - lo)
t = (1 - alpha) ** 2
a, b, c, d, e = [u + (v-u)*t for u,v in [(21.5473,25.3245),(3.82987,3.32435),(.19823,.16801),(-1.97760,-1.27393),(-4.32054,-4.85967)]]
def lam(z):
    def ell(v):
        return a/(1+b*max(v,1e-8)**c)+d*v+e
    return math.exp(ell(z) if z < .5 else 2*ell(.5)-ell(1-z))
lambda_v = lam(max(no_v, .0001))
def integrate(n):
    total = 0.
    for iy in range(n):
        y = -1 + (iy + .5) * 2/n
        for ix in range(n):
            x = -2 + (ix + .5) * 4/n - .36
            z = 1.94
            distance2 = x*x+y*y+z*z
            inverse_distance = 1/math.sqrt(distance2)
            lx,ly,lz = x*inverse_distance,y*inverse_distance,z*inverse_distance
            no_l = normal[0]*lx + normal[1]*ly + normal[2]*lz
            if no_l <= 0:
                continue
            hx,hy,hz = view_x+lx,ly,view_z+lz
            no_h = (normal[0]*hx+normal[1]*hy+normal[2]*hz)/math.sqrt(hx*hx+hy*hy+hz*hz)
            distribution = (2+1/alpha)*max(0,1-no_h*no_h)**(.5/alpha)/(2*math.pi)
            visibility = 1/(4*max(no_v,.0001)*max(no_l,.0001)*(1+lambda_v+lam(max(no_l,.0001))))
            total += normalization*distribution*visibility*no_l*lz/distance2
    return total*8/(n*n)
coarse, reference = integrate(128), integrate(256)
assert abs(reference-coarse) <= max(1e-6, reference*.001), (coarse,reference)
error = max(abs(v-reference) for v in measured)
assert error <= max(.001, reference*.10), (measured,reference,error)
print(json.dumps(dict(status='pass',rgb=measured,resolved_normal=normal,reference=reference,absolute_error=error,relative_error=error/reference,quadrature_delta=abs(reference-coarse)),indent=2))
