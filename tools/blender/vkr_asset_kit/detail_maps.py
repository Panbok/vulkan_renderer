"""Tileable detail maps for baked texture sets (ADR-136), numpy only.

A texture set's own maps hold about 1 px per millimetre at best; a detail
map repeats a few centimetres of surface relief over the same UVs so close
views keep their grain. Each family is a 512 x 512 tileable height field
turned into a tangent normal map (+Y up) and a grey albedo overlay about
0.5; `FAMILY` maps a smart kind to its family, `TILE` gives the metres one
repeat covers. `plaster`, `concrete` and `carpet` serve level surfaces
(brush faces whose UVs count a material's `world_size`) rather than baked
sets.
"""
import os
import struct
import sys
import zlib

import numpy as np

SIZE = 512

FAMILY = {"paint": "fine", "plastic": "fine", "laminate": "fine", "label": "fine", "glass": None,
          "emissive": None, "metal": "metal", "wood": "wood", "fabric": "fabric", "rubber": "rubber",
          "cardboard": "cardboard", "organic": "fine"}

# Metres one repeat covers, and the detail normal's strength.
TILE = {"fine": 0.04, "metal": 0.06, "wood": 0.05, "fabric": 0.02, "rubber": 0.03, "cardboard": 0.05,
        "plaster": 0.25, "concrete": 0.3, "carpet": 0.06}
STRENGTH = {"fine": 0.3, "metal": 0.5, "wood": 0.6, "fabric": 0.9, "rubber": 0.6, "cardboard": 0.7,
            "plaster": 0.6, "concrete": 0.8, "carpet": 1.0}
ALBEDO_STRENGTH = {"fine": 0.08, "metal": 0.3, "wood": 0.5, "fabric": 0.5, "rubber": 0.3, "cardboard": 0.6,
                   "plaster": 0.25, "concrete": 0.45, "carpet": 0.6}


def _band_noise(rng, low, high, size=SIZE):
    """Tileable noise with spatial frequencies between `low` and `high`
    cycles per tile (an FFT band-pass of white noise)."""
    white = rng.normal(size=(size, size))
    f = np.fft.fftfreq(size) * size
    fx, fy = np.meshgrid(f, f)
    radius = np.sqrt(fx * fx + fy * fy)
    band = (radius >= low) & (radius <= high)
    field = np.real(np.fft.ifft2(np.fft.fft2(white) * band))
    return (field - field.mean()) / (field.std() + 1e-9)


def _fbm_noise(rng, low, high, exponent=1.0, size=SIZE):
    """Tileable noise over a wide band of frequencies, amplitude falling
    as 1/f^exponent (natural surfaces), without a narrow band's worms."""
    white = rng.normal(size=(size, size))
    f = np.fft.fftfreq(size) * size
    fx, fy = np.meshgrid(f, f)
    radius = np.sqrt(fx * fx + fy * fy)
    band = (radius >= low) & (radius <= high)
    weight = np.where(band, 1.0 / np.maximum(radius, 1.0) ** exponent, 0.0)
    field = np.real(np.fft.ifft2(np.fft.fft2(white) * weight))
    return (field - field.mean()) / (field.std() + 1e-9)


def _pits(rng, count, radius_min, radius_max, size=SIZE):
    """Tileable round dents (0..1 depth), each a smooth bowl."""
    canvas = np.zeros((size, size))
    y, x = np.mgrid[0:size, 0:size]
    for _ in range(count):
        cx, cy = rng.uniform(0, size, 2)
        r = rng.uniform(radius_min, radius_max)
        dx = (x - cx + size / 2) % size - size / 2
        dy = (y - cy + size / 2) % size - size / 2
        d2 = (dx * dx + dy * dy) / (r * r)
        canvas = np.maximum(canvas, np.clip(1.0 - d2, 0.0, 1.0) ** 0.7 * rng.uniform(0.5, 1.0))
    return canvas


def _scratches(rng, count, length, size=SIZE):
    """Tileable thin straight scratches in random directions."""
    canvas = np.zeros((size, size))
    for _ in range(count):
        x, y = rng.uniform(0, size, 2)
        angle = rng.uniform(0, np.pi)
        n = int(length * rng.uniform(0.4, 1.0))
        for k in range(n):
            px = int(x + np.cos(angle) * k) % size
            py = int(y + np.sin(angle) * k) % size
            canvas[py, px] = max(canvas[py, px], rng.uniform(0.4, 1.0))
    return canvas


