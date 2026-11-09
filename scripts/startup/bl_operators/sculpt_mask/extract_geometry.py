# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Pure geometry helpers for the live Extract feature (no UI, no global state).

Kept as its own module so the mesh math can be unit-tested on its own (see the test task).
Everything here is a plain function over a ``Mesh``/numpy; the per-object draft build
(``extract_parts``) and the operator build on top of it.
"""

import math
from itertools import compress

import bmesh
from sculpt_mesh_common import FACE_SET_ATTRIBUTE, MASK_ATTRIBUTE

# A face is kept when all its corners are masked at least this much, and it is not hidden.
MASK_THRESHOLD = 0.5
# The edge band: the boundary and a few rings in, fading. Name of the vertex group the smooth
# modifier is limited to.
EDGE_RINGS = (1.0, 0.6, 0.3)
EDGE_GROUP = "Extract Edge"

# Polygroup colors, as the overlay picks their hues (keep in step).
GOLDEN_RATIO_CONJUGATE = 0.618033988749895
DEFAULT_HUE = 0.0235
RED_BAND = 0.06


def _hue_distance(a, b):
    d = abs(a - b) % 1.0
    return min(d, 1.0 - d)


def polygroup_hue(face_set, default_face_set, seed):
    """The hue a polygroup is drawn in."""
    if face_set == default_face_set:
        return DEFAULT_HUE
    hue = GOLDEN_RATIO_CONJUGATE * (face_set + math.fmod(seed, 10))
    hue -= math.floor(hue)
    from_scarlet = hue - DEFAULT_HUE
    from_scarlet -= math.floor(from_scarlet + 0.5)
    if abs(from_scarlet) < RED_BAND:
        hue += -2.0 * RED_BAND if from_scarlet < 0.0 else 2.0 * RED_BAND
        hue -= math.floor(hue)
    return hue


def mesh_polygroup_hues(mesh):
    """The hues of the polygroups a mesh shows (one when it has none)."""
    import numpy as np
    attr = mesh.attributes.get(FACE_SET_ATTRIBUTE)
    ids = [1]
    if attr is not None and attr.domain == 'FACE' and len(attr.data):
        values = np.empty(len(attr.data), dtype=np.int32)
        attr.data.foreach_get("value", values)
        ids = [int(i) for i in np.unique(values)]
    return [polygroup_hue(i, mesh.face_sets_color_default, mesh.face_sets_color_seed) for i in ids]


def distinct_seed(ids, avoid_hues, default_face_set=0):
    """A color seed for polygroups `ids` (none of them the default) whose hues are as far as can
    be from `avoid_hues`."""
    def score(seed):
        hues = [polygroup_hue(i, default_face_set, seed) for i in ids]
        return min(_hue_distance(h, a) for h in hues for a in avoid_hues)
    # Only the seed's last digit moves the hues.
    return max(range(10), key=score)


def _face_adjacency(mesh):
    """Loops partitioned per face, as arrays: `corner` (loop -> vertex index), `starts` (face ->
    first loop) and the vertex count. Faces are contiguous runs of loops in a Blender mesh, so
    ``np.add.reduceat`` over `starts` aggregates per face."""
    import numpy as np
    corner = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corner)
    starts = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", starts)
    return corner, starts, len(mesh.vertices)


def _hidden_flags(mesh):
    """Per-face bool: the face is hidden in sculpt. All-false when there is no hide attribute."""
    import numpy as np
    hide = mesh.attributes.get(".hide_poly")
    if hide is None:
        return np.zeros(len(mesh.polygons), dtype=bool)
    hidden = np.empty(len(mesh.polygons), dtype=bool)
    hide.data.foreach_get("value", hidden)
    return hidden


def _loop_to_face(starts, loop_count):
    """Which face each loop belongs to, from the per-face first-loop offsets."""
    import numpy as np
    return np.searchsorted(starts, np.arange(loop_count), side="right") - 1


def _faces_touching(corner, starts, vertex_mask):
    """Per-face bool: at least one of the face's vertices is set in `vertex_mask`."""
    import numpy as np
    per_loop = vertex_mask[corner].astype(np.int32)
    return np.add.reduceat(per_loop, starts) > 0


