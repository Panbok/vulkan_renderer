"""Motion capture retargeting and loop extraction for the mannequin (numpy).

Source takes are 100STYLE BVH files (Xsens, 60 fps): centimetres, Y up,
facing +Z with the actor's left on +X. They are converted to the mannequin
frame (metres, Z up, facing +Y, left on -X) before any analysis.

A clip is produced in four steps:

1. **Window.** Choose a stretch of the take: for locomotion an integer number
   of gait cycles along a straight path, between left heel strikes whose
   poses match best; for idles a stretch whose ends match.
2. **Canonical frame.** Turn the window so it travels along the requested
   direction, remove the mean horizontal velocity (in-place playback), and
   spread the remaining end-to-start mismatch over the window so the loop
   closes exactly.
3. **Retarget.** Every mapped bone takes the source joint's world-space
   rotation delta from the T-pose, applied to the mannequin bone's rest
   orientation aligned to that T-pose (arms and legs by direction; spine,
   head and feet keep their own rest). The pelvis path scales with hip
   height.
4. **Feet.** Heel and ball contacts detected on the source are planted: in a
   loop the planted foot slides backwards at exactly the clip speed, so a
   character moving at that speed shows no skating. Two-bone IK places the
   ankles; knees keep the retargeted bend direction.
"""

from dataclasses import dataclass

import numpy as np

import mannequin_bvh as bvh


FPS = 60

_TO_FRAME = np.array([[-1.0, 0.0, 0.0],
                      [0.0, 0.0, 1.0],
                      [0.0, 1.0, 0.0]])


# =============================================================================
# Rotation helpers (3x3 matrices, column vectors)
# =============================================================================

def _normalize(v):
    v = np.asarray(v, dtype=np.float64)
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-12)


def axis_angle(axis, angle):
    """Rotation matrices about unit axes; broadcasts over leading dims."""
    axis = _normalize(axis)
    angle = np.asarray(angle, dtype=np.float64)
    x, y, z = axis[..., 0], axis[..., 1], axis[..., 2]
    c = np.cos(angle)
    s = np.sin(angle)
    t = 1.0 - c
    m = np.empty(np.broadcast_shapes(x.shape, angle.shape) + (3, 3))
    m[..., 0, 0] = t * x * x + c
    m[..., 0, 1] = t * x * y - s * z
    m[..., 0, 2] = t * x * z + s * y
    m[..., 1, 0] = t * x * y + s * z
    m[..., 1, 1] = t * y * y + c
    m[..., 1, 2] = t * y * z - s * x
    m[..., 2, 0] = t * x * z - s * y
    m[..., 2, 1] = t * y * z + s * x
    m[..., 2, 2] = t * z * z + c
    return m


def rotation_between(a, b):
    """Minimal rotation taking direction a to direction b."""
    a = _normalize(a)
    b = _normalize(b)
    axis = np.cross(a, b)
    sin = np.linalg.norm(axis)
    cos = float(np.clip(a @ b, -1.0, 1.0))
    if sin < 1e-9:
        if cos > 0.0:
            return np.eye(3)
        helper = np.array([1.0, 0.0, 0.0]) if abs(a[0]) < 0.9 else np.array([0.0, 1.0, 0.0])
        return axis_angle(np.cross(a, helper), np.pi)
    return axis_angle(axis / sin, np.arctan2(sin, cos))


def log_map(r):
    """Rotation vectors of rotation matrices (..., 3, 3)."""
    trace = np.trace(r, axis1=-2, axis2=-1)
    angle = np.arccos(np.clip((trace - 1.0) * 0.5, -1.0, 1.0))
    v = np.stack([r[..., 2, 1] - r[..., 1, 2], r[..., 0, 2] - r[..., 2, 0], r[..., 1, 0] - r[..., 0, 1]], axis=-1)
    sin = np.sin(angle)
    small = sin < 1e-6
    scale = np.where(small, 0.5, angle / (2.0 * np.where(small, 1.0, sin)))
    result = v * scale[..., None]
    # Near pi the antisymmetric part vanishes; recover the axis from the
    # symmetric part.
    near_pi = angle > np.pi - 1e-3
    if np.any(near_pi):
        for index in zip(*np.nonzero(near_pi)):
            m = r[index]
            axis = np.sqrt(np.maximum((np.diag(m) + 1.0) * 0.5, 0.0))
            axis[1] = np.copysign(axis[1], m[0, 1] + m[1, 0]) if axis[0] > 1e-6 else axis[1]
            axis[2] = np.copysign(axis[2], m[0, 2] + m[2, 0]) if axis[0] > 1e-6 else np.copysign(axis[2], m[1, 2] + m[2, 1])
            result[index] = _normalize(axis) * angle[index]
    return result


def exp_map(v):
    v = np.asarray(v, dtype=np.float64)
    angle = np.linalg.norm(v, axis=-1)
    axis = v / np.maximum(angle, 1e-12)[..., None]
    axis = np.where((angle < 1e-12)[..., None], np.array([1.0, 0.0, 0.0]), axis)
    return axis_angle(axis, angle)


def slerp_matrix(a, b, t):
    """a * exp(t log(a^T b)); broadcasts."""
    return a @ exp_map(log_map(np.swapaxes(a, -1, -2) @ b) * np.asarray(t)[..., None])


def orthonormalize(r):
    u, _, vt = np.linalg.svd(r)
    fixed = u @ vt
    flip = np.linalg.det(fixed) < 0.0
    if np.any(flip):
        u[flip, :, -1] *= -1.0
        fixed = u @ vt
    return fixed


