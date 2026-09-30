"""CMU Graphics Lab ASF/AMC motion capture reading (plain numpy).

ASF describes each segment by a rest direction, length, local axis and
degrees of freedom; AMC holds per-frame degrees. A segment's global rotation
is its parent's times C R(frame) C^-1, where C comes from the segment's axis
and R is built from the frame's angles (static X, then Y, then Z); the
segment ends at its start plus length times the rotated direction.

`load_take` returns the same Take structure as the BVH path, keyed by the
joint names the retargeter maps (Hips, Chest..Chest4, Neck, Head and the
Left/Right Collar, Shoulder, Elbow, Wrist, Hip, Knee, Ankle and Toe joints).
A joint's rotation is the rotation of the segment starting there.
"""

from dataclasses import dataclass, field

import numpy as np


# CMU length units are 1/0.45 inch.
_LENGTH_TO_METRES = 0.0254 / 0.45

# CMU (Y up, arbitrary facing) to the mannequin frame is decided per take by
# its heading; axes map as for the other sources: (-x, z, y).
_TO_FRAME = np.array([[-1.0, 0.0, 0.0],
                      [0.0, 0.0, 1.0],
                      [0.0, 1.0, 0.0]])

# Segment whose start is each retargeted joint.
JOINT_SEGMENTS = {
    "Hips": "root",
    "Chest": "lowerback",
    "Chest2": "upperback",
    "Chest3": "thorax",
    "Chest4": "thorax",
    "Neck": "lowerneck",
    "Head": "head",
}
for _side, _cmu in (("Left", "l"), ("Right", "r")):
    JOINT_SEGMENTS.update({
        _side + "Collar": _cmu + "clavicle",
        _side + "Shoulder": _cmu + "humerus",
        _side + "Elbow": _cmu + "radius",
        _side + "Wrist": _cmu + "wrist",
        _side + "Hip": _cmu + "femur",
        _side + "Knee": _cmu + "tibia",
        _side + "Ankle": _cmu + "foot",
        _side + "Toe": _cmu + "toes",
    })


@dataclass
class Segment:
    name: str
    direction: np.ndarray
    length: float
    axis: np.ndarray
    dof: list
    parent: str = None
    children: list = field(default_factory=list)


def _euler_xyz(angles):
    """Rz Ry Rx for (n, 3) degrees: X applied first in the static frame."""
    radians = np.radians(angles)
    cx, cy, cz = np.cos(radians).T
    sx, sy, sz = np.sin(radians).T
    m = np.zeros((len(radians), 3, 3))
    m[:, 0, 0] = cy * cz
    m[:, 0, 1] = sx * sy * cz - cx * sz
    m[:, 0, 2] = cx * sy * cz + sx * sz
    m[:, 1, 0] = cy * sz
    m[:, 1, 1] = sx * sy * sz + cx * cz
    m[:, 1, 2] = cx * sy * sz - sx * cz
    m[:, 2, 0] = -sy
    m[:, 2, 1] = sx * cy
    m[:, 2, 2] = cx * cy
    return m


def read_asf(path):
    segments = {"root": Segment("root", np.zeros(3), 0.0, np.zeros(3), ["tx", "ty", "tz", "rx", "ry", "rz"])}
    lines = [line.strip() for line in open(path, encoding="latin-1")]
    index = 0
    while index < len(lines):
        line = lines[index]
        if line.startswith(":root"):
            index += 1
            while not lines[index].startswith(":"):
                parts = lines[index].split()
                if parts and parts[0] == "axis" and len(parts) == 2 and parts[1] != "XYZ":
                    raise ValueError("Unsupported root axis order " + parts[1])
                if parts and parts[0] == "orientation":
                    segments["root"].axis = np.array([float(v) for v in parts[1:4]])
                index += 1
            continue
        if line == "begin":
            data = {}
            index += 1
            while lines[index] != "end":
                parts = lines[index].split()
                if parts[0] == "limits":
                    pass
                elif parts[0] == "axis":
                    if parts[4] != "XYZ":
                        raise ValueError("Unsupported axis order " + parts[4])
                    data["axis"] = np.array([float(v) for v in parts[1:4]])
                elif parts[0] == "direction":
                    data["direction"] = np.array([float(v) for v in parts[1:4]])
                elif parts[0] in ("name", "length"):
                    data[parts[0]] = parts[1]
                elif parts[0] == "dof":
                    data["dof"] = parts[1:]
                index += 1
            name = data["name"]
            segments[name] = Segment(name, data["direction"], float(data["length"]) * _LENGTH_TO_METRES,
                                     data.get("axis", np.zeros(3)), data.get("dof", []))
            index += 1
            continue
        if line.startswith(":hierarchy"):
            index += 1
            while index < len(lines) and lines[index] != "end":
                parts = lines[index].split()
                if parts and parts[0] != "begin":
                    for child in parts[1:]:
                        segments[child].parent = parts[0]
                        segments[parts[0]].children.append(child)
                index += 1
            break
        index += 1
    return segments


