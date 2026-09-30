"""BVH motion capture parsing and forward kinematics (plain numpy).

Rotations are 3x3 matrices applied to column vectors; a joint's global
transform is its parent's global transform times its local offset and
rotation. Positions stay in the file's units and axes; the caller converts.
"""

from dataclasses import dataclass, field

import numpy as np


@dataclass
class Joint:
    name: str
    parent: int
    offset: np.ndarray
    channels: list = field(default_factory=list)
    end_site: np.ndarray = None


@dataclass
class Motion:
    joints: list
    frame_time: float
    # (frames, channel count) raw channel values.
    values: np.ndarray

    def index(self, name):
        for i, joint in enumerate(self.joints):
            if joint.name == name:
                return i
        raise KeyError(name)


def _axis_matrix(axis, degrees):
    """Rotation matrices (n, 3, 3) about a principal axis."""
    radians = np.radians(degrees)
    c = np.cos(radians)
    s = np.sin(radians)
    m = np.zeros((len(radians), 3, 3))
    if axis == "X":
        m[:, 0, 0] = 1.0
        m[:, 1, 1] = c
        m[:, 1, 2] = -s
        m[:, 2, 1] = s
        m[:, 2, 2] = c
    elif axis == "Y":
        m[:, 1, 1] = 1.0
        m[:, 0, 0] = c
        m[:, 0, 2] = s
        m[:, 2, 0] = -s
        m[:, 2, 2] = c
    else:
        m[:, 2, 2] = 1.0
        m[:, 0, 0] = c
        m[:, 0, 1] = -s
        m[:, 1, 0] = s
        m[:, 1, 1] = c
    return m


def read(path):
    with open(path, encoding="utf-8") as source:
        tokens = source.read().split()
    position = 0

    def take():
        nonlocal position
        position += 1
        return tokens[position - 1]

    if take() != "HIERARCHY":
        raise ValueError("Not a BVH file: " + str(path))
    joints = []
    stack = []
    while True:
        token = take()
        if token in ("ROOT", "JOINT"):
            name = take()
            if take() != "{":
                raise ValueError("Malformed joint " + name)
            if take() != "OFFSET":
                raise ValueError("Missing offset for " + name)
            offset = np.array([float(take()) for _ in range(3)])
            if take() != "CHANNELS":
                raise ValueError("Missing channels for " + name)
            channels = [take() for _ in range(int(take()))]
            joints.append(Joint(name, stack[-1] if stack else -1, offset, channels))
            stack.append(len(joints) - 1)
        elif token == "End":
            take()  # "Site"
            take()  # "{"
            take()  # "OFFSET"
            joints[stack[-1]].end_site = np.array([float(take()) for _ in range(3)])
            take()  # "}"
        elif token == "}":
            stack.pop()
        elif token == "MOTION":
            break
        else:
            raise ValueError("Unexpected BVH token " + token)
    take()  # "Frames:"
    frames = int(take())
    take()  # "Frame"
    take()  # "Time:"
    frame_time = float(take())
    width = sum(len(j.channels) for j in joints)
    values = np.array(tokens[position:position + frames * width], dtype=np.float64).reshape(frames, width)
    return Motion(joints, frame_time, values)


def local_transforms(motion, start=0, stop=None):
    """Per-joint local rotations (frames, joints, 3, 3) and the root's
    translation (frames, 3) for frames [start, stop)."""
    values = motion.values[start:stop]
    frames = len(values)
    rotations = np.tile(np.eye(3), (frames, len(motion.joints), 1, 1))
    root = np.zeros((frames, 3))
    column = 0
    for index, joint in enumerate(motion.joints):
        rotation = np.tile(np.eye(3), (frames, 1, 1))
        for channel in joint.channels:
            data = values[:, column]
            column += 1
            if channel.endswith("position"):
                root[:, "XYZ".index(channel[0])] = data
            else:
                # BVH lists rotations outermost first: R = R1 R2 R3.
                rotation = rotation @ _axis_matrix(channel[0], data)
        rotations[:, index] = rotation
    return rotations, root


def forward_kinematics(motion, rotations, root):
    """Global rotations (frames, joints, 3, 3) and positions (frames, joints, 3)."""
    frames, count = rotations.shape[:2]
    global_rotation = np.zeros_like(rotations)
    global_position = np.zeros((frames, count, 3))
    for index, joint in enumerate(motion.joints):
        if joint.parent < 0:
            global_rotation[:, index] = rotations[:, index]
            global_position[:, index] = root + joint.offset
        else:
            parent_rotation = global_rotation[:, joint.parent]
            global_rotation[:, index] = parent_rotation @ rotations[:, index]
            global_position[:, index] = (global_position[:, joint.parent] +
                                         np.einsum("fij,j->fi", parent_rotation, joint.offset))
    return global_rotation, global_position


def rest_positions(motion):
    """Joint positions with every rotation zero (the file's reference pose)."""
    positions = np.zeros((len(motion.joints), 3))
    for index, joint in enumerate(motion.joints):
        positions[index] = joint.offset if joint.parent < 0 else positions[joint.parent] + joint.offset
    return positions
