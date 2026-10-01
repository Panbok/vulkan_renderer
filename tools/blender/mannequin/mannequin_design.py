"""The mannequin's shell layout: light armour plates over a dark carbon suit
that shows in the channels between them and at the joints, in the design
language of a sleek test mannequin (a faceplate helmet on a strong neck, a
chest plate over chevron bands, capped shoulders and knees, segmented
fingers) with VKR's own layout and details.

`layout(rig)` returns mannequin_shell entries for the character's right
side and the midline; entries mirror themselves to the left unless marked
otherwise. Uncovered surface keeps the suit tone (`BASE_TONE`). Limb panels
follow the rig's joints so the layout moves with the skeleton; torso and
helmet outlines are drawn in orthographic views in metres.
"""

import numpy as np

import mannequin_base as mb
from mannequin_shell import (Band, Box, Capsule, Curve, Ellipsoid, HalfSpace, Plane, Plate, Region, Seam,
                             Tube, Underlayer, Unrolled)


BASE_TONE = "under"
RIM_POINT = tuple(mb.RIM_POINT)
RIM_NORMAL = tuple(mb.RIM_NORMAL)
BELOW_RIM = tuple(-mb.RIM_NORMAL)
# The neck's axis: the collar and the neck ribs ring it.
NECK_LOW = (0.0, -0.004, 1.38)
NECK_HIGH = (0.0, -0.004, 1.62)
# Looking down on the character: X to the right, forward up the image.
TOP = ((1.0, 0.0, 0.0), (0.0, 1.0, 0.0), (0.0, 0.0, 1.0))


def _mirror_x(points):
    """Close a right-side outline across the midline: the points run from
    the top of the midline around the right side back to the midline."""
    return list(points) + [(-u, v) for u, v in reversed(points[1:-1])]


def _circle(center, radius, count=20):
    angles = np.linspace(0.0, 2.0 * np.pi, count, endpoint=False)
    return [(center[0] + radius * np.cos(a), center[1] + radius * np.sin(a)) for a in angles]


def _rounded_rect(u0, u1, v0, v1, corner):
    """A rectangle in a parameter plane with corners rounded by `corner`."""
    c = corner
    return [(u0 + c, v0), (u1 - c, v0), (u1, v0 + c), (u1, v1 - c), (u1 - c, v1), (u0 + c, v1), (u0, v1 - c),
            (u0, v0 + c)]


def _along(a, b, t):
    return tuple(np.asarray(a) + (np.asarray(b) - np.asarray(a)) * t)