def read_amc(path, segments):
    """Per-segment (frames, dof) angle arrays and the root translation."""
    frames = []
    current = None
    for line in open(path, encoding="latin-1"):
        line = line.strip()
        if not line or line.startswith("#") or line.startswith(":"):
            continue
        parts = line.split()
        if len(parts) == 1 and parts[0].isdigit():
            current = {}
            frames.append(current)
            continue
        current[parts[0]] = [float(v) for v in parts[1:]]
    values = {}
    for name, segment in segments.items():
        width = len(segment.dof)
        values[name] = np.array([frame.get(name, [0.0] * width) for frame in frames]) if width else None
    return values, len(frames)


def _segment_order(segments):
    order = []
    stack = ["root"]
    while stack:
        name = stack.pop()
        order.append(name)
        stack.extend(reversed(segments[name].children))
    return order


def global_segments(segments, values, frames):
    """Global rotations (frames, 3, 3) and start/end positions per segment,
    in the file's axes and metres."""
    rotation = {}
    start = {}
    end = {}
    for name in _segment_order(segments):
        segment = segments[name]
        c = _euler_xyz(segment.axis[None])[0]
        angles = np.zeros((frames, 3))
        if segment.dof:
            data = values[name]
            for column, dof in enumerate(segment.dof):
                if dof.startswith("r"):
                    angles[:, "xyz".index(dof[1])] = data[:, column]
        local = c @ _euler_xyz(angles) @ c.T
        if name == "root":
            translation = np.zeros((frames, 3))
            data = values["root"]
            for column, dof in enumerate(segment.dof):
                if dof.startswith("t"):
                    translation[:, "xyz".index(dof[1])] = data[:, column]
            rotation[name] = local
            start[name] = translation * _LENGTH_TO_METRES
            end[name] = start[name]
        else:
            parent = segment.parent
            rotation[name] = rotation[parent] @ local
            start[name] = end[parent]
            end[name] = start[name] + segment.length * np.einsum("fij,j->fi", rotation[name], segment.direction)
    return rotation, start, end


def load_take(asf_path, amc_path, start=0, stop=None, fps=120.0, resample=60.0):
    """A Take in the mannequin frame. CMU captures at 120 fps; frames are
    resampled to `resample` fps by picking every other frame."""
    from mannequin_motion import Take
    segments = read_asf(asf_path)
    values, frames = read_amc(amc_path, segments)
    rotation, seg_start, seg_end = global_segments(segments, values, frames)
    step = max(1, int(round(fps / resample)))
    picks = np.arange(frames)[start:stop:step]
    names = list(JOINT_SEGMENTS)
    parents = []
    joint_parent = {"Hips": None, "Chest": "Hips", "Chest2": "Chest", "Chest3": "Chest2", "Chest4": "Chest3",
                    "Neck": "Chest4", "Head": "Neck"}
    for side in ("Left", "Right"):
        joint_parent.update({side + "Collar": "Chest4", side + "Shoulder": side + "Collar",
                             side + "Elbow": side + "Shoulder", side + "Wrist": side + "Elbow",
                             side + "Hip": "Hips", side + "Knee": side + "Hip",
                             side + "Ankle": side + "Knee", side + "Toe": side + "Ankle"})
    for name in names:
        parents.append(names.index(joint_parent[name]) if joint_parent[name] else -1)
    frame_rotation = np.stack([_TO_FRAME @ rotation[JOINT_SEGMENTS[n]][picks] @ _TO_FRAME.T for n in names], axis=1)
    # Chest4 is a second name for the thorax: it starts where the thorax ends
    # so the collars attach at the right height.
    positions = []
    rest = []
    rest_segments = global_segments(segments, {k: (None if v is None else np.zeros_like(v[:1]))
                                               for k, v in values.items()}, 1)
    for name in names:
        segment = JOINT_SEGMENTS[name]
        use_end = name == "Chest4"
        source = seg_end if use_end else seg_start
        rest_source = rest_segments[2] if use_end else rest_segments[1]
        positions.append(source[segment][picks] @ _TO_FRAME.T)
        rest.append(rest_source[segment][0] @ _TO_FRAME.T)
    end_sites = {}
    for side, cmu in (("Left", "l"), ("Right", "r")):
        toes = segments[cmu + "toes"]
        end_sites[side + "Toe"] = (toes.direction * toes.length) @ _TO_FRAME.T
        hand = segments[cmu + "hand"]
        wrist = segments[cmu + "wrist"]
        end_sites[side + "Wrist"] = (wrist.direction * wrist.length + hand.direction * hand.length) @ _TO_FRAME.T
    head = segments["head"]
    end_sites["Head"] = (head.direction * head.length) @ _TO_FRAME.T
    from mannequin_motion import stand_on_floor
    return stand_on_floor(Take(names, parents, np.array(rest), end_sites, frame_rotation,
                               np.stack(positions, axis=1), resample))
