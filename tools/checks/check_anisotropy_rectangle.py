#!/usr/bin/env python3
"""Characterize the native anisotropic rectangle fixture against GGX quadrature.

Usage: python3 tools/checks/check_anisotropy_rectangle.py <snapshot-directory>

This reads captured HDR and normal data. Axis, strength and roughness are
inferred from the fixture because it has no anisotropy capture channel. The
reported envelope includes adjacent UNorm8 codes for those inferred values.
The analytic GGX reference uses only production directional DFG coefficients.
A separate integration of the decoded LTC density checks native polygon
integration independently of BRDF fit error. Both references use area quadrature;
neither reproduces the GPU spherical polygon integral. No universal LTC error
threshold is asserted.
"""
from array import array
import hashlib
import itertools
import json
import math
from pathlib import Path
import re
import struct
import sys
from check_clearcoat_fixture import capture_path, half_rgba, oct_decode


def dot(a, b):
    return sum(x*y for x, y in zip(a, b))


def unit(v):
    length = math.sqrt(dot(v, v))
    return tuple(x/length for x in v)


def cross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])


def projected_axis(v, normal):
    amount = dot(v, normal)
    return unit(tuple(v[i]-amount*normal[i] for i in range(3)))


def oct_encode(v):
    scale = sum(abs(x) for x in v)
    x, y, z = (a/scale for a in v)
    if z < 0:
        x, y = (1-abs(y))*(1 if x >= 0 else -1), (1-abs(x))*(1 if y >= 0 else -1)
    return ((x+1)*.5, (y+1)*.5)


def axis_from_codes(x, y, normal):
    x, y = x/255*2-1, y/255*2-1
    z = 1-abs(x)-abs(y)
    if z < 0:
        x, y = (1-abs(y))*(1 if x >= 0 else -1), (1-abs(x))*(1 if y >= 0 else -1)
    return projected_axis((x, y, z), normal)


def dfg_energy_table():
    source = Path('renderer/src/vkr_anisotropy_lut_data.inc').read_bytes()
    table_words = 64*64*64*4
    energy = array('f')
    first = None
    payload = array('f')
    for index, match in enumerate(re.finditer(rb'0x([0-9a-fA-F]{4})', source)):
        value = struct.unpack('<e', struct.pack('<H', int(match[1], 16)))[0]
        payload.append(value)
        if index >= 2*table_words and index % 4 >= 2:
            if index % 4 == 2:
                first = value
            else:
                energy.append(first+value)
    assert index+1 == 3*table_words and len(energy) == 64*64*64
    return energy, hashlib.sha256(source).hexdigest(), payload


def dfg_sample(table, normal, view, axis, roughness, strength):
    bitangent = cross(normal, axis)
    phi = math.atan2(abs(dot(view, bitangent)), abs(dot(view, axis)))
    coordinates = ((roughness-.04)/.96*63,
                   (1-math.sqrt(max(.0001, min(1, dot(normal, view)))))/.99*63,
                   strength*7, phi/(math.pi/2)*7)
    lower = [int(v) for v in coordinates]
    upper = [min(v+1, 63 if i < 2 else 7) for i, v in enumerate(lower)]
    fraction = [v-i for v, i in zip(coordinates, lower)]
    result = 0.
    for corner in itertools.product((0, 1), repeat=4):
        indices = [upper[i] if corner[i] else lower[i] for i in range(4)]
        weight = math.prod(fraction[i] if corner[i] else 1-fraction[i] for i in range(4))
        x, y, s, p = indices
        result += weight*table[((s*8+p)*64+y)*64+x]
    return result


