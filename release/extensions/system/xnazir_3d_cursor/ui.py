# SPDX-FileCopyrightText: 2026 XNazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.types import Panel, UIList

from bl_ui.generic_ui_list import draw_ui_list


class SCULPT_UL_cursor_buttons(UIList):
    """List of user-defined buttons shown above the sculpt 3D cursor gizmo."""

    def draw_item(self, context, layout, data, item, icon, active_data, active_propname, index):
        row = layout.row(align=True)
        # The content dims while the button is hidden, the eye stays clickable in either state.
        sub = row.row(align=True)
        sub.enabled = item.enabled

        # Same resolution as the category tag list: a custom image must resolve to an icon id
        # (0 means the file is missing), otherwise the row falls back to the built-in icon.
        custom_icon_id = 0
        if item.icon_source == 'CUSTOM_FILE' and item.icon_path:
            custom_icon_id = layout.icon_from_file(item.icon_path)
        use_glyph = item.icon_source == 'GLYPH' and item.glyph and custom_icon_id == 0

        if use_glyph:
            sub.label(text=item.glyph, translate=False)
            icon_kwargs = {"icon": 'NONE'}
        elif custom_icon_id:
            icon_kwargs = {"icon_value": custom_icon_id}
        else:
            icon_kwargs = {"icon": item.icon}

        if item.builtin != 0:
            # Built-in buttons cannot be renamed: the name is drawn as plain text.
            sub.label(text=item.name, translate=False, **icon_kwargs)
        else:
            sub.prop(item, "name", text="", emboss=False, **icon_kwargs)
        if item.operator:
            sub.label(text=item.operator)
        if item.builtin != 0:
            sub.label(text="", icon='DECORATE')
        # Eye toggle (like the Outliner): the same "enabled" state as the panel checkbox.
        if item.enabled:
            row.prop(item, "enabled", text="", icon='HIDE_OFF', emboss=False)
        else:
            row.prop(item, "enabled", text="", icon='HIDE_ON', emboss=False)

    def filter_items(self, context, data, propname):
        # Standard name filtering (see other UIList implementations in this file).
        items = getattr(data, propname)
        flags = bpy.types.UI_UL_list.filter_items_by_name(
            self.filter_name, self.bitflag_filter_item, items, propname="name",
            reverse=self.use_filter_invert)
        return flags, []


class VIEW3D_PT_sculpt_cursor_custom_buttons(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'HEADER'
    bl_label = "Custom Buttons"
    bl_ui_units_x = 14

    def draw(self, context):
        layout = self.layout
        prefs = context.preferences
        buttons = prefs.sculpt_cursor_buttons

        col = layout.column()
        draw_ui_list(
            col, context,
            class_name="SCULPT_UL_cursor_buttons",
            unique_id="sculpt_cursor_custom_buttons",
            list_path="preferences.sculpt_cursor_buttons",
            active_index_path="preferences.sculpt_cursor_buttons_active",
        )
        custom_count = sum(1 for b in buttons if b.builtin == 0)
        if custom_count >= 12:  # SCULPT_CURSOR_CUSTOM_BUTTONS_MAX (built-ins are not counted)
            col.label(text="Button limit reached (12)", icon='ERROR')
        col.prop(prefs, "sculpt_cursor_buttons_per_row")

        idx = prefs.sculpt_cursor_buttons_active
        if 0 <= idx < len(buttons):  # -1 is valid and means "no active item"
            item = buttons[idx]
            builtin = item.builtin != 0
            col = layout.column(align=True)
            if builtin:
                col.label(text="Built-in button", icon='DECORATE')
                # The name and the operator of a built-in button are fixed; only the icons can be
                # changed (and restored with the reset button below).
                row = col.row()
                row.enabled = False
                row.prop(item, "name")
                row = col.row()
                row.enabled = False
                row.prop(item, "operator")
            else:
                col.prop(item, "name")
            col.separator()
            for field, label, source, icon, glyph, path in (
                    ('ICON', "Icon", item.icon_source, item.icon, item.glyph, item.icon_path),
                    ('ICON_ACTIVE', "Icon Active", item.icon_active_source, item.icon_active,
                     item.glyph_active, item.icon_active_path),
            ):
                row = col.row(align=True)
                row.enabled = field == 'ICON' or not item.use_single_icon
                row.label(text=label)
                icon_value = layout.icon_from_file(path) if source == 'CUSTOM_FILE' and path else 0
                if source == 'GLYPH' and glyph:
                    props = row.operator("sculpt.cursor_button_icon_pick", text=glyph)
                elif icon_value:
                    props = row.operator("sculpt.cursor_button_icon_pick", text="", icon_value=icon_value)
                else:
                    props = row.operator("sculpt.cursor_button_icon_pick", text="", icon=icon)
                props.index = idx
                props.field = field
            col.prop(item, "use_single_icon")
            col.separator()
            if not builtin:
                col.prop(item, "operator")
            col.label(text=f"ID: {item.unique_id}", icon='DOT')
            if builtin:
                props = col.operator(
                    "sculpt.cursor_button_reset", text="Reset to Default", icon='LOOP_BACK')
                props.index = idx

        # Buttons live in the preferences, so they persist only once the preferences are saved.
        layout.separator()
        row = layout.row()
        row.enabled = prefs.is_dirty
        row.operator(
            "wm.save_userpref",
            text="Save Preferences" + (" *" if prefs.is_dirty else ""),
            icon='FILE_TICK',
        )


_classes = (
    SCULPT_UL_cursor_buttons,
    VIEW3D_PT_sculpt_cursor_custom_buttons,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
