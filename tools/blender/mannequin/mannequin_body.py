"""The mannequin's rest-pose surface: the MakeHuman body between the neck and
ankle loops, with a new visor head and boots grown from those loops.

Plain numpy. The result keeps every retained MakeHuman vertex's source index
so its default skin weights and UVs survive, and labels every face with the
part it belongs to (body, head, boot) for materials and weights.
"""

import collections
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

import mannequin_makehuman as mh
import mannequin_sdf as sdf


STATURE = 1.80

# Athletic build over MakeHuman's 25-year-old male: macro sliders
# (muscle, weight, height, proportions) and region targets with weights.
MACRO = (1.0, 0.3, 0.6, 1.0)
SHAPE_TARGETS = {
    "torso/torso-vshape-incr": 0.6,
    "torso/torso-muscle-dorsi-incr": 0.5,
    "torso/torso-muscle-pectoral-incr": 0.4,
    "measure/measure-shoulder-dist-incr": 0.5,
    "measure/measure-upperarm-circ-incr": 0.4,
    "measure/measure-neck-circ-incr": 0.6,
    "measure/measure-waist-circ-decr": 0.3,
    "measure/measure-thigh-circ-incr": 0.3,
    "measure/measure-calf-circ-incr": 0.4,
    "armslegs/l-upperarm-shoulder-muscle-incr": 0.6,
    "armslegs/r-upperarm-shoulder-muscle-incr": 0.6,
    "armslegs/l-upperarm-muscle-incr": 0.5,
    "armslegs/r-upperarm-muscle-incr": 0.5,
    "armslegs/l-lowerarm-muscle-incr": 0.5,
    "armslegs/r-lowerarm-muscle-incr": 0.5,
    "armslegs/l-upperleg-muscle-incr": 0.4,
    "armslegs/r-upperleg-muscle-incr": 0.4,
    "armslegs/l-lowerleg-muscle-incr": 0.5,
    "armslegs/r-lowerleg-muscle-incr": 0.5,
    "neck/neck-scale-horiz-incr": 0.4,
    "stomach/stomach-tone-incr": 0.5,
}

PART_BODY = 0
PART_HEAD = 1
PART_BOOT = 2


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
# Head and boots
# =============================================================================

def _fit_ring(points):
    """Centroid, unit normal (sign arbitrary) and in-plane radii of a loop."""
    center = points.mean(axis=0)
    _, _, axes = np.linalg.svd(points - center)
    return center, axes[2], axes[0], axes[1]


def _column(points, low_center, high_center, radii):
    """Elliptic column along low->high whose cross-section has the given
    lateral (x) and depth (y) radii; open towards `low_center`."""
    axis = high_center - low_center
    axis = axis / np.linalg.norm(axis)
    rel = points - low_center
    along = rel @ axis
    radial = rel - along[:, None] * axis
    scaled = radial[:, :2] / np.asarray(radii)
    return (np.linalg.norm(scaled, axis=1) - 1.0) * min(radii)


def head_shape(neck, joint):
    """Visor head over the upper neck loop, in the build's absolute frame
    (metres, facing +Y). `joint` is the head joint (atlas)."""
    low = neck.mean(axis=0)
    radii = (np.abs(neck[:, 0] - low[0]).max(), np.abs(neck[:, 1] - low[1]).max())
    top = STATURE

    def shape(p):
        cranium = sdf.ellipsoid(p, (0.0, 0.048, top - 0.097), (0.076, 0.103, 0.097))
        face = sdf.ellipsoid(p, (0.0, 0.084, 1.642), (0.069, 0.078, 0.088))
        # Visor plane, leaning back towards the brow.
        face = sdf.smooth_intersect(face, sdf.half_space(p, (0.0, 0.155, 1.64), (0.0, 1.0, 0.10)), 0.03)
        for side in (-1.0, 1.0):
            cheek = sdf.half_space(p, (side * 0.058, 0.126, 1.64), (side * 0.80, 0.60, -0.02))
            face = sdf.smooth_intersect(face, cheek, 0.024)
            # A square jaw: steep side planes keep the width down to the chin.
            jaw = sdf.half_space(p, (side * 0.053, 0.090, 1.600), (side * 0.93, 0.18, -0.32))
            face = sdf.smooth_intersect(face, jaw, 0.02)
        face = sdf.smooth_intersect(face, sdf.half_space(p, (0.0, 0.110, 1.573), (0.0, 0.30, -1.0)), 0.018)
        chin = sdf.ellipsoid(p, (0.0, 0.132, 1.585), (0.034, 0.022, 0.021))
        face = sdf.smooth_union(face, chin, 0.014)
        head = sdf.smooth_union(cranium, face, 0.03)
        # The upper neck enters the skull from the loop below.
        column = _column(p, low - np.array([0.0, 0.0, 0.04]), low + np.array([0.0, 0.01, 0.1]), radii)
        column = sdf.smooth_intersect(column, sdf.half_space(p, (0.0, 0.0, low[2] + 0.07), (0.0, 0.0, 1.0)), 0.02)
        return sdf.smooth_union(head, column, 0.035)

    center = np.array([0.0, 0.060, 1.665])
    return shape, center


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


