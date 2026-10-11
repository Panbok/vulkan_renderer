#!/usr/bin/env python3
"""Crop a rectangle of a generator's float heights into a 16-bit PNG and print
the terrain.stamp arguments that write those samples back exactly (mode set).

Use it to undo a sculpt whose falloff reached too far, or to re-apply the
generator's ground under one area after edits elsewhere. The crop snaps to
the grid, so every stamped pixel lands on one terrain sample.

  restore_crop.py --heights main_h.f32 --n 1025 --origin X0 Z0 --step 2 \
      --range MIN MAX --rect X0 Z0 X1 Z1 --out /abs/restore.png \
      [--terrain NAME] [--terrain-y Y]

--origin is the world x, z of sample (0, 0) (terrain center - size / 2);
--range is the terrain's local height_min/height_max and --terrain-y its
entity's world y (default 0). The crop is a square from the rectangle's min
corner and restores heights only. The printed JSON is the terrain.stamp
call, with the image's absolute path.
"""
import argparse
import array
import json
import os
import struct
import sys
import zlib


def png16(path, width, height, rows):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 16, 0, 0, 0, 0)
    raw = b"".join(b"\x00" + row for row in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw)) +
                chunk(b"IEND", b""))


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--heights", required=True, help="float32 row-major N x N heights")
    p.add_argument("--n", type=int, required=True)
    p.add_argument("--origin", type=float, nargs=2, required=True, metavar=("X0", "Z0"))
    p.add_argument("--step", type=float, required=True)
    p.add_argument("--range", type=float, nargs=2, required=True, metavar=("MIN", "MAX"))
    p.add_argument("--rect", type=float, nargs=4, required=True, metavar=("X0", "Z0", "X1", "Z1"))
    p.add_argument("--out", required=True)
    p.add_argument("--terrain", default="TERRAIN")
    p.add_argument("--terrain-y", type=float, default=0.0)
    a = p.parse_args()

    h = array.array("f")
    with open(a.heights, "rb") as f:
        h.frombytes(f.read())
    if len(h) != a.n * a.n:
        sys.exit("%s holds %d floats, not %d x %d" % (a.heights, len(h), a.n, a.n))
    lo, hi = a.range
    if not hi > lo:
        sys.exit("--range MAX must exceed MIN")

    i0 = max(0, int(round((min(a.rect[0], a.rect[2]) - a.origin[0]) / a.step)))
    j0 = max(0, int(round((min(a.rect[1], a.rect[3]) - a.origin[1]) / a.step)))
    i1 = min(a.n - 1, int(round((max(a.rect[0], a.rect[2]) - a.origin[0]) / a.step)))
    j1 = min(a.n - 1, int(round((max(a.rect[1], a.rect[3]) - a.origin[1]) / a.step)))
    # terrain.stamp takes one square size: the crop is square, from the
    # rectangle's min corner.
    side = max(i1 - i0, j1 - j0) + 1
    if side < 2 or i0 + side > a.n or j0 + side > a.n:
        sys.exit("the rectangle is empty or its square crop leaves the grid")

    rows = []
    for j in range(j0, j0 + side):
        row = array.array("H")
        for i in range(i0, i0 + side):
            v = (h[j * a.n + i] - lo) / (hi - lo)
            row.append(max(0, min(65535, int(round(v * 65535)))))
        if sys.byteorder == "little":
            row.byteswap()
        rows.append(row.tobytes())
    out = os.path.abspath(a.out)
    png16(out, side, side, rows)

    size = (side - 1) * a.step
    x0 = a.origin[0] + i0 * a.step
    z0 = a.origin[1] + j0 * a.step
    print(json.dumps({"op": "terrain.stamp", "args": {
        "terrain": a.terrain, "image": out, "mode": "set", "size": size, "height_scale": hi - lo,
        "center": [x0 + size / 2, a.terrain_y + lo, z0 + size / 2],
        "label": "restore terrain %.0f..%.0f x %.0f..%.0f" % (x0, x0 + size, z0, z0 + size)}}))


if __name__ == "__main__":
    main()
