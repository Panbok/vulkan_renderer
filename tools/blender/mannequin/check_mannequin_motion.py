"""Measure foot slip, ground penetration and loop seams of mannequin clips.

    Blender --background <out>/mannequin.blend --python check_mannequin_motion.py -- \\
        [--json report.json] [--max-slip 0.05] [--max-seam-degrees 0.5]

The check samples Blender's own evaluation of each action (what the glTF
exporter bakes) rather than the generator's arrays. Looping clips advance
the armature at the clip's recorded speed along its travel direction; a foot point in ground contact
should then stay still, so its horizontal speed
is the slip a player sees; "in contact" means within 5 mm of the floor,
where the generator pins planted points. The seam compares the first frame with the
closing key. Exits 1 when a limit fails.
"""

import argparse
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector


FOOT_POINTS = {
    # Contact points in each foot bone's rest frame are measured from the
    # rest pose: the heel under the ankle and the ball joint on the floor.
    "l": ("foot_l", "ball_l"),
    "r": ("foot_r", "ball_r"),
}


def _arguments():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser()
    parser.add_argument("--json", type=Path)
    parser.add_argument("--max-slip", type=float, default=0.05, help="m/s, 95th percentile in contact")
    parser.add_argument("--max-seam-degrees", type=float, default=0.5)
    parser.add_argument("--max-penetration", type=float, default=0.015, help="metres below the floor")
    return parser.parse_args(argv)


def _contact_points(armature):
    """(bone, local offset) pairs of every foot's heel and ball on the floor."""
    points = []
    for side, (foot, ball) in FOOT_POINTS.items():
        foot_bone = armature.data.bones[foot]
        ball_bone = armature.data.bones[ball]
        rest = foot_bone.matrix_local
        heel_world = Vector((foot_bone.head_local.x, foot_bone.head_local.y - 0.035, 0.0))
        ball_world = Vector((ball_bone.head_local.x, ball_bone.head_local.y, 0.0))
        points.append((side + "_heel", foot, rest.inverted() @ heel_world))
        points.append((side + "_ball", foot, rest.inverted() @ ball_world))
    return points


def _sample(scene, armature, action, points):
    armature.animation_data.action = action
    if action.slots:
        armature.animation_data.action_slot = action.slots[0]
    start, end = (int(round(f)) for f in action.frame_range)
    speed = float(action.get("vkr_speed", 0.0))
    travel = Vector((*action.get("vkr_travel", (0.0, 1.0)), 0.0))
    fps = scene.render.fps
    positions = {name: [] for name, _, _ in points}
    rotations = []
    for frame in range(start, end + 1):
        scene.frame_set(frame)
        advance = travel * (speed * (frame - start) / fps)
        for name, bone, offset in points:
            matrix = armature.pose.bones[bone].matrix
            positions[name].append(tuple(matrix @ offset + advance))
        rotations.append([tuple(pb.matrix_basis.to_quaternion()) for pb in armature.pose.bones])
    return {k: np.array(v) for k, v in positions.items()}, np.array(rotations), fps


def main():
    args = _arguments()
    scene = bpy.context.scene
    armature = next(o for o in scene.objects if o.type == "ARMATURE")
    armature.animation_data_create()
    points = _contact_points(armature)
    report = {"clips": {}, "limits": {"max_slip": args.max_slip, "max_seam_degrees": args.max_seam_degrees,
                                      "max_penetration": args.max_penetration}}
    failed = False
    for action in bpy.data.actions:
        positions, rotations, fps = _sample(scene, armature, action, points)
        slips = []
        lowest = math.inf
        # Airborne frames (jumps) float above the capsule's floor: neither
        # slip nor penetration applies to them.
        grounded = np.ones(len(rotations), dtype=bool)
        grounded[[f for f in action.get("vkr_airborne", []) if f < len(grounded)]] = False
        for name, path in positions.items():
            velocity = np.gradient(path, axis=0) * fps
            horizontal = np.linalg.norm(velocity[:, :2], axis=1)
            # Planted: within 5 mm of the floor (the IK pins contacts at 0)
            # and not lifting off or landing.
            contact = (path[:, 2] < 0.005) & (np.abs(velocity[:, 2]) < 0.05) & grounded
            if contact.any():
                slips.extend(horizontal[contact].tolist())
            if grounded.any():
                lowest = min(lowest, float(path[grounded, 2].min()))
        # One-shots have no seam; loops end on a copy of their first frame.
        seam = 0.0
        if action.get("vkr_loop", True):
            dots = np.abs(np.sum(rotations[0] * rotations[-1], axis=1)).clip(0.0, 1.0)
            seam = float(np.degrees(2.0 * np.arccos(dots)).max())
        slip95 = float(np.percentile(slips, 95)) if slips else 0.0
        entry = {"frames": len(rotations), "speed": float(action.get("vkr_speed", 0.0)),
                 "contact_samples": len(slips), "slip_p95": slip95,
                 "slip_max": float(max(slips)) if slips else 0.0,
                 "lowest_point": lowest, "seam_degrees": seam}
        entry["pass"] = (slip95 <= args.max_slip and seam <= args.max_seam_degrees and
                         lowest >= -args.max_penetration)
        failed |= not entry["pass"]
        report["clips"][action.name] = entry
        print(f"{action.name:18s} frames {entry['frames']:4d} speed {entry['speed']:5.2f} "
              f"slip p95 {slip95:6.3f} max {entry['slip_max']:6.3f} m/s  lowest {lowest:+.3f} m  "
              f"seam {seam:5.2f} deg  {'ok' if entry['pass'] else 'FAIL'}")
    if args.json:
        args.json.write_text(json.dumps(report, indent=2))
    sys.exit(1 if failed else 0)


main()
