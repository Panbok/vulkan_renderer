"""Implicit shapes and ring-patch meshing for the mannequin's head and boots.

Shapes are vectorized numpy functions of (n, 3) metre positions returning an
approximate signed distance: negative inside. Only their zero set matters to
meshing, but blends assume values in metres near the surface.

``radial_patch`` grows a quad patch from an existing ordered boundary loop:
every vertex is the first exit of a ray from an interior centre, so a shape
that is star-shaped from that centre meshes without folds. Rings run from
the boundary towards a pole direction and a square grid caps the pole, so
every face is a quad except the triangles of an optional doubling ring.
"""

import numpy as np


# =============================================================================
# Primitives and blends
# =============================================================================

def _normalize(vectors):
    vectors = np.asarray(vectors, dtype=np.float64)
    length = np.linalg.norm(vectors, axis=-1, keepdims=True)
    return vectors / np.maximum(length, 1e-12)


def ellipsoid(points, center, radii):
    """Approximate distance to an axis-aligned ellipsoid."""
    p = (points - np.asarray(center)) / np.asarray(radii)
    k0 = np.linalg.norm(p, axis=1)
    k1 = np.linalg.norm(p / np.asarray(radii), axis=1)
    return k0 * (k0 - 1.0) / np.maximum(k1, 1e-12)


def rounded_box(points, center, half_extents, radius, rotation=None):
    p = points - np.asarray(center)
    if rotation is not None:
        p = p @ np.asarray(rotation)
    q = np.abs(p) - (np.asarray(half_extents) - radius)
    outside = np.linalg.norm(np.maximum(q, 0.0), axis=1)
    inside = np.minimum(np.max(q, axis=1), 0.0)
    return outside + inside - radius


def capsule(points, a, b, radius_a, radius_b=None):
    """Round cone from `a` to `b`, radii linearly interpolated."""
    if radius_b is None:
        radius_b = radius_a
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    axis = b - a
    t = np.clip(((points - a) @ axis) / (axis @ axis), 0.0, 1.0)
    closest = a + t[:, None] * axis
    return np.linalg.norm(points - closest, axis=1) - (radius_a + (radius_b - radius_a) * t)


def half_space(points, point, normal):
    """Negative behind the plane through `point` facing `normal`."""
    return (points - np.asarray(point)) @ _normalize(normal)


def smooth_union(a, b, k):
    h = np.clip(0.5 + 0.5 * (b - a) / k, 0.0, 1.0)
    return b + (a - b) * h - k * h * (1.0 - h)


def smooth_subtract(a, b, k):
    """`a` with `b` carved out."""
    h = np.clip(0.5 - 0.5 * (a + b) / k, 0.0, 1.0)
    return a + (-b - a) * h + k * h * (1.0 - h)


def smooth_intersect(a, b, k):
    h = np.clip(0.5 - 0.5 * (b - a) / k, 0.0, 1.0)
    return b + (a - b) * h + k * h * (1.0 - h)


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


# =============================================================================
# Ray casting
# =============================================================================

def first_exit(shape, origins, directions, reach, step=0.0015):
    """First inside-to-outside crossing along each ray, refined by bisection.

    Every origin must lie inside `shape`. Raises when a ray stays inside for
    its whole reach, which means the shape or centre is wrong.
    """
    origins = np.broadcast_to(np.asarray(origins, dtype=np.float64), directions.shape).copy()
    directions = _normalize(directions)
    count = len(directions)
    if np.any(shape(origins) >= 0.0):
        raise ValueError("Ray origin outside the shape")
    low = np.zeros(count)
    high = np.full(count, np.nan)
    t = 0.0
    while t < reach and np.isnan(high).any():
        t += step
        pending = np.isnan(high)
        values = shape(origins[pending] + directions[pending] * t)
        crossed = np.flatnonzero(pending)[values >= 0.0]
        high[crossed] = t
        low[np.flatnonzero(pending)[values < 0.0]] = t
    if np.isnan(high).any():
        raise ValueError("Ray never left the shape within its reach")
    for _ in range(30):
        middle = 0.5 * (low + high)
        inside = shape(origins + directions * middle[:, None]) < 0.0
        low = np.where(inside, middle, low)
        high = np.where(inside, high, middle)
    return origins + directions * (0.5 * (low + high))[:, None]