def matrix_to_quaternion(r):
    """(w, x, y, z) quaternions, w >= 0 before hemisphere continuity."""
    r = np.asarray(r)
    q = np.empty(r.shape[:-2] + (4,))
    trace = r[..., 0, 0] + r[..., 1, 1] + r[..., 2, 2]
    for index in np.ndindex(r.shape[:-2]):
        m = r[index]
        t = trace[index]
        if t > 0.0:
            s = np.sqrt(t + 1.0) * 2.0
            q[index] = (0.25 * s, (m[2, 1] - m[1, 2]) / s, (m[0, 2] - m[2, 0]) / s, (m[1, 0] - m[0, 1]) / s)
        elif m[0, 0] > m[1, 1] and m[0, 0] > m[2, 2]:
            s = np.sqrt(1.0 + m[0, 0] - m[1, 1] - m[2, 2]) * 2.0
            q[index] = ((m[2, 1] - m[1, 2]) / s, 0.25 * s, (m[0, 1] + m[1, 0]) / s, (m[0, 2] + m[2, 0]) / s)
        elif m[1, 1] > m[2, 2]:
            s = np.sqrt(1.0 + m[1, 1] - m[0, 0] - m[2, 2]) * 2.0
            q[index] = ((m[0, 2] - m[2, 0]) / s, (m[0, 1] + m[1, 0]) / s, 0.25 * s, (m[1, 2] + m[2, 1]) / s)
        else:
            s = np.sqrt(1.0 + m[2, 2] - m[0, 0] - m[1, 1]) * 2.0
            q[index] = ((m[1, 0] - m[0, 1]) / s, (m[0, 2] + m[2, 0]) / s, (m[1, 2] + m[2, 1]) / s, 0.25 * s)
    return q / np.linalg.norm(q, axis=-1, keepdims=True)


def continuous_quaternions(q):
    """Flip signs along axis 0 so consecutive quaternions share a hemisphere."""
    q = q.copy()
    for f in range(1, len(q)):
        flip = np.sum(q[f] * q[f - 1], axis=-1) < 0.0
        q[f][flip] *= -1.0
    return q


def smoothstep(edge0, edge1, x):
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def smooth(signal, frames):
    """Centred moving average along axis 0 with edge padding."""
    if frames <= 1:
        return signal
    kernel = np.ones(frames) / frames
    pad = frames // 2
    padded = np.concatenate([np.repeat(signal[:1], pad, axis=0), signal,
                             np.repeat(signal[-1:], frames - 1 - pad, axis=0)], axis=0)
    flat = padded.reshape(len(padded), -1)
    out = np.stack([np.convolve(flat[:, i], kernel, mode="valid") for i in range(flat.shape[1])], axis=1)
    return out.reshape((len(signal),) + signal.shape[1:])


# =============================================================================
# Source takes
# =============================================================================

@dataclass
class Take:
    names: list
    parents: list
    rest: np.ndarray          # (J, 3) T-pose joint positions, metres
    end_sites: dict           # joint name -> end offset (metres, frame axes)
    rotation: np.ndarray      # (F, J, 3, 3) global rotations
    position: np.ndarray      # (F, J, 3) global positions
    fps: float

    def joint(self, name):
        return self.names.index(name)

    @property
    def frames(self):
        return len(self.rotation)

    def copy(self):
        return Take(list(self.names), list(self.parents), self.rest.copy(), dict(self.end_sites),
                    self.rotation.copy(), self.position.copy(), self.fps)


def load_take(path, start=0, stop=None):
    motion = bvh.read(path)
    rotations, root = bvh.local_transforms(motion, start, stop)
    global_rotation, global_position = bvh.forward_kinematics(motion, rotations, root)
    frame_rotation = _TO_FRAME @ global_rotation @ _TO_FRAME.T
    frame_position = (global_position @ _TO_FRAME.T) * 0.01
    rest = (bvh.rest_positions(motion) @ _TO_FRAME.T) * 0.01
    end_sites = {j.name: (j.end_site @ _TO_FRAME.T) * 0.01 for j in motion.joints if j.end_site is not None}
    take = Take([j.name for j in motion.joints], [j.parent for j in motion.joints], rest, end_sites,
                frame_rotation, frame_position, 1.0 / motion.frame_time)
    return stand_on_floor(take)


def stand_on_floor(take):
    """Shift a take so its lowest ball-of-foot contacts rest on z = 0."""
    floor = np.percentile(np.minimum(take.position[:, take.joint("LeftToe"), 2],
                                     take.position[:, take.joint("RightToe"), 2]), 1.0)
    take.position[..., 2] -= floor - _ball_height(take)
    return take


def hip_center(take):
    """Per-frame midpoint of the hip joints (F, 3)."""
    return 0.5 * (take.position[:, take.joint("LeftHip")] + take.position[:, take.joint("RightHip")])


def _ball_height(take):
    """Height of the toe joint above the sole when the foot is flat: the
    Xsens toe joint sits above its end site by the end's drop."""
    return max(0.0, -take.end_sites["LeftToe"][2]) + 0.01


# =============================================================================
# Gait analysis
# =============================================================================

def foot_points(take, side):
    """World positions (F, 3) of the heel and ball contact points."""
    prefix = "Left" if side == "l" else "Right"
    ankle = take.position[:, take.joint(prefix + "Ankle")]
    toe = take.position[:, take.joint(prefix + "Toe")]
    heel = ankle.copy()
    # The heel sits under the ankle, a little behind it along the foot.
    forward = _normalize((toe - ankle) * np.array([1.0, 1.0, 0.0]))
    heel = heel - forward * 0.035
    heel[:, 2] = ankle[:, 2] - (take.rest[take.joint(prefix + "Ankle"), 2] - take.rest[take.joint(prefix + "Toe"), 2]) \
        - _ball_height(take)
    return heel, toe - np.array([0.0, 0.0, _ball_height(take)])


def _runs(mask):
    """(start, stop) index pairs of True runs."""
    edges = np.diff(np.concatenate([[0], mask.astype(np.int8), [0]]))
    return list(zip(np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)))