def integrate(normal, view, axis, roughness, strength, energy, samples):
    bitangent = cross(normal, axis)
    alpha_b = roughness*roughness
    alpha_t = alpha_b+(1-alpha_b)*strength*strength
    no_v = dot(normal, view)
    root_v = math.sqrt((alpha_t*dot(axis, view))**2+
                       (alpha_b*dot(bitangent, view))**2+no_v*no_v)
    total = 0.
    for iy in range(samples):
        y = -1+(iy+.5)*2/samples
        for ix in range(samples):
            # Center camera ray intersects the front z=.06 at x=.36.
            x, z = -2+(ix+.5)*4/samples-.36, 1.94
            distance2 = x*x+y*y+z*z
            inverse = 1/math.sqrt(distance2)
            light = (x*inverse, y*inverse, z*inverse)
            no_l = dot(normal, light)
            if no_l <= 0:
                continue
            half_vector = unit(tuple(a+b for a, b in zip(view, light)))
            denominator = (dot(axis, half_vector)/alpha_t)**2 + (dot(bitangent, half_vector)/alpha_b)**2 + dot(normal, half_vector)**2
            distribution = 1/(math.pi*alpha_t*alpha_b*denominator*denominator)
            root_l = math.sqrt((alpha_t*dot(axis, light))**2+
                               (alpha_b*dot(bitangent, light))**2+no_l*no_l)
            visibility = .5/(no_l*root_v+no_v*root_l)
            # White F0/F90 gives Fresnel one and compensation 1/(DFG_A+B).
            total += distribution*visibility*no_l*light[2]/distance2/energy
    return total*8/(samples*samples)


