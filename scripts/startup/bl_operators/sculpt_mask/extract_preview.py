# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Viewport drawing of the live Extract (POST_VIEW): the Thickness Loops preview.

The panel itself (POST_PIXEL) is drawn by :mod:`extract_panel` through the shared ``panel_ui``
toolkit; this module only draws into the 3D scene, on top of the draft objects.
"""

import bpy
import gpu
from gpu_extras.batch import batch_for_shader

from .theme import get_theme


def draw_preview(op):
    """POST_VIEW: show the Thickness Loops on the extracts (see :func:`_draw_loops`) while the
    Preview toggle is on."""
    if not op.preview or op.cleaned or bpy.context.area != op.area:
        return
    for part in op.parts:
        _draw_loops(op, part)


def _draw_loops(op, part):
    """POST_VIEW: draw the extra loops Accept will cut across the thickness, as lines along the
    extract's boundary at equal steps between its two surfaces. Taken from the evaluated draft, so
    Smoothness, Inflate and even thickness are in the shape. Drawn twice -- faintly through the
    shape and solid where visible -- so they read even where they lie in the rim's own surface."""
    import numpy as np
    ob = part.object()
    loops = int(op.values["thickness_loops"])
    if ob is None or loops < 1 or op.values["thickness"] == 0.0 or part.large:
        return
    if part.loops_dirty:
        mesh = ob.data
        part.loops_data = None
        if len(mesh.vertices) and len(mesh.polygons):
            edge_verts = np.empty(len(mesh.edges) * 2, dtype=np.int32)
            mesh.edges.foreach_get("vertices", edge_verts)
            loop_edges = np.empty(len(mesh.loops), dtype=np.int32)
            mesh.loops.foreach_get("edge_index", loop_edges)
            # The outline of the extract: edges used by a single face.
            boundary = np.bincount(loop_edges, minlength=len(mesh.edges)) == 1
            part.loops_data = (len(mesh.vertices), edge_verts.reshape(-1, 2)[boundary])
        part.loops_dirty = False
    data = part.loops_data
    if data is None or len(data[1]) == 0:
        return
    count, edges = data
    evaluated = ob.evaluated_get(bpy.context.evaluated_depsgraph_get()).data
    # Solidify (simple mode) lists the original vertices first and their offset copies after them,
    # so vertex i and i + count are the two surfaces at the same spot.
    if len(evaluated.vertices) != 2 * count:
        return
    co = np.empty(len(evaluated.vertices) * 3, dtype=np.float32)
    evaluated.vertices.foreach_get("co", co)
    co = co.reshape(-1, 3)
    first, second = co[:count], co[count:]
    mw = np.array(ob.matrix_world, dtype=np.float32)
    points = []
    for k in range(1, loops + 1):
        local = first + (second - first) * (k / (loops + 1))
        world = local @ mw[:3, :3].T + mw[:3, 3]
        points.append(world[edges].reshape(-1, 3))
    positions = np.concatenate(points).tolist()
    shader = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
    batch = batch_for_shader(shader, 'LINES', {"pos": positions})
    rgb = get_theme()["HINT_ADD"][:3]
    shader.bind()
    shader.uniform_float("viewportSize", gpu.state.viewport_get()[2:])
    shader.uniform_float("lineWidth", 2.0)
    gpu.state.blend_set('ALPHA')
    for depth, alpha in (('ALWAYS', 0.35), ('LESS_EQUAL', 1.0)):
        gpu.state.depth_test_set(depth)
        shader.uniform_float("color", (*rgb, alpha))
        batch.draw(shader)
    gpu.state.blend_set('NONE')
    gpu.state.depth_test_set('NONE')
