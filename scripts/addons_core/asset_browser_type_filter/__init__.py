# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

bl_info = {
    "name": "X-Nazir: Asset Browser Type Filter",
    "author": "Nazir Galimov",
    "version": (1, 0, 0),
    "blender": (5, 2, 0),
    "location": "Asset Browser header, left of Display Mode",
    "description": "Icon filter row for the native Asset Browser: Image/Material/Brush (ID type, "
                   "shift = multi-select) plus Mask/Opacity-Alpha (Name Matching map types). "
                   "Note that a plain click on a type button rewrites every data-block filter "
                   "flag of the browser, not just the three it shows buttons for",
    "warning": "",
    "support": 'COMMUNITY',
    "category": "Interface",
}

import bpy
from bpy.types import Operator

# -------------------------------------------------------------------------
# Two independent native filter systems are combined here:
#
# 1. Image/Material/Brush: FileAssetSelectParams.filter_asset_id booleans
#    (filter_image, filter_material, filter_brush -- rna_space.cc:7778).
#    Exclusive by default; Shift-click extends to a multi-selection.
#
# 2. Mask / Opacity-Alpha: these are NOT ID types, they're built-in Name
#    Matching map-type identifiers ("MASK", "ALPHA" -- name_matching.cc:510-511),
#    toggled via the existing native operator
#    ASSET_OT_browser_name_match_map_type_toggle(identifier=...), which writes
#    to FileAssetSelectParams.filter_name_match_map_types.
#
#    That list previously had no RNA read accessor from Python (only
#    AssetShelf's equivalent list did). Added one: FileAssetSelectParams
#    .filter_name_match_map_types, a read-only collection of
#    AssetShelfNameMatchMapType items (rna_space.cc, reusing the item type
#    already registered for AssetShelf -- same underlying AssetNameMatchIdLink
#    struct), so this addon's buttons stay in sync even when Mask/Alpha are
#    toggled from the native "Name Match Filter" popover instead of from here.
# -------------------------------------------------------------------------

ID_TYPES = ("Image", "Material", "Brush")
ID_TYPE_PROP = {"Image": "filter_image", "Material": "filter_material", "Brush": "filter_brush"}
ID_TYPE_ICON = {"Image": 'IMAGE_DATA', "Material": 'MATERIAL_DATA', "Brush": 'BRUSH_DATA'}

MAP_TYPES = ("MASK", "ALPHA")
MAP_TYPE_LABEL = {"MASK": "Mask", "ALPHA": "Opacity / Alpha"}
MAP_TYPE_ICON = {"MASK": 'MOD_MASK', "ALPHA": 'IMAGE_ALPHA'}

# Below this area width, the View/Select/Library/Catalog/Asset menu row is drawn collapsed into
# a single "COLLAPSEMENU" icon -- the same rendering as Header > Show Menus being off, but
# derived from the current width instead of stored in the file (see _menus_collapsed).
# Interface-scaled: what has to fit are menu entries, not pixels.
MENU_COLLAPSE_WIDTH = 800

# The Library/Catalog fallback selector's fixed width (ui_units_x) scales linearly with area
# width between these two (interface-scaled width, ui_units_x) points, clamped outside that
# range: widest when there's room to spare, narrowest once the area itself is the scarce
# resource -- monotonic, no reversal in either direction.
LIBRARY_SELECTOR_UNITS_MIN = 5
LIBRARY_SELECTOR_UNITS_MAX = 10
LIBRARY_SELECTOR_WIDTH_MIN_AT = 200
LIBRARY_SELECTOR_WIDTH_MAX_AT = MENU_COLLAPSE_WIDTH


def _ui_scale(context):
    return context.preferences.system.ui_scale


def _library_selector_ui_units_x(context):
    area_width = context.area.width / _ui_scale(context)
    span = LIBRARY_SELECTOR_WIDTH_MAX_AT - LIBRARY_SELECTOR_WIDTH_MIN_AT
    if span <= 0:
        return LIBRARY_SELECTOR_UNITS_MIN
    t = (area_width - LIBRARY_SELECTOR_WIDTH_MIN_AT) / span
    t = min(1.0, max(0.0, t))
    return LIBRARY_SELECTOR_UNITS_MIN + t * (LIBRARY_SELECTOR_UNITS_MAX - LIBRARY_SELECTOR_UNITS_MIN)