def contact_mask(points, fps, height=0.035, speed=0.35, debounce=0.1):
    """Frames where a contact point is low and nearly still. Gaps and runs
    shorter than `debounce` seconds are absorbed so stance edges, where the
    speed threshold flickers, neither split nor invent a stance."""
    velocity = np.gradient(points, axis=0) * fps
    horizontal = np.linalg.norm(velocity[:, :2], axis=1)
    mask = (points[:, 2] < height) & (horizontal < speed)
    shortest = max(1, int(round(debounce * fps)))
    for start, stop in _runs(~mask):
        if stop - start < shortest and start > 0 and stop < len(mask):
            mask[start:stop] = True
    for start, stop in _runs(mask):
        if stop - start < shortest:
            mask[start:stop] = False
    return mask


def heel_strikes(take, side):
    heel, ball = foot_points(take, side)
    down = contact_mask(heel, take.fps) | contact_mask(ball, take.fps)
    return np.array([start for start, _ in _runs(down)], dtype=np.int64), down


def travel(take, window=60):
    """Smoothed horizontal hip velocity (F, 2) and heading (radians)."""
    hips = take.position[:, take.joint("Hips"), :2]
    velocity = np.gradient(smooth(hips, window), axis=0) * take.fps
    return velocity, np.arctan2(velocity[:, 1], velocity[:, 0])


def body_heading(take):
    """Facing heading per frame from the hip joints (radians, 0 = +X)."""
    left = take.position[:, take.joint("LeftHip"), :2]
    right = take.position[:, take.joint("RightHip"), :2]
    across = right - left  # points to the character's right
    forward = np.stack([-across[:, 1], across[:, 0]], axis=1)
    return np.arctan2(forward[:, 1], forward[:, 0])


def _angle_diff(a, b):
    return (a - b + np.pi) % (2.0 * np.pi) - np.pi


def _pose_distance(take, a, b):
    """Rotation distance between two frames of the hips-relative pose, plus
    hip height and in-plane velocity terms."""
    hips = take.joint("Hips")
    ra = np.swapaxes(take.rotation[a, hips], -1, -2) @ take.rotation[a]
    rb = np.swapaxes(take.rotation[b, hips], -1, -2) @ take.rotation[b]
    angles = np.linalg.norm(log_map(np.swapaxes(ra, -1, -2) @ rb), axis=-1)
    height = abs(take.position[a, hips, 2] - take.position[b, hips, 2])
    velocity = np.gradient(take.position[:, hips], axis=0) * take.fps
    speed = np.linalg.norm(velocity[a] - velocity[b])
    return angles.sum() + 10.0 * height + 0.5 * speed


def select_cycle(take, cycles, direction, tolerance=np.radians(12.0), min_speed=0.2):
    """Frames [a, b) spanning `cycles` left gait cycles on a straight stretch
    whose travel is `direction` relative to the body's facing (radians: 0
    forward, pi backward, +pi/2 to the character's left). Returns the best
    pair by pose match."""
    strikes, _ = heel_strikes(take, "l")
    right, _ = heel_strikes(take, "r")
    velocity, heading = travel(take)
    facing = body_heading(take)
    speed = np.linalg.norm(velocity, axis=1)
    best = None
    for i in range(len(strikes) - cycles):
        a, b = strikes[i], strikes[i + cycles]
        span = slice(a, b)
        # Each cycle holds one right strike and lasts a plausible stride time.
        cycle_frames = np.diff(strikes[i:i + cycles + 1])
        if cycle_frames.min() < 0.45 * take.fps or cycle_frames.max() > 2.0 * take.fps:
            continue
        if np.count_nonzero((right > a) & (right < b)) != cycles:
            continue
        if np.any(speed[span] < min_speed):
            continue
        mean_heading = np.arctan2(np.sin(heading[span]).mean(), np.cos(heading[span]).mean())
        if np.max(np.abs(_angle_diff(heading[span], mean_heading))) > tolerance:
            continue
        relative = _angle_diff(mean_heading, facing[span])
        if np.max(np.abs(_angle_diff(relative, direction))) > np.radians(25.0):
            continue
        if cycle_frames.max() > 1.25 * cycle_frames.min():
            continue
        score = _pose_distance(take, a, b)
        if best is None or score < best[0]:
            best = (score, a, b)
    if best is None:
        raise ValueError("No straight gait cycle found for the requested direction")
    return best[1], best[2]


def select_idle(take, length, search=None):
    """An idle window [a, a+length) whose end pose matches its start."""
    best = None
    frames = take.frames if search is None else search
    hips = take.joint("Hips")
    for a in range(0, frames - length, 6):
        b = a + length
        drift = np.linalg.norm(take.position[b, hips, :2] - take.position[a, hips, :2])
        score = _pose_distance(take, a, b) + 20.0 * drift
        if best is None or score < best[0]:
            best = (score, a, b)
    return best[1], best[2]


# =============================================================================
# Canonical loops
# =============================================================================

@dataclass
class Clip:
    take: Take
    speed: float              # metres/second along `travel`, before retarget scale
    travel: np.ndarray        # unit horizontal travel direction in the clip frame
    loop: bool
    # Frames with both feet in the air (jumps): never planted.
    airborne: np.ndarray = None


