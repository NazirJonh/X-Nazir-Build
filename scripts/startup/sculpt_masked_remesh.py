# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Mask-protected voxel remesh: rebuilds the unmasked area of a sculpt mesh.

Remeshes only the unmasked region of the mesh while the masked faces and their vertices stay
bit-exact: frozen coordinates, frozen protected topology (checked after the solve, the
operation refuses to publish on any deviation). Like ZBrush's DynaMesh with a protect mask.

The free area is rebuilt from the source triangles (the unmasked region only, so volumes do
not merge through the masked patch) by iterative split/collapse toward the voxel target plus
Laplacian-style tangential smoothing projected back onto that source surface, with a
beautify pass every third iteration.

The solve is split into the steps a rebuild always has -- `_prepare` (what must stay exact),
one `_split_step` / `_collapse_step` pair per round with the optional `_relax_step`, and
`_verify` (did anything protected move) -- so a different rebuild (a tetmesh or a quad-oriented
variant) can reuse the prepare and verify halves and replace only the middle ones.

All diagnostics land in one dict (`_diagnose`): the protected and final counts, the per-round
history, the new self-intersections, the non-manifold counts and the `problems` the solve ran
into. They are reported, and never reject the user's voxel size.
"""

import math

import bmesh
import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree

from sculpt_mesh_common import (LARGE_MESH_WARNING_FACES, MASK_ATTRIBUTE, object_mode,
                                single_user_problem)

# Rounds of split/collapse. Enough for a voxel-sized patch to settle, and the loop also stops
# early once a round changes nothing.
REMESH_ITERATIONS = 24
# A voxel of `voxel` leaves an edge of voxel * sqrt(2) between two voxel corners, so that is the
# edge length the rebuild aims for, not `voxel` itself.
VOXEL_EDGE_FACTOR = math.sqrt(2)
# Split above this multiple of the target: cutting an edge only a third too long would make edges
# the next collapse round removes again.
SPLIT_EDGE_FACTOR = 4 / 3
# Collapse below this multiple of the target, so an edge stops near the target instead of
# oscillating around it.
COLLAPSE_EDGE_FACTOR = 0.75
# An edge inside a triangle pair has exactly two shared neighbours; any other count means a
# boundary or a fan, where a collapse would move the protected shape.
COLLAPSE_SHARED_NEIGHBOURS = 2
# Relax on every third round: beautify and smoothing every round would fight the split and
# collapse passes for the same edges.
RELAX_EVERY_ITERATION = 3
# Cap the relaxation step at this multiple of the target, so one vertex cannot cross the surface
# it is projected back onto.
RELAX_MAX_STEP_FACTOR = 0.25
# Project from a point this far ahead of the step, so the surface found is the one the vertex is
# moving toward rather than the one it is already resting on.
RELAX_PROBE_FACTOR = 0.35
# A mask this thin still protects: the paint mask is stored as a float and a barely-touched
# vertex must not be rebuilt.
MASK_PROTECT_EPSILON = 1e-6

# The bmesh layers the solve writes. They are temporary and are removed from the bmesh before
# it is published.
PROTECT_LAYER = ".local_protected"
FACE_ID_LAYER = ".local_face_id"
VERTEX_ID_LAYER = ".local_vertex_id"
_TEMP_NAMES = (PROTECT_LAYER, FACE_ID_LAYER, VERTEX_ID_LAYER)


def _free_faces(bm, protect):
    """The faces the rebuild may change."""
    return [f for f in bm.faces if not f[protect]]


def _free_edges(bm, protect):
    """The edges of the free faces (some of them may also border a protected face)."""
    return {e for f in _free_faces(bm, protect) for e in f.edges}


def _prepare(bm, voxel):
    """Index the mesh, snapshot what must stay exact, build the surface the rebuild aims at.

    Returns the state the per-round steps share: the target edge length, the source surface, the
    frozen coordinates and topology, and the self-intersections the mesh already had (they are not
    this operation's fault, so the final count is a difference against that baseline).
    """
    bm.verts.index_update()
    bm.faces.index_update()
    bm.normal_update()

    mask = bm.verts.layers.float.get(MASK_ATTRIBUTE)
    if not mask:
        raise ValueError("No mask to protect")

    old_tree = BVHTree.FromBMesh(bm)
    old_pairs = {tuple(sorted((a + 1, b + 1))) for a, b in old_tree.overlap(old_tree) if a < b}

    protected = [any(v[mask] > MASK_PROTECT_EPSILON for v in f.verts) for f in bm.faces]
    protect = bm.faces.layers.int.new(PROTECT_LAYER)
    face_id = bm.faces.layers.int.new(FACE_ID_LAYER)
    vertex_id = bm.verts.layers.int.new(VERTEX_ID_LAYER)
    for i, f in enumerate(bm.faces):
        f[protect] = protected[i]
        f[face_id] = i + 1

    # Frozen: every vertex of a protected face, plus the mesh's own boundary.
    frozen = ({v for f in bm.faces if f[protect] for v in f.verts}
              | {v for v in bm.verts if v.is_boundary})
    for i, v in enumerate(bm.verts):
        v[vertex_id] = i + 1 if v in frozen else 0
    saved = {v[vertex_id]: tuple(v.co) for v in bm.verts if v[vertex_id]}
    saved_faces = {f[face_id]: tuple(v[vertex_id] for v in f.verts)
                   for f in bm.faces if f[protect]}

    # The rebuild source is the unmasked triangles only: no volume merges through the patch.
    source_triangles = [tri for tri in bm.calc_loop_triangles() if not tri[0].face[protect]]
    positions = [v.co.copy() for v in bm.verts]
    surface = BVHTree.FromPolygons(
        positions, [[loop.vert.index for loop in tri] for tri in source_triangles],
        all_triangles=True)

    bmesh.ops.triangulate(bm, faces=_free_faces(bm, protect), quad_method='BEAUTY',
                          ngon_method='BEAUTY')

    return dict(target=voxel * VOXEL_EDGE_FACTOR, surface=surface, saved=saved,
                saved_faces=saved_faces, old_pairs=old_pairs)


def _split_step(bm, state):
    """Split the long manifold edges of the free area. Returns how many were split."""
    protect = bm.faces.layers.int[PROTECT_LAYER]
    limit = state["target"] * SPLIT_EDGE_FACTOR
    long = [e for e in _free_edges(bm, protect)
            if e.is_manifold and e.calc_length() > limit
            and all(not f[protect] for f in e.link_faces)]
    if not long:
        return 0
    split = bmesh.ops.subdivide_edges(bm, edges=long, cuts=1, use_grid_fill=True)
    # New vertices are free (they came out of a free face), but tag them explicitly: `vertex_id` is
    # what the collapse and the relax rounds trust to know a vertex may move.
    vertex_id = bm.verts.layers.int[VERTEX_ID_LAYER]
    for key in ('geom_inner', 'geom_split'):
        for v in split.get(key, []):
            if isinstance(v, bmesh.types.BMVert):
                v[vertex_id] = 0
    bmesh.ops.triangulate(bm, faces=_free_faces(bm, bm.faces.layers.int[PROTECT_LAYER]),
                          quad_method='BEAUTY', ngon_method='BEAUTY')
    return len(long)


def _collapse_step(bm, state):
    """Collapse the short manifold edges between free triangles. Returns how many were collapsed."""
    protect = bm.faces.layers.int[PROTECT_LAYER]
    vertex_id = bm.verts.layers.int[VERTEX_ID_LAYER]
    limit = state["target"] * COLLAPSE_EDGE_FACTOR
    candidates = sorted(
        (e for e in _free_edges(bm, protect)
         if e.is_manifold and e.calc_length() < limit
         and all(not v[vertex_id] for v in e.verts)
         and all(len(f.verts) == 3 for f in e.link_faces)),
        key=lambda e: e.calc_length())
    selected = []
    avoid = set()
    for e in candidates:
        a, b = e.verts
        if a in avoid or b in avoid:
            # A vertex touched by an edge already chosen cannot be collapsed again in this round.
            continue
        na = {edge.other_vert(a) for edge in a.link_edges}
        nb = {edge.other_vert(b) for edge in b.link_edges}
        if len(na & nb) != COLLAPSE_SHARED_NEIGHBOURS:
            continue
        selected.append(e)
        avoid.update(na | nb | {a, b})
    if selected:
        bmesh.ops.collapse(bm, edges=selected, uvs=False)
    return len(selected)


def _relax_step(bm, state):
    """Beautify the free area, relax it tangentially and pull the points onto the source surface."""
    target = state["target"]
    surface = state["surface"]
    protect = bm.faces.layers.int[PROTECT_LAYER]
    vertex_id = bm.verts.layers.int[VERTEX_ID_LAYER]
    free = _free_faces(bm, protect)
    edges = {e for e in _free_edges(bm, protect)
             if e.is_manifold and all(not neighbour[protect] for neighbour in e.link_faces)}
    if edges:
        bmesh.ops.beautify_fill(bm, faces=free, edges=list(edges),
                                use_restrict_tag=False, method='AREA')
    bm.normal_update()
    updates = []
    for v in bm.verts:
        if v[vertex_id]:
            continue
        neighbours = [edge.other_vert(v) for edge in v.link_edges]
        if not neighbours:
            continue
        delta = sum((n.co for n in neighbours), Vector()) / len(neighbours) - v.co
        # Tangential only: the part along the normal belongs to the projection below.
        delta -= v.normal * delta.dot(v.normal)
        if delta.length > target * RELAX_MAX_STEP_FACTOR:
            delta *= target * RELAX_MAX_STEP_FACTOR / delta.length
        point, _normal, _face, _distance = surface.find_nearest(
            v.co + delta * RELAX_PROBE_FACTOR)
        if point is not None:
            updates.append((v, point))
    # Positions are written after the pass, so no vertex relaxes toward a neighbour that has
    # already moved this round.
    for v, point in updates:
        v.co = point


def _verify(bm, state):
    """Every way the protected area could have drifted, as a list (empty when it held)."""
    bm.normal_update()
    bm.verts.index_update()
    bm.faces.index_update()
    vertex_id = bm.verts.layers.int[VERTEX_ID_LAYER]
    face_id = bm.faces.layers.int[FACE_ID_LAYER]
    protect = bm.faces.layers.int[PROTECT_LAYER]
    problems = []
    if {v[vertex_id]: tuple(v.co) for v in bm.verts if v[vertex_id]} != state["saved"]:
        problems.append("Frozen vertices changed")
    if ({f[face_id]: tuple(v[vertex_id] for v in f.verts)
         for f in bm.faces if f[protect]} != state["saved_faces"]):
        problems.append("Protected faces changed")
    return problems


def _diagnose(bm, state):
    """The one diagnostics structure: counts, history and quality measures of the result."""
    face_id = bm.faces.layers.int[FACE_ID_LAYER]
    final_tree = BVHTree.FromBMesh(bm)
    labels = [f[face_id] if f[face_id] else -f.index - 1 for f in bm.faces]
    pairs = {tuple(sorted((labels[a], labels[b])))
             for a, b in final_tree.overlap(final_tree) if a < b}
    return dict(protected_vertices=len(state["saved"]), protected_faces=len(state["saved_faces"]),
                final_vertices=len(bm.verts), final_faces=len(bm.faces), iterations=[],
                new_intersections=len(pairs - state["old_pairs"]),
                nonmanifold_edges=sum(not e.is_manifold for e in bm.edges),
                inconsistent_edges=sum(len(e.link_loops) == 2 and e.link_loops[0].vert ==
                                       e.link_loops[1].vert for e in bm.edges))


def _core(bm, voxel, iterations=REMESH_ITERATIONS, on_progress=None):
    """Rebuild the unmasked area of `bm` toward the voxel size. Returns the bmesh and the
    diagnostics, and raises when the protected area moved at all."""
    state = _prepare(bm, voxel)
    rows = []
    for iteration in range(iterations):
        split = _split_step(bm, state)
        collapsed = _collapse_step(bm, state)
        if iteration % RELAX_EVERY_ITERATION == RELAX_EVERY_ITERATION - 1:
            _relax_step(bm, state)
        rows.append(dict(iteration=iteration, split=split, collapsed=collapsed,
                         verts=len(bm.verts)))
        if on_progress is not None:
            on_progress(iteration + 1, iterations)
        if not collapsed and not split:
            break

    # `_verify` re-indexes the mesh, so the metrics (which read face indices) come after it.
    problems = _verify(bm, state)
    info = _diagnose(bm, state)
    info["iterations"] = rows
    info["problems"] = problems
    info["protected_coordinates_exact"] = "Frozen vertices changed" not in problems
    info["protected_topology_exact"] = "Protected faces changed" not in problems
    if problems:
        raise ValueError("; ".join(problems))
    return bm, info


def local_remesh(mesh, voxel, on_progress=None):
    """Remesh `mesh` in place on the returned bmesh; raises instead of publishing when the
    protected area deviated. The caller's mesh is only touched through the returned bm."""
    if any(attribute.name in _TEMP_NAMES for attribute in mesh.attributes):
        raise ValueError("Temporary attributes already present; nothing changed")
    bm = bmesh.new()
    try:
        bm.from_mesh(mesh)
        return _core(bm, voxel, on_progress=on_progress)
    except Exception:
        bm.free()
        raise


def _guard(context):
    """Why this mesh cannot be remeshed, or None. The mesh-wide part comes from
    `sculpt_mesh_common`; only the voxel size and the mask are this tool's own."""
    problem = single_user_problem(context, modes=('OBJECT', 'SCULPT'))
    if problem is not None:
        return problem
    source = context.object.data
    if source.remesh_voxel_size <= 0:
        return "The Voxel Size is not valid"
    mask = source.attributes.get(MASK_ATTRIBUTE)
    if mask is None:
        return "Paint a mask first: it marks the area to protect"
    if all(any(mask.data[i].value > MASK_PROTECT_EPSILON for i in face.vertices)
           for face in source.polygons):
        return "The mask covers the whole surface: nothing would change"
    return None


def _remove_temporary_layers(bm):
    """Drop the solve's own layers from `bm`, so they never reach the published mesh."""
    bm.faces.layers.int.remove(bm.faces.layers.int[PROTECT_LAYER])
    bm.faces.layers.int.remove(bm.faces.layers.int[FACE_ID_LAYER])
    bm.verts.layers.int.remove(bm.verts.layers.int[VERTEX_ID_LAYER])


def _drop_unpreserved_attributes(mesh):
    """Drop the attributes the native remesh checkbox does not preserve. Mask protection retains
    its own mask."""
    for attribute in list(mesh.attributes):
        if not attribute.is_required and attribute.name != MASK_ATTRIBUTE:
            mesh.attributes.remove(attribute)


class OBJECT_OT_voxel_remesh_protected(bpy.types.Operator):
    """Voxel-size the unmasked area to the remesh voxel size; the masked area keeps its exact
    coordinates and topology (a protect-mask DynaMesh, like ZBrush). Without Preserve Attributes
    every attribute except the mask is lost: face sets, hide and selection flags, UVs and colors"""
    bl_idname = "object.voxel_remesh_protected"
    bl_label = "Remesh Unmasked (Mask Protected)"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        # Only the cheap part of the guard. The mask checks below walk every face, which a panel
        # redraw cannot afford; `execute` runs the full guard and reports what it finds.
        return single_user_problem(context, modes=('OBJECT', 'SCULPT')) is None

    def execute(self, context):
        problem = _guard(context)
        if problem is not None:
            self.report({'ERROR'}, problem)
            return {'CANCELLED'}
        ob = context.object
        source = ob.data
        if len(source.polygons) > LARGE_MESH_WARNING_FACES:
            self.report({'WARNING'},
                        "{0} faces: the protected remesh may take a while".format(
                            len(source.polygons)))
        window_manager = context.window_manager

        def report_progress(done, _total):
            window_manager.progress_update(done)

        window_manager.progress_begin(0, REMESH_ITERATIONS)
        bm = None
        try:
            with object_mode(context):
                # The solve and `_verify` finish before anything is written, so a refused result
                # leaves the source untouched; the single write below is the only mutation.
                bm, info = local_remesh(source, source.remesh_voxel_size,
                                        on_progress=report_progress)
                _remove_temporary_layers(bm)
                bm.to_mesh(source)
                bm.free()
                bm = None
                if not source.use_remesh_preserve_attributes:
                    _drop_unpreserved_attributes(source)
                source.update()
                ob.update_tag(refresh={'DATA'})
            self.report({'INFO'},
                        "Mask-protected remesh: {0} vertices protected, {1} faces after {2}"
                        " rounds".format(info["protected_vertices"], info["final_faces"],
                                         len(info["iterations"])))
            if info["new_intersections"]:
                self.report({'WARNING'},
                            "{0} new self-intersections".format(info["new_intersections"]))
            return {'FINISHED'}
        except (ValueError, RuntimeError, KeyError) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            window_manager.progress_end()
            if bm:
                bm.free()


classes = (
    OBJECT_OT_voxel_remesh_protected,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
