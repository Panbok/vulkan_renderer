"""MakeHuman CC0 base mesh, macro targets and default rig data.

Plain Python and numpy, so both Blender and the standalone checks can read
the same sources. MakeHuman stores decimetres, Y up, facing +Z, with its
``.L`` side on +X. ``to_blender`` converts to the mannequin frame: metres,
Blender Z up, facing +Y (glTF -Z, the engine's actor forward), with the
character's left side on -X.
"""

import json
from pathlib import Path

import numpy as np


BODY_VERTEX_COUNT = 13380

# MakeHuman (x, y, z) to Blender (-x, z, y): a proper rotation that turns the
# figure to face +Y with Z up.
_MH_TO_BLENDER = np.array([[-1.0, 0.0, 0.0],
                           [0.0, 0.0, 1.0],
                           [0.0, 1.0, 0.0]])


def to_blender(points_dm):
    return (np.asarray(points_dm, dtype=np.float64) @ _MH_TO_BLENDER.T) * 0.1


class BaseMesh:
    """All vertices of base.obj (body plus helpers) and the body's quads."""

    def __init__(self, path):
        vertices = []
        uvs = []
        body_faces = []
        body_face_uvs = []
        group = None
        with open(path, encoding="utf-8") as source:
            for line in source:
                if line.startswith("v "):
                    vertices.append([float(x) for x in line.split()[1:4]])
                elif line.startswith("vt "):
                    uvs.append([float(x) for x in line.split()[1:3]])
                elif line.startswith("g "):
                    group = line.split()[1]
                elif line.startswith("f ") and group == "body":
                    corners = [part.split("/") for part in line.split()[1:]]
                    body_faces.append([int(c[0]) - 1 for c in corners])
                    body_face_uvs.append([int(c[1]) - 1 for c in corners])
        self.vertices = np.array(vertices, dtype=np.float64)
        self.uvs = np.array(uvs, dtype=np.float64)
        self.body_faces = body_faces
        self.body_face_uvs = body_face_uvs
        if len(body_faces) != 13378 or max(max(f) for f in body_faces) + 1 != BODY_VERTEX_COUNT:
            raise ValueError("Unexpected MakeHuman body topology in " + str(path))


def read_target(path):
    """Sparse vertex offsets of one .target file, decimetres."""
    indices = []
    offsets = []
    with open(path, encoding="utf-8") as source:
        for line in source:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            indices.append(int(parts[0]))
            offsets.append([float(x) for x in parts[1:4]])
    # A target may list no offsets at all.
    return (np.array(indices, dtype=np.int64),
            np.array(offsets, dtype=np.float64).reshape(-1, 3))


def macro_targets(muscle, weight, height, proportions):
    """MakeHuman macro target weights for a 25-year-old male with an even race
    mix. Values follow MakeHuman's sliders: 0.5 is average; muscle, height and
    proportions reach their maximum at 1.0, and weight its minimum at 0.0."""
    if not (0.5 <= muscle <= 1.0 and 0.0 <= weight <= 0.5 and
            0.5 <= height <= 1.0 and 0.5 <= proportions <= 1.0):
        raise ValueError("Macro sliders outside the supported ranges")
    max_muscle = (muscle - 0.5) * 2.0
    min_weight = (0.5 - weight) * 2.0
    muscles = [("averagemuscle", 1.0 - max_muscle), ("maxmuscle", max_muscle)]
    weights = [("averageweight", 1.0 - min_weight), ("minweight", min_weight)]
    result = []
    for race in ("african", "asian", "caucasian"):
        result.append((f"macrodetails/{race}-male-young.target", 1.0 / 3.0))
    for muscle_name, muscle_share in muscles:
        for weight_name, weight_share in weights:
            share = muscle_share * weight_share
            if share <= 0.0:
                continue
            body = f"male-young-{muscle_name}-{weight_name}"
            result.append((f"macrodetails/universal-{body}.target", share))
            result.append((f"macrodetails/height/{body}-maxheight.target",
                           share * (height - 0.5) * 2.0))
            result.append((f"macrodetails/proportions/{body}-idealproportions.target",
                           share * (proportions - 0.5) * 2.0))
    return [(path, share) for path, share in result if share > 0.0]


def apply_targets(base, target_dir, weighted_targets):
    """Base vertices (body and helpers) with the weighted targets added."""
    vertices = base.vertices.copy()
    for relative, weight in weighted_targets:
        indices, offsets = read_target(Path(target_dir) / relative)
        vertices[indices] += offsets * weight
    return vertices


class Skeleton:
    """The default MakeHuman rig: bone heads and tails are the means of
    listed vertices, so joints follow every target."""

    def __init__(self, path):
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        self.bones = data["bones"]
        self.joints = {name: np.array(indices, dtype=np.int64)
                       for name, indices in data["joints"].items()}

    def joint(self, vertices, name):
        return vertices[self.joints[name]].mean(axis=0)

    def head(self, vertices, bone):
        return self.joint(vertices, self.bones[bone]["head"])

    def tail(self, vertices, bone):
        return self.joint(vertices, self.bones[bone]["tail"])


def read_weights(path):
    """Bone name to (vertex indices, weights) of the default weights file."""
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    result = {}
    for bone, pairs in data["weights"].items():
        if pairs:
            array = np.array(pairs, dtype=np.float64)
            result[bone] = (array[:, 0].astype(np.int64), array[:, 1])
    return result