def _is_asset_browser(context):
    space = context.space_data
    return space is not None and getattr(space, "browse_mode", None) == 'ASSETS'


def _id_filter(context):
    if not _is_asset_browser(context):
        return None
    return context.space_data.params.filter_asset_id


def _all_id_filter_flags(id_filter):
    """Every boolean flag on FileAssetSelectIDFilter, not just our 3 tracked ones -- there
    are many more (Scene, Object, Geometry categories, experimental_* types, ...). Exclusive
    selection has to clear *all* of them, or types we don't have a button for keep leaking
    through alongside whatever we did select."""
    return [p.identifier for p in id_filter.bl_rna.properties if p.type == 'BOOLEAN']


def _map_type_params(context):
    """FileAssetSelectParams, or None if we're not in an Asset Browser."""
    if not _is_asset_browser(context):
        return None
    return context.space_data.params


def _map_type_read_supported(context):
    params = _map_type_params(context)
    return params is not None and hasattr(params, "filter_name_match_map_types")


def _active_map_types(context):
    params = _map_type_params(context)
    if params is None or not hasattr(params, "filter_name_match_map_types"):
        return set()
    return {item.identifier for item in params.filter_name_match_map_types if item.identifier in MAP_TYPES}


def _clear_active_map_types(context):
    for m in _active_map_types(context):
        bpy.ops.asset.browser_name_match_map_type_toggle(identifier=m)


def _reset_id_filter_to_all(context):
    id_filter = _id_filter(context)
    if id_filter is None:
        return
    for f in _all_id_filter_flags(id_filter):
        setattr(id_filter, f, True)


class ASSETFILTER_OT_select_id_type(Operator):
    bl_idname = "asset_browser_type_filter.select_id_type"
    bl_label = "Filter Asset Type"
    bl_options = {'REGISTER', 'UNDO'}

    id_type: bpy.props.StringProperty()
    shift_extend: bpy.props.BoolProperty(default=False)

    @classmethod
    def poll(cls, context):
        return _id_filter(context) is not None

    @classmethod
    def description(cls, context, properties):
        id_filter = _id_filter(context)
        if id_filter is None:
            return "Filter the Asset Browser by data-block type"
        active = {t for t in ID_TYPES if getattr(id_filter, ID_TYPE_PROP[t])}
        target = properties.id_type
        currently = ", ".join(sorted(active)) if active else "All"
        if active == {target}:
            action = f"Click: show all types again (currently: only {target})"
        else:
            action = f"Click: show only {target} (currently shown: {currently})"
        return f"{action}\nShift+Click: add/remove {target} from the current selection"

    def invoke(self, context, event):
        self.shift_extend = event.shift
        return self.execute(context)

    def execute(self, context):
        id_filter = _id_filter(context)
        all_flags = _all_id_filter_flags(id_filter)
        prop_name = ID_TYPE_PROP[self.id_type]

        if self.shift_extend:
            # Extend/reduce the current selection by exactly this one flag, whatever else is
            # currently on or off (tracked or not) is left untouched.
            setattr(id_filter, prop_name, not getattr(id_filter, prop_name))
        else:
            currently_on = {f for f in all_flags if getattr(id_filter, f)}
            if currently_on == {prop_name}:
                # This was the only visible type -> restore "All" (every flag back on).
                for f in all_flags:
                    setattr(id_filter, f, True)
            else:
                # Exclusive: only this type's flag stays on, every other flag on the struct
                # (tracked or not -- Scene, Object, experimental_* categories, etc.) goes off,
                # so nothing unrelated keeps leaking through.
                for f in all_flags:
                    setattr(id_filter, f, f == prop_name)
            # Plain click resets the OTHER filter group too -- without this, Brush then Mask
            # (both plain clicks) would leave both active with no Shift involved.
            _clear_active_map_types(context)

        return {'FINISHED'}