def _head():
    """Helmet: a shell raised over the jaw line, a faceplate and chin guard,
    temple slots with a fin, ear discs, cheek vents, a rear cap."""
    head = Region("head")
    helmet = [head, HalfSpace(RIM_POINT, RIM_NORMAL, 0.003)]
    rim = tuple(np.asarray(RIM_POINT) + 0.006 * np.asarray(RIM_NORMAL))
    # A long teardrop, widest across the brow and narrowing to the chin.
    faceplate = _mirror_x([(0.000, 1.786), (0.024, 1.781), (0.046, 1.765), (0.061, 1.740), (0.067, 1.710),
                           (0.066, 1.680), (0.060, 1.654), (0.047, 1.628), (0.031, 1.605), (0.015, 1.591),
                           (0.000, 1.586)])
    chin = _mirror_x([(0.000, 1.577), (0.016, 1.579), (0.032, 1.587), (0.042, 1.595), (0.040, 1.581),
                      (0.030, 1.567), (0.014, 1.558), (0.000, 1.555)])
    slot = [(0.070, 1.784), (0.084, 1.776), (0.080, 1.744), (0.066, 1.708), (0.046, 1.676), (0.030, 1.664),
            (0.024, 1.672), (0.034, 1.698), (0.046, 1.736), (0.056, 1.770)]
    fin = [(0.070, 1.770), (0.066, 1.742), (0.054, 1.710), (0.038, 1.684), (0.042, 1.682), (0.059, 1.708),
           (0.072, 1.740), (0.076, 1.768)]
    ear = (-0.012, 1.665)
    rear = _mirror_x([(0.000, 1.738), (0.040, 1.732), (0.066, 1.708), (0.074, 1.672), (0.066, 1.642),
                      (0.040, 1.624), (0.000, 1.619)])
    return [
        # The head's underside and the neck below the helmet are suit.
        Underlayer(Plane(rim, RIM_NORMAL, [head]), recess=0.002, bevel=0.002, mirror=False),
        Plate(Plane(rim, BELOW_RIM, [head]), height=0.003, bevel=0.0025, groove=0.0018, depth=0.0018,
              mirror=False),
        Plate(Curve("front", faceplate, closed=True, masks=helmet, facing=0.2), height=0.0025, bevel=0.002,
              groove=0.0014, depth=0.0015, stack=True, mirror=False),
        Plate(Curve("front", chin, closed=True, masks=helmet, facing=0.1), height=0.002, bevel=0.002,
              groove=0.0014, depth=0.0015, stack=True, mirror=False),
        Underlayer(Curve("front", _rounded_rect(-0.016, 0.016, 1.5665, 1.5705, 0.0018), closed=True,
                         masks=helmet, facing=0.1), recess=0.0015, bevel=0.001, mirror=False),
        Underlayer(Curve("side", slot, closed=True, masks=helmet, facing=0.3), recess=0.004, bevel=0.002),
        Plate(Curve("side", fin, closed=True, masks=helmet, facing=0.3), height=0.003, bevel=0.0015, groove=0.0,
              tone="accent", stack=True),
        Plate(Curve("side", _circle(ear, 0.022), closed=True, masks=helmet, facing=0.4), height=0.003,
              bevel=0.002, groove=0.0014, depth=0.0014, stack=True),
        Seam(Curve("side", _circle(ear, 0.0145), closed=True, masks=helmet, facing=0.4), width=0.0010,
             depth=0.0008, fine=True),
        Underlayer(Curve("side", _circle(ear, 0.006), closed=True, masks=helmet, facing=0.4), recess=0.002,
                   bevel=0.001, fine=True),
        *[Underlayer(Curve("side", _rounded_rect(0.048 - k * 0.002, 0.068 - k * 0.002, z, z + 0.0032, 0.0014),
                           closed=True, masks=helmet, facing=0.3), recess=0.0015, bevel=0.0008, fine=True)
          for k, z in enumerate((1.630, 1.620, 1.610))],
        Plate(Curve("back", rear, closed=True, masks=helmet, facing=0.2), height=0.002, bevel=0.002,
              groove=0.0012, depth=0.0012, stack=True, mirror=False),
        # Panel lines from the faceplate's peak back over the crown.
        Seam(Curve(TOP, [(0.026, 0.100), (0.030, 0.040), (0.028, -0.020), (0.020, -0.075)],
                   masks=helmet + [HalfSpace((0.0, 0.0, 1.745), (0.0, 0.0, 1.0))], facing=0.2),
             width=0.0010, depth=0.0009, fine=True),
    ]


def _neck_and_collar():
    """A dark ribbed neck rising from a raised collar ring."""
    torso = Region("torso")
    neck_zone = Capsule(NECK_LOW, NECK_HIGH, 0.071, 0.006)
    return [
        Plate(Tube(NECK_LOW, NECK_HIGH, (0.128, 0.098), masks=[torso, HalfSpace(RIM_POINT, BELOW_RIM)]),
              height=0.0045, bevel=0.0025, groove=0.002, depth=0.002, mirror=False),
        Underlayer(Tube(NECK_LOW, NECK_HIGH, (0.074, 0.080), masks=[HalfSpace((0.0, 0.0, 1.40), (0.0, 0.0, 1.0)),
                                                                    HalfSpace(RIM_POINT, BELOW_RIM)]),
                   recess=0.003, bevel=0.003, mirror=False),
        Underlayer(Curve("front", _circle((0.0, 1.452), 0.012), closed=True, masks=[torso]), recess=0.003,
                   bevel=0.0015, mirror=False),
        *[Plate(Band((0.0, 0.0, z - 0.004), (0.0, 0.0, z + 0.004), 0.0, 1.0, radius=0.09, masks=[neck_zone]),
                height=0.002, bevel=0.0015, groove=0.0, tone="under", stack=True, mirror=False)
          for z in (1.492, 1.514)],
        # Cords from under the jaw down to the collar.
        Plate(Curve("front", [(0.030, 1.566), (0.040, 1.566), (0.050, 1.520), (0.058, 1.470), (0.046, 1.470),
                              (0.038, 1.520)], closed=True, masks=[neck_zone], facing=0.1),
              height=0.0025, bevel=0.002, groove=0.0, tone="under", stack=True),
    ]


