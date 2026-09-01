#!/usr/bin/env python3
"""Minimal binary-STL inspector: bbox, watertightness, genus (through-holes)."""
import struct, sys
from collections import defaultdict


def load(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:5].lower() == b"solid" and b"facet" in data[:512]:
        raise SystemExit(f"{path}: ASCII STL not supported")
    (count,) = struct.unpack_from("<I", data, 80)
    tris = []
    off = 84
    for _ in range(count):
        vals = struct.unpack_from("<12f", data, off)
        tris.append((vals[3:6], vals[6:9], vals[9:12]))
        off += 50
    return tris


def analyze(path):
    tris = load(path)
    # weld vertices on a 1e-4 mm grid
    key = lambda v: (round(v[0], 4), round(v[1], 4), round(v[2], 4))
    index, verts = {}, []
    faces = []
    for tri in tris:
        f = []
        for v in tri:
            k = key(v)
            if k not in index:
                index[k] = len(verts)
                verts.append(k)
            f.append(index[k])
        if len(set(f)) == 3:          # drop degenerate facets
            faces.append(tuple(f))

    edges = defaultdict(int)
    for a, b, c in faces:
        for x, y in ((a, b), (b, c), (c, a)):
            edges[(min(x, y), max(x, y))] += 1

    V, E, F = len(verts), len(edges), len(faces)
    boundary = sum(1 for n in edges.values() if n == 1)
    nonmanifold = sum(1 for n in edges.values() if n > 2)
    euler = V - E + F
    # connected components over faces
    adj = defaultdict(list)
    for i, (a, b, c) in enumerate(faces):
        for x, y in ((a, b), (b, c), (c, a)):
            adj[(min(x, y), max(x, y))].append(i)
    seen, comps = set(), 0
    for i in range(len(faces)):
        if i in seen:
            continue
        comps += 1
        stack = [i]
        seen.add(i)
        while stack:
            cur = stack.pop()
            a, b, c = faces[cur]
            for x, y in ((a, b), (b, c), (c, a)):
                for nb in adj[(min(x, y), max(x, y))]:
                    if nb not in seen:
                        seen.add(nb)
                        stack.append(nb)

    xs = [v[0] for v in verts]; ys = [v[1] for v in verts]; zs = [v[2] for v in verts]
    watertight = boundary == 0 and nonmanifold == 0
    genus = (2 * comps - euler) / 2 if watertight else None

    print(f"{path}")
    print(f"  triangles {F}   vertices {V}   shells {comps}")
    print(f"  bbox  {max(xs)-min(xs):.2f} x {max(ys)-min(ys):.2f} x {max(zs)-min(zs):.2f} mm")
    print(f"  watertight {watertight}  (boundary edges {boundary}, non-manifold {nonmanifold})")
    print(f"  euler {euler}   genus {genus if genus is None else int(genus)}"
          f"{'   <- through-holes' if genus not in (None,) else ''}")
    print()


for p in sys.argv[1:]:
    analyze(p)
