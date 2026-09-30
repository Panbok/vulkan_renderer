"""The mannequin skeleton and skin weights.

Bone names follow the UE5 mannequin convention (root, pelvis, spine_01..05,
neck_01/02, head, clavicle/upperarm/lowerarm/hand with twist bones, full
fingers with metacarpals, thigh/calf/foot/ball with twist bones and the ik_*
virtual bones), so retargeting tools map it without manual work. Joint
placement comes from the MakeHuman joint helpers of the target-applied body,
and weights from MakeHuman's CC0 default weights. Plain numpy.
"""

from dataclasses import dataclass

import numpy as np

import mannequin_body as mb
import mannequin_makehuman as mh


MAX_INFLUENCES = 4

SIDES = (("l", ".L"), ("r", ".R"))

FINGERS = (("thumb", 1, False), ("index", 2, True), ("middle", 3, True),
           ("ring", 4, True), ("pinky", 5, True))


@dataclass
class Bone:
    name: str
    parent: str
    head: np.ndarray
    tail: np.ndarray
    deform: bool = True


def _bone_specs():
    """(name, parent, MakeHuman head bone, MakeHuman tail source) in parent-
    first order. A tail source is a bone whose head ends this bone, or None
    to extend along the parent."""
    specs = [
        ("pelvis", "root", "root", "spine05"),
        ("spine_01", "pelvis", "spine05", "spine04"),
        ("spine_02", "spine_01", "spine04", "spine03"),
        ("spine_03", "spine_02", "spine03", "spine02"),
        ("spine_04", "spine_03", "spine02", "spine01"),
        ("spine_05", "spine_04", "spine01", "neck01"),
        ("neck_01", "spine_05", "neck01", "neck02"),
        ("neck_02", "neck_01", "neck02", "head"),
        ("head", "neck_02", "head", None),
    ]
    for side, mh_side in SIDES:
        specs += [
            (f"clavicle_{side}", "spine_05", "clavicle" + mh_side, "upperarm01" + mh_side),
            (f"upperarm_{side}", f"clavicle_{side}", "upperarm01" + mh_side, "lowerarm01" + mh_side),
            (f"upperarm_twist_01_{side}", f"upperarm_{side}", "upperarm02" + mh_side, "lowerarm01" + mh_side),
            (f"lowerarm_{side}", f"upperarm_{side}", "lowerarm01" + mh_side, "wrist" + mh_side),
            (f"lowerarm_twist_01_{side}", f"lowerarm_{side}", "lowerarm02" + mh_side, "wrist" + mh_side),
            (f"hand_{side}", f"lowerarm_{side}", "wrist" + mh_side, "finger3-1" + mh_side),
        ]
        for finger, digit, metacarpal in FINGERS:
            parent = f"hand_{side}"
            if metacarpal:
                name = f"{finger}_metacarpal_{side}"
                specs.append((name, parent, f"metacarpal{digit - 1}{mh_side}", f"finger{digit}-1{mh_side}"))
                parent = name
            for segment in (1, 2, 3):
                name = f"{finger}_0{segment}_{side}"
                tail = f"finger{digit}-{segment + 1}{mh_side}" if segment < 3 else None
                specs.append((name, parent, f"finger{digit}-{segment}{mh_side}", tail))
                parent = name
        specs += [
            (f"thigh_{side}", "pelvis", "upperleg01" + mh_side, "lowerleg01" + mh_side),
            (f"thigh_twist_01_{side}", f"thigh_{side}", "upperleg02" + mh_side, "lowerleg01" + mh_side),
            (f"calf_{side}", f"thigh_{side}", "lowerleg01" + mh_side, "foot" + mh_side),
            (f"calf_twist_01_{side}", f"calf_{side}", "lowerleg02" + mh_side, "foot" + mh_side),
            (f"foot_{side}", f"calf_{side}", "foot" + mh_side, None),
        ]
    return specs


# MakeHuman default-rig bone to mannequin bone (weight shares).
def _weight_map():
    mapping = {"root": {"pelvis": 1.0}, "pelvis.L": {"pelvis": 1.0}, "pelvis.R": {"pelvis": 1.0},
               "spine05": {"spine_01": 1.0}, "spine04": {"spine_02": 1.0},
               "spine03": {"spine_03": 1.0}, "spine02": {"spine_04": 1.0},
               "spine01": {"spine_05": 1.0}, "breast.L": {"spine_04": 1.0},
               "breast.R": {"spine_04": 1.0}, "neck01": {"neck_01": 1.0},
               "neck02": {"neck_02": 1.0}, "neck03": {"neck_02": 0.5, "head": 0.5}}
    for side, mh_side in SIDES:
        mapping.update({
            "clavicle" + mh_side: {f"clavicle_{side}": 1.0},
            # The deltoid cap shares clavicle and arm so a raised arm neither
            # collapses the shoulder nor drags the trapezius.
            "shoulder01" + mh_side: {f"clavicle_{side}": 0.45, f"upperarm_{side}": 0.55},
            "upperarm01" + mh_side: {f"upperarm_{side}": 1.0},
            "upperarm02" + mh_side: {f"upperarm_twist_01_{side}": 1.0},
            "lowerarm01" + mh_side: {f"lowerarm_{side}": 1.0},
            "lowerarm02" + mh_side: {f"lowerarm_twist_01_{side}": 1.0},
            "wrist" + mh_side: {f"hand_{side}": 1.0},
            "upperleg01" + mh_side: {f"thigh_{side}": 1.0},
            "upperleg02" + mh_side: {f"thigh_twist_01_{side}": 1.0},
            "lowerleg01" + mh_side: {f"calf_{side}": 1.0},
            "lowerleg02" + mh_side: {f"calf_twist_01_{side}": 1.0},
            "foot" + mh_side: {f"foot_{side}": 1.0},
        })
        for finger, digit, metacarpal in FINGERS:
            if metacarpal:
                mapping[f"metacarpal{digit - 1}{mh_side}"] = {f"{finger}_metacarpal_{side}": 1.0}
            for segment in (1, 2, 3):
                mapping[f"finger{digit}-{segment}{mh_side}"] = {f"{finger}_0{segment}_{side}": 1.0}
        for toe in range(1, 6):
            for segment in range(1, 4):
                mapping[f"toe{toe}-{segment}{mh_side}"] = {f"ball_{side}": 1.0}
    return mapping


