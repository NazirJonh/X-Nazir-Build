# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Mode-flag resolution for the Category Tabs / Glyph / Tag system.

Single source for the Python equivalents of the C++ ``get_current_tag_mode_flag()``
and the WindowManager ``category_tag_filter_mode`` enum-to-flag conversion, plus the
space/mode context predicate every Python caller shares
(:func:`category_matches_context`).

These helpers depend only on the mode tables in :mod:`.defaults` and on the
``bpy`` context / ``WindowManager``, so they can be imported standalone.
"""

from .conversions import (
    _space_type_id_to_str,
    _space_type_str_to_id,
)
from .defaults import (
    SPACE_TO_FLAG,
    _CATEGORY_TAG_FILTER_ENUM_TO_FLAG,
    _CATEGORY_TAG_MODE_NAME_TO_FLAG,
)

# Node Editor modes (Geometry Nodes, Shader Editor) are unlike 3D View modes, so mode filtering
# is skipped there; see :func:`category_matches_context`.
_SPACE_TYPE_NODE = _space_type_str_to_id('SPACE_NODE')


def category_matches_context(discovered_spaces, discovered_modes, install_mode_flag,
                             space_type, current_mode_flag):
    """Return whether a category belongs in the given space/mode context.

    Zero means "any" throughout: a category discovered in no particular space or mode matches
    every context, ``space_type == -1`` means the caller is not filtering by space, and
    ``current_mode_flag == 0`` means it is not filtering by mode. Filtering only ever rejects
    when both sides are known and their bits do not intersect.

    ``install_mode_flag`` is the fallback for extensions whose panels declare no ``bl_context``,
    so nothing was ever discovered for them: the mode the extension was installed in is the only
    signal available.

    This rule is evaluated independently by C++ in ``category_is_unassigned_for_context``
    (#interface_panel.cc), which cannot call into Python during panel drawing. The two must agree
    or the tag bar's "New Add-ons!" count will not match the categories the panel filter shows;
    ``tests/python/bl_glyph_tag_schema_roundtrip.py`` pins this implementation against the C++
    source so the pair cannot drift silently.

    Args:
        discovered_spaces: Bitmask of spaces the category's panels were discovered in.
        discovered_modes: Bitmask of modes the category's panels were discovered in.
        install_mode_flag: Mode captured when the extension was installed (fallback).
        space_type: Space type ID to test against, or -1 for "any space".
        current_mode_flag: Mode bitmask to test against, or 0 for "not filtering".
    """
    if space_type != -1 and discovered_spaces != 0:
        space_flag = SPACE_TO_FLAG.get(_space_type_id_to_str(space_type), 0)
        if not (discovered_spaces & space_flag):
            return False

    if space_type == _SPACE_TYPE_NODE or current_mode_flag == 0:
        return True

    effective_mode_flags = discovered_modes if discovered_modes != 0 else install_mode_flag
    if effective_mode_flags != 0 and not (effective_mode_flags & current_mode_flag):
        return False
    return True


def _get_tag_filter_mode_flag_from_wm(wm):
    """Convert WindowManager.category_tag_filter_mode enum index to CategoryTagMode bit flag."""
    enum_value = getattr(wm, "category_tag_filter_mode", 0)
    if isinstance(enum_value, str):
        return _CATEGORY_TAG_FILTER_ENUM_TO_FLAG.get(enum_value, 0)
    try:
        return _CATEGORY_TAG_FILTER_ENUM_TO_FLAG.get(int(enum_value), 0)
    except (TypeError, ValueError):
        return 0


def get_current_tag_mode_flag(context):
    """Get the current object mode as a CategoryTagMode bitmask.

    This is the Python equivalent of the C++ get_current_tag_mode_flag() function.
    Returns a bit flag corresponding to the current object mode.
    For edit mode, returns a detailed flag based on object type (mesh_edit, curve_edit, etc.)
    """
    area = getattr(context, "area", None)
    if area is not None:
        if area.type == 'NODE_EDITOR':
            snode = context.space_data
            if snode is not None:
                if snode.tree_type == 'GeometryNodeTree':
                    return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("GEOMETRY_NODES", 0)
                if snode.tree_type == 'ShaderNodeTree':
                    return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("SHADER_EDITOR", 0)
        elif area.type == 'IMAGE_EDITOR':
            sima = context.space_data
            if sima is not None:
                if sima.mode == 'PAINT':
                    return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("IMAGE_PAINT", 0)
                if sima.mode == 'UV':
                    return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("UV_EDIT", 0)

    ob = context.active_object
    if not ob:
        return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("OBJECT_MODE", 0)

    mode = ob.mode
    if mode == 'EDIT':
        # Return detailed edit mode flag based on object type
        # This matches bl_context values like "mesh_edit", "curve_edit", etc.
        ob_type_to_mode = {
            'MESH': "MESH_EDIT",
            'CURVE': "CURVE_EDIT",
            'CURVES': "CURVE_EDIT",
            'SURFACE': "SURFACE_EDIT",
            'ARMATURE': "ARMATURE_EDIT",
            'LATTICE': "LATTICE_EDIT",
            'META': "META_EDIT",
            'FONT': "FONT_EDIT",
            'GREASEPENCIL': "GREASE_PENCIL_EDIT",
            'POINTCLOUD': "POINTCLOUD_EDIT",
            'VOLUME': "VOLUME_EDIT",
        }
        mode_name = ob_type_to_mode.get(ob.type, "EDIT_MODE")
        return _CATEGORY_TAG_MODE_NAME_TO_FLAG.get(mode_name, _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("EDIT_MODE", 0))

    mode_map = {
        'OBJECT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("OBJECT_MODE", 0),
        'SCULPT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("SCULPT_MODE", 0),
        'VERTEX_PAINT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("VERTEX_PAINT", 0),
        'WEIGHT_PAINT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("WEIGHT_PAINT", 0),
        'TEXTURE_PAINT': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("TEXTURE_PAINT", 0),
        'PAINT_UV': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("UV_EDIT", 0),
        'POSE': _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("POSE_MODE", 0),
    }
    return mode_map.get(mode, _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("OBJECT_MODE", 0))