def _torso_front():
    torso = [Region("torso")]
    chest = _mirror_x([(0.000, 1.418), (0.050, 1.430), (0.100, 1.448), (0.140, 1.444), (0.168, 1.418),
                       (0.184, 1.372), (0.184, 1.326), (0.170, 1.290), (0.140, 1.266), (0.100, 1.256),
                       (0.060, 1.250), (0.025, 1.240), (0.000, 1.228)])
    chevron = [(0.070, 1.408), (0.112, 1.408), (0.0, 1.300), (-0.112, 1.408), (-0.070, 1.408), (0.0, 1.345)]
    band_upper = _mirror_x([(0.000, 1.214), (0.060, 1.232), (0.120, 1.252), (0.168, 1.282), (0.186, 1.296),
                            (0.188, 1.262), (0.176, 1.236), (0.130, 1.212), (0.070, 1.192), (0.000, 1.176)])
    band_lower = _mirror_x([(0.000, 1.164), (0.062, 1.180), (0.118, 1.198), (0.160, 1.222), (0.178, 1.238),
                            (0.180, 1.204), (0.166, 1.182), (0.124, 1.160), (0.068, 1.144), (0.000, 1.130)])
    abdomen = _mirror_x([(0.000, 1.120), (0.046, 1.128), (0.062, 1.118), (0.064, 1.080), (0.058, 1.040),
                         (0.046, 1.004), (0.030, 0.978), (0.000, 0.968)])
    oblique = [(0.076, 1.136), (0.112, 1.148), (0.150, 1.168), (0.166, 1.146), (0.164, 1.094), (0.152, 1.048),
               (0.128, 1.018), (0.094, 1.002), (0.068, 0.998), (0.072, 1.044), (0.076, 1.094)]
    brief = _mirror_x([(0.000, 0.958), (0.036, 0.968), (0.064, 0.984), (0.094, 0.990), (0.130, 1.004),
                       (0.160, 1.024), (0.178, 1.040), (0.182, 1.000), (0.179, 0.962), (0.160, 0.934),
                       (0.120, 0.900), (0.080, 0.868), (0.046, 0.848), (0.020, 0.839), (0.000, 0.837)])
    flank = [(0.035, 1.330), (0.050, 1.290), (0.052, 1.200), (0.046, 1.110), (0.040, 1.050), (0.000, 1.030),
             (-0.040, 1.050), (-0.050, 1.120), (-0.055, 1.210), (-0.050, 1.300), (-0.025, 1.335)]
    return [
        Underlayer(Curve("side", flank, closed=True, masks=torso, facing=0.35), recess=0.002, bevel=0.003),
        Plate(Curve("front", chest, closed=True, masks=torso), height=0.0045, bevel=0.0025, mirror=False),
        Plate(Curve("front", chevron, closed=True, masks=torso, smooth=False), height=0.002, bevel=0.0015,
              groove=0.0012, depth=0.0012, stack=True, mirror=False),
        Plate(Curve("front", band_upper, closed=True, masks=torso), height=0.0035, bevel=0.0025, mirror=False),
        Plate(Curve("front", band_lower, closed=True, masks=torso), height=0.0035, bevel=0.0025, mirror=False),
        Plate(Curve("front", abdomen, closed=True, masks=torso), height=0.004, bevel=0.0025, mirror=False),
        *[Seam(Curve("front", [(-0.07, z), (0.07, z)], masks=torso), width=0.0014, depth=0.0012, mirror=False)
          for z in (1.086, 1.038)],
        Plate(Curve("front", oblique, closed=True, masks=torso), height=0.003, bevel=0.0025),
        Plate(Curve("front", brief, closed=True, masks=torso), height=0.004, bevel=0.0025, mirror=False),
        Underlayer(Ellipsoid((0.0, 0.02, 0.828), (0.045, 0.09, 0.040)), recess=0.002, bevel=0.004, mirror=False),
        *[Underlayer(Curve("front", _circle(c, 0.0028, 12), closed=True, masks=torso), recess=0.0012,
                     bevel=0.0008, fine=True) for c in ((0.140, 1.428), (0.172, 1.306))],
    ]


