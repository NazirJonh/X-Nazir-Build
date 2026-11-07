# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The row button operators the Sculpt Layers source's rows carry.

Each one resolves its row through the stable key the button's properties carry -- the same key
the build named the row by, with no per-space state between the two -- and runs the sculpt layer
operator that owns the mutation: the undo steps and the tree invariants stay with it.
"""

import bpy
from bpy.props import IntProperty, StringProperty

from .source import finish_open_mask_edit, mesh_of, nodes_by_uid, parse_stable_key, \
    row_make_active, selected_or_active


class _RowOperatorBase:
    """The plumbing every row button operator shares: the row properties, the poll, and the
    helpers that carry the pinned object and resolve the pressed row's node.

    A plain mix-in rather than a bpy.types.Operator subclass: the property annotations of a
    registered type are collected from bases that are not registered types themselves, so the
    annotations declared here reach every operator below through its bases, and the classes
    carry only their own id, label and execute."""

    ordinal: IntProperty(name="Ordinal", default=-1)
    marker: StringProperty(name="Marker", default="")
    stable_key: StringProperty(name="Stable Key", default="")

    @classmethod
    def poll(cls, context):
        """The row buttons only mean something on a mesh object: the sculpt-layer operators they
        delegate to act on the active object's layer tree."""
        object = context.active_object
        return object is not None and object.type == 'MESH'

    def _carry_pinned_object(self, context):
        """The object the row buttons act on: the one the Stack Layers view shows. With Pin Stack
        on, that is not necessarily the active object, and the sculpt layer operators act on the
        active one, so the pinned object is made active first -- as the Outliner does for a click
        on a row. Returns the object, or None when there is nothing to act on."""
        object = context.active_object
        space = context.space_data
        if space is not None and space.type == 'OUTLINER' and space.use_stack_layer_pin:
            pinned = bpy.data.objects.get(space.stack_focus_name)
            if pinned is not None and pinned != object and pinned.type == 'MESH':
                context.view_layer.objects.active = pinned
                object = pinned
        return object

    def _resolve_pressed_node(self, context):
        """The node the pressed button names, through its stable key: 'sculpt_layer_17' or
        'sculpt_group_3' names the node in the pinned (or active) object's tree, wherever the row
        sits now and whichever Outliner was last rebuilt. Returns (object, node), either possibly
        None."""
        object = self._carry_pinned_object(context)
        if object is None:
            return None, None
        mesh = mesh_of(object)
        if mesh is None or not self.stable_key:
            return object, None
        parsed = parse_stable_key(self.stable_key)
        if parsed is None:
            return object, None
        uid, _kind = parsed
        return object, nodes_by_uid(mesh).get(uid)

    def _mask_op_exec(self, context, operator):
        """Run a `sculpt.layer_mask_*` operator on every mask target, reporting each refusal."""
        object = self._carry_pinned_object(context)
        if object is None:
            return {'CANCELLED'}
        nodes = selected_or_active(object.data)
        if not nodes:
            self.report({'WARNING'}, "Select a sculpt layer or folder first")
            return {'CANCELLED'}
        succeeded = False
        for node in nodes:
            try:
                result = getattr(bpy.ops.sculpt, operator)(node_uid=node.uid)
            except RuntimeError as ex:
                # A refusal of the delegated operator (an open mask edit, a stale domain) is the
                # user's to see, not a silent no-op.
                self.report({'ERROR'}, str(ex))
                continue
            succeeded = succeeded or result == {'FINISHED'}
            if operator == "layer_mask_add" and result == {'FINISHED'}:
                # Adding a mask opens it for painting. From the Outliner it only attaches the
                # mask: the edit is entered from the mask slot, and an open session would also
                # refuse the next target of this loop.
                finish_open_mask_edit(object.data)
        return {'FINISHED' if succeeded else 'CANCELLED'}


class SCULPT_LAYERS_OT_mask_add(_RowOperatorBase, bpy.types.Operator):
    """Add a mask to the selected sculpt layers and folders"""

    bl_idname = "sculpt_layers.mask_add"
    bl_label = "Add Sculpt Layer Mask"
    # The delegated operator records its own sculpt undo step, as for the mask toggle.
    bl_options = {'REGISTER'}

    def execute(self, context):
        return self._mask_op_exec(context, "layer_mask_add")


class SCULPT_LAYERS_OT_mask_remove(_RowOperatorBase, bpy.types.Operator):
    """Remove the masks of the selected sculpt layers and folders"""

    bl_idname = "sculpt_layers.mask_remove"
    bl_label = "Remove Sculpt Layer Mask"
    # The delegated operator records its own sculpt undo step, as for the mask toggle.
    bl_options = {'REGISTER'}

    def execute(self, context):
        return self._mask_op_exec(context, "layer_mask_remove")


