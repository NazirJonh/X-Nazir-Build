# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Blender operators for the Category Tabs / Glyph / Tag system.

Extracted verbatim from ``space_userpref.py`` (no behavioural change).
All operators that manage tags, filter modes, display modes, and icon selection
are collected here.

Functions that still live in ``space_userpref.py`` (``sync_wm_to_glyph_cache``,
``toggle_category_tag_no_save``, ``clear_category_tags_no_save``,
``update_category_tags_in_wm``, ``finalize_category_tag_changes``,
``_auto_save_tags``, ``_save_tag_order_only``, ``_sync_mode_flags_from_wm_to_cache``)
are accessed through lazy imports inside the functions that call them to avoid a
circular dependency at import time.
"""

import bpy
from bpy.types import Operator

from bl_ui.glyph_tag_system.defaults import (
    DEFAULT_TAG_GLYPH_HEX,
    _CATEGORY_TAG_DEFAULT_MODE_FLAGS,
    _CATEGORY_TAG_MODE_NAME_TO_FLAG,
)
from bl_ui.glyph_tag_system._state import (
    state,
    set_tag_order,
)
from bl_ui.glyph_tag_system.conversions import (
    _glyph_to_hex,
    _hex_to_glyph,
    _tag_custom_icon_mode_from_data,
    _tag_display_mode_from_data,
    _tag_icon_fields_from_display_mode,
)
from bl_ui.glyph_tag_system.glyph_cache import (
    mark_all_unassigned_categories_as_without_tag,
)
from bl_ui.glyph_tag_system.tags_cache import (
    _generate_unique_tag_name,
    _validate_custom_icon_path,
    _validate_icon_key,
    add_category_tag,
    create_tag,
    delete_tag,
    generate_unique_tag_name,
    get_tag_data,
    get_tag_names,
    remove_category_tag,
    rename_tag,
    toggle_category_tag,
    update_tag,
)
from bl_ui.glyph_tag_system.modes import (
    get_current_tag_mode_flag,
)
from bl_ui.glyph_tag_system.log import (
    category_debug_print,
    tag_log,
)
from bl_ui.glyph_tag_system.log import (
    _pref_log_once,
)
from bl_ui.glyph_tag_system.properties import (
    with_context_check,
)


# -----------------------------------------------------------------------------
# Lazy-import shim for the system facade (avoids an import cycle at load time)
# -----------------------------------------------------------------------------


def _get_su():
    """Return the system facade module (``glyph_tag_system.api``) via lazy import.

    Kept named ``_su`` at call sites for historical continuity; it now resolves
    to the explicit facade instead of ``space_userpref``.
    """
    import bl_ui.glyph_tag_system.api as _api
    return _api


# -----------------------------------------------------------------------------
# Shared dialog drawing
# -----------------------------------------------------------------------------


def _draw_tag_custom_icon_mode_toggle(layout, dialog):
    """Draw the Blender/Custom sub-mode toggle shown inside the Icon display mode.

    Returns:
        True when the dialog should draw its built-in icon picker, False when the custom icon
        file row applies instead.
    """
    mode_row = layout.row(align=True)
    mode_row.prop(dialog, "custom_icon_mode_ui", expand=True)
    layout.separator()
    return dialog.custom_icon_mode_ui == 'BLENDER'


def _draw_tag_custom_icon_row(layout, dialog):
    """Draw the custom icon file path, its file operators and its preview.

    The file operators are told which operator to write into via ``target_operator_ptr``, the
    decimal address of this dialog - the same channel ``screen.category_tab_icon_picker`` uses.

    Args:
        layout: Layout to draw into.
        dialog: The operator holding the ``icon_path`` and ``color`` properties.
    """
    target_ptr = str(id(dialog))

    path_split = layout.split(factor=0.38)
    path_split.alignment = 'RIGHT'
    path_split.label(text="Custom:")
    path_row = path_split.row(align=True)
    path_row.alignment = 'LEFT'

    path_display = path_row.row(align=True)
    path_display.enabled = False
    path_display.label(text=dialog.icon_path if dialog.icon_path else "None")

    pick_op = path_row.operator(
        "screen.category_tab_pick_custom_icon", text="", icon='FILE_FOLDER')
    pick_op.target_operator_ptr = target_ptr
    reload_op = path_row.operator(
        "screen.category_tab_reload_custom_icon", text="", icon='FILE_REFRESH')
    reload_op.target_operator_ptr = target_ptr

    if not dialog.icon_path:
        return

    is_valid, error_msg = _validate_custom_icon_path(dialog.icon_path)
    if not is_valid:
        layout.label(text=error_msg, icon='ERROR')
        return

    layout.separator()
    preview_row = layout.row()
    preview_row.alignment = 'CENTER'
    preview_row.template_icon_preview(
        icon_key="",
        icon_path=dialog.icon_path,
        data=dialog,
        color_property="color",
        size_multiplier=2.0
    )


# -----------------------------------------------------------------------------
# Operators
# -----------------------------------------------------------------------------


class USERPREF_OT_category_tag_remove_from_category(Operator):
    """Remove this tag from a specific category"""
    bl_idname = "wm.category_tag_remove_from_category"
    bl_label = "Remove Tag from Category"
    bl_options = {'REGISTER', 'INTERNAL'}

    category: bpy.props.StringProperty(name="Category")
    tag_name: bpy.props.StringProperty(name="Tag")
    space_type: bpy.props.IntProperty(name="Space Type", default=-1)

    def execute(self, context):
        # FIXED: Handle categories with empty names by searching through all space_types
        category_to_remove = self.category

        # If category is a display_name, try to find the actual category with empty name
        if category_to_remove:
            # First try direct removal
            success, message = remove_category_tag(category_to_remove, self.tag_name, auto_save=True, space_type=self.space_type)
            if success:
                self.report({'INFO'}, message)
                context.area.tag_redraw()
                return {'FINISHED'}

            # If direct removal failed, search for category by display_name
            for cat_key, cat_data in state.glyph_cache.items():
                if isinstance(cat_key, tuple) and len(cat_key) >= 2 and isinstance(cat_data, dict):
                    space_type_id, actual_category = cat_key
                    display_name = cat_data.get("display_name", "")

                    # Check if this category has the tag and matches the display_name
                    if (self.tag_name in cat_data.get("tags", []) and
                            display_name == category_to_remove):
                        # Found the category by display_name, use actual category name (even if empty)
                        # Global-First: Always use GLOBAL key (-1) for tag removal
                        success, message = remove_category_tag(actual_category, self.tag_name, auto_save=True, space_type=-1)
                        if success:
                            self.report({'INFO'}, f"Tag '{self.tag_name}' removed from '{display_name}'")
                            context.area.tag_redraw()
                            return {'FINISHED'}
                        break

        # If all attempts failed
        self.report({'ERROR'}, f"Could not remove tag '{self.tag_name}' from category '{category_to_remove}'")
        return {'CANCELLED'}


class USERPREF_OT_save_category_glyphs(Operator):
    """Save category glyph settings to preferences file"""
    bl_idname = "wm.save_category_glyphs"
    bl_label = "Save Category Glyphs"
    bl_options = {'REGISTER', 'INTERNAL'}

    def execute(self, context):
        # Sync from WM to cache and save to JSON
        _get_su().sync_wm_to_glyph_cache()
        return {'FINISHED'}


class USERPREF_OT_sync_category_glyphs(Operator):
    """Sync category glyph settings from window manager to file"""
    bl_idname = "wm.sync_category_glyphs"
    bl_label = "Sync Category Glyphs"
    bl_options = {'REGISTER', 'INTERNAL'}

    def execute(self, context):
        # Sync from WM to cache and save to JSON
        if _get_su().sync_wm_to_glyph_cache():
            self.report({'INFO'}, "Category glyphs synchronized")
        else:
            self.report({'WARNING'}, "No changes to synchronize")
        return {'FINISHED'}


class USERPREF_OT_category_tag_filter_set_mode(Operator):
    """Set category tag filter mode: Current Mode or All Tags"""
    bl_idname = "wm.category_tag_filter_set_mode"
    bl_label = "Set Tag Filter Mode"
    bl_options = {'REGISTER', 'INTERNAL'}

    use_current_mode: bpy.props.BoolProperty(
        name="Use Current Mode",
        description="Filter tags by current object mode (True) or show all tags (False)",
        default=True
    )

    @with_context_check
    def execute(self, context):
        wm = context.window_manager

        if self.use_current_mode:
            # Get current object mode and convert to filter mode enum string
            current_mode_flag = get_current_tag_mode_flag(context)
            # Map mode flags to RNA enum string identifiers
            mode_flag_to_enum = {
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("OBJECT_MODE", 0): "OBJECT_MODE",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("EDIT_MODE", 0): "EDIT_MODE",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("SCULPT_MODE", 0): "SCULPT_MODE",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("VERTEX_PAINT", 0): "VERTEX_PAINT",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("WEIGHT_PAINT", 0): "WEIGHT_PAINT",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("TEXTURE_PAINT", 0): "TEXTURE_PAINT",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("UV_EDIT", 0): "UV_EDIT",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("POSE_MODE", 0): "POSE_MODE",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("GEOMETRY_NODES", 0): "GEOMETRY_NODES",
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("SHADER_EDITOR", 0): "SHADER_EDITOR",
            }
            wm.category_tag_filter_mode = mode_flag_to_enum.get(current_mode_flag, "OBJECT_MODE")
        else:
            # Show all tags
            wm.category_tag_filter_mode = "ALL"

        # Trigger UI update
        context.area.tag_redraw()
        return {'FINISHED'}


class USERPREF_OT_category_tag_create(Operator):
    """Create a new category tag and assign it to the current category"""
    bl_idname = "wm.category_tag_create"
    bl_label = "Create Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    name: bpy.props.StringProperty(
        name="Name",
        description="Tag name",
        maxlen=32,
        default="New Tag"
    )
    category: bpy.props.StringProperty(
        name="Category",
        description="Category to assign the tag to",
        default="",
        options={'HIDDEN'}
    )
    glyph: bpy.props.StringProperty(
        name="Glyph",
        description="Unicode glyph",
        default=""
    )
    glyph_search: bpy.props.StringProperty(
        name="Glyph Search",
        description="Search query for glyph picker",
        default=""
    )
    color: bpy.props.FloatVectorProperty(
        name="Color",
        subtype='COLOR_GAMMA',
        size=3,
        min=0.0,
        max=1.0,
        default=(0.0, 0.0, 0.0)
    )
    current_mode_only: bpy.props.BoolProperty(
        name="Current Mode Only",
        description="Show tag only in the current object mode (otherwise shows in default modes)",
        default=True
    )
    space_type: bpy.props.IntProperty(
        name="Space Type",
        description="Space type context ID",
        default=-1,
        options={'HIDDEN'}
    )
    error_message: bpy.props.StringProperty(
        name="Error Message",
        description="Validation error message to display in the dialog",
        default="",
        options={'HIDDEN'}
    )
    validation_attempted: bpy.props.BoolProperty(
        name="Validation Attempted",
        description="Whether the user has attempted to submit the form",
        default=False,
        options={'HIDDEN'}
    )

    # NEW: Icon properties
    display_mode_ui: bpy.props.EnumProperty(
        name="Display Mode",
        items=[
            ('GLYPH', "Glyph", "Display as glyph character", '', 0),
            ('ICON', "Icon", "Display as Blender icon", '', 1),
        ],
        default='GLYPH'
    )
    def _icon_key_update(self, context):
        """Callback when icon_key is updated from C++ icon picker."""
        category_debug_print(f"[TAG ICON UPDATE] CALLED! icon_key='{self.icon_key}', display_mode_ui='{self.display_mode_ui}'")

        # Auto-switch to ICON mode when icon_key is set (IMPORTANT!)
        if self.icon_key and self.display_mode_ui != 'ICON':
            self.display_mode_ui = 'ICON'
            category_debug_print(f"[TAG ICON UPDATE] Switched display_mode_ui to ICON")

        # Force UI redraw - the icon preview should update
        if context and context.area:
            context.area.tag_redraw()

    icon_key: bpy.props.StringProperty(
        name="Icon",
        description="Blender icon identifier",
        default="",
        update=_icon_key_update
    )
    icon_source: bpy.props.EnumProperty(
        name="Icon Source",
        description="Icon source type",
        items=[
            ('GLYPH', "Glyph", "Display as glyph", 0),
            ('BLENDER_ICON', "Blender Icon", "Display as Blender icon", 1),
            ('CUSTOM', "Custom", "Display as custom icon", 2),
        ],
        default='GLYPH'
    )
    custom_icon_mode_ui: bpy.props.EnumProperty(
        name="Custom Icon Mode",
        items=[
            ('BLENDER', "Blender", "Pick a built-in Blender icon", '', 0),
            ('CUSTOM', "Custom", "Use a custom icon image file", '', 1),
        ],
        default='BLENDER'
    )
    icon_path: bpy.props.StringProperty(
        name="Icon Path",
        description="Path to a custom icon image file",
        subtype='FILE_PATH',
        default=""
    )

    @with_context_check
    def invoke(self, context, event):
        _pref_log_once(
            f"[DEBUG CREATE_TAG invoke] self={self!r}, "
            f"incoming category='{self.category}', name='{self.name}', validation_attempted={self.validation_attempted}"
        )
        context.window_manager.category_tag_glyph_hex = ""
        self.glyph_search = ""

        # Reset icon fields to defaults
        self.display_mode_ui = 'GLYPH'
        self.icon_key = ""
        self.custom_icon_mode_ui = 'BLENDER'
        self.icon_path = ""

        # Only set defaults if this is a fresh dialog (not a re-opening after validation failure)
        if not self.validation_attempted:
            # Set default glyph for tags (not category glyph)
            self.glyph = DEFAULT_TAG_GLYPH_HEX
            self.current_mode_only = True

            # Generate unique name with random suffix (only on fresh dialog)
            self.name = _generate_unique_tag_name("New Tag")

        # When validation_attempted is True, preserve the name from previous attempt
        category_debug_print(f"[CREATE_TAG invoke] Final name: '{self.name}', validation_attempted={self.validation_attempted}")

        self.error_message = ""
        _pref_log_once(
            f"[DEBUG CREATE_TAG invoke] prepared name='{self.name}', glyph='{self.glyph}', "
            f"glyph_search='{self.glyph_search}', category='{self.category}'"
        )
        # IMPORTANT: Keep width in sync with UI_CATEGORY_TAG_CREATE_POPUP_WIDTH in interface_intern.hh
        return context.window_manager.invoke_props_dialog(self, width=430)

    @with_context_check
    def execute(self, context):
        _su = _get_su()

        # DEBUG: Проверяем значение glyph при сохранении
        category_debug_print(f"[DEBUG CREATE_TAG execute] self.glyph = '{self.glyph}'")
        category_debug_print(f"[DEBUG CREATE_TAG execute] self.name = '{self.name}'")
        category_debug_print(f"[DEBUG CREATE_TAG execute] self.category = '{self.category}'")
        category_debug_print(f"[DEBUG CREATE_TAG execute] _preview_mode_active = {_su.is_preview_mode_active()}")
        category_debug_print(f"[DEBUG CREATE_TAG execute] self.icon_key = '{self.icon_key}'")
        category_debug_print(f"[DEBUG CREATE_TAG execute] self.icon_source = {self.icon_source}")
        category_debug_print(f"[DEBUG CREATE_TAG execute] self.display_mode_ui = '{self.display_mode_ui}'")

        # Validate name - show error and reopen dialog with error message
        if not self.name.strip():
            self.validation_attempted = True
            self.error_message = "Tag name is required"

            # Use timer to reopen dialog with error message
            def reopen_dialog():
                # Store operator properties for the new invocation
                props = {
                    'name': self.name,
                    'category': self.category,
                    'glyph': self.glyph,
                    'glyph_search': self.glyph_search,
                    'color': list(self.color),
                    'current_mode_only': self.current_mode_only,
                    'space_type': self.space_type,
                    'validation_attempted': True,
                    'error_message': "Tag name is required"
                }
                # Execute the operator with stored properties
                bpy.ops.wm.category_tag_create('INVOKE_DEFAULT', **props)
                return None  # Don't repeat the timer

            bpy.app.timers.register(reopen_dialog, first_interval=0.001)
            return {'FINISHED'}

        # Generate unique name with random suffix if user kept default "New Tag"
        # This ensures each tag has a unique name even if user doesn't edit it
        if self.name == "New Tag":
            self.name = _generate_unique_tag_name("New Tag")
            category_debug_print(f"[CREATE_TAG execute] Generated unique name: '{self.name}'")

        # IMPORTANT: User's settings (icon, glyph, color) are preserved even if they didn't change the default name.
        # This allows users to configure a tag visually and then just accept the default name without losing their work.
        # The name is already unique (generated with random suffix), so no conflict will occur.

        # Determine icon_source from display_mode_ui and the custom-icon sub-mode
        category_debug_print(
            f"[TAG EXECUTE] BEFORE: self.icon_key='{self.icon_key}', "
            f"self.icon_path='{self.icon_path}', self.display_mode_ui='{self.display_mode_ui}'")
        icon_source, icon_key, icon_path = _tag_icon_fields_from_display_mode(
            self.display_mode_ui, self.custom_icon_mode_ui, self.icon_key, self.icon_path)
        category_debug_print(f"[TAG EXECUTE] AFTER: icon_source='{icon_source}'")

        # Always convert glyph - needed for display in tag management panel even in Icon mode
        glyph = _hex_to_glyph(self.glyph) if self.glyph else ""
        category_debug_print(
            f"[TAG EXECUTE] FINAL: icon_key='{icon_key}', icon_path='{icon_path}', "
            f"glyph='{glyph}', icon_source='{icon_source}'")

        # Validate icon_key if using icon mode
        if icon_source == 'BLENDER_ICON' and icon_key:
            is_valid, error_msg = _validate_icon_key(icon_key)
            if not is_valid:
                self.report({'ERROR'}, error_msg)
                return {'CANCELLED'}

        if icon_source == 'CUSTOM':
            is_valid, error_msg = _validate_custom_icon_path(icon_path)
            if not is_valid:
                self.error_message = error_msg
                self.validation_attempted = True
                self.report({'ERROR'}, error_msg)
                return {'CANCELLED'}

        # Determine mode_flags based on current_mode_only checkbox
        if self.current_mode_only:
            current_mode_flag = get_current_tag_mode_flag(context)
            # Convert detailed edit modes (MESH_EDIT, CURVE_EDIT, etc.) to base EDIT_MODE
            # This matches the C++ visibility check logic which expects EDIT_MODE flag for tags
            detailed_edit_modes = {
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("MESH_EDIT", 0),
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("CURVE_EDIT", 0),
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("SURFACE_EDIT", 0),
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("ARMATURE_EDIT", 0),
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("LATTICE_EDIT", 0),
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("META_EDIT", 0),
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("FONT_EDIT", 0),
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("GREASE_PENCIL_EDIT", 0),
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("POINTCLOUD_EDIT", 0),
                _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("VOLUME_EDIT", 0),
            }
            if current_mode_flag in detailed_edit_modes:
                mode_flags = _CATEGORY_TAG_MODE_NAME_TO_FLAG.get("EDIT_MODE", 0)
                category_debug_print(f"[TAG CREATE] Converted {current_mode_flag} to EDIT_MODE ({mode_flags}) for tag visibility")
            else:
                mode_flags = current_mode_flag
        else:
            mode_flags = _CATEGORY_TAG_DEFAULT_MODE_FLAGS

        # Skip WM sync if we're in edit dialog - will be done by Save button
        skip_wm_sync = is_in_edit_dialog if 'is_in_edit_dialog' in locals() else False

        success, message = create_tag(
            self.name,
            glyph=glyph,
            color=list(self.color),
            mode_flags=mode_flags,
            icon_key=icon_key,
            icon_path=icon_path,
            icon_source=icon_source,
            auto_save=True,
            skip_wm_sync=skip_wm_sync
        )
        if success:
            # Check if we're being called from the category edit dialog
            # If category is specified and we're not already in preview mode, enable it
            is_in_edit_dialog = (self.category and self.category != "")

            if is_in_edit_dialog and not _su.is_preview_mode_active():
                # Enable preview mode for this tag assignment
                _su.set_preview_mode_active(True)
                category_debug_print(f"[CREATE_TAG] Enabled preview mode for tag assignment to '{self.category}'")

            # If category is specified, assign the tag to it
            if self.category:
                # Get space type from context to match C++ logic
                space_type = self.space_type
                if space_type == -1:
                    area = getattr(context, "area", None)
                    if area:
                        area_type_map = {
                            'VIEW_3D': 1, 'GRAPH_EDITOR': 2, 'OUTLINER': 3, 'PROPERTIES': 4,
                            'FILE_BROWSER': 5, 'IMAGE_EDITOR': 6, 'INFO': 7, 'SEQUENCE_EDITOR': 8,
                            'TEXT_EDITOR': 9, 'DOPESHEET_EDITOR': 12, 'NLA_EDITOR': 13, 'NODE_EDITOR': 16,
                            'CONSOLE': 18, 'PREFERENCES': 19, 'CLIP_EDITOR': 20,
                            'TOPBAR': 21, 'STATUSBAR': 22, 'SPREADSHEET': 23
                        }
                        space_type = area_type_map.get(area.type, -1)

                if is_in_edit_dialog:
                    # In edit dialog: use preview mode (no auto-save, no WM update)
                    category_debug_print(f"[CREATE_TAG] In edit dialog - using preview mode for tag assignment")
                    add_category_tag(self.category, self.name, auto_save=False, space_type=space_type, update_wm=False)
                    tag_log(f"Preview-assigned tag '{self.name}' to category '{self.category}'")
                    # Update wm.category_glyph_overrides so C++ UI can see the tag assignment immediately
                    category_debug_print(f"[CREATE_TAG] Updating wm.category_glyph_overrides for C++ UI visibility")
                    _su.update_category_tags_in_wm(self.category, space_type)
                else:
                    # Normal mode: immediate assignment with save and WM update
                    add_category_tag(self.category, self.name, auto_save=True, space_type=space_type)
                    tag_log(f"Auto-assigned tag '{self.name}' to category '{self.category}'")

            self.report({'INFO'}, message)

            if not is_in_edit_dialog:
                # Defer save to background thread - prevents UI freeze
                # Tag is already visible via _sync_single_tag_to_wm
                category_debug_print(f"[CREATE_TAG] Deferring save - tag already visible")
                _su._auto_save_tags()
            else:
                # In edit dialog: defer heavy sync/save until Save button is clicked
                # This prevents UI freeze when creating tags during preview
                category_debug_print(f"[CREATE_TAG] Deferring heavy sync/save - will be done by Save button")

            context.area.tag_redraw()

            # Set active index to the new tag AFTER all operations are complete
            # This ensures the tag is properly selected in the UI
            for i, t in enumerate(context.window_manager.category_tags):
                if t.name == self.name:
                    context.window_manager.category_tags_active_index = i
                    break

            return {'FINISHED'}

        self.report({'ERROR'}, message)
        return {'CANCELLED'}

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(self, "name")

        # Display mode toggle (2 options only)
        row = layout.row(align=True)
        row.prop(self, "display_mode_ui", expand=True)

        layout.separator()

        if self.display_mode_ui == 'GLYPH':
            # Glyph input with preview and search
            layout.template_glyph_selector(
                data=self.properties,
                glyph_property="glyph",
                search_property="glyph_search",
                color_property="color",
                category=self.category or "",
                show_preview=True,
                show_search=True,
                show_code=True,
            )

            # Color presets row (separate from glyph selector)
            layout.separator()
            layout.label(text="Color:")
            row = layout.row()
            row.template_color_glyph_presets(self.properties, "color")
        else:
            if _draw_tag_custom_icon_mode_toggle(layout, self):
                # Icon picker - label and button on same row like Display Mode
                icon_split = layout.split(factor=0.38)
                icon_split.alignment = 'RIGHT'
                icon_split.label(text="Icon:")
                icon_row = icon_split.row(align=True)
                icon_row.alignment = 'LEFT'
                # Pass operator pointer to icon picker so it knows which operator to update
                icon_picker_op = icon_row.operator(
                    "screen.category_tab_icon_picker", text="       Choose        ", icon='VIEWZOOM')
                # Set target_operator_ptr as decimal string of operator memory address
                icon_picker_op.target_operator_ptr = str(id(self))
                icon_row.separator()

                # Preview - always show (empty button when no icon)
                preview_row = layout.row()
                preview_row.alignment = 'CENTER'
                preview_row.template_icon_preview(
                    icon_key=self.icon_key,
                    data=self,
                    color_property="color",
                    size_multiplier=2.0
                )
            else:
                _draw_tag_custom_icon_row(layout, self)

            # Color (for monochrome icons)
            layout.separator()
            layout.label(text="Color:")
            row = layout.row()
            row.template_color_glyph_presets(self.properties, "color")

        # Separator before Current Mode Only checkbox
        layout.separator()

        # Current mode only checkbox (last item)
        layout.prop(self, "current_mode_only")

        # Show error message at the bottom if there's a validation error
        if self.error_message:
            layout.separator()
            row = layout.row()
            row.label(text=self.error_message, icon='ERROR')

    def invoke(self, context, event):
        _pref_log_once(
            f"[DEBUG CREATE_TAG invoke] self={self!r}, "
            f"incoming category='{self.category}', name='{self.name}', validation_attempted={self.validation_attempted}"
        )
        context.window_manager.category_tag_glyph_hex = ""
        self.glyph_search = ""

        # Only set defaults if this is a fresh dialog (not a re-opening after validation failure)
        if not self.validation_attempted:
            # ALWAYS set default glyph (e8e7) - required for proper display in tag management panel
            # Even if user switches to Icon mode, glyph field must have a value to avoid errors
            self.glyph = "e8e7"
            self.current_mode_only = True
        # When validation_attempted is True, preserve the values passed to the operator

        self.error_message = ""
        _pref_log_once(
            f"[DEBUG CREATE_TAG invoke] prepared glyph='{self.glyph}', "
            f"glyph_search='{self.glyph_search}', category='{self.category}'"
        )
        # IMPORTANT: Keep width in sync with UI_CATEGORY_TAG_CREATE_POPUP_WIDTH in interface_intern.hh
        return context.window_manager.invoke_props_dialog(self, width=430)

    def check(self, context):
        # Clear error message when user starts typing a name
        if self.name.strip() and self.error_message:
            self.error_message = ""
        # Trigger redraw when name changes to update the warning message
        return True


class USERPREF_OT_category_tag_add(Operator):
    """Add a new tag with default name (for quick list addition)"""
    bl_idname = "wm.category_tag_add"
    bl_label = "Add Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    @with_context_check
    def execute(self, context):
        # Generate unique default name
        tag_name = generate_unique_tag_name()

        # Create tag with default glyph and color
        # auto_save=True triggers deferred save (no blocking)
        glyph = _hex_to_glyph(DEFAULT_TAG_GLYPH_HEX)
        success, message = create_tag(
            tag_name,
            glyph,
            [0.0, 0.0, 0.0],
            auto_save=True,  # Deferred save only - no blocking
            skip_wm_sync=False  # Sync single tag to WM so it can be selected
        )

        if success:
            # _sync_single_tag_to_wm synced the tag to WM for immediate visibility
            # Skip full sync_glyph_mappings_to_wm to prevent UI freeze
            category_debug_print(f"[QUICK_CREATE_TAG] Tag created and visible - no heavy sync")

            # Set active index to the new tag
            for i, t in enumerate(context.window_manager.category_tags):
                if t.name == tag_name:
                    context.window_manager.category_tags_active_index = i
                    break

            context.area.tag_redraw()
            self.report({'INFO'}, f"Added '{tag_name}'")
            return {'FINISHED'}

        self.report({'ERROR'}, message)
        return {'CANCELLED'}


class USERPREF_OT_category_tag_edit(Operator):
    """Edit an existing tag"""
    bl_idname = "wm.category_tag_edit"
    bl_label = "Edit Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    name: bpy.props.StringProperty(name="Tag Name")
    original_name: bpy.props.StringProperty(name="Original Name", options={'HIDDEN'})
    glyph: bpy.props.StringProperty(name="Glyph")
    color: bpy.props.FloatVectorProperty(
        name="Color",
        subtype='COLOR_GAMMA',
        size=3,
        min=0.0,
        max=1.0
    )

    # NEW: Icon properties
    display_mode_ui: bpy.props.EnumProperty(
        name="Display Mode",
        items=[
            ('GLYPH', "Glyph", "Display as glyph character"),
            ('ICON', "Icon", "Display as Blender icon"),
        ],
        default='GLYPH'
    )
    def _icon_key_update(self, context):
        """Callback when icon_key is updated from C++ icon picker."""
        category_debug_print(f"[TAG ICON UPDATE] CALLED! icon_key='{self.icon_key}', display_mode_ui='{self.display_mode_ui}'")

        # Auto-switch to ICON mode when icon_key is set (IMPORTANT!)
        if self.icon_key and self.display_mode_ui != 'ICON':
            self.display_mode_ui = 'ICON'
            category_debug_print(f"[TAG ICON UPDATE] Switched display_mode_ui to ICON")

        # Force UI redraw - the icon preview should update
        if context and context.area:
            context.area.tag_redraw()

    icon_key: bpy.props.StringProperty(
        name="Icon",
        description="Blender icon identifier",
        default="",
        update=_icon_key_update
    )
    custom_icon_mode_ui: bpy.props.EnumProperty(
        name="Custom Icon Mode",
        items=[
            ('BLENDER', "Blender", "Pick a built-in Blender icon", '', 0),
            ('CUSTOM', "Custom", "Use a custom icon image file", '', 1),
        ],
        default='BLENDER'
    )
    icon_path: bpy.props.StringProperty(
        name="Icon Path",
        description="Path to a custom icon image file",
        subtype='FILE_PATH',
        default=""
    )
    # Read back by the C++ tag_icon_live_update_cb, which expects the same three-value domain
    # CategoryTagDef.icon_source uses.
    icon_source: bpy.props.EnumProperty(
        name="Icon Source",
        description="Icon source type",
        items=[
            ('GLYPH', "Glyph", "Display as glyph", 0),
            ('BLENDER_ICON', "Blender Icon", "Display as Blender icon", 1),
            ('CUSTOM', "Custom", "Display as custom icon", 2),
        ],
        default='GLYPH'
    )

    def invoke(self, context, event):
        # Save original name so we can restore it if user clears the field
        self.original_name = self.name
        # Load current values - convert Unicode glyph to hex for display
        tag_data = get_tag_data(self.name)
        self.glyph = _glyph_to_hex(tag_data.get("glyph", ""))
        self.color = tag_data.get("color", [0.0, 0.0, 0.0])

        # NEW: Load icon data
        self.icon_key = tag_data.get("icon_key", "")
        self.icon_path = tag_data.get("icon_path", "")
        self.display_mode_ui = _tag_display_mode_from_data(tag_data)
        self.custom_icon_mode_ui = _tag_custom_icon_mode_from_data(tag_data)

        context.window_manager.category_tag_glyph_hex = ""
        return context.window_manager.invoke_props_dialog(self, width=400)

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(self, "name")

        # Warn user if name is empty and stop drawing to avoid C++ crash
        if not self.name.strip():
            layout.alert = True
            layout.label(text="Tag name cannot be empty", icon='ERROR')
            layout.alert = False
            return

        # Display mode toggle (2 options only)
        row = layout.row(align=True)
        row.prop(self, "display_mode_ui", expand=True)

        layout.separator()

        if self.display_mode_ui == 'GLYPH':
            # Glyph input
            layout.template_glyph_input_row(
                self.properties,      # data
                "glyph",              # glyph_property
                None,                 # search_property (not used for Edit Tag)
                has_search=False,     # no search field in Edit Tag
                has_code=True,        # show Code field
                category=""           # no category context for Edit Tag
            )

            # Color presets with glyph buttons
            layout.label(text="Color:")
            row = layout.row()
            row.template_color_glyph_presets(self.properties, "color")

            # Glyph preview - show current glyph with color
            glyph = _hex_to_glyph(self.glyph) if self.glyph else ""
            if glyph:
                layout.template_glyph_preview(
                    glyph_unicode=glyph,
                    data=self.properties,
                    color_property="color",
                    size_multiplier=2.0
                )
        else:
            if _draw_tag_custom_icon_mode_toggle(layout, self):
                # Icon picker - label and button on same row like Display Mode
                icon_split = layout.split(factor=0.38)
                icon_split.alignment = 'RIGHT'
                icon_split.label(text="Icon:")
                icon_row = icon_split.row(align=True)
                icon_row.alignment = 'LEFT'
                # Pass operator pointer to icon picker so it knows which operator to update
                icon_picker_op = icon_row.operator("screen.category_tab_icon_picker", text="        Choose        ", icon='VIEWZOOM')
                # Set target_operator_ptr as decimal string of operator memory address
                icon_picker_op.target_operator_ptr = str(id(self))
                icon_row.separator()

                # Preview
                if self.icon_key:
                    layout.separator()
                    try:
                        import bl_ui.icon_helper as icon_helper
                        icon_id = icon_helper.icon_name_to_id(self.icon_key)
                        if icon_id > 0:
                            preview_row = layout.row()
                            preview_row.alignment = 'CENTER'
                            preview_row.label(text="", icon_value=icon_id)
                            preview_row.label(text=self.icon_key)
                    except Exception:
                        layout.label(text=f"Selected: {self.icon_key}")
            else:
                _draw_tag_custom_icon_row(layout, self)

            # Color (for monochrome icons)
            layout.separator()
            layout.label(text="Color (for monochrome icons):")
            row = layout.row()
            row.template_color_glyph_presets(self.properties, "color")

    @with_context_check
    def execute(self, context):
        # If user cleared the name field, restore the original name and cancel
        if not self.name.strip():
            self.name = self.original_name
            self.report({'WARNING'}, "Tag name cannot be empty, restored original name")
            return {'CANCELLED'}

        new_name = self.name.strip()

        # Determine icon_source from display_mode_ui and the custom-icon sub-mode
        category_debug_print(
            f"[TAG EXECUTE] BEFORE: self.icon_key='{self.icon_key}', "
            f"self.icon_path='{self.icon_path}', self.display_mode_ui='{self.display_mode_ui}'")
        icon_source, icon_key, icon_path = _tag_icon_fields_from_display_mode(
            self.display_mode_ui, self.custom_icon_mode_ui, self.icon_key, self.icon_path)
        category_debug_print(f"[TAG EXECUTE] AFTER: icon_source='{icon_source}'")

        glyph = _hex_to_glyph(self.glyph) if (icon_source == 'GLYPH' and self.glyph) else ""
        category_debug_print(
            f"[TAG EXECUTE] FINAL: icon_key='{icon_key}', icon_path='{icon_path}', glyph='{glyph}'")

        # Validate icon_key if using icon mode
        if icon_source == 'BLENDER_ICON' and icon_key:
            is_valid, error_msg = _validate_icon_key(icon_key)
            if not is_valid:
                self.report({'ERROR'}, error_msg)
                return {'CANCELLED'}

        if icon_source == 'CUSTOM':
            is_valid, error_msg = _validate_custom_icon_path(icon_path)
            if not is_valid:
                self.report({'ERROR'}, error_msg)
                return {'CANCELLED'}

        # Handle rename if name changed
        working_name = self.original_name
        if new_name != self.original_name:
            success, message = rename_tag(self.original_name, new_name, auto_save=False)
            if not success:
                self.report({'ERROR'}, message)
                return {'CANCELLED'}
            working_name = new_name

        # Update glyph/color/icon using the (possibly renamed) key
        success, message = update_tag(
            working_name,
            glyph=glyph,
            color=list(self.color),
            icon_key=icon_key,
            icon_path=icon_path,
            icon_source=icon_source,
            auto_save=True
        )
        if success:
            self.report({'INFO'}, message)
            category_debug_print(f"[EDIT_TAG] Deferring save - no heavy sync")
            _get_su()._auto_save_tags()
            context.area.tag_redraw()
            return {'FINISHED'}
        self.report({'ERROR'}, message)
        return {'CANCELLED'}


class WM_OT_category_tag_pick_icon(Operator):
    """Pick an icon for the selected tag"""
    bl_idname = "wm.category_tag_pick_icon"
    bl_label = "Pick Icon for Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    @classmethod
    def poll(cls, context):
        wm = context.window_manager
        if not wm or not hasattr(wm, 'category_tags'):
            return False
        idx = wm.category_tags_active_index
        return 0 <= idx < len(wm.category_tags)

    def execute(self, context):
        wm = context.window_manager
        active_idx = wm.category_tags_active_index
        tags = wm.category_tags

        if not (0 <= active_idx < len(tags)):
            self.report({'ERROR'}, "No tag selected")
            return {'CANCELLED'}

        tag = tags[active_idx]
        tag_name = tag.name

        # Set icon_source to ICON mode before opening picker
        if hasattr(tag, 'icon_source'):
            tag.icon_source = 1  # ICON mode

        # Update state.all_tags_cache
        if tag_name in state.all_tags_cache and isinstance(state.all_tags_cache[tag_name], dict):
            state.all_tags_cache[tag_name]["icon_source"] = 1

        # Build RNA path for the icon_key property
        target_property = f"category_tags[{active_idx}].icon_key"

        # Call the icon picker with target property
        bpy.ops.screen.category_tab_icon_picker(
            'INVOKE_DEFAULT',
            target_property=target_property
        )

        return {'FINISHED'}


class USERPREF_OT_category_tag_delete(Operator):
    """Delete a tag and remove from all categories"""
    bl_idname = "wm.category_tag_delete"
    bl_label = "Delete Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    def invoke(self, context, event):
        wm = context.window_manager
        active_idx = wm.category_tags_active_index
        tags = wm.category_tags

        if not (0 <= active_idx < len(tags)):
            self.report({'ERROR'}, "No tag selected")
            return {'CANCELLED'}

        tag_name = tags[active_idx].name
        return context.window_manager.invoke_confirm(
            self,
            event,
            message=f"Delete tag '{tag_name}'?\nIt will be removed from all categories."
        )

    @with_context_check
    def execute(self, context):
        wm = context.window_manager
        active_idx = wm.category_tags_active_index
        tags = wm.category_tags

        if not (0 <= active_idx < len(tags)):
            self.report({'ERROR'}, "No tag selected")
            return {'CANCELLED'}

        tag = tags[active_idx]
        tag_name = tag.name

        success, message = delete_tag(tag_name, auto_save=True)
        if success:
            # Tag removed from WM collection automatically via state.all_tags_cache deletion
            # Skip full sync_glyph_mappings_to_wm to prevent UI freeze
            category_debug_print(f"[DELETE_TAG] Tag deleted - skipping heavy WM sync")

            # Adjust active index if needed
            tags = wm.category_tags
            if len(tags) > 0:
                wm.category_tags_active_index = min(active_idx, len(tags) - 1)
            else:
                wm.category_tags_active_index = 0

            self.report({'INFO'}, message)
            context.area.tag_redraw()
            return {'FINISHED'}

        self.report({'ERROR'}, message)
        return {'CANCELLED'}


class USERPREF_OT_mark_all_unassigned_as_distributed(Operator):
    """Mark all unassigned categories as 'Distributed' (Without Tag).

    This clears the pending_tag_assignment flag for all categories that:
    - Have no tags assigned
    - Are marked as pending (from new extensions)

    This is useful when you want to dismiss the "New Add-ons!" button
    without manually assigning tags to each category.
    """
    bl_idname = "wm.mark_all_unassigned_as_distributed"
    bl_label = "Mark All as Distributed"
    bl_options = {'REGISTER', 'INTERNAL'}

    @with_context_check
    def execute(self, context):
        # Call the core function to mark all unassigned as distributed
        # Use space_type=-1 to process all spaces (popover is not space-specific)
        updated = mark_all_unassigned_categories_as_without_tag(
            space_type=-1,
            mode_flag=get_current_tag_mode_flag(context)
        )

        if updated > 0:
            self.report({'INFO'}, f"Marked {updated} unassigned categor{'y' if updated == 1 else 'ies'} as 'Distributed'")
        else:
            self.report({'INFO'}, "No unassigned categories found")

        # Trigger WM sync and redraw
        if context.area:
            context.area.tag_redraw()

        return {'FINISHED'}


class USERPREF_OT_category_tag_move(Operator):
    """Move a tag up or down in the list"""
    bl_idname = "wm.category_tag_move"
    bl_label = "Move Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    direction: bpy.props.EnumProperty(
        name="Direction",
        description="Move direction",
        items=[
            ('UP', "Up", "Move tag up"),
            ('DOWN', "Down", "Move tag down"),
        ],
        default='UP'
    )

    @with_context_check
    def execute(self, context):
        wm = context.window_manager
        active_idx = wm.category_tags_active_index
        tags = wm.category_tags

        category_debug_print(f"[TAG MOVE] Execute called: direction={self.direction}, active_idx={active_idx}, len(tags)={len(tags)}")

        if not (0 <= active_idx < len(tags)):
            self.report({'ERROR'}, "No tag selected")
            return {'CANCELLED'}

        # Calculate new index
        if self.direction == 'UP':
            if active_idx == 0:
                category_debug_print(f"[TAG MOVE] Already at top, cancelling")
                return {'CANCELLED'}
            new_idx = active_idx - 1
        else:  # DOWN
            if active_idx >= len(tags) - 1:
                category_debug_print(f"[TAG MOVE] Already at bottom, cancelling")
                return {'CANCELLED'}
            new_idx = active_idx + 1

        category_debug_print(f"[TAG MOVE] Moving tag from index {active_idx} to {new_idx}")

        # Get current order of tag names
        tag_names = [tag.name for tag in tags]
        category_debug_print(f"[TAG MOVE] Before swap: {tag_names}")

        # Swap the two items in the name list
        tag_names[active_idx], tag_names[new_idx] = tag_names[new_idx], tag_names[active_idx]
        category_debug_print(f"[TAG MOVE] After swap: {tag_names}")

        # Use RNA reorder function to reorder WITHOUT destroying elements
        # This properly notifies WM and preserves all data (including icon_key, icon_source)
        tags.reorder_from_names(",".join(tag_names))

        category_debug_print(f"[TAG MOVE] Reordered collection via RNA")
        wm.category_tags_active_index = new_idx

        # Update tag order cache for persistence
        set_tag_order(tag_names)
        category_debug_print(f"[TAG MOVE] Updated tag_order_cache: {state.tag_order_cache}")

        # Sync mode flags to cache and save ONLY tag order (not full sync which would reset WM)
        _su = _get_su()
        _su._sync_mode_flags_from_wm_to_cache()
        _su._save_tag_order_only()

        # Force Tag Bar redraw in ALL areas (not just current)
        # The C++ reorder_from_names sends NC_WM | ND_CATEGORY_GLYPHS, but we also
        # need to mark all Tag Bar caches as dirty and redraw all relevant regions
        for window in context.window_manager.windows:
            for area in window.screen.areas:
                if area.type in {'VIEW_3D', 'PROPERTIES', 'NODE_EDITOR', 'IMAGE_EDITOR', 'CLIP_EDITOR'}:
                    for region in area.regions:
                        if region.type == 'TAG_BAR' or region.type == 'HEADER':
                            region.tag_redraw()

        context.area.tag_redraw()
        self.report({'INFO'}, f"Moved tag {self.direction}")
        return {'FINISHED'}


class USERPREF_OT_category_tag_toggle(Operator):
    """Toggle a tag on/off for the current category"""
    bl_idname = "wm.category_tag_toggle"
    bl_label = "Toggle Category Tag"
    bl_options = {'REGISTER', 'INTERNAL'}

    category: bpy.props.StringProperty(name="Category")
    tag_name: bpy.props.StringProperty(
        name="Tag",
        description="Tag name to toggle",
        maxlen=32
    )
    space_type: bpy.props.IntProperty(
        name="Space Type",
        description="Space type ID for category lookup (-1 for global)",
        default=-1
    )

    @with_context_check
    def execute(self, context):
        # DEBUG
        category_debug_print(f"[TOGGLE_OPERATOR] CALLED: category='{self.category}', tag='{self.tag_name}', space_type={self.space_type}")

        if not self.tag_name:
            self.report({'WARNING'}, "No tag specified.")
            return {'CANCELLED'}
        # Use no-save version for live preview in edit dialog
        # Changes are persisted only when user clicks Save
        # WM is NOT updated to prevent immediate filtering changes
        category_debug_print(f"[TOGGLE_OPERATOR] Calling toggle_category_tag_no_save")
        success, message = _get_su().toggle_category_tag_no_save(
            self.category,
            self.tag_name,
            self.space_type
        )
        category_debug_print(f"[TOGGLE_OPERATOR] Result: success={success}, message='{message}'")
        if success:
            # Trigger UI redraw for immediate feedback in the dialog itself
            context.area.tag_redraw()
            return {'FINISHED'}
        self.report({'ERROR'}, message)
        return {'CANCELLED'}


class USERPREF_OT_category_clear_tags(Operator):
    """Clear all tags and mark category as not needing tag assignment (Without Tag)"""
    bl_idname = "wm.category_clear_tags"
    bl_label = "Clear Category Tags (Without Tag)"
    bl_options = {'REGISTER', 'INTERNAL'}

    category: bpy.props.StringProperty(name="Category")
    space_type: bpy.props.IntProperty(
        name="Space Type",
        description="Space type ID for category lookup (-1 for global)",
        default=-1
    )

    @with_context_check
    def execute(self, context):
        # DEBUG
        category_debug_print(f"[CLEAR_TAGS] CALLED: category='{self.category}', space_type={self.space_type}")

        if not self.category:
            self.report({'WARNING'}, "No category specified.")
            return {'CANCELLED'}

        # Use no-save version for live preview in edit dialog
        # Changes are persisted only when user clicks Save
        # WM is NOT updated to prevent immediate filtering changes
        category_debug_print(f"[CLEAR_TAGS] Calling clear_category_tags_no_save")
        success, message = _get_su().clear_category_tags_no_save(
            self.category,
            self.space_type
        )
        category_debug_print(f"[CLEAR_TAGS] Result: success={success}, message='{message}'")
        if success:
            # Trigger UI redraw for immediate feedback in the dialog itself
            context.area.tag_redraw()
            return {'FINISHED'}
        self.report({'ERROR'}, message)
        return {'CANCELLED'}


# NOTE: The operator classes above are registered by ``space_userpref.register()``, which owns the
# single class tuple for the whole system (see ``bl_ui/__init__.py``). This module deliberately
# exposes no ``register()`` of its own: a second registration path would re-register the same
# ``bl_idname``s and silently unregister the first ones.