def boot_shape(ankle, foot_head, toe_tip):
    """Boot below an ankle loop. The foot axis runs from the ankle towards the
    MakeHuman toe tip, keeping the source turnout."""
    ankle_center = ankle.mean(axis=0)
    forward = toe_tip - foot_head
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
    face_parts: list
    # Per face corner UVs: MakeHuman's for retained faces; for new parts,
    # unpacked islands in metres of surface.
    face_uvs: list
    # Per vertex: MakeHuman body index, or -1 for new vertices.
    source: np.ndarray
    # Per vertex: part label and ring parameter (0 at the loop, 1 at the pole).
    vertex_part: np.ndarray
    ring_parameter: np.ndarray
    # Per new part: (vertex indices with the boundary loop first, loop size).
    part_uvs: dict = field(default_factory=dict)
    # Target-applied MakeHuman vertices (body and helpers) for joints.
    makehuman_positions: np.ndarray = None


def build(source_dir):
    source_dir = Path(source_dir)
    base = mh.BaseMesh(source_dir / "3dobjs/base.obj")
    weighted = mh.macro_targets(*MACRO) + [(k + ".target", w) for k, w in SHAPE_TARGETS.items()]
    raw = mh.to_blender(mh.apply_targets(base, source_dir / "targets", weighted))
    body = raw[:mh.BODY_VERTEX_COUNT]
    floor = body[:, 2].min()
    scale = STATURE / (body[:, 2].max() - floor)
    positions_all = (raw - np.array([0.0, 0.0, floor])) * scale
    positions = positions_all[:mh.BODY_VERTEX_COUNT]
    skeleton = mh.Skeleton(source_dir / "rigs/default.mhskel")
    topology = Topology(base.body_faces)

    # Loops found on the untouched base so their topology never depends on
    # the build's targets.
    reference = mh.to_blender(base.vertices[:mh.BODY_VERTEX_COUNT])
    reference = (reference - [0, 0, reference[:, 2].min()]) * (STATURE / np.ptp(reference[:, 2]))
    # The narrow loop under the jaw: its throat side is lower than the nape.
    neck_high = topology.encircling_loop(reference, np.array([0.0, 0.04, 1.559]), 0.08)
    ankles = {side: topology.encircling_loop(reference, np.array([sign * 0.21, 0.01, 0.118]), 0.06)
              for side, sign in (("l", -1.0), ("r", 1.0))}

    top_vertex = int(np.argmax(reference[:, 2]))
    removed = topology.region_faces(top_vertex, neck_high)
    for side, loop in ankles.items():
        # The character's left is -X.
        candidates = np.flatnonzero(reference[:, 0] * (-1.0 if side == "l" else 1.0) > 0.0)
        sole_vertex = int(candidates[np.argmin(reference[candidates, 2])])
        removed |= topology.region_faces(sole_vertex, loop)
    kept_faces = [i for i in range(len(base.body_faces)) if i not in removed]
    used = sorted({v for i in kept_faces for v in base.body_faces[i]})
    remap = {old: new for new, old in enumerate(used)}

    out_positions = [positions[used]]
    source = [np.array(used, dtype=np.int64)]
    vertex_part = [np.full(len(used), PART_BODY)]
    ring_parameter = [np.zeros(len(used))]
    faces = [tuple(remap[v] for v in base.body_faces[i]) for i in kept_faces]
    face_parts = [PART_BODY] * len(faces)
    face_uvs = [tuple(base.uvs[t] for t in base.body_face_uvs[i]) for i in kept_faces]
    part_uvs = {}
    count = len(used)

    def add_patch(label, key, loop, shape, center, pole, rings, cap_fraction):
        nonlocal count
        ring = positions[loop]
        new_positions, patch_faces, patch_uvs, params = sdf.radial_patch(
            shape, center, ring, pole, rings, cap_fraction=cap_fraction)
        index = np.concatenate([np.array([remap[v] for v in loop]),
                                count + np.arange(len(new_positions))])
        out_positions.append(new_positions)
        source.append(np.full(len(new_positions), -1))
        vertex_part.append(np.full(len(new_positions), label))
        ring_parameter.append(params)
        for face, uvs in zip(patch_faces, patch_uvs):
            faces.append(tuple(int(index[v]) for v in face))
            face_parts.append(label)
            face_uvs.append(tuple(uvs))
        part_uvs[key] = (index, len(loop))
        count += len(new_positions)

    joint = skeleton.head(positions_all, "head")
    head, head_center = head_shape(positions[neck_high], joint)
    add_patch(PART_HEAD, "head", neck_high, head, head_center, np.array([0.0, -0.15, 1.0]), 24, 0.86)
    for side, loop in ankles.items():
        suffix = ".L" if side == "l" else ".R"
        foot_head = skeleton.head(positions_all, "foot" + suffix)
        toe_tip = skeleton.tail(positions_all, "toe3-3" + suffix)
        boot, boot_center, _ = boot_shape(positions[loop], foot_head, toe_tip)
        add_patch(PART_BOOT, "boot_" + side, loop, boot, boot_center, np.array([0.0, 0.1, -1.0]), 22, 0.8)

    return Body(positions=np.concatenate(out_positions), faces=faces, face_parts=face_parts,
                face_uvs=face_uvs, source=np.concatenate(source),
                vertex_part=np.concatenate(vertex_part), ring_parameter=np.concatenate(ring_parameter),
                part_uvs=part_uvs, makehuman_positions=positions_all)