def ltc_record(payload, normal, view, axis, roughness, strength):
    phi = math.atan2(abs(dot(view, cross(normal, axis))), abs(dot(view, axis)))
    coordinates = ((roughness-.04)/.96*63,
                   (1-math.sqrt(max(.0001, min(1, dot(normal, view)))))/.99*63,
                   strength*7, phi/(math.pi/2)*7)
    lower = [int(v) for v in coordinates]
    upper = [min(v+1, 63 if i < 2 else 7) for i, v in enumerate(lower)]
    fraction = [v-i for v, i in zip(coordinates, lower)]
    result = [0.]*12
    for corner in itertools.product((0, 1), repeat=4):
        x, y, strength_index, phi_index = [upper[i] if corner[i] else lower[i] for i in range(4)]
        weight = math.prod(fraction[i] if corner[i] else 1-fraction[i] for i in range(4))
        offset = (((strength_index*8+phi_index)*64+y)*64+x)*4
        for channel in range(12):
            result[channel] += weight*payload[channel//4*64*64*64*4+offset+channel%4]
    return result


def integrate_ltc(payload, normal, view, axis, roughness, strength, energy, samples):
    record = ltc_record(payload, normal, view, axis, roughness, strength)
    a, b = 2**record[0], 2**record[1]
    h, j, k = record[3:6]
    u = unit(record[6:9])
    plane = unit((h*k-j*b, -k*a, a*b))
    inverse = -1/(1+plane[2])
    plane_xy = plane[0]*plane[1]*inverse
    plane_x = (1+plane[0]**2*inverse, plane_xy, -plane[0])
    plane_y = (plane_xy, 1+plane[1]**2*inverse, -plane[1])
    q = tuple(plane_x[i]*u[0]+plane_y[i]*u[1]+plane[i]*u[2] for i in range(3))
    amplitude = (record[2]+record[9])/(energy*.5*(1+u[2]))
    bitangent = cross(normal, axis)
    signs = (-1 if dot(view, axis) < 0 else 1, -1 if dot(view, bitangent) < 0 else 1)
    total = 0.
    for iy in range(samples):
        y = -1+(iy+.5)*2/samples
        for ix in range(samples):
            x, z = -2+(ix+.5)*4/samples-.36, 1.94
            distance2 = x*x+y*y+z*z
            inverse_distance = 1/math.sqrt(distance2)
            light = (x*inverse_distance, y*inverse_distance, z*inverse_distance)
            no_l = dot(normal, light)
            if no_l <= 0:
                continue
            local_x, local_y = dot(axis, light)*signs[0], dot(bitangent, light)*signs[1]
            transformed = (a*local_x, h*local_x+b*local_y, j*local_x+k*local_y+no_l)
            length2 = dot(transformed, transformed)
            # Q is orthogonal: evaluate the transformed cosine density using
            # its support vector and C norm, without reproducing shader Q rows
            # or the spherical polygon integration used by either backend.
            density = a*b*max(dot(q, transformed), 0)/(math.pi*length2*length2)
            total += amplitude*density*light[2]/distance2
    return total*8/(samples*samples)


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    run = Path(sys.argv[1])
    item, path = capture_path(run, 'hdr_pre_transmission')
    assert (item['width'], item['height']) == (129, 129)
    pixel = 64*129+64
    measured = half_rgba(path, 129, 129)[pixel][:3]
    _, normal_path = capture_path(run, 'gbuffer_normal')
    normal = oct_decode(normal_path.read_bytes(), pixel)
    assert normal[2] > .9999, normal
    view = unit((6, 0, 1))
    # The procedural cube front has tangent +X and generated handedness -1.
    # Its enabled default normal map is RGB(128,128,255), so decoded map Y
    # and negative bitangent give a positive world Y perturbation. Construct
    # the axis against that pre-storage mapped normal, then quantize its oct
    # coordinates; deferred lighting projects it against the captured normal.
    pre_storage_normal = (1/255, 1/255, math.sqrt(1-2/(255*255)))
    unquantized_axis = projected_axis((math.cos(.4), math.sin(.4), 0), pre_storage_normal)
    encoded_axis = oct_encode(unquantized_axis)
    values = (*encoded_axis, .45, .7)
    nearest = tuple(round(v*255) for v in values)
    table, table_sha, payload = dfg_energy_table()
    def reference(codes, count):
        x, y, r, s = codes
        axis = axis_from_codes(x, y, normal)
        roughness, strength = r/255, s/255
        energy = dfg_sample(table, normal, view, axis, roughness, strength)
        return integrate(normal, view, axis, roughness, strength, energy, count)
    coarse, fine = reference(nearest, 128), reference(nearest, 256)
    assert abs(fine-coarse) <= max(1e-6, abs(fine)*.0001), (coarse, fine)
    adjacent = [tuple(sorted({math.floor(v*255), math.ceil(v*255)})) for v in values]
    envelope = [reference(codes, 256) for codes in itertools.product(*adjacent)]
    def fitted(codes, count):
        x, y, r, strength_code = codes
        axis = axis_from_codes(x, y, normal)
        roughness, strength = r/255, strength_code/255
        energy = dfg_sample(table, normal, view, axis, roughness, strength)
        return integrate_ltc(payload, normal, view, axis, roughness, strength, energy, count)
    fitted_coarse, fitted_fine = fitted(nearest, 128), fitted(nearest, 256)
    assert abs(fitted_fine-fitted_coarse) <= max(1e-6, abs(fitted_fine)*.0001)
    fitted_envelope = [fitted(codes, 256) for codes in itertools.product(*adjacent)]
    low, high = min(envelope), max(envelope)
    error = max(abs(value-fine) for value in measured)
    print(json.dumps(dict(
        status='characterized', measured_rgb=measured, resolved_normal=normal,
        inferred_axis=axis_from_codes(*nearest[:2], normal),
        inferred_roughness=nearest[2]/255, inferred_strength=nearest[3]/255,
        inferred_unorm8_codes=dict(axis_rg=nearest[:2], roughness=nearest[2], strength=nearest[3]),
        reference=fine, absolute_error=error, relative_error=error/fine,
        quadrature_delta=abs(fine-coarse),
        adjacent_unorm_reference_range=(low, high),
        adjacent_unorm_absolute_error_range=(max(0, low-max(measured), min(measured)-high),
                                            max(abs(value-ref) for value in measured for ref in (low, high))),
        inference_limit='Axis, roughness and strength inferred from authored fixture and UNorm8 storage; no captured anisotropy payload.',
        cpu_ltc_reference=fitted_fine,
        cpu_ltc_absolute_error=max(abs(value-fitted_fine) for value in measured),
        cpu_ltc_relative_error=max(abs(value-fitted_fine) for value in measured)/fitted_fine,
        cpu_ltc_quadrature_delta=abs(fitted_fine-fitted_coarse),
        cpu_ltc_adjacent_unorm_range=(min(fitted_envelope), max(fitted_envelope)),
        pre_storage_normal_inference=pre_storage_normal,
        lut_source_sha256=table_sha,
        acceptance_limit='No universal LTC error threshold asserted.'), indent=2))


if __name__ == '__main__':
    main()