def _torso_back():
    torso = [Region("torso")]
    blade = [(0.022, 1.455), (0.070, 1.462), (0.120, 1.455), (0.160, 1.430), (0.180, 1.390), (0.178, 1.330),
             (0.160, 1.285), (0.120, 1.262), (0.075, 1.262), (0.040, 1.280), (0.024, 1.320)]
    lat = [(0.090, 1.246), (0.140, 1.255), (0.176, 1.270), (0.176, 1.200), (0.162, 1.150), (0.132, 1.120),
           (0.092, 1.110), (0.062, 1.130), (0.052, 1.180), (0.060, 1.230)]
    lumbar = _mirror_x([(0.000, 1.250), (0.030, 1.248), (0.040, 1.200), (0.044, 1.140), (0.054, 1.096),
                        (0.046, 1.070), (0.000, 1.062)])
    glute = [(0.016, 1.036), (0.070, 1.042), (0.130, 1.036), (0.168, 1.022), (0.176, 0.978), (0.166, 0.924),
             (0.138, 0.878), (0.096, 0.854), (0.052, 0.850), (0.018, 0.860)]
    return [
        Underlayer(Curve("back", _rounded_rect(-0.014, 0.014, 1.262, 1.470, 0.006), closed=True, masks=torso),
                   recess=0.003, bevel=0.002, mirror=False),
        *[Plate(Curve("back", _rounded_rect(-0.010, 0.010, z - 0.020, z, 0.004), closed=True, masks=torso),
                height=0.0035, bevel=0.0015, groove=0.0012, depth=0.0012, tone="accent", mirror=False)
          for z in (1.452, 1.424, 1.396, 1.368, 1.340, 1.312)],
        Plate(Curve("back", blade, closed=True, masks=torso), height=0.0045, bevel=0.0025),
        Plate(Curve("back", lat, closed=True, masks=torso), height=0.0035, bevel=0.0025),
        Plate(Curve("back", lumbar, closed=True, masks=torso), height=0.004, bevel=0.0025, mirror=False),
        *[Underlayer(Curve("back", _rounded_rect(-0.022, 0.022, z, z + 0.0035, 0.0016), closed=True, masks=torso),
                     recess=0.0015, bevel=0.0008, fine=True, mirror=False) for z in (1.200, 1.189, 1.178)],
        Plate(Curve("back", glute, closed=True, masks=torso), height=0.0045, bevel=0.0025),
        *[Underlayer(Curve("back", _circle(c, 0.0028, 12), closed=True, masks=torso), recess=0.0012,
                     bevel=0.0008, fine=True) for c in ((0.150, 1.424), (0.162, 1.306))],
    ]