def canonical_loop(take, a, b, travel_direction, in_place=True):
    """Window [a, b) turned to travel along `travel_direction` (a unit 2D
    vector in the mannequin frame), with the mean velocity removed and the
    loop closed so frame b would equal frame a."""
    hips = take.joint("Hips")
    frames = b - a
    seconds = frames / take.fps
    start = take.position[a, hips]
    delta = take.position[b, hips] - start
    mean_heading = np.arctan2(delta[1], delta[0]) if np.linalg.norm(delta[:2]) > 0.05 else body_heading(take)[a]
    target = np.arctan2(travel_direction[1], travel_direction[0])
    turn = axis_angle(np.array([0.0, 0.0, 1.0]), target - mean_heading)
    clip = take.copy()
    clip.rotation = turn @ take.rotation[a:b + 1]
    clip.position = (take.position[a:b + 1] - np.array([start[0], start[1], 0.0])) @ turn.T
    velocity = (clip.position[-1, hips] - clip.position[0, hips]) / seconds
    velocity[2] = 0.0
    speed = float(np.linalg.norm(velocity[:2]))
    if in_place:
        times = np.arange(frames + 1) / take.fps
        clip.position -= (times[:, None] * velocity)[:, None, :]

    # Close the loop: spread the end-to-start difference linearly in time.
    weights = np.arange(frames + 1) / frames
    rotation_error = log_map(clip.rotation[0] @ np.swapaxes(clip.rotation[-1], -1, -2))  # (J, 3)
    correction = exp_map(weights[:, None, None] * rotation_error[None])
    clip.rotation = correction @ clip.rotation
    position_error = clip.position[0] - clip.position[-1]
    clip.position += weights[:, None, None] * position_error[None]
    clip.rotation = clip.rotation[:-1]
    clip.position = clip.position[:-1]
    travel = np.array([np.cos(target), np.sin(target), 0.0])
    return Clip(clip, speed, travel, True)


def window(take, a, b):
    """A one-shot window [a, b) with its first frame's hips at the origin,
    heading unchanged."""
    hips = take.joint("Hips")
    clip = take.copy()
    start = take.position[a, hips]
    clip.rotation = take.rotation[a:b]
    clip.position = take.position[a:b] - np.array([start[0], start[1], 0.0])
    return Clip(clip, 0.0, np.array([0.0, 1.0, 0.0]), False)


def axial_reference(take):
    """Mean orientation of every joint relative to the hips' heading over a
    neutral idle take. The Xsens spine, neck and head frames are not level
    in natural standing (the neck segment leans forward about 20 degrees),
    so the axial skeleton is retargeted relative to this posture rather than
    to the file's T-pose."""
    hips = take.joint("Hips")
    forward = take.rotation[:, hips, :, 1]
    yaw = np.arctan2(forward[:, 1], forward[:, 0]) - np.pi / 2.0
    unyaw = axis_angle(np.array([0.0, 0.0, 1.0]), -yaw)
    relative = unyaw[:, None] @ take.rotation
    return orthonormalize(relative.mean(axis=0))


# =============================================================================
# Jumps
# =============================================================================

def flight(take, clearance=0.03, shortest=6):
    """(takeoff, touchdown) of the take's longest stretch with both feet off
    the floor: the first airborne frame and the first frame back down."""
    lowest = None
    for side in ("l", "r"):
        heel, ball = foot_points(take, side)
        low = np.minimum(heel[:, 2], ball[:, 2])
        lowest = low if lowest is None else np.minimum(lowest, low)
    runs = [(a, b) for a, b in _runs(lowest > clearance) if b - a >= shortest]
    if not runs:
        raise ValueError("No flight phase in take")
    return max(runs, key=lambda run: run[1] - run[0])


def remove_flight_arc(take, takeoff, touchdown):
    """Subtract the ballistic rise of the hips between takeoff and touchdown
    from every joint, keeping the pose's tuck relative to the body. A game
    character's capsule supplies the arc."""
    take = take.copy()
    hips = hip_center(take)[:, 2]
    frames = np.arange(takeoff - 1, touchdown + 1)
    times = frames / take.fps
    coefficients = np.polyfit(times, hips[frames], 2)
    arc = np.polyval(coefficients, np.arange(take.frames) / take.fps)
    base = np.interp(np.arange(take.frames), [takeoff - 1, touchdown], [arc[takeoff - 1], arc[touchdown]])
    lift = np.zeros(take.frames)
    inside = slice(takeoff - 1, touchdown + 1)
    lift[inside] = (arc - base)[inside]
    take.position[..., 2] -= lift[:, None]
    return take


def one_shot(take, a, b, in_place=True):
    """Frames [a, b) with the hips' start at the origin; `in_place` also
    removes the horizontal drift across the window."""
    hips = take.joint("Hips")
    clip = window(take, a, b)
    if in_place:
        center = hip_center(clip.take)
        drift = np.linspace(0.0, 1.0, b - a)[:, None] * (center[-1] - center[0])
        drift[:, 2] = 0.0
        clip.take.position -= drift[:, None, :]
    del hips
    return clip


# =============================================================================
# Target skeleton and retargeting
# =============================================================================

SPINE_MAP = {
    "pelvis": ("Hips", "Hips", 0.0),
    "spine_01": ("Chest", "Chest", 0.0),
    "spine_02": ("Chest2", "Chest2", 0.0),
    "spine_03": ("Chest2", "Chest3", 0.5),
    "spine_04": ("Chest3", "Chest3", 0.0),
    "spine_05": ("Chest4", "Chest4", 0.0),
    "neck_01": ("Neck", "Neck", 0.0),
    "neck_02": ("Neck", "Head", 0.5),
    "head": ("Head", "Head", 0.0),
}

LIMB_MAP = {}
for _side, _prefix in (("l", "Left"), ("r", "Right")):
    LIMB_MAP.update({
        f"clavicle_{_side}": _prefix + "Collar",
        f"upperarm_{_side}": _prefix + "Shoulder",
        f"lowerarm_{_side}": _prefix + "Elbow",
        f"hand_{_side}": _prefix + "Wrist",
        f"thigh_{_side}": _prefix + "Hip",
        f"calf_{_side}": _prefix + "Knee",
        f"foot_{_side}": _prefix + "Ankle",
        f"ball_{_side}": _prefix + "Toe",
    })

