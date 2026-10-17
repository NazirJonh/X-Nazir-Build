# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The thin operators behind the paint-layer RNA: the channel records. Stack edits (add, remove,
move) live in the RNA and the Outliner; UI-only code lives in bl_ui. These operate on
Material.paint_layers by marker."""

import bpy
from bpy.types import Operator


def _context_material(context):
    """The material the operators act on.

    `context.material` only exists in the Properties editor's material contexts; from the
    Outliner or a viewport the owner is the active object's active material slot."""
    material = getattr(context, "material", None)
    if material is None:
        ob = getattr(context, "object", None)
        material = ob.active_material if ob is not None else None
    return material


def _paint_layers_active_layer(context):
    mat = _context_material(context)
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
            mat = _context_material(context)
            # A marker that names no row must not fall back to the active layer: the caller asked
            # for a specific row, and acting on another one would edit the wrong layer.
            return mat.paint_layers.find(self.marker) if mat is not None else None
        return _paint_layers_active_layer(context)

    @classmethod
    def poll(cls, context):
        mat = _context_material(context)
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
    mat = _context_material(context)
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
    material = _context_material(context)
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


class MATERIAL_OT_paint_layer_mask_filter_add(_PaintLayerOperator):
    """Add a Color ID or UV Island filter over the layer's mask: it keeps the coverage only where
    an ID Map matches the listed colours, or on the captured UV islands."""

    bl_idname = "material.paint_layer_mask_filter_add"
    bl_label = "Add Mask Filter"

    filter_type: bpy.props.EnumProperty(
        name="Filter",
        items=(
            ('COLOR_ID', "Color ID", "Keep the coverage where an ID Map matches the listed colors"),
            ('UV_ISLAND', "UV Island", "Keep the coverage on the UV islands captured from the Image Editor"),
        ),
        default='COLOR_ID',
    )

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        try:
            # A filter works on the coverage below it, so the layer gets its white mask first, like
            # the other mask corrections.
            if not len(layer.mask_items):
                layer.mask_add(value=1.0)
            layer.mask_filter_add(filter_type=self.filter_type)
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_id_color_remove(Operator):
    """Remove a color from the active Color ID mask item."""

    bl_idname = "material.paint_layer_id_color_remove"
    bl_label = "Remove Color"
    bl_options = {'UNDO', 'REGISTER'}

    index: bpy.props.IntProperty(
        name="Index",
        description="Position of the color in the item's list",
        min=0,
        options={'SKIP_SAVE'},
    )

    @classmethod
    def poll(cls, context):
        mat = _context_material(context)
        return mat is not None and mat.is_layered and mat.paint_layers.active_mask_item is not None

    def execute(self, context):
        item = _context_material(context).paint_layers.active_mask_item
        try:
            item.id_color_remove(index=self.index)
        except RuntimeError as ex:
            self.report({'WARNING'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_object_mask_toggle(_PaintLayerOperator):
    """Show or hide the active layer on an object, or on one UDIM tile of it"""

    bl_idname = "material.paint_layer_object_mask_toggle"
    bl_label = "Toggle Layer On Object"
    bl_options = {'REGISTER', 'UNDO', 'INTERNAL'}

    object_name: bpy.props.StringProperty(name="Object")
    object_session_uid: bpy.props.IntProperty(
        name="Object Session UID",
        default=0,
        description="Session UID of the object; preferred over the name lookup",
    )
    tile: bpy.props.IntProperty(name="Tile", default=0, description="UDIM tile, or 0 for the whole object")

    @staticmethod
    def _target_object(object_name, object_session_uid):
        if object_session_uid:
            for candidate in bpy.data.objects:
                if candidate.session_uid == object_session_uid:
                    if candidate.library is not None or candidate.override_library is not None:
                        continue
                    return candidate
        # Fallback for scripted calls: only an editable local match; the RNA setter refuses the rest.
        for candidate in bpy.data.objects:
            if candidate.name == object_name and candidate.library is None and candidate.override_library is None:
                return candidate
        return None

    def execute(self, context):
        layer = self._layer(context)
        if layer is None:
            self.report({'ERROR'}, "Paint layer not found" if self.marker else "No active paint layer")
            return {'CANCELLED'}
        ob = self._target_object(self.object_name, self.object_session_uid)
        if ob is None:
            self.report({'ERROR'}, "Object \"{:s}\" not found".format(self.object_name))
            return {'CANCELLED'}
        if self.tile:
            layer.object_mask_tile_set(ob, self.tile, not layer.object_mask_tile_get(ob, self.tile))
        else:
            layer.object_mask_visible_set(ob, not layer.object_mask_visible_get(ob))
        return {'FINISHED'}


class MATERIAL_OT_paint_layer_object_mask_refresh_tiles(_PaintLayerOperator):
    """Rescan the UVs of the objects using the active layered material for occupied UDIM tiles"""

    bl_idname = "material.paint_layer_object_mask_refresh_tiles"
    bl_label = "Refresh Tiles"
    # No undo step: the tile lists are runtime-only caches.
    bl_options = {'INTERNAL'}

    def execute(self, context):
        mat = _context_material(context)
        layer = self._layer(context)
        if layer is None:
            return {'CANCELLED'}
        for ob in context.view_layer.objects:
            if any(slot.material == mat for slot in ob.material_slots):
                layer.object_mask_tiles(ob, refresh=True)
        return {'FINISHED'}


classes = (
    MATERIAL_OT_paint_layer_channel_add,
    MATERIAL_OT_paint_layer_channel_remove,
    MATERIAL_OT_paint_layer_channel_toggle,
    MATERIAL_OT_paint_layer_mask_add,
    MATERIAL_OT_paint_layer_mask_remove,
    MATERIAL_OT_paint_layer_mask_toggle,
    MATERIAL_OT_paint_layer_mask_filter_add,
    MATERIAL_OT_paint_layer_id_color_remove,
    MATERIAL_OT_paint_layer_object_mask_toggle,
    MATERIAL_OT_paint_layer_object_mask_refresh_tiles,
)
