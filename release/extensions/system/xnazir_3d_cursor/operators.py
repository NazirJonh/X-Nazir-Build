# SPDX-FileCopyrightText: 2026 XNazir
#
# SPDX-License-Identifier: GPL-2.0-or-later

import bpy
from bpy.props import (
    BoolProperty,
    EnumProperty,
    FloatVectorProperty,
    IntProperty,
    StringProperty,
)
from bpy.types import Operator

# Item properties that hold the image of each button state, see `_state_props`.
_FIELD_ITEMS = (
    ('ICON', "Icon", ""),
    ('ICON_ACTIVE', "Icon Active", ""),
)


def _state_props(field):
    """Names of the (source, icon, glyph, path) properties of a button state."""
    if field == 'ICON_ACTIVE':
        return "icon_active_source", "icon_active", "glyph_active", "icon_active_path"
    return "icon_source", "icon", "glyph", "icon_path"


class SCULPT_OT_cursor_button_icon_set(Operator):
    """Set the icon of a sculpt cursor button"""
    bl_idname = "sculpt.cursor_button_icon_set"
    bl_label = "Set Icon"
    bl_options = {'INTERNAL'}
    index: IntProperty()
    field: EnumProperty(name="Field", items=_FIELD_ITEMS)
    icon_name: StringProperty(name="Icon")

    def execute(self, context):
        prefs = context.preferences
        buttons = prefs.sculpt_cursor_buttons
        if not (0 <= self.index < len(buttons)):
            return {'CANCELLED'}
        item = buttons[self.index]
        source_prop, icon_prop, _glyph_prop, _path_prop = _state_props(self.field)
        setattr(item, icon_prop, self.icon_name)
        # Picking from the built-in grid switches this state to the Blender icon source.
        setattr(item, source_prop, 'BLENDER_ICON')
        return {'FINISHED'}


class SCULPT_OT_cursor_button_glyph_set(Operator):
    """Set the glyph of a sculpt cursor button"""
    bl_idname = "sculpt.cursor_button_glyph_set"
    bl_label = "Set Glyph"
    bl_options = {'INTERNAL'}
    index: IntProperty()
    field: EnumProperty(name="Field", items=_FIELD_ITEMS)
    glyph: StringProperty(name="Glyph")

    def execute(self, context):
        prefs = context.preferences
        buttons = prefs.sculpt_cursor_buttons
        if not (0 <= self.index < len(buttons)):
            return {'CANCELLED'}
        if not self.glyph:
            return {'CANCELLED'}
        item = buttons[self.index]
        # Applied right away through RNA (with update notifiers), like the icon
        # grid operator above: no IDProperty round-trip through the dialog.
        source_prop, _icon_prop, glyph_prop, _path_prop = _state_props(self.field)
        setattr(item, source_prop, 'GLYPH')
        setattr(item, glyph_prop, self.glyph)
        return {'FINISHED'}


class SCULPT_OT_cursor_button_glyph_page(Operator):
    """Switch the page of the glyph grid in the icon picker"""
    bl_idname = "sculpt.cursor_button_glyph_page"
    bl_label = "Glyph Page"
    bl_options = {'INTERNAL'}
    # Target page, 1-based. The caller passes the page count so the value is clamped here and the
    # stored page can never leave the valid range.
    page: IntProperty()
    pages_num: IntProperty(default=1)
    # Window manager key holding the page, so the icon and the glyph grids page independently.
    page_key: StringProperty(default="sculpt_cursor_glyph_page", options={'HIDDEN'})

    def execute(self, context):
        context.window_manager[self.page_key] = min(max(self.page, 1), max(self.pages_num, 1))
        return {'FINISHED'}


class SCULPT_OT_cursor_button_icon_browse(Operator):
    """Choose an image file to draw on a sculpt cursor button"""
    bl_idname = "sculpt.cursor_button_icon_browse"
    bl_label = "Browse Image"
    bl_options = {'INTERNAL'}
    index: IntProperty()
    field: EnumProperty(name="Field", items=_FIELD_ITEMS)
    filepath: StringProperty(name="Image", subtype='FILE_PATH')
    filter_image: BoolProperty(default=True, options={'HIDDEN'})

    def execute(self, context):
        prefs = context.preferences
        buttons = prefs.sculpt_cursor_buttons
        if not (0 <= self.index < len(buttons)):
            return {'CANCELLED'}
        item = buttons[self.index]
        if self.filepath:
            source_prop, _icon_prop, _glyph_prop, path_prop = _state_props(self.field)
            setattr(item, source_prop, 'CUSTOM_FILE')
            setattr(item, path_prop, self.filepath)
        return {'FINISHED'}

    def invoke(self, context, event):
        context.window_manager.fileselect_add(self)
        return {'RUNNING_MODAL'}