def _arm(j, rig):
    shoulder, elbow, wrist = j["upperarm_r"], j["lowerarm_r"], j["hand_r"]
    upper = [Region("upperarm_r")]
    fore = [Region("forearm_r")]
    hand = [Region("hand_r")]
    upper_length = float(np.linalg.norm(elbow - shoulder))
    fore_length = float(np.linalg.norm(wrist - elbow))
    cap = shoulder + np.array([0.036, 0.0, 0.028])
    shoulder_zone = [Box((0.13, -0.2, 1.28), (0.40, 0.2, 1.60), 0.02)]
    elbow_zone = [Capsule(tuple(_along(shoulder, elbow, 0.75)), tuple(_along(elbow, wrist, 0.25)), 0.07)]
    outward = (1.0, 0.0, 0.0)
    entries = [
        Underlayer(Ellipsoid(tuple(shoulder + np.array([-0.012, 0.0, -0.088])), (0.045, 0.068, 0.058),
                             masks=[Box((0.10, -0.2, 1.20), (0.30, 0.2, 1.45), 0.01)]), recess=0.003, bevel=0.004),
        # A two-layer shoulder cap.
        Plate(Ellipsoid(tuple(cap), (0.076, 0.088, 0.092), masks=shoulder_zone), height=0.0055, bevel=0.003),
        Plate(Ellipsoid(tuple(cap + np.array([0.006, 0.0, 0.010])), (0.058, 0.066, 0.068), masks=shoulder_zone),
              height=0.0025, bevel=0.002, groove=0.0012, depth=0.0012, stack=True),
        # The upper-arm sleeve, the inner arm dark.
        Underlayer(Unrolled(tuple(shoulder), tuple(elbow), outward,
                            _rounded_rect(0.125, 0.225, 0.16 * upper_length, 0.94 * upper_length, 0.012),
                            radius=0.055, reach=0.09, masks=upper), recess=0.002, bevel=0.003),
        Plate(Unrolled(tuple(shoulder), tuple(elbow), outward,
                       _rounded_rect(-0.075, 0.085, 0.30 * upper_length, 0.84 * upper_length, 0.014),
                       radius=0.055, reach=0.09, masks=upper), height=0.004, bevel=0.0025),
        # Elbow cap over the point of the elbow, the inner elbow dark.
        Plate(Ellipsoid(tuple(elbow + np.array([0.010, -0.036, 0.004])), (0.034, 0.026, 0.044), masks=elbow_zone),
              height=0.004, bevel=0.0025),
        Underlayer(Band(tuple(shoulder), tuple(wrist), 0.0, 1.0, radius=0.2,
                        masks=elbow_zone + [HalfSpace(tuple(elbow), (0.0, 1.0, 0.0), 0.004),
                                            Capsule(tuple(_along(shoulder, elbow, 0.93)),
                                                    tuple(_along(elbow, wrist, 0.07)), 0.06, 0.006)]),
                   recess=0.0015, bevel=0.003),
        # Forearm guard, wrist cuff, the wrist joint dark.
        Plate(Unrolled(tuple(elbow), tuple(wrist), outward,
                       [(-0.050, 0.16 * fore_length), (0.000, 0.12 * fore_length), (0.050, 0.16 * fore_length),
                        (0.052, 0.50 * fore_length), (0.040, 0.78 * fore_length), (0.000, 0.80 * fore_length),
                        (-0.040, 0.78 * fore_length), (-0.052, 0.50 * fore_length)],
                       radius=0.044, reach=0.08, masks=fore), height=0.0045, bevel=0.0025),
        Seam(Unrolled(tuple(elbow), tuple(wrist), outward,
                      [(-0.040, 0.46 * fore_length), (0.040, 0.46 * fore_length)], closed=False,
                      radius=0.044, reach=0.08, masks=fore), width=0.0011, depth=0.0009, fine=True),
        Plate(Band(tuple(elbow), tuple(wrist), 0.855, 0.955, radius=0.07, masks=fore), height=0.0035, bevel=0.002),
        Underlayer(Band(tuple(elbow), tuple(wrist), 0.965, 1.05, radius=0.07, masks=[Capsule(tuple(elbow),
                        tuple(_along(elbow, wrist, 1.2)), 0.07)]), recess=0.0015, bevel=0.002),
    ]
    # Palm dark; back of the hand plated; a plate on every finger segment
    # with dark joints between.
    knuckle = j["middle_01_r"]
    dorsal = np.cross(knuckle - wrist, j["index_01_r"] - j["pinky_01_r"])
    dorsal = dorsal / np.linalg.norm(dorsal)
    if dorsal[0] < 0.0:
        dorsal = -dorsal
    center = wrist + 0.5 * (knuckle - wrist)
    entries.append(Underlayer(Plane(tuple(center - 0.004 * dorsal), tuple(dorsal), hand), recess=0.0015,
                              bevel=0.003))
    entries.append(Plate(Ellipsoid(tuple(wrist + 0.55 * (knuckle - wrist) + 0.012 * dorsal), (0.024, 0.034, 0.030),
                                   masks=hand + [Capsule(tuple(wrist), tuple(knuckle), 0.05)]),
                         height=0.0025, bevel=0.002, groove=0.0012, depth=0.0012))
    for finger in ("thumb", "index", "middle", "ring", "pinky"):
        for segment in (1, 2, 3):
            if finger == "thumb" and segment == 1:
                continue
            bone = rig.bones[f"{finger}_0{segment}_r"]
            region = [Region(f"{finger}_r")]
            entries.append(Plate(Band(tuple(bone.head), tuple(bone.tail), 0.14, 0.88, radius=0.016, masks=region),
                                 height=0.0012, bevel=0.0012, groove=0.0008, depth=0.0008))
            entries.append(Underlayer(Band(tuple(bone.head), tuple(bone.tail), -0.10, 0.08, radius=0.016,
                                           masks=region), recess=0.0008, bevel=0.0010))
    return entries


