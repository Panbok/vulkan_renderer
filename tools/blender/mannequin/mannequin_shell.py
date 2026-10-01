"""The mannequin's shell design: recessed seams, raised plates and the dark
flexible underlayer, as displacement and material masks on a dense mesh.

Plain numpy. Every feature is a signed field in metres over mesh vertices
whose zero set is the seam's centre line, with a soft mask limiting where it
applies:

- `Plane`: a plane cut, for rings around limbs, the collar and the belt.
- `Ellipsoid`: an ellipsoid surface, for caps over shoulders, knees, elbows.
- `Curve`: a smooth curve drawn in an orthographic view (front, back, side
  or any frame) and projected onto the surfaces facing that view inside its
  masks, for torso panels and the faceplate.
- `Unrolled`: a curve drawn on a limb or the torso unrolled around an axis,
  for panels that wrap around it.
- `Tube` and `Band`: an elliptic tube around an axis and a slab across it,
  for the collar, cuffs and rings.

Masks narrow features to a body part (`Region`), a side of a plane
(`HalfSpace`) or a volume (`Capsule`, `Box`).

`evaluate` turns features into a displacement along the vertex normals
(plates raised, grooves cut, the underlayer recessed) and into masks for the
texture bake: groove distance, underlayer, accent and plate rims. The design
is symmetric: features on the character's right (+X) are mirrored to the
left.
"""

from dataclasses import dataclass, field

import numpy as np


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _normalize(v):
    v = np.asarray(v, dtype=np.float64)
    return v / np.linalg.norm(v)


# =============================================================================
# Masks
# =============================================================================

@dataclass
class Capsule:
    """Soft mask: 1 within `radius` of the segment a-b, 0 beyond `radius +
    falloff`."""
    a: tuple
    b: tuple
    radius: float
    falloff: float = 0.01

    def __call__(self, p):
        a = np.asarray(self.a, dtype=np.float64)
        b = np.asarray(self.b, dtype=np.float64)
        axis = b - a
        t = np.clip(((p - a) @ axis) / (axis @ axis), 0.0, 1.0)
        d = np.linalg.norm(p - (a + t[:, None] * axis), axis=1)
        return 1.0 - smoothstep(self.radius, self.radius + self.falloff, d)


@dataclass
class Box:
    """Soft axis-aligned box mask."""
    low: tuple
    high: tuple
    falloff: float = 0.01

    def __call__(self, p):
        low = np.asarray(self.low)
        high = np.asarray(self.high)
        inside = np.minimum(p - low, high - p).min(axis=1)
        return smoothstep(-self.falloff, 0.0, inside)


@dataclass
class HalfSpace:
    """Soft mask: 1 on the side `normal` points to, fading over `falloff`."""
    point: tuple
    normal: tuple
    falloff: float = 0.004

    def __call__(self, p):
        s = (p - np.asarray(self.point)) @ _normalize(self.normal)
        return smoothstep(-self.falloff, self.falloff, s)


@dataclass
class Region:
    """Soft mask from a per-vertex region weight of the base body (upper
    arm, thigh, torso...), so features stay on their body part."""
    name: str

    def __call__(self, p, ctx):
        return ctx["regions"][self.name]


def _mask(p, masks, ctx):
    result = np.ones(len(p))
    for mask in masks:
        result *= mask(p, ctx) if isinstance(mask, Region) else mask(p)
    return result


# =============================================================================
# Features
# =============================================================================

@dataclass
class Plane:
    point: tuple
    normal: tuple
    masks: list = field(default_factory=list)

    def field(self, p, n, ctx):
        return (p - np.asarray(self.point)) @ _normalize(self.normal), _mask(p, self.masks, ctx)


@dataclass
class Ellipsoid:
    center: tuple
    radii: tuple
    masks: list = field(default_factory=list)
    # Optional rows of a rotation matrix: local axes in world space.
    axes: tuple = None

    def field(self, p, n, ctx):
        q = p - np.asarray(self.center)
        if self.axes is not None:
            q = q @ np.asarray(self.axes).T
        radii = np.asarray(self.radii)
        k0 = np.linalg.norm(q / radii, axis=1)
        k1 = np.linalg.norm(q / (radii * radii), axis=1)
        return k0 * (k0 - 1.0) / np.maximum(k1, 1e-12), _mask(p, self.masks, ctx)


