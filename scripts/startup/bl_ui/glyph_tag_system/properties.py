# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""PropertyGroups and helper classes for the Category Tabs / Glyph / Tag system.

Extracted verbatim from ``space_userpref.py`` (no behavioural change).
Contains:
  - ``CategoryTagItem``   — bpy.types.PropertyGroup for a single tag in WM collection.
  - ``CategoryTagAssignment`` — bpy.types.PropertyGroup for tag-to-category assignment.
  - ``TagModeItem``       — plain Python wrapper that proxies mode flags through cache.
  - ``get_tag_mode_item`` — factory helper returning a ``TagModeItem``.
  - ``with_context_check`` — decorator that validates context before RNA calls.
  - ``tag_enum_items_callback`` — dynamic EnumProperty callback for tag selection.

Cross-module calls to ``_auto_save_tags`` use a lazy import inside ``CategoryTagItem``
property update lambdas to avoid a circular dependency with ``space_userpref``.
"""

import bpy
from bpy.types import PropertyGroup

from bl_ui.glyph_tag_system.defaults import (
    _CATEGORY_TAG_DEFAULT_MODE_FLAGS,
)
from bl_ui.glyph_tag_system._state import (
    state,
)
from bl_ui.glyph_tag_system.conversions import (
    _hex_to_glyph,
)
from bl_ui.glyph_tag_system.tags_cache import (
    get_tag_names,
    update_tag,
)
from bl_ui.glyph_tag_system.log import (
    category_debug_print,
)


# -----------------------------------------------------------------------------
# Auto-save helper (lazy import to avoid circular dependency)
# -----------------------------------------------------------------------------


def _auto_save_tags():
    """Thin forwarding shim — avoids a circular import at module load time."""
    from bl_ui.glyph_tag_system import handlers as _handlers
    _handlers._auto_save_tags()


def _sync_wm_to_glyph_cache():
    """Thin forwarding shim — avoids a circular import at module load time."""
    from bl_ui.glyph_tag_system import wm_sync_from_wm as _wm_sync_from_wm
    _wm_sync_from_wm.sync_wm_to_glyph_cache()


# -----------------------------------------------------------------------------
# PropertyGroups
# -----------------------------------------------------------------------------


class CategoryTagItem(PropertyGroup):
    """Single tag with glyph and color."""
    name: bpy.props.StringProperty(
        name="Name",
        description="Tag name",
        maxlen=32
    )
    glyph: bpy.props.StringProperty(
        name="Glyph",
        description="Unicode glyph character",
        default="",
        update=lambda self, ctx: _auto_save_tags()  # Auto-save
    )
    color: bpy.props.FloatVectorProperty(
        name="Color",
        subtype='COLOR_GAMMA',
        size=3,
        min=0.0,
        max=1.0,
        default=(0.0, 0.0, 0.0),
        update=lambda self, ctx: _auto_save_tags()  # Auto-save
    )

    # НОВОЕ: Режимы для фильтрации
    mode_object: bpy.props.BoolProperty(
        name="Object Mode",
        default=True,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_edit: bpy.props.BoolProperty(
        name="Edit Mode",
        default=True,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_sculpt: bpy.props.BoolProperty(
        name="Sculpt Mode",
        default=True,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_vertex_paint: bpy.props.BoolProperty(
        name="Vertex Paint",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_weight_paint: bpy.props.BoolProperty(
        name="Weight Paint",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_texture_paint: bpy.props.BoolProperty(
        name="Texture Paint",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_uv_edit: bpy.props.BoolProperty(
        name="UV Edit",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_pose: bpy.props.BoolProperty(
        name="Pose Mode",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_geometry_nodes: bpy.props.BoolProperty(
        name="Geometry Nodes",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_shader_editor: bpy.props.BoolProperty(
        name="Shader Editor",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )
    mode_image_paint: bpy.props.BoolProperty(
        name="Image Paint",
        default=False,
        update=lambda self, ctx: _auto_save_tags()
    )

    def get_mode_flags(self):
        """Convert boolean mode properties to bitmask."""
        flags = 0
        if self.mode_object:
            flags |= 1 << 0  # OBJECT_MODE
        if self.mode_edit:
            flags |= 1 << 1  # EDIT_MODE
        if self.mode_sculpt:
            flags |= 1 << 2  # SCULPT_MODE
        if self.mode_vertex_paint:
            flags |= 1 << 3  # VERTEX_PAINT
        if self.mode_weight_paint:
            flags |= 1 << 4  # WEIGHT_PAINT
        if self.mode_texture_paint:
            flags |= 1 << 5  # TEXTURE_PAINT
        if self.mode_uv_edit:
            flags |= 1 << 6  # UV_EDIT
        if self.mode_pose:
            flags |= 1 << 7  # POSE_MODE
        if self.mode_geometry_nodes:
            flags |= 1 << 8  # GEOMETRY_NODES
        if self.mode_shader_editor:
            flags |= 1 << 9  # SHADER_EDITOR
        if self.mode_image_paint:
            flags |= 1 << 10  # IMAGE_PAINT
        return flags

    def set_mode_flags(self, flags):
        """Set boolean mode properties from bitmask."""
        self.mode_object = bool(flags & (1 << 0))
        self.mode_edit = bool(flags & (1 << 1))
        self.mode_sculpt = bool(flags & (1 << 2))
        self.mode_vertex_paint = bool(flags & (1 << 3))
        self.mode_weight_paint = bool(flags & (1 << 4))
        self.mode_texture_paint = bool(flags & (1 << 5))
        self.mode_uv_edit = bool(flags & (1 << 6))
        self.mode_pose = bool(flags & (1 << 7))
        self.mode_geometry_nodes = bool(flags & (1 << 8))
        self.mode_shader_editor = bool(flags & (1 << 9))
        self.mode_image_paint = bool(flags & (1 << 10))


class CategoryTagAssignment(PropertyGroup):
    """Assignment of a tag to a category."""
    tag_name: bpy.props.StringProperty(name="Tag Name")


# -----------------------------------------------------------------------------
# TagModeItem — plain Python wrapper (not a PropertyGroup)
# -----------------------------------------------------------------------------


class TagModeItem:
    """Wrapper class for tag mode editing.
    Provides boolean properties for UI that sync with state.all_tags_cache.
    """
    def __init__(self, tag_name):
        self._tag_name = tag_name
        self._load_from_cache()

    def _load_from_cache(self):
        """Load mode flags from cache."""
        tag_data = state.all_tags_cache.get(self._tag_name, {})
        mode_flags = tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS) if isinstance(tag_data, dict) else _CATEGORY_TAG_DEFAULT_MODE_FLAGS

        self._mode_object = bool(mode_flags & (1 << 0))
        self._mode_edit = bool(mode_flags & (1 << 1))
        self._mode_sculpt = bool(mode_flags & (1 << 2))
        self._mode_vertex_paint = bool(mode_flags & (1 << 3))
        self._mode_weight_paint = bool(mode_flags & (1 << 4))
        self._mode_texture_paint = bool(mode_flags & (1 << 5))
        self._mode_uv_edit = bool(mode_flags & (1 << 6))
        self._mode_pose = bool(mode_flags & (1 << 7))
        self._mode_geometry_nodes = bool(mode_flags & (1 << 8))
        self._mode_shader_editor = bool(mode_flags & (1 << 9))
        self._mode_image_paint = bool(mode_flags & (1 << 10))

    def _save_to_cache(self):
        """Save mode flags to cache."""
        if self._tag_name in state.all_tags_cache and isinstance(state.all_tags_cache[self._tag_name], dict):
            flags = 0
            if self._mode_object:
                flags |= 1 << 0
            if self._mode_edit:
                flags |= 1 << 1
            if self._mode_sculpt:
                flags |= 1 << 2
            if self._mode_vertex_paint:
                flags |= 1 << 3
            if self._mode_weight_paint:
                flags |= 1 << 4
            if self._mode_texture_paint:
                flags |= 1 << 5
            if self._mode_uv_edit:
                flags |= 1 << 6
            if self._mode_pose:
                flags |= 1 << 7
            if self._mode_geometry_nodes:
                flags |= 1 << 8
            if self._mode_shader_editor:
                flags |= 1 << 9
            if self._mode_image_paint:
                flags |= 1 << 10
            state.all_tags_cache[self._tag_name]["mode_flags"] = flags

    @property
    def mode_object(self):
        return self._mode_object

    @mode_object.setter
    def mode_object(self, value):
        self._mode_object = value
        self._save_to_cache()

    @property
    def mode_edit(self):
        return self._mode_edit

    @mode_edit.setter
    def mode_edit(self, value):
        self._mode_edit = value
        self._save_to_cache()

    @property
    def mode_sculpt(self):
        return self._mode_sculpt

    @mode_sculpt.setter
    def mode_sculpt(self, value):
        self._mode_sculpt = value
        self._save_to_cache()

    @property
    def mode_vertex_paint(self):
        return self._mode_vertex_paint

    @mode_vertex_paint.setter
    def mode_vertex_paint(self, value):
        self._mode_vertex_paint = value
        self._save_to_cache()

    @property
    def mode_weight_paint(self):
        return self._mode_weight_paint

    @mode_weight_paint.setter
    def mode_weight_paint(self, value):
        self._mode_weight_paint = value
        self._save_to_cache()

    @property
    def mode_texture_paint(self):
        return self._mode_texture_paint

    @mode_texture_paint.setter
    def mode_texture_paint(self, value):
        self._mode_texture_paint = value
        self._save_to_cache()

    @property
    def mode_uv_edit(self):
        return self._mode_uv_edit

    @mode_uv_edit.setter
    def mode_uv_edit(self, value):
        self._mode_uv_edit = value
        self._save_to_cache()

    @property
    def mode_pose(self):
        return self._mode_pose

    @mode_pose.setter
    def mode_pose(self, value):
        self._mode_pose = value
        self._save_to_cache()

    @property
    def mode_geometry_nodes(self):
        return self._mode_geometry_nodes

    @mode_geometry_nodes.setter
    def mode_geometry_nodes(self, value):
        self._mode_geometry_nodes = value
        self._save_to_cache()

    @property
    def mode_shader_editor(self):
        return self._mode_shader_editor

    @mode_shader_editor.setter
    def mode_shader_editor(self, value):
        self._mode_shader_editor = value
        self._save_to_cache()

    @property
    def mode_image_paint(self):
        return self._mode_image_paint

    @mode_image_paint.setter
    def mode_image_paint(self, value):
        self._mode_image_paint = value
        self._save_to_cache()

    def _glyph_update(self, context):
        """Callback for glyph property change via RNA (e.g. from C++ picker)."""
        if self.name:
            update_tag(self.name, glyph=_hex_to_glyph(self.glyph))
            # Sync back to cache and trigger redraw
            _sync_wm_to_glyph_cache()
            context.area.tag_redraw()

    def _color_update(self, context):
        """Callback for color property change via RNA."""
        if self.name:
            update_tag(self.name, color=list(self.color))
            # Sync back to cache and trigger redraw
            _sync_wm_to_glyph_cache()
            context.area.tag_redraw()


# -----------------------------------------------------------------------------
# Helper functions
# -----------------------------------------------------------------------------


def get_tag_mode_item(tag_name):
    """Get a TagModeItem for the given tag name."""
    return TagModeItem(tag_name)


def with_context_check(func):
    """Decorator to verify context before RNA operations."""
    def wrapper(self, context):
        if context is None:
            self.report({'ERROR'}, "No context available")
            return {'CANCELLED'}
        if context.window_manager is None:
            self.report({'ERROR'}, "Window manager not available")
            return {'CANCELLED'}
        return func(self, context)
    return wrapper


def tag_enum_items_callback(self, context):
    """Dynamic enum callback for tag selection."""
    tags = get_tag_names()
    if not tags:
        return [('__none__', "No tags available", "Create a tag first")]
    return [(tag, tag, f"Tag: {tag}") for tag in tags]


# -----------------------------------------------------------------------------
# Registration
# -----------------------------------------------------------------------------


def register():
    import bpy
    bpy.utils.register_class(CategoryTagItem)
    bpy.utils.register_class(CategoryTagAssignment)


def unregister():
    import bpy
    bpy.utils.unregister_class(CategoryTagAssignment)
    bpy.utils.unregister_class(CategoryTagItem)