def _leg(j):
    hip, knee, ankle = j["thigh_r"], j["calf_r"], j["foot_r"]
    thigh = [Region("thigh_r")]
    shin = [Region("shin_r")]
    boot = [Region("boot_r")]
    thigh_length = float(np.linalg.norm(knee - hip))
    shin_length = float(np.linalg.norm(ankle - knee))
    knee_zone = [Capsule(tuple(_along(hip, knee, 0.8)), tuple(_along(knee, ankle, 0.2)), 0.10)]
    forward = (0.0, 1.0, 0.0)
    backward = (0.0, -1.0, 0.0)
    ball = j["ball_r"]
    toe_axis = ball - ankle
    toe_axis[2] = 0.0
    toe_axis = toe_axis / np.linalg.norm(toe_axis)
    upper = [HalfSpace((0.0, 0.0, 0.030), (0.0, 0.0, 1.0))]
    return [
        # Inner thigh dark; a front plate with an outer wing and a panel
        # line across.
        Underlayer(Unrolled(tuple(hip), tuple(knee), forward,
                            _rounded_rect(0.105, 0.185, 0.10 * thigh_length, 0.80 * thigh_length, 0.02),
                            radius=0.085, reach=0.14, masks=thigh), recess=0.002, bevel=0.003),
        Plate(Unrolled(tuple(hip), tuple(knee), forward,
                       [(-0.140, 0.10 * thigh_length), (-0.060, 0.14 * thigh_length), (0.020, 0.20 * thigh_length),
                        (0.070, 0.26 * thigh_length), (0.070, 0.66 * thigh_length), (0.040, 0.82 * thigh_length),
                        (-0.040, 0.84 * thigh_length), (-0.100, 0.74 * thigh_length), (-0.150, 0.40 * thigh_length)],
                       radius=0.085, reach=0.14, masks=thigh), height=0.0045, bevel=0.0025),
        Seam(Unrolled(tuple(hip), tuple(knee), forward,
                      [(-0.140, 0.52 * thigh_length), (-0.040, 0.56 * thigh_length), (0.060, 0.54 * thigh_length)],
                      closed=False, radius=0.085, reach=0.14, masks=thigh), width=0.0012, depth=0.0010, fine=True),
        # The back of the thigh: a plate under the glute, a channel to the
        # front plate's wing.
        Plate(Unrolled(tuple(hip), tuple(knee), backward,
                       [(-0.070, 0.22 * thigh_length), (0.010, 0.18 * thigh_length), (0.090, 0.22 * thigh_length),
                        (0.092, 0.60 * thigh_length), (0.060, 0.80 * thigh_length), (-0.050, 0.80 * thigh_length),
                        (-0.072, 0.60 * thigh_length)],
                       radius=0.085, reach=0.14, masks=thigh), height=0.004, bevel=0.0025),
        # Back of the knee dark, a two-layer knee cap.
        Underlayer(Ellipsoid(tuple(knee + np.array([0.0, -0.050, 0.004])), (0.052, 0.030, 0.060), masks=knee_zone),
                   recess=0.002, bevel=0.003),
        Plate(Ellipsoid(tuple(knee + np.array([0.0, 0.052, 0.008])), (0.048, 0.032, 0.064), masks=knee_zone),
              height=0.0055, bevel=0.003),
        Plate(Ellipsoid(tuple(knee + np.array([0.0, 0.060, 0.016])), (0.032, 0.026, 0.042), masks=knee_zone),
              height=0.0025, bevel=0.002, groove=0.0012, depth=0.0012, stack=True),
        # Shin greave and calf plate with vents, the ankle dark.
        Plate(Unrolled(tuple(knee), tuple(ankle), forward,
                       [(-0.060, 0.14 * shin_length), (0.000, 0.10 * shin_length), (0.050, 0.14 * shin_length),
                        (0.052, 0.60 * shin_length), (0.034, 0.88 * shin_length), (-0.040, 0.88 * shin_length),
                        (-0.062, 0.60 * shin_length)],
                       radius=0.055, reach=0.10, masks=shin), height=0.0045, bevel=0.0025),
        Plate(Unrolled(tuple(knee), tuple(ankle), backward,
                       _rounded_rect(-0.066, 0.062, 0.14 * shin_length, 0.70 * shin_length, 0.020),
                       radius=0.055, reach=0.10, masks=shin), height=0.0035, bevel=0.0025),
        *[Underlayer(Unrolled(tuple(knee), tuple(ankle), backward,
                              _rounded_rect(-0.022, 0.022, t * shin_length, t * shin_length + 0.0035, 0.0016),
                              radius=0.055, reach=0.10, masks=shin), recess=0.0015, bevel=0.0008, fine=True)
          for t in (0.30, 0.33, 0.36)],
        Underlayer(Band(tuple(knee), tuple(ankle), 0.92, 1.10, radius=0.08,
                        masks=[Capsule(tuple(knee), tuple(ankle), 0.08)]), recess=0.002, bevel=0.003),
        # Boot: a dark sole under a shell upper with toe and heel caps.
        Underlayer(Plane((0.0, 0.0, 0.030), (0.0, 0.0, 1.0), boot), recess=0.0, bevel=0.002),
        Plate(Plane(tuple(ball - 0.015 * toe_axis), tuple(-toe_axis), boot + upper),
              height=0.002, bevel=0.003, groove=0.0012, depth=0.0012),
        Plate(Ellipsoid(tuple(ankle + np.array([0.0, -0.045, -0.035])), (0.045, 0.030, 0.045), masks=boot + upper),
              height=0.002, bevel=0.003, groove=0.0012, depth=0.0012),
        Seam(Plane((0.0, 0.0, 0.014), (0.0, 0.0, 1.0), boot), width=0.0012, depth=0.0010, fine=True),
    ]