class Rig:
    def __init__(self, body, skeleton):
        positions = body.makehuman_positions
        bones = {"root": Bone("root", None, np.zeros(3), np.array([0.0, 0.25, 0.0]))}
        order = ["root"]
        for name, parent, mh_head, mh_tail in _bone_specs():
            head = skeleton.head(positions, mh_head)
            if mh_tail is not None:
                tail = skeleton.head(positions, mh_tail)
            else:
                tail = skeleton.tail(positions, mh_head)
            bones[name] = Bone(name, parent, head, tail)
            order.append(name)
        for side, mh_side in SIDES:
            # The ball joint sits over the toe joints at the boot's forefoot.
            toes = np.mean([skeleton.head(positions, f"toe{t}-1{mh_side}") for t in range(1, 6)], axis=0)
            foot = bones[f"foot_{side}"]
            ball_head = np.array([toes[0], toes[1], 0.035])
            foot.tail = ball_head.copy()
            tip = skeleton.tail(positions, f"toe3-3{mh_side}")
            bones[f"ball_{side}"] = Bone(f"ball_{side}", f"foot_{side}", ball_head,
                                         np.array([tip[0], tip[1] + 0.02, 0.035]))
            order.append(f"ball_{side}")
        # Head: its tail at the crown for a stable orientation.
        bones["head"].tail = np.array([0.0, bones["head"].head[1], mb.STATURE])
        # UE-style virtual bones for IK targets; they carry no weights.
        virtual = [("ik_foot_root", "root", np.zeros(3)),
                   ("ik_foot_l", "ik_foot_root", bones["foot_l"].head),
                   ("ik_foot_r", "ik_foot_root", bones["foot_r"].head),
                   ("ik_hand_root", "root", np.zeros(3)),
                   ("ik_hand_gun", "ik_hand_root", bones["hand_r"].head),
                   ("ik_hand_l", "ik_hand_gun", bones["hand_l"].head),
                   ("ik_hand_r", "ik_hand_gun", bones["hand_r"].head)]
        for name, parent, head in virtual:
            bones[name] = Bone(name, parent, np.array(head, dtype=np.float64),
                               np.array(head, dtype=np.float64) + np.array([0.0, 0.0, 0.1]), deform=False)
            order.append(name)
        bones["root"].deform = False
        self.bones = bones
        self.order = order

    def deform_names(self):
        return [name for name in self.order if self.bones[name].deform]


def skin_weights(body, rig, weights_path):
    """Per vertex of `body`: up to MAX_INFLUENCES (bone, weight) pairs,
    normalized."""
    source = mh.read_weights(weights_path)
    mapping = _weight_map()
    names = rig.deform_names()
    column = {name: i for i, name in enumerate(names)}
    table = np.zeros((mh.BODY_VERTEX_COUNT, len(names)))
    for mh_bone, (indices, values) in source.items():
        # Helper geometry (tights, skirt, hair) follows the body indices.
        body_only = indices < mh.BODY_VERTEX_COUNT
        indices = indices[body_only]
        values = values[body_only]
        # Unlisted source bones are the face rig: all of it rides the head.
        for bone, share in mapping.get(mh_bone, {"head": 1.0}).items():
            np.add.at(table[:, column[bone]], indices, values * share)

    count = len(body.positions)
    dense = np.zeros((count, len(names)))
    kept = body.source >= 0
    dense[kept] = table[body.source[kept]]

    # New parts start from their loop's weights and hand over to the part's
    # own bones within the first rings.
    for key, (index, loop_count) in body.part_uvs.items():
        loop = index[:loop_count]
        loop_positions = body.positions[loop]
        new = index[loop_count:]
        nearest = np.argmin(np.linalg.norm(body.positions[new][:, None] - loop_positions[None], axis=2), axis=1)
        inherited = dense[loop[nearest]]
        own = np.zeros((len(new), len(names)))
        if key == "head":
            own[:, column["head"]] = 1.0
        else:
            side = key[-1]
            ball = rig.bones[f"ball_{side}"].head
            foot = rig.bones[f"foot_{side}"].head
            axis = ball - foot
            axis[2] = 0.0
            axis /= np.linalg.norm(axis)
            along = (body.positions[new] - ball) @ axis
            share = np.clip((along + 0.012) / 0.024, 0.0, 1.0)
            share = share * share * (3.0 - 2.0 * share)
            own[:, column[f"ball_{side}"]] = share
            own[:, column[f"foot_{side}"]] = 1.0 - share
        blend = np.clip(body.ring_parameter[new] / 0.22, 0.0, 1.0)
        blend = blend * blend * (3.0 - 2.0 * blend)
        dense[new] = inherited * (1.0 - blend[:, None]) + own * blend[:, None]

    order = np.argsort(-dense, axis=1)[:, :MAX_INFLUENCES]
    top = np.take_along_axis(dense, order, axis=1)
    total = top.sum(axis=1, keepdims=True)
    if np.any(total <= 0.0):
        raise ValueError("Vertices without skin weights: " + str(np.flatnonzero(total[:, 0] <= 0.0)[:10]))
    top /= total
    return names, order, top
