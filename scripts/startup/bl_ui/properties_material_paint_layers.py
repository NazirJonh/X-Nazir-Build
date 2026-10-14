# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Layer Material tab's own draw code: the material's global paint channel set and the
channels of one stack layer.

Split out of `properties_material.py` so the panels stay thin and the channel vocabulary lives in
one place. The panels that call these functions are drawn from `properties_material.py` (they need
the Layer Material tab context), so this module only supplies functions.
"""

from bl_ui.properties_paint_common import _MATERIAL_PAINT_CHANNEL_UI_ORDER

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
