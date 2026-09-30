"""The mannequin's clip set: which motion capture window becomes which named
clip, and the Blender actions that carry them to glTF.

Clip names follow the engine's player controller and UE conventions
(Idle, Walk_Fwd, Jog_Fwd, ...). Each loop's playback speed in metres per
second is recorded so controllers can match stride to movement.
"""

from dataclasses import dataclass
from pathlib import Path

import bpy
import numpy as np

import mannequin_motion as mm


@dataclass
class ClipSpec:
    name: str
    # A 100STYLE file stem such as Neutral_FW, or CMU trials as
    # "cmu:<subject>:<trial>,<trial>..." whose best cycle wins.
    take: str
    kind: str               # "cycle", "idle" or "jump_start", "jump_loop", "jump_land"
    direction: float = 0.0  # travel relative to facing: 0 forward, pi back, +pi/2 left
    cycles: int = 1
    seconds: float = 0.0    # idle length
    hands: str = "relaxed"  # mannequin_motion.HAND_POSES


CLIPS = [
    ClipSpec("Idle", "Neutral_ID", "idle", seconds=6.0),
    # Forward walk and jog at game pace (100STYLE's are 0.85 and 1.86 m/s).
    ClipSpec("Walk_Fwd", "cmu:35:" + ",".join(f"35_{i:02d}" for i in range(1, 17)), "cycle", 0.0, 1),
    ClipSpec("Walk_Bwd", "Neutral_BW", "cycle", np.pi, 1),
    ClipSpec("Walk_Left", "Neutral_SW", "cycle", np.pi / 2.0, 1),
    ClipSpec("Walk_Right", "Neutral_SW", "cycle", -np.pi / 2.0, 1),
    # The easier CMU 35 jogs; its faster ones nearly match the run's speed.
    ClipSpec("Jog_Fwd", "cmu:35:35_17,35_18", "cycle", 0.0, 1, hands="fist"),
    ClipSpec("Jog_Bwd", "Neutral_BR", "cycle", np.pi, 1, hands="fist"),
    ClipSpec("Jog_Left", "Neutral_SR", "cycle", np.pi / 2.0, 1, hands="fist"),
    ClipSpec("Jog_Right", "Neutral_SR", "cycle", -np.pi / 2.0, 1, hands="fist"),
    ClipSpec("Run_Fwd", "cmu:09:09_01,09_02,09_03,09_04,09_05,09_06,09_07,09_08,09_09,09_10,09_11", "cycle", 0.0, 1,
             hands="fist"),
    ClipSpec("Jump_Start", "cmu:13:13_41", "jump_start", seconds=0.45),
    ClipSpec("Jump_Loop", "cmu:13:13_41", "jump_loop", seconds=0.5),
    ClipSpec("Jump_Land", "cmu:13:13_41", "jump_land", seconds=0.75),
    ClipSpec("Crouch_Idle", "Crouched_ID", "idle", seconds=5.0),
    ClipSpec("Crouch_Walk_Fwd", "Crouched_FW", "cycle", 0.0, 1),
    ClipSpec("Crouch_Walk_Bwd", "Crouched_BW", "cycle", np.pi, 1),
    ClipSpec("Crouch_Walk_Left", "Crouched_SW", "cycle", np.pi / 2.0, 1),
    ClipSpec("Crouch_Walk_Right", "Crouched_SW", "cycle", -np.pi / 2.0, 1),
]


def target_from_armature(armature_obj):
    bones = armature_obj.data.bones
    names = [bone.name for bone in bones]
    parents = [names.index(bone.parent.name) if bone.parent else -1 for bone in bones]
    rest = np.array([np.array(bone.matrix_local) for bone in bones])
    lengths = np.array([bone.length for bone in bones])
    return mm.Target(names, parents, rest, lengths)


def hip_scale(target, take):
    """Mannequin hip-centre height over the take's standing hip height."""
    hips = 0.5 * (take.rest[take.joint("LeftHip"), 2] + take.rest[take.joint("RightHip"), 2])
    sole = take.rest[take.joint("LeftToe"), 2] - mm._ball_height(take)
    center = 0.5 * (target.head("thigh_l") + target.head("thigh_r"))
    return center[2] / (hips - sole)


_REFERENCE = {}


def actor_reference(mocap_dir, frame_cuts):
    """The 100STYLE actor's neutral idle posture, shared by every take."""
    key = str(mocap_dir)
    if key not in _REFERENCE:
        start, stop = frame_cuts["Neutral_ID"]
        _REFERENCE[key] = mm.axial_reference(mm.load_take(Path(mocap_dir) / "Neutral_ID.bvh", start, stop))
    return _REFERENCE[key]


def _cmu_candidates(spec, cmu_dir):
    """(take, trial) pairs of a CMU clip spec, trimmed to straight travel."""
    import mannequin_amc as amc
    _, subject, trials = spec.take.split(":")
    for trial in trials.split(","):
        yield amc.load_take(Path(cmu_dir) / f"{subject}.asf", Path(cmu_dir) / f"{trial}.amc"), trial


