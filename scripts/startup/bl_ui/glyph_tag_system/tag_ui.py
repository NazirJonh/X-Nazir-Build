# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""UI classes for category-tab and tag management.

Extracted verbatim from ``space_userpref.py`` (no behavioural change).
Contains:
  - ``VIEW3D_OT_category_tabs_settings`` — popup operator for display-mode / zoom settings.
  - ``USERPREF_UL_category_tags`` — UIList for the tag list with glyph/icon rendering.
  - ``TagsPanel`` — mixin for panels registered under the "tags" context.
  - ``USERPREF_OT_tag_mode_toggle`` — toggle a single mode bit on the active tag.
  - ``USERPREF_OT_tag_mode_select_all`` — select all modes for the active tag.
  - ``USERPREF_OT_tag_mode_select_none`` — deselect all modes for the active tag.
  - ``USERPREF_OT_category_tag_set_display_mode`` — switch active tag between Glyph / Icon.
  - ``USERPREF_PT_tag_mode_filter_popover`` — popover with per-mode toggle buttons.
  - ``USERPREF_PT_tag_management`` — main two-column tag management panel.
  - ``USERPREF_PT_custom_icon_picker`` — panel for the default icon folder setting.
  - ``WM_OT_debug_tag_bar_state`` — debug operator that dumps tag-bar state to console.