class SCULPT_OT_cursor_button_icon_pick(Operator):
    bl_idname = "sculpt.cursor_button_icon_pick"
    bl_label = "Pick Icon"
    bl_description = "Pick an icon from the grid of all available icons"
    bl_options = {'INTERNAL'}
    index: IntProperty()
    field: EnumProperty(items=_FIELD_ITEMS)
    display_mode: EnumProperty(
        name="Display Mode",
        description="Where the button image comes from",
        items=(
            ('ICON', "Icon", "Pick a built-in Blender icon"),
            ('GLYPH', "Glyph", "Pick a unicode glyph"),
            ('CUSTOM', "Custom", "Use an image file from disk"),
        ),
    )
    # The glyph is edited as a hex code on the operator itself and applied on OK. Grid
    # buttons apply through sculpt.cursor_button_glyph_set right away, bypassing this field.
    glyph: StringProperty(name="Glyph", default="")
    # Hex code the dialog was opened with: execute() applies the Code field only when it differs,
    # so confirming after a grid click does not roll the button back to the opening glyph.
    initial_glyph: StringProperty(name="Initial Glyph", default="", options={'HIDDEN'})
    # Only the preview tint; the cursor buttons draw with the viewport text color.
    color: FloatVectorProperty(
        name="Color",
        subtype='COLOR_GAMMA',
        size=3,
        min=0.0,
        max=1.0,
        default=(0.0, 0.0, 0.0),
    )

    def execute(self, context):
        buttons = context.preferences.sculpt_cursor_buttons
        if not (0 <= self.index < len(buttons)):
            return {'CANCELLED'}
        item = buttons[self.index]
        # Icons and image files are applied right away by their own operators; the glyph Code
        # field is applied here only when edited (differs from the opening value), so OK after
        # a grid click keeps the clicked glyph instead of restoring the opening one.
        if self.display_mode == 'GLYPH' and self.glyph and self.glyph != self.initial_glyph:
            from bl_ui.glyph_tag_system.conversions import _hex_to_glyph
            glyph = _hex_to_glyph(self.glyph)
            if glyph:
                source_prop, _icon_prop, glyph_prop, _path_prop = _state_props(self.field)
                setattr(item, source_prop, 'GLYPH')
                setattr(item, glyph_prop, glyph)
        return {'FINISHED'}

    def invoke(self, context, event):
        wm = context.window_manager
        prefs = context.preferences
        if 0 <= self.index < len(prefs.sculpt_cursor_buttons):
            item = prefs.sculpt_cursor_buttons[self.index]
            source_prop, _icon_prop, glyph_prop, _path_prop = _state_props(self.field)
            self.display_mode = {
                'GLYPH': 'GLYPH',
                'BLENDER_ICON': 'ICON',
                'CUSTOM_FILE': 'CUSTOM',
            }.get(getattr(item, source_prop), 'ICON')
            item_glyph = getattr(item, glyph_prop)
            if item_glyph:
                from bl_ui.glyph_tag_system.conversions import _glyph_to_hex
                self.glyph = _glyph_to_hex(item_glyph)
                self.initial_glyph = self.glyph
            else:
                self.glyph = ""
                self.initial_glyph = ""
        if wm.get("sculpt_cursor_icon_filter") is None:
            wm["sculpt_cursor_icon_filter"] = ""
        if wm.get("sculpt_cursor_glyph_filter") is None:
            wm["sculpt_cursor_glyph_filter"] = ""
        if wm.get("sculpt_cursor_glyph_page") is None:
            wm["sculpt_cursor_glyph_page"] = 1
        if wm.get("sculpt_cursor_icon_page") is None:
            wm["sculpt_cursor_icon_page"] = 1
        return wm.invoke_props_dialog(self, width=_SCULPT_CURSOR_ICON_PICK_WIDTH)

    def draw(self, context):
        wm = context.window_manager
        prefs = context.preferences
        buttons = prefs.sculpt_cursor_buttons
        layout = self.layout

        if not (0 <= self.index < len(buttons)):
            layout.label(text="No active button", icon='ERROR')
            return

        item = buttons[self.index]
        _source_prop, icon_prop, glyph_prop, path_prop = _state_props(self.field)
        item_glyph = getattr(item, glyph_prop)
        item_path = getattr(item, path_prop)

        # Source tabs, as in the tag system's Edit Tag dialog: Icon / Glyph / Custom.
        row = layout.row(align=True)
        row.prop(self, "display_mode", expand=True)
        layout.separator()

        if self.display_mode == 'ICON':
            current_value = getattr(item, icon_prop)  # the identifier of the current enum item

            row = layout.row(align=True)
            row.prop(wm, '["sculpt_cursor_icon_filter"]', text="", icon='VIEWZOOM')

            flt = str(wm.get("sculpt_cursor_icon_filter", "") or "").lower()
            identifiers = [i for i in _sculpt_cursor_icon_identifiers() if not flt or flt in i.lower()]
            if not identifiers:
                layout.label(text="No icons found", icon='INFO')
                return
            shown = _draw_page_nav(
                layout, wm, "sculpt_cursor_icon_page", identifiers, _SCULPT_CURSOR_ICON_GRID_LIMIT, "icons")
            flow = layout.grid_flow(
                row_major=True,
                columns=_SCULPT_CURSOR_ICON_GRID_COLUMNS,
                even_columns=True,
                even_rows=True)
            for identifier in shown:
                props = flow.operator("sculpt.cursor_button_icon_set", text="", icon=identifier,
                                      depress=(identifier == current_value))
                props.index = self.index
                props.field = self.field
                props.icon_name = identifier
        elif self.display_mode == 'GLYPH':
            # Inline glyph grid from the same registry the Tabs/C++ picker use
            # (bl_ui.glyph_library.registry.search_glyphs, also served to C++ via
            # search_glyphs_summary). Buttons apply right away through
            # sculpt.cursor_button_glyph_set, so the dialog never relies on the
            # C++ grid popup writing the hex code past RNA update.
            row = layout.row(align=True)
            row.prop(wm, '["sculpt_cursor_glyph_filter"]', text="", icon='VIEWZOOM')

            glyph_filter = wm.get("sculpt_cursor_glyph_filter", "")
            if glyph_filter is None:
                glyph_filter = ""
            glyph_filter = str(glyph_filter).lower()

            glyph_items = []
            try:
                from bl_ui.glyph_library.registry import search_glyphs
                # The whole library: only one page of it is drawn as buttons below.
                glyph_items = search_glyphs(glyph_filter, "", 100000)
            except Exception:
                glyph_items = []

            if not glyph_items:
                layout.label(text="No glyphs found", icon='INFO')
            else:
                shown = _draw_page_nav(
                    layout, wm, "sculpt_cursor_glyph_page", glyph_items, _SCULPT_CURSOR_GLYPH_GRID_LIMIT, "glyphs")
                flow = layout.grid_flow(
                    row_major=True,
                    columns=_SCULPT_CURSOR_GLYPH_GRID_COLUMNS,
                    even_columns=True,
                    even_rows=True)
                for glyph_data in shown:
                    glyph_unicode = glyph_data.get('unicode', "")
                    if not glyph_unicode:
                        continue
                    props = flow.operator(
                        "sculpt.cursor_button_glyph_set",
                        text=glyph_unicode,
                        depress=(glyph_unicode == item_glyph))
                    props.index = self.index
                    props.field = self.field
                    props.glyph = glyph_unicode

            layout.separator()
            # Manual hex-code entry (same Code field as before, applied on OK).
            layout.prop(self, "glyph", text="Code", translate=False)
            from bl_ui.glyph_tag_system.conversions import _hex_to_glyph
            glyph = _hex_to_glyph(self.glyph) if self.glyph else ""
            if glyph:
                layout.template_glyph_preview(
                    glyph_unicode=glyph,
                    data=self.properties,
                    color_property="color",
                    size_multiplier=2.0,
                )
        else:  # 'CUSTOM'
            path_row = layout.row(align=True)
            path_display = path_row.row(align=True)
            path_display.enabled = False
            path_display.label(text=item_path if item_path else "None")
            props = path_row.operator("sculpt.cursor_button_icon_browse", text="", icon='FILE_FOLDER')
            props.index = self.index
            props.field = self.field
            if item_path:
                layout.separator()
                preview_row = layout.row()
                preview_row.alignment = 'CENTER'
                preview_row.template_icon_preview(
                    icon_key="",
                    icon_path=item_path,
                    data=self.properties,
                    color_property="color",
                    size_multiplier=2.0,
                )
            else:
                layout.label(text="No image file selected", icon='INFO')
            layout.label(text="A missing file draws a placeholder", icon='QUESTION')


