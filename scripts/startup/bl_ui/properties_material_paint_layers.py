# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Layer Material tab's own draw code: the material's global paint channel set and the
channels of one stack layer.

Split out of `properties_material.py` so the panels stay thin and the channel vocabulary lives in
one place. The panels that call these functions are drawn from `properties_material.py` (they need
the Layer Material tab context), so this module only supplies functions.
"""

from bl_ui.properties_paint_common import (
    _MATERIAL_PAINT_CHANNEL_TOGGLE_LABELS,
    _MATERIAL_PAINT_CHANNEL_TOGGLE_UI_UNITS_X,
    _MATERIAL_PAINT_CHANNEL_UI_ORDER,
    MaterialPaintChannelView,
    _draw_channel_panel,
)

# Channel count at which a row is considered heavy: five records weigh 34 generated nodes, six
# weigh 40, over the AUTO-bake threshold of 36. A row this wide bakes instead of evaluating live.
_MATERIAL_PAINT_LAYER_HEAVY_CHANNEL_COUNT = 6


def material_layer_visible_channels(material):
    """The channels of the material's global set the layer widget can draw, in display order.

    Height and Custom have no channel widget and are absent from the display order, so a set that
    names them still offers them nowhere in a layer; the records themselves are never dropped.
    """
    if material is None:
        return []
    return [
        channel_id for channel_id in _MATERIAL_PAINT_CHANNEL_UI_ORDER
        if material.paint_layers_channel_in_set(channel=channel_id)
    ]


def material_layer_weight_record_count(material, layer):
    """How many of the layer's channel records take part in the material's set.

    Every record whose channel is in the set costs generated nodes, DISABLED or not; above
    #_MATERIAL_PAINT_LAYER_HEAVY_CHANNEL_COUNT the row is heavy enough to bake in AUTO mode.
    """
    return sum(
        1 for record in layer.channels
        if material.paint_layers_channel_in_set(channel=record.channel))


def draw_material_channel_set(layout, material):
    """Checkboxes for the material's global paint channel set, one per supported channel.

    Base Color is always in the set (BKE forces it back on) and is the material's constant, so its
    row is shown checked but disabled rather than offering a toggle that can never stick.
    """
    col = layout.column()
    for channel_id in _MATERIAL_PAINT_CHANNEL_UI_ORDER:
        row = col.row(align=True)
        if channel_id == 'BASE_COLOR':
            row.enabled = False
        row.prop_enum(material, "paint_layers_channels", channel_id)


def _layer_images_locked(layer):
    """Whether a row's channel maps belong to the row and must not be replaced or unlinked.

    A Paint (IMAGE) layer creates and owns its maps, which the brush writes into; a Fill (CONSTANT)
    is never painted, so the user may assign their own maps there.
    """
    return layer.source == 'IMAGE'


def _draw_layer_channel_source_preview(layout, record, locked=False):
    """Compact preview of a layer channel's own image, in the panel header."""
    if record.image is not None:
        row = layout.row(align=True)
        row.enabled = not locked
        row.template_ID_browser(
            record, "image", compact=True, image_filter='PAINT_SOURCE',
        )


def _draw_layer_channel_source(panel, record, locked=False):
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
    value_color = None
    if channel_id in ('BASE_COLOR', 'NORMAL', 'EMISSION'):
        color = (record, "color")
    else:
        scalar = (record, "value_scalar")
        value_color = (record, "value_color")
    return MaterialPaintChannelView(
        record,
        channel_id,
        name,
        use=(record, "use"),
        color=color,
        scalar=scalar,
        value_color=value_color,
        image=(record, "image"),
        socket_menu="PAINT_MT_material_layer_channel_socket",
        socket_context_set=(("material_paint_layer_channel", record),
                            ("material_paint_layer_owner", layer)),
        source_draw=lambda panel, ctx, enabled: _draw_layer_channel_source(
            panel, record, locked=_layer_images_locked(layer)),
        source_preview=lambda layout: _draw_layer_channel_source_preview(
            layout, record, locked=_layer_images_locked(layer)),
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


def draw_material_mask_item(layout, _context, item):
    """The controls of one mask item, the base mask or a mask correction: its own settings and one
    coverage channel drawn like a scalar channel of a layer (panel header with the value, body with
    the map slot and the gradient).

    The base mask and a Fill correction lay a flat grey while they have no map and take a map the
    user assigns; a Paint correction owns its map (the first stroke creates it), so it is shown, not
    swapped. Invert reads the grey as `1 - value`, the map's or the flat one.
    """
    record = item.channels[0] if len(item.channels) else None
    base = item.is_mask_base
    assignable = base or item.source == 'CONSTANT'

    col = layout.column()
    if not base:
        # The base is the Multiply item of the row's own mask: it has no blend to choose.
        col.prop(item, "blend_type", text="Blend")
    col.prop(item, "opacity", text="Opacity")
    if record is None:
        return
    has_map = record.image is not None

    header, panel = layout.panel("material_paint_mask_item", default_closed=False)
    header.use_property_split = True
    header.use_property_decorate = False
    row = header.row(align=True)
    split = row.split(factor=0.4, align=True)
    split.label(text="Mask")
    value_row = split.row(align=True)
    if has_map:
        sub = value_row.row(align=True)
        sub.enabled = assignable
        sub.template_ID_browser(record, "image", compact=True, image_filter='PAINT_SOURCE')
    elif assignable:
        controls = value_row.row(align=True)
        controls.prop(item, "mask_color", text="")
        controls.prop(item, "mask_value", text="", slider=True)
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
        row = panel.row(align=True)
        sub = row.row(align=True)
        sub.enabled = False
        sub.template_ID_browser(record, "image", compact=True, image_filter='PAINT_SOURCE')
        row.prop(record, "invert", text="", icon='ARROW_LEFTRIGHT', toggle=True)
    else:
        panel.label(text="The first stroke creates the map", icon='INFO')

    if not has_map and assignable:
        panel.separator()
        row = panel.row(align=True)
        row.template_material_paint_value_slider(item, "mask_value", index=0)
        row.prop(record, "invert", text="", icon='ARROW_LEFTRIGHT', toggle=True)
    panel.separator()


def draw_material_layer_channels(layout, context, material, layer):
    """Draw a stack layer's channels with the shared PBR Paint channel widget.

    The same fixed channel order and toggle buttons as the brush panel, but each channel's value
    and source come from the layer's own #MaterialPaintLayerChannel records. The channel set drives
    the rows: every channel of the material's set is offered, and one the layer carries no record
    for (or carries DISABLED) reads as off. Enabling it lazily creates the record; a channel outside
    the set is never shown, even when the row still holds its record and map.
    """
    channel_ids = material_layer_visible_channels(material)
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
        col.ui_units_x = _MATERIAL_PAINT_CHANNEL_TOGGLE_UI_UNITS_X
        op = col.operator(
            "material.paint_layer_channel_toggle",
            text=_MATERIAL_PAINT_CHANNEL_TOGGLE_LABELS[channel_id],
            depress=enabled,
        )
        op.marker = layer.marker
        op.channel = channel_id
        op.enable = not enabled

    if material_layer_weight_record_count(
            material, layer) >= _MATERIAL_PAINT_LAYER_HEAVY_CHANNEL_COUNT:
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