Functions that call back into space_userpref (``register_category_glyph_mappings``)
use lazy imports inside their bodies to avoid a circular-import cycle.
"""

import bpy
from bpy.types import Operator, Panel, UIList

from bl_ui.glyph_tag_system.defaults import (
    _CATEGORY_TAG_ALL_MODE_FLAGS,
    _CATEGORY_TAG_DEFAULT_MODE_FLAGS,
    _CATEGORY_TAG_MODES,
    _CATEGORY_TAG_MODE_ID_TO_BIT,
)
from bl_ui.glyph_tag_system.conversions import _hex_to_glyph
from bl_ui.glyph_tag_system.log import category_debug_print
from bl_ui.glyph_tag_system._state import state
from bl_ui.glyph_tag_system.tags_cache import (
    _get_mode_flags_for_tag,
)
from bl_ui.glyph_tag_system.glyph_cache import (
    get_categories_for_tag,
    get_category_glyph_data,
    get_category_icon_data,
)
from bl_ui.glyph_tag_system.modes import _get_tag_filter_mode_flag_from_wm
from bl_ui.glyph_tag_system.handlers import _schedule_display_mode_change


# -----------------------------------------------------------------------------
# Display-mode settings popup
# -----------------------------------------------------------------------------


class VIEW3D_OT_category_tabs_settings(Operator):
    """Adjust category tabs size"""
    bl_idname = "view3d.category_tabs_settings"
    bl_label = "Display Mode Settings"
    bl_description = "Adjust display mode settings for category tabs"
    bl_options = {'REGISTER', 'UNDO'}

    @staticmethod
    def _display_mode_owner(context):
        space = context.space_data
        if space and hasattr(space, "category_tabs_display_mode"):
            return space
        return context.preferences.view

    @classmethod
    def _display_mode_value(cls, context):
        owner = cls._display_mode_owner(context)
        return owner.category_tabs_display_mode

    @classmethod
    def _zoom_owner(cls, context):
        owner = cls._display_mode_owner(context)
        if hasattr(owner, "category_tabs_zoom_icon"):
            return owner
        return context.preferences.view

    def draw(self, context):
        layout = self.layout
        prefs = context.preferences
        view = prefs.view
        display_mode_owner = self._display_mode_owner(context)
        display_mode_value = self._display_mode_value(context)
        zoom_owner = self._zoom_owner(context)

        # Display mode buttons
        layout.label(text="Display Mode")
        row = layout.row(align=True)

        # Per-editor when available; fallback to global preference.
        row.prop_enum(display_mode_owner, "category_tabs_display_mode", "GLYPHS_ONLY", text="Icon")
        row.prop_enum(display_mode_owner, "category_tabs_display_mode", "GLYPHS_TEXT", text="Mixed")
        row.prop_enum(display_mode_owner, "category_tabs_display_mode", "TEXT_ONLY", text="Text")

        # Size slider - different property based on mode
        layout.separator()
        if display_mode_value == 'GLYPHS_ONLY':
            layout.prop(zoom_owner, "category_tabs_zoom_icon", text="Icon Size")
        elif display_mode_value == 'GLYPHS_TEXT':
            layout.prop(zoom_owner, "category_tabs_zoom_mixed", text="Mixed Size")
        else:  # TEXT_ONLY
            layout.prop(zoom_owner, "category_tabs_zoom_text", text="Text Size")

        # Show active tab name option - only in Icon mode
        if display_mode_value == 'GLYPHS_ONLY':
            layout.separator()
            layout.prop(view, "category_tabs_show_active_name", text="Show Active Tab Name")
            layout.prop(view, "category_tabs_show_drag_tooltips", text="Show Drag Tooltips")
            # Inactive tab behavior - only in Icon mode
            # Sticky Tab option requires Show Active Tab Name to be enabled
            layout.separator()
            layout.label(text="Inactive Tab Settings")
            row = layout.row(align=True)
            row.prop_enum(view, "category_tabs_inactive_behavior", "DEFAULT", text="Default")
            sticky_row = row.row(align=True)
            sticky_row.active = view.category_tabs_show_active_name
            sticky_row.prop_enum(view, "category_tabs_inactive_behavior", "STICKY", text="Sticky Tab")
            # Tab shape - only in Icon mode
            layout.separator()
            layout.label(text="Tab Shape")
            row = layout.row(align=True)
            row.prop_enum(view, "category_tabs_shape", "BOX", text="Box Shape")
            row.prop_enum(view, "category_tabs_shape", "CAPSULE", text="Capsule Shape")

            # Visual effect - only in Icon mode
            layout.separator()
            visual_row = layout.row()
            visual_row.prop(view, "category_tabs_visual_effect", text="Visual Effect")

            # Outline effect with color picker
            outline_row = layout.row(align=True)
            outline_row.active = (not view.category_tabs_show_active_name and
                                  view.category_tabs_visual_effect)
            outline_row.prop(view, "category_tabs_visual_outline", text="Outline")
            outline_row.prop(view, "category_tabs_visual_outline_color", text="")

        # --- BEGIN: MIXED_MODE_CONTENT_FLAGS (optional per-type visibility in Mixed mode) ---
        # To remove: delete this entire block
        if display_mode_value == 'GLYPHS_TEXT':
            layout.separator()
            layout.label(text="Content Display")
            row = layout.row(align=True)
            row.prop(view, "category_tabs_mixed_show_glyphs", text="Glyphs")
            row.prop(view, "category_tabs_mixed_show_first_letter", text="First Letter")
            row.prop(view, "category_tabs_mixed_show_icons", text="Icons")
        # --- END: MIXED_MODE_CONTENT_FLAGS ---

        # Show color indicator option - only in Text mode
        if display_mode_value == 'TEXT_ONLY':
            layout.separator()
            layout.prop(view, "category_tabs_text_mode_show_color_indicator", text="Show Color Indicator")

        # Show colored text option - only in Text mode
        if display_mode_value == 'TEXT_ONLY':
            layout.prop(view, "category_tabs_text_mode_show_colored_text", text="Show Colored Text")

        # Hide text for inactive reserved tabs in Mixed/Text modes
        if display_mode_value in {'GLYPHS_TEXT', 'TEXT_ONLY'}:
            layout.separator()
            layout.prop(view,
                        "category_tabs_hide_reserved_inactive_text",
                        text="Reserved Tabs: Icons Only")

        # Lock editing of category data
        layout.separator()
        layout.prop(view, "category_tabs_lock_edit", text="Lock Category Editing")

    def execute(self, context):
        return {'FINISHED'}

    def invoke(self, context, event):
        # Ensure glyph mappings are registered when opening settings
        from bl_ui.glyph_tag_system import wm_sync_to_wm as _wm_sync_to_wm
        _wm_sync_to_wm.register_category_glyph_mappings()
        wm = context.window_manager
        return wm.invoke_popup(self, width=240) # in interface_panel void panel_category_tabs_settings_popover_open(bContext *C, ARegion *region) use const int popup_width = 240 * UI_SCALE_FAC;


# -----------------------------------------------------------------------------
# Category Tags UIList
# -----------------------------------------------------------------------------


class USERPREF_UL_category_tags(UIList):
    """UI List for displaying category tags with colored glyphs."""

    def draw_item(self, context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        tag = item
        # DEBUG: Log tag properties
        icon_source_val = getattr(tag, "icon_source", 0)
        icon_key_val = getattr(tag, "icon_key", "")
        glyph_val = getattr(tag, "glyph", "")
        category_debug_print(f"[UI_LIST draw_item] tag='{tag.name}' icon_source={icon_source_val} icon_key='{icon_key_val}' glyph='{glyph_val}'")

        # Check if tag uses icon (icon_source == 1) or glyph
        use_icon = (icon_source_val == 1 and icon_key_val)
        use_glyph = (not use_icon and glyph_val)

        category_debug_print(f"[UI_LIST draw_item] tag='{tag.name}' use_icon={use_icon} use_glyph={use_glyph}")

        if self.layout_type in {'DEFAULT', 'COMPACT'}:
            # Use a split with a fixed factor to separate glyph/icon and name
            # factor=0.15 gives enough space for the glyph even when resized
            split = layout.split(factor=0.15, align=True)

            # Left: Icon or Glyph (fixed relative width)
            col_glyph = split.column()
            col_glyph.ui_units_x = 4  # Keep some width reserved so glyph never disappears first

            if use_icon:
                # Display Blender icon with tag color tint
                icon_key = getattr(tag, "icon_key", "")
                col_glyph.colored_label(
                    text="",
                    icon=icon_key,
                    color_r=tag.color[0],
                    color_g=tag.color[1],
                    color_b=tag.color[2]
                )
            elif use_glyph:
                # Display colored glyph
                glyph_char = _hex_to_glyph(tag.glyph)
                col_glyph.colored_label(
                    text=glyph_char,
                    icon='NONE',
                    color_r=tag.color[0],
                    color_g=tag.color[1],
                    color_b=tag.color[2]
                )
            else:
                col_glyph.label(text="", icon='DOT')

            # Right: Name (will be truncated if not enough space)
            col_name = split.column()
            col_name.label(text=tag.name, translate=False)

        elif self.layout_type == 'GRID':
            layout.alignment = 'CENTER'
            if use_icon:
                # Display Blender icon with tag color tint
                icon_key = getattr(tag, "icon_key", "")
                layout.colored_label(
                    text="",
                    icon=icon_key,
                    color_r=tag.color[0],
                    color_g=tag.color[1],
                    color_b=tag.color[2]
                )
            elif use_glyph:
                glyph_char = _hex_to_glyph(tag.glyph)
                layout.colored_label(
                    text=glyph_char,
                    icon='NONE',
                    color_r=tag.color[0],
                    color_g=tag.color[1],
                    color_b=tag.color[2]
                )
            else:
                layout.label(text="", icon='DOT')

    def filter_items(self, context, data, propname):
        """Filter tags by WindowManager.category_tag_filter_mode (0 = all tags)."""
        items = getattr(data, propname, None)
        if not items:
            return ([], [])

        wm = context.window_manager
        filter_mode_flag = _get_tag_filter_mode_flag_from_wm(wm)
        if filter_mode_flag == 0:
            return ([], [])

        flags = []
        hidden_flag = self.bitflag_filter_item
        for item in items:
            mode_flags = int(getattr(item, "mode_flags", 0))
            visible = (mode_flags == 0) or bool(mode_flags & filter_mode_flag)
            flags.append(hidden_flag if visible else 0)

        return (flags, [])


# -----------------------------------------------------------------------------
# Category Tags Panel
# -----------------------------------------------------------------------------


class TagsPanel:
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'WINDOW'
    bl_context = "tags"


# -----------------------------------------------------------------------------
# Tag mode operators
# -----------------------------------------------------------------------------


class USERPREF_OT_tag_mode_toggle(Operator):
    """Toggle a specific mode for the current tag."""
    bl_idname = "userpref.tag_mode_toggle"
    bl_label = "Toggle Mode"
    bl_options = {'REGISTER', 'INTERNAL'}

    mode: bpy.props.EnumProperty(
        items=[(mode_id, label, "") for _name, mode_id, _bit, label, _icon in _CATEGORY_TAG_MODES]
    )

    def execute(self, context):

        wm = context.window_manager
        idx = wm.category_tags_active_index

        if not wm or not hasattr(wm, 'category_tags'):
            return {'CANCELLED'}

        if idx < 0 or idx >= len(wm.category_tags):
            return {'CANCELLED'}

        tag_name = wm.category_tags[idx].name
        if tag_name not in state.all_tags_cache:
            return {'CANCELLED'}

        tag_data = state.all_tags_cache[tag_name]
        if not isinstance(tag_data, dict):
            return {'CANCELLED'}

        mode_flags = tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS)
        bit = _CATEGORY_TAG_MODE_ID_TO_BIT.get(self.mode, 0)
        mode_flags ^= (1 << bit)  # Toggle bit

        tag_data["mode_flags"] = mode_flags

        # Sync to WM
        if idx < len(wm.category_tags):
            wm.category_tags[idx].mode_flags = mode_flags

        return {'FINISHED'}


class USERPREF_OT_tag_mode_select_all(Operator):
    """Select all modes for the current tag."""
    bl_idname = "userpref.tag_mode_select_all"
    bl_label = "Select All Modes"

    def execute(self, context):

        wm = context.window_manager
        idx = wm.category_tags_active_index

        if not wm or not hasattr(wm, 'category_tags'):
            return {'CANCELLED'}

        if idx < 0 or idx >= len(wm.category_tags):
            return {'CANCELLED'}

        tag_name = wm.category_tags[idx].name
        if tag_name in state.all_tags_cache and isinstance(state.all_tags_cache[tag_name], dict):
            state.all_tags_cache[tag_name]["mode_flags"] = _CATEGORY_TAG_ALL_MODE_FLAGS
            wm.category_tags[idx].mode_flags = _CATEGORY_TAG_ALL_MODE_FLAGS

        return {'FINISHED'}


class USERPREF_OT_tag_mode_select_none(Operator):
    """Deselect all modes for the current tag."""
    bl_idname = "userpref.tag_mode_select_none"
    bl_label = "Select None"

    def execute(self, context):

        wm = context.window_manager
        idx = wm.category_tags_active_index

        if not wm or not hasattr(wm, 'category_tags'):
            return {'CANCELLED'}

        if idx < 0 or idx >= len(wm.category_tags):
            return {'CANCELLED'}

        tag_name = wm.category_tags[idx].name
        if tag_name in state.all_tags_cache and isinstance(state.all_tags_cache[tag_name], dict):
            state.all_tags_cache[tag_name]["mode_flags"] = 0
            wm.category_tags[idx].mode_flags = 0

        return {'FINISHED'}


class USERPREF_OT_category_tag_set_display_mode(Operator):
    """Set display mode (Glyph or Icon) for a tag."""
    bl_idname = "wm.category_tag_set_display_mode"
    bl_label = "Set Display Mode"
    bl_options = {'REGISTER', 'INTERNAL', 'UNDO'}

    mode: bpy.props.EnumProperty(
        name="Mode",
        items=[
            ('GLYPH', "Glyph", "Display as glyph character"),
            ('ICON', "Icon", "Display as Blender icon"),
        ],
        default='GLYPH'
    )

    def execute(self, context):

        wm = context.window_manager
        idx = wm.category_tags_active_index

        if not wm or not hasattr(wm, 'category_tags'):
            return {'CANCELLED'}

        if idx < 0 or idx >= len(wm.category_tags):
            return {'CANCELLED'}

        tag_name = wm.category_tags[idx].name
        if tag_name not in state.all_tags_cache:
            return {'CANCELLED'}

        # Set icon_source: 0=GLYPH, 1=ICON
        icon_source = 0 if self.mode == 'GLYPH' else 1

        # Update WM immediately for visual feedback
        wm.category_tags[idx].icon_source = icon_source

        category_debug_print(f"[DISPLAY_MODE] Switched '{tag_name}' to {self.mode}, icon_source={icon_source}")

        # Schedule deferred cache update and save (prevents crash on rapid toggle)
        _schedule_display_mode_change(tag_name, icon_source)

        # Redraw UI to show change immediately
        context.area.tag_redraw()

        return {'FINISHED'}


# -----------------------------------------------------------------------------
# Tag mode filter popover
# -----------------------------------------------------------------------------


class USERPREF_PT_tag_mode_filter_popover(Panel):
    """Popover panel for selecting tag filter modes."""
    bl_label = "Filter Mode"
    bl_idname = "USERPREF_PT_tag_mode_filter_popover"
    bl_space_type = 'PREFERENCES'
    bl_region_type = 'HEADER'

    @classmethod
    def poll(cls, context):
        wm = context.window_manager
        if not wm or not hasattr(wm, 'category_tags'):
            return False
        idx = wm.category_tags_active_index
        return 0 <= idx < len(wm.category_tags)

    def draw(self, context):
        layout = self.layout
        wm = context.window_manager
        idx = wm.category_tags_active_index

        if not wm or not hasattr(wm, 'category_tags'):
            layout.label(text="No tags available")
            return

        if idx < 0 or idx >= len(wm.category_tags):
            layout.label(text="No tag selected")
            return

        tag_name = wm.category_tags[idx].name
        mode_flags = _get_mode_flags_for_tag(tag_name)

        col = layout.column(align=True)
        for _mode_name, mode_id, bit, label, mode_icon in _CATEGORY_TAG_MODES:
            is_active = bool(mode_flags & (1 << bit))
            check_icon = 'CHECKBOX_HLT' if is_active else 'CHECKBOX_DEHLT'

            row = col.row(align=True)
            # Left part: Checkbox icon
            op_check = row.operator("userpref.tag_mode_toggle", text="", icon=check_icon, emboss=False)
            op_check.mode = mode_id

            # Right part: Mode icon and label
            op_label = row.operator("userpref.tag_mode_toggle", text=label, icon=mode_icon, emboss=False)
            op_label.mode = mode_id

        # Quick buttons
        row = layout.row()
        row.operator("userpref.tag_mode_select_all", text="All")
        row.operator("userpref.tag_mode_select_none", text="None")


# -----------------------------------------------------------------------------
# Tag management panel
# -----------------------------------------------------------------------------


class USERPREF_PT_tag_management(TagsPanel, Panel):
    bl_label = "Tag Management"
    bl_icon = 'FUND'

    def draw(self, context):
        layout = self.layout
        wm = context.window_manager
        # Main container box
        main_box = layout.box()

        # Two-column layout inside the box - split for proportional sizing
        # 30% for UI List (left), 70% for detail panel (right)
        split = main_box.split(factor=0.35)

        # === Left: Tag list with buttons ===
        left_container = split.row()
        left_col = left_container.column()

        mode_row = left_col.row(align=True)
        mode_row.prop(wm, "category_tag_filter_mode", text="")
        mode_row.separator()

        # template_list
        left_col.template_list(
            "USERPREF_UL_category_tags", "",
            wm, "category_tags",
            wm, "category_tags_active_index",
            rows=20, maxrows=64
        )

        # Buttons to the right of list (all in one column)
        col_btn = left_container.column(align=True)
        col_btn.operator("wm.category_tag_add", text="", icon='ADD')
        col_btn.operator("wm.category_tag_delete", text="", icon='REMOVE')
        col_btn.separator()
        # Move Up/Down buttons - disabled when not in "All Tags" filter mode
        filter_mode_flag = _get_tag_filter_mode_flag_from_wm(wm)
        can_move = (filter_mode_flag == 0)  # 0 means "ALL" mode
        col_move = col_btn.column(align=True)
        col_move.enabled = can_move
        col_move.operator("wm.category_tag_move", text="", icon='TRIA_UP').direction = 'UP'
        col_move.operator("wm.category_tag_move", text="", icon='TRIA_DOWN').direction = 'DOWN'

        # === Right: Detail panel ===
        col_right = split.column()

        # Get selected tag
        active_idx = wm.category_tags_active_index
        tags = wm.category_tags
        tag = tags[active_idx] if 0 <= active_idx < len(tags) else None

        if tag:
            # Preview section (first)
            preview_box = col_right.box()
            preview_box.label(text="Preview (as in tabs):")

            # Use template_glyph_preview for larger glyph rendering (same as C++ category tab edit)
            # Center the glyph or icon
            preview_row = preview_box.row()
            preview_row.alignment = 'CENTER'

            # Check if tag uses icon (icon_source == 1) or glyph
            icon_source_val = getattr(tag, "icon_source", 0)
            icon_key_val = getattr(tag, "icon_key", "")
            glyph_val = tag.glyph
            category_debug_print(f"[PREVIEW] tag='{tag.name}' icon_source={icon_source_val} icon_key='{icon_key_val}' glyph='{glyph_val}'")

            use_icon = (icon_source_val == 1 and icon_key_val)
            use_glyph = (not use_icon and glyph_val)

            category_debug_print(f"[PREVIEW] tag='{tag.name}' use_icon={use_icon} use_glyph={use_glyph}")

            if use_icon:
                # Display Blender icon with tag color tint
                icon_key = getattr(tag, "icon_key", "")
                preview_row.template_icon_preview(
                    icon_key=icon_key,
                    data=tag,
                    color_property="color",
                    size_multiplier=2.0
                )
            elif use_glyph:
                glyph_char = _hex_to_glyph(tag.glyph)
                if glyph_char:
                    preview_row.template_glyph_preview(
                        glyph_unicode=glyph_char,
                        data=tag,
                        color_property="color",
                        size_multiplier=2.0
                    )

            # Name below, centered
            name_row = preview_box.row()
            name_row.alignment = 'CENTER'
            name_row.label(text=tag.name, translate=False)


            # Edit section (second)
            col_right.separator()
            col_right.label(text="Edit Tag", icon='GREASEPENCIL')

            box = col_right.box()
            box.use_property_split = True
            box.prop(tag, "name", text="Name")

            # Display Mode selector (Glyph vs Icon) - FIRST
            box.separator()
            display_split = box.split(factor=0.38)
            display_split.alignment = 'RIGHT'
            display_split.label(text="Display Mode:")
            display_row = display_split.row(align=True)
            display_row.alignment = 'LEFT'

            # Get current icon_source value (0=GLYPH, 1=ICON)
            icon_source_val = getattr(tag, "icon_source", 0)

            # Toggle buttons for display mode (always enabled for visual feedback)
            glyph_row = display_row.row(align=True)
            glyph_row.active = (icon_source_val == 0)
            op_glyph = glyph_row.operator("wm.category_tag_set_display_mode", text="Glyph",
                                          depress=(icon_source_val == 0))
            op_glyph.mode = 'GLYPH'

            icon_row = display_row.row(align=True)
            icon_row.active = (icon_source_val == 1)
            op_icon = icon_row.operator("wm.category_tag_set_display_mode", text="Icon",
                                        depress=(icon_source_val == 1))
            op_icon.mode = 'ICON'

            # Conditional UI based on display mode
            if icon_source_val == 1:
                # Icon mode - show icon picker (similar layout to Glyph mode)
                box.separator()
                icon_key_val = getattr(tag, "icon_key", "")
                # Row with label on left, fields on right (like Create/Edit Tag)
                icon_split = box.split(factor=0.38)
                icon_split.alignment = 'RIGHT'
                icon_split.label(text="Icon:")
                icon_row = icon_split.row(align=True)
                icon_row.alignment = 'LEFT'
                icon_row.operator("wm.category_tag_pick_icon", text="        Choose        ", icon='VIEWZOOM')
                icon_row.separator()
                # Preview below
                if icon_key_val:
                    box.separator()
                    try:
                        import bl_ui.icon_helper as icon_helper
                        icon_id = icon_helper.icon_name_to_id(icon_key_val)
                        if icon_id > 0:
                            # Show icon preview only (no text)
                            preview_row = box.row()
                            preview_row.alignment = 'CENTER'
                            preview_row.label(text="", icon_value=icon_id)
                    except Exception:
                        pass
            else:
                # Glyph mode - show glyph picker BELOW the mode buttons
                box.separator()

                row = box.row(align=True)
                row.prop(tag, "glyph", text="Glyph")
                op = row.operator("wm.glyph_picker_grid", text=_hex_to_glyph("f02f"), icon='NONE')
                op.target_property = f"category_tags[{active_idx}].glyph"

            # Color presets with glyph buttons
            box.separator()
            box.label(text="Color:")
            row = box.row()
            row.template_color_glyph_presets(tag, "color")

            # Filter Mode button
            row = box.row()
            row.label(text="Filter Mode:")

            # Show active mode icons at a glance
            m_flags = _get_mode_flags_for_tag(tag.name)
            m_icons = [(bit, icon) for _name, _mode_id, bit, _label, icon in _CATEGORY_TAG_MODES]

            icon_row = row.row(align=True)
            any_mode = False
            for bit, m_icon in m_icons:
                if m_flags & (1 << bit):
                    icon_row.label(text="", icon=m_icon)
                    any_mode = True

            if not any_mode:
                icon_row.label(text="", icon='RESTRICT_SELECT_ON')

            row.popover("USERPREF_PT_tag_mode_filter_popover", text="", icon='TRIA_DOWN')

            # Categories using this tag
            col_right.separator()
            col_right.label(text="Categories using this tag:", icon='FILE_PARENT')

            cats_box = col_right.box()
            categories = get_categories_for_tag(tag.name)

            if categories:
                # Use automatic columns (columns=0) but force each item to be compact
                cats_flow = cats_box.grid_flow(row_major=True, columns=0, even_columns=False, even_rows=False, align=False)

                for cat_info in categories:
                    # Extract category info
                    cat_name = cat_info['name']
                    cat_display_name = cat_info['display_name']
                    cat_space_type = cat_info['space_type']

                    # Create an aligned row inside the flow
                    item_row = cats_flow.row()
                    item_row.alignment = 'LEFT'

                    # Get all visual data for the category using the original name
                    glyph, color, display_name = get_category_glyph_data(cat_name, cat_space_type)
                    icon_key, icon_path = get_category_icon_data(cat_name, cat_space_type)  # Get icon key and path
                    # Use the display_name from category info if available
                    final_display_name = cat_display_name if cat_display_name else display_name

                    # Create row layout with Tag button (returns row for adding more buttons)
                    # Use color even if glyph is empty (for categories with display_name but no glyph)
                    tag_row = item_row.tag_button_pref_row(
                        tag_name=final_display_name,
                        glyph=glyph if glyph else "",
                        color=(color[0], color[1], color[2]) if color and any(c > 0.0 for c in color) else (0.0, 0.0, 0.0),
                        width=0,  # Auto width
                        height=0,  # Auto height
                        no_background=True,
                        align=False,  # Align buttons together for seamless appearance
                        center_glyph=False,  # Center glyph in button
                        icon_key=icon_key,  # Blender internal icon key (e.g. 'PLAY')
                        icon_path=icon_path,  # External icon path
                        operator="",  # Optional operator for button click
                        context_menu_operator="",  # TODO: Temporarily disabled
                        operator_param_name="",  # TODO: Temporarily disabled
                        operator_param_value=""  # TODO: Temporarily disabled
                    )

                    # Add delete button (X) to the same row for seamless appearance with borders
                    op_x = tag_row.operator("wm.category_tag_remove_from_category", text="", icon='X')
                    op_x.category = cat_name  # Use original category name (may be empty)
                    op_x.tag_name = tag.name
                    op_x.space_type = cat_space_type  # Pass space_type for proper lookup
            else:
                cats_box.label(text="No categories using this tag", icon='INFO')
        else:
            # No tag selected or list is empty
            col_right.label(text="Select a tag to edit", icon='INFO')
            if len(tags) == 0:
                col_right.label(text="Click '+' to create a tag", icon='ADD')


# -----------------------------------------------------------------------------
# Custom icon picker panel
# -----------------------------------------------------------------------------


class USERPREF_PT_custom_icon_picker(TagsPanel, Panel):
    bl_label = "Custom Icon Picker"
    bl_icon = 'FUND'

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False

        prefs = context.preferences
        view = prefs.view

        col = layout.column()
        col.use_property_split = True
        col.use_property_decorate = False
        col.prop(view, "category_tabs_custom_icon_directory", text="Default Icon Folder")


# -----------------------------------------------------------------------------
# Debug Tag Bar State Operator
# -----------------------------------------------------------------------------


class WM_OT_debug_tag_bar_state(bpy.types.Operator):
    """Debug: Print tag bar state to console."""
    bl_idname = "wm.debug_tag_bar_state"
    bl_label = "Debug Tag Bar State"
    bl_options = {'REGISTER', 'INTERNAL'}

    def execute(self, context):
        wm = context.window_manager
        category_debug_print("=" * 60)
        category_debug_print("[DEBUG TAG BAR STATE] Checking WM category_tags")
        category_debug_print("=" * 60)

        if not hasattr(wm, 'category_tags'):
            category_debug_print("[DEBUG TAG BAR STATE] ERROR: wm.category_tags not found!")
            return {'CANCELLED'}

        tags = wm.category_tags
        category_debug_print(f"[DEBUG TAG BAR STATE] Total tags in WM: {len(tags)}")

        for i, tag in enumerate(tags):
            icon_source = getattr(tag, 'icon_source', 0)
            icon_key = getattr(tag, 'icon_key', '')
            glyph = getattr(tag, 'glyph', '')
            mode_flags = getattr(tag, 'mode_flags', 0)
            category_debug_print(f"[DEBUG TAG BAR STATE] Tag {i}: name='{tag.name}' icon_source={icon_source} icon_key='{icon_key}' glyph='{glyph}' mode_flags={mode_flags}")

        # Also check current mode
        try:
            mode = context.mode
            category_debug_print(f"[DEBUG TAG BAR STATE] Current Blender mode: {mode}")
        except:
            category_debug_print("[DEBUG TAG BAR STATE] Could not get current mode")

        category_debug_print("=" * 60)
        return {'FINISHED'}


# -----------------------------------------------------------------------------
# Class Registration
# -----------------------------------------------------------------------------


classes = (
    VIEW3D_OT_category_tabs_settings,
    USERPREF_UL_category_tags,
    USERPREF_OT_tag_mode_toggle,
    USERPREF_OT_tag_mode_select_all,
    USERPREF_OT_tag_mode_select_none,
    USERPREF_OT_category_tag_set_display_mode,
    USERPREF_PT_tag_mode_filter_popover,
    USERPREF_PT_tag_management,
    USERPREF_PT_custom_icon_picker,
    WM_OT_debug_tag_bar_state,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
