"""Generates the clustered-binning Bistro fixtures (clustered binning
proposal, M0 and M3).

Finds open street cells in the Bistro street view (ground below 1 m, nothing
overhead for 5 m) by rasterising bistro-lights.gltf onto a 0.5 m xz grid, then
places short-range lights (some on movers) and ground decals there. The output
is deterministic: `clustered` writes bistro_clustered_local.scene.json (320
lights) and `m3` bistro_clustered_m3_local.scene.json (20,000 lights, past the
desktop's 4,096 shaded rows).
Run: python3 -I tools/gen_clustered_scene.py <repo root> [clustered|m3] [out]
"""

import json
import math
import random
import struct
import sys

ROOT = sys.argv[1]
GLTF = ROOT + "/assets/models/bistro-lights.gltf"
BIN = ROOT + "/assets/models/bistro-lights.bin"
BASE = ROOT + "/assets/scenes/fixtures/bistro_lamps_current_night_local.scene.json"
VARIANT = sys.argv[2] if len(sys.argv) > 2 else "clustered"

CAMERA = (-28.4774647, 4.60749865, 2.86034966)
YAW = math.radians(228.125198)
FORWARD = (math.cos(YAW), math.sin(YAW))
RIGHT = (-FORWARD[1], FORWARD[0])
NEAR_M_DEFAULT = 2.0
FAR_M_DEFAULT = 45.0
HALF_TAN = 1.15  # a little inside tan(51 deg), the 16:9 half width at 70 deg
CELL = 0.5

# lights, movers (spinners included), spinners, decals, light spacing,
# light heights, candidate heights per cell, range, intensity, decal spacing,
# decal size, seed, compact light entities.
VARIANTS = {
    "clustered": dict(out="bistro_clustered_local", lights=320, movers=64,
                      spins=16, decals=176, spacing=1.0, heights=(0.4, 3.0),
                      per_cell=3, range=(2.0, 4.0), intensity=(3.0, 8.0),
                      decal_spacing=1.0, decal_size=(1.0, 2.5), seed=20261026,
                      compact=False, prefix="Clustered"),
    "m3": dict(out="bistro_clustered_m3_local", lights=20000, movers=1024,
                  spins=256, decals=2000, spacing=0.25, heights=(0.2, 4.8),
                  per_cell=48, range=(0.75, 1.5), intensity=(0.5, 1.5),
                  decal_spacing=0.25, decal_size=(0.5, 1.5), seed=20261029,
                  compact=True, prefix="Dense", jitter=True, far=90.0, near=6.0),
}
V = VARIANTS[VARIANT]
FAR_M = V.get("far", FAR_M_DEFAULT)
# The ground nearer than about 6 m lies below the view's bottom edge.
NEAR_M = V.get("near", NEAR_M_DEFAULT)
OUT = (sys.argv[3] if len(sys.argv) > 3 else
       ROOT + "/assets/scenes/fixtures/%s.scene.json" % V["out"])
LIGHT_COUNT = V["lights"]
MOVER_COUNT = V["movers"]
SPIN_COUNT = V["spins"]
DECAL_COUNT = V["decals"]


def mat_mul(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)]
            for r in range(4)]


def node_matrix(node):
    if "matrix" in node:
        m = node["matrix"]
        return [[m[c * 4 + r] for c in range(4)] for r in range(4)]
    t = node.get("translation", [0, 0, 0])
    q = node.get("rotation", [0, 0, 0, 1])
    s = node.get("scale", [1, 1, 1])
    x, y, z, w = q
    rot = [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]
    return [[rot[r][0] * s[0], rot[r][1] * s[1], rot[r][2] * s[2], t[r]]
            for r in range(3)] + [[0, 0, 0, 1]]


def in_view(x, z):
    dx = x - CAMERA[0]
    dz = z - CAMERA[2]
    along = dx * FORWARD[0] + dz * FORWARD[1]
    side = dx * RIGHT[0] + dz * RIGHT[1]
    return NEAR_M <= along <= FAR_M and abs(side) <= along * HALF_TAN