def grow_faces(mesh, keep, rings):
    """Faces within `rings` vertex-adjacent steps of `keep` (one ring: a face sharing any vertex
    with the current set). Hidden faces are never grown in. Negative `rings` shrinks."""
    import numpy as np
    keep = np.asarray(keep, dtype=bool).copy()
    if rings == 0:
        return keep
    if rings < 0:
        return shrink_faces(mesh, keep, rings)
    corner, starts, vertex_count = _face_adjacency(mesh)
    loop_to_face = _loop_to_face(starts, len(corner))
    for _ in range(rings):
        used = np.zeros(vertex_count, dtype=bool)
        used[corner[keep[loop_to_face]]] = True
        new = _faces_touching(corner, starts, used) & ~keep
        if not new.any():
            break
        keep |= new
    return keep & ~_hidden_flags(mesh)


def shrink_faces(mesh, keep, rings):
    """Drop the boundary of `keep` `|rings|` times: a face survives only when none of its vertices
    is shared with a face outside the set. Never returns empty -- if a step would, it stops."""
    import numpy as np
    keep = np.asarray(keep, dtype=bool).copy()
    if rings >= 0:
        return keep
    corner, starts, vertex_count = _face_adjacency(mesh)
    loop_to_face = _loop_to_face(starts, len(corner))
    for _ in range(-rings):
        boundary = np.zeros(vertex_count, dtype=bool)
        boundary[corner[~keep[loop_to_face]]] = True
        touches = _faces_touching(corner, starts, boundary)
        survivors = keep & ~touches
        if not survivors.any():
            break
        keep = survivors
    return keep


def extract_keep(mesh, grow=0, threshold=MASK_THRESHOLD):
    """The faces to extract: the mask, grown or shrunk by `grow` rings. None without a mask (like
    :func:`masked_faces`)."""
    import numpy as np
    base = masked_faces(mesh, threshold)
    if base is None:
        return None
    if grow == 0:
        return base
    out = grow_faces(mesh, base, int(grow)) if grow > 0 else shrink_faces(mesh, base, int(grow))
    return out


def _edge_weights(bm, edge_rings):
    """{vertex index: weight} fading from the boundary inward over `edge_rings` rings; the first
    ring is 1.0 and the last approaches 0 (linear). Empty for ``edge_rings <= 0``."""
    weights = {}
    if edge_rings <= 0:
        return weights
    bm.verts.index_update()
    ring = [v for v in bm.verts if v.is_boundary]
    for k in range(edge_rings):
        weight = 1.0 - k / float(edge_rings)
        nxt = []
        for v in ring:
            if v.index not in weights:
                weights[v.index] = weight
                nxt += [e.other_vert(v) for e in v.link_edges]
        ring = nxt
        if not ring:
            break
    return weights


def masked_faces(mesh, threshold=MASK_THRESHOLD):
    """One bool per face: all its corners masked (at least `threshold`) and not hidden. None
    without a mask on the mesh itself."""
    import numpy as np
    mask_attr = mesh.attributes.get(MASK_ATTRIBUTE)
    if mask_attr is None or mask_attr.domain != 'POINT':
        return None
    masks = np.empty(len(mesh.vertices), dtype=np.float32)
    mask_attr.data.foreach_get("value", masks)
    corner_verts = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corner_verts)
    starts = np.empty(len(mesh.polygons), dtype=np.int32)
    mesh.polygons.foreach_get("loop_start", starts)
    unmasked = (masks[corner_verts] < threshold).astype(np.int32)
    keep = np.add.reduceat(unmasked, starts) == 0 if len(starts) else np.zeros(0, dtype=bool)
    hide = mesh.attributes.get(".hide_poly")
    if hide is not None:
        hidden = np.empty(len(mesh.polygons), dtype=bool)
        hide.data.foreach_get("value", hidden)
        keep &= ~hidden
    return keep


def extracted_mesh(source, keep, edge_rings=3):
    """A copy of `source` with only the faces in `keep`, without mask, polygroups or hiding, and
    with its edge band in the `EDGE_GROUP` weights (returned as {vertex: weight}). `edge_rings`
    is the width of the fading edge band."""
    mesh = source.copy()
    mesh.name = source.name + " Extract"
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bm.faces.ensure_lookup_table()
    bmesh.ops.delete(bm, geom=list(compress(bm.faces, (not k for k in keep))), context='FACES')
    for elems in (bm.verts, bm.edges, bm.faces):
        for elem in elems:
            elem.hide = False
    weights = _edge_weights(bm, edge_rings)
    bm.to_mesh(mesh)
    bm.free()
    for name in (MASK_ATTRIBUTE, FACE_SET_ATTRIBUTE):
        attr = mesh.attributes.get(name)
        if attr is not None:
            mesh.attributes.remove(attr)
    mesh.update()
    return mesh, weights