# Bones aligned to the source T-pose by direction; the rest keep their own
# rest orientation (spine curve, flat feet).
ALIGNED = ("clavicle", "upperarm", "lowerarm", "hand", "thigh", "calf")


@dataclass
class Target:
    names: list
    parents: list            # parent index or -1
    rest: np.ndarray         # (B, 4, 4) armature-space rest matrices (bone Y along the bone)
    lengths: np.ndarray      # (B,) bone lengths (head to tail)

    def index(self, name):
        return self.names.index(name)

    def head(self, name):
        return self.rest[self.index(name), :3, 3]

    def direction(self, name):
        return self.rest[self.index(name), :3, 1]


def _source_direction(take, joint):
    """Rest direction of a source joint towards its child or end site."""
    j = take.joint(joint)
    children = [i for i, p in enumerate(take.parents) if p == j]
    if joint in take.end_sites:
        return _normalize(take.end_sites[joint])
    child = {"Hips": "Chest", "Chest4": "Neck"}.get(joint)
    if child is None:
        child = take.names[children[0]]
    return _normalize(take.rest[take.joint(child)] - take.rest[j])


def aligned_rest(target, take):
    """Target rest orientations (B, 3, 3) posed into the source's T-pose."""
    count = len(target.names)
    align = [np.eye(3) for _ in range(count)]
    oriented = np.zeros((count, 3, 3))
    for b, name in enumerate(target.names):
        parent = target.parents[b]
        inherited = align[parent] if parent >= 0 else np.eye(3)
        kind = name.rsplit("_", 1)[0]
        if kind in ALIGNED and name in LIMB_MAP:
            current = inherited @ target.direction(name)
            align[b] = rotation_between(current, _source_direction(take, LIMB_MAP[name])) @ inherited
        elif name in SPINE_MAP or kind in ("foot", "ball"):
            align[b] = np.eye(3)
        else:
            align[b] = inherited
        oriented[b] = align[b] @ target.rest[b, :3, :3]
    return oriented


# Flexion per finger segment in degrees (01, 02, 03) and metacarpal cupping.
HAND_POSES = {
    "relaxed": {"thumb": (6.0, 10.0, 14.0), "index": (14.0, 22.0, 12.0), "middle": (18.0, 28.0, 15.0),
                "ring": (22.0, 33.0, 18.0), "pinky": (27.0, 36.0, 20.0), "cup": (0.0, 1.5, 4.0, 7.0)},
    "fist": {"thumb": (14.0, 22.0, 26.0), "index": (48.0, 62.0, 32.0), "middle": (55.0, 68.0, 34.0),
             "ring": (60.0, 72.0, 36.0), "pinky": (64.0, 74.0, 38.0), "cup": (0.0, 3.0, 7.0, 11.0)},
}


def hand_pose(target, style):
    """Local rotations (bone rest frame) curling every finger towards its
    palm. The palm normal comes from the wrist and the index and pinky
    knuckles, so the pose rides whatever the hand does."""
    pose = HAND_POSES[style]
    result = {}
    for side, sign in (("l", -1.0), ("r", 1.0)):
        wrist = target.head(f"hand_{side}")
        index = target.head(f"index_01_{side}")
        pinky = target.head(f"pinky_01_{side}")
        knuckles = 0.5 * (index + pinky)
        along = _normalize(knuckles - wrist)
        across = _normalize(pinky - index)
        palm = _normalize(sign * np.cross(along, across))
        for finger, (a, b, c) in ((f, pose[f]) for f in ("thumb", "index", "middle", "ring", "pinky")):
            for segment, degrees in zip((1, 2, 3), (a, b, c)):
                name = f"{finger}_0{segment}_{side}"
                bone = target.index(name)
                direction = target.rest[bone, :3, 1]
                axis_world = _normalize(np.cross(direction, palm))
                axis_local = target.rest[bone, :3, :3].T @ axis_world
                result[name] = axis_angle(axis_local, np.radians(degrees))
        for finger, degrees in zip(("index", "middle", "ring", "pinky"), pose["cup"]):
            if degrees == 0.0:
                continue
            name = f"{finger}_metacarpal_{side}"
            bone = target.index(name)
            direction = target.rest[bone, :3, 1]
            axis_world = _normalize(np.cross(direction, palm))
            result[name] = axis_angle(target.rest[bone, :3, :3].T @ axis_world, np.radians(degrees))
    return result


def _twist_angle(rotation, axis):
    """Twist of a rotation about an axis (swing-twist), radians."""
    q = matrix_to_quaternion(rotation)
    projection = (q[..., 1:] * axis).sum(axis=-1)
    return 2.0 * np.arctan2(projection, q[..., 0])