VIEWS = {
    # Right and up axes of each orthographic view in world space, and the
    # direction it looks from: its curves project onto surfaces facing it.
    # Front and back share world X so one outline mirrors across both;
    # "side" is the character's right (+X), "side_left" its mirror.
    "front": (np.array([1.0, 0.0, 0.0]), np.array([0.0, 0.0, 1.0]), np.array([0.0, 1.0, 0.0])),
    "back": (np.array([1.0, 0.0, 0.0]), np.array([0.0, 0.0, 1.0]), np.array([0.0, -1.0, 0.0])),
    "side": (np.array([0.0, 1.0, 0.0]), np.array([0.0, 0.0, 1.0]), np.array([1.0, 0.0, 0.0])),
    "side_left": (np.array([0.0, 1.0, 0.0]), np.array([0.0, 0.0, 1.0]), np.array([-1.0, 0.0, 0.0])),
}


@dataclass
class Band:
    """The slab between fractions `t0` and `t1` along segment a-b: a sleeve
    around a limb (negative inside), within `radius` of the segment."""
    a: tuple
    b: tuple
    t0: float
    t1: float
    radius: float = 0.12
    masks: list = field(default_factory=list)

    def field(self, p, n, ctx):
        a = np.asarray(self.a, dtype=np.float64)
        b = np.asarray(self.b, dtype=np.float64)
        axis = b - a
        length = np.linalg.norm(axis)
        axis = axis / length
        t = (p - a) @ axis
        slab = np.maximum(self.t0 * length - t, t - self.t1 * length)
        radial = np.linalg.norm((p - a) - t[:, None] * axis, axis=1)
        mask = _mask(p, self.masks, ctx) * (1.0 - smoothstep(self.radius, self.radius + 0.01, radial))
        return slab, mask


@dataclass
class Tube:
    """Elliptic tube around the axis a-b (negative inside), `radii` along
    `lateral` and the axis-perpendicular direction orthogonal to it."""
    a: tuple
    b: tuple
    radii: tuple
    lateral: tuple = (1.0, 0.0, 0.0)
    masks: list = field(default_factory=list)

    def field(self, p, n, ctx):
        a = np.asarray(self.a, dtype=np.float64)
        axis = _normalize(np.asarray(self.b, dtype=np.float64) - a)
        lateral = np.asarray(self.lateral, dtype=np.float64)
        lateral = _normalize(lateral - (lateral @ axis) * axis)
        depth = np.cross(axis, lateral)
        q = p - a
        u = (q @ lateral) / self.radii[0]
        v = (q @ depth) / self.radii[1]
        return (np.sqrt(u * u + v * v) - 1.0) * min(self.radii), _mask(p, self.masks, ctx)


def _unrolled_uv(p, a, b, ref, radius):
    axis = _normalize(np.asarray(b, dtype=np.float64) - np.asarray(a, dtype=np.float64))
    ref = np.asarray(ref, dtype=np.float64)
    ref = _normalize(ref - (ref @ axis) * axis)
    side = np.cross(ref, axis)
    q = p - np.asarray(a, dtype=np.float64)
    t = q @ axis
    radial = q - t[:, None] * axis
    angle = np.arctan2(radial @ side, radial @ ref)
    return np.stack([angle * radius, t], axis=1), np.linalg.norm(radial, axis=1)


@dataclass
class Unrolled:
    """A curve drawn on the surface unrolled around the axis a-b: points are
    (arc, height), the arc being the angle from `ref` (towards `ref` x axis)
    times `radius` and the height the distance along a-b, in metres. Only
    surface within `reach` of the axis takes part. `closed` makes the field
    signed, negative inside."""
    a: tuple
    b: tuple
    ref: tuple
    points: list
    closed: bool = True
    radius: float = 0.05
    reach: float = 0.2
    masks: list = field(default_factory=list)
    smooth: bool = True

    def field(self, p, n, ctx):
        uv, radial = _unrolled_uv(p, self.a, self.b, self.ref, self.radius)
        mask = _mask(p, self.masks, ctx) * (1.0 - smoothstep(self.reach, self.reach + 0.01, radial))
        return _polyline_field(uv, mask, self.points, self.closed, self.smooth), mask