class ASSETFILTER_OT_toggle_map_type(Operator):
    bl_idname = "asset_browser_type_filter.toggle_map_type"
    bl_label = "Filter Name Match Map Type"
    bl_options = {'REGISTER', 'UNDO'}

    identifier: bpy.props.StringProperty()
    shift_extend: bpy.props.BoolProperty(default=False)

    @classmethod
    def poll(cls, context):
        return _is_asset_browser(context)

    @classmethod
    def description(cls, context, properties):
        target = properties.identifier
        label = MAP_TYPE_LABEL.get(target, target)
        if not _map_type_read_supported(context):
            return f"Toggle the {label} Name Matching map type"
        active = _active_map_types(context)
        currently = ", ".join(MAP_TYPE_LABEL[m] for m in MAP_TYPES if m in active) if active else "All"
        if active == {target}:
            action = f"Click: show all map types again (currently: only {label})"
        else:
            action = f"Click: show only {label} (currently shown: {currently})"
        return f"{action}\nShift+Click: add/remove {label} from the current selection"

    def invoke(self, context, event):
        self.shift_extend = event.shift
        return self.execute(context)

    def execute(self, context):
        active_before = _active_map_types(context)

        if self.shift_extend:
            active_after = set(active_before)
            if self.identifier in active_after:
                active_after.remove(self.identifier)
            else:
                active_after.add(self.identifier)
        else:
            # Clicking the only active map type again clears the filter (-> show all).
            active_after = set() if active_before == {self.identifier} else {self.identifier}
            # Plain click resets the ID-type group back to "All" too -- same reasoning as the
            # mirror case in ASSETFILTER_OT_select_id_type.
            _reset_id_filter_to_all(context)

        # Only our two tracked identifiers are managed here; the native toggle
        # operator flips one identifier at a time, so only call it for the ones
        # whose state actually needs to change.
        for m in MAP_TYPES:
            if (m in active_before) != (m in active_after):
                bpy.ops.asset.browser_name_match_map_type_toggle(identifier=m)

        return {'FINISHED'}


def _is_tools_region_collapsed(context):
    """The 'channels (directories)' region (RGN_TYPE_TOOLS) is where the native Asset
    Browser draws the Library list and the Catalog tree. When the user drags it shut its
    width collapses to 0/1px -- same detection pattern as is_option_region_visible() in
    bl_ui.space_filebrowser uses for the TOOL_PROPS region."""
    area = context.area
    if area is None:
        return False
    for region in area.regions:
        if region.type == 'TOOLS' and region.width <= 1:
            return True
    return False


def _draw_import_settings_in_collapsed_menu(self, context):
    """Appended to ASSETBROWSER_MT_editor_menus (bpy.types.Menu.append() -- the standard way
    other add-ons extend an existing menu). _draw_editor_menus_collapsible() reuses this same
    class's draw() method in two different ways: as a flat inline row of menus while there is
    room for it, and as the contents of a single collapsed dropdown (behind the "All
    Libraries" hamburger) once there isn't. Appended draw functions run in
    both cases, so bail out while inline -- Import Settings already has its own dedicated
    button in the header row then (see _patched_header_draw); only add it here once the row
    is actually collapsed, since that's when its own button disappears from the row."""
    if not _menus_collapsed(context):
        return

    params = getattr(context.space_data, "params", None)
    if params is None or params.asset_library_reference in {'LOCAL', 'ESSENTIALS'}:
        return

    layout = self.layout
    layout.separator()
    layout.operator(
        "wm.call_panel", text="Import Settings", icon='IMPORT'
    ).name = "ASSETBROWSER_PT_import_settings"


