"""Smart materials and surface details for baked game assets (numpy only).

The bake (bake.py) captures per texel: the surface's own albedo, roughness,
metallic and slot, its position and normal, an edge mask, local cavity and
occlusion. `compose` layers wear onto them per slot by a material *kind*
(painted metal chips to bare steel on convex edges, grime gathers in
cavities and near the floor, dust settles on faces that look up, smudges
and hairline scratches break the roughness), then stamps the asset's
details (screws, rivets, vents, perforations, seams, ribs, touch and wear
zones, grime, status lights) where the bake's position map puts them, and
returns the texture set's layers and a height map for the normal map.

No Blender import: the composer runs and tests outside Blender too.
"""
import numpy as np

KINDS = {
    # kind: wear (edge chips), wear colour, wear metal, wear rough, grime,
    #       dust, smudge (roughness noise), scratch, cavity darkening
    "laminate": dict(wear=0.35, wear_col=None, wear_metal=0.0, wear_rough=0.62, grime=0.55, dust=0.35,
                     smudge=0.10, scratch=0.25, cavity=0.35),
    "plastic": dict(wear=0.25, wear_col=None, wear_metal=0.0, wear_rough=0.65, grime=0.6, dust=0.4,
                    smudge=0.12, scratch=0.3, cavity=0.4),
    "paint": dict(wear=0.8, wear_col=(0.50, 0.50, 0.50), wear_metal=1.0, wear_rough=0.35, grime=0.8, dust=0.5,
                  smudge=0.10, scratch=0.45, cavity=0.45),
    "metal": dict(wear=0.45, wear_col="bright", wear_metal=1.0, wear_rough=0.22, grime=0.45, dust=0.25,
                  smudge=0.16, scratch=0.6, cavity=0.35),
    "wood": dict(wear=0.4, wear_col="bright", wear_metal=0.0, wear_rough=0.7, grime=0.45, dust=0.3,
                 smudge=0.08, scratch=0.25, cavity=0.4),
    "fabric": dict(wear=0.15, wear_col="bright", wear_metal=0.0, wear_rough=1.0, grime=0.5, dust=0.6,
                   smudge=0.04, scratch=0.0, cavity=0.5),
    "rubber": dict(wear=0.2, wear_col="bright", wear_metal=0.0, wear_rough=0.9, grime=0.4, dust=0.7,
                   smudge=0.1, scratch=0.1, cavity=0.35),
    "glass": dict(wear=0.0, wear_col=None, wear_metal=0.0, wear_rough=0.1, grime=0.25, dust=0.35,
                  smudge=0.22, scratch=0.25, cavity=0.2),
    "label": dict(wear=0.3, wear_col="bright", wear_metal=0.0, wear_rough=0.75, grime=0.35, dust=0.25,
                  smudge=0.05, scratch=0.2, cavity=0.2),
    "emissive": dict(wear=0.0, wear_col=None, wear_metal=0.0, wear_rough=0.3, grime=0.0, dust=0.0,
                     smudge=0.05, scratch=0.0, cavity=0.0),
    "cardboard": dict(wear=0.5, wear_col="bright", wear_metal=0.0, wear_rough=0.95, grime=0.5, dust=0.8,
                      smudge=0.05, scratch=0.1, cavity=0.4),
    "organic": dict(wear=0.0, wear_col=None, wear_metal=0.0, wear_rough=0.8, grime=0.2, dust=0.1,
                    smudge=0.1, scratch=0.0, cavity=0.5),
}

SLOT_KIND = {
    "plastic_beige": "plastic", "plastic_dark": "plastic", "plastic_grey": "plastic",
    "paint_grey": "paint", "paint_beige": "paint", "paint_blue": "paint", "paint_red": "paint",
    "accent_orange": "plastic",
    "steel": "metal", "steel_dark": "metal", "aluminium": "metal", "mirror": "glass",
    "wood_desk": "wood", "oak": "wood",
    "fabric_dark": "fabric", "mesh_fabric": "fabric", "fabric_light": "fabric", "fabric_white": "fabric",
    "fabric_blue": "fabric",
    "rubber": "rubber", "rubber_black": "rubber",
    "screen": "glass", "glass_dark": "glass", "glass_black": "glass", "glass_tint": "glass",
    "laminate_white": "laminate", "laminate_grey": "laminate", "paper": "laminate",
    "led_green": "emissive", "led_cyan": "emissive", "led_white": "emissive", "vend_panel": "emissive",
    "labels": "label",
    "food": "organic", "food_green": "organic", "food_yellow": "organic", "leaf": "organic", "soil": "organic",
    "paint_green": "paint", "cardboard": "cardboard", "tape": "plastic",
}

