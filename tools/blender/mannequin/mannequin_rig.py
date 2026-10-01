"""The mannequin skeleton.

Bone names follow the UE5 mannequin convention (root, pelvis, spine_01..05,
neck_01/02, head, clavicle/upperarm/lowerarm/hand with twist bones, full
fingers with metacarpals, thigh/calf/foot/ball with twist bones and the ik_*
virtual bones), so retargeting tools map it without manual work.

Joints come from the base body: limb and finger joints are the centres of
the rings where the bundle's sculpt face sets meet (upper arm and forearm at
the elbow, each phalanx at its knuckle); the spine, neck and hips follow
proportions of the torso's sections. Plain numpy; skin weights are bone heat
in Blender (mannequin_blender.skin).
"""

from dataclasses import dataclass

import numpy as np


SIDES = ("l", "r")

# Face sets of the base body on the character's right (+X); the left side
# uses the mirrored sets below.
RIGHT_SETS = {"torso": 1, "upperarm": 20, "forearm": 11, "hand": 10,
              "thigh": 23, "shin": 16, "pelvis": 18}
LEFT_SETS = {"torso": 1, "upperarm": 21, "forearm": 12, "hand": 9,
             "thigh": 24, "shin": 15, "pelvis": 18}
# Finger chains from the palm outwards: segment sets, then the nail.
RIGHT_FINGERS = {"thumb": (84, 85, 86, 87), "index": (88, 89, 90, 91), "middle": (92, 93, 94, 95),
                 "ring": (96, 97, 98, 99), "pinky": (100, 101, 102, 103)}
LEFT_FINGERS = {"thumb": (80, 81, 82, 83), "index": (76, 77, 78, 79), "middle": (72, 73, 74, 75),
                "ring": (68, 69, 70, 71), "pinky": (64, 65, 66, 67)}

# Joint heights of the spine chain (metres, 1.80 m stature).
SPINE = (("pelvis", 0.960), ("spine_01", 1.005), ("spine_02", 1.085), ("spine_03", 1.165),
         ("spine_04", 1.250), ("spine_05", 1.340), ("neck_01", 1.455), ("neck_02", 1.525),
         ("head", 1.605))


@dataclass
class Bone:
    name: str
    parent: str
    head: np.ndarray
    tail: np.ndarray
    deform: bool = True


class Joints:
    """Ring centres where the base body's face sets meet."""

    def __init__(self, body):
        self.positions = body.positions
        self.members = {}
        for face, face_set in zip(body.faces, body.face_sets):
            self.members.setdefault(int(face_set), set()).update(face)

    def ring(self, a, b):
        shared = sorted(self.members[a] & self.members[b])
        if not shared:
            raise ValueError(f"Face sets {a} and {b} do not meet")
        return self.positions[shared].mean(axis=0)

    def centroid(self, face_set):
        return self.positions[sorted(self.members[face_set])].mean(axis=0)


def _section(positions, z, half_width=0.05):
    """Front and back of the torso's midline at height z."""
    band = positions[(np.abs(positions[:, 2] - z) < 0.006) & (np.abs(positions[:, 0]) < half_width)]
    return band[:, 1].max(), band[:, 1].min()