def draw_library_catalog_fallback_selectors(layout, context):
    """Shown only while the TOOLS region (Library + Catalog tree) is collapsed.

    Opens FILE_PT_asset_catalog_selector_popover (file_panels.cc) -- a native C++ panel that
    reuses file_panel_asset_catalog_buttons_draw(), the *exact* Library column selector +
    AssetCatalogTreeView tree (asset_catalog_tree_view.cc) already drawn in the TOOLS sidebar
    itself, just exposed as a header popover. This is the same tree widget used elsewhere in
    the interface (e.g. the image grid's catalog picker) -- not a flat search field -- so it
    shows the real catalog hierarchy with click-to-select rows. Requires a C++ rebuild; falls
    back to the plain inline selectors on older builds that don't have the popover panel yet.

    Uses prop_with_popover() bound to `asset_library_reference` (a real ButtonType::Menu),
    not a plain popover() button -- binding it gives back native Ctrl+Scroll cycling
    (button_supports_cycling() in interface_handlers.cc requires but->rnaprop) that a plain
    popover() button can't have, matching the behavior of the plain `layout.prop()` selector
    this replaced when the TOOLS region isn't collapsed (draw_library_catalog_fallback_selectors
    below). text="" -- not a manually-computed label -- because #Layout::prop() always draws a
    *separate* label item for PROP_ENUM properties whenever `text` is non-empty
    (interface_layout.cc:2266, "property with separate label"); passing the library's display
    name there duplicated it next to the dropdown's own caption (which already shows the current
    enum item's name on its own). Leaving text empty keeps only the single dropdown button.
    #
    # This previously crashed (BLI_assert_unreachable in library_reference_from_enum_value,
    # seen 2026-08 testing): the panel this opens draws its own, independent library selector
    # that writes `asset_library_reference` itself (template_asset_library_column_selector in
    # file_panel_asset_catalog_buttons_draw), while *this* button's own (never-seeded)
    # PopupBlockHandle.retvalue -- still 0 from init, since nothing inside ever set it, the
    # panel's own selector applies through its own separate handle -- got re-applied to the
    # property when the *outer* popover closed, writing garbage (0, not a valid
    # eAssetLibraryType) after the panel's inner selector had already set the library
    # correctly. Fixed at the root in block_open_begin() (interface_handlers.cc): a
    # ButtonType::Menu button whose popup is panel-drawn now seeds its own retvalue with the
    # RNA property's current value before the panel can be shown, so closing it without going
    # through the button's own menu re-applies the same value (a no-op) instead of 0.
    """
    params = context.space_data.params

    if hasattr(params, "catalog_path"):
        # Fixed width -- popover() otherwise stretches to fill whatever space the row leaves
        # it, which fights the elastic spacers the caller brackets this with for centering (a
        # button filling 100% of the leftover space can't visually "center"). Scales up with
        # area width so long library names have room once space allows.
        sub = layout.row()
        sub.ui_units_x = _library_selector_ui_units_x(context)
        sub.prop_with_popover(
            params,
            "asset_library_reference",
            panel="FILE_PT_asset_catalog_selector_popover",
            text="",
        )
        return

    layout.prop(params, "asset_library_reference", text="")
    layout.prop(params, "asset_catalog_visibility", text="")
    if params.asset_catalog_visibility == 'CATALOG':
        layout.prop(params, "catalog_id", text="")


def draw_id_type_filter_row(layout, context):
    id_filter = _id_filter(context)
    active_ids = {t for t in ID_TYPES if getattr(id_filter, ID_TYPE_PROP[t])}
    active_maps = _active_map_types(context)

    # All three tracked flags being True is the neutral "no type filter" state (also the
    # state a plain Mask/Alpha click resets the group to), not a deliberate 3-way selection --
    # depressing all three then would visually look like they're filtering when they aren't.
    depressed_ids = active_ids if active_ids != set(ID_TYPES) else set()

    # Box groups the whole filter row visually apart from the rest of the header.
    box = layout.box()
    row = box.row(align=False)
    row.separator()
    for id_type in ID_TYPES:
        op = row.operator(
            ASSETFILTER_OT_select_id_type.bl_idname,
            text="",
            icon=ID_TYPE_ICON[id_type],
            depress=(id_type in depressed_ids),
        )
        op.id_type = id_type

    for identifier in MAP_TYPES:
        op = row.operator(
            ASSETFILTER_OT_toggle_map_type.bl_idname,
            text="",
            icon=MAP_TYPE_ICON[identifier],
            depress=(identifier in active_maps),
        )
        op.identifier = identifier
    row.separator()


