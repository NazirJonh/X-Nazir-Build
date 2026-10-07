# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Layer Material tab's own draw code: the active layer's channels, masks and corrections.

Everything here is layer-side drawing, split out of the shared brush-settings module
(`properties_paint_common.py`) so that module keeps only what the PBR Paint brush panel and the
other paint modes need. The shared channel widget (`_draw_channel_panel`,
`MaterialPaintChannelView`) still lives there; this module is its layer-side client.
"""

import bpy

# Keep `bpy` used: the panels that draw this live in properties_material.py; this module only
# supplies functions.
_ = bpy

from bl_ui.properties_paint_common import (
    MaterialPaintChannelView,
    PAINT_MT_material_layer_channel_socket,
    _draw_channel_panel,
)


def _layer_channel_view(layer, channel, channel_id, name):
    """A #MaterialPaintLayerChannel under the shared widget's contract.

    The layer has no brush-side source slot or grid: its source is the channel record's own image,
    assigned through RNA. Its value lives in the record (``value`` for colors, ``value_scalar`` for
    scalars) and is edited through the layer's BKE setters. Its socket menu is the layer's own
    (#PAINT_MT_material_layer_channel_socket), which needs both the record and the owning layer.
    """
    color = None
    scalar = None
    value_color = None
    if channel_id in ('BASE_COLOR', 'NORMAL', 'EMISSION'):
        color = (channel, "value")
    else:
        scalar = (channel, "value_scalar")
        value_color = (channel, "value_color")
    return MaterialPaintChannelView(
        channel,
        channel_id,
        name,
        use=(channel, "use"),
        color=color,
        scalar=scalar,
        value_color=value_color,
        image=(channel, "image"),
        socket_menu="PAINT_MT_material_layer_channel_socket",
        socket_context_set=(("material_paint_layer_channel", channel),
                            ("material_paint_layer_owner", layer)),
        source_draw=lambda panel, ctx, enabled: _draw_layer_channel_source(
            panel, channel, locked=_layer_images_locked(layer)),
        source_preview=lambda layout: _draw_layer_channel_source_preview(
            layout, channel, locked=_layer_images_locked(layer)),
        source_has=lambda: channel.image is not None,
        source_active=lambda enabled: channel.image is not None,
        invert_channel=None,
    )


def _layer_images_locked(layer):
    """Whether a row's channel maps belong to the row and must not be replaced or unlinked.

    A Paint (IMAGE) layer or correction creates and owns its maps, which the brush writes into; a
    Fill (CONSTANT) correction is never painted, so the user may assign their own maps there.
    """
    return layer.source == 'IMAGE'


def _draw_layer_channel_source_preview(layout, channel, locked=False):
    """Compact preview of a layer channel's own image, in the panel header."""
    if channel.image is not None:
        row = layout.row(align=True)
        row.enabled = not locked
        row.template_ID_browser(
            channel, "image", compact=True, image_filter='PAINT_SOURCE',
        )


def _draw_layer_channel_source(panel, channel, locked=False):
    """The layer channel's source row: a "Drop image" picker writing the record's own image.

    \a locked draws it read-only, so the map owned by a Paint row can be seen but not swapped.
    """
    row = panel.row(align=True)
    row.enabled = not locked
    row.template_ID_browser(
        channel,
        "image",
        open="image.open",
        text="Drop image: {:s}".format(channel.channel),
        image_filter='PAINT_SOURCE',
        use_unlink=True,
        use_users=False,
    )


_MATERIAL_PAINT_CHANNEL_LABELS = {
    'BASE_COLOR': "Color",
    'METALLIC': "Metal",
    'ROUGHNESS': "Rough",
    'SPECULAR': "Spec",
    'NORMAL': "Normal",
    'HEIGHT': "Height",
    'ALPHA': "Alpha",
    'AO': "AO",
    'EMISSION': "Emit",
    'CUSTOM': "Custom",
}


def material_layer_visible_channels(material):
    """The channels of \a material's global set the layer widget can draw, in display order.

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
    """How many of \a layer's channel records take part in \a material's set.

    This is the channel count #paint_layer_subtree_weight (paint_layers_bake.cc) weighs: every
    record whose channel is in the set costs nodes, DISABLED or not. Five records weigh 34 nodes,
    six weigh 40, over the AUTO-bake threshold of 36.
    """
    return sum(
        1 for record in layer.channels
        if material.paint_layers_channel_in_set(channel=record.channel))


def draw_material_channel_set(layout, material):
    """Checkboxes for \a material's global paint channel set, one per supported channel.

    Base Color is always in the set (BKE forces it back on) and is the material's constant, so its
    row is shown checked but disabled rather than offering a toggle that can never stick.
    """
    col = layout.column()
    for channel_id in _MATERIAL_PAINT_CHANNEL_UI_ORDER:
        row = col.row(align=True)
        if channel_id == 'BASE_COLOR':
            row.enabled = False
        row.prop_enum(material, "paint_layers_channels", channel_id)


def draw_material_layer_channels(layout, context, material, layer):
    """Draw a stack layer's channels with the shared PBR Paint channel widget.

    The same fixed channel order and toggle buttons as the brush panel, but each channel's value
    and source come from the layer's own #MaterialPaintLayerChannel records. The channel set drives
    the rows: every channel of the material's set is offered, and one the layer carries no record
    for (or carries DISABLED) reads as off. Enabling it lazily creates the record; a channel outside
    the set is never shown, even when the row still holds its record and map.
    """
    # A Stack row is a folder: it groups children and carries no channel records of its own, so it
    # shows only the per-channel blend/opacity overrides it applies to them.
    if layer.source == 'STACK':
        _draw_material_folder_channels(layout, material, layer)
        return
    # A Material or Node Group row is baked: its channels are maps, not records to toggle, so it
    # gets a read-only status list instead of the set-driven widget. A Node Group also offers its
    # custom channels.
    if layer.source == 'MATERIAL':
        _draw_material_baked_channel_status(layout, material, layer)
        return
    if layer.source == 'NODE_GROUP':
        _draw_material_baked_channel_status(layout, material, layer)
        _draw_material_custom_channels(layout, layer)
        return
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
            text=_MATERIAL_PAINT_CHANNEL_LABELS[channel_id],
            depress=enabled,
        )
        op.marker = layer.marker
        op.channel = channel_id
        op.enable = not enabled

    if material_layer_weight_record_count(
            material, layer) > _MATERIAL_PAINT_LAYER_HEAVY_CHANNEL_COUNT:
        layout.label(
            text="Six or more channels: the row becomes heavy and bakes in AUTO mode",
            icon='ERROR',
        )

    # A Mesh Map row reads one geometry map from the material's shared atlas: the type selector is
    # part of the row, and (unlike a Paint row) there is no per-channel image to drop, only the
    # read-only atlas preview below.
    if layer.source == 'MESH_MAP':
        row = layout.row(align=True)
        row.prop(layer, "mesh_map_type", text="Map")

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
                _MATERIAL_PAINT_CHANNEL_LABELS[channel_id]),
            source_enabled=True,
        )


def _draw_material_folder_channels(layout, material, layer):
    """A folder row's per-channel blend/opacity overrides, one row per channel of the set.

    A folder owns no records (BKE refuses channel_add for it), so these shared settings are the
    only per-channel controls it can offer.
    """
    channel_ids = material_layer_visible_channels(material)
    if not channel_ids:
        layout.label(text="No channels", icon='INFO')
        return
    settings = {item.channel: item for item in layer.channel_settings}
    col = layout.column(align=True)
    for channel_id in channel_ids:
        item = settings.get(channel_id)
        if item is None:
            continue
        row = col.row(align=True)
        row.label(text=_MATERIAL_PAINT_CHANNEL_LABELS[channel_id])
        row.prop(item, "blend_type", text="")
        row.prop(item, "opacity", text="")


def _draw_material_baked_channel_status(layout, material, layer):
    """A Material / Node Group row's channels as a read-only baked/not list.

    Such a row has no records to toggle and no map to drop: its channels come from a bake.
    ``baked_channels`` is the exact per-channel answer (every channel the current bake holds a map
    for, coverage reported as Alpha), so the list names precisely what is baked rather than deriving
    it from the overall validity plus a record's map.
    """
    channel_ids = material_layer_visible_channels(material)
    if not channel_ids:
        layout.label(text="No channels", icon='INFO')
        return
    baked = {item.channel for item in layer.baked_channels}
    col = layout.column(align=True)
    for channel_id in channel_ids:
        row = col.row(align=True)
        row.label(text=_MATERIAL_PAINT_CHANNEL_LABELS[channel_id])
        if channel_id in baked:
            row.label(text="Baked", icon='IMAGE_DATA')
        else:
            row.label(text="No map", icon='INFO')


def _draw_material_custom_channels(layout, layer):
    """A Node Group row's custom channels and the Add button the old Channels box carried."""
    for custom in layer.custom_channels:
        layout.label(text=_MATERIAL_PAINT_CHANNEL_LABELS.get(custom.channel, custom.channel),
                     icon='RNDCURVE')
    layout.operator_menu_enum("material.paint_layer_custom_channel_add", "channel",
                              text="Add Custom Channel", icon='ADD')


def _correction_base_color_record(item):
    for record in item.channels:
        if record.channel == 'BASE_COLOR':
            return record
    return None


def _draw_material_constant_value(layout, item, channel_id):
    """One value control for a constant correction: a gradient for colours, a scalar slider else."""
    row = layout.row(align=True)
    if channel_id in ('BASE_COLOR', 'NORMAL', 'EMISSION'):
        row.prop(item, "fill_color", text="")
    else:
        row.template_material_paint_value_slider(item, "fill_color", index=0)


def draw_material_mask_item(layout, item):
    """One mask item's control: a Fill shows a single value, a Paint only its channel and map.

    A constant mask reads its strength from ``fill_color``; an image mask reads a channel record's
    map, and shows no constant. A MESH_MAP mask reads the material's shared atlas (read-only), the
    same map a MESH_MAP row reads, so it shows a preview instead of a Drop image picker.
    """
    if item.source == 'CONSTANT':
        # The map replaces the constant, so the value is hidden while one is assigned.
        if item.mask_image is None:
            _draw_material_constant_value(layout, item, item.mask_channel)
        row = layout.row(align=True)
        row.template_ID_browser(
            item,
            "mask_image",
            open="image.open",
            text="Drop image: Mask",
            image_filter='PAINT_SOURCE',
            use_unlink=True,
            use_users=False,
        )
    elif item.source == 'IMAGE':
        row = layout.row(align=True)
        row.prop(item, "mask_channel", text="")
        record = _correction_base_color_record(item)
        if record is None or record.image is None:
            row.label(text="No map", icon='INFO')
        else:
            _draw_layer_channel_source(row, record, locked=True)
    elif item.source == 'MESH_MAP':
        row = layout.row(align=True)
        row.prop(item, "mesh_map_type", text="Map")
        _draw_mesh_map_atlas_preview(row, item)
    else:
        # A Material / Node Group / Stack mask carries more than one scalar, so it keeps the channel
        # picker the old Mask box offered.
        row = layout.row()
        row.use_property_split = True
        row.prop(item, "mask_channel")


def _draw_mesh_map_atlas_preview(layout, item):
    """A read-only preview of the shared atlas a MESH_MAP mask reads, resolved through owner+type.

    A MESH_MAP mask never assigns a map of its own (it reads the material's per-type atlas), so it
    shows the atlas image compactly rather than a Drop image picker that would be a no-op.
    """
    # A #MaterialPaintLayer is a non-ID RNA struct owned by its Material, which `id_data` resolves.
    owner = getattr(item, "id_data", None)
    if owner is None or not hasattr(owner, "mesh_map_slots"):
        layout.label(text="No atlas", icon='INFO')
        return
    slot = next(
        (candidate for candidate in owner.mesh_map_slots
         if candidate.type == item.mesh_map_type), None)
    if slot is None or slot.atlas_image is None:
        layout.label(text="No atlas", icon='INFO')
        return
    layout.template_ID(slot, "atlas_image", text="", compact=True)


def draw_material_correction_channels(layout, context, material, item):
    """An Effect correction's channel controls, the same widget the layer rows use.

    A Fill effect's toggles create a record lazily and its value sliders write ``record.value``; a
    Paint effect shows the record's own map picker and no constant. A Material or Node Group effect
    is a baked/live source, so it shows the per-channel bake status (``baked_channels``) rather than
    record toggles; a Stack effect owns no records, so it shows per-channel blend/opacity overrides
    like a folder. Material and Node Group also keep the channel the widget reads.
    """
    if item.source in {'IMAGE', 'CONSTANT', 'MESH_MAP'}:
        draw_material_layer_channels(layout, context, material, item)
        return
    if item.source == 'STACK':
        _draw_material_folder_channels(layout, material, item)
        return
    # MATERIAL / NODE_GROUP: the content is a bake, not records to toggle.
    _draw_material_baked_channel_status(layout, material, item)
    row = layout.row()
    row.prop(item, "mask_channel", text="Channel")