def retarget(clip, target, hip_scale, reference, finger_pose=None):
    """Global target rotations (F, B, 3, 3) and bone head positions (F, B, 3).
    `reference` is `axial_reference` of the actor's neutral idle."""
    take = clip.take
    frames = take.frames
    oriented = aligned_rest(target, take)
    rotation = np.zeros((frames, len(target.names), 3, 3))
    for b, name in enumerate(target.names):
        parent = target.parents[b]
        if name in SPINE_MAP:
            first, second, t = SPINE_MAP[name]
            delta = take.rotation[:, take.joint(first)] @ reference[take.joint(first)].T
            if t > 0.0:
                other = take.rotation[:, take.joint(second)] @ reference[take.joint(second)].T
                delta = slerp_matrix(delta, other, np.full(frames, t))
            rotation[:, b] = delta @ oriented[b]
        elif name in LIMB_MAP:
            rotation[:, b] = take.rotation[:, take.joint(LIMB_MAP[name])] @ oriented[b]
        elif parent >= 0:
            # Twist bones, fingers and virtual bones keep their rest offset
            # from the parent.
            local = np.swapaxes(target.rest[parent, :3, :3], -1, -2) @ target.rest[b, :3, :3]
            rotation[:, b] = rotation[:, parent] @ local
        else:
            rotation[:, b] = np.eye(3)
        if finger_pose is not None and name in finger_pose:
            rotation[:, b] = rotation[:, b] @ finger_pose[name]
            # Children inherit through their own rest offsets above.
    # Forearm twist: half of the hand's roll about the forearm.
    for side in ("l", "r"):
        lower = target.index(f"lowerarm_{side}")
        hand = target.index(f"hand_{side}")
        twist = target.index(f"lowerarm_twist_01_{side}")
        relative = np.swapaxes(rotation[:, lower], -1, -2) @ rotation[:, hand] @ \
            np.swapaxes(np.swapaxes(target.rest[lower, :3, :3], -1, -2) @ target.rest[hand, :3, :3], -1, -2)
        angle = _twist_angle(relative, np.array([0.0, 1.0, 0.0]))
        local = np.swapaxes(target.rest[lower, :3, :3], -1, -2) @ target.rest[twist, :3, :3]
        rotation[:, twist] = rotation[:, lower] @ axis_angle(np.array([0.0, 1.0, 0.0]), 0.5 * angle) @ local
    rotation = orthonormalize(rotation)

    # Heads by forward kinematics from the scaled pelvis path. The source
    # Hips joint is the hip-joint centre; the pelvis head keeps its rest
    # offset from the mannequin's hip centre in the pelvis frame.
    position = np.zeros((frames, len(target.names), 3))
    hips = hip_center(take)
    pelvis = target.index("pelvis")
    target_hips = 0.5 * (target.head("thigh_l") + target.head("thigh_r"))
    pelvis_offset = target.rest[pelvis, :3, :3].T @ (target.head("pelvis") - target_hips)
    for b, name in enumerate(target.names):
        parent = target.parents[b]
        if parent < 0:
            position[:, b] = target.rest[b, :3, 3]
            continue
        if b == pelvis:
            position[:, b] = hips * hip_scale + np.einsum("fij,j->fi", rotation[:, b], pelvis_offset)
            continue
        offset = np.swapaxes(target.rest[parent, :3, :3], -1, -2) @ (target.rest[b, :3, 3] - target.rest[parent, :3, 3])
        position[:, b] = position[:, parent] + np.einsum("fij,j->fi", rotation[:, parent], offset)
    return rotation, position


# =============================================================================
# Feet
# =============================================================================

def _two_bone_ik(hip, knee, ankle, goal, pole_hint):
    """New knee and ankle positions reaching `goal` with fixed bone lengths;
    the knee stays in the plane of the hip, goal and pole hint."""
    upper = np.linalg.norm(knee - hip, axis=1)
    lower = np.linalg.norm(ankle - knee, axis=1)
    reach = goal - hip
    distance = np.linalg.norm(reach, axis=1)
    distance = np.clip(distance, np.abs(upper - lower) + 1e-4, upper + lower - 1e-4)
    direction = reach / np.maximum(np.linalg.norm(reach, axis=1, keepdims=True), 1e-9)
    goal = hip + direction * distance[:, None]
    cos_hip = (upper ** 2 + distance ** 2 - lower ** 2) / (2.0 * upper * distance)
    angle = np.arccos(np.clip(cos_hip, -1.0, 1.0))
    bend = pole_hint - hip
    bend = bend - (bend * direction).sum(axis=1, keepdims=True) * direction
    bend = bend / np.maximum(np.linalg.norm(bend, axis=1, keepdims=True), 1e-9)
    new_knee = hip + upper[:, None] * (np.cos(angle)[:, None] * direction + np.sin(angle)[:, None] * bend)
    return new_knee, goal


def _rotate_bone(rotation, head, old_tail, new_tail):
    """Rotate a bone's global rotation so its head->tail direction follows."""
    swing = np.stack([rotation_between(o - h, n - h) for h, o, n in zip(head, old_tail, new_tail)])
    return swing @ rotation


def _contact_offsets(target, side):
    """Heel and ball floor points in the foot bone's rest frame: the heel
    3.5 cm behind the ankle and the ball under the ball joint, both on the
    floor when the foot rests flat."""
    foot = target.index(f"foot_{side}")
    ankle = target.head(f"foot_{side}")
    heel = np.array([0.0, -0.035, -ankle[2]])
    ball = target.head(f"ball_{side}") - ankle
    ball[2] = -ankle[2]
    rest = target.rest[foot, :3, :3]
    return rest.T @ heel, rest.T @ ball