def _has_active_tool_header(context):
    """True once the RGN_TYPE_TOOL_HEADER region (added in space_file.cc, gated by
    file_tool_header_region_poll() on area width) is actually laid out.

    Asking the region itself keeps the collapse threshold in C++ alone -- duplicating
    FILE_TOOL_HEADER_COLLAPSE_WIDTH here would silently drift from it."""
    from bl_ui.space_filebrowser import is_tool_header_region_visible
    return is_tool_header_region_visible(context)


def _menus_collapsed(context):
    """Whether the View/Select/Library/Catalog/Asset menu row should be drawn as a single
    COLLAPSEMENU icon rather than inline.

    Derived state, never stored: area.show_menus is the user's own persistent Header > Show
    Menus choice, and writing it from a draw function would both overwrite that choice for good
    (it is saved in the .blend) and change data while the interface is being drawn. So the
    manual toggle still wins when it is off, and the width check only adds collapsing on top.

    Collapse as soon as the tool header activates rather than at MENU_COLLAPSE_WIDTH, so the
    Library/Catalog selectors in the main header stop fighting the menus for space."""
    area = context.area
    if area is None or not area.show_menus:
        return True
    if _has_active_tool_header(context):
        return True
    return area.width / _ui_scale(context) < MENU_COLLAPSE_WIDTH


def _draw_editor_menus_collapsible(context, layout):
    """Menu.draw_collapsible() with _menus_collapsed() in place of its area.show_menus check."""
    from bl_ui.space_filebrowser import ASSETBROWSER_MT_editor_menus

    if _menus_collapsed(context):
        layout.menu(ASSETBROWSER_MT_editor_menus.__name__, icon='COLLAPSEMENU')
    else:
        layout.row(align=True).menu_contents(ASSETBROWSER_MT_editor_menus.__name__)


# -------------------------------------------------------------------------
# Patch draw_asset_browser_buttons in place so our row lands at a specific
# position in the header, instead of after the whole header (which is all
# .append() can do).
# -------------------------------------------------------------------------

_original_draw_asset_browser_buttons = None
_original_header_draw = None
_original_tool_header_buttons = None


def _draw_asset_browser_buttons_tool_header(self, context):
    """Replaces ASSETBROWSER_HT_tool_header.draw_asset_browser_buttons (bl_ui.space_filebrowser)
    while this add-on is enabled.

    Optimized layout for the tool header (second line) with better alignment:
    - ID Type Filter + Name Match Filter centered (equal spacers on both sides -- a single
      trailing spacer only pushes the group left, it doesn't center it).
    - Display Mode + Filter + Toggle Region pushed to the right edge.

    Search field is NOT drawn here -- it's already drawn once in the main header
    (_patched_header_draw), which stays on that line regardless of TOOL_HEADER state;
    drawing it again here would duplicate it."""
    from bl_ui.space_filebrowser import is_option_region_visible

    layout = self.layout
    space_data = context.space_data
    params = space_data.params

    # Equal elastic spacers on both sides of the filter group center it in the row.
    layout.separator_spacer()

    filter_group = layout.row(align=True)
    draw_id_type_filter_row(filter_group, context)

    name_match = filter_group.row(align=True)
    name_match.prop(
        params,
        "filter_name_match_enabled",
        text="",
        icon='FILTER_FILLED' if params.filter_name_match_enabled else 'FILTER',
        toggle=True,
        icon_only=True,
    )
    name_match.popover(panel="ASSETBROWSER_PT_name_match", text="")

    layout.separator_spacer()

    # Right side: Display Mode, Filter, Toggle Region (pinned to the right edge since nothing
    # follows them and the spacer above already claimed the leftover space).
    right_group = layout.row(align=False)

    right_group.prop_with_popover(
        params,
        "display_type",
        panel="ASSETBROWSER_PT_display",
        text="",
        icon_only=True,
    )

    right_group.popover(panel="ASSETBROWSER_PT_filter", text="", icon='FILTER')

    right_group.operator(
        "screen.region_toggle",
        text="",
        icon='PREFERENCES',
        depress=is_option_region_visible(context, space_data),
    ).region_type = 'TOOL_PROPS'