def region_bounds():
    corners = []
    for along in (NEAR_M, FAR_M):
        for sign in (-1.0, 1.0):
            side = sign * along * HALF_TAN
            corners.append((CAMERA[0] + FORWARD[0] * along + RIGHT[0] * side,
                            CAMERA[2] + FORWARD[1] * along + RIGHT[1] * side))
    xs = [c[0] for c in corners]
    zs = [c[1] for c in corners]
    return min(xs), max(xs), min(zs), max(zs)


def accessor_reader(gltf, data, index):
    acc = gltf["accessors"][index]
    view = gltf["bufferViews"][acc["bufferView"]]
    offset = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    count = acc["count"]
    comp = {5126: ("f", 4), 5125: ("I", 4), 5123: ("H", 2), 5121: ("B", 1)}
    fmt, size = comp[acc["componentType"]]
    width = {"SCALAR": 1, "VEC3": 3}[acc["type"]]
    stride = view.get("byteStride", size * width)
    if stride == size * width:
        flat = struct.unpack_from("<%d%s" % (count * width, fmt), data, offset)
        return [flat[i * width:(i + 1) * width] for i in range(count)]
    return [struct.unpack_from("<%d%s" % (width, fmt), data, offset + i * stride)
            for i in range(count)]


def surface_heights():
    gltf = json.load(open(GLTF))
    data = open(BIN, "rb").read()
    min_x, max_x, min_z, max_z = region_bounds()
    nx = int((max_x - min_x) / CELL) + 1
    nz = int((max_z - min_z) / CELL) + 1
    heights = {}
    identity = [[1 if r == c else 0 for c in range(4)] for r in range(4)]
    stack = [(n, identity) for n in gltf["scenes"][0]["nodes"]]
    while stack:
        index, parent = stack.pop()
        node = gltf["nodes"][index]
        world = mat_mul(parent, node_matrix(node))
        for child in node.get("children", []):
            stack.append((child, world))
        if "mesh" not in node:
            continue
        for prim in gltf["meshes"][node["mesh"]]["primitives"]:
            if prim.get("mode", 4) != 4:
                continue
            local = accessor_reader(gltf, data, prim["attributes"]["POSITION"])
            pos = [(world[0][0] * p[0] + world[0][1] * p[1] + world[0][2] * p[2] + world[0][3],
                    world[1][0] * p[0] + world[1][1] * p[1] + world[1][2] * p[2] + world[1][3],
                    world[2][0] * p[0] + world[2][1] * p[1] + world[2][2] * p[2] + world[2][3])
                   for p in local]
            if "indices" in prim:
                idx = [i[0] for i in accessor_reader(gltf, data, prim["indices"])]
            else:
                idx = list(range(len(pos)))
            for t in range(0, len(idx) - 2, 3):
                a = pos[idx[t]]
                b = pos[idx[t + 1]]
                c = pos[idx[t + 2]]
                tx0 = min(a[0], b[0], c[0])
                tx1 = max(a[0], b[0], c[0])
                tz0 = min(a[2], b[2], c[2])
                tz1 = max(a[2], b[2], c[2])
                if tx1 < min_x or tx0 > max_x or tz1 < min_z or tz0 > max_z:
                    continue
                det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
                if abs(det) < 1.0e-9:
                    continue
                i0 = max(0, int(math.ceil((tx0 - min_x) / CELL - 0.5)))
                i1 = min(nx - 1, int(math.floor((tx1 - min_x) / CELL - 0.5)))
                j0 = max(0, int(math.ceil((tz0 - min_z) / CELL - 0.5)))
                j1 = min(nz - 1, int(math.floor((tz1 - min_z) / CELL - 0.5)))
                for i in range(i0, i1 + 1):
                    x = min_x + (i + 0.5) * CELL
                    for j in range(j0, j1 + 1):
                        z = min_z + (j + 0.5) * CELL
                        l0 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / det
                        l1 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / det
                        l2 = 1.0 - l0 - l1
                        if l0 < 0.0 or l1 < 0.0 or l2 < 0.0:
                            continue
                        y = l0 * a[1] + l1 * b[1] + l2 * c[1]
                        heights.setdefault((i, j), []).append(y)
    cells = []
    for (i, j), hs in heights.items():
        x = min_x + (i + 0.5) * CELL
        z = min_z + (j + 0.5) * CELL
        if not in_view(x, z):
            continue
        below = [h for h in hs if h < 1.0]
        if not below:
            continue
        ground = max(below)
        if ground < -1.0:
            continue
        if any(ground + 0.05 < h < ground + 5.0 for h in hs):
            continue
        cells.append((x, ground, z))
    cells.sort()
    return cells


