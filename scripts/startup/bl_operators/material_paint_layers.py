# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The thin operators behind the paint-layer RNA: the channel records. Stack edits (add, remove,
move) live in the RNA and the Outliner; UI-only code lives in bl_ui. These operate on
Material.paint_layers by marker."""

import bpy
from bpy.types import Operator


def _paint_layers_active_layer(context):
    mat = context.material
    if mat is None or not mat.is_layered:
        return None
    return mat.paint_layers.active


class _PaintLayerOperator(Operator):
    """Base for the thin layer operators: undo, a report and the RNA notifier, and the row they
    act on taken from the operator or the material's active layer (a marker lookup).

    The row is addressed by its UUID marker as a string, not a PointerProperty: #MaterialPaintLayer
    is a plain (non-ID) RNA struct, and Python property types only accept IDs or property groups."""

    bl_options = {'UNDO', 'REGISTER'}

    marker: bpy.props.StringProperty(
        name="Layer",
        description="UUID marker of the row to act on; the active layer when unset",
        options={'SKIP_SAVE'},
    )

    def _layer(self, context):
        if self.marker:
            layer = context.material.paint_layers.find(self.marker)
            if layer is not None:
                return layer
        return _paint_layers_active_layer(context)

    @classmethod
    def poll(cls, context):
        mat = context.material
        return mat is not None and mat.is_layered


# Dynamic enum items must stay referenced from Python: Blender keeps raw pointers to the strings of
# the last returned list, so a list rebuilt on every call leaves the UI reading freed memory.
_ENUM_ITEMS_CACHE = {}


def _rna_enum_items_cached(struct, prop, keep=None):
    """The (identifier, name, "") items of an RNA enum, built once and kept alive."""
    key = (struct, prop, tuple(keep) if keep is not None else None)
    items = _ENUM_ITEMS_CACHE.get(key)
    if items is None:
        items = [
            (item.identifier, item.name, "")
            for item in getattr(bpy.types, struct).bl_rna.properties[prop].enum_items
            if item.identifier and (keep is None or item.identifier in keep)
        ]
        _ENUM_ITEMS_CACHE[key] = items
    return items


def _paint_channel_enum_items():
    """Every paint channel as an enum item tuple, the shared channel vocabulary.

    The identifiers match #Material.paint_layers_channels and a layer record's channel, so the same
    strings address the global set and a layer record.
    """
    return _rna_enum_items_cached("MaterialPaintLayerChannel", "channel")


def _paint_layer_addable_channel_items(context):
    """The channels the Add Channel menu offers for the active layer.

    Only channels of the material's global set that the layer carries no record for: a channel
    outside the set can never be added through the UI, and one already on the row has nothing to
    add. Falls back to the full list when no layered material/layer is in context, so the menu is
    never empty-by-accident in a scripted call.
    """
    mat = context.material
    layer = mat.paint_layers.active if (mat is not None and mat.is_layered) else None
    if layer is None:
        return _paint_channel_enum_items()
    existing = {record.channel for record in layer.channels}
    return [
        item for item in _paint_channel_enum_items()
        if item[0] not in existing and mat.paint_layers_channel_in_set(channel=item[0])
    ]


class MATERIAL_OT_paint_layer_channel_add(_PaintLayerOperator):
    bl_idname = "material.paint_layer_channel_add"
    bl_label = "Add Paint Layer Channel"

    channel: bpy.props.EnumProperty(
        name="Channel",
        items=lambda self, context: _paint_layer_addable_channel_items(context),
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.channel_add(channel=self.channel)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_channel_remove(_PaintLayerOperator):
    bl_idname = "material.paint_layer_channel_remove"
    bl_label = "Remove Paint Layer Channel"

    channel: bpy.props.EnumProperty(
        name="Channel",
        items=lambda self, context: _paint_channel_enum_items(),
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        try:
            layer.channel_remove(channel=self.channel)
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_channel_toggle(_PaintLayerOperator):
    """Switch one channel of a layer on or off, creating the record lazily on enable.

    An absent record is how a channel the layer never opted into reads as off; enabling has to both
    create the record and start it ENABLED (`channel_add` already does). Disabling only flips an
    existing record's state, so its map stays on the row (BKE_paint_layers_channel_set_enabled).
    """

    bl_idname = "material.paint_layer_channel_toggle"
    bl_label = "Toggle Paint Layer Channel"

    channel: bpy.props.EnumProperty(
        name="Channel",
        items=lambda self, context: _paint_channel_enum_items(),
    )
    enable: bpy.props.BoolProperty(name="Enable", default=True)

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        record = next((item for item in layer.channels if item.channel == self.channel), None)
        try:
            if self.enable:
                if record is None:
                    layer.channel_add(channel=self.channel)
                else:
                    layer.channel_set_enabled(channel=self.channel, enabled=True)
            elif record is not None:
                layer.channel_set_enabled(channel=self.channel, enabled=False)
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


def _leave_mask_target(context, layer):
    """Back to painting the content when the mask being painted is about to go away or off.

    Only the active layer's mask is the one a stroke is aimed at, so another layer's mask going away
    leaves the mode alone, as the Outliner's seam does."""
    material = context.material
    if material is None or material.paint_layers.active is None:
        return
    if material.paint_layers.active.marker != layer.marker:
        return
    if context.scene.tool_settings.paint_mode.layer_target_mode != 'MASK':
        return
    try:
        bpy.ops.paint.layer_target_mode_set(mode='CONTENT')
    except RuntimeError:
        # No active object to switch for (the operator's poll): nothing is being painted either.
        pass


class MATERIAL_OT_paint_layer_mask_add(_PaintLayerOperator):
    """Give a layer a mask: a coverage factor laid over it. A mask of one shows the layer
    everywhere; a stroke in Mask mode paints it."""

    bl_idname = "material.paint_layer_mask_add"
    bl_label = "Add Paint Layer Mask"

    value: bpy.props.FloatProperty(
        name="Value",
        description="Constant coverage of the new mask: one shows the layer, zero hides it",
        default=1.0, min=0.0, max=1.0,
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        try:
            layer.mask_add(value=self.value)
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_remove(_PaintLayerOperator):
    """Remove a layer's mask, together with its map."""

    bl_idname = "material.paint_layer_mask_remove"
    bl_label = "Remove Paint Layer Mask"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        # A stroke into a mask that is gone would go nowhere, so the brush goes back to the content.
        _leave_mask_target(context, layer)
        try:
            layer.mask_remove()
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_toggle(_PaintLayerOperator):
    """Switch a layer's mask on or off, keeping the mask and its paint content."""

    bl_idname = "material.paint_layer_mask_toggle"
    bl_label = "Toggle Paint Layer Mask"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None or not len(layer.mask_items):
            return {'CANCELLED'}
        item = layer.mask_items[0]
        if item.enabled:
            # A switched-off mask hides nothing: strokes into it would be invisible.
            _leave_mask_target(context, layer)
        item.enabled = not item.enabled
        return {'FINISHED'}


classes = (
    MATERIAL_OT_paint_layer_channel_add,
    MATERIAL_OT_paint_layer_channel_remove,
    MATERIAL_OT_paint_layer_channel_toggle,
    MATERIAL_OT_paint_layer_mask_add,
    MATERIAL_OT_paint_layer_mask_remove,
    MATERIAL_OT_paint_layer_mask_toggle,
)