def _patched_draw_asset_browser_buttons(self, context):
    from bl_ui.space_filebrowser import is_option_region_visible

    layout = self.layout
    space_data = context.space_data
    params = space_data.params

    # Library/Catalog fallback, Import Settings and Search live in the main header instead
    # (see _patched_header_draw), not in this row -- they shouldn't move down to the tool
    # header row together with the rest of the buttons when the area gets narrow.
    #
    # No leading spacer here: the caller (_patched_header_draw) already places a fixed
    # separator() right before invoking this, right after Search, so that Search and this
    # buttons row sit pinned together at the right edge. Adding another spacer here (elastic
    # or otherwise) would reopen a gap between Search and these buttons.

    draw_id_type_filter_row(layout, context)

    name_match = layout.row(align=True)
    name_match.prop(
        params,
        "filter_name_match_enabled",
        text="",
        icon='FILTER_FILLED' if params.filter_name_match_enabled else 'FILTER',
        toggle=True,
        icon_only=True,
    )
    name_match.popover(panel="ASSETBROWSER_PT_name_match", text="")

    layout.prop_with_popover(
        params,
        "display_type",
        panel="ASSETBROWSER_PT_display",
        text="",
        icon_only=True,
    )

    layout.popover(panel="ASSETBROWSER_PT_filter", text="", icon='FILTER')

    layout.operator(
        "screen.region_toggle",
        text="",
        icon='PREFERENCES',
        depress=is_option_region_visible(context, space_data),
    ).region_type = 'TOOL_PROPS'


def _patched_header_draw(self, context):
    from bpy_extras.asset_utils import SpaceAssetInfo
    from bl_ui.space_filebrowser import FILEBROWSER_MT_editor_menus

    layout = self.layout
    space_data = context.space_data

    if space_data.active_operator is None:
        layout.template_header()

    if SpaceAssetInfo.is_asset_browser(space_data):
        _draw_editor_menus_collapsible(context, layout)

        params = space_data.params
        has_tool_header = _has_active_tool_header(context)
        tools_collapsed = _is_tools_region_collapsed(context)
        # Once the View/Select/Library/Catalog/Asset menu row collapses behind the "All
        # Libraries" hamburger (area too narrow, or TOOL_HEADER active -- see
        # _menus_collapsed), Import Settings moves into that same dropdown instead of keeping
        # its own button in this row (see _draw_import_settings_in_collapsed_menu, appended to
        # ASSETBROWSER_MT_editor_menus) -- it's rarely used, no point spending a whole
        # button/icon on it once the row is already tight on space.
        menu_collapsed = _menus_collapsed(context)
        show_import_settings_inline = not menu_collapsed
        # Import doesn't apply while browsing Current File/Essentials (native Blender hides the
        # button outright for the same two libraries -- see FILEBROWSER_HT_header.draw_asset_browser_buttons
        # in space_filebrowser.py). Here the button stays drawn either way and is only greyed
        # out: hiding it changes this group's width, which re-centers it between the two
        # elastic spacers below and shifts the library selector sideways under the user's mouse
        # mid Ctrl+Scroll -- disabling keeps the row's geometry constant across library
        # switches.
        import_settings_enabled = params.asset_library_reference not in {'LOCAL', 'ESSENTIALS'}

        # Library/Catalog fallback and Search always stay on this line -- unlike the rest of
        # the buttons row, they don't move down to the tool header row when the area narrows.
        #
        # Bracket the fallback selector + Import Settings with elastic spacers -- this centers
        # that group between the menu row and the Search+buttons cluster, and pins Search
        # (plus the buttons row, when it's still on this line) to the right edge.
        #
        # When there's nothing to show in that group (TOOLS not collapsed -- no fallback
        # selector -- and Import Settings already moved into the collapsed menu), Search
        # becomes the only thing left on this line, so it gets centered instead: exactly one
        # elastic spacer on each side of it, same weight, so it lands at the true midpoint --
        # matching the centered filter row on TOOL_HEADER below it. Using two adjacent spacers
        # here (one for the empty group's "before", one for its "after") would double the left
        # side's weight against a single one on the right and skew Search off-center.
        group_is_empty = not tools_collapsed and not show_import_settings_inline

        layout.separator_spacer()

        if tools_collapsed:
            draw_library_catalog_fallback_selectors(layout, context)

        if show_import_settings_inline:
            import_settings_row = layout.row()
            import_settings_row.enabled = import_settings_enabled
            import_settings_row.popover(
                "ASSETBROWSER_PT_import_settings", text="Import Settings", icon='IMPORT'
            )

        if not group_is_empty:
            layout.separator_spacer()

        sub = layout.row()
        sub.ui_units_x = 6
        sub.prop(params, "filter_search", text="", icon='VIEWZOOM')

        if group_is_empty and has_tool_header:
            layout.separator_spacer()

        # Once the tool header region is active (area too narrow, see
        # file_tool_header_region_poll() in space_file.cc), it draws the rest of the buttons
        # row on its own line instead -- drawing it here too would duplicate every button.
        if not has_tool_header:
            # Fixed (non-elastic) gap -- the spacer above already pinned Search to the right,
            # so the buttons row just needs to sit directly next to it, both ending up
            # together at the right edge.
            layout.separator()
            self.draw_asset_browser_buttons(context)
    else:
        FILEBROWSER_MT_editor_menus.draw_collapsible(context, layout)
        layout.separator_spacer()

    if not context.screen.show_statusbar:
        layout.template_running_jobs()