def pick_spaced(points, count, spacing, rng, axes):
    """Shuffles the points and keeps each at least `spacing` from those kept
    before it, in the given axes; a hash of spacing-wide buckets finds the
    neighbours, so the result equals the all-pairs test."""
    order = points[:]
    rng.shuffle(order)
    chosen = []
    buckets = {}
    for point in order:
        key = tuple(int(math.floor(point[k] / spacing)) for k in axes)
        near = False
        for offset in range(3 ** len(axes)):
            probe = list(key)
            for n in range(len(axes)):
                probe[n] += (offset // (3 ** n)) % 3 - 1
            for o in buckets.get(tuple(probe), ()):
                if sum((point[k] - o[k]) ** 2 for k in axes) < spacing * spacing:
                    near = True
                    break
            if near:
                break
        if near:
            continue
        chosen.append(point)
        buckets.setdefault(key, []).append(point)
        if len(chosen) == count:
            break
    return chosen


def spaced(cells, count, spacing, rng):
    return pick_spaced(cells, count, spacing, rng, (0, 2))


def spaced3(points, count, spacing, rng):
    return pick_spaced(points, count, spacing, rng, (0, 1, 2))


def transform(pos, yaw=0.0, scale=(1.0, 1.0, 1.0)):
    return {
        "pos": [round(v, 3) for v in pos],
        "rot": [0.0, round(math.sin(yaw * 0.5), 6), 0.0, round(math.cos(yaw * 0.5), 6)],
        "scale": [round(v, 3) for v in scale],
    }


PALETTE = [
    [1.0, 0.78, 0.5], [1.0, 0.6, 0.3], [0.55, 0.75, 1.0], [0.9, 0.4, 1.0],
    [0.4, 1.0, 0.6], [1.0, 0.35, 0.35], [1.0, 1.0, 0.85], [0.35, 0.9, 1.0],
]

DECAL_MATERIALS = [
    "assets/materials/bistro-lights/gltf_mat_0f0a4adcc4741230_42.mt",
    "assets/materials/bistro-lights/gltf_mat_0f0a4adcc4741230_43.mt",
    "assets/materials/bistro-lights/gltf_mat_0f0a4adcc4741230_44.mt",
]


def main():
    rng = random.Random(V["seed"])
    cells = surface_heights()
    print("open street cells in view:", len(cells))
    print("ground min/max:", min(c[1] for c in cells), max(c[1] for c in cells))
    # Lights pack in 3D: several heights per open cell, 1 m apart.
    if V.get("jitter"):
        # Jittered inside the cell, so a column holds more than one xz.
        half = 0.5 * CELL
        stacked = [(x + rng.uniform(-half, half),
                    ground + rng.uniform(*V["heights"]),
                    z + rng.uniform(-half, half))
                   for (x, ground, z) in cells for _ in range(V["per_cell"])]
    else:
        stacked = [(x, ground + rng.uniform(*V["heights"]), z)
                   for (x, ground, z) in cells for _ in range(V["per_cell"])]
    light_points = spaced3(stacked, LIGHT_COUNT, V["spacing"], rng)
    light_cells = [(x, y, z) for (x, y, z) in light_points]
    # Decals go on the street and pavement, not on terrace tables.
    decal_ground = [c for c in cells if c[1] < 0.45]
    if V.get("jitter"):
        half = 0.5 * CELL
        decal_ground = [(x + rng.uniform(-half, half), ground,
                         z + rng.uniform(-half, half))
                        for (x, ground, z) in decal_ground for _ in range(8)]
    decal_cells = spaced(decal_ground, DECAL_COUNT, V["decal_spacing"], rng)
    print("lights:", len(light_cells), "decals:", len(decal_cells))
    if len(light_cells) < LIGHT_COUNT or len(decal_cells) < DECAL_COUNT:
        sys.exit("not enough open street cells")

    scene = json.load(open(BASE))
    entities = scene["entities"]
    # The harness moves this group at a case's motion_blur_entity_velocity,
    # since it runs no simulation; in Play the movers below it run too.
    group = len(entities)
    entities.append({"name": "%s Moving Lights" % V["prefix"], "parent": None,
                     "transform": transform((0.0, 0.0, 0.0))})
    for n, (x, height, z) in enumerate(light_cells):
        spot = n % 5 == 4
        light = {
            "enabled": True,
            "kind": 2 if spot else 1,
            "color": PALETTE[n % len(PALETTE)],
            "intensity": round(rng.uniform(*V["intensity"]), 2),
            "range": round(rng.uniform(*V["range"]), 2),
            "casts_shadow": False,
            "mobility": "dynamic",
        }
        if V["compact"]:
            # Loader defaults: enabled, no shadow, no parent, identity
            # rotation and scale.
            del light["enabled"]
            del light["casts_shadow"]
        if spot:
            light["direction_local"] = [0.0, -1.0, 0.0]
            if not V["compact"]:
                light["inner_cone_angle"] = 0.5
                light["outer_cone_angle"] = 0.85
        name = "%s Light %d" % (V["prefix"], n + 1)
        if n < MOVER_COUNT - SPIN_COUNT:
            angle = rng.uniform(0.0, 2.0 * math.pi)
            entities.append({
                "name": name + " Rail", "parent": group,
                "transform": transform((x, height, z)),
                "components": {"mover": {
                    "activation": "scripted",
                    "direction": [round(math.cos(angle), 4), 0.0, round(math.sin(angle), 4)],
                    "distance": round(rng.uniform(1.5, 3.5), 2),
                    "speed": round(rng.uniform(0.5, 1.5), 2),
                    "wait": 0.5, "loop": True}}})
            entities.append({"name": name, "parent": len(entities) - 1,
                             "transform": transform((0.0, 0.0, 0.0)),
                             "point_light": light})
        elif n < MOVER_COUNT:
            entities.append({
                "name": name + " Spinner", "parent": group,
                "transform": transform((x, height, z)),
                "components": {"mover": {
                    "activation": "scripted", "spin": True, "start_open": True,
                    "axis": [0.0, 1.0, 0.0],
                    "speed": round(rng.uniform(45.0, 120.0), 1)}}})
            entities.append({"name": name, "parent": len(entities) - 1,
                             "transform": transform((1.0, 0.0, 0.0)),
                             "point_light": light})
        elif V["compact"]:
            entities.append({"transform": {
                "pos": [round(v, 3) for v in (x, height, z)]},
                "point_light": light})
        else:
            entities.append({"name": name, "parent": None,
                             "transform": transform((x, height, z)),
                             "point_light": light})
    for n, (x, ground, z) in enumerate(decal_cells):
        size = rng.uniform(*V["decal_size"])
        entities.append({
            "name": "%s Decal %d" % (V["prefix"], n + 1), "parent": None,
            "transform": transform((x, ground + 0.3, z),
                                   rng.uniform(0.0, 2.0 * math.pi),
                                   (size, 1.0, size * rng.uniform(0.6, 1.0))),
            "components": {"decal": {
                "enabled": True,
                "opacity": round(rng.uniform(0.6, 1.0), 2),
                "depth_fade": 0.1,
                "sort_order": rng.randrange(3),
                "material": DECAL_MATERIALS[n % len(DECAL_MATERIALS)]}}})
    with open(OUT, "w") as f:
        f.write("{\n")
        keys = [k for k in scene if k != "entities"]
        for k in keys:
            f.write('  "%s": %s,\n' % (k, json.dumps(scene[k])))
        f.write('  "entities": [\n')
        separators = (",", ":") if V["compact"] else None
        f.write(",\n".join("    " + json.dumps(e, separators=separators)
                            for e in entities))
        f.write("\n  ]\n}\n")
    print("wrote", OUT, "entities:", len(entities))


main()
