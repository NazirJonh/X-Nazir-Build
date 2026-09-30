# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Tag pie menu and Category Tabs hotkey preferences.

Contains:
  - ``USERPREF_OT_category_tag_activate`` — switch the current editor's tag filter
    to a single tag (the action behind the tag pie menu entries).
  - ``SCREEN_MT_category_tag_pie`` — pie menu listing the current tags with their
    icons and names. Invoked through the ``Category Tabs`` keymap (unassigned by default, set in Preferences)
    or via Quick Favorites (``wm.call_menu_pie`` / ``wm.call_menu``).
  - ``USERPREF_PT_category_tabs_keymap`` — Preferences panel (Category Tags section,
    after Tag Management) listing the remappable hotkeys of the ``Category Tabs``
    keymap: category search and tag pie menu (both unassigned by default).
"""

import bpy
from bpy.types import Menu, Operator, Panel

from bl_ui.glyph_tag_system.log import category_debug_print
from bl_ui.glyph_tag_system.defaults import _CATEGORY_TAG_EDIT_MODE_MASK, _CATEGORY_TAG_MODE_NAME_TO_FLAG
from bl_ui.glyph_tag_system.modes import get_current_tag_mode_flag
from bl_ui.glyph_tag_system.tag_ui import TagsPanel

# Pie menus have 8 slots (#PIE_MAX_ITEMS in interface_intern.hh); extra items wrap onto used slots.
_PIE_MAX_ITEMS = 8


def _tag_display_icon(layout, tag):
    """Return (icon, icon_value) suitable for ``layout.operator`` for a tag.

    Blender built-in icon identifier goes into ``icon``, custom file icons are
    resolved into a preview id for ``icon_value``. Glyph-only tags get no icon.
    """
    icon_source = getattr(tag, "icon_source", 0)
    icon_key = getattr(tag, "icon_key", "")
    icon_path = getattr(tag, "icon_path", "")
    if icon_source == 2 and icon_path:
        try:
            return None, layout.icon_from_file(icon_path)
        except Exception:
            return None, 0
    if icon_source == 1 and icon_key:
        if icon_key in _valid_icon_ids():
            return icon_key, 0
    return None, 0


_VALID_ICONS_CACHE = None


def _valid_icon_ids():
    """Set of valid built-in icon identifiers (cached)."""
    global _VALID_ICONS_CACHE
    if _VALID_ICONS_CACHE is None:
        # `UILayout` has no `icon` property, the enum lives on the `icon` parameter of its functions.
        icon_prop = bpy.types.UILayout.bl_rna.functions["label"].parameters["icon"]
        _VALID_ICONS_CACHE = {item.identifier for item in icon_prop.enum_items_static}
    return _VALID_ICONS_CACHE


def _tag_valid_for_mode(tag, mode_flag):
    """Mirror of the C++ ``is_tag_valid_for_mode`` (interface_tag_bar.cc), as the tag bar shows it."""
    tag_flags = tag.mode_flags
    if tag_flags == 0 or mode_flag == 0:
        return True
    if tag_flags & mode_flag:
        return True
    # A tag restricted to plain Edit Mode stays visible in every detailed edit mode.
    return bool(mode_flag & _CATEGORY_TAG_EDIT_MODE_MASK) and bool(
        tag_flags & _CATEGORY_TAG_MODE_NAME_TO_FLAG["EDIT_MODE"])


def _space_tag_filter_state(context):
    """Return (space, active_tags_string) for tag-filter capable editors, else (None, "")."""
    space = context.space_data
    if space is None or not hasattr(space, "active_tag_filter_tags"):
        return None, ""
    return space, space.active_tag_filter_tags or ""


# -----------------------------------------------------------------------------
# Operator: activate a single tag for the current editor
# -----------------------------------------------------------------------------


class USERPREF_OT_category_tag_activate(Operator):
    """Switch the current editor's category tab bar to this tag

    Activating a tag shows only the categories assigned to it. Clicking the
    already-active tag clears the filter so all categories are shown again.
    """
    bl_idname = "wm.category_tag_activate"
    bl_label = "Switch to Tag"
    bl_options = {'REGISTER', 'UNDO'}

    tag_name: bpy.props.StringProperty(
        name="Tag",
        description="Tag to activate in the current editor",
        maxlen=64,
    )

    @classmethod
    def poll(cls, context):
        return _space_tag_filter_state(context)[0] is not None

    def execute(self, context):
        if not self.tag_name:
            self.report({'WARNING'}, "No tag specified")
            return {'CANCELLED'}

        space, active_tags = _space_tag_filter_state(context)
        if space is None:
            self.report({'WARNING'}, "This editor does not support tag filtering")
            return {'CANCELLED'}

        active_list = [t.strip() for t in active_tags.split(",") if t.strip()]

        if active_list == [self.tag_name]:
            # Toggle off: the tag was already the only active one.
            space.active_tag_filter_tags = ""
            space.tag_filter_enabled = False
            category_debug_print(f"[TAG PIE] deactivated filter for tag='{self.tag_name}'")
        else:
            space.active_tag_filter_tags = self.tag_name
            space.tag_filter_enabled = True
            if hasattr(space, "new_addon_filter_active"):
                space.new_addon_filter_active = False
            category_debug_print(f"[TAG PIE] activated tag='{self.tag_name}'")

        # A tag switch dismisses a category temporarily promoted by quick-focus search.
        if hasattr(space, "quick_focus_temp_category"):
            space.quick_focus_temp_category = ""

        if context.area:
            context.area.tag_redraw()
        return {'FINISHED'}


# -----------------------------------------------------------------------------
# Pie menu: switch between tags
# -----------------------------------------------------------------------------


class SCREEN_MT_category_tag_pie(Menu):
    bl_label = "Category Tags"
    bl_idname = "SCREEN_MT_category_tag_pie"

    @classmethod
    def poll(cls, context):
        # Only editors with a category tab bar (and thus a tag filter) offer the pie.
        return _space_tag_filter_state(context)[0] is not None

    def draw(self, context):
        wm = context.window_manager
        layout = self.layout

        if not wm or not hasattr(wm, "category_tags") or len(wm.category_tags) == 0:
            layout.label(text="No tags defined", icon='INFO')
            return

        space, active_tags = _space_tag_filter_state(context)
        active_list = [t.strip() for t in active_tags.split(",") if t.strip()]

        # Only tags valid for the current mode, exactly like the tag bar.
        mode_flag = get_current_tag_mode_flag(context)
        tags = [tag for tag in wm.category_tags if _tag_valid_for_mode(tag, mode_flag)]
        if not tags:
            layout.label(text="No tags for the current mode", icon='INFO')
            return

        pie = layout.menu_pie()
        total = len(tags)
        # Reserve the last slot for the overflow label so no entry wraps onto another.
        limit = _PIE_MAX_ITEMS if total <= _PIE_MAX_ITEMS else _PIE_MAX_ITEMS - 1
        shown = 0
        for tag in tags:
            if shown >= limit:
                break
            icon, icon_value = _tag_display_icon(pie, tag)
            op = pie.operator(
                "wm.category_tag_activate",
                text=tag.name,
                depress=(tag.name in active_list),
                **({"icon": icon} if icon else {}),
                **({"icon_value": icon_value} if icon_value else {}),
            )
            op.tag_name = tag.name
            shown += 1

        if total > shown:
            pie.label(text=f"... and {total - shown} more", icon='INFO')


# -----------------------------------------------------------------------------
# Preferences panel: Category Tabs hotkeys
# -----------------------------------------------------------------------------


class USERPREF_PT_category_tabs_keymap(TagsPanel, Panel):
    bl_label = "Hotkeys"
    bl_icon = 'EVENT_ALT'
    bl_options = {'DEFAULT_CLOSED'}

    # Same section as Tag Management (via TagsPanel), drawn after it (registration order).

    def draw(self, context):
        layout = self.layout

        keymap = self._category_tabs_keymap(context)
        if keymap is None:
            box = layout.box()
            box.label(text="Category Tabs keymap not found", icon='ERROR')
            box.label(text="Reset the key configuration to restore defaults")
            return

        from rna_keymap_ui import draw_kmi

        kc = context.window_manager.keyconfigs.user

        layout.label(text="Hotkeys for the Category Tabs system (remappable):", icon='EVENT_ALT')
        col = layout.column(align=True)
        for item in keymap.keymap_items:
            if item.idname in {"none", ""}:
                continue
            draw_kmi([], kc, keymap, item, col, 0)

        # Both operators act on the keymap found in the context.
        row = layout.row(align=True)
        row.context_pointer_set("keymap", keymap)
        row.operator("preferences.keyitem_add", text="Add Hotkey", icon='ADD')
        row.operator("preferences.keymap_restore", text="Restore Defaults", icon='BACK')

    @staticmethod
    def _category_tabs_keymap(context):
        kc = context.window_manager.keyconfigs.user
        if kc is None:
            return None
        try:
            return kc.keymaps["Category Tabs"]
        except KeyError:
            return None


classes = (
    USERPREF_OT_category_tag_activate,
    SCREEN_MT_category_tag_pie,
    USERPREF_PT_category_tabs_keymap,
)

# NOTE: These classes are registered through space_userpref.register() (the single
# registration owner of the whole glyph tag system), same as the rest of tag_ui.py.
# This module deliberately has no register()/unregister() of its own.
