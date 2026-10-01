"""The mannequin's rest-pose surface: Blender Studio's CC0 realistic male body,
smoothed into a shell, with a new helmet head and boots grown from its neck
and ankle loops.

`load_body` runs inside Blender: it appends `GEO-body_male_realistic` from
the Human Base Meshes bundle pinned in sources.json and evaluates its
Multiresolution modifier. Everything else is plain numpy in the mannequin
frame: metres, Z up, facing +Y, the character's left at -X, feet on the
ground, 1.80 m tall. The bundle's own frame faces -Y, so the body turns half
a revolution about Z.
"""

import collections
import math
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

import mannequin_sdf as sdf


BUNDLE = "blender-studio/human-base-meshes-bundle-v1.4.1/human_base_meshes_bundle.blend"
OBJECT = "GEO-body_male_realistic"
STATURE = 1.80

PART_BODY = 0
PART_HEAD = 1
PART_BOOT = 2

# The helmet's frame: its centre and its upright up and forward axes.
HEAD_AXIS = np.array([0.0, 0.0, 1.0])
HEAD_FORWARD = np.array([0.0, 1.0, 0.0])
HEAD_CENTER = np.array([0.0, 0.025, 1.675])
# The jaw line: the helmet's underside rises gently from the chin to the
# neck along this plane; the helmet is the part above it.
RIM_POINT = np.array([0.0, 0.117, 1.556])
RIM_NORMAL = np.array([0.0, 0.25, 1.0]) / math.hypot(0.25, 1.0)


# =============================================================================
# The CC0 base
# =============================================================================

@dataclass
class BaseBody:
    positions: np.ndarray    # (n, 3) metres
    faces: list              # quads, vertex indices
    face_sets: np.ndarray    # (faces,) the bundle's sculpt face sets
    corner_uvs: list         # per face, one (u, v) per corner (UDIM tiles)


