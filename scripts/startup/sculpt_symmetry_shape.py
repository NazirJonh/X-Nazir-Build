# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Shape Only symmetry transfer with the masked area protected.

Mirrors the chosen source side of the mesh onto the other side without touching topology
(like ZBrush's "Mirror > Clone" shape transfer / Subdivision Symmetry "shape only"), while
every masked vertex keeps its shape: the transfer is applied with weight `1 - max(mask)`
per vertex, and masked, hidden or unselected coordinates are preserved bit for bit.

The heart is `topology_pairs`: a *complete* mirrored correspondence rebuilt from face
connectivity -- nearest-point pairs only seed it (`_seed_pairs`), then reversed face adjacency
propagates it (`_propagate`) and the result is validated edge by edge. A geometric match on its
own is never trusted on a deformed surface, and every check runs on a working copy before any
coordinate is written ("validate first, publish once"). After a transfer that could produce
folds, colliding neighbourhoods are frozen (`_find_folds`) and the blend re-runs through a
geodesic smoothstep collar (`_collar_blend`).

Only the topological method exists: a mesh whose two halves do not have a complete mirrored
correspondence is refused, and nothing is changed.
"""

import contextlib
import heapq
import math
from collections import deque
from statistics import median

import bmesh
import bpy
from bpy.props import EnumProperty, FloatProperty
from mathutils import Matrix
from mathutils.bvhtree import BVHTree
from mathutils.kdtree import KDTree

from sculpt_mesh_common import (LARGE_MESH_WARNING_FACES, MASK_ATTRIBUTE, object_mode,
                                single_user_problem)

# Correspondence tolerance floors, as a share of the mesh extent: the strict pass only seeds
# pairs that are unambiguously the same point, the loose pass may reach a deformed area. Two
# nearest hits closer than the tie margin are a tie, and a tie is dropped rather than guessed.
EXTENT_STRICT_RATIO = 1e-6
EXTENT_TIE_RATIO = 1e-9
MIN_STRICT_TOLERANCE = 1e-8
MIN_TIE = 1e-12

# How many times the blend is re-solved around the collisions before the transfer is refused.
MAX_BLEND_SOLVES = 24
# The collar the blend runs through, in mean edge lengths, for the default Blend Band: the band
# property is a share, so the widest band it can ask for is `transition_width` times this.
COLLAR_BAND_EDGES_PER_UNIT = 40.0
# Below this, a triangle that lost its area has no area to lose and is not called a fold.
MIN_TRIANGLE_AREA = 1e-15
# A triangle under this share of the area it had is a fold, not a sliver.
DEGENERATE_AREA_RATIO = 1e-6
# The self-intersection test tolerance, as a share of the mesh extent (and its floor, so a point
# mesh does not ask BVHTree for an epsilon of zero).
BVH_EPSILON_RATIO = 1e-7
BVH_EPSILON_FLOOR = 1e-20
# The narrowest collar worth growing, however small the edges are.
COLLAR_MIN_EXTENT_RATIO = 1e-5
# Two halves whose median coordinates this close apart cannot tell which one is the source.
AMBIGUOUS_SIDE_EPSILON = 1e-8
# A mesh with no extent at all (a point) still needs a usable seed distance.
MIN_MESH_DIAGONAL = 1e-6

# The seed distance is a share of the mesh's bounding-box diagonal, shown as a percentage. This is
# the default: on the 2-unit primitive sphere a new sculpt is made from, it gives the same 0.1
# units the absolute setting used to default to.
DEFAULT_SEED_DISTANCE_PERCENT = 3.0
DEFAULT_TRANSITION_WIDTH = 0.15


class CorrespondenceError(ValueError):
    pass


class _Correspondence:
    """The mirrored correspondence while it is rebuilt: the pair arrays, the face map and the
    faces still to propagate. Shared by `_seed_pairs` and `_propagate`."""

    def __init__(self, bm, vertices, faces):
        self.bm = bm
        self.vertices = vertices
        self.faces = faces
        self.mirror = [-1] * len(vertices)
        self.face_map = [-1] * len(faces)
        self.queue = deque()
        self.queued = bytearray(len(faces))


def _mirror_hits(coordinates, tolerance):
    """The nearest vertex to every reflected point, and the reciprocal pairs among them.

    Returns `(nearest, candidates, strict)`: `nearest[i]` is `(point, index, distance)` or None,
    `candidates` are the pairs each side of which found the other, and `strict` is the tolerance
    of the unambiguous pass.
    """
    tree = KDTree(len(coordinates))
    for i, p in enumerate(coordinates):
        tree.insert(p, i)
    tree.balance()

    reflected = [p.copy() for p in coordinates]
    for p in reflected:
        p.x = -p.x
    extent = max(max(p[k] for p in coordinates) - min(p[k] for p in coordinates)
                 for k in range(3))
    strict = min(tolerance, max(extent * EXTENT_STRICT_RATIO, MIN_STRICT_TOLERANCE))
    tie = max(extent * EXTENT_TIE_RATIO, MIN_TIE)
    nearest = []
    for p in reflected:
        hits = tree.find_n(p, 2)
        nearest.append(hits[0] if hits and (len(hits) < 2 or hits[1][2] - hits[0][2] > tie)
                       else None)
    candidates = []
    for i, hit in enumerate(nearest):
        if hit is None:
            continue
        _point, j, distance = hit
        if distance <= tolerance and nearest[j] is not None and nearest[j][1] == i and i <= j:
            candidates.append((i, j, distance))
    return nearest, candidates, strict


def _enqueue(state, vertex):
    """Queue the unpaired faces around `vertex` for `_propagate`."""
    for face in vertex.link_faces:
        if state.face_map[face.index] < 0 and not state.queued[face.index]:
            state.queued[face.index] = 1
            state.queue.append(face)


def _assign(state, i, j):
    """Record the pair (i, j) and queue the faces that can propagate from it."""
    mirror = state.mirror
    if mirror[i] not in (-1, j) or mirror[j] not in (-1, i):
        raise CorrespondenceError("Ambiguous topological correspondence: "
                                  "nothing was changed")
    if mirror[i] == j:
        return
    mirror[i] = j
    mirror[j] = i
    _enqueue(state, state.vertices[i])
    _enqueue(state, state.vertices[j])


def _mirror_side(state, face, a, b):
    """The faces that hold the edge (a, b) wound the other way round than `face`.

    Returns `(face, entry loop)` pairs, or None when the two vertices are not joined by an edge at
    all. Both the seeding and the propagation walk exactly this neighbourhood, so the same shape
    and mapping checks live here once.
    """
    edge = state.bm.edges.get((state.vertices[a], state.vertices[b]))
    if edge is None:
        return None
    found = []
    for loop in edge.link_loops:
        if loop.vert.index != b or loop.link_loop_next.vert.index != a:
            continue
        target = loop.link_loop_next
        other = target.face
        if (len(other.verts) != len(face.verts)
                or state.face_map[other.index] not in (-1, face.index)):
            continue
        found.append((other, target))
    return found


def _propagate(state):
    """Walk the queue: a face whose mirror is settled finds the face mirrored across one of its
    already-paired edges and pairs the whole ring."""
    while state.queue:
        face = state.queue.popleft()
        state.queued[face.index] = 0
        if state.face_map[face.index] >= 0:
            continue
        for start in face.loops:
            a = state.mirror[start.vert.index]
            b = state.mirror[start.link_loop_next.vert.index]
            if a < 0 or b < 0:
                continue
            mirrored = _mirror_side(state, face, a, b)
            if mirrored is None:
                raise CorrespondenceError(
                    "The topology has no complete mirror correspondence")
            possibilities = []
            for other, target_loop in mirrored:
                proposal = {}
                source_loop = start
                valid = True
                for _ in range(len(face.verts)):
                    i = source_loop.vert.index
                    j = target_loop.vert.index
                    if (state.mirror[i] not in (-1, j) or state.mirror[j] not in (-1, i)
                            or proposal.get(i, j) != j or proposal.get(j, i) != i):
                        valid = False
                        break
                    proposal[i] = j
                    proposal[j] = i
                    source_loop = source_loop.link_loop_next
                    target_loop = target_loop.link_loop_prev
                if valid:
                    possibilities.append((other, proposal))
            if not possibilities:
                raise CorrespondenceError(
                    "Incompatible face correspondence: nothing was changed")
            if len(possibilities) != 1:
                # More than one face could be the mirror: nothing is installed from a guess, the
                # next paired edge of this face decides it instead.
                continue
            other, proposal = possibilities[0]
            state.face_map[face.index] = other.index
            state.face_map[other.index] = face.index
            for i, j in proposal.items():
                _assign(state, i, j)
            break


def _seed_face(state, face, nearest, limit):
    """Establish one coherent face, and with it its connected component.

    Isolated nearest points in folded or deformed areas must not be installed as hard constraints
    beforehand, so a face is only seeded when every one of its vertices points back at the same
    ring the connectivity agrees with.
    """
    hits = []
    for loop in face.loops:
        i = loop.vert.index
        hit = nearest[i]
        if hit is None or hit[2] > limit:
            return False
        j = hit[1]
        if nearest[j] is None or nearest[j][1] != i or state.mirror[i] not in (-1, j):
            return False
        hits.append(j)
    mirrored = _mirror_side(state, face, hits[0], hits[1])
    if mirrored is None:
        return False
    options = []
    for other, walk in mirrored:
        for j in hits:
            if walk.vert.index != j:
                break
            walk = walk.link_loop_prev
        else:
            options.append(other)
    if len(options) != 1:
        return False
    other = options[0]
    state.face_map[face.index] = other.index
    state.face_map[other.index] = face.index
    for loop, j in zip(face.loops, hits):
        _assign(state, loop.vert.index, j)
    _propagate(state)
    return True


def _seed_pairs(state, nearest, limit):
    """Seed every still-unpaired face that matches geometrically, propagating as each one lands."""
    for face in state.faces:
        if state.face_map[face.index] < 0:
            _seed_face(state, face, nearest, limit)


def topology_pairs(bm, coordinates, tolerance):
    """Seed in unchanged regions, then propagate reversed face connectivity.

    Geometric matching alone must never publish a subset on a deformed surface.
    All correspondence is validated before any coordinate is written.
    `coordinates` are already expressed in a frame whose X is the mirror axis.
    """
    bm.verts.ensure_lookup_table()
    bm.verts.index_update()
    bm.faces.ensure_lookup_table()
    bm.faces.index_update()
    vertices = list(bm.verts)
    faces = list(bm.faces)
    if not vertices:
        return []
    if not all(all(math.isfinite(c) for c in p) for p in coordinates):
        raise CorrespondenceError("Invalid coordinates: nothing was changed")

    nearest, candidates, strict = _mirror_hits(coordinates, tolerance)
    state = _Correspondence(bm, vertices, faces)
    for limit in (strict, tolerance):
        _seed_pairs(state, nearest, limit)
    # Loose/wire vertices have no face seed. Keep their mapping reciprocal and verify edges below.
    for i, j, distance in candidates:
        if (state.mirror[i] < 0 and state.mirror[j] < 0 and not vertices[i].link_faces
                and not vertices[j].link_faces):
            _assign(state, i, j)
    if any(i < 0 for i in state.mirror) or any(i < 0 for i in state.face_map):
        raise CorrespondenceError("Could not reconstruct all pairs from the topology; "
                                  "nothing was changed")
    for edge in bm.edges:
        if bm.edges.get((vertices[state.mirror[edge.verts[0].index]],
                         vertices[state.mirror[edge.verts[1].index]])) is None:
            raise CorrespondenceError("Incompatible edge correspondence: nothing was changed")
    votes = [state.mirror[i] == j for i, j, distance in candidates if distance <= strict]
    if votes and sum(votes) < len(votes) / 2:
        raise CorrespondenceError(
            "The topological correspondence is not consistent with the chosen plane")
    return state.mirror


def topology_sides(bm, mirror, coordinates, axis):
    """Keep the two topological halves stable when a sculpt crosses the plane."""
    vertices = list(bm.verts)
    component = [-1] * len(vertices)
    groups = []
    for root in range(len(vertices)):
        if mirror[root] == root or component[root] >= 0:
            continue
        index = len(groups)
        group = []
        stack = [root]
        component[root] = index
        while stack:
            i = stack.pop()
            group.append(i)
            for edge in vertices[i].link_edges:
                j = edge.other_vert(vertices[i]).index
                if mirror[j] == j or j == mirror[i]:
                    continue
                if component[j] < 0:
                    component[j] = index
                    stack.append(j)
        groups.append(group)
    sides = [0] * len(vertices)
    for k, group in enumerate(groups):
        if sides[group[0]]:
            continue
        other = component[mirror[group[0]]]
        if (other < 0 or other == k
                or any(component[mirror[i]] != other for i in group)):
            raise CorrespondenceError(
                "The topology does not allow telling the two mirror halves apart")
        a = median(coordinates[i][axis] for i in group)
        b = median(coordinates[i][axis] for i in groups[other])
        if abs(a - b) < AMBIGUOUS_SIDE_EPSILON:
            raise CorrespondenceError("Ambiguous source side with respect to the chosen plane")
        sign = 1 if a > b else -1
        for i in group:
            sides[i] = sign
        for i in groups[other]:
            sides[i] = -sign
    return sides


def transfer(bm, transform, axis, source_sign, tolerance, selected_only):
    """Mirror the chosen side onto the other one on `bm`.

    `transform` maps `bm` coordinates into the mirror frame; the operator passes the identity (the
    mesh's own axes), other values are reserved for callers and tests. `selected_only` restricts
    the write to selected vertices; the operator passes False, so it is used by tests only.

    Returns the number of vertices written and the number skipped -- hidden, outside the selection
    (when `selected_only`), or fully masked on either side of their pair.
    """
    vertices = list(bm.verts)
    coordinates = [transform @ v.co for v in vertices]
    # Permute the chosen axis to X for the connectivity solver; no coordinates are modified.
    ordered = [p.copy() for p in coordinates]
    if axis != 0:
        for p in ordered:
            p[0], p[axis] = p[axis], p[0]
    mirror = topology_pairs(bm, ordered, tolerance)
    sides = topology_sides(bm, mirror, coordinates, axis)
    mask = bm.verts.layers.float.get(MASK_ATTRIBUTE)
    inverse = transform.inverted()
    changes = []
    skipped = 0
    for i in range(len(vertices)):
        j = mirror[i]
        if j != i and sides[i] * source_sign >= 0:
            # A source-side vertex keeps its own shape; only the other side is written.
            continue
        v = vertices[i]
        if v.hide or (selected_only and not v.select):
            skipped += 1
            continue
        weight = (1 - max(0, min(1, max(v[mask], vertices[j][mask]))) if mask else 1)
        if weight == 0:
            skipped += 1
            continue
        if j != i and vertices[j].hide:
            raise CorrespondenceError("Hidden source vertex: nothing was changed")
        point = coordinates[j].copy()
        point[axis] = 0 if j == i else -point[axis]
        target = inverse @ point
        value = v.co.lerp(target, weight)
        if (value - v.co).length_squared > 0:
            changes.append((v, value))
    for v, co in changes:
        v.co = co
    return len(changes), skipped


def _find_folds(working, points, face_count, old_area, old_pairs, epsilon):
    """The faces that collapsed or newly cross themselves when the mesh sits at `points`.

    `working` is a copy of the mesh, moved to `points` here: the triangle list and the normals the
    overlap test leans on are then the ones the real mesh would have, so nothing is judged on
    positions it will never be put in.
    """
    for v, p in zip(working.verts, points):
        v.co = p
    working.normal_update()
    loop_triangles = working.calc_loop_triangles()
    triangles = [[loop.vert.index for loop in tri] for tri in loop_triangles]
    face_ids = [tri[0].face.index for tri in loop_triangles]
    areas = [0.0] * face_count
    for tri, face in zip(triangles, face_ids):
        a, b, c = tri
        areas[face] += (points[b] - points[a]).cross(points[c] - points[a]).length
    bad_faces = {i for i, (old, new) in enumerate(zip(old_area, areas))
                 if old > MIN_TRIANGLE_AREA and new < old * DEGENERATE_AREA_RATIO}
    tree = BVHTree.FromPolygons(points, triangles, all_triangles=True, epsilon=epsilon)
    new_pairs = {tuple(sorted((face_ids[a], face_ids[b])))
                 for a, b in tree.overlap(tree) if a < b}
    for a, b in new_pairs - old_pairs:
        bad_faces.update((a, b))
    return bad_faces


def _collar_blend(frozen, neighbours, collar, blend):
    """Grow a geodesic collar out of the frozen vertices and write its smoothstep into `blend`.

    Vertices inside the collar keep the mirrored shape; the ones near a frozen neighbourhood are
    pulled back toward the shape they had, with the flat part of the curve at both ends so the
    band does not crease where it starts or stops.
    """
    distance = [float('inf')] * len(blend)
    queue = []
    for i in frozen:
        distance[i] = 0.0
        heapq.heappush(queue, (0.0, i))
    while queue:
        d, i = heapq.heappop(queue)
        if d != distance[i] or d >= collar:
            continue
        for j, length in neighbours[i]:
            nd = d + length
            if nd < distance[j] and nd < collar:
                distance[j] = nd
                heapq.heappush(queue, (nd, j))
    for i, d in enumerate(distance):
        t = min(1.0, d / collar)
        blend[i] = t * t * t * (t * (6 * t - 15) + 10)


def transfer_checked(bm, transform, axis, source_sign, tolerance, selected_only,
                     transition_width=DEFAULT_TRANSITION_WIDTH, on_progress=None):
    """Topology transfer validated as a whole: never publish a partial result.

    Collision neighborhoods freeze the affected vertices and the blend re-runs with a
    smoothstep collar; masked, hidden or unselected coordinates are preserved bit for bit.
    Returns `(changed, skipped, details)`.
    """
    bm.verts.ensure_lookup_table()
    bm.verts.index_update()
    bm.faces.ensure_lookup_table()
    bm.faces.index_update()
    vertices = list(bm.verts)
    before = [v.co.copy() for v in vertices]
    working = bm.copy()
    try:
        for original, copy in zip(before, working.verts):
            if original != copy.co:
                raise CorrespondenceError("Inconsistent vertex order")
        count, skipped = transfer(working, transform, axis, source_sign, tolerance,
                                  selected_only)
        candidate = [v.co.copy() for v in working.verts]
        # All masked/hidden/unselected coordinates are preserved bit for bit.
        mask = bm.verts.layers.float.get(MASK_ATTRIBUTE)
        for i, v in enumerate(vertices):
            if (v.hide or (selected_only and not v.select)
                    or (mask and v[mask] >= 1)):
                candidate[i] = before[i].copy()
        if not any(a != b for a, b in zip(before, candidate)):
            return 0, skipped, dict(method='topology', strength=1.0, adapted_vertices=0)
        bm.faces.index_update()
        # Refresh the normals before the baseline is triangulated: whoever built this bmesh may have
        # written coordinates since (a test, a future method), and tessellating a face whose normal
        # is stale is an assertion, not a result.
        bm.normal_update()
        old_triangles = [[loop.vert.index for loop in tri] for tri in bm.calc_loop_triangles()]
        old_faces = [tri[0].face.index for tri in bm.calc_loop_triangles()]
        size = max(max(p[k] for p in before) - min(p[k] for p in before) for k in range(3))
        epsilon = max(BVH_EPSILON_FLOOR, (size * BVH_EPSILON_RATIO) ** 2)
        old_tree = BVHTree.FromPolygons(before, old_triangles, all_triangles=True,
                                       epsilon=epsilon)
        old_pairs = {tuple(sorted((old_faces[a], old_faces[b])))
                     for a, b in old_tree.overlap(old_tree) if a < b}
        old_area = [0.0] * len(bm.faces)
        for tri, face in zip(old_triangles, old_faces):
            a, b, c = tri
            old_area[face] += (before[b] - before[a]).cross(before[c] - before[a]).length

        neighbours = [[] for _ in before]
        lengths = []
        for edge in bm.edges:
            a, b = (v.index for v in edge.verts)
            length = (before[a] - before[b]).length
            neighbours[a].append((b, length))
            neighbours[b].append((a, length))
            lengths.append(length)
        # The band is `transition_width` of these 40 mean edge lengths, so the default lands on the
        # six-edge collar the transfer has always grown.
        collar = max(median(lengths) * transition_width * COLLAR_BAND_EDGES_PER_UNIT,
                     size * COLLAR_MIN_EXTENT_RATIO)

        accepted = None
        frozen = set()
        blend = [1.0] * len(before)
        adapted = 0
        iteration = -1
        for iteration in range(MAX_BLEND_SOLVES):
            if on_progress is not None:
                on_progress(iteration + 1, MAX_BLEND_SOLVES)
            trial = [a.copy() if a == b or weight == 0 else a.lerp(b, weight)
                     for a, b, weight in zip(before, candidate, blend)]
            if not all(all(math.isfinite(x) for x in p) for p in trial):
                raise CorrespondenceError("Non-finite coordinates: nothing was changed")
            bad_faces = _find_folds(working, trial, len(bm.faces), old_area, old_pairs, epsilon)
            if not bad_faces:
                accepted = trial
                adapted = sum(w < 1 and a != b
                              for w, a, b in zip(blend, before, candidate))
                break
            previous = len(frozen)
            for face in bad_faces:
                frozen.update(v.index for v in bm.faces[face].verts)
            if len(frozen) == previous:
                break
            _collar_blend(frozen, neighbours, collar, blend)
        if accepted is None:
            raise CorrespondenceError(
                "The transfer would create folds or intersections: nothing was changed")
        changed = 0
        blended = []
        for i, (v, p, original) in enumerate(zip(vertices, accepted, before)):
            if p != original:
                v.co = p
                changed += 1
                blended.append(i)
        # How much of the mirrored shape actually reached the mesh: 1.0 when nothing had to be
        # softened around a collision, less when the collar took up part of the transfer.
        strength = sum(blend[i] for i in blended) / changed if changed else 1.0
        return changed, skipped, dict(
            method='topology', strength=strength, adapted_vertices=adapted,
            protected_collision_vertices=sum(before[i] != candidate[i] for i in frozen),
            validation_iterations=iteration + 1)
    finally:
        working.free()


def _diagonal_tolerance(coordinates, percent):
    """The correspondence seed distance in local units: `percent` of the bounding-box diagonal of
    `coordinates` (any sequence of 3-component points).

    An absolute distance was a different question on every mesh -- the same number that seeds a
    head-sized sculpt is hopeless on a landscape -- so the setting is a share of the size the
    sculpt actually has.
    """
    if len(coordinates) == 0:
        return MIN_MESH_DIAGONAL * percent / 100.0
    diagonal = math.sqrt(sum(
        (max(p[k] for p in coordinates) - min(p[k] for p in coordinates)) ** 2
        for k in range(3)))
    return max(diagonal, MIN_MESH_DIAGONAL) * percent / 100.0


def _seed_tolerance(mesh, percent):
    """`_diagonal_tolerance` of the coordinates stored in `mesh`, read in one bulk read. Only
    valid when `mesh` holds the live coordinates (not in Edit Mode)."""
    import numpy as np
    count = len(mesh.vertices)
    if count == 0:
        return MIN_MESH_DIAGONAL * percent / 100.0
    coordinates = np.empty(count * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", coordinates)
    coordinates = coordinates.reshape(-1, 3)
    extent = coordinates.max(axis=0) - coordinates.min(axis=0)
    return max(float(np.linalg.norm(extent)), MIN_MESH_DIAGONAL) * percent / 100.0


class MESH_OT_symmetrize_shape(bpy.types.Operator):
    """Copy the chosen side of the mesh onto the other side (shape only, topology kept);
    masked vertices keep their own shape"""
    bl_idname = "mesh.symmetrize_shape"
    bl_label = "Symmetrize Shape (Mask Protected)"
    bl_options = {'REGISTER', 'UNDO'}

    axis: EnumProperty(
        name="Axis",
        items=(('X', 'X', "Mirror across the local X axis"),
               ('Y', 'Y', "Mirror across the local Y axis"),
               ('Z', 'Z', "Mirror across the local Z axis")),
        default='X')
    source_side: EnumProperty(
        name="Source Side",
        items=(('POSITIVE', 'Positive', "The + axis side is copied to the - side"),
               ('NEGATIVE', 'Negative', "The - axis side is copied to the + side")),
        default='POSITIVE')
    distance: FloatProperty(
        name="Seed Distance",
        default=DEFAULT_SEED_DISTANCE_PERCENT, min=0.01, max=100.0, subtype='PERCENTAGE',
        description="Share of the mesh's bounding-box diagonal used to initialize the "
                    "correspondence pairs; the transfer still requires a complete topological "
                    "correspondence")
    transition_width: FloatProperty(
        name="Blend Band",
        default=DEFAULT_TRANSITION_WIDTH, min=0.01, max=1.0, subtype='FACTOR',
        description="Width of the blend near the protected area, as a share of the collar it "
                    "grows into; outside it the mirrored shape is kept exact")

    @classmethod
    def poll(cls, context):
        return (context.mode in {'EDIT_MESH', 'SCULPT'} and context.object
                and context.object.type == 'MESH')

    def invoke(self, context, event):
        return context.window_manager.invoke_props_dialog(self)

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "axis")
        layout.prop(self, "source_side")
        layout.prop(self, "distance")
        layout.prop(self, "transition_width")
        layout.label(text="Masked area preserved · geometry validated", icon='INFO')

    def execute(self, context):
        ob = context.object
        sculpt = context.mode == 'SCULPT'
        problem = single_user_problem(context, modes=('EDIT_MESH', 'SCULPT'))
        if problem is None and not sculpt and len(context.objects_in_mode) != 1:
            # In Edit Mode bmesh edits the mesh that is being edited; several of them would make
            # the transfer's target ambiguous.
            problem = "Edit one mesh at a time"
        if problem is not None:
            self.report({'ERROR'}, problem)
            return {'CANCELLED'}
        if len(ob.data.polygons) > LARGE_MESH_WARNING_FACES:
            self.report({'WARNING'},
                        "{0} faces: the validated transfer may take a while".format(
                            len(ob.data.polygons)))
        # Object Mode keeps the sculpt session's own state out of the way while the coordinates are
        # written through the mesh data (the session is rebuilt from them on re-entering Sculpt
        # Mode); in Edit Mode the edit mesh is written directly, so no mode is left.
        stay = object_mode(context) if sculpt else contextlib.nullcontext()
        window_manager = context.window_manager

        def report_progress(done, _total):
            window_manager.progress_update(done)

        bm = None
        window_manager.progress_begin(0, MAX_BLEND_SOLVES)
        try:
            with stay:
                if sculpt:
                    bm = bmesh.new()
                    bm.from_mesh(ob.data)
                else:
                    bm = bmesh.from_edit_mesh(ob.data)
                # In Edit Mode `Mesh.vertices` is the snapshot from before editing started, so the
                # seed distance is measured on the bmesh, which holds the live coordinates.
                tolerance = _diagonal_tolerance([v.co for v in bm.verts], self.distance)
                # The mirror plane is the mesh's own (local) axes: the object transform is not
                # folded in, so moving or rotating the object does not move the plane.
                transform = Matrix.Identity(4)
                axis = 'XYZ'.index(self.axis)
                source_sign = 1 if self.source_side == 'POSITIVE' else -1
                # Coordinates reach the mesh only after the validated transfer succeeded.
                count, skipped, details = transfer_checked(
                    bm, transform, axis, source_sign, tolerance,
                    selected_only=False, transition_width=self.transition_width,
                    on_progress=report_progress)
                if count:
                    if sculpt:
                        # Topology is unchanged, so write the positions only: `bm.to_mesh` would
                        # rebuild every attribute, face set and UV layer of the mesh for nothing.
                        mesh = ob.data
                        coordinates = [c for v in bm.verts for c in v.co]
                        mesh.vertices.foreach_set("co", coordinates)
                        mesh.update()
                    else:
                        bm.normal_update()
                        bmesh.update_edit_mesh(ob.data, loop_triangles=True, destructive=False)
        except (ValueError, RuntimeError) as ex:
            # `CorrespondenceError` is a `ValueError`.
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            window_manager.progress_end()
            if sculpt and bm is not None:
                bm.free()
        if not count:
            self.report({'WARNING'}, "Nothing to transfer")
            return {'CANCELLED'}
        adapted = details.get("adapted_vertices", 0)
        message = "{0} vertices · {1}".format(count, details["method"])
        if skipped:
            message += " · {0} protected".format(skipped)
        if adapted:
            message += " · local blend: {0} vertices adapted".format(adapted)
        self.report({'INFO'}, message)
        return {'FINISHED'}


classes = (
    MESH_OT_symmetrize_shape,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