def height_field(family, seed=7):
    """The family's tileable height field, about -1..1."""
    rng = np.random.default_rng(seed)
    if family == "fine":
        # Orange peel and a little grain.
        return 0.8 * _band_noise(rng, 8, 40) + 0.3 * _band_noise(rng, 60, 160)
    if family == "metal":
        h = 0.4 * _band_noise(rng, 30, 120) - 1.5 * _scratches(rng, 140, 180)
        return h
    if family == "wood":
        pores = _band_noise(rng, 80, 200)
        return 0.6 * _band_noise(rng, 4, 16) - 0.8 * np.clip(pores - 1.2, 0, None)
    if family == "fabric":
        # A plain weave: crossing threads, over and under.
        t = np.arange(SIZE) / SIZE * 2 * np.pi * 32
        x, y = np.meshgrid(t, t)
        warp = np.cos(y) * (np.sign(np.sin(x / 2 + np.pi / 4)))
        weft = np.cos(x) * (np.sign(np.sin(y / 2 + np.pi / 4)))
        return 0.6 * (np.maximum(warp, weft)) + 0.25 * _band_noise(rng, 60, 200)
    if family == "rubber":
        return 0.9 * _band_noise(rng, 40, 120)
    if family == "cardboard":
        fibres = _band_noise(rng, 20, 90)
        return 0.7 * fibres + 0.3 * _band_noise(rng, 2, 6)
    if family == "plaster":
        # Trowel undulation, sand in the skim coat and a few pinholes.
        trowel = _fbm_noise(rng, 1, 12, 1.6)
        sand = _fbm_noise(rng, 80, 256, 0.5)
        return 0.8 * trowel + 0.2 * sand - 0.8 * _pits(rng, 40, 1.0, 2.0)
    if family == "concrete":
        # Aggregate and sand under the surface, and bugholes: small round
        # voids left by air against the formwork.
        surface = _fbm_noise(rng, 2, 256, 1.1)
        sand = _fbm_noise(rng, 100, 256, 0.3)
        return 0.6 * surface + 0.25 * sand - 1.6 * _pits(rng, 60, 1.5, 5.0)
    if family == "carpet":
        # Loop pile: round tufts in slightly wavy rows, each its own height.
        rows, cols = 64, 64
        y, x = np.mgrid[0:SIZE, 0:SIZE] / SIZE
        wave = 0.15 * _fbm_noise(rng, 1, 4, 1.0)
        u = x * cols + wave
        v = y * rows + 0.5 * (np.floor(u) % 2)
        heights = rng.uniform(0.4, 1.0, (rows, cols))
        shift = rng.uniform(-0.18, 0.18, (rows, cols, 2))
        iv = np.floor(v).astype(int) % rows
        iu = np.floor(u).astype(int) % cols
        fu = u - np.floor(u) - 0.5 - shift[iv, iu, 0]
        fv = v - np.floor(v) - 0.5 - shift[iv, iu, 1]
        tuft = np.clip(1.0 - 3.2 * (fu * fu + fv * fv), 0.0, 1.0) * heights[iv, iu]
        return 0.9 * tuft + 0.25 * _fbm_noise(rng, 2, 16, 1.0)
    raise ValueError(f"unknown detail family {family}")


def normal_and_overlay(family, seed=7):
    """(normal RGB 0..1, overlay grey 0..1) of a family, both HxWx3, rows
    bottom-up as Blender images keep them; +Y of the normal is up the
    image."""
    h = height_field(family, seed)
    h = h / (np.abs(h).max() + 1e-9)
    # Wrapped central differences; one tile spans SIZE texels.
    dx = (np.roll(h, -1, axis=1) - np.roll(h, 1, axis=1)) * 0.5
    dy = (np.roll(h, -1, axis=0) - np.roll(h, 1, axis=0)) * 0.5
    slope = 6.0
    n = np.stack([-dx * slope, -dy * slope, np.ones_like(h)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    overlay = np.clip(0.5 + 0.25 * h, 0.0, 1.0)
    return n * 0.5 + 0.5, np.repeat(overlay[..., None], 3, -1)


def write_png(path, rgb):
    """An 8-bit RGB PNG of an HxWx3 image 0..1 whose first row is the top."""
    data = (np.clip(rgb, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    height, width, _ = data.shape
    raw = b"".join(b"\x00" + data[y].tobytes() for y in range(height))

    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def main(argv):
    """detail_maps.py <project> <family>...: writes each family's
    <family>_normal.png and <family>_overlay_mask.png into the project's
    assets/textures/vkr_detail (run with a Python that has numpy, such as
    Blender's; cook them with `vkr_bakery cook` afterwards)."""
    if len(argv) < 2:
        raise SystemExit(main.__doc__)
    folder = os.path.join(argv[0], "assets", "textures", "vkr_detail")
    os.makedirs(folder, exist_ok=True)
    for family in argv[1:]:
        normal, overlay = normal_and_overlay(family)
        # Rows are bottom-up here; a PNG's first row is the top.
        write_png(os.path.join(folder, f"{family}_normal.png"), normal[::-1])
        write_png(os.path.join(folder, f"{family}_overlay_mask.png"), overlay[::-1])
        print("wrote", family)


if __name__ == "__main__":
    main(sys.argv[1:])
