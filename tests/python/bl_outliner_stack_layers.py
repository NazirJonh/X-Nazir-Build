# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression tests for the Stack Layers Outliner display mode."""

import sys
import unittest

import bpy


class StackLayersOutlinerTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        # The global context carries no window/screen in a script or timer context, and a file
        # read replaces the screen it had; the window manager's own list is what stays current.
        # Same pattern as bl_run_operators_event_simulate.py.
        self.window = next(window for window in bpy.context.window_manager.windows
                           if window.screen is not None)
        self.area = next(area for area in self.window.screen.areas if area.type == 'OUTLINER')
        self.region = next(region for region in self.area.regions if region.type == 'WINDOW')
        self.space = self.area.spaces.active

    def outliner_override(self):
        return bpy.context.temp_override(
            window=self.window,
            screen=self.window.screen,
            area=self.area,
            region=self.region,
        )

    def redraw_window(self):
        """A full redraw, tree rebuild included: wm.redraw_timer's poll wants a window, which the
        script context has none of, so it runs under the outliner's own override."""
        self.area.tag_redraw()
        with self.outliner_override():
            bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)

    def add_object_with_image_material(self):
        mesh = bpy.data.meshes.new("StackLayersMesh")
        object = bpy.data.objects.new("StackLayersObject", mesh)
        bpy.context.collection.objects.link(object)
        bpy.context.view_layer.objects.active = object
        object.select_set(True)

        material = bpy.data.materials.new("StackLayersMaterial")
        material.use_nodes = True
        image = bpy.data.images.new("StackLayersImage", 8, 8)
        texture = material.node_tree.nodes.new('ShaderNodeTexImage')
        texture.image = image
        principled = material.node_tree.nodes.get("Principled BSDF")
        material.node_tree.links.new(texture.outputs['Color'], principled.inputs['Base Color'])
        mesh.materials.append(material)
        return object, material, image

    def add_object_with_shape_keys(self):
        mesh = bpy.data.meshes.new("ShapeKeyMesh")
        mesh.vertices.add(1)
        object = bpy.data.objects.new("ShapeKeyObject", mesh)
        bpy.context.collection.objects.link(object)
        bpy.context.view_layer.objects.active = object
        object.select_set(True)
        object.shape_key_add(name="Basis")
        object.shape_key_add(name="Key 1")
        object.shape_key_add(name="Key 2")
        return object

    def focus_and_draw_stack(self, object):
        self.space.display_mode = 'STACK_LAYERS'
        with self.outliner_override():
            result = bpy.ops.outliner.stack_layer_focus(
                object=object.name,
                sub_index=-1,
                enter_edit_mode=False,
            )
        self.assertEqual(result, {'FINISHED'})
        self.redraw_window()

    def test_display_mode_roundtrip(self):
        self.space.display_mode = 'STACK_LAYERS'
        self.assertEqual(self.space.display_mode, 'STACK_LAYERS')
        self.space.display_mode = 'VIEW_LAYER'
        self.assertEqual(self.space.display_mode, 'VIEW_LAYER')

    def test_stack_layers_flags_defaults_all_shown(self):
        self.assertTrue(self.space.show_stack_layer_opacity)
        self.assertTrue(self.space.show_stack_layer_blend)
        self.assertTrue(self.space.show_stack_items)
        # A stack opens like a layer manager: tall rows, visibility toggle on the left.
        self.assertTrue(self.space.use_stack_layer_big_rows)
        self.assertTrue(self.space.use_stack_layer_visibility_left)
        self.assertFalse(self.space.use_stack_layer_sort_by_name)

    def test_filter_panel_properties_exist(self):
        for name in (
            "show_stack_layer_opacity",
            "show_stack_layer_blend",
            "show_stack_items",
            "use_stack_layer_big_rows",
            "use_stack_layer_sort_by_name",
            "use_stack_layer_pin",
            "stack_source",
        ):
            self.assertIn(name, bpy.types.SpaceOutliner.bl_rna.properties)

    def test_stack_operators_are_registered(self):
        for name in (
            "stack_layer_focus",
            "stack_layers_back",
            "stack_layer_pin_toggle",
            "stack_layer_activate",
            "stack_layer_clear_target",
            "stack_layer_move",
            "stack_layer_remove",
            "stack_layer_drop",
            "stack_layer_image_drop",
            "stack_layer_material_drop",
            "stack_layer_merge_down",
        ):
            self.assertTrue(hasattr(bpy.ops.outliner, name), name)

    def test_stack_source_default_and_switch(self):
        self.assertEqual(self.space.stack_source, 'PAINT_MATERIAL')
        self.space.stack_source = 'SHAPE_KEYS'
        self.assertEqual(self.space.stack_source, 'SHAPE_KEYS')

    def test_shape_key_source_lists_and_activates(self):
        # The second source exists to prove the display mode is not tied to paint layers: the same
        # tree, ordinals and operators drive data that shares nothing with images.
        object = self.add_object_with_shape_keys()
        self.space.stack_source = 'SHAPE_KEYS'
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_activate(ordinal=2), {'FINISHED'})
        self.assertEqual(object.active_shape_key_index, 2)
        self.assertEqual(object.active_shape_key.name, "Key 2")

    def test_shape_key_source_reorders(self):
        # Reorder goes through the same operator and the same ordinals as the paint stack does;
        # only what happens at the bottom of the seam differs.
        object = self.add_object_with_shape_keys()
        self.space.stack_source = 'SHAPE_KEYS'
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            result = bpy.ops.outliner.stack_layer_move(ordinal=1, to_ordinal=2)
        self.assertEqual(result, {'FINISHED'})
        self.assertEqual([key.name for key in object.data.shape_keys.key_blocks],
                         ["Basis", "Key 2", "Key 1"])

    def test_shape_key_source_leaves_paint_bindings_alone(self):
        object = self.add_object_with_shape_keys()
        self.space.stack_source = 'SHAPE_KEYS'
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            bpy.ops.outliner.stack_layer_activate(ordinal=1)
        bindings = bpy.context.scene.tool_settings.paint_mode.channel_image_bindings
        self.assertTrue(all(binding.image is None for binding in bindings))

    def test_shape_key_source_offers_no_add(self):
        # A source that declares no kinds of rows gets no Add at all: the operator polls out
        # before it could create anything, instead of offering kinds nothing listens to.
        object = self.add_object_with_shape_keys()
        self.space.stack_source = 'SHAPE_KEYS'
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            with self.assertRaises(RuntimeError):
                bpy.ops.outliner.stack_layer_add(ordinal=-1)

    def test_activate_writes_bindings(self):
        object, material, image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            result = bpy.ops.outliner.stack_layer_activate(ordinal=0)
        self.assertEqual(result, {'FINISHED'})
        bindings = bpy.context.scene.tool_settings.paint_mode.channel_image_bindings
        self.assertEqual(bindings[0].image, image)
        self.assertTrue(all(binding.image is None for binding in bindings[1:]))
        self.assertEqual(object.active_material, material)

    def test_activate_undo_preserves_paint_target(self):
        # Decided (docs/adr/0001-paint-bindings-preserve-across-undo.md): the paint bindings are
        # deliberately preserved across an undo restore -- a live paint target must not jump when
        # the graph rolls back -- so undoing the very step that wrote a binding leaves the binding
        # in place. What that undo does revert is the graph the step touched; the explicit exit is
        # #stack_layer_clear_target (test_clear_target_after_undo).
        object, _material, image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            bpy.ops.ed.undo_push(message="Stack Layers test baseline")
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_activate(ordinal=0), {'FINISHED'})
        self.assertEqual(bpy.context.scene.tool_settings.paint_mode.channel_image_bindings[0].image, image)
        with self.outliner_override():
            bpy.ops.ed.undo()
        # The binding survived the undo that wrote it, and the target it names is still alive.
        self.assertEqual(bpy.context.scene.tool_settings.paint_mode.channel_image_bindings[0].image, image)
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=0), image.session_uid)

    def test_clear_target_after_undo(self):
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_activate(ordinal=0), {'FINISHED'})
        with self.outliner_override():
            bpy.ops.ed.undo()
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_clear_target(), {'FINISHED'})
        self.assertTrue(
            all(binding.image is None for binding in bpy.context.scene.tool_settings.paint_mode.channel_image_bindings)
        )

    def test_dropped_image_assigns_to_channel(self):
        # The drop flow's exec, driven directly: an empty layer target adds a new layer on top,
        # and the image becomes that layer's map for the channel the popup picked. The marker it
        # carries is what a row-targeted drop addresses the layer by.
        object, material, image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(
                bpy.ops.outliner.stack_layer_channel_image_assign(
                    image_uid=image.session_uid, channel='0', layer=""),
                {'FINISHED'})
        self.assertNotEqual(image.paint_layer_id, "")
        self.assertEqual(image.paint_layer_channel, 'BASE_COLOR')
        # The image is referenced twice: by the test setup's own node (the stack's bare base) and
        # by the map the new layer got.
        maps = [node for node in material.node_tree.nodes
                if node.type == 'TEX_IMAGE' and node.image == image]
        self.assertEqual(len(maps), 2)

        # The new layer's row previews the image it was given; the bare base previews its own
        # map, which is the same image from the test setup.
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=1), image.session_uid)
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=0), image.session_uid)

        # A row-targeted drop with a second image addresses the same layer by the marker the
        # first drop handed out, and replaces that layer's map: the map node is reused (it is
        # the layer's own), so the second image takes the one slot and the first goes back to
        # being the bare base's map alone.
        other = bpy.data.images.new("SecondStackLayersImage", 8, 8)
        with self.outliner_override():
            self.assertEqual(
                bpy.ops.outliner.stack_layer_channel_image_assign(
                    image_uid=other.session_uid, channel='0', layer=image.paint_layer_id),
                {'FINISHED'})
        self.assertEqual(other.paint_layer_id, image.paint_layer_id)
        self.assertEqual(other.paint_layer_channel, 'BASE_COLOR')
        maps = [node for node in material.node_tree.nodes
                if node.type == 'TEX_IMAGE' and node.image == other]
        self.assertEqual(len(maps), 1)
        maps = [node for node in material.node_tree.nodes
                if node.type == 'TEX_IMAGE' and node.image == image]
        self.assertEqual(len(maps), 1)

        # The row's preview followed its layer to the second image.
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=1), other.session_uid)
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=0), image.session_uid)

    def test_drag_sub_row_carries_channel_image(self):
        # Dragging a channel sub-row has to carry the sub-row's own data-block -- the map image --
        # not the stack's data-block the tree store points at, and not reinterpret the image
        # pointer as some other struct. The hook runs the same carrier logic the drag operator's
        # invoke does, and builds the same drag to read back.
        object, _material, image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        self.redraw_window()
        # The bare base's own map is the setup's image, on the Base Color channel.
        self.assertEqual(
            self.space.debug_stack_layer_item_drag_id(ordinal=0, role=0), image.session_uid)
        # An ordinal past the stack has no sub-row, and nothing to drag.
        self.assertEqual(self.space.debug_stack_layer_item_drag_id(ordinal=99, role=0), 0)

    def test_rewire_in_node_tree_updates_stack(self):
        # The rows are read from the stack's node graph, so a rewire made in the node editor --
        # without touching any layer the Outliner was told about -- has to reach them all the
        # same: the notifiers a graph edit sends must lead to a re-read, and the state hash must
        # see the graph's shape, not just its size.
        object, material, image = self.add_object_with_image_material()
        other = bpy.data.images.new("RewiredStackLayersImage", 8, 8)
        self.focus_and_draw_stack(object)
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=0), image.session_uid)

        # Swap the map the bare base reads: one node property, one redraw, one new map.
        texture = next(node for node in material.node_tree.nodes
                       if node.type == 'TEX_IMAGE' and node.image == image)
        texture.image = other
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=0), other.session_uid)

        # Unlink the map from the graph and the row has nothing to read; re-linking it brings the
        # map back.
        material.node_tree.links.remove(texture.outputs['Color'].links[0])
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=0), 0)
        principled = material.node_tree.nodes.get("Principled BSDF")
        material.node_tree.links.new(texture.outputs['Color'], principled.inputs['Base Color'])
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=0), other.session_uid)

    def test_marker_addresses_the_row_not_the_position(self):
        # A marker names the row by what it is, so a script that re-addresses a row after an edit
        # moved it gets the same row -- where a remembered ordinal is the position trap. The
        # layer's marker is what its map carries as paint_layer_id, and the operators that address
        # an existing row accept it in place of the position.
        object, _material, image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(
                bpy.ops.outliner.stack_layer_channel_image_assign(
                    image_uid=image.session_uid, channel='0', layer=""),
                {'FINISHED'})
        marker = image.paint_layer_id
        self.assertNotEqual(marker, "")
        self.redraw_window()
        # The image layer sits at ordinal 1 when the marker is taken.
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=1), image.session_uid)

        # A new layer on top renumbers the image layer to ordinal 2; the marker still names it.
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_add(ordinal=-1), {'FINISHED'})
        self.redraw_window()
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_activate(marker=marker), {'FINISHED'})
        bindings = bpy.context.scene.tool_settings.paint_mode.channel_image_bindings
        self.assertEqual(bindings[0].image, image)

        # Removing by marker takes the image layer out, not whatever sits at the position the
        # marker was issued at: the bare base at ordinal 0 survives.
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_remove(marker=marker), {'FINISHED'})
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=0), image.session_uid)
        # Only the bare base and the empty layer are left; ordinal 2 is no row at all.
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=2), 0)

    def test_dropped_material_adds_group(self):
        # The material drop's handler path, driven directly: a material dropped on the stack --
        # anywhere on it, the empty-space target here -- becomes a new group on top that stands
        # for it. Nothing of the stack's own graph changes.
        object, material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)

        dropped = bpy.data.materials.new("DroppedGroupMaterial")
        with self.outliner_override():
            self.assertTrue(
                self.space.debug_stack_layer_drop_id(dropped=dropped, target_ordinal=-1))
        # The group is the top row: it previews the material it stands for.
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=1), dropped.session_uid)
        # The group is a real folder in the stack's graph, referencing the material from its own
        # node tree; the stack's material gained no map nodes.
        groups = [node for node in material.node_tree.nodes if node.type == 'GROUP']
        self.assertEqual(len(groups), 1)
        self.assertEqual(groups[0].node_tree.get("pbr_paint_layer_material"), dropped)
        self.assertEqual(
            len([node for node in material.node_tree.nodes if node.type == 'TEX_IMAGE']), 1)

    def test_dropped_material_on_a_row_lands_on_top(self):
        # A row target does not change where the group goes: the landing spot is not the point,
        # the group goes on top and the row it was dropped on keeps its own map.
        object, material, image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)

        dropped = bpy.data.materials.new("DroppedGroupMaterial")
        with self.outliner_override():
            self.assertTrue(
                self.space.debug_stack_layer_drop_id(dropped=dropped, target_ordinal=0))
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=1), dropped.session_uid)
        maps = [node for node in material.node_tree.nodes
                if node.type == 'TEX_IMAGE' and node.image == image]
        self.assertEqual(len(maps), 1)

    def test_dropped_linked_material_is_referenced(self):
        # A linked material is referenced, not changed, so it makes as good a placeholder as a
        # local one: the group reads as it and points at it.
        object, material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)

        filepath = bpy.path.abspath(bpy.app.tempdir + "/stack_layers_material_group_lib.blend")
        # The copy keeps this session on the untitled file: linking from the file one is currently
        # in is refused outright.
        bpy.ops.wm.save_as_mainfile(filepath=filepath, relative_remap=False, copy=True)
        with bpy.data.libraries.load(filepath, link=True) as (data_from, data_to):
            data_to.materials = [name for name in data_from.materials
                                 if name.startswith("StackLayersMaterial")]
        self.assertEqual(len(data_to.materials), 1)
        linked = data_to.materials[0]
        self.assertIsNotNone(linked.library)

        with self.outliner_override():
            self.assertTrue(
                self.space.debug_stack_layer_drop_id(dropped=linked, target_ordinal=-1))
        self.redraw_window()
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=1), linked.session_uid)

    def test_dropped_material_refuses_on_linked_stack(self):
        # The group is created in the stack's own graph, so a linked stack material refuses the
        # drop the way it refuses every other edit -- and refusing means nothing was added.
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)

        filepath = bpy.path.abspath(bpy.app.tempdir + "/stack_layers_material_owner_lib.blend")
        bpy.ops.wm.save_as_mainfile(filepath=filepath, relative_remap=False, copy=True)
        with bpy.data.libraries.load(filepath, link=True) as (data_from, data_to):
            data_to.materials = [name for name in data_from.materials
                                 if name.startswith("StackLayersMaterial")]
        self.assertEqual(len(data_to.materials), 1)
        linked = data_to.materials[0]

        mesh = bpy.data.meshes.new("LinkedGroupMesh")
        linked_object = bpy.data.objects.new("LinkedGroupObject", mesh)
        bpy.context.collection.objects.link(linked_object)
        mesh.materials.append(linked)
        bpy.context.view_layer.objects.active = linked_object
        self.focus_and_draw_stack(linked_object)

        local = bpy.data.materials.new("DroppedGroupMaterial")
        self.assertFalse(
            self.space.debug_stack_layer_drop_id(dropped=local, target_ordinal=-1))

    def test_merged_down_layers_become_a_group(self):
        # Merge down is a collapse into a folder: the pair composites the same inside a group
        # whose blend is the lower row's, and the stack lists one row where there were two.
        object, material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            bpy.ops.outliner.stack_layer_add(ordinal=-1)
            bpy.ops.outliner.stack_layer_add(ordinal=-1)
        self.redraw_window()

        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_merge_down(ordinal=2), {'FINISHED'})
        self.redraw_window()
        # The group row previews nothing of its own, and the row above it is gone.
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=1), 0)
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=2), 0)
        # The collapse is a real folder in the stack's graph.
        self.assertEqual(
            len([node for node in material.node_tree.nodes if node.type == 'GROUP']), 1)
        # The bottom of the stack has nothing below to merge into.
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_merge_down(ordinal=0), {'CANCELLED'})

    def test_removed_rows_span_the_selection(self):
        # The removal operator acts on the whole selection, the way group does: whatever rows are
        # picked go at once, the highest ordinal first.
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            bpy.ops.outliner.stack_layer_add(ordinal=-1)
            bpy.ops.outliner.stack_layer_add(ordinal=-1)
        self.redraw_window()

        self.space.debug_stack_layer_row_select(ordinal=1)
        self.space.debug_stack_layer_row_select(ordinal=2, extend=True)
        self.assertTrue(self.space.debug_stack_layer_row_is_selected(ordinal=1))
        self.assertTrue(self.space.debug_stack_layer_row_is_selected(ordinal=2))
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_remove(), {'FINISHED'})
        self.redraw_window()
        # Only the bare base is left.
        self.assertEqual(self.space.debug_stack_layer_row_preview_uid(ordinal=1), 0)

    def test_linked_material_operators_cancel(self):
        # The stack of a linked material is read-only: the edit operators refuse, and refusing
        # means the graph was not touched, not that it was half-edited.
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)

        filepath = bpy.path.abspath(bpy.app.tempdir + "/stack_layers_lib_test.blend")
        # The copy keeps this session on the untitled file: linking from the file one is currently
        # in is refused outright.
        bpy.ops.wm.save_as_mainfile(filepath=filepath, relative_remap=False, copy=True)
        with bpy.data.libraries.load(filepath, link=True) as (data_from, data_to):
            data_to.materials = [name for name in data_from.materials
                                 if name.startswith("StackLayersMaterial")]
        self.assertEqual(len(data_to.materials), 1)
        linked = data_to.materials[0]
        self.assertIsNotNone(linked.library)

        mesh = bpy.data.meshes.new("LinkedStackMesh")
        linked_object = bpy.data.objects.new("LinkedStackObject", mesh)
        bpy.context.collection.objects.link(linked_object)
        mesh.materials.append(linked)
        bpy.context.view_layer.objects.active = linked_object
        self.focus_and_draw_stack(linked_object)

        with self.outliner_override():
            # Remove, add and move all poll on editability, and refuse before the operator runs.
            with self.assertRaises(RuntimeError):
                bpy.ops.outliner.stack_layer_remove(ordinal=0)
            with self.assertRaises(RuntimeError):
                bpy.ops.outliner.stack_layer_move(ordinal=0, direction='UP')
            with self.assertRaises(RuntimeError):
                bpy.ops.outliner.stack_layer_add(ordinal=-1)
            # The image-assign exec polls only for the area, and refuses from the edit itself
            # (an error report, which bpy.ops raises): nothing is written to the linked graph.
            dropped = bpy.data.images.new("DroppedStackLayersImage", 8, 8)
            with self.assertRaises(RuntimeError):
                bpy.ops.outliner.stack_layer_channel_image_assign(
                    image_uid=dropped.session_uid, channel='0', layer="")

    def test_two_outliners_single_owner(self):
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)

        # Two Outliners on one stack: the duplicated one reads the same focus and the same
        # bindings, and neither of them owns the other. The duplicate goes to a new window, which
        # a background build has none of, so this runs where it can and skips where it cannot.
        try:
            with self.outliner_override():
                bpy.ops.screen.area_dupli('INVOKE_DEFAULT')
        except RuntimeError:
            self.skipTest("Needs a window to duplicate the area into")
        windows = [
            (window, area)
            for window in bpy.context.window_manager.windows
            for area in window.screen.areas
            if area.type == 'OUTLINER' and area != self.area
        ]
        if not windows:
            self.skipTest("The duplicated Outliner area is not reachable here")
        window, area = windows[0]
        region = next(region for region in area.regions if region.type == 'WINDOW')
        area.spaces.active.display_mode = 'STACK_LAYERS'
        area.spaces.active.stack_layers_view = 'STACK'

        with bpy.context.temp_override(window=window, screen=window.screen, area=area, region=region):
            self.assertEqual(bpy.ops.outliner.stack_layer_activate(ordinal=0), {'FINISHED'})
        # What the second Outliner activated is true everywhere: the bindings are session state,
        # not a per-space secret.
        bindings = bpy.context.scene.tool_settings.paint_mode.channel_image_bindings
        self.assertIsNotNone(bindings[0].image)

    def test_delete_focused_object_does_not_crash(self):
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        bpy.data.objects.remove(object)
        self.redraw_window()

    def test_delete_pinned_focused_object_does_not_crash(self):
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        self.space.use_stack_layer_pin = True
        bpy.data.objects.remove(object)
        self.redraw_window()

    def test_pinned_focus_survives_undo(self):
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        self.space.use_stack_layer_pin = True
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_add(ordinal=-1), {'FINISHED'})
        # The undo restores the stack; the focus was set before it, and names its object by
        # session UID, which survives the undo where a pointer would not.
        with self.outliner_override():
            bpy.ops.ed.undo()
        self.redraw_window()

    def test_remap_material_does_not_crash(self):
        object, material, _image = self.add_object_with_image_material()
        other = bpy.data.materials.new("OtherStackMaterial")
        self.focus_and_draw_stack(object)
        material.user_remap(other)
        self.redraw_window()

    def test_stack_focus_name_reads_owner(self):
        object, material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        self.redraw_window()
        self.assertEqual(self.space.stack_focus_name, material.name)

    def test_no_active_object_does_not_crash(self):
        self.space.display_mode = 'STACK_LAYERS'
        self.space.stack_layers_view = 'STACK'
        bpy.context.view_layer.objects.active = None
        self.redraw_window()

    def test_selected_row_survives_move(self):
        # A move renumbers the row it acted on; the row itself -- not whatever ordinal it used to
        # have -- is what has to read as selected once the tree rebuilds.
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_add(ordinal=-1), {'FINISHED'})
        self.redraw_window()
        self.assertTrue(self.space.debug_stack_layer_row_is_selected(ordinal=1))

        with self.outliner_override():
            result = bpy.ops.outliner.stack_layer_move(ordinal=1, direction='DOWN')
        self.assertEqual(result, {'FINISHED'})
        self.redraw_window()
        self.assertTrue(self.space.debug_stack_layer_row_is_selected(ordinal=0))
        self.assertFalse(self.space.debug_stack_layer_row_is_selected(ordinal=1))

    def test_selected_group_survives_renumbering(self):
        # An empty group starts out open and selected; inserting a layer below it renumbers the
        # group, and both have to still read true at the group's new ordinal.
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_group_add(ordinal=-1), {'FINISHED'})
        self.redraw_window()
        self.assertTrue(self.space.debug_stack_layer_row_is_selected(ordinal=1))
        self.assertTrue(self.space.debug_stack_layer_row_is_open(ordinal=1))

        with self.outliner_override():
            result = bpy.ops.outliner.stack_layer_add(type='EMPTY', ordinal=0)
        self.assertEqual(result, {'FINISHED'})
        self.redraw_window()
        self.assertTrue(self.space.debug_stack_layer_row_is_open(ordinal=2))

    def test_clicked_row_state_survives_rebuild(self):
        # What the user did since the last build -- selecting a row, collapsing one -- lives in the
        # tree store; a rebuild reads it into the identity-keyed map rather than reverting the rows
        # to what the map held before the click.
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_add(ordinal=-1), {'FINISHED'})
        self.redraw_window()
        # The add left its own row selected; picking row 0 instead is the user's doing, not an
        # edit's.
        self.space.debug_stack_layer_row_select(ordinal=0)
        self.redraw_window()
        self.assertTrue(self.space.debug_stack_layer_row_is_selected(ordinal=0))
        self.assertFalse(self.space.debug_stack_layer_row_is_selected(ordinal=1))
        # The next build finds the row already in the map and agrees with itself.
        self.redraw_window()
        self.assertTrue(self.space.debug_stack_layer_row_is_selected(ordinal=0))

    def test_row_state_survives_ten_renumbering_edits(self):
        # Ten rebuilds with a renumbering edit between each. The collapsed group has to follow its
        # row however the ordinals shift -- it must not pop open on a fresh tree-store entry, and
        # no other row may inherit its collapse -- and the selection has to be the edit's own row,
        # not whatever moved into the number.
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            # The material's own bottom layer and two added layers, wrapped into a group. The
            # group is what the edit leaves selected; it has to hold something, since a row
            # without children cannot be collapsed.
            self.assertEqual(bpy.ops.outliner.stack_layer_add(ordinal=-1), {'FINISHED'})
            self.assertEqual(bpy.ops.outliner.stack_layer_add(ordinal=-1), {'FINISHED'})
            self.assertEqual(
                bpy.ops.outliner.stack_layer_group(ordinal=1, to_ordinal=2), {'FINISHED'})
        self.redraw_window()

        group_ordinal = next(ordinal for ordinal in range(6)
                             if self.space.debug_stack_layer_row_is_selected(ordinal=ordinal))
        self.assertTrue(self.space.debug_stack_layer_row_is_open(ordinal=group_ordinal))

        self.space.debug_stack_layer_row_closed_set(ordinal=group_ordinal, closed=True)
        self.redraw_window()
        self.assertFalse(self.space.debug_stack_layer_row_is_open(ordinal=group_ordinal))
        # The collapse did not take the selection with it.
        self.assertTrue(self.space.debug_stack_layer_row_is_selected(ordinal=group_ordinal))

        for _round in range(10):
            with self.outliner_override():
                # A layer at the bottom renumbers every row above it, the group included.
                self.assertEqual(
                    bpy.ops.outliner.stack_layer_add(type='EMPTY', ordinal=0), {'FINISHED'})
            group_ordinal += 1
            self.redraw_window()
            self.assertFalse(self.space.debug_stack_layer_row_is_open(ordinal=group_ordinal))
            # The fresh row the edit made starts open: it did not inherit the group's collapse.
            self.assertTrue(self.space.debug_stack_layer_row_is_open(ordinal=0))
            self.assertTrue(self.space.debug_stack_layer_row_is_selected(ordinal=0))

            with self.outliner_override():
                self.assertEqual(bpy.ops.outliner.stack_layer_remove(ordinal=0), {'FINISHED'})
            group_ordinal -= 1
            self.redraw_window()
            self.assertFalse(self.space.debug_stack_layer_row_is_open(ordinal=group_ordinal))
            # Removing leaves nothing selected: no row inherited the removed one's state.
            self.assertFalse(self.space.debug_stack_layer_row_is_selected(ordinal=0))
            self.assertFalse(self.space.debug_stack_layer_row_is_selected(ordinal=1))

    def test_selection_does_not_leak_across_focus_change(self):
        object, _material, _image = self.add_object_with_image_material()
        other_object, _other_material, _other_image = self.add_object_with_image_material()

        # Give the second stack a row at the ordinal the first stack is about to record a selection
        # for, so a leak would land on a row that actually exists rather than passing by having
        # nothing to land on.
        self.focus_and_draw_stack(other_object)
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_add(ordinal=-1), {'FINISHED'})
        self.redraw_window()

        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_add(ordinal=-1), {'FINISHED'})
        # The selection for `object`'s row 1 is now recorded against that row's identity.
        # Switching focus before the rebuild must not let it land on the other stack's own row 1
        # just because the ordinal matches.
        self.focus_and_draw_stack(other_object)
        self.assertFalse(self.space.debug_stack_layer_row_is_selected(ordinal=1))

    def test_selection_does_not_leak_across_source_switch(self):
        # Same problem as a focus change, but for switching which source is shown on one object:
        # the selection names a paint stack row, and must not resolve against the shape key stack
        # it is switched to.
        object, _material, _image = self.add_object_with_image_material()
        object.data.vertices.add(1)
        object.shape_key_add(name="Basis")
        object.shape_key_add(name="Key 1")
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_add(ordinal=-1), {'FINISHED'})

        self.space.stack_source = 'SHAPE_KEYS'
        self.focus_and_draw_stack(object)
        self.assertFalse(self.space.debug_stack_layer_row_is_selected(ordinal=1))

    def _duplicate_outliner_area(self):
        """A second, independently drawn Outliner area, or None where the test runner has no
        window to duplicate one into -- see test_two_outliners_single_owner for why this can only
        run where it can and skip where it cannot."""
        try:
            with self.outliner_override():
                bpy.ops.screen.area_dupli('INVOKE_DEFAULT')
        except RuntimeError:
            return None
        windows = [
            (window, area)
            for window in bpy.context.window_manager.windows
            for area in window.screen.areas
            if area.type == 'OUTLINER' and area != self.area
        ]
        return windows[0] if windows else None

    def test_debug_drop_refuses_between_different_materials(self):
        # #outliner_stack_layer_debug_drop goes through the same identity resolve a real drop
        # does; two Outliners on two different materials must refuse each other's rows.
        object_a, _material_a, _image_a = self.add_object_with_image_material()
        object_b, _material_b, _image_b = self.add_object_with_image_material()
        self.focus_and_draw_stack(object_a)

        duplicated = self._duplicate_outliner_area()
        if duplicated is None:
            self.skipTest("Needs a window to duplicate the area into")
        window, area = duplicated
        region = next(region for region in area.regions if region.type == 'WINDOW')
        other_space = area.spaces.active
        other_space.display_mode = 'STACK_LAYERS'
        other_space.stack_layers_view = 'STACK'
        with bpy.context.temp_override(window=window, screen=window.screen, area=area, region=region):
            self.assertEqual(
                bpy.ops.outliner.stack_layer_focus(
                    object=object_b.name, sub_index=-1, enter_edit_mode=False),
                {'FINISHED'},
            )
            area.tag_redraw()
            bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)

        # Refusing before the resolve reaches a move is what guarantees neither graph is touched:
        # #outliner_stack_layer_debug_drop calls #StackSource::row_move nowhere but its last line.
        self.assertFalse(
            other_space.debug_stack_layer_drop_between(
                source_space=self.space, source_ordinal=0, target_ordinal=0))

    def test_debug_drop_reorders_within_the_same_stack(self):
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_add(ordinal=-1), {'FINISHED'})
        self.redraw_window()

        duplicated = self._duplicate_outliner_area()
        if duplicated is None:
            self.skipTest("Needs a window to duplicate the area into")
        window, area = duplicated
        other_space = area.spaces.active
        other_space.display_mode = 'STACK_LAYERS'
        other_space.stack_layers_view = 'STACK'
        with bpy.context.temp_override(window=window, screen=window.screen, area=area):
            area.tag_redraw()
            bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)

        # Both Outliners show the same object's stack, so the drop is exactly a reorder.
        self.assertTrue(
            other_space.debug_stack_layer_drop_between(
                source_space=self.space, source_ordinal=1, target_ordinal=0))

    def test_material_without_nodetree_does_not_crash(self):
        object, material, _image = self.add_object_with_image_material()
        material.use_nodes = False
        self.focus_and_draw_stack(object)
        self.redraw_window()


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