def _view_axes(view):
    return VIEWS[view] if isinstance(view, str) else tuple(np.asarray(a, dtype=np.float64) for a in view)


def _spline(points, closed, spacing=0.002):
    """Catmull-Rom samples through `points`, about `spacing` metres apart."""
    pts = np.asarray(points, dtype=np.float64)
    if len(pts) < 3:
        return np.vstack([pts, pts[:1]]) if closed else pts
    ring = np.vstack([pts[-1:], pts, pts[:2]]) if closed else np.vstack([pts[:1], pts, pts[-1:]])
    samples = []
    for i in range(1, len(ring) - 2):
        p0, p1, p2, p3 = ring[i - 1], ring[i], ring[i + 1], ring[i + 2]
        count = max(2, int(np.linalg.norm(p2 - p1) / spacing))
        t = np.linspace(0.0, 1.0, count, endpoint=False)[:, None]
        samples.append(0.5 * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t ** 2 +
                              (-p0 + 3 * p1 - 3 * p2 + p3) * t ** 3))
    samples.append(ring[-2:-1])
    out = np.vstack(samples)
    return np.vstack([out, out[:1]]) if closed else out


@dataclass
class Curve:
    """Smooth curve through points in a view's (right, up) plane; `closed`
    makes the field signed, negative inside. `view` names an orthographic
    view or gives its (right, up, toward) axes."""
    view: object
    points: list
    closed: bool = False
    masks: list = field(default_factory=list)
    facing: float = 0.15
    smooth: bool = True

    def field(self, p, n, ctx):
        right, up, toward = _view_axes(self.view)
        mask = _mask(p, self.masks, ctx) * smoothstep(self.facing - 0.1, self.facing + 0.1, n @ toward)
        uv = np.stack([p @ right, p @ up], axis=1)
        return _polyline_field(uv, mask, self.points, self.closed, self.smooth), mask


def _polyline_field(uv_all, mask, points, closed, smooth, margin=0.02):
    """Distance in a 2D parameter plane to a curve through `points`, signed
    for closed curves. Distances are exact only near the curve and inside
    the mask; elsewhere the field keeps its sign, for plates inside closed
    outlines."""
    if smooth:
        pts = _spline(points, closed)
    else:
        pts = np.asarray(points, dtype=np.float64)
        if closed:
            pts = np.vstack([pts, pts[:1]])
    low = pts.min(axis=0) - margin
    high = pts.max(axis=0) + margin
    near = (mask > 0.0) & np.all(uv_all >= low, axis=1) & np.all(uv_all <= high, axis=1)
    uv = uv_all[near]
    best = np.full(len(uv), np.inf)
    for a, b in zip(pts[:-1], pts[1:]):
        ab = b - a
        t = np.clip(((uv - a) @ ab) / max(ab @ ab, 1e-12), 0.0, 1.0)
        best = np.minimum(best, np.linalg.norm(uv - (a + t[:, None] * ab), axis=1))
    distance = np.full(len(uv_all), margin)
    distance[near] = np.minimum(best, margin)
    if closed:
        distance[near] = np.where(_inside(uv, pts[:-1]), -distance[near], distance[near])
    return distance


def _inside(uv, polygon):
    """Even-odd point-in-polygon for many points."""
    inside = np.zeros(len(uv), dtype=bool)
    x, y = uv[:, 0], uv[:, 1]
    for (x0, y0), (x1, y1) in zip(polygon, np.roll(polygon, -1, axis=0)):
        crosses = (y0 > y) != (y1 > y)
        at = x0 + (y - y0) * (x1 - x0) / np.where(y1 != y0, y1 - y0, 1e-12)
        inside ^= crosses & (x < at)
    return inside