def _screws(view, centers, masks, radius=0.0026):
    """Screw heads: a recessed ring with a raised centre."""
    entries = []
    for center in centers:
        entries.append(Underlayer(Curve(view, _circle(center, radius, 14), closed=True, masks=masks),
                                  recess=0.0010, bevel=0.0007, fine=True))
        entries.append(Plate(Curve(view, _circle(center, radius * 0.55, 10), closed=True, masks=masks),
                             height=0.0008, bevel=0.0006, groove=0.0, stack=True, fine=True))
    return entries


def _hatch(view, u0, u1, v0, v1, masks, mirror=True):
    """A recessed maintenance hatch: a groove outline with screws at two
    corners."""
    entries = [Seam(Curve(view, _rounded_rect(u0, u1, v0, v1, 0.004), closed=True, masks=masks),
                    width=0.0010, depth=0.0010, fine=True, mirror=mirror)]
    for entry in _screws(view, [(u0 + 0.005, v1 - 0.005), (u1 - 0.005, v0 + 0.005)], masks, 0.0018):
        entry.mirror = mirror
        entries.append(entry)
    return entries


def _details(j):
    """Small parts that give the shell its wear and purpose: screws, hatches,
    vents, panel lines and paint marks, all normal map only."""
    torso = [Region("torso")]
    shoulder, elbow, wrist = j["upperarm_r"], j["lowerarm_r"], j["hand_r"]
    hip, knee, ankle = j["thigh_r"], j["calf_r"], j["foot_r"]
    upper_length = float(np.linalg.norm(elbow - shoulder))
    fore_length = float(np.linalg.norm(wrist - elbow))
    thigh_length = float(np.linalg.norm(knee - hip))
    shin_length = float(np.linalg.norm(ankle - knee))
    outward = (1.0, 0.0, 0.0)
    forward = (0.0, 1.0, 0.0)
    cap = shoulder + np.array([0.036, 0.0, 0.028])
    entries = [
        # Collar and chest fixings.
        *_screws("front", [(0.105, 1.462), (0.118, 1.300)], torso),
        # A panel line across the chest plate under the chevron.
        Seam(Curve("front", [(-0.150, 1.300), (-0.060, 1.286), (0.0, 1.282), (0.060, 1.286), (0.150, 1.300)],
                   masks=torso), width=0.0009, depth=0.0008, fine=True, mirror=False),
        # Back: blade fixings and a hatch on each lat plate.
        *_screws("back", [(0.100, 1.444), (0.150, 1.300)], torso),
        *_hatch("back", 0.100, 0.150, 1.150, 1.215, torso),
        # Glutes: a panel line.
        Seam(Curve("back", [(0.040, 0.960), (0.100, 0.968), (0.160, 0.955)], masks=torso),
             width=0.0009, depth=0.0008, fine=True),
        # Shoulder cap: a ring line on the lower layer and a pair of screws.
        Seam(Ellipsoid(tuple(cap), (0.064, 0.074, 0.078), masks=[Box((0.13, -0.2, 1.28), (0.40, 0.2, 1.60), 0.02)]),
             width=0.0009, depth=0.0008, fine=True),
        # Upper arm: a hatch on the sleeve and a paint stripe pair.
        Seam(Unrolled(tuple(shoulder), tuple(elbow), outward,
                      _rounded_rect(-0.030, 0.030, 0.52 * upper_length, 0.70 * upper_length, 0.005),
                      radius=0.055, reach=0.09, masks=[Region("upperarm_r")]), width=0.0010, depth=0.0010,
             fine=True),
        *[Plate(Unrolled(tuple(shoulder), tuple(elbow), outward,
                         _rounded_rect(-0.070, 0.080, t * upper_length, t * upper_length + 0.004, 0.001),
                         radius=0.055, reach=0.09, masks=[Region("upperarm_r")]),
                height=0.0, bevel=0.0008, groove=0.0, tone="accent", paint=True, fine=True)
          for t in (0.34, 0.37)],
        # Forearm: a vent grille by the cuff.
        *[Underlayer(Unrolled(tuple(elbow), tuple(wrist), outward,
                              _rounded_rect(-0.020, 0.020, t * fore_length, t * fore_length + 0.003, 0.0012),
                              radius=0.044, reach=0.08, masks=[Region("forearm_r")]),
                     recess=0.0012, bevel=0.0007, fine=True) for t in (0.64, 0.67, 0.70)],
        # Thigh: a hatch on the outer wing, a paint mark by the hip.
        Seam(Unrolled(tuple(hip), tuple(knee), forward,
                      _rounded_rect(-0.130, -0.080, 0.24 * thigh_length, 0.40 * thigh_length, 0.005),
                      radius=0.085, reach=0.14, masks=[Region("thigh_r")]), width=0.0010, depth=0.0010, fine=True),
        *[Plate(Unrolled(tuple(hip), tuple(knee), forward,
                         _rounded_rect(u, u + 0.004, 0.16 * thigh_length, 0.34 * thigh_length, 0.001),
                         radius=0.085, reach=0.14, masks=[Region("thigh_r")]),
                height=0.0, bevel=0.0008, groove=0.0, tone="accent", paint=True, fine=True)
          for u in (-0.066, -0.058)],
        # Shin: a panel line across the greave's lower third.
        Seam(Unrolled(tuple(knee), tuple(ankle), forward,
                      [(-0.058, 0.64 * shin_length), (0.0, 0.66 * shin_length), (0.050, 0.64 * shin_length)],
                      closed=False, radius=0.055, reach=0.10, masks=[Region("shin_r")]),
             width=0.0009, depth=0.0008, fine=True),
    ]
    return entries


def layout(rig):
    j = {name: np.asarray(rig.bones[name].head, dtype=np.float64) for name in rig.bones}
    return [
        *_neck_and_collar(),
        *_head(),
        *_torso_front(),
        *_torso_back(),
        *_arm(j, rig),
        *_leg(j),
        *_details(j),
    ]