def _slerp(a, b, t):
    """Per-row spherical interpolation of unit vectors a towards b."""
    a = _normalize(a)
    b = _normalize(b)
    dot = np.clip(np.sum(a * b, axis=-1), -1.0, 1.0)
    angle = np.arccos(dot)
    sin = np.sin(angle)
    t = np.asarray(t, dtype=np.float64)
    small = sin < 1e-6
    wa = np.where(small, 1.0 - t, np.sin((1.0 - t) * angle) / np.where(small, 1.0, sin))
    wb = np.where(small, t, np.sin(t * angle) / np.where(small, 1.0, sin))
    return _normalize(a * wa[..., None] + b * wb[..., None])


# =============================================================================
# Ring patches
# =============================================================================

def _coons_grid(boundary, size):
    """Interior of a size x size grid whose 4*size boundary points are given
    counter-clockwise from a corner. Returns (size+1, size+1, d)."""
    side = [boundary[i * size:(i + 1) * size + 1] if i < 3
            else np.concatenate([boundary[3 * size:], boundary[:1]])
            for i in range(4)]
    bottom = side[0]
    right = side[1]
    top = side[2][::-1]
    left = side[3][::-1]
    grid = np.zeros((size + 1, size + 1, boundary.shape[1]))
    for i in range(size + 1):
        u = i / size
        for j in range(size + 1):
            v = j / size
            grid[j, i] = ((1 - v) * bottom[i] + v * top[i] +
                          (1 - u) * left[j] + u * right[j] -
                          ((1 - u) * (1 - v) * bottom[0] + u * (1 - v) * bottom[size] +
                           (1 - u) * v * top[0] + u * v * top[size]))
    return grid