def plant_feet(clip, target, rotation, position, hip_scale):
    """Pin heel and ball contacts detected on the source, then solve the legs.

    Contact points follow the source's scaled path; while a point is in
    contact its path is replaced by the point where the stance began moving
    at the loop's constant backward velocity, so playback at the clip speed
    keeps the foot fixed on the ground."""
    take = clip.take
    fps = take.fps
    frames = take.frames
    velocity = -clip.travel * clip.speed * hip_scale if clip.loop else np.zeros(3)
    times = np.arange(frames) / fps
    rotation = rotation.copy()
    position = position.copy()
    # Contacts are detected on the source in the world frame: an in-place
    # loop's planted foot slides backwards at the travel speed.
    travel = (clip.travel * clip.speed)[None] if clip.loop else np.zeros((1, 3))
    contacts = {}
    for side in ("l", "r"):
        heel_src, ball_src = foot_points(take, side)
        contacts[side] = (contact_mask(heel_src + times[:, None] * travel, fps),
                          contact_mask(ball_src + times[:, None] * travel, fps))

    # Mocap stance legs are nearly straight, so the IK cannot push a hovering
    # foot down. Lower the whole body by the forward-kinematics contact
    # height plus a small margin; the knees take the slack.
    heights = []
    for side in ("l", "r"):
        foot = target.index(f"foot_{side}")
        for contact, offset in zip(contacts[side], _contact_offsets(target, side)):
            if contact.any():
                point = position[contact, foot] + np.einsum("fij,j->fi", rotation[contact, foot], offset)
                heights.append(point[:, 2])
    if heights:
        # The root stays at rest; everything it carries drops.
        drop = np.median(np.concatenate(heights)) + 0.004
        moving = np.array([b != target.index("root") for b in range(len(target.names))])
        position[:, moving, 2] -= drop

    # Loops are processed cyclically: three copies side by side, so stances
    # and eases that straddle the seam see their neighbours, and the middle
    # copy is kept.
    copies = 3 if clip.loop else 1
    span = frames * copies
    first = frames if clip.loop else 0
    span_times = (np.arange(span) - first) / fps
    world = span_times[:, None] * -velocity[None]
    for side in ("l", "r"):
        thigh = target.index(f"thigh_{side}")
        calf = target.index(f"calf_{side}")
        foot = target.index(f"foot_{side}")
        local_heel, local_ball = _contact_offsets(target, side)
        foot_rotation = np.tile(rotation[:, foot], (copies, 1, 1))
        ankle = np.tile(position[:, foot], (copies, 1))

        def points(offset):
            return ankle + np.einsum("fij,j->fi", foot_rotation, offset)

        # Contacts on the mannequin's own lowered feet, in the world frame.
        # A point at floor level is down even while its source still skates
        # after touchdown (common at running impact), up to 1.5 m/s.
        heel_contact = contact_mask(points(local_heel) + world, fps, height=0.012, speed=1.5)
        ball_contact = contact_mask(points(local_ball) + world, fps, height=0.012, speed=1.5)
        if not clip.loop:
            # A one-shot's first frame only has a one-sided velocity; at floor
            # level before a planted frame it belongs to the same stance.
            for contact, offset in ((heel_contact, local_heel), (ball_contact, local_ball)):
                if span > 1 and contact[1] and points(offset)[0, 2] < 0.012:
                    contact[0] = True
        if clip.airborne is not None:
            grounded = ~np.tile(clip.airborne, copies)
            heel_contact &= grounded
            ball_contact &= grounded
        both = heel_contact & ball_contact

        # A foot with heel and ball down lies flat: pitch it about its
        # lateral axis until both points share a height.
        flat = smooth(both.astype(np.float64), 5)
        heel_point = points(local_heel)
        ball_point = points(local_ball)
        along = ball_point - heel_point
        length = np.linalg.norm(along, axis=1)
        pitch = np.arcsin(np.clip((ball_point[:, 2] - heel_point[:, 2]) / np.maximum(length, 1e-6), -1.0, 1.0))
        lateral = _normalize(np.cross(along, np.array([0.0, 0.0, 1.0])))
        foot_rotation = axis_angle(lateral, pitch * flat) @ foot_rotation

        # Anchors per contact, in the world frame. A point touching down
        # while its foot is already planted elsewhere takes its anchor from
        # that planted pose, so heel and ball agree at the handover. Through
        # a flat-foot run the pivot rolls between them, keeping the goal
        # continuous at both ends of the run.
        heel_path = points(local_heel)
        ball_path = points(local_ball)
        release = 6
        # Weight of the ball anchor through each flat-foot run: from the point
        # that was down before the run to the one that stays down after it
        # (heel to ball walking forwards, ball to heel walking backwards).
        roll = np.zeros(span)
        for run_start, run_stop in _runs(both):
            before = 1.0 if run_start > 0 and ball_contact[run_start - 1] and not heel_contact[run_start - 1] else 0.0
            after = 0.0 if run_stop < span and heel_contact[run_stop] and not ball_contact[run_stop] else 1.0
            roll[run_start:run_stop] = np.linspace(before, after, run_stop - run_start)
        planted = np.full((span, 3), np.nan)
        heel_anchor = None
        ball_anchor = None
        # While both points are planted the source foot may still yaw (side
        # steps pivot); turning the foot to the anchors' heel-to-ball line
        # keeps both points still. The last correction then decays so the
        # foot pivots about the point that stays down.
        yaw_left = 0.0
        yaw_decay = 0
        up = np.array([0.0, 0.0, 1.0])
        for f in range(span):
            if not heel_contact[f]:
                heel_anchor = None
            if not ball_contact[f]:
                ball_anchor = None
            if heel_anchor is not None and ball_anchor is not None:
                wanted = (ball_anchor - heel_anchor)[:2]
                current = (foot_rotation[f] @ (local_ball - local_heel))[:2]
                yaw = np.arctan2(current[0] * wanted[1] - current[1] * wanted[0], current @ wanted)
                foot_rotation[f] = axis_angle(up, yaw) @ foot_rotation[f]
                yaw_left = yaw
                yaw_decay = release
            elif yaw_decay > 0:
                yaw_decay -= 1
                foot_rotation[f] = axis_angle(up, yaw_left * yaw_decay / release) @ foot_rotation[f]
            heel_offset = foot_rotation[f] @ local_heel
            ball_offset = foot_rotation[f] @ local_ball
            if heel_contact[f] and heel_anchor is None:
                if ball_anchor is not None:
                    heel_anchor = ball_anchor - ball_offset + heel_offset
                else:
                    heel_anchor = heel_path[f] + world[f]
                heel_anchor[2] = 0.0
            if ball_contact[f] and ball_anchor is None:
                if heel_anchor is not None:
                    ball_anchor = heel_anchor - heel_offset + ball_offset
                else:
                    ball_anchor = ball_path[f] + world[f]
                ball_anchor[2] = 0.0
            if heel_anchor is not None and ball_anchor is not None:
                from_heel = heel_anchor - heel_offset
                from_ball = ball_anchor - ball_offset
                planted[f] = from_heel * (1.0 - roll[f]) + from_ball * roll[f] - world[f]
            elif heel_anchor is not None:
                planted[f] = heel_anchor - heel_offset - world[f]
            elif ball_anchor is not None:
                planted[f] = ball_anchor - ball_offset - world[f]
        # Hold goals exactly through each stance. Before touchdown and after
        # lift-off, the lock's offset from the forward-kinematics foot fades
        # over a short window while the foot is in the air.
        goal = ankle.copy()
        lead_in = 4
        for run_start, run_stop in _runs(heel_contact | ball_contact):
            goal[run_start:run_stop] = planted[run_start:run_stop]
            first_offset = planted[run_start] - ankle[run_start]
            last_offset = planted[run_stop - 1] - ankle[run_stop - 1]
            for k in range(1, lead_in + 1):
                f = run_start - k
                if f >= 0:
                    goal[f] += first_offset * (1.0 - smoothstep(0.0, lead_in + 1.0, float(k)))
            for k in range(release):
                f = run_stop + k
                if f < span:
                    goal[f] += last_offset * (1.0 - smoothstep(0.0, float(release), float(k + 1)))
        # No contact point below the floor, and a swinging foot clears it:
        # a point moving faster than a stance point may rises to 1 cm.
        heel_now = goal + np.einsum("fij,j->fi", foot_rotation, local_heel)
        ball_now = goal + np.einsum("fij,j->fi", foot_rotation, local_ball)
        clearance = np.zeros(span)
        for path in (heel_now, ball_now):
            speed = np.linalg.norm((np.gradient(path + world, axis=0) * fps)[:, :2], axis=1)
            clearance = np.maximum(clearance, 0.01 * smoothstep(0.3, 0.8, speed) - path[:, 2])
        clearance[heel_contact | ball_contact] = 0.0
        # The swing clearance eases in and out; penetration is removed exactly.
        penetration = np.maximum(0.0, -np.minimum(heel_now[:, 2], ball_now[:, 2]))
        goal[:, 2] += np.maximum(smooth(np.maximum(clearance, 0.0), 3), penetration)
        goal = goal[first:first + frames]
        rotation[:, foot] = foot_rotation[first:first + frames]

        pole = position[:, calf] + (position[:, calf] - 0.5 * (position[:, thigh] + position[:, foot])) * 4.0
        new_knee, new_ankle = _two_bone_ik(position[:, thigh], position[:, calf], position[:, foot], goal, pole)
        rotation[:, thigh] = _rotate_bone(rotation[:, thigh], position[:, thigh], position[:, calf], new_knee)
        # The calf keeps its orientation relative to the new knee position.
        rotation[:, calf] = _rotate_bone(rotation[:, calf], position[:, calf], position[:, foot],
                                         position[:, calf] + (new_ankle - new_knee))
        position[:, calf] = new_knee
        position[:, foot] = new_ankle
        # Twist bones follow their re-aimed parents.
        for b, name in enumerate(target.names):
            if target.parents[b] in (thigh, calf) and b not in (calf, foot):
                parent = target.parents[b]
                local = target.rest[parent, :3, :3].T @ target.rest[b, :3, :3]
                rotation[:, b] = rotation[:, parent] @ local
                offset = target.rest[parent, :3, :3].T @ (target.rest[b, :3, 3] - target.rest[parent, :3, 3])
                position[:, b] = position[:, parent] + np.einsum("fij,j->fi", rotation[:, parent], offset)
        # The ball bone rides the (possibly flattened) foot.
        ball = target.index(f"ball_{side}")
        offset = target.rest[foot, :3, :3].T @ (target.rest[ball, :3, 3] - target.rest[foot, :3, 3])
        position[:, ball] = position[:, foot] + np.einsum("fij,j->fi", rotation[:, foot], offset)
        # The ball keeps its own pose relative to the foot.
    return orthonormalize(rotation), position