classes = (
    ASSETFILTER_OT_select_id_type,
    ASSETFILTER_OT_toggle_map_type,
)


def register():
    global _original_draw_asset_browser_buttons
    global _original_header_draw
    global _original_tool_header_buttons

    for cls in classes:
        bpy.utils.register_class(cls)

    from bl_ui.space_filebrowser import (
        ASSETBROWSER_HT_tool_header,
        ASSETBROWSER_MT_editor_menus,
        FILEBROWSER_HT_header,
    )

    # Only ever capture the originals once: a second register() (script reload, or a double
    # registration) would otherwise capture the already-patched functions as the "originals",
    # and unregister() would then restore the patch instead of removing it.
    if _original_draw_asset_browser_buttons is None:
        _original_draw_asset_browser_buttons = FILEBROWSER_HT_header.draw_asset_browser_buttons
    if _original_header_draw is None:
        _original_header_draw = FILEBROWSER_HT_header.draw
    if _original_tool_header_buttons is None:
        _original_tool_header_buttons = ASSETBROWSER_HT_tool_header.draw_asset_browser_buttons

    FILEBROWSER_HT_header.draw_asset_browser_buttons = _patched_draw_asset_browser_buttons
    FILEBROWSER_HT_header.draw = _patched_header_draw
    # The tool header region and its Header class are part of Blender itself; this only swaps
    # the row it draws for the narrow-area layout below, so disabling this add-on leaves a
    # working (just plainer) second header line rather than an empty bar.
    ASSETBROWSER_HT_tool_header.draw_asset_browser_buttons = (
        _draw_asset_browser_buttons_tool_header
    )

    ASSETBROWSER_MT_editor_menus.append(_draw_import_settings_in_collapsed_menu)


def unregister():
    from bl_ui.space_filebrowser import (
        ASSETBROWSER_HT_tool_header,
        ASSETBROWSER_MT_editor_menus,
        FILEBROWSER_HT_header,
    )

    if _original_draw_asset_browser_buttons is not None:
        FILEBROWSER_HT_header.draw_asset_browser_buttons = _original_draw_asset_browser_buttons
    if _original_header_draw is not None:
        FILEBROWSER_HT_header.draw = _original_header_draw
    if _original_tool_header_buttons is not None:
        ASSETBROWSER_HT_tool_header.draw_asset_browser_buttons = _original_tool_header_buttons

    ASSETBROWSER_MT_editor_menus.remove(_draw_import_settings_in_collapsed_menu)

    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