CONDITION = 1.0


def kind_of(slot_name, kind=None):
    """A slot's smart material kind: the material's own `vkr_smart`
    property when it names one, else the library slot table, else words in
    the name, else plastic."""
    if kind in KINDS:
        return kind
    if slot_name in SLOT_KIND:
        return SLOT_KIND[slot_name]
    lowered = slot_name.lower()
    for words, found in ((("paint",), "paint"), (("steel", "alu", "metal", "chrome", "iron", "brass"), "metal"),
                         (("wood", "oak", "walnut", "pine"), "wood"), (("fabric", "cloth", "felt", "leather"), "fabric"),
                         (("rubber", "tyre", "tire"), "rubber"), (("glass", "screen", "mirror"), "glass"),
                         (("laminate", "formica"), "laminate"), (("led", "light", "lamp", "emit"), "emissive"),
                         (("label", "sticker", "decal", "sign"), "label"), (("cardboard", "carton", "paper"), "cardboard"),
                         (("food", "leaf", "plant", "soil"), "organic")):
        if any(w in lowered for w in words):
            return found
    return "plastic"


# ---------------------------------------------------------------------------
# Noise

_PERM = np.random.default_rng(1234).permutation(256).astype(np.int64)
_P = np.concatenate([_PERM, _PERM])
_OCTAVE_ROT = np.array([[0.00, 0.80, 0.60], [-0.80, 0.36, -0.48], [-0.60, -0.48, 0.64]])


def _fade(t):
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0)


def _grad(h, x, y, z):
    h = h & 15
    u = np.where(h < 8, x, y)
    v = np.where(h < 4, y, np.where((h == 12) | (h == 14), x, z))
    return np.where(h & 1, -u, u) + np.where(h & 2, -v, v)


def perlin(p):
    """Gradient noise in about -1..1 at points `p` (N, 3)."""
    pi = np.floor(p)
    pf = (p - pi).astype(np.float32)
    pi = pi.astype(np.int64)
    X, Y, Z = pi[:, 0] & 255, pi[:, 1] & 255, pi[:, 2] & 255
    x, y, z = pf[:, 0], pf[:, 1], pf[:, 2]
    u, v, w = _fade(x), _fade(y), _fade(z)
    A = _P[X] + Y
    AA = _P[A] + Z
    AB = _P[A + 1] + Z
    B = _P[X + 1] + Y
    BA = _P[B] + Z
    BB = _P[B + 1] + Z

    def lerp(t, a, b):
        return a + t * (b - a)

    x1, y1, z1 = x - 1.0, y - 1.0, z - 1.0
    return lerp(w,
                lerp(v, lerp(u, _grad(_P[AA], x, y, z), _grad(_P[BA], x1, y, z)),
                     lerp(u, _grad(_P[AB], x, y1, z), _grad(_P[BB], x1, y1, z))),
                lerp(v, lerp(u, _grad(_P[AA + 1], x, y, z1), _grad(_P[BA + 1], x1, y, z1)),
                     lerp(u, _grad(_P[AB + 1], x, y1, z1), _grad(_P[BB + 1], x1, y1, z1))))


def fbm(p, scale, octaves=4, seed=0, gain=0.5):
    """Fractal noise in 0..1 (about 0.5 mean) at `p` metres, `scale` cycles
    per metre at the first octave."""
    total = np.zeros(len(p), dtype=np.float32)
    amp, norm = 1.0, 0.0
    q = p * scale + seed * 17.31
    for _ in range(octaves):
        total += amp * perlin(q)
        norm += amp
        amp *= gain
        q = (q @ _OCTAVE_ROT) * 2.02 + 3.7
    return np.clip(0.5 + 0.5 * total / norm * 1.6, 0.0, 1.0)