# =============================================================================
# Output
# =============================================================================

def local_pose(target, rotation, position):
    """Per bone local rotation quaternions (F, B, 4, wxyz) relative to the
    rest pose, and pelvis translation offsets in its parent's rest frame
    (F, 3), for Blender pose bones (matrix_basis)."""
    frames = len(rotation)
    count = len(target.names)
    basis = np.zeros((frames, count, 3, 3))
    translation = np.zeros((frames, count, 3))
    for b in range(count):
        parent = target.parents[b]
        rest = target.rest[b, :3, :3]
        if parent < 0:
            parent_global = np.broadcast_to(np.eye(3), (frames, 3, 3))
            parent_rest = np.eye(3)
            parent_head = np.zeros((frames, 3))
            parent_rest_head = np.zeros(3)
        else:
            parent_global = rotation[:, parent]
            parent_rest = target.rest[parent, :3, :3]
            parent_head = position[:, parent]
            parent_rest_head = target.rest[parent, :3, 3]
        rest_local = parent_rest.T @ rest
        posed_local = np.swapaxes(parent_global, -1, -2) @ rotation[:, b]
        basis[:, b] = np.swapaxes(rest_local, -1, -2) @ posed_local
        # Head offset in the parent's frame compared with the rest offset,
        # expressed in the bone's rest frame (Blender pose location).
        offset_now = np.einsum("fji,fj->fi", parent_global, position[:, b] - parent_head)
        offset_rest = parent_rest.T @ (target.rest[b, :3, 3] - parent_rest_head)
        translation[:, b] = (offset_now - offset_rest) @ rest_local
    quaternions = continuous_quaternions(matrix_to_quaternion(orthonormalize(basis)))
    return quaternions, translation
