# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The thin operators behind the paint-layer RNA: add, remove, move, group, mask, channels,
corrections. UI-only code lives in bl_ui; these operate on Material.paint_layers by marker."""

import bpy
from bpy.types import Operator


class MATERIAL_OT_paint_layers_regenerate(Operator):
    bl_idname = "material.paint_layers_regenerate"
    bl_label = "Regenerate Layer Tree"
    bl_description = "Rebuild the material's generated node tree from its layer description"

    @classmethod
    def poll(cls, context):
        mat = context.material
        return mat is not None and mat.is_layered

    def execute(self, context):
        context.material.paint_layers.regenerate()
        return {'FINISHED'}


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


class MATERIAL_OT_paint_layer_add(_PaintLayerOperator):
    bl_idname = "material.paint_layer_add"
    bl_label = "Add Paint Layer"
    bl_description = "Add a row on top of the stack"

    kind: bpy.props.EnumProperty(
        name="Kind",
        items=(
            ('PAINT', "Paint", "A painted layer"),
            ('FILL', "Fill", "A flat fill layer"),
            ('FOLDER', "Folder", "A folder grouping other layers"),
        ),
        default='PAINT',
    )
    name: bpy.props.StringProperty(name="Name")

    def execute(self, context):
        layers = context.material.paint_layers
        layer = layers.new(kind=self.kind, name=self.name)
        if layer is not None:
            layers.active = layer
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_remove(_PaintLayerOperator):
    bl_idname = "material.paint_layer_remove"
    bl_label = "Remove Paint Layer"
    bl_description = "Remove this row and everything nested under it"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        context.material.paint_layers.remove(layer)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_rename(_PaintLayerOperator):
    bl_idname = "material.paint_layer_rename"
    bl_label = "Rename Paint Layer"

    name: bpy.props.StringProperty(name="Name")

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.name = self.name
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_duplicate(_PaintLayerOperator):
    bl_idname = "material.paint_layer_duplicate"
    bl_label = "Duplicate Paint Layer"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layers = context.material.paint_layers
        copy = layers.duplicate(layer)
        if copy is not None:
            layers.active = copy
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_group(_PaintLayerOperator):
    bl_idname = "material.paint_layer_group"
    bl_label = "Group Paint Layers"
    bl_description = "Fold this row and another into a new folder"

    with_marker: bpy.props.StringProperty(
        name="With",
        description="UUID marker of the other row to fold in with",
        options={'SKIP_SAVE'},
    )

    def execute(self, context):
        layer = self._layer(context)
        with_layer = (
            context.material.paint_layers.find(self.with_marker) if self.with_marker else None
        )
        if layer is None or with_layer is None:
            return {'CANCELLED'}
        folder = context.material.paint_layers.group(layer, with_layer)
        if folder is not None:
            context.material.paint_layers.active = folder
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_ungroup(_PaintLayerOperator):
    bl_idname = "material.paint_layer_ungroup"
    bl_label = "Ungroup Paint Layer"
    bl_description = "Lift the rows of this folder into its parent and free the folder"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        context.material.paint_layers.ungroup(layer)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_move(_PaintLayerOperator):
    bl_idname = "material.paint_layer_move"
    bl_label = "Move Paint Layer"

    anchor_marker: bpy.props.StringProperty(
        name="Anchor",
        description="UUID marker of the row to move relative to; the top of the stack when unset",
        options={'SKIP_SAVE'},
    )
    place: bpy.props.EnumProperty(
        name="Place",
        items=(
            ('ABOVE', "Above", "Directly above the anchor"),
            ('BELOW', "Below", "Directly below the anchor"),
            ('INTO', "Into", "Inside the anchor folder"),
        ),
        default='ABOVE',
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        anchor = (
            context.material.paint_layers.find(self.anchor_marker) if self.anchor_marker else None
        )
        context.material.paint_layers.move(layer, anchor, self.place)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_add(_PaintLayerOperator):
    bl_idname = "material.paint_layer_mask_add"
    bl_label = "Add Paint Layer Mask"

    value: bpy.props.FloatProperty(name="Value", min=0.0, max=1.0, default=1.0)

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.mask_add(value=self.value)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_remove(_PaintLayerOperator):
    bl_idname = "material.paint_layer_mask_remove"
    bl_label = "Remove Paint Layer Mask"

    item_marker: bpy.props.StringProperty(
        name="Mask Item",
        description="UUID marker of the mask item to remove; the first one when unset",
        options={'SKIP_SAVE'},
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layers = context.material.paint_layers
        item = layers.find(self.item_marker) if self.item_marker else None
        if item is None:
            item = layer.mask_stack[0] if len(layer.mask_stack) else None
        if item is None:
            return {'CANCELLED'}
        layers.remove(item)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_mask_toggle(_PaintLayerOperator):
    bl_idname = "material.paint_layer_mask_toggle"
    bl_label = "Toggle Paint Layer Mask"

    item_marker: bpy.props.StringProperty(
        name="Mask Item",
        description="UUID marker of the mask item to toggle; the first one when unset",
        options={'SKIP_SAVE'},
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layers = context.material.paint_layers
        item = layers.find(self.item_marker) if self.item_marker else None
        if item is None:
            item = layer.mask_stack[0] if len(layer.mask_stack) else None
        if item is None:
            return {'CANCELLED'}
        item.enabled = not item.enabled
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_channel_add(_PaintLayerOperator):
    bl_idname = "material.paint_layer_channel_add"
    bl_label = "Add Paint Layer Channel"

    channel: bpy.props.EnumProperty(
        name="Channel",
        items=lambda self, context: [
            (item.identifier, item.name, "")
            for item in bpy.types.Material.bl_rna.functions["bake_paint_channels"].parameters[
                "channels"].enum_items
            if item.identifier
        ],
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
        items=lambda self, context: [
            (item.identifier, item.name, "")
            for item in bpy.types.Material.bl_rna.functions["bake_paint_channels"].parameters[
                "channels"].enum_items
            if item.identifier
        ],
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.channel_remove(channel=self.channel)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_correction_add(_PaintLayerOperator):
    bl_idname = "material.paint_layer_correction_add"
    bl_label = "Add Paint Layer Correction"

    section: bpy.props.EnumProperty(
        name="Section",
        items=lambda self, context: [
            (item.identifier, item.name, "")
            for item in bpy.types.MaterialPaintLayer.bl_rna.properties["section"].enum_items
            if item.identifier
        ],
    )
    effect: bpy.props.EnumProperty(
        name="Effect",
        items=lambda self, context: [
            (item.identifier, item.name, "")
            for item in bpy.types.MaterialPaintLayer.bl_rna.properties["effect"].enum_items
            if item.identifier
        ],
    )
    name: bpy.props.StringProperty(name="Name")

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.correction_add(section=self.section, effect=self.effect, name=self.name)
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_rebake(_PaintLayerOperator):
    bl_idname = "material.paint_layer_rebake"
    bl_label = "Rebake Paint Layer"
    bl_description = "Mark the layer's bake stale so the planner re-bakes it"

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        layer.bake_request()
        return {'FINISHED'}


classes = (
    MATERIAL_OT_paint_layers_regenerate,
    MATERIAL_OT_paint_layer_add,
    MATERIAL_OT_paint_layer_remove,
    MATERIAL_OT_paint_layer_rename,
    MATERIAL_OT_paint_layer_duplicate,
    MATERIAL_OT_paint_layer_group,
    MATERIAL_OT_paint_layer_ungroup,
    MATERIAL_OT_paint_layer_move,
    MATERIAL_OT_paint_layer_mask_add,
    MATERIAL_OT_paint_layer_mask_remove,
    MATERIAL_OT_paint_layer_mask_toggle,
    MATERIAL_OT_paint_layer_channel_add,
    MATERIAL_OT_paint_layer_channel_remove,
    MATERIAL_OT_paint_layer_correction_add,
    MATERIAL_OT_paint_layer_rebake,
)
