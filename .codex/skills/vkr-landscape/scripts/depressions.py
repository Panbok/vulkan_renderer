#!/usr/bin/env python3
"""Find closed depressions in a float heightmap: pits erosion dug, and the
basins where a lake can stand. A priority flood from the grid's border fills
every depression to its spill level; each connected region filled deeper
than --min-depth is reported with its spill level, deepest point and area.

  depressions.py --heights main_h.f32 --n 1025 --origin X0 Z0 --step 2 \
      [--min-depth 3] [--top 12] [--filled out.f32]

A deep, small region next to an edge is an outlet pit (fill it); a broad,
shallow one on a valley floor is a lake basin (water at or under its spill
level stays inside it). --filled writes the filled heights.
"""
import argparse
import array
import heapq
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--heights", required=True, help="float32 row-major N x N heights")
    p.add_argument("--n", type=int, required=True)
    p.add_argument("--origin", type=float, nargs=2, required=True, metavar=("X0", "Z0"))
    p.add_argument("--step", type=float, required=True)
    p.add_argument("--min-depth", type=float, default=3.0)
    p.add_argument("--top", type=int, default=12)
    p.add_argument("--filled")
    a = p.parse_args()

    n = a.n
    h = array.array("f")
    with open(a.heights, "rb") as f:
        h.frombytes(f.read())
    if len(h) != n * n:
        sys.exit("%s holds %d floats, not %d x %d" % (a.heights, len(h), n, n))

    filled = array.array("f", h)
    seen = bytearray(n * n)
    queue = []
    for i in range(n):
        for k in (i, (n - 1) * n + i, i * n, i * n + n - 1):
            if not seen[k]:
                seen[k] = 1
                heapq.heappush(queue, (filled[k], k))
    while queue:
        v, k = heapq.heappop(queue)
        j, i = divmod(k, n)
        for jj, ii in ((j + 1, i), (j - 1, i), (j, i + 1), (j, i - 1)):
            if 0 <= jj < n and 0 <= ii < n:
                kk = jj * n + ii
                if not seen[kk]:
                    seen[kk] = 1
                    if filled[kk] < v:
                        filled[kk] = v
                    heapq.heappush(queue, (filled[kk], kk))

    # Connected regions filled deeper than the threshold.
    deep = bytearray(1 if filled[k] - h[k] > a.min_depth else 0 for k in range(n * n))
    regions = []
    for start in range(n * n):
        if not deep[start]:
            continue
        deep[start] = 0
        stack = [start]
        count, lowest, low_k = 0, float("inf"), start
        i_lo = j_lo = n
        i_hi = j_hi = -1
        spill = filled[start]
        while stack:
            k = stack.pop()
            j, i = divmod(k, n)
            count += 1
            if h[k] < lowest:
                lowest, low_k = h[k], k
            i_lo, i_hi, j_lo, j_hi = min(i_lo, i), max(i_hi, i), min(j_lo, j), max(j_hi, j)
            for jj, ii in ((j + 1, i), (j - 1, i), (j, i + 1), (j, i - 1)):
                if 0 <= jj < n and 0 <= ii < n:
                    kk = jj * n + ii
                    if deep[kk]:
                        deep[kk] = 0
                        stack.append(kk)
        edge = min(i_lo, j_lo, n - 1 - i_hi, n - 1 - j_hi) * a.step
        regions.append((count, spill, lowest, low_k, (i_lo, j_lo, i_hi, j_hi), edge))

    regions.sort(reverse=True)
    x0, z0, s = a.origin[0], a.origin[1], a.step
    print("%d regions deeper than %.1f m" % (len(regions), a.min_depth))
    for count, spill, lowest, low_k, (i0, j0, i1, j1), edge in regions[:a.top]:
        lj, li = divmod(low_k, n)
        print("area %8.0f m2  spill %8.2f  deepest %8.2f at (%.0f, %.0f)  x %.0f..%.0f z %.0f..%.0f  "
              "%.0f m from an edge" % (count * s * s, spill, lowest, x0 + li * s, z0 + lj * s,
                                       x0 + i0 * s, x0 + i1 * s, z0 + j0 * s, z0 + j1 * s, edge))
    if a.filled:
        with open(a.filled, "wb") as f:
            filled.tofile(f)


if __name__ == "__main__":
    main()