def value_noise(p, seed=0):
    """Fine grain in 0..1 (one Perlin octave, remapped)."""
    return np.clip(0.5 + 0.7 * perlin(p + seed * 31.7), 0.0, 1.0)


def smoothstep(e0, e1, x):
    span = e1 - e0
    if abs(span) < 1e-6:
        span = 1e-6
    t = np.clip((x - e0) / span, 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _blur(img, radius):
    """Separable box blur (two passes) in texture space."""
    if radius < 1:
        return img
    out = img
    for axis in (0, 1):
        c = np.cumsum(np.pad(out, [(radius + 1, radius) if a == axis else (0, 0) for a in range(out.ndim)],
                             mode="edge"), axis=axis, dtype=np.float64)
        n = out.shape[axis]
        hi = np.take(c, np.arange(2 * radius + 1, 2 * radius + 1 + n), axis=axis)
        lo = np.take(c, np.arange(0, n), axis=axis)
        out = ((hi - lo) / (2 * radius + 1)).astype(np.float32)
    return out


# ---------------------------------------------------------------------------
# Texture-space details ("stamps"): each Builder.detail() spec paints height
# (metres, negative is a recess), colour, roughness and metal into the texels
# whose baked position lies on its surface. No geometry: the bake's position
# and normal maps place them, as a projected decal would.

def _basis(normal, up):
    n = np.asarray(normal, dtype=np.float64)
    n /= np.linalg.norm(n) + 1e-12
    if up is None:
        up = (0.0, 0.0, 1.0) if abs(n[2]) < 0.9 else (0.0, 1.0, 0.0)
    u = np.cross(np.asarray(up, dtype=np.float64), n)
    if np.linalg.norm(u) < 1e-6:
        u = np.cross((1.0, 0.0, 0.0), n)
    u /= np.linalg.norm(u)
    v = np.cross(n, u)
    return n, u, v


class StampTarget:
    """The layers stamps write into (flat views of the composer's arrays),
    with a grid of texel positions so each stamp visits only the texels
    near it."""

    CELL = 0.05

    def __init__(self, pos, nrm, valid, albedo, rough, metal, height, emission, texel):
        self.pos, self.nrm, self.valid = pos, nrm, valid
        self.albedo, self.rough, self.metal = albedo, rough, metal
        self.height, self.emission, self.texel = height, emission, texel
        idx = np.nonzero(valid)[0]
        cells = np.floor(pos[idx] / self.CELL).astype(np.int64)
        self.origin = cells.min(0) if len(cells) else np.zeros(3, dtype=np.int64)
        self.dims = (cells.max(0) - self.origin + 1) if len(cells) else np.ones(3, dtype=np.int64)
        keys = self._key(cells)
        order = np.argsort(keys, kind="stable")
        self.sorted_keys = keys[order]
        self.sorted_idx = idx[order]

    def _key(self, cells):
        c = cells - self.origin
        return (c[:, 0] * self.dims[1] + c[:, 1]) * self.dims[2] + c[:, 2]

    def near(self, lo, hi):
        """Flat indices of valid texels whose cells overlap the box lo..hi."""
        a = np.maximum(np.floor(np.asarray(lo) / self.CELL).astype(np.int64), self.origin)
        b = np.minimum(np.floor(np.asarray(hi) / self.CELL).astype(np.int64), self.origin + self.dims - 1)
        if np.any(b < a):
            return np.zeros(0, dtype=np.int64)
        gx, gy, gz = np.meshgrid(np.arange(a[0], b[0] + 1), np.arange(a[1], b[1] + 1), np.arange(a[2], b[2] + 1),
                                 indexing="ij")
        keys = self._key(np.stack([gx.ravel(), gy.ravel(), gz.ravel()], 1))
        starts = np.searchsorted(self.sorted_keys, keys, side="left")
        ends = np.searchsorted(self.sorted_keys, keys, side="right")
        parts = [self.sorted_idx[s0:e0] for s0, e0 in zip(starts, ends) if e0 > s0]
        return np.concatenate(parts) if parts else np.zeros(0, dtype=np.int64)

    def select(self, spec, half_u, half_v, depth=0.006):
        """Texels on the stamp's surface: (indices, u, v) in local metres."""
        n, ua, va = _basis(spec["normal"], spec.get("up"))
        c = np.asarray(spec["center"], dtype=np.float64)
        reach = abs(half_u) + abs(half_v) + depth
        idx = self.near(c - reach, c + reach)
        if not len(idx):
            return idx, np.zeros(0), np.zeros(0)
        d = self.pos[idx] - c
        u = d @ ua
        v = d @ va
        keep = (np.abs(d @ n) < depth) & ((self.nrm[idx] @ n) > 0.55) & (np.abs(u) < half_u) & (np.abs(v) < half_v)
        return idx[keep], u[keep], v[keep]

    def sphere(self, spec, radius):
        """Texels within `radius` of the spec's centre: (indices, falloff)."""
        c = np.asarray(spec["center"], dtype=np.float64)
        idx = self.near(c - radius, c + radius)
        if not len(idx):
            return idx, np.zeros(0)
        d = np.linalg.norm(self.pos[idx] - c, axis=-1)
        keep = d < radius
        return idx[keep], (1.0 - d[keep] / radius) * spec.get("strength", 1.0)

    def paint(self, idx, weight, colour=None, rough=None, metal=None, height=None, mul=None):
        """Blends `weight` (per index, or one value) of each given layer."""
        if not len(idx):
            return
        w = np.broadcast_to(np.asarray(weight, dtype=np.float32), (len(idx),))
        if colour is not None:
            col = np.asarray(colour, dtype=np.float32)
            self.albedo[idx] = self.albedo[idx] * (1.0 - w[:, None]) + col * w[:, None]
        if mul is not None:
            self.albedo[idx] = self.albedo[idx] * (1.0 - w[:, None] * (1.0 - np.asarray(mul, dtype=np.float32)))
        if rough is not None:
            self.rough[idx] = self.rough[idx] * (1.0 - w) + rough * w
        if metal is not None:
            self.metal[idx] = self.metal[idx] * (1.0 - w) + metal * w
        if height is not None:
            self.height[idx] = self.height[idx] + np.broadcast_to(np.asarray(height, dtype=np.float32), (len(idx),)) * w


FINISH = {"steel": ((0.56, 0.56, 0.57), 1.0, 0.32), "black": ((0.035, 0.035, 0.037), 1.0, 0.42),
          "brass": ((0.78, 0.58, 0.26), 1.0, 0.3), "chrome": ((0.8, 0.8, 0.82), 1.0, 0.12),
          "zinc": ((0.66, 0.67, 0.66), 1.0, 0.45), "plastic": ((0.04, 0.04, 0.04), 0.0, 0.5)}


def stamp_screw(t, spec):
    r = spec.get("radius", 0.004)
    idx, u, v = t.select(spec, r * 1.3, r * 1.3)
    if not len(idx):
        return
    dist = np.sqrt(u * u + v * v)
    aa = t.texel * 0.75
    head = 1.0 - smoothstep(r - aa, r + aa, dist)
    col, metal, rough = FINISH[spec.get("finish", "steel")]
    dome = np.sqrt(np.clip(1.0 - (dist / r) ** 2, 0.0, 1.0)) * r * spec.get("dome", 0.35)
    kind = spec.get("drive", "phillips")
    au, av = np.abs(u), np.abs(v)
    if kind == "phillips":
        slot = ((au < r * 0.11) & (av < r * 0.62)) | ((av < r * 0.11) & (au < r * 0.62))
    elif kind == "slot":
        slot = (av < r * 0.1) & (au < r * 0.85)
    elif kind == "hex":
        slot = np.maximum(au * 0.866 + av * 0.5, av) < r * 0.42
    else:
        slot = np.zeros_like(au, dtype=bool)
    slot = slot.astype(np.float32) * head
    t.paint(idx, head, colour=col, metal=metal, rough=rough, height=dome - slot * r * 0.45)
    ring = smoothstep(r * 0.8, r, dist) * head
    t.paint(idx, np.clip(ring * 0.6 + slot * 0.7, 0.0, 1.0), mul=(0.35, 0.35, 0.35))
    seat = smoothstep(r * 1.3, r, dist) * (1.0 - head)
    t.paint(idx, seat * 0.5, mul=(0.45, 0.45, 0.45), height=-0.0003)


def stamp_rivet(t, spec):
    stamp_screw(t, dict(spec, drive="none", dome=spec.get("dome", 0.6)))


def stamp_vent(t, spec):
    """Horizontal slots (long along u) in a w x h panel."""
    w, h = spec["width"], spec["height"]
    idx, u, v = t.select(spec, w / 2, h / 2)
    if not len(idx):
        return
    n = max(1, int(spec.get("slots", 6)))
    pitch = h / n
    sw = spec.get("slot_width", pitch * 0.45)
    end = spec.get("end", min(sw, w * 0.1))
    vv = (v + h / 2) % pitch - pitch / 2
    aa = t.texel * 0.75
    inside = (1.0 - smoothstep(sw / 2 - aa, sw / 2 + aa, np.abs(vv))) * \
        (1.0 - smoothstep(w / 2 - end - aa, w / 2 - end + aa, np.abs(u)))
    t.paint(idx, inside, mul=(0.06, 0.06, 0.06), rough=0.9, height=-spec.get("depth", 0.004))
    lip = smoothstep(sw / 2 + 2.5 * aa, sw / 2, np.abs(vv)) * (1.0 - inside)
    t.paint(idx, lip * 0.35, mul=(0.6, 0.6, 0.6))


def stamp_perforation(t, spec):
    """Round holes on a staggered grid in a w x h panel."""
    w, h = spec["width"], spec["height"]
    idx, u, v = t.select(spec, w / 2, h / 2)
    if not len(idx):
        return
    p = spec.get("pitch", 0.012)
    hr = spec.get("hole_radius", p * 0.32)
    row = np.floor((v + h / 2) / (p * 0.866))
    vv = (v + h / 2) - (row + 0.5) * p * 0.866
    uu = (u + w / 2 + (row % 2) * p / 2) % p - p / 2
    dist = np.sqrt(uu * uu + vv * vv)
    aa = t.texel * 0.75
    margin = spec.get("margin", p * 0.6)
    inner = ((np.abs(u) < w / 2 - margin) & (np.abs(v) < h / 2 - margin)).astype(np.float32)
    hole = (1.0 - smoothstep(hr - aa, hr + aa, dist)) * inner
    t.paint(idx, hole, mul=(0.05, 0.05, 0.05), rough=0.95, height=-0.004)


def stamp_seam(t, spec):
    """A groove where the plane through `point` with normal `cut` meets the
    surfaces inside the box `min`..`max`."""
    cut = np.asarray(spec["cut"], dtype=np.float64)
    cut /= np.linalg.norm(cut)
    lo = np.asarray(spec["min"], dtype=np.float64)
    hi = np.asarray(spec["max"], dtype=np.float64)
    idx = t.near(lo, hi)
    if not len(idx):
        return
    p = t.pos[idx]
    keep = np.all((p >= lo) & (p <= hi), axis=-1)
    idx, p = idx[keep], p[keep]
    dist = np.abs((p - np.asarray(spec["point"])) @ cut)
    width = spec.get("width", 0.0014)
    aa = t.texel * 0.75
    groove = 1.0 - smoothstep(width / 2 - aa, width / 2 + aa, dist)
    shade = 1.0 - smoothstep(width / 2, width / 2 + 3 * aa, dist)
    t.paint(idx, groove, mul=(0.25, 0.25, 0.25), rough=0.85, height=-spec.get("depth", 0.0008))
    t.paint(idx, shade * 0.3, mul=(0.7, 0.7, 0.7))


def stamp_ribs(t, spec):
    """Parallel raised ribs along u (grips, rubber treads, knurling)."""
    w, h = spec["width"], spec["height"]
    idx, u, v = t.select(spec, w / 2, h / 2)
    if not len(idx):
        return
    p = spec.get("pitch", 0.004)
    rib = 0.5 + 0.5 * np.cos((v / p) * 2 * np.pi)
    t.paint(idx, 1.0, height=spec.get("depth", 0.0006) * (rib - 0.5))
    t.paint(idx, (1.0 - rib) * 0.25, mul=(0.6, 0.6, 0.6))


def stamp_touch(t, spec):
    """Hands' wear around `center`: greasy, smudged roughness on paint and
    plastic, polished metal, a little darkening."""
    idx, f = t.sphere(spec, spec.get("radius", 0.08))
    if not len(idx):
        return
    w = f ** 1.5 * (value_noise(t.pos[idx] * 90.0, 7) * 0.6 + 0.4)
    metal = t.metal[idx] > 0.5
    rough = t.rough[idx]
    t.rough[idx] = np.where(metal, rough - 0.12 * w, rough + 0.18 * w * (1.0 - rough))
    t.albedo[idx] = t.albedo[idx] * (1.0 - 0.18 * w[:, None])


def stamp_wear(t, spec):
    """Extra scuffing in a sphere (kick plates, armrest tops, door edges)."""
    idx, f = t.sphere(spec, spec.get("radius", 0.1))
    if not len(idx):
        return
    p = t.pos[idx]
    sc = value_noise(np.stack([p[:, 0] * 400.0, p[:, 1] * 9.0, p[:, 2] * 400.0], 1), 3)
    marks = smoothstep(0.72, 0.9, sc) * f
    t.albedo[idx] = np.clip(t.albedo[idx] * (1.0 + 0.6 * marks[:, None]) + 0.04 * marks[:, None], 0.0, 1.0)
    t.rough[idx] = t.rough[idx] + 0.2 * marks


def stamp_grime(t, spec):
    """Dirt collected in a sphere: darker, rougher."""
    idx, f = t.sphere(spec, spec.get("radius", 0.15))
    if not len(idx):
        return
    w = np.clip(f * (0.3 + fbm(t.pos[idx], 25.0, 3, 11)), 0.0, 1.0) * 0.6
    t.albedo[idx] = t.albedo[idx] * (1.0 - w[:, None]) + np.array([0.05, 0.045, 0.038]) * w[:, None]
    t.rough[idx] = t.rough[idx] + 0.25 * w


def stamp_glow(t, spec):
    """A small emissive dot or bar (status LEDs) painted on a surface."""
    w, h = spec.get("width", 0.004), spec.get("height", 0.004)
    idx, u, v = t.select(spec, w / 2 + t.texel, h / 2 + t.texel)
    if not len(idx):
        return
    aa = t.texel * 0.75
    if spec.get("round", True):
        inside = 1.0 - smoothstep(w / 2 - aa, w / 2 + aa, np.sqrt(u * u + v * v))
    else:
        inside = (1.0 - smoothstep(w / 2 - aa, w / 2 + aa, np.abs(u))) * \
            (1.0 - smoothstep(h / 2 - aa, h / 2 + aa, np.abs(v)))
    col = np.asarray(spec.get("colour", (0.2, 1.0, 0.4)), dtype=np.float32)
    t.paint(idx, inside, colour=np.clip(col, 0, 1) * 0.3 + 0.1, rough=0.2)
    t.emission[idx] = t.emission[idx] * (1.0 - inside[:, None]) + col * spec.get("strength", 4.0) * inside[:, None]


STAMPS = {"screw": stamp_screw, "rivet": stamp_rivet, "vent": stamp_vent, "perforation": stamp_perforation,
          "seam": stamp_seam, "ribs": stamp_ribs, "touch": stamp_touch, "wear": stamp_wear,
          "grime": stamp_grime, "glow": stamp_glow}


def compose(ch, slots, details=(), density=1024.0, condition=CONDITION, seed=1, kinds=None, keep=(),
            debug=None, unknown=None):
    """Layers the smart materials over the baked channels, then the asset's
    detail stamps. Returns albedo (linear), roughness, metallic, occlusion,
    emission and a height map in metres (all HxW)."""
    h, w = ch["albedo"].shape[:2]
    albedo = ch["albedo"][..., :3].reshape(-1, 3).astype(np.float32).copy()
    rough = np.clip(ch["data"][..., 0], 0.0, 1.0).ravel().astype(np.float32).copy()
    metal = np.clip(ch["data"][..., 1], 0.0, 1.0).ravel().astype(np.float32).copy()
    slot = np.clip(np.floor(ch["data"][..., 2] * 32.0), 0, len(slots) - 1).astype(np.int32).ravel()
    pos = ch["position"][..., :3].reshape(-1, 3).astype(np.float64)
    nrm = ch["normal_ws"][..., :3].reshape(-1, 3).astype(np.float64)
    valid = np.linalg.norm(nrm, axis=-1) > 0.5
    # Ray-traced masks carry sampling noise: smooth them a little.
    edge = _blur(np.clip(ch["masks"][..., 0] * 4.0, 0.0, 1.0), 1).ravel()
    cavity = _blur(np.clip(ch["masks"][..., 1], 0.0, 1.0), 1).ravel()
    lib_ao = np.clip(ch["masks"][..., 2], 0.0, 1.0).ravel()
    occ = _blur(np.clip(ch["occlusion"][..., 0], 0.0, 1.0), 2).ravel()
    emission = ch["emission"][..., :3].reshape(-1, 3).astype(np.float32).copy()

    big = fbm(pos, 1.6, 3, seed)
    mid = fbm(pos, 7.0, 4, seed + 5)
    fine = fbm(pos, 38.0, 3, seed + 9)
    grain = value_noise(pos * 220.0, seed + 13)
    # Hairline scratches: noise stretched along a direction that turns
    # slowly over the surface, thresholded thin.
    ang = (fbm(pos, 0.8, 2, seed + 3) - 0.5) * 6.283
    along = (pos[:, 0] * np.cos(ang) + pos[:, 1] * np.sin(ang)) * 4.0 + pos[:, 2] * 0.9
    across = (-pos[:, 0] * np.sin(ang) + pos[:, 1] * np.cos(ang)) * 300.0 + pos[:, 2] * 210.0
    sc = value_noise(np.stack([along, across, pos[:, 2] * 4.0], 1), seed + 21)
    scratches = smoothstep(0.83, 0.95, sc) * smoothstep(0.4, 0.7, mid)

    # An asset states its condition: 0.5 near new, 1 used, 2 worn out.
    for spec in details:
        if spec.get("kind") == "condition":
            condition = float(spec.get("value", condition))
    up = np.clip(nrm[:, 2], 0.0, 1.0)
    z = pos[:, 2]
    zmin = float(z[valid].min()) if valid.any() else 0.0
    height = np.zeros(h * w, dtype=np.float32)
    for k, name in enumerate(slots):
        if name in keep:
            continue
        m = (slot == k) & valid
        if not m.any():
            continue
        kind = KINDS[kind_of(name, (kinds or {}).get(name))]
        a = albedo[m]
        r = rough[m]
        mt = metal[m]
        lum = float((a * np.array([0.2126, 0.7152, 0.0722])).sum(-1).mean())
        tone = 0.012 if kind_of(name, (kinds or {}).get(name)) in ("laminate", "plastic", "glass") else 0.05
        a = a * (1.0 - tone + 2.0 * tone * big[m])[:, None]
        # Cavity darkening: a little ambient term in the albedo.
        cav = 1.0 - cavity[m]
        a = a * (1.0 - kind["cavity"] * 0.55 * smoothstep(0.1, 0.7, cav))[:, None]
        # Grime gathers in crevices, in occluded corners and near the floor:
        # it darkens and dulls what is there rather than painting brown
        # over it.
        g_occ = smoothstep(0.4, 0.95, 1.0 - occ[m]) * (0.45 + 0.9 * mid[m])
        g_crevice = smoothstep(0.15, 0.6, 1.0 - cavity[m]) * (0.6 + 0.4 * fine[m])
        g_floor = smoothstep(0.18, 0.0, z[m] - zmin) * (0.3 + 0.9 * big[m]) * 0.6
        g = np.clip((np.maximum(g_occ, g_crevice) + g_floor) * kind["grime"] * condition, 0.0, 0.75)
        grime_col = np.array([0.05, 0.044, 0.036]) if lum > 0.08 else np.array([0.075, 0.07, 0.062])
        a = a * (1.0 - 0.6 * g[:, None]) + grime_col * (0.25 * g[:, None])
        r = r + g * 0.15
        # Edge wear on convex edges (the edge mask where the local cavity is
        # open), broken by noise so it chips rather than outlines.
        convex = edge[m] * smoothstep(0.72, 0.97, cavity[m])
        wear_noise = 0.55 * mid[m] + 0.45 * fine[m]
        e = smoothstep(0.58, 0.78, convex * 0.85 + wear_noise * 0.7) * smoothstep(0.04, 0.22, convex)
        e = np.clip(e * kind["wear"] * condition * 1.5, 0.0, 1.0)
        if kind["wear_col"] == "bright":
            wear_a = np.clip(a * 1.3 + 0.03, 0.0, 1.0)
        elif kind["wear_col"] is None:
            wear_a = np.clip(a * 1.06 + 0.01, 0.0, 1.0)
        else:
            wear_a = np.broadcast_to(np.array(kind["wear_col"]), a.shape)
        a = a * (1.0 - e[:, None]) + wear_a * e[:, None]
        r = r * (1.0 - e) + kind["wear_rough"] * e
        mt = mt * (1.0 - e) + kind["wear_metal"] * e
        if kind["wear_metal"] > 0.5:
            height[m] -= 0.00035 * e  # chips through the paint
        # Dust settles on faces that look up.
        d = smoothstep(0.6, 0.97, up[m]) * (0.25 + 0.95 * big[m]) * kind["dust"] * condition
        d = np.clip(d * 0.3, 0.0, 0.35)
        a = a * (1.0 - d[:, None]) + np.array([0.34, 0.325, 0.3]) * d[:, None]
        r = r + d * 0.4
        # Smudges and fingerprints in the roughness, then hairline scratches.
        r = r + (mid[m] - 0.5) * kind["smudge"] * 1.4 + (grain[m] - 0.5) * kind["smudge"] * 0.5
        sct = scratches[m] * kind["scratch"] * condition
        r = r * (1.0 - sct * 0.6) + np.where(mt > 0.5, 0.12, 0.6) * sct * 0.6
        a = a * (1.0 + 0.3 * sct[:, None])
        albedo[m] = np.clip(a, 0.0, 1.0)
        rough[m] = np.clip(r, 0.04, 1.0)
        metal[m] = np.clip(mt, 0.0, 1.0)
    target = StampTarget(pos, nrm, valid, albedo, rough, metal, height, emission, 1.0 / density)
    unknown = [] if unknown is None else unknown
    for spec in details:
        if spec.get("kind") == "condition":
            continue
        fn = STAMPS.get(spec.get("kind"))
        if fn is None:
            unknown.append(spec.get("kind"))
            continue
        fn(target, spec)
    albedo = np.clip(albedo, 0.0, 1.0).reshape(h, w, 3)
    rough = np.clip(rough, 0.04, 1.0).reshape(h, w)
    metal = np.clip(metal, 0.0, 1.0).reshape(h, w)
    ao = np.clip(occ * (0.55 + 0.45 * lib_ao), 0.0, 1.0).reshape(h, w)
    height = height.reshape(h, w)
    emission = emission.reshape(h, w, 3)
    if debug is not None:
        debug.update(edge=edge.reshape(h, w), cavity=cavity.reshape(h, w), occlusion=occ.reshape(h, w),
                     big=big.reshape(h, w), mid=mid.reshape(h, w), scratches=scratches.reshape(h, w))
    return albedo, rough, metal, ao, emission, height


def apply_height(nmap, height, texel):
    """Adds a height map (metres per texel of `texel` metres) to the baked
    tangent normal map with a whiteout blend. Arrays are bottom-up, so
    axis 1 runs along U (tangent) and axis 0 along V (bitangent)."""
    if not np.any(height):
        return nmap[..., :3]
    gv, gu = np.gradient(height.astype(np.float32))
    su = np.clip(-gu / texel, -3.0, 3.0)
    sv = np.clip(-gv / texel, -3.0, 3.0)
    n = nmap[..., :3] * 2.0 - 1.0
    out = np.stack([n[..., 0] + su, n[..., 1] + sv, n[..., 2]], -1)
    out /= np.linalg.norm(out, axis=-1, keepdims=True) + 1e-8
    return out * 0.5 + 0.5