def load_body(sources_dir, level=1):
    """The base body at Multiresolution `level` (0 is the 10.5 k-vertex cage,
    1 is 42 k vertices). Runs inside Blender."""
    import bpy

    path = Path(sources_dir) / BUNDLE
    with bpy.data.libraries.load(str(path), link=False) as (data_from, data_to):
        if OBJECT not in data_from.objects:
            raise ValueError(f"{path} has no {OBJECT}")
        data_to.objects = [OBJECT]
    source = data_to.objects[0]
    bpy.context.scene.collection.objects.link(source)
    for modifier in source.modifiers:
        if modifier.type == 'MULTIRES':
            modifier.levels = level
            modifier.render_levels = level
    depsgraph = bpy.context.evaluated_depsgraph_get()
    mesh = bpy.data.meshes.new_from_object(source.evaluated_get(depsgraph),
                                           preserve_all_data_layers=True,
                                           depsgraph=depsgraph)
    bpy.data.objects.remove(source, do_unlink=True)

    positions = np.zeros(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get("co", positions)
    positions = positions.reshape(-1, 3)
    positions[:, :2] *= -1.0
    positions[:, 2] -= positions[:, 2].min()
    positions *= STATURE / positions[:, 2].max()

    faces = [tuple(polygon.vertices) for polygon in mesh.polygons]
    face_sets = np.zeros(len(mesh.polygons), dtype=np.int32)
    mesh.attributes[".sculpt_face_set"].data.foreach_get("value", face_sets)
    uv = np.zeros(len(mesh.loops) * 2)
    mesh.uv_layers["UVMap"].data.foreach_get("uv", uv)
    uv = uv.reshape(-1, 2)
    corner_uvs = [tuple(map(tuple, uv[p.loop_start:p.loop_start + p.loop_total]))
                  for p in mesh.polygons]
    bpy.data.meshes.remove(mesh)
    return BaseBody(positions=positions, faces=faces, face_sets=face_sets,
                    corner_uvs=corner_uvs)


# =============================================================================
# Quad-mesh topology
# =============================================================================

class Topology:
    def __init__(self, faces):
        self.faces = faces
        self.edge_faces = collections.defaultdict(list)
        self.vertex_edges = collections.defaultdict(set)
        for index, face in enumerate(faces):
            for i in range(len(face)):
                a, b = face[i], face[(i + 1) % len(face)]
                edge = (min(a, b), max(a, b))
                self.edge_faces[edge].append(index)
                self.vertex_edges[a].add(edge)
                self.vertex_edges[b].add(edge)

    def _continue(self, vertex, edge):
        """The edge-loop continuation through a valence-4 vertex."""
        if len(self.vertex_edges[vertex]) != 4:
            return None
        faces = set(self.edge_faces[edge])
        for candidate in self.vertex_edges[vertex]:
            if candidate != edge and not faces & set(self.edge_faces[candidate]):
                return candidate
        return None

    def loop_vertices(self, start_edge):
        """Ordered vertices of the closed edge loop through `start_edge`, or
        None when the loop meets a pole or does not close."""
        order = [start_edge[0]]
        vertex, edge = start_edge[1], start_edge
        while len(order) < 1000:
            if vertex == order[0]:
                return order
            order.append(vertex)
            edge = self._continue(vertex, edge)
            if edge is None:
                return None
            vertex = edge[0] if edge[1] == vertex else edge[1]
        return None

    def encircling_loop(self, positions, point, min_extent):
        """The closed loop around a limb or the neck (at least `min_extent`
        across in both X and Y) through a vertex near `point`, whose mean
        height is closest to the point's."""
        best = None
        near = np.argsort(np.linalg.norm(positions - point, axis=1))[:24]
        for vertex in near:
            for edge in self.vertex_edges[vertex]:
                loop = self.loop_vertices(edge)
                if loop is None:
                    continue
                spread = positions[loop].max(axis=0) - positions[loop].min(axis=0)
                if min(spread[0], spread[1]) < min_extent:
                    continue
                error = abs(positions[loop, 2].mean() - point[2])
                if best is None or error < best[0]:
                    best = (error, loop)
        if best is None:
            raise ValueError("No closed loop near " + str(point))
        return best[1]

    def region_faces(self, seed_vertex, barrier):
        """Faces reachable from a face touching `seed_vertex` without crossing
        an edge whose two vertices both lie on `barrier`."""
        barrier = set(barrier)
        start = next(iter(self.edge_faces[next(iter(self.vertex_edges[seed_vertex]))]))
        seen = {start}
        stack = [start]
        while stack:
            face = self.faces[stack.pop()]
            for i in range(len(face)):
                a, b = face[i], face[(i + 1) % len(face)]
                if a in barrier and b in barrier:
                    continue
                for neighbour in self.edge_faces[(min(a, b), max(a, b))]:
                    if neighbour not in seen:
                        seen.add(neighbour)
                        stack.append(neighbour)
        return seen


# =============================================================================
# Stylization: an idealized shell over the anatomy
# =============================================================================

def _adjacency(faces, count):
    """(neighbour indices, starts) in CSR form from face edges."""
    pairs = []
    for face in faces:
        for i in range(len(face)):
            a, b = face[i], face[(i + 1) % len(face)]
            pairs.append((a, b))
            pairs.append((b, a))
    pairs = np.unique(np.array(pairs), axis=0)
    starts = np.searchsorted(pairs[:, 0], np.arange(count + 1))
    return pairs[:, 1], starts


def taubin(positions, faces, weight, iterations, lam=0.5, mu=-0.53):
    """Masked Taubin smoothing, `weight` in [0, 1] per vertex."""
    targets, starts = _adjacency(faces, len(positions))
    counts = np.diff(starts)
    owner = np.repeat(np.arange(len(positions)), counts)
    p = positions.copy()
    for _ in range(iterations):
        for factor in (lam, mu):
            mean = np.zeros_like(p)
            np.add.at(mean, owner, p[targets])
            mean /= np.maximum(counts, 1)[:, None]
            p += (factor * weight)[:, None] * (mean - p)
    return p


def soft_box(p, low, high, falloff):
    inside = np.minimum(p - np.asarray(low), np.asarray(high) - p).min(axis=1)
    return np.clip(inside / falloff, 0.0, 1.0)


def fit_toward(p, region, axis_index, degree=2):
    """Blend `region`-weighted vertices toward a smooth polynomial surface
    fitted over them: coordinate `axis_index` as a function of the others."""
    others = [i for i in range(3) if i != axis_index]
    selected = region > 0.01
    a = p[selected][:, others[0]]
    b = p[selected][:, others[1]]
    low_a, high_a, low_b, high_b = a.min(), a.max(), b.min(), b.max()

    def basis(u, v):
        u = (u - low_a) / max(high_a - low_a, 1e-9) * 2.0 - 1.0
        v = (v - low_b) / max(high_b - low_b, 1e-9) * 2.0 - 1.0
        return np.stack([u ** i * v ** j for i in range(degree + 1) for j in range(degree + 1 - i)], axis=1)

    matrix = basis(a, b)
    w = region[selected]
    coef, *_ = np.linalg.lstsq(matrix * w[:, None], p[selected][:, axis_index] * w, rcond=None)
    out = p.copy()
    out[selected, axis_index] += w * (matrix @ coef - p[selected, axis_index])
    return out


def _neck(p):
    """Reshape the neck between the collar and the jaw about its own axis:
    narrower, deeper and set back over the spine, a strong column under
    the helmet."""
    out = p.copy()
    band = (p[:, 2] > 1.40) & (p[:, 2] < 1.66) & (np.abs(p[:, 0]) < 0.12)
    heights = p[band, 2]
    share = sdf.smoothstep(1.43, 1.51, heights)
    center_y = np.interp(heights, [1.40, 1.50, 1.60], [0.015, 0.022, 0.030])
    radial = np.stack([p[band, 0], p[band, 1] - center_y], axis=1)
    distance = np.linalg.norm(radial / np.array([1.0, 0.85]), axis=1)
    # Only the neck column, not the shoulders or chest around it.
    share = share * (1.0 - sdf.smoothstep(0.075, 0.10, distance))
    out[band, 0] = radial[:, 0] * (1.0 - 0.15 * share)
    out[band, 1] = center_y - 0.026 * share + radial[:, 1] * (1.0 + 0.16 * share)
    return out


def _bump(z, low, rise, fall, high):
    """1 between `rise` and `fall`, easing to 0 at `low` and `high`."""
    return sdf.smoothstep(low, rise, z) * (1.0 - sdf.smoothstep(fall, high, z))


def _heroic(p):
    """An armoured torso: the chest forward, the upper back and shoulder
    blades back, the belly in, so the torso tapers from a deep chest to a
    lean waist."""
    out = p.copy()
    z = p[:, 2]
    middle = np.interp(z, [1.00, 1.20, 1.40, 1.50], [0.030, 0.030, 0.015, -0.005])
    front = sdf.smoothstep(0.02, 0.07, p[:, 1] - middle)
    back = sdf.smoothstep(0.02, 0.07, middle - p[:, 1])
    lateral = 1.0 - sdf.smoothstep(0.13, 0.19, np.abs(p[:, 0]))
    chest = _bump(z, 1.20, 1.30, 1.38, 1.46)
    belly = _bump(z, 1.02, 1.09, 1.15, 1.21)
    blades = _bump(z, 1.10, 1.27, 1.40, 1.50)
    dy = front * (0.016 * chest - 0.009 * belly) - back * 0.022 * blades
    out[:, 1] += dy * lateral
    return out


def stylize(positions, faces):
    """Armour-smooth forms: one chest shield and back shield, a smooth
    abdomen and glutes, no nipples, a softened groin, a slender neck."""
    front = soft_box(positions, (-0.175, 0.045, 0.99), (0.175, 0.30, 1.46), 0.05)
    p = fit_toward(positions, 0.70 * front, 1, degree=4)
    p = fit_toward(p, 0.85 * soft_box(p, (-0.17, -0.30, 1.10), (0.17, -0.03, 1.47), 0.05), 1, degree=3)
    for side in (-1.0, 1.0):
        glute = soft_box(p, (min(0.0, side * 0.19), -0.30, 0.80), (max(0.0, side * 0.19), -0.03, 1.06), 0.035)
        glute *= np.clip(side * p[:, 0] / 0.02, 0.0, 1.0)
        p = fit_toward(p, glute, 1)
    weight = np.maximum(soft_box(p, (-0.03, -0.30, 0.80), (0.03, -0.03, 1.05), 0.02),
                        soft_box(p, (-0.07, 0.02, 0.76), (0.07, 0.30, 0.92), 0.03))
    for side in (-1.0, 1.0):
        r = np.linalg.norm((p - np.array([side * 0.10, 0.15, 1.295])) / np.array([0.035, 0.03, 0.035]), axis=1)
        weight = np.maximum(weight, np.clip(1.5 - r, 0.0, 1.0))
    p = _neck(_heroic(taubin(p, faces, weight, 60)))
    # A smooth neck column: no anatomy under the ribs and cords.
    radial = np.linalg.norm((p[:, :2] - np.array([0.0, -0.004])) / np.array([1.0, 1.1]), axis=1)
    neck = _bump(p[:, 2], 1.42, 1.47, 1.60, 1.65) * (1.0 - sdf.smoothstep(0.075, 0.095, radial))
    return taubin(p, faces, neck, 40)


# =============================================================================
# Helmet head and boots
# =============================================================================

def _head_local(p):
    """Points in the helmet frame: lateral, forward and up from its centre."""
    q = p - HEAD_CENTER
    return np.stack([q[:, 0], q @ HEAD_FORWARD, q @ HEAD_AXIS], axis=1)


def _column(points, low_center, high_center, radii):
    """Elliptic column along low->high with lateral (x) and depth (y) radii."""
    axis = high_center - low_center
    axis = axis / np.linalg.norm(axis)
    rel = points - low_center
    along = rel @ axis
    radial = rel - along[:, None] * axis
    scaled = radial[:, :2] / np.asarray(radii)
    return (np.linalg.norm(scaled, axis=1) - 1.0) * min(radii)


def head_shape(neck):
    """The helmet over the neck loop: a broad cranium over a long, upright
    face that narrows to a pointed chin, a level jaw line, and the neck
    rising into its underside."""
    low = neck.mean(axis=0)
    radii = (np.abs(neck[:, 0] - low[0]).max(), np.abs(neck[:, 1] - low[1]).max())

    def shape(p):
        q = _head_local(p)
        x, f, a = q[:, 0], q[:, 1], q[:, 2]
        cranium = sdf.ellipsoid(q, (0.0, -0.020, 0.040), (0.0875, 0.092, 0.088))
        brow = sdf.ellipsoid(q, (0.0, 0.015, 0.030), (0.076, 0.095, 0.075))
        head = sdf.smooth_union(cranium, brow, 0.03)
        face = sdf.ellipsoid(q, (0.0, 0.030, -0.035), (0.070, 0.095, 0.115))
        head = sdf.smooth_union(head, face, 0.03)
        # The face is upright and nearly flat, gently round across.
        front = np.sqrt(x * x + (f - (0.112 - 0.25)) ** 2) - 0.25
        head = sdf.smooth_intersect(head, front, 0.015)
        # Seen from the front the lower face narrows to the chin.
        taper = (np.abs(x) - (0.020 + 0.75 * (a + 0.117))) / 1.25
        head = sdf.smooth_intersect(head, taper, 0.012)
        # The jaw line rises gently from the chin to the neck.
        jaw = -(p - RIM_POINT) @ RIM_NORMAL
        head = sdf.smooth_intersect(head, jaw, 0.010)
        column = _column(p, low - np.array([0.0, 0.0, 0.04]), low + np.array([0.0, -0.006, 0.12]), radii)
        column = sdf.smooth_intersect(column, sdf.half_space(p, (0.0, 0.0, low[2] + 0.11), (0.0, 0.0, 1.0)), 0.02)
        return sdf.smooth_union(head, column, 0.025)

    return shape


def _footprint(q):
    """2D distance to the sole outline in the foot frame (x lateral, y
    forward from the ankle): a tapered stadium from heel to ball and a
    round toe."""
    heel = np.array([0.0, -0.026])
    ball = np.array([0.0, 0.118])
    axis = ball - heel
    t = np.clip(((q[:, :2] - heel) @ axis) / (axis @ axis), 0.0, 1.0)
    radius = 0.037 + (0.052 - 0.037) * t
    along = np.linalg.norm(q[:, :2] - (heel + t[:, None] * axis), axis=1) - radius
    toe = np.linalg.norm((q[:, :2] - np.array([0.0, 0.158])) / np.array([1.0, 1.15]), axis=1) - 0.047
    return sdf.smooth_union(along, toe, 0.03)


def boot_shape(ankle, toe_tip):
    """Boot below an ankle loop, its axis from the ankle towards the base
    foot's toe tip (keeping the source turnout)."""
    ankle_center = ankle.mean(axis=0)
    forward = toe_tip - ankle_center
    forward[2] = 0.0
    forward /= np.linalg.norm(forward)
    lateral = np.cross(forward, np.array([0.0, 0.0, 1.0]))
    frame = np.stack([lateral, forward, [0.0, 0.0, 1.0]], axis=1)
    origin = np.array([ankle_center[0], ankle_center[1], 0.0])
    ring = (ankle - ankle_center) @ frame
    radii = (np.abs(ring[:, 0]).max() + 0.004, np.abs(ring[:, 1]).max() + 0.004)
    ankle_z = ankle_center[2]

    def top_profile(y):
        """Height of the upper along the foot."""
        instep = ankle_z - 0.012 - (ankle_z - 0.075) * sdf.smoothstep(0.02, 0.13, y)
        return instep - 0.030 * sdf.smoothstep(0.12, 0.215, y)

    def shape(points):
        q = (points - origin) @ frame
        outline = _footprint(q)
        sole = sdf.smooth_intersect(outline, np.abs(q[:, 2] - 0.012) - 0.012, 0.004)
        crown = 0.25 * q[:, 0] ** 2 / 0.05
        upper = sdf.smooth_intersect(outline + 0.004, q[:, 2] + crown - top_profile(q[:, 1]), 0.028)
        upper = sdf.smooth_intersect(upper, 0.012 - q[:, 2], 0.004)
        boot = sdf.smooth_union(sole, upper, 0.006)
        shaft = _column(q, np.array([0.0, 0.0, 0.03]), np.array([0.0, 0.0, 1.0]), radii)
        shaft = sdf.smooth_intersect(shaft, sdf.half_space(q, (0, 0, ankle_z), (0, 0, 1)), 0.008)
        boot = sdf.smooth_union(boot, shaft, 0.02)
        return sdf.smooth_intersect(boot, -q[:, 2], 0.002)

    center = origin + frame @ np.array([0.0, 0.03, 0.05])
    return shape, center, frame


# =============================================================================
# Assembly
# =============================================================================

@dataclass
class Body:
    positions: np.ndarray
    faces: list
    face_parts: np.ndarray
    # Per face corner UVs: the base's UDIM layout for retained faces; for
    # new parts, islands in metres of surface.
    face_uvs: list
    # The base's face set per retained face, -1 for new faces.
    face_sets: np.ndarray
    # Per boot side, the foot frame (lateral, forward, up columns) and the
    # ankle loop centre.
    feet: dict = field(default_factory=dict)
    # Per vertex soft weights of body parts, from the base's face sets and
    # the new parts: head, torso, upperarm_l/r, forearm_l/r, hand_l/r, each
    # finger (thumb_r...pinky_l), thigh_l/r, shin_l/r, boot_l/r.
    regions: dict = field(default_factory=dict)


# The base's face sets per body part, character's right then left.
REGION_SETS = {
    "torso": (1, 17, 18, 19),
    "upperarm_r": (20,), "upperarm_l": (21,),
    "forearm_r": (11,), "forearm_l": (12,),
    "hand_r": (10,) + tuple(range(84, 104)), "hand_l": (9,) + tuple(range(64, 84)),
    "thigh_r": (23,), "thigh_l": (24,),
    "shin_r": (16,), "shin_l": (15,),
    "thumb_r": (84, 85, 86, 87), "index_r": (88, 89, 90, 91), "middle_r": (92, 93, 94, 95),
    "ring_r": (96, 97, 98, 99), "pinky_r": (100, 101, 102, 103),
    "thumb_l": (80, 81, 82, 83), "index_l": (76, 77, 78, 79), "middle_l": (72, 73, 74, 75),
    "ring_l": (68, 69, 70, 71), "pinky_l": (64, 65, 66, 67),
}


def _regions(positions, faces, face_sets, face_parts):
    """Per-vertex region weights, softened over two rings of neighbours."""
    count = len(positions)
    regions = {}
    targets, starts = _adjacency(faces, count)
    counts = np.diff(starts)
    owner = np.repeat(np.arange(count), counts)
    def soften(weight):
        for _ in range(2):
            mean = np.zeros(count)
            np.add.at(mean, owner, weight[targets])
            weight = 0.5 * weight + 0.5 * mean / np.maximum(counts, 1)
        return weight
    for name, sets in REGION_SETS.items():
        weight = np.zeros(count)
        for face, face_set in zip(faces, face_sets):
            if face_set in sets:
                weight[list(face)] = 1.0
        regions[name] = soften(weight)
    head = np.zeros(count)
    boots = {"l": np.zeros(count), "r": np.zeros(count)}
    for face, part in zip(faces, face_parts):
        if part == PART_HEAD:
            head[list(face)] = 1.0
        elif part == PART_BOOT:
            side = "l" if positions[face[0], 0] < 0.0 else "r"
            boots[side][list(face)] = 1.0
    regions["torso"] = np.maximum(regions["torso"], soften(head))
    regions["head"] = soften(head)
    regions["boot_l"] = soften(boots["l"])
    regions["boot_r"] = soften(boots["r"])
    # The boots take over the shins' ankle rings.
    regions["shin_l"] = np.maximum(regions["shin_l"], regions["boot_l"])
    regions["shin_r"] = np.maximum(regions["shin_r"], regions["boot_r"])
    return regions


def build(sources_dir):
    base = load_body(sources_dir, level=1)
    positions = stylize(base.positions, base.faces)
    topology = Topology(base.faces)

    neck = topology.encircling_loop(positions, np.array([0.0, 0.0, 1.55]), 0.08)
    removed = topology.region_faces(int(np.argmax(positions[:, 2])), neck)
    patches = [(PART_HEAD, neck, head_shape(positions[neck]), HEAD_CENTER - 0.02 * HEAD_AXIS,
                np.array([0.0, -0.15, 1.0]), 56, 0.86, False)]
    feet = {}
    for side, sign in (("l", -1.0), ("r", 1.0)):
        ankle = topology.encircling_loop(positions, np.array([sign * 0.18, -0.03, 0.10]), 0.05)
        candidates = np.flatnonzero(positions[:, 0] * sign > 0.03)
        sole_vertex = int(candidates[np.argmin(positions[candidates, 2])])
        removed |= topology.region_faces(sole_vertex, ankle)
        foot = positions[(positions[:, 2] < 0.13) & (positions[:, 0] * sign > 0.03)]
        toe_tip = foot[np.argmax(foot[:, 1])]
        shape, center, frame = boot_shape(positions[ankle], toe_tip)
        feet[side] = (frame, positions[ankle].mean(axis=0))
        patches.append((PART_BOOT, ankle, shape, center, np.array([0.0, 0.1, -1.0]), 22, 0.8, True))

    kept = [i for i in range(len(base.faces)) if i not in removed]
    used = sorted({v for i in kept for v in base.faces[i]})
    remap = {old: new for new, old in enumerate(used)}
    out_positions = [positions[used]]
    faces = [tuple(remap[v] for v in base.faces[i]) for i in kept]
    face_parts = [PART_BODY] * len(faces)
    face_uvs = [base.corner_uvs[i] for i in kept]
    face_sets = [int(base.face_sets[i]) for i in kept]
    count = len(used)
    for part, loop, shape, center, pole, rings, cap, double in patches:
        new_positions, patch_faces, patch_uvs, _ = sdf.radial_patch(
            shape, center, positions[loop], pole, rings, cap_fraction=cap, double=double)
        index = np.concatenate([np.array([remap[v] for v in loop]), count + np.arange(len(new_positions))])
        out_positions.append(new_positions)
        for face, uvs in zip(patch_faces, patch_uvs):
            faces.append(tuple(int(index[v]) for v in face))
            face_parts.append(part)
            face_uvs.append(tuple(uvs))
            face_sets.append(-1)
        count += len(new_positions)
    all_positions = np.concatenate(out_positions)
    face_parts = np.asarray(face_parts)
    face_sets = np.asarray(face_sets)
    return Body(positions=all_positions, faces=faces, face_parts=face_parts, face_uvs=face_uvs,
                face_sets=face_sets, feet=feet,
                regions=_regions(all_positions, faces, face_sets, face_parts))
