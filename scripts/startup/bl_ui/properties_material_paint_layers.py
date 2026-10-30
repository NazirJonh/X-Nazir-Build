# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Layer Material tab's own draw code: the material's global paint channel set and the
channels of one stack layer.

Split out of `properties_material.py` so the panels stay thin and the channel vocabulary lives in
one place. The panels that call these functions are drawn from `properties_material.py` (they need
the Layer Material tab context), so this module only supplies functions.
"""

from collections import namedtuple

from bl_ui.properties_paint_common import (
    _LAYER_VALUE_PANEL_BY_CHANNEL,
    _MATERIAL_PAINT_CHANNEL_TOGGLE_LABELS,
    _MATERIAL_PAINT_CHANNEL_TOGGLE_UI_UNITS_X,
    _MATERIAL_PAINT_CHANNEL_UI_ORDER,
    MaterialPaintChannelView,
    _draw_channel_panel,
)

# Channel count at which a row is considered heavy: five records weigh 34 generated nodes, six
# weigh 40, over the AUTO-bake threshold of 36. A row this wide bakes instead of evaluating live.
_MATERIAL_PAINT_LAYER_HEAVY_CHANNEL_COUNT = 6


# The draw state of one mask item (see #_mask_item_ui_state).
_MaskItemUIState = namedtuple(
    "_MaskItemUIState", ("record", "base", "flat_value", "assignable", "show_map", "has_map")
)


def _mask_item_ui_state(context, item):
    """The draw state of one mask item, computed once so the branches below read by state.

    `record` is the item's structural Base Color record (its constant and its map). `flat_value` is
    the grey the base mask and a Fill correction lay while they have no map. `assignable` is
    whether the map slot is the user's to assign (only a Fill correction's; the base mask owns no
    assignable slot). `show_map` is whether the stack's own map is drawn at all (read-only, and
    only while Developer Extras is on). `has_map` is whether the record carries one.

    `assignable` and `show_map` ask the RNA view of #BKE_paint_layers_mapping_available rather than
    re-encoding which sources take a mapping: only a Fill correction may carry one, and a Paint
    correction or a filter may not. The base mask is excluded explicitly: its map the Mask brush
    paints is never the user's to assign, and it is a debug view.
    """
    record = item.channels[0] if len(item.channels) else None
    base = item.is_mask_base
    assignable = not base and item.mapping_available
    return _MaskItemUIState(
        record=record,
        base=base,
        flat_value=base or item.source == 'CONSTANT',
        assignable=assignable,
        show_map=assignable or _developer_ui_enabled(context),
        has_map=record is not None and record.image is not None,
    )


def material_layer_visible_channels(material):
    """The channels of the material's global set the layer widget can draw, in display order.

    A layer row offers a switch for every channel of the material's set, Height included: its record
    is edited through the row's own channel panel. Custom has no channel widget and is absent from
    the display order, so a set that names it still offers it nowhere in a layer; the records
    themselves are never dropped.
    """
    if material is None:
        return []
    return [
        channel_id for channel_id in _MATERIAL_PAINT_CHANNEL_UI_ORDER
        if material.paint_layers_channel_in_set(channel=channel_id)
    ]


def material_layer_channel_weight(layer):
    """How many channels the row takes part in, so how many it wires in the generated tree.

    The row's generated nodes follow the channels it takes part in, not the records it carries: a
    record with neither a map nor any alpha wires nothing and costs no node, so a fresh row of a
    six-channel material is not heavy. The measure is C++'s
    (#BKE_paint_layers_layer_channel_weight, the generator's own participation rule); above
    #_MATERIAL_PAINT_LAYER_HEAVY_CHANNEL_COUNT the row is heavy enough to bake in AUTO mode.
    """
    return layer.channel_weight


def draw_material_channel_set(layout, material):
    """Checkboxes for the material's global paint channel set, one per supported channel.

    Base Color is an ordinary channel: the stack can work without it. The set itself can never be
    empty, so the last remaining checkbox is drawn disabled rather than accepting a click that BKE
    would refuse.
    """
    col = layout.column()
    enabled = [
        channel_id for channel_id in _MATERIAL_PAINT_CHANNEL_UI_ORDER
        if material.paint_layers_channel_in_set(channel=channel_id)
    ]
    last_enabled = enabled[0] if len(enabled) == 1 else None
    for channel_id in _MATERIAL_PAINT_CHANNEL_UI_ORDER:
        row = col.row(align=True)
        row.enabled = channel_id != last_enabled
        row.prop_enum(material, "paint_layers_channels", channel_id)


def _layer_images_locked(layer):
    """Whether a row's channel maps belong to the row and must not be replaced or unlinked.

    A Paint (IMAGE) layer creates and owns its maps, which the brush writes into; a Fill (CONSTANT)
    is never painted, so the user may assign their own maps there.
    """
    return layer.source == 'IMAGE'


def _developer_ui_enabled(context):
    """Whether Developer Extras is on; a missing context (a headless draw) counts as off."""
    return context is not None and context.preferences.view.show_developer_ui


def _map_is_internal_hidden(context, record):
    """Whether the record's map is one the stack generated and hides while Developer Extras is off.

    The maps the system creates for a row (its paint canvases, a correction's map, a bake) are its
    internals: the user reaches them through painting and the layer UI. While Developer Extras is
    off a panel may still name one read-only, but must not offer its browser; with it on the
    browser is shown for debugging. A map the user assigned (a dropped file) is never hidden here.
    """
    if record.image is None or not record.image.is_paint_layer_internal_map:
        return False
    return not _developer_ui_enabled(context)


def _layer_channel_subpanel_hidden(context, layer, record):
    """Whether a layer channel's whole subpanel is debug-only.

    A Paint row owns the map of every channel it carries, so with Developer Extras off the
    subpanel is hidden even before that map exists: AO is in the default channel set but has no map
    until the first stroke, and its subpanel has only the stack's own slot and channel settings to
    show. A Fill row's map is the user's to assign, so its subpanel stays and only an internal map
    on it, if one ever lands there, hides it.
    """
    if _layer_images_locked(layer):
        return not _developer_ui_enabled(context)
    return _map_is_internal_hidden(context, record)


def _draw_layer_channel_source_preview(layout, _context, record, locked=False):
    """Compact preview of a layer channel's own image, in the panel header."""
    if record.image is not None:
        row = layout.row(align=True)
        row.enabled = not locked
        row.template_ID_browser(
            record, "image", compact=True, image_filter='PAINT_SOURCE',
        )


def _draw_layer_channel_source(panel, _context, record, locked=False):
    """The layer channel's source row: a "Drop image" picker writing the record's own image.

    `locked` draws it read-only, so the map owned by a Paint row can be seen but not swapped.
    """
    row = panel.row(align=True)
    row.enabled = not locked
    row.template_ID_browser(
        record,
        "image",
        open="image.open",
        text="Drop image: {:s}".format(record.channel),
        image_filter='PAINT_SOURCE',
        use_unlink=True,
        use_users=False,
    )
    # A Normal map made elsewhere comes in either tangent-space convention. The choice lives in the
    # record and becomes a green-flip node chain in the generated graph (Shader Editor).
    if record.channel == 'NORMAL' and not locked:
        space_row = panel.row(align=True)
        space_row.prop(record, "normal_space", text="Tangent Space", expand=True)


def _layer_color_eyedropper_path(record):
    """Path of the record's `color` that `ui.eyedropper_color` resolves from the context.

    The operator resolves `prop_data_path` against #Context, which has no `material` member, so the
    path goes through `blend_data.materials` by name. A linked material is not editable and gets no
    eyedropper.
    """
    material = record.id_data
    if material is None or material.library is not None:
        return None
    name = material.name.replace("\\", "\\\\").replace('"', '\\"')
    return 'blend_data.materials["{:s}"].{:s}'.format(name, record.path_from_id("color"))


def _layer_channel_view(layer, record, channel_id, name):
    """A #MaterialPaintLayerChannel under the shared widget's contract.

    The layer has no brush-side source slot or grid: its source is the channel record's own image,
    assigned through RNA. Its value lives in the record (`color`, RGB without alpha, for colors; `value_scalar` for
    scalars) and is edited through the layer's BKE setters. Its socket menu is the layer's own
    (#PAINT_MT_material_layer_channel_socket), which needs both the record and the owning layer.
    """
    color = None
    scalar = None
    value_panel = None
    if channel_id in ('BASE_COLOR', 'NORMAL', 'EMISSION'):
        color = (record, "color")
    else:
        scalar = (record, "value_scalar")
        value_panel = _LAYER_VALUE_PANEL_BY_CHANNEL.get(channel_id)
    return MaterialPaintChannelView(
        record,
        channel_id,
        name,
        use=(record, "use"),
        color=color,
        scalar=scalar,
        value_panel=value_panel,
        image=(record, "image"),
        socket_menu="PAINT_MT_material_layer_channel_socket",
        socket_context_set=(("material_paint_layer_channel", record),
                            ("material_paint_layer_owner", layer)),
        source_draw=lambda panel, ctx, enabled: _draw_layer_channel_source(
            panel, ctx, record, locked=_layer_images_locked(layer)),
        source_preview=lambda layout, ctx: _draw_layer_channel_source_preview(
            layout, ctx, record, locked=_layer_images_locked(layer)),
        source_has=lambda: record.image is not None,
        source_active=lambda enabled: record.image is not None,
        invert_channel=channel_id if scalar is not None else None,
        invert_marker=layer.marker,
        eyedropper_path=_layer_color_eyedropper_path(record) if color is not None else None,
    )


def _mask_item_eyedropper_path(item):
    """Path of the item's `mask_color` that `ui.eyedropper_color` resolves from the context.

    Through `blend_data.materials` by name, as for a layer channel's colour; a linked material is not
    editable and gets no eyedropper.
    """
    material = item.id_data
    if material is None or material.library is not None:
        return None
    name = material.name.replace("\\", "\\\\").replace('"', '\\"')
    return 'blend_data.materials["{:s}"].{:s}'.format(name, item.path_from_id("mask_color"))


def draw_material_mask_item(layout, context, item):
    """The controls of one mask item, the base mask or a mask correction: its own settings and one
    coverage channel drawn like a scalar channel of a layer -- the header carries its value as the
    swatch that opens the value popover, the body the map slot it reads once it has one, and the
    gradient slider while it lays a flat grey.

    A Fill correction lays a flat grey while it has no map and takes a map the user assigns. The
    base mask and a Paint correction own their map instead -- the Mask brush writes it -- so their
    slot is a debug view that Developer Extras reveals, and only a Fill correction is offered a
    Mapping panel (see #BKE_paint_layers_mapping_available). The base mask has no subpanel of its
    own either, unless it still holds a flat grey: everything inside it is that debug view. Invert
    reads the grey as `1 - value`, the map's or the flat one.
    """
    # The state every branch below reads; what each field means is #_mask_item_ui_state's.
    state = _mask_item_ui_state(context, item)
    record = state.record
    base = state.base
    flat_value = state.flat_value
    assignable = state.assignable
    show_map = state.show_map
    has_map = state.has_map

    col = layout.column()
    if not base:
        # The base is the Multiply item of the row's own mask: it has no blend to choose.
        col.prop(item, "blend_type", text="Blend")
    col.prop(item, "opacity", text="Opacity")
    if base and record is not None:
        # The base mask's slot is a debug view (see below), so its Invert is offered here, where it
        # stays reachable with Developer Extras off.
        col.prop(record, "invert", text="Invert", toggle=True)
    if record is None:
        return

    # The base mask's subpanel holds the stack's own map, so it is a debug view like its slot: with
    # Developer Extras off it is drawn only while the mask has no map at all, where the flat grey
    # is the mask's whole content.
    if base and has_map and not _developer_ui_enabled(context):
        return

    header, panel = layout.panel("material_paint_mask_item", default_closed=False)
    header.use_property_split = True
    header.use_property_decorate = False
    row = header.row(align=True)
    split = row.split(factor=0.4, align=True)
    split.label(text="Mask")
    value_row = split.row(align=True)
    if has_map:
        if show_map:
            sub = value_row.row(align=True)
            sub.enabled = assignable
            sub.template_ID_browser(record, "image", compact=True, image_filter='PAINT_SOURCE')
    elif flat_value:
        controls = value_row.row(align=True)
        # The value is the swatch alone: it shows the grey the item lays and opens the value
        # popover, where the gradient slider and the palette are -- the same widget a scalar
        # channel's value wears. `mask_value` is the scalar, so the swatch takes its colour from
        # the value itself.
        controls.context_pointer_set("material_paint_layer_mask_item", item)
        controls.template_material_paint_value_swatch(
            item, "mask_value", index=0, panel="PAINT_PT_material_mask_item_value")
        path = _mask_item_eyedropper_path(item)
        if path:
            eye = controls.operator("ui.eyedropper_color", text="", icon='EYEDROPPER')
            eye.prop_data_path = path
    else:
        value_row.label(text="No map yet")
    if not panel:
        return

    panel.separator()
    if assignable:
        row = panel.row(align=True)
        row.template_ID_browser(
            record,
            "image",
            open="image.open",
            text="Drop image: Mask",
            image_filter='PAINT_SOURCE',
            use_unlink=True,
            use_users=False,
        )
        if has_map:
            # The browser with a map is two buttons high; the toggle matches it, like its neighbours.
            invert = row.column(align=True)
            invert.scale_y = 2.0
            invert.prop(record, "invert", text="", icon='ARROW_LEFTRIGHT', toggle=True)
    elif has_map:
        # The base mask and a Paint correction own their map: it is shown read-only, and only while
        # Developer Extras is on. The base mask's Invert sits with its own settings, above.
        row = panel.row(align=True)
        if show_map:
            sub = row.row(align=True)
            sub.enabled = False
            sub.template_ID_browser(record, "image", compact=True, image_filter='PAINT_SOURCE')
        if not base:
            row.prop(record, "invert", text="", icon='ARROW_LEFTRIGHT', toggle=True)
    elif not base:
        panel.label(text="The first stroke creates the map", icon='INFO')

    # A mask that lays a flat grey edits it where the map will be once it has one: the gradient
    # value slider sits in the body, with the item's own invert beside it.
    if not has_map and flat_value:
        panel.separator()
        row = panel.row(align=True)
        row.template_material_paint_value_slider(item, "mask_value", index=0)
        if not base:
            row.prop(record, "invert", text="", icon='ARROW_LEFTRIGHT', toggle=True)
    panel.separator()


def _draw_mask_output(layout, record, item):
    """The map the filter gives back: White keeps what it matches, Black hides it, Gradient scales
    the match by the standard value.

    White and Black are two states of the item's invert flag; Gradient is a mode of its own. The
    gradient slider is the same one a map-less mask base uses, with its invert toggle beside it.
    The alpha choice moves the coverage into the alpha channel, leaving the background transparent.
    Bake to Image writes that result into a new image, to use as the mask map of another object.
    """
    header, panel = layout.panel("material_paint_mask_output", default_closed=False)
    header.label(text="Output")
    if not panel:
        return
    panel.prop(item, "output_map", text="Format", expand=True)
    panel.prop(item, "output_alpha", text="Alpha Background")
    if item.output_map == 'GRADIENT':
        row = panel.row(align=True)
        row.template_material_paint_value_slider(item, "mask_value")
        row.prop(record, "invert", text="", icon='ARROW_LEFTRIGHT', toggle=True)
    panel.operator("paint.mask_item_bake", text="Bake to Image", icon='IMAGE_DATA')


def _draw_kind_param(layout, param):
    """One parameter of a row's kind, drawn as its type asks.

    A float draws as a slider; the value is the property of the parameter that carries its type.
    Typed parameters (an enum as a menu, a color as a picker) draw here once the kind schema
    carries them; every parameter is a float in this version.
    """
    layout.prop(param, "value", text=param.label, slider=True)


def draw_material_kind_params(layout, params):
    """The parameters of a row's kind, in the order the kind reads them."""
    for param in params:
        _draw_kind_param(layout, param)


def draw_material_smart_asset_params(layout, ma):
    """The exposed parameters of an imported smart asset, over every row it laid down.

    Each parameter is the row's own property, so an edit here is an edit in the row's place with
    the undo and the invalidation that any edit of it gets. A payload parameter (a ramp, curves, a
    color balance) is drawn in its row's own panel and not here. Items under a row (a correction,
    a mask item, a folder child) draw after it, indented, with their name as the caption.
    """
    drawn_any = False

    def draw_item(item, depth):
        nonlocal drawn_any
        params = [param for param in item.kind_params if param.exposed]
        if not params:
            return
        drawn_any = True
        column = layout
        if depth > 0:
            caption = layout.row()
            caption.label(text=item.name + ":", icon='DOT')
            column = layout.column(align=True)
        for param in params:
            _draw_kind_param(column, param)

    def draw_row(row, depth):
        draw_item(row, depth)
        for effect in row.effects:
            draw_row(effect, depth + 1)
        for item in row.mask_items:
            draw_row(item, depth + 1)
        for child in row.children:
            draw_row(child, depth + 1)

    for row in ma.paint_layers:
        draw_row(row, 0)
    return drawn_any


def draw_material_mask_coverage_item(layout, _context, item):
    """The controls of a map-driven mask filter (Color ID): its map, how it matches and its output.

    Color ID takes an external ID Map (dropped like any paint source, never painted) and a list of
    the colors to keep, newest first, each with a switch and a delete button. Simple mode shares one
    tolerance and softness; Advanced hides them and unfolds a tolerance on every colour.
    """
    record = item.channels[0] if len(item.channels) else None
    if record is None:
        return
    layout.separator()

    if item.filter_type == 'COLOR_ID':
        # The empty-slot variant is the drop target: the ID Map can be dragged onto it.
        row = layout.row(align=True)
        row.template_ID_browser(
            record,
            "image",
            open="image.open",
            text="Drop image: ID Map",
            image_filter='PAINT_SOURCE',
            use_unlink=True,
            use_users=False,
        )
        layout.prop(item, "color_id_mode", text="Mode", expand=True)
        if item.color_id_mode == 'SIMPLE':
            for param in item.kind_params:
                layout.prop(param, "value", text=param.label, slider=True)

        # Pick Color sits right before the colours, with air above and below it.
        layout.separator()
        row = layout.row(align=True)
        row.enabled = record.image is not None
        row.operator("paint.id_color_pick", text="Pick Color", icon='EYEDROPPER')
        layout.separator()

        if not len(item.id_colors):
            layout.label(text="No colors: nothing is kept", icon='INFO')
        advanced = item.color_id_mode == 'ADVANCED'
        for index, color in enumerate(item.id_colors):
            row = layout.row(align=True)
            row.prop(color, "enabled", text="")
            sub = row.row(align=True)
            sub.enabled = color.enabled
            sub.prop(color, "color", text="")
            remove = row.operator("material.paint_layer_id_color_remove", text="", icon='X')
            remove.index = index
            if advanced:
                row.prop(color,
                         "show_advanced",
                         text="",
                         icon='TRIA_DOWN' if color.show_advanced else 'TRIA_RIGHT',
                         emboss=False)
            if advanced and color.show_advanced:
                sub = layout.row()
                sub.use_property_split = True
                sub.prop(color, "tolerance", text="Tolerance", slider=True)
                # A little air below an unfolded colour, so the rows read apart.
                layout.separator()

        # The output sits below the colours: it is what all of them together give back.
        layout.separator()
        _draw_mask_output(layout, record, item)
        return

    # UV Island: the map is created by the first pick.
    layout.operator("paint.uv_island_pick", text="Select Islands (Box)", icon='RESTRICT_SELECT_OFF')
    layout.separator()
    _draw_mask_output(layout, record, item)


def draw_material_layer_height(layout, material, layer):
    """Draw a layer's Height record controls, on the channel they belong to.

    Offered only while the material's channel set carries Height at all, and only for a row that has
    a Height entry to write: Inherit follows the row's blend (Mix, Add and Subtract keep their
    meaning, any other blend reads as Mix), and every mode works on the distance from the neutral
    level, so a neutral record changes nothing whatever the mode. The
    write switch suppresses a record the row already holds -- on a row with none it would be a flag
    waiting to silence a map added later -- so it is drawn only where it acts.
    """
    if not material.paint_layers_channel_in_set(channel='HEIGHT'):
        return
    settings = next((entry for entry in layer.channel_settings if entry.channel == 'HEIGHT'), None)
    if settings is None:
        return
    layout.separator()
    col = layout.column(align=True)
    col.prop(settings, "height_blend_type", text="Height Blend")
    if settings.height_blend_type != 'INHERIT':
        col.prop(settings, "height_scale", text="Height Scale")
    if any(record.channel == 'HEIGHT' for record in layer.channels):
        col.prop(settings, "height_write", text="Write Height")


def draw_material_layer_channels(layout, context, material, layer):
    """Draw a stack layer's channels with the shared PBR Paint channel widget.

    The same fixed channel order and toggle buttons as the brush panel, but each channel's value
    and source come from the layer's own #MaterialPaintLayerChannel records. The channel set drives
    the rows: every channel of the material's set is offered, and one the layer carries no record
    for (or carries DISABLED) reads as off. Enabling it lazily creates the record; a channel outside
    the set is never shown, even when the row still holds its record and map.
    """
    channel_ids = material_layer_visible_channels(material)
    is_filter = layer.source == 'FILTER'
    # A filter and an Anchor Point Reader have no map or value per channel: the switches are all.
    switches_only = is_filter or layer.source == 'ANCHOR_POINT'
    is_generator = layer.source == 'GENERATOR'
    if is_filter:
        # A tangent normal is not a colour a filter may change.
        channel_ids = [channel_id for channel_id in channel_ids if channel_id != 'NORMAL']
    if is_generator:
        # A tangent normal is not a ramp a generator can produce either.
        channel_ids = [channel_id for channel_id in channel_ids if channel_id != 'NORMAL']
    if not channel_ids:
        layout.label(text="No channels", icon='INFO')
        return

    records = {record.channel: record for record in layer.channels}

    flow = layout.grid_flow(row_major=True, columns=0, even_columns=False, even_rows=False,
                            align=False)
    flow.use_property_split = False
    flow.use_property_decorate = False
    for channel_id in channel_ids:
        record = records.get(channel_id)
        enabled = record is not None and record.use
        col = flow.column(align=False)
        if is_generator:
            # An eye opens the channel's ramp on the gizmo; exactly one channel can be open. The
            # eye sits right before its channel switch, so the row reads "what the gizmo shows" and
            # "what the row paints" side by side.
            color_channel = channel_id in {'BASE_COLOR', 'EMISSION'}
            has_ramp = (layer.gradient_ramp is not None if color_channel else
                        record is not None and record.gradient_ramp is not None)
            col.ui_units_x = _MATERIAL_PAINT_CHANNEL_TOGGLE_UI_UNITS_X + 1.3
            row = col.row(align=True)
            if channel_id != 'NORMAL':
                # The eye is always in the layout, even before a channel is activated: it is
                # disabled while there is no ramp to show, and turns into a working eye as soon as
                # the channel gets its value ramp.
                gizmo_shown = has_ramp and layer.gradient_gizmo_channel == channel_id
                eye_row = row.row(align=True)
                eye_row.enabled = has_ramp
                eye = eye_row.operator(
                    "paint.paint_layer_gradient_set_gizmo_channel",
                    text="",
                    icon='HIDE_OFF' if gizmo_shown else 'HIDE_ON',
                )
                eye.marker = layer.marker
                eye.channel = channel_id
            else:
                # Normal has no generator ramp at all; keep its switch in line with the rest.
                row.label(text="", icon='BLANK1')
            op = row.operator(
                "material.paint_layer_channel_toggle",
                text=_MATERIAL_PAINT_CHANNEL_TOGGLE_LABELS[channel_id],
                depress=enabled,
            )
        else:
            col.ui_units_x = _MATERIAL_PAINT_CHANNEL_TOGGLE_UI_UNITS_X
            op = col.operator(
                "material.paint_layer_channel_toggle",
                text=_MATERIAL_PAINT_CHANNEL_TOGGLE_LABELS[channel_id],
                depress=enabled,
            )
        op.marker = layer.marker
        op.channel = channel_id
        op.enable = not enabled

    if switches_only:
        # The switches are the whole choice: no map or value per channel.
        return

    if is_generator:
        # Every enabled channel gets its own subpanel, like a painted layer's channels. A colour
        # channel shows the row's ColorRamp inside it; a scalar channel shows its own value ramp,
        # whose grey stops are edited with the gradient value slider the Mask Base uses.
        channel_col = layout.column(align=False)
        first = True
        for channel_id in channel_ids:
            record = records.get(channel_id)
            if record is None or not record.use:
                continue
            if not first:
                channel_col.separator()
            first = False
            header, panel = channel_col.panel(
                "material_paint_gradient_{:s}".format(channel_id.lower()),
                default_closed=False,
            )
            header.label(text=_MATERIAL_PAINT_CHANNEL_TOGGLE_LABELS[channel_id])
            if not panel:
                continue
            if channel_id in {'BASE_COLOR', 'EMISSION'}:
                if layer.gradient_supported and layer.gradient_ramp is not None:
                    panel.template_color_ramp(layer, "gradient_ramp", expand=True)
            else:
                # A scalar channel reads its own black-to-white value ramp: as many grey stops as
                # the gradient needs, each edited with the gradient value slider rather than a
                # colour picker. Sync Base Color copies the colour ramp's stop layout, so a user can
                # lay out colours first and then tune each channel's values at the same positions.
                if record.gradient_ramp is not None:
                    # The full template keeps Interpolation and the tools menu (Flip, Distribute,
                    # Eyedropper, Reset) available. Value mode only swaps the active stop's colour
                    # picker for the gradient value slider and leaves Colour Mode out.
                    panel.template_color_ramp(
                        record, "gradient_ramp", expand=True, value=True)
                row = panel.row(align=True)
                op = row.operator(
                    "paint.paint_layer_gradient_sync_channel",
                    text="Sync Base Color",
                    icon='FILE_REFRESH',
                )
                op.marker = layer.marker
                op.channel = channel_id
        if first:
            layout.label(text="No enabled channels", icon='INFO')
        return

    if material_layer_channel_weight(layer) >= _MATERIAL_PAINT_LAYER_HEAVY_CHANNEL_COUNT:
        layout.label(
            text="Six or more channels: the row becomes heavy and bakes in AUTO mode",
            icon='ERROR',
        )

    channel_col = layout.column(align=False)
    first = True
    for channel_id in channel_ids:
        record = records.get(channel_id)
        if record is None or not record.use:
            continue
        # The subpanel exists for the channel's own map: its slot and the settings around it. A
        # row that owns its maps keeps the whole subpanel debug-only, even before a map is minted
        # (AO starts without one). The channel switch above stays either way.
        if _layer_channel_subpanel_hidden(context, layer, record):
            continue
        if not first:
            channel_col.separator()
        first = False
        _draw_channel_panel(
            channel_col,
            context,
            _layer_channel_view(
                layer, record, channel_id,
                _MATERIAL_PAINT_CHANNEL_TOGGLE_LABELS[channel_id]),
            source_enabled=True,
        )