# =============================================================================
# Seams, plates and underlayer
# =============================================================================

@dataclass
class Seam:
    """A groove along a feature's zero set. `fine` seams only reach the
    baked normal map, not the game mesh."""
    feature: object
    width: float = 0.0030     # half width at the surface
    depth: float = 0.0025
    fine: bool = False
    mirror: bool = True


@dataclass
class Plate:
    """A plate raised `height` inside a closed feature, rising over `bevel`
    from its outline, with a groove of half width `groove` at its foot.
    `tone` is "shell", "accent" or "under" (a raised part of the dark
    underlayer, such as a rib); `stack` adds the height to the plates below
    instead of taking the highest."""
    feature: object
    height: float = 0.004
    bevel: float = 0.008
    groove: float = 0.0022
    depth: float = 0.002
    tone: str = "shell"
    stack: bool = False
    fine: bool = False
    mirror: bool = True
    # Only a tone over what is below, no step.
    paint: bool = False


@dataclass
class Underlayer:
    """The dark flexible layer, recessed `recess` inside a closed feature."""
    feature: object
    recess: float = 0.0025
    bevel: float = 0.005
    groove: float = 0.0
    depth: float = 0.0
    fine: bool = False
    mirror: bool = True


def _mirrored(feature):
    """The same feature on the character's left (x negated)."""
    import copy
    other = copy.deepcopy(feature)
    def flip(value):
        return (-value[0], value[1], value[2])
    if isinstance(other, Plane):
        other.point = flip(other.point)
        other.normal = flip(other.normal)
    elif isinstance(other, Ellipsoid):
        other.center = flip(other.center)
        if other.axes is not None:
            other.axes = tuple(flip(axis) for axis in other.axes)
            # Keep a right-handed frame.
            other.axes = (tuple(-c for c in other.axes[0]), other.axes[1], other.axes[2])
    elif isinstance(other, Band):
        other.a = flip(other.a)
        other.b = flip(other.b)
    elif isinstance(other, Tube):
        other.a = flip(other.a)
        other.b = flip(other.b)
        other.lateral = flip(other.lateral)
    elif isinstance(other, Unrolled):
        # A reflection reverses the unrolled arc's direction.
        other.a = flip(other.a)
        other.b = flip(other.b)
        other.ref = flip(other.ref)
        other.points = [(-u, v) for u, v in other.points]
    elif isinstance(other, Curve):
        if other.view == "side":
            other.view = "side_left"
        elif isinstance(other.view, str):
            other.points = [(-u, v) for u, v in other.points][::-1]
        else:
            # A custom view mirrors its axes; right keeps pointing to +X so
            # the outline mirrors in u.
            right, up, toward = (np.asarray(a, dtype=np.float64) for a in other.view)
            flip3 = np.array([-1.0, 1.0, 1.0])
            other.view = (right * flip3 * -1.0 if abs(right[0]) > 0.5 else right * flip3,
                          up * flip3, toward * flip3)
            if abs(right[0]) > 0.5:
                other.points = [(-u, v) for u, v in other.points][::-1]
    for mask in other.masks:
        if isinstance(mask, Capsule):
            mask.a = flip(mask.a)
            mask.b = flip(mask.b)
        elif isinstance(mask, Box):
            low, high = mask.low, mask.high
            mask.low = (-high[0], low[1], low[2])
            mask.high = (-low[0], high[1], high[2])
        elif isinstance(mask, HalfSpace):
            mask.point = flip(mask.point)
            mask.normal = flip(mask.normal)
        elif isinstance(mask, Region):
            if mask.name.endswith("_r"):
                mask.name = mask.name[:-2] + "_l"
            elif mask.name.endswith("_l"):
                mask.name = mask.name[:-2] + "_r"
    return other


def _groove(distance, width):
    """1 at the centre line falling to 0 at `width`: a raised-cosine profile
    whose walls span the whole half width, so a dense mesh samples them
    smoothly."""
    if width <= 0.0:
        return np.zeros_like(distance)
    t = np.clip(np.abs(distance) / width, 0.0, 1.0)
    return 0.5 * (1.0 + np.cos(np.pi * t))