_SCULPT_CURSOR_ICON_IDENTIFIERS_CACHE = None

# Width of the sculpt cursor icon picker dialog (single width for all tabs so the
# dialog does not jump when switching tabs; sized for ~15 glyph columns).
_SCULPT_CURSOR_ICON_PICK_WIDTH = 600
# Glyph grid layout: columns and max buttons drawn in one redraw. Drawing all
# 2700+ glyphs as operator buttons in grid_flow stalls the popup, so the grid
# shows the first page and asks to refine the search (same compromise as the
# plan allows; the C++ WM_OT_glyph_picker_grid GridView virtualizes instead,
# but wiring its result callback to the cursor button would need C++ changes).
_SCULPT_CURSOR_GLYPH_GRID_COLUMNS = 18
_SCULPT_CURSOR_GLYPH_GRID_LIMIT = 144  # 8 rows of 18, keeps the dialog short
# Icon grid: paged like the glyph grid so the dialog keeps the same compact height.
_SCULPT_CURSOR_ICON_GRID_COLUMNS = 18
_SCULPT_CURSOR_ICON_GRID_LIMIT = 144  # 8 rows of 18


def _draw_page_nav(layout, wm, page_key, items, page_size, noun):
    """Draw the page navigation row of a grid and return the items of the current page."""
    pages_num = max((len(items) + page_size - 1) // page_size, 1)
    # Pages are 1-based; a stale value (the search changed) is clamped.
    page = min(max(int(wm.get(page_key, 1)), 1), pages_num)

    page_row = layout.row(align=True)
    for icon, target, enabled in (
            ('REW', 1, page > 1),
            ('TRIA_LEFT', page - 1, page > 1),
    ):
        sub = page_row.row(align=True)
        sub.enabled = enabled
        props = sub.operator("sculpt.cursor_button_glyph_page", text="", icon=icon)
        props.page = target
        props.pages_num = pages_num
        props.page_key = page_key
    page_row.label(text="{0:d} / {1:d}  ({2:d} {3:s})".format(page, pages_num, len(items), noun))
    for icon, target, enabled in (
            ('TRIA_RIGHT', page + 1, page < pages_num),
            ('FF', pages_num, page < pages_num),
    ):
        sub = page_row.row(align=True)
        sub.enabled = enabled
        props = sub.operator("sculpt.cursor_button_glyph_page", text="", icon=icon)
        props.page = target
        props.pages_num = pages_num
        props.page_key = page_key
    return items[(page - 1) * page_size:page * page_size]


def _sculpt_cursor_icon_identifiers():
    """All built-in icon identifiers (cached), used by the sculpt cursor icon picker."""
    global _SCULPT_CURSOR_ICON_IDENTIFIERS_CACHE
    if _SCULPT_CURSOR_ICON_IDENTIFIERS_CACHE is None:
        # `UILayout` has no `icon` property, the enum lives on the `icon` parameter of its
        # functions (same access pattern as in `glyph_tag_system/quick_pie.py` and
        # `space_toolsystem_common.py`).
        icon_prop = bpy.types.UILayout.bl_rna.functions["label"].parameters["icon"]
        # Skip items without identifier (separators), like the C++ icon grid collector does.
        _SCULPT_CURSOR_ICON_IDENTIFIERS_CACHE = tuple(
            item.identifier for item in icon_prop.enum_items_static if item.identifier)
    return _SCULPT_CURSOR_ICON_IDENTIFIERS_CACHE


_classes = (
    SCULPT_OT_cursor_button_icon_set,
    SCULPT_OT_cursor_button_glyph_set,
    SCULPT_OT_cursor_button_glyph_page,
    SCULPT_OT_cursor_button_icon_browse,
    SCULPT_OT_cursor_button_icon_pick,
)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)


def unregister():
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
