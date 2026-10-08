#!/usr/bin/env python3
"""Writes the engine's greybox looks (docs/proposals/artist-toolkit.md, part 1).

Each surface tag and mark gets one material per face orientation under
assets/materials/greybox/, all sampling the metric grid texture that
assets/textures/greybox/ holds: 25 cm lines, 1 m lines and a 4 m border, one
repeat per VKR_SURFACE_GREYBOX_REPEAT meters (runtime/src/level/vkr_surface.h).
The tag and mark lists must match vkr_surface.c; tests/src/surface_test.c
checks that every look it names exists.

Run from the repository root: python3 tools/gen_greybox.py
"""

import os
import struct
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MATERIALS = os.path.join(ROOT, "assets", "materials", "greybox")
TEXTURES = os.path.join(ROOT, "assets", "textures", "greybox")

# Pixels per repeat; the repeat covers 4 m, so 128 px per meter.
SIZE = 512
PX_PER_M = SIZE // 4

# Linear base colour factor, roughness, metallic and emissive of each look.
TAGS = [
    ("none", (0.80, 0.80, 0.80), 0.85, 0.0, None),
    ("concrete", (0.56, 0.56, 0.58), 0.95, 0.0, None),
    ("metal", (0.46, 0.50, 0.56), 0.45, 0.85, None),
    ("wood", (0.50, 0.34, 0.20), 0.80, 0.0, None),
    ("tile", (0.84, 0.85, 0.87), 0.45, 0.0, None),
    ("plaster", (0.86, 0.82, 0.74), 0.90, 0.0, None),
    ("brick", (0.62, 0.30, 0.22), 0.90, 0.0, None),
    ("rock", (0.46, 0.44, 0.41), 0.90, 0.0, None),
    ("dirt", (0.44, 0.34, 0.24), 0.95, 0.0, None),
    ("grass", (0.32, 0.50, 0.22), 0.90, 0.0, None),
    ("glass", (0.58, 0.78, 0.84), 0.15, 0.0, None),
    ("fabric", (0.56, 0.34, 0.56), 0.95, 0.0, None),
    ("water", (0.22, 0.42, 0.62), 0.20, 0.0, None),
    ("emissive", (1.00, 0.98, 0.92), 0.50, 0.0, (6.0, 5.9, 5.5)),
]

MARKS = [
    ("mark_hazard", (1.0, 1.0, 1.0), 0.60, "greybox_hazard.png"),
    ("mark_orange", (1.00, 0.50, 0.16), 0.85, None),
    ("mark_blue", (0.30, 0.55, 1.00), 0.85, None),
    ("mark_red", (0.72, 0.12, 0.10), 0.75, None),
    ("mark_green", (0.22, 0.52, 0.26), 0.75, None),
    ("mark_dark", (0.13, 0.13, 0.14), 0.70, None),
]

ROLES = [
    ("role_clip", (0.62, 0.30, 1.00), 0.85),
    ("role_trigger", (1.00, 0.62, 0.10), 0.85),
]

# Floors keep the tone, walls and ceilings darken it, so the three read
# apart without light.
ORIENTATIONS = [("floor", 1.0), ("wall", 0.82), ("ceiling", 0.64)]


def write_png(path, width, height, rows):
    """Writes 8-bit RGB rows as a PNG."""
    raw = b"".join(b"\x00" + bytes(row) for row in rows)

    def chunk(kind, data):
        body = kind + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    with open(path, "wb") as out:
        out.write(b"\x89PNG\r\n\x1a\n")
        out.write(chunk(b"IHDR", header))
        out.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        out.write(chunk(b"IEND", b""))


def grid_level(i):
    """Brightness added by the grid lines through pixel column or row i."""
    border = 2
    if i < border or i >= SIZE - border:
        return 92
    if i % PX_PER_M in (0, PX_PER_M - 1):
        return 52
    if i % (PX_PER_M // 4) == 0:
        return 22
    return 0


def grid_rows(base):
    """Rows of the metric grid over `base(x, y)`, an RGB triple."""
    levels = [grid_level(i) for i in range(SIZE)]
    rows = []
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            add = max(levels[x], levels[y])
            r, g, b = base(x, y)
            row += [min(255, r + add), min(255, g + add), min(255, b + add)]
        rows.append(row)
    return rows


def hazard_base(x, y):
    """Diagonal yellow and black stripes, 25 cm wide."""
    stripe = ((x + y) // (PX_PER_M // 4)) % 2
    return (232, 186, 24) if stripe == 0 else (36, 34, 30)


def material(name, color, roughness, metallic=0.0, emissive=None,
             texture="greybox_grid.png"):
    emissive = emissive or (0.0, 0.0, 0.0)
    lines = [
        f"name=greybox_{name}",
        "type=pbr",
        "base_color_colorspace=srgb",
        "base_color=%.3f,%.3f,%.3f,1.000000" % color,
        "metallic=%.6f" % metallic,
        "roughness=%.6f" % roughness,
        "emissive_factor=%.6f,%.6f,%.6f" % emissive,
        "alpha_mode=opaque",
        "double_sided=false",
        f"base_color_texture=./../../textures/greybox/{texture}"
        "?cs=srgb&tc=color_srgb",
    ]
    path = os.path.join(MATERIALS, name + ".mt")
    with open(path, "w", encoding="utf-8") as out:
        out.write("# Greybox look written by tools/gen_greybox.py\n")
        out.write("\n".join(lines) + "\n")


def main():
    os.makedirs(MATERIALS, exist_ok=True)
    os.makedirs(TEXTURES, exist_ok=True)
    write_png(os.path.join(TEXTURES, "greybox_grid.png"), SIZE, SIZE,
              grid_rows(lambda x, y: (124, 124, 124)))
    write_png(os.path.join(TEXTURES, "greybox_hazard.png"), SIZE, SIZE,
              grid_rows(hazard_base))
    for name, color, roughness, metallic, emissive in TAGS:
        for side, shade in ORIENTATIONS:
            material(f"{name}_{side}", tuple(c * shade for c in color),
                     roughness, metallic, emissive)
    for name, color, roughness, texture in MARKS:
        for side, shade in ORIENTATIONS:
            material(f"{name}_{side}", tuple(c * shade for c in color),
                     roughness, texture=texture or "greybox_grid.png")
    for name, color, roughness in ROLES:
        material(name, color, roughness)


if __name__ == "__main__":
    main()
