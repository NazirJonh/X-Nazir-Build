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


def _paint_layers_uv_autofill(context):
    """Name the stack's UV layer after the object's active UV map when it is still empty."""
    mat = context.material
    obj = context.object
    if mat is not None and obj is not None:
        mat.paint_layers_uv_map_autofill(object=obj)


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

    source: bpy.props.EnumProperty(
        name="Source",
        items=(
            ('IMAGE', "Paint", "A painted layer"),
            ('CONSTANT', "Fill", "A flat fill layer"),
            ('STACK', "Folder", "A folder grouping other layers"),
        ),
        default='IMAGE',
    )
    name: bpy.props.StringProperty(name="Name")

    def execute(self, context):
        layers = context.material.paint_layers
        _paint_layers_uv_autofill(context)
        layer = layers.new(source=self.source, name=self.name)
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

    role: bpy.props.EnumProperty(
        name="Role",
        items=lambda self, context: [
            (item.identifier, item.name, "")
            for item in bpy.types.MaterialPaintLayer.bl_rna.properties["role"].enum_items
            # A correction is Effect or Mask Item; Layer is a stack row, not something this
            # operator ever creates.
            if item.identifier in ('EFFECT', 'MASK_ITEM')
        ],
    )
    source: bpy.props.EnumProperty(
        name="Source",
        items=lambda self, context: [
            (item.identifier, item.name, "")
            for item in bpy.types.MaterialPaintLayer.bl_rna.properties["source"].enum_items
            # A correction only ever paints (Image) or fills (Constant).
            if item.identifier in ('IMAGE', 'CONSTANT')
        ],
    )
    name: bpy.props.StringProperty(name="Name")

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        _paint_layers_uv_autofill(context)
        layer.correction_add(role=self.role, source=self.source, name=self.name)
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


def _mesh_map_owner(context):
    # The stack owner is the active slot's material, not `context.material`: in the Layer Material
    # tab that can be the active Material row's source. The bake operator uses the active slot too.
    ob = context.object
    mat = ob.active_material if ob is not None else None
    if mat is None or not mat.is_layered or mat.grease_pencil:
        return None
    return mat


class OBJECT_OT_mesh_map_refresh(Operator):
    bl_idname = "object.mesh_map_refresh"
    bl_label = "Refresh Mesh Map Status"
    bl_description = "Re-check mesh map bake statuses for the active object"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return ob is not None and ob.type == 'MESH'

    def execute(self, context):
        depsgraph = context.evaluated_depsgraph_get()
        changed = context.object.mesh_map_states.refresh_all(depsgraph=depsgraph)
        self.report({'INFO'}, "Changed {} mesh map status(es)".format(changed))
        return {'FINISHED'}


class MATERIAL_OT_mesh_map_add_layer(Operator):
    bl_idname = "material.mesh_map_add_layer"
    bl_label = "Add Mesh Map Layer"
    bl_description = "Add a Mesh Map layer to the material"
    bl_options = {'REGISTER', 'UNDO'}

    type: bpy.props.EnumProperty(
        name="Map Type",
        items=(
            ('AO', "Ambient Occlusion", "Ambient occlusion map"),
            ('CURVATURE', "Curvature", "Curvature map"),
            ('NORMAL_WORLD', "Normal (World)", "World-space normal map"),
            ('NORMAL_OBJECT', "Normal (Object)", "Object-space normal map"),
            ('ID_OBJECT', "Object ID", "Object index map"),
            ('ID_MATERIAL', "Material ID", "Material index map"),
            ('EDGE', "Edge", "Edge/bevel map"),
        ),
    )

    @classmethod
    def poll(cls, context):
        return _mesh_map_owner(context) is not None

    def execute(self, context):
        names = {
            'AO': "AO Map",
            'CURVATURE': "Curvature Map",
            'NORMAL_WORLD': "Normal (World) Map",
            'NORMAL_OBJECT': "Normal (Object) Map",
            'ID_OBJECT': "Object ID Map",
            'ID_MATERIAL': "Material ID Map",
            'EDGE': "Edge Map",
        }
        mat = _mesh_map_owner(context)
        layer = mat.paint_layers.new(source='MESH_MAP', name=names[self.type])
        if layer is None:
            self.report({'ERROR'}, "Cannot add Mesh Map layer")
            return {'CANCELLED'}
        layer.mesh_map_type = self.type
        mat.paint_layers.active = layer
        return {'FINISHED'}


class MATERIAL_OT_mesh_map_add_mask(Operator):
    bl_idname = "material.mesh_map_add_mask"
    bl_label = "Add Mesh Map Mask"
    bl_description = "Add a Mesh Map mask to the active paint layer"
    bl_options = {'REGISTER', 'UNDO'}

    type: bpy.props.EnumProperty(
        name="Map Type",
        items=(
            ('AO', "Ambient Occlusion", "Ambient occlusion map"),
            ('CURVATURE', "Curvature", "Curvature map"),
            ('NORMAL_WORLD', "Normal (World)", "World-space normal map"),
            ('NORMAL_OBJECT', "Normal (Object)", "Object-space normal map"),
            ('ID_OBJECT', "Object ID", "Object index map"),
            ('ID_MATERIAL', "Material ID", "Material index map"),
            ('EDGE', "Edge", "Edge/bevel map"),
        ),
    )

    @classmethod
    def poll(cls, context):
        return _mesh_map_owner(context) is not None

    def execute(self, context):
        mat = _mesh_map_owner(context)
        layer = mat.paint_layers.active
        if layer is None:
            self.report({'ERROR'}, "No active paint layer for a Mesh Map mask")
            return {'CANCELLED'}
        names = {
            'AO': "AO Mask",
            'CURVATURE': "Curvature Mask",
            'NORMAL_WORLD': "Normal (World) Mask",
            'NORMAL_OBJECT': "Normal (Object) Mask",
            'ID_OBJECT': "Object ID Mask",
            'ID_MATERIAL': "Material ID Mask",
            'EDGE': "Edge Mask",
        }
        mask = layer.correction_add(role='MASK_ITEM', source='MESH_MAP', name=names[self.type])
        if mask is None:
            self.report({'ERROR'}, "Cannot add Mesh Map mask to the active paint layer")
            return {'CANCELLED'}
        mask.mesh_map_type = self.type
        return {'FINISHED'}


class MATERIAL_OT_mesh_map_use_active_uv(Operator):
    bl_idname = "material.mesh_map_use_active_uv"
    bl_label = "Use Active UV"
    bl_description = "Use the object's active UV layer for the material paint stack"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        ob = context.object
        return (
            _mesh_map_owner(context) is not None and
            ob.type == 'MESH' and ob.data is not None
        )

    def execute(self, context):
        mat = _mesh_map_owner(context)
        ob = context.object
        if mat.paint_layers_uv_map:
            return {'FINISHED'}
        if ob.data.uv_layers.active is None:
            self.report({'ERROR'}, "The object has no active UV layer")
            return {'CANCELLED'}
        mat.paint_layers_uv_map_autofill(object=ob)
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
    OBJECT_OT_mesh_map_refresh,
    MATERIAL_OT_mesh_map_add_layer,
    MATERIAL_OT_mesh_map_add_mask,
    MATERIAL_OT_mesh_map_use_active_uv,
)
