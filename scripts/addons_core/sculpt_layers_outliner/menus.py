# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The menus the Sculpt Layers source offers: the Stack Layers header's Add menu and the active
row's context menu. The source names them through the stack source menu registry."""

import bpy

from .source import selected_or_active


class OUTLINER_MT_sculpt_layer_add(bpy.types.Menu):
    """Add a sculpt layer or a folder"""

    bl_idname = "OUTLINER_MT_sculpt_layer_add"
    bl_label = "Add Sculpt Layer"

    def draw(self, _context):
        layout = self.layout
        layout.operator("sculpt.layer_add", text="Layer", icon='ADD')
        layout.operator("sculpt.layer_group_add", text="Group", icon='NEWFOLDER')


def draw_header_start(layout, context, _space):
    """REC and Solo, labeled, right after the Back button: the two switches that change how the
    next strokes land, so they sit where the eye starts."""
    object = context.object
    mesh = object.data if object is not None and object.type == 'MESH' else None
    in_sculpt_mode = object is not None and object.mode == 'SCULPT'
    solo_active = mesh is not None and mesh.sculpt_layers_solo_active
    rec_active = object is not None and object.sculpt_layers_rec_active

    row = layout.row(align=True)
    row.enabled = in_sculpt_mode and not solo_active
    row.operator(
        "sculpt.layer_toggle_rec",
        text="REC",
        icon='RECORD_ON' if rec_active else 'RECORD_OFF',
        depress=rec_active,
    )

    row = layout.row(align=True)
    row.enabled = mesh is not None
    row.operator(
        "sculpt.layer_solo_base",
        text="Solo",
        icon='SOLO_ON' if solo_active else 'SOLO_OFF',
        depress=solo_active,
    )


def draw_header(layout, context, _space):
    """The source's own header controls: the two Add buttons and the operations menu shared with
    the row context menu. REC and Solo are in #draw_header_start, next to the Back button."""
    object = context.object
    mesh = object.data if object is not None and object.type == 'MESH' else None

    row = layout.row(align=True)
    row.enabled = mesh is not None
    row.operator("sculpt.layer_add", text="", icon='ADD')
    row.operator("sculpt.layer_group_add", text="", icon='NEWFOLDER')
    row.operator("sculpt_layers.mask_add", text="", icon='MOD_MASK')

    # The same entries the row's context menu carries: Merge Down and Bake among the rest.
    layout.menu("OUTLINER_MT_sculpt_layer_stack_context", text="", icon='DOWNARROW_HLT')


def _context_targets(context):
    """The nodes the menu acts on: the selected layers and folders, or the active layer when
    nothing is selected. The same rule the mask operators resolve their targets by."""
    object = context.object
    if object is None or object.type != 'MESH':
        return []
    return selected_or_active(object.data)


class OUTLINER_MT_sculpt_layer_stack_context(bpy.types.Menu):
    """Sculpt layer operations on the active row"""

    bl_idname = "OUTLINER_MT_sculpt_layer_stack_context"
    bl_label = "Sculpt Layer"

    def draw(self, context):
        layout = self.layout
        targets = _context_targets(context)
        layers = [node for node in targets if not isinstance(node, bpy.types.SculptLayerGroup)]
        with_mask = [node for node in targets if node.has_mask or node.mask_edit_active]
        without_mask = [node for node in targets if node not in with_mask]

        # Only the entries that mean something for what is selected: a folder has no layer verbs,
        # a node without a mask has no mask verbs but Add, one with a mask has no Add.
        if layers:
            layout.operator("sculpt.layer_duplicate", text="Duplicate", icon='DUPLICATE')
            layout.operator("sculpt.layer_merge_down", text="Merge Down", icon='TRIA_DOWN')
            if len(layers) > 1:
                layout.operator("sculpt.layer_merge_selected", text="Merge Selected")
            layout.separator()
            layout.operator("sculpt.layer_clear", text="Clear Layer", icon='X')
            layout.operator("sculpt.layer_invert", text="Invert Layer")
            layout.separator()
        layout.operator("sculpt.layer_solo_base", text="Solo Base")
        if with_mask:
            layout.operator("sculpt.layer_mask_isolate", text="Isolate by Mask")
        layout.separator()
        if without_mask:
            layout.operator("sculpt_layers.mask_add", text="Add Mask", icon='MOD_MASK')
        if with_mask:
            layout.operator("sculpt_layers.mask_edit", text="Edit Mask", icon='SCULPTMODE_HLT')
            layout.operator("sculpt_layers.mask_toggle", text="Toggle Mask", icon='MOD_SUBSURF')
            layout.operator("sculpt_layers.mask_remove", text="Remove Mask", icon='X')
        layout.separator()
        if layers:
            layout.operator("sculpt.layer_bake_to_shape_key", text="Bake to Shape Keys")
            layout.operator("sculpt.layer_bake", text="Bake and Remove Layers")
            layout.separator()
        layout.operator("sculpt.layer_validate", text="Validate Stale Layers")


classes = (
    OUTLINER_MT_sculpt_layer_add,
    OUTLINER_MT_sculpt_layer_stack_context,
)


def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)


def unregister():
    from bpy.utils import unregister_class
    for cls in reversed(classes):
        unregister_class(cls)
