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


def draw_material_layer_channels(layout, _context, material, layer):
    """Draw a stack layer's channels: toggles driven by the material's set, then each enabled
    channel's own value and map.

    Every channel of the material's set is offered as a toggle, and one the layer carries no record
    for (or carries DISABLED) reads as off; enabling it lazily creates the record through
    `material.paint_layer_channel_toggle`, and disabling keeps its record and map. A channel outside
    the set is never shown, even when the row still holds its record. An enabled channel then shows
    its own constant value and, when the user assigns one, a source image -- a Paint row can be
    given a constant or a map without a stroke.
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
        enabled = record is not None and record.state == 'ENABLED'
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
        if record is None or record.state != 'ENABLED':
            continue
        if not first:
            channel_col.separator()
        first = False
        row = channel_col.row(align=True)
        row.label(text=_MATERIAL_PAINT_CHANNEL_TOGGLE_LABELS[channel_id])
        row.prop(record, "value", text="")
        channel_col.template_ID(record, "image", open="image.open", text="Map")