class Rig:
    def __init__(self, body):
        joints = Joints(body)
        p = body.positions
        bones = {"root": Bone("root", None, np.zeros(3), np.array([0.0, 0.25, 0.0]), deform=False)}
        order = ["root"]

        def add(name, parent, head, tail, deform=True):
            bones[name] = Bone(name, parent, np.asarray(head, dtype=np.float64),
                               np.asarray(tail, dtype=np.float64), deform)
            order.append(name)

        # Spine: a third of the torso's depth in from the back.
        chain = []
        for name, z in SPINE:
            if z < 1.45:
                front, back = _section(p, z, 0.05)
                y = back + 0.36 * (front - back)
            else:
                # The neck and head joints sit on the neck column's axis,
                # the head's a little forward of it, under the helmet.
                front, back = _section(p, min(z, 1.52), 0.04)
                y = back + 0.5 * (front - back) + (0.008 if name == "head" else 0.0)
            chain.append((name, np.array([0.0, y, z])))
        hips = {side: self._hip(joints, side) for side in SIDES}
        chain[0] = ("pelvis", np.array([0.0, 0.5 * (hips["l"][1] + hips["r"][1]), SPINE[0][1]]))
        for i, (name, head) in enumerate(chain):
            parent = "root" if i == 0 else chain[i - 1][0]
            tail = chain[i + 1][1] if i + 1 < len(chain) else np.array([0.0, head[1], p[:, 2].max()])
            add(name, parent, head, tail)

        for side in SIDES:
            sets = LEFT_SETS if side == "l" else RIGHT_SETS
            fingers = LEFT_FINGERS if side == "l" else RIGHT_FINGERS
            sign = -1.0 if side == "l" else 1.0
            shoulder = joints.ring(sets["torso"], sets["upperarm"]) + np.array([sign * 0.012, 0.0, 0.010])
            elbow = joints.ring(sets["upperarm"], sets["forearm"])
            wrist = joints.ring(sets["forearm"], sets["hand"])
            sternal = np.array([sign * 0.020, bones["spine_05"].head[1] + 0.035, 1.445])
            add(f"clavicle_{side}", "spine_05", sternal, shoulder)
            add(f"upperarm_{side}", f"clavicle_{side}", shoulder, elbow)
            add(f"upperarm_twist_01_{side}", f"upperarm_{side}", 0.5 * (shoulder + elbow), elbow)
            add(f"lowerarm_{side}", f"upperarm_{side}", elbow, wrist)
            add(f"lowerarm_twist_01_{side}", f"lowerarm_{side}", 0.5 * (elbow + wrist), wrist)
            knuckles = {name: joints.ring(sets["hand"], chain_sets[0]) for name, chain_sets in fingers.items()}
            add(f"hand_{side}", f"lowerarm_{side}", wrist, knuckles["middle"])
            for name, chain_sets in fingers.items():
                s1, s2, s3, nail = chain_sets
                tip = joints.ring(s3, nail)
                tip = tip + (tip - joints.ring(s2, s3)) * 0.35
                if name == "thumb":
                    heads = [wrist + 0.40 * (knuckles["thumb"] - wrist), joints.ring(s1, s2), joints.ring(s2, s3)]
                    parent = f"hand_{side}"
                else:
                    metacarpal = wrist + 0.28 * (knuckles[name] - wrist)
                    add(f"{name}_metacarpal_{side}", f"hand_{side}", metacarpal, knuckles[name])
                    heads = [knuckles[name], joints.ring(s1, s2), joints.ring(s2, s3)]
                    parent = f"{name}_metacarpal_{side}"
                ends = heads[1:] + [tip]
                for segment, (head, tail) in enumerate(zip(heads, ends), start=1):
                    bone = f"{name}_0{segment}_{side}"
                    add(bone, parent, head, tail)
                    parent = bone

            hip = hips[side]
            knee = joints.ring(sets["thigh"], sets["shin"]) + np.array([0.0, 0.0, 0.008])
            frame, ankle_center = body.feet[side]
            ankle = ankle_center + np.array([0.0, 0.004, -0.016])
            forward = frame[:, 1]
            ball = np.array([ankle[0], ankle[1], 0.0]) + forward * 0.135 + np.array([0.0, 0.0, 0.032])
            toe = np.array([ankle[0], ankle[1], 0.0]) + forward * 0.205 + np.array([0.0, 0.0, 0.032])
            add(f"thigh_{side}", "pelvis", hip, knee)
            add(f"thigh_twist_01_{side}", f"thigh_{side}", 0.5 * (hip + knee), knee)
            add(f"calf_{side}", f"thigh_{side}", knee, ankle)
            add(f"calf_twist_01_{side}", f"calf_{side}", 0.5 * (knee + ankle), ankle)
            add(f"foot_{side}", f"calf_{side}", ankle, ball)
            add(f"ball_{side}", f"foot_{side}", ball, toe)

        # UE-style virtual bones for IK targets; they carry no weights.
        virtual = [("ik_foot_root", "root", np.zeros(3)),
                   ("ik_foot_l", "ik_foot_root", bones["foot_l"].head),
                   ("ik_foot_r", "ik_foot_root", bones["foot_r"].head),
                   ("ik_hand_root", "root", np.zeros(3)),
                   ("ik_hand_gun", "ik_hand_root", bones["hand_r"].head),
                   ("ik_hand_l", "ik_hand_gun", bones["hand_l"].head),
                   ("ik_hand_r", "ik_hand_gun", bones["hand_r"].head)]
        for name, parent, head in virtual:
            add(name, parent, head, np.asarray(head) + np.array([0.0, 0.0, 0.1]), deform=False)
        self.bones = bones
        self.order = order

    @staticmethod
    def _hip(joints, side):
        """The hip joint: the thigh ring's centre is pulled down by the groin,
        so the joint sits at the pelvis ring's lateral third, above it."""
        sets = LEFT_SETS if side == "l" else RIGHT_SETS
        ring = joints.ring(sets["pelvis"], sets["thigh"])
        return np.array([ring[0] * 0.96, ring[1], 0.915])

    def deform_names(self):
        return [name for name in self.order if self.bones[name].deform]