@dataclass
class Result:
    displacement: np.ndarray
    under: np.ndarray       # dark underlayer, 0..1
    accent: np.ndarray      # accent-coloured plates, 0..1
    edge: np.ndarray        # the rims of raised plates, where wear shows, 0..1
    # Distance to the nearest groove's centre line in units of its half
    # width (0 on the line, 1 at its edge, capped at GROOVE_REACH): smooth
    # enough to interpolate, so grooves are shaped per texel.
    groove_distance: np.ndarray


GROOVE_REACH = 4.0


# The game mesh holds no edge finer than about two of its own edges: its
# plate steps rise over at least this width and carry no grooves, and the
# baked normal map restores the crisp bevels and grooves of the detail mesh.
COARSE_BEVEL = 0.006


def evaluate(p, n, design, regions, fine=True, base="shell"):
    """Displacement along `n` and bake masks for a design (Seam, Plate and
    Underlayer entries). Tones layer in list order: each plate paints its
    tone over what came before and each underlayer paints it dark;
    uncovered surface keeps the `base` tone; `groove_shade` darkens grooves.
    Without `fine` (the game mesh), fine entries and grooves are left out
    and steps soften to COARSE_BEVEL."""
    ctx = {"regions": regions}
    count = len(p)
    groove_depth = np.zeros(count)
    groove_distance = np.full(count, GROOVE_REACH)

    def track(distance, width, mask):
        nonlocal groove_distance
        t = np.abs(distance) / (width * np.maximum(mask, 1e-3))
        groove_distance = np.minimum(groove_distance, t)

    highest = np.zeros(count)
    stacked = np.zeros(count)
    recess = np.zeros(count)
    shell_tone = np.full(count, 1.0 if base == "shell" else 0.0)
    accent = np.zeros(count)
    edge = np.zeros(count)
    for entry in design:
        if entry.fine and not fine:
            continue
        features = [entry.feature] + ([_mirrored(entry.feature)] if entry.mirror else [])
        for feature in features:
            s, mask = feature.field(p, n, ctx)
            if isinstance(entry, Seam):
                if fine:
                    g = _groove(s, entry.width) * mask
                    groove_depth = np.maximum(groove_depth, g * entry.depth)
                    track(s, entry.width, mask)
                continue
            inside = smoothstep(0.0, entry.bevel, -s) * mask
            step = inside if fine else smoothstep(0.0, max(entry.bevel, COARSE_BEVEL), -s) * mask
            if fine and entry.groove > 0.0:
                foot = _groove(s - entry.groove, entry.groove) * mask
                groove_depth = np.maximum(groove_depth, foot * entry.depth)
                track(s - entry.groove, entry.groove, mask)
            if isinstance(entry, Plate):
                if not entry.paint:
                    if entry.stack:
                        stacked += step * entry.height
                    else:
                        highest = np.maximum(highest, step * entry.height)
                    rim = smoothstep(0.3 * entry.bevel, entry.bevel, -s) * (
                        1.0 - smoothstep(entry.bevel, entry.bevel + 0.003, -s))
                    edge = np.maximum(edge, rim * mask)
                shell_tone = shell_tone * (1.0 - inside) + inside * (entry.tone == "shell")
                accent = accent * (1.0 - inside) + inside * (entry.tone == "accent")
            else:
                shell_tone *= 1.0 - inside
                accent *= 1.0 - inside
                recess = np.maximum(recess, step * entry.recess)
    displacement = highest + stacked - recess - groove_depth
    under = np.clip(1.0 - shell_tone - accent, 0.0, 1.0)
    return Result(displacement, under, accent, edge * (1.0 - under), groove_distance)


def groove_shade(distance):
    """Groove share and dark groove floor from `groove_distance`, per texel
    so the lines stay crisp and even."""
    return 1.0 - smoothstep(0.35, 1.0, distance), 1.0 - smoothstep(0.30, 0.70, distance)