def radial_patch(shape, center, boundary, pole, rings, cap_fraction=0.82,
                 double=True, reach=0.6, spacing_power=1.0, blend_rings=4.0):
    """Mesh the part of `shape` beyond an ordered closed boundary loop.

    `boundary` is (N, 3) existing vertices in loop order; `pole` is the
    direction the rings converge to from `center`. Rings stop at
    `cap_fraction` of the angle to the pole, and a grid caps the rest.
    With `double`, the first new ring has 2N vertices (triangles bridge the
    step) so the patch is twice as dense as the boundary. The boundary's
    offset from the shape fades out over `blend_rings` rings.

    Returns (vertices, faces, face_uvs, ring_parameter). Face indices below
    N refer to `boundary` and higher indices to `vertices` offset by N.
    `face_uvs` holds one (u, v) per face corner: the rings form one island
    (u around the loop, v from the boundary) and the cap another, both in
    metres of surface so islands keep their relative texel density.
    `ring_parameter` is 0 at the boundary and 1 at the pole per new vertex.
    """
    center = np.asarray(center, dtype=np.float64)
    pole = _normalize(pole)
    boundary = np.asarray(boundary, dtype=np.float64)
    count = len(boundary)
    base_dirs = _normalize(boundary - center)
    if double:
        mids = _slerp(base_dirs, np.roll(base_dirs, -1, axis=0), np.full(count, 0.5))
        ring_dirs = np.empty((2 * count, 3))
        ring_dirs[0::2] = base_dirs
        ring_dirs[1::2] = mids
    else:
        ring_dirs = base_dirs
    longitudes = len(ring_dirs)
    if longitudes % 4:
        raise ValueError("Ring vertex count must be divisible by four for the cap")

    # Place rings at even arc length along the average longitude.
    samples = np.linspace(0.0, cap_fraction, 96)[1:]
    dense = np.stack([first_exit(shape, center, _slerp(ring_dirs, np.broadcast_to(pole, ring_dirs.shape),
                                                       np.full(longitudes, s)), reach)
                      for s in samples])
    start = boundary
    if double:
        start = np.empty((longitudes, 3))
        start[0::2] = boundary
        start[1::2] = 0.5 * (boundary + np.roll(boundary, -1, axis=0))
    path = np.concatenate([start[None], dense], axis=0)
    arc = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(path, axis=0), axis=2).mean(axis=1))])
    targets = (np.arange(1, rings + 1) / rings) ** spacing_power * arc[-1]
    ring_s = np.interp(targets, arc, np.concatenate([[0.0], samples]))

    # The shape only approximates the surface the boundary lies on. Carry the
    # boundary's offset from its own ray hits into the first rings so the
    # patch leaves the loop without a step.
    residual = start - first_exit(shape, center, ring_dirs, reach)

    vertices = []
    param = []
    ring_ids = []
    for index, s in enumerate(ring_s):
        points = first_exit(shape, center, _slerp(ring_dirs, np.broadcast_to(pole, ring_dirs.shape),
                                                  np.full(longitudes, s)), reach)
        fade = 1.0 - smoothstep(0.0, blend_rings, float(index + 1))
        points = points + residual * fade
        ids = count + len(vertices) + np.arange(longitudes)
        ring_ids.append(ids)
        vertices.extend(points)
        param.extend([(index + 1) / (rings + 1)] * longitudes)

    # Ring island: u runs around the loop, v along the rings, both scaled to
    # the mean surface lengths.
    ring_points = np.concatenate([start[None], np.asarray(vertices).reshape(len(ring_s), longitudes, 3)])
    circumference = np.linalg.norm(np.diff(np.concatenate([ring_points, ring_points[:, :1]], axis=1), axis=1),
                                   axis=2).sum(axis=1).mean()
    heights = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(ring_points, axis=0), axis=2).mean(axis=1))])

    def ring_uv(ring, k):
        return (circumference * k / longitudes, heights[ring])

    faces = []
    face_uvs = []
    first = ring_ids[0]
    if double:
        for j in range(count):
            a = j
            b = (j + 1) % count
            m0 = first[2 * j]
            m1 = first[2 * j + 1]
            m2 = first[(2 * j + 2) % longitudes]
            faces.append((a, b, m2, m1))
            face_uvs.append((ring_uv(0, 2 * j), ring_uv(0, 2 * j + 2), ring_uv(1, 2 * j + 2), ring_uv(1, 2 * j + 1)))
            faces.append((a, m1, m0))
            face_uvs.append((ring_uv(0, 2 * j), ring_uv(1, 2 * j + 1), ring_uv(1, 2 * j)))
    else:
        for j in range(count):
            faces.append((j, (j + 1) % count, first[(j + 1) % count], first[j]))
            face_uvs.append((ring_uv(0, j), ring_uv(0, j + 1), ring_uv(1, j + 1), ring_uv(1, j)))
    for ring, (inner, outer) in enumerate(zip(ring_ids[:-1], ring_ids[1:]), start=1):
        for k in range(longitudes):
            k1 = (k + 1) % longitudes
            faces.append((inner[k], inner[k1], outer[k1], outer[k]))
            face_uvs.append((ring_uv(ring, k), ring_uv(ring, k + 1), ring_uv(ring + 1, k + 1), ring_uv(ring + 1, k)))

    # Grid cap in the gnomonic plane around the pole.
    size = longitudes // 4
    last = np.asarray(vertices[-longitudes:])
    last_dirs = _normalize(last - center)
    helper = np.array([1.0, 0.0, 0.0]) if abs(pole[0]) < 0.9 else np.array([0.0, 1.0, 0.0])
    axis_u = _normalize(np.cross(pole, helper))
    axis_v = np.cross(pole, axis_u)
    plane = np.stack([last_dirs @ axis_u, last_dirs @ axis_v], axis=1) / (last_dirs @ pole)[:, None]
    grid = _coons_grid(plane, size)
    last_ids = ring_ids[-1]
    grid_ids = np.full((size + 1, size + 1), -1, dtype=np.int64)
    # Boundary of the grid, counter-clockwise from (0,0), maps to the ring.
    boundary_cells = ([(0, i) for i in range(size)] + [(j, size) for j in range(size)] +
                      [(size, size - i) for i in range(size)] + [(size - j, 0) for j in range(size)])
    grid_points = np.zeros((size + 1, size + 1, 3))
    for k, (j, i) in enumerate(boundary_cells):
        grid_ids[j, i] = last_ids[k]
        grid_points[j, i] = last[k]
    interior = [(j, i) for j in range(1, size) for i in range(1, size)]
    if interior:
        planar = np.array([grid[j, i] for j, i in interior])
        dirs = _normalize(pole[None] + planar[:, 0:1] * axis_u[None] + planar[:, 1:2] * axis_v[None])
        points = first_exit(shape, center, dirs, reach)
        for (j, i), point in zip(interior, points):
            grid_ids[j, i] = count + len(vertices)
            grid_points[j, i] = point
            vertices.append(point)
            param.append(1.0)
    # Cap island in metres: mean grid spacing along each direction.
    step_u = np.linalg.norm(np.diff(grid_points, axis=1), axis=2).mean()
    step_v = np.linalg.norm(np.diff(grid_points, axis=0), axis=2).mean()
    for j in range(size):
        for i in range(size):
            faces.append((grid_ids[j, i], grid_ids[j, i + 1], grid_ids[j + 1, i + 1], grid_ids[j + 1, i]))
            face_uvs.append(((i * step_u, j * step_v), ((i + 1) * step_u, j * step_v),
                             ((i + 1) * step_u, (j + 1) * step_v), (i * step_u, (j + 1) * step_v)))
    return np.asarray(vertices), faces, face_uvs, np.asarray(param)