def _build_jump(spec, take, target, trial):
    """Jump one-shots and the airborne loop from one standing jump."""
    takeoff, touchdown = mm.flight(take)
    take = mm.remove_flight_arc(take, takeoff, touchdown)
    frames = int(round(spec.seconds * take.fps))
    reference = np.tile(np.eye(3), (len(take.names), 1, 1))
    if spec.kind == "jump_start":
        # From the last push-off frames into the air; no anticipation, so it
        # can start the moment the capsule leaves the ground. The frame before
        # these has the leg at full extension, beyond what the feet can hold.
        a = takeoff - 2
        b = min(a + frames, touchdown - 4)
        clip = mm.one_shot(take, a, b)
    elif spec.kind == "jump_land":
        a = touchdown - 6
        b = min(a + frames, take.frames)
        clip = mm.one_shot(take, a, b)
    else:
        apex = (takeoff + touchdown) // 2
        half = frames // 2
        a, b = apex - half, apex + half
        clip = mm.canonical_loop(take, a, b, np.array([0.0, 1.0]), in_place=True)
        clip.speed = 0.0
    window = np.arange(a, a + clip.take.frames)
    clip.airborne = (window >= takeoff) & (window < touchdown)
    return _finish(spec, take, clip, a, b, 0, target, reference, trial)


def build_clip(spec, mocap_dir, frame_cuts, target, cmu_dir=None):
    if spec.kind.startswith("jump"):
        take, trial = next(_cmu_candidates(spec, cmu_dir))
        return _build_jump(spec, take, target, trial)
    if spec.take.startswith("cmu:"):
        # CMU heads and necks sit near neutral in the file's T-pose.
        best = None
        for take, trial in _cmu_candidates(spec, cmu_dir):
            try:
                a, b = mm.select_cycle(take, spec.cycles, spec.direction)
            except ValueError:
                continue
            score = mm._pose_distance(take, a, b)
            if best is None or score < best[0]:
                best = (score, take, a, b, trial)
        if best is None:
            raise ValueError(f"No usable cycle for {spec.name} in {spec.take}")
        _, take, a, b, trial = best
        reference = np.tile(np.eye(3), (len(take.names), 1, 1))
        return _finish_cycle(spec, take, a, b, 0, target, reference, trial)
    reference = actor_reference(mocap_dir, frame_cuts)
    start, stop = frame_cuts[spec.take]
    take = mm.load_take(Path(mocap_dir) / (spec.take + ".bvh"), start, stop)
    if spec.kind == "idle":
        a, b = mm.select_idle(take, int(round(spec.seconds * take.fps)))
        clip = mm.canonical_loop(take, a, b, np.array([0.0, 1.0]), in_place=True)
        clip.speed = 0.0
    else:
        a, b = mm.select_cycle(take, spec.cycles, spec.direction)
        return _finish_cycle(spec, take, a, b, start, target, reference, spec.take)
    return _finish(spec, take, clip, a, b, start, target, reference, spec.take)


def _finish_cycle(spec, take, a, b, start, target, reference, source):
    facing = np.array([0.0, 1.0])
    c, s = np.cos(spec.direction), np.sin(spec.direction)
    travel = np.array([c * facing[0] - s * facing[1], s * facing[0] + c * facing[1]])
    clip = mm.canonical_loop(take, a, b, travel)
    return _finish(spec, take, clip, a, b, start, target, reference, source)


def _finish(spec, take, clip, a, b, start, target, reference, source):
    scale = hip_scale(target, take)
    rotation, position = mm.retarget(clip, target, scale, reference, mm.hand_pose(target, spec.hands))
    rotation, position = mm.plant_feet(clip, target, rotation, position, scale)
    quaternions, translation = mm.local_pose(target, rotation, position)
    return {"clip": clip, "window": (int(a + start), int(b + start)), "source": source, "scale": scale,
            "rotation": rotation, "position": position,
            "quaternions": quaternions, "translation": translation,
            "speed": clip.speed * scale}


def add_action(armature_obj, name, target, quaternions, translation, loop, translated=("pelvis",)):
    """One action keyed every frame. A loop gets a closing key equal to its
    first so it wraps exactly at the clip's duration."""
    action = bpy.data.actions.new(name)
    action.use_fake_user = True
    action["vkr_loop"] = bool(loop)
    armature_obj.animation_data_create()
    armature_obj.animation_data.action = action
    frames = len(quaternions)
    closing = 1 if loop else 0
    keys = np.arange(frames + closing, dtype=np.float64)
    for b, bone in enumerate(target.names):
        pose_bone = armature_obj.pose.bones[bone]
        pose_bone.rotation_mode = "QUATERNION"
        channels = [("rotation_quaternion", quaternions[:, b], 4)]
        if bone in translated:
            channels.append(("location", translation[:, b], 3))
        for path, values, width in channels:
            if loop:
                values = np.concatenate([values, values[:1]], axis=0)
                if path == "rotation_quaternion" and np.dot(values[-1], values[-2]) < 0.0:
                    values[-1] = -values[-1]
            for index in range(width):
                curve = action.fcurve_ensure_for_datablock(
                    armature_obj, f'pose.bones["{bone}"].{path}', index=index, group_name=bone)
                curve.keyframe_points.add(frames + closing)
                curve.keyframe_points.foreach_set("co", np.stack([keys, values[:, index]], axis=1).ravel())
                for point in curve.keyframe_points:
                    point.interpolation = "LINEAR"
                curve.update()
    armature_obj.animation_data.action = None
    return action
