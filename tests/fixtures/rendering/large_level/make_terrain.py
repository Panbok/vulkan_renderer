#!/usr/bin/env python3
"""Writes the large-level fixture's terrains (docs/proposals/large-level-rendering.md).

A 2,048 m playable terrain at 8 m spacing around Bistro and a 16,384 m
backdrop at 64 m spacing, both from one height function: flat under the
town out to 190 m, rolling hills rising to about 120 m at the playable
edge, and mountains beyond. Layer 0 is grass, layer 1 rock on steep
ground. The files follow runtime/src/level/vkr_heightfield.h (version 2).

    python3 make_terrain.py [output directory]
"""

import math
import os
import struct
import sys

TILE = 64
OVERVIEW_STRIDE = 16
# Under the town the ground sits just below Bistro's street, which covers it.
TOWN_RADIUS = 190.0
TOWN_HEIGHT = -0.6


def hash2(ix, iz, seed):
    h = (ix * 374761393 + iz * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def value_noise(x, z, seed):
    ix, iz = math.floor(x), math.floor(z)
    fx, fz = x - ix, z - iz
    sx, sz = fx * fx * (3 - 2 * fx), fz * fz * (3 - 2 * fz)
    a = hash2(ix, iz, seed)
    b = hash2(ix + 1, iz, seed)
    c = hash2(ix, iz + 1, seed)
    d = hash2(ix + 1, iz + 1, seed)
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sz


def fbm(x, z, seed, octaves):
    total, amplitude, frequency = 0.0, 0.5, 1.0
    for octave in range(octaves):
        total += amplitude * value_noise(x * frequency, z * frequency,
                                         seed + octave)
        amplitude *= 0.5
        frequency *= 2.0
    return total


def smoothstep(edge0, edge1, x):
    t = min(max((x - edge0) / (edge1 - edge0), 0.0), 1.0)
    return t * t * (3 - 2 * t)


def world_height(x, z):
    r = math.hypot(x, z)
    hills = 22.0 + 0.09 * (r - TOWN_RADIUS) + 60.0 * fbm(x / 400.0, z / 400.0,
                                                         7, 4)
    height = TOWN_HEIGHT + smoothstep(TOWN_RADIUS, 450.0, r) * (hills -
                                                                TOWN_HEIGHT)
    ridges = 1.0 - abs(2.0 * fbm(x / 2500.0, z / 2500.0, 19, 5) - 1.0)
    return height + smoothstep(1400.0, 5000.0, r) * 1100.0 * ridges * ridges


def write_field(path, size, spacing, height_min, height_max, height_of):
    cells = int(size / spacing)
    samples = cells + 1
    tiles = (samples + TILE - 1) // TILE
    half = size / 2.0
    heights = [[0.0] * samples for _ in range(samples)]
    for z in range(samples):
        for x in range(samples):
            heights[z][x] = height_of(-half + x * spacing, -half + z * spacing)

    def quantize(h):
        t = (h - height_min) / (height_max - height_min)
        return int(round(min(max(t, 0.0), 1.0) * 65535.0))

    def weights(x, z):
        x0, x1 = max(x - 1, 0), min(x + 1, samples - 1)
        z0, z1 = max(z - 1, 0), min(z + 1, samples - 1)
        dx = (heights[z][x1] - heights[z][x0]) / ((x1 - x0) * spacing)
        dz = (heights[z1][x] - heights[z0][x]) / ((z1 - z0) * spacing)
        rock = int(round(smoothstep(0.45, 0.9, math.hypot(dx, dz)) * 255.0))
        return (255 - rock) | (rock << 8)

    with open(path, "wb") as out:
        out.write(b"VKRHFLD1")
        out.write(struct.pack("<IIfffIIIII", 2, cells, spacing, height_min,
                              height_max, TILE, tiles, OVERVIEW_STRIDE, 0, 0))
        for tz in range(tiles):
            for tx in range(tiles):
                tile_heights, tile_weights = [], []
                for z in range(tz * TILE, tz * TILE + TILE):
                    for x in range(tx * TILE, tx * TILE + TILE):
                        inside = x < samples and z < samples
                        tile_heights.append(quantize(heights[z][x])
                                            if inside else 0)
                        tile_weights.append(weights(x, z) if inside else 0xFF)
                out.write(struct.pack("<%dH" % len(tile_heights),
                                      *tile_heights))
                out.write(struct.pack("<%dI" % len(tile_weights),
                                      *tile_weights))
        side = cells // OVERVIEW_STRIDE + 1
        points = [(x * OVERVIEW_STRIDE, z * OVERVIEW_STRIDE)
                  for z in range(side) for x in range(side)]
        out.write(struct.pack("<%dH" % len(points),
                              *[quantize(heights[z][x]) for x, z in points]))
        out.write(struct.pack("<%dI" % len(points),
                              *[weights(x, z) for x, z in points]))


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(
        os.path.abspath(__file__))
    write_field(os.path.join(directory, "ll_terrain.vkrhf"), 2048.0, 8.0,
                -20.0, 400.0, world_height)
    # The backdrop lies 10 m under the playable ground inside its footprint,
    # where it draws as holes, and meets it at the border.
    def backdrop_height(x, z):
        inside = max(abs(x), abs(z)) < 1024.0 - 64.0
        return world_height(x, z) - (10.0 if inside else 0.0)

    write_field(os.path.join(directory, "ll_backdrop.vkrhf"), 16384.0, 64.0,
                -50.0, 1600.0, backdrop_height)


if __name__ == "__main__":
    main()