class SCULPT_LAYERS_OT_mask_toggle(_RowOperatorBase, bpy.types.Operator):
    """Toggle the mask of the node a row button was pressed on"""

    bl_idname = "sculpt_layers.mask_toggle"
    bl_label = "Toggle Sculpt Layer Mask"
    # The delegated operator records its own sculpt undo step; adding UNDO here would insert a
    # memfile step that does not compose with the sculpt stroke steps.
    bl_options = {'REGISTER'}

    def execute(self, context):
        if self.ordinal < 0:
            # From a menu: the selected layers and folders, like Add and Remove Mask.
            return self._mask_op_exec(context, "layer_mask_toggle")
        _object, node = self._resolve_pressed_node(context)
        if node is None:
            return {'CANCELLED'}
        try:
            return bpy.ops.sculpt.layer_mask_toggle(node_uid=node.uid)
        except RuntimeError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}


class SCULPT_LAYERS_OT_mask_edit(_RowOperatorBase, bpy.types.Operator):
    """Open the mask of the node a row's mask slot was clicked on for editing, or close it"""

    bl_idname = "sculpt_layers.mask_edit"
    bl_label = "Edit Sculpt Layer Mask"
    # The delegated operator records its own sculpt undo step, as for the mask toggle.
    bl_options = {'REGISTER'}

    def execute(self, context):
        if self.ordinal < 0:
            return self._mask_op_exec(context, "layer_mask_edit_toggle")
        _object, node = self._resolve_pressed_node(context)
        if node is None:
            return {'CANCELLED'}
        try:
            return bpy.ops.sculpt.layer_mask_edit_toggle(node_uid=node.uid)
        except RuntimeError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}


class SCULPT_LAYERS_OT_mask_edit_finish(_RowOperatorBase, bpy.types.Operator):
    """Apply the weight mask being edited and return to sculpting"""

    bl_idname = "sculpt_layers.mask_edit_finish"
    bl_label = "Finish Sculpt Layer Mask Edit"
    # The delegated operator records its own sculpt undo step, as for the mask toggle.
    bl_options = {'REGISTER'}

    def execute(self, context):
        object = self._carry_pinned_object(context)
        if object is None:
            return {'CANCELLED'}
        # A closed session (or none at all) is a success: the row buttons run this on every click
        # on the Base slot, masked or not.
        if not finish_open_mask_edit(object.data):
            self.report({'ERROR'}, "Could not apply the weight mask")
            return {'CANCELLED'}
        return {'FINISHED'}


class SCULPT_LAYERS_OT_rec_toggle(_RowOperatorBase, bpy.types.Operator):
    """Arm or disarm recording into the sculpt layers of this object"""

    bl_idname = "sculpt_layers.rec_toggle"
    bl_label = "Toggle Sculpt Layer REC"
    # The delegated operator owns the undo step, as for the mask toggle.
    bl_options = {'REGISTER'}

    def execute(self, context):
        obj = self._carry_pinned_object(context)
        if obj is None:
            return {'CANCELLED'}
        try:
            was_rec = obj.sculpt_layers_rec_active
            _object, node = self._resolve_pressed_node(context)
            if node is not None and not isinstance(node, bpy.types.SculptLayerGroup):
                active = obj.data.sculpt_layers_active
                was_active = active is not None and active.uid == node.uid
                # The row the mark sits on becomes the active, selected one at once: recording
                # follows the active layer, and the Outliner row has to read the same way.
                if not row_make_active(obj, node.uid):
                    self.report({'WARNING'}, "Could not make the sculpt layer active")
                    return {'CANCELLED'}
                if not was_active and was_rec:
                    # Recording that is already armed simply moves to this layer.
                    return {'FINISHED'}
            if bpy.ops.sculpt.layer_toggle_rec() != {'FINISHED'}:
                self.report({'WARNING'}, "Could not toggle sculpt layer recording")
                return {'CANCELLED'}
        except RuntimeError as ex:
            # A refusal of the delegated operator (a mask being edited, a hidden group) is the
            # user's to see, not a silent no-op.
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


classes = (
    SCULPT_LAYERS_OT_mask_add,
    SCULPT_LAYERS_OT_mask_remove,
    SCULPT_LAYERS_OT_mask_toggle,
    SCULPT_LAYERS_OT_mask_edit,
    SCULPT_LAYERS_OT_mask_edit_finish,
    SCULPT_LAYERS_OT_rec_toggle,
)


def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)


def unregister():
    from bpy.utils import unregister_class
    for cls in reversed(classes):
        unregister_class(cls)
