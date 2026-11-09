# SPDX-FileCopyrightText: 2026 Nazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Shared plumbing for the mask-aware sculpt mesh tools.

``sculpt_masked_remesh`` and ``sculpt_symmetry_shape`` both start from the same three questions:
which object may be rewritten, where the sculpt mask lives, and how to leave Sculpt Mode while
plain mesh data is edited. Those answers used to be copied into each module, so a new tool (a
SURFACE symmetry method, another protected rebuild) would have copied them a third time. They are
kept here instead.

Nothing about the algorithms lives here: no geometry, no bmesh work, only the guard, the attribute
name and the mode switch. A future module that needs a different mesh query should add its own
check rather than grow flags in ``single_user_problem``.
"""

import contextlib

import bpy

# The sculpt mask is stored as this point attribute (see `paint_mask.cc`); every tool that protects
# the masked area reads it by this exact name.
MASK_ATTRIBUTE = ".sculpt_mask"
# The sculpt face sets (polygroups) live in this face attribute.
FACE_SET_ATTRIBUTE = ".sculpt_face_set"

# Both tools walk the whole mesh per solve step in pure Python, and run synchronously without a
# way to cancel, so past this face count the user is warned that a moment is coming. The number is
# a conservative estimate (no measurements were available), not a limit: the tools still run, and
# the solve is refused only by its own correctness checks.
LARGE_MESH_WARNING_FACES = 50_000

# Mode identifiers as the message spells them; the guard lists whichever modes it accepts.
_MODE_NAMES = {
    'OBJECT': "Object Mode",
    'SCULPT': "Sculpt Mode",
    'EDIT_MESH': "Edit Mode",
}


# `context.mode` values whose `bpy.ops.object.mode_set` identifier differs.
_MODE_SET_NAMES = {
    'EDIT_MESH': 'EDIT',
    'PAINT_VERTEX': 'VERTEX_PAINT',
    'PAINT_WEIGHT': 'WEIGHT_PAINT',
    'PAINT_TEXTURE': 'TEXTURE_PAINT',
}


def _mode_list(modes):
    """"Object or Sculpt Mode" from the accepted mode identifiers."""
    names = [_MODE_NAMES.get(mode, mode) for mode in modes]
    return " or ".join(names)


def single_user_problem(context, modes=('OBJECT', 'SCULPT')):
    """Why this context's mesh cannot be rewritten in place, or None when it can.

    Both tools rebuild or overwrite the mesh data block, so they need the same mesh: one that no
    other object shares (a shared mesh would silently edit those too), is not linked from a library
    (read-only), has no shape keys (they would be left behind the new coordinates), is not under a
    Multires modifier (its own grids are the real surface) and, while sculpting, is not a Dyntopo
    one (its topology changes under the operation).

    `modes` is the set the tool can run in: the remesh rewrites the mesh from Object or Sculpt
    Mode, the symmetry transfer also accepts Edit Mode, where the mesh is edited through the
    mesh's own data.
    """
    if context.mode not in modes:
        return "Works from {0}".format(_mode_list(modes))
    ob = context.object
    if ob is None or ob.type != 'MESH':
        return "A mesh object is required"
    source = ob.data
    if (source.users != 1 or source.library or source.shape_keys
            or any(modifier.type == 'MULTIRES' for modifier in ob.modifiers)
            or (context.mode == 'SCULPT' and ob.use_dynamic_topology_sculpting)):
        return "Use a single-user mesh without shape keys, Multires or Dyntopo"
    return None


@contextlib.contextmanager
def object_mode(context):
    """Switch to Object Mode for the duration of the block, then back to the mode it was in.

    Plain mesh data (bmesh, attribute pruning, a whole-mesh write) can only be edited from Object
    Mode; the tools that do it all leave Sculpt Mode and come back the same way, so the mode
    handling belongs here instead of in each `execute`. The restore runs in `finally`, because a
    raise in the middle of publishing must not strand the user in Object Mode with half-written
    mesh data.
    """
    previous = context.mode
    if previous == 'OBJECT':
        yield
        return
    # `context.mode` and `mode_set` spell some modes differently (`EDIT_MESH` against `EDIT`).
    restore = _MODE_SET_NAMES.get(previous, previous)
    bpy.ops.object.mode_set(mode='OBJECT')
    try:
        yield
    finally:
        try:
            bpy.ops.object.mode_set(mode=restore)
        except (RuntimeError, TypeError):
            # The object or its mode is gone (deleted, another file loaded): the caller's own error
            # is the one worth reporting, and Blender re-enters a valid mode on its own.
            pass
