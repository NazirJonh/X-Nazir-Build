# SPDX-FileCopyrightText: 2026 Nazir Galimov
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
        self.space.stack_source = 'SHAPE_KEYS'
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

    def test_stack_source_default_is_paint_material(self):
        # Paint layers are the mode's primary source and the one a space starts at; shape keys is
        # the other source a user can switch to.
        self.assertEqual(self.space.stack_source, 'PAINT_MATERIAL')
        items = [item.identifier for item in
                 bpy.types.SpaceOutliner.bl_rna.properties['stack_source'].enum_items]
        self.assertEqual(items, ['PAINT_MATERIAL', 'SHAPE_KEYS'])
        # The channel the paint stack's value column addresses lives on the paint settings.
        self.assertTrue(
            hasattr(bpy.context.tool_settings.paint_mode, "stack_layer_channel"))

    def test_back_leaves_the_mode(self):
        self.space.display_mode = 'STACK_LAYERS'
        self.space.stack_layers_view = 'STACK'
        with self.outliner_override():
            # With `keep_view` the mode stays and only steps back to the objects list.
            self.assertEqual(bpy.ops.outliner.stack_layers_back(keep_view=True), {'FINISHED'})
        self.assertEqual(self.space.display_mode, 'STACK_LAYERS')
        self.assertEqual(self.space.stack_layers_view, 'OBJECTS')
        self.space.stack_layers_view = 'STACK'
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layers_back(), {'FINISHED'})
        self.assertEqual(self.space.display_mode, 'VIEW_LAYER')
        self.assertEqual(self.space.stack_layers_view, 'OBJECTS')

    def test_shape_key_source_lists_and_activates(self):
        # The stack is drawn from the object's shape keys, and activating a row writes the
        # object's active shape key: no image or material is involved anywhere.
        object = self.add_object_with_shape_keys()
        self.space.display_mode = 'STACK_LAYERS'
        self.space.stack_source = 'SHAPE_KEYS'
        self.space.stack_layers_view = 'STACK'
        self.redraw_window()
        self.assertEqual(self.space.stack_focus_name, "Key")
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_activate(ordinal=2), {'FINISHED'})
        self.assertEqual(object.active_shape_key_index, 2)
        self.assertEqual(object.active_shape_key.name, "Key 2")

    def test_stack_layer_focus_opens_the_stack_of_an_object(self):
        object = self.add_object_with_shape_keys()
        self.assertEqual(self.space.stack_layers_view, 'OBJECTS')
        self.focus_and_draw_stack(object)
        self.assertEqual(self.space.stack_layers_view, 'STACK')
        self.assertEqual(self.space.stack_focus_name, "Key")

    def test_stack_layer_focus_refuses_an_unknown_object(self):
        self.space.display_mode = 'STACK_LAYERS'
        with self.outliner_override():
            with self.assertRaises(RuntimeError):
                bpy.ops.outliner.stack_layer_focus(object="NoSuchObject", enter_edit_mode=False)

    def test_pin_property_flips_the_flag(self):
        object = self.add_object_with_shape_keys()
        self.focus_and_draw_stack(object)
        self.assertFalse(self.space.use_stack_layer_pin)
        self.space.use_stack_layer_pin = True
        self.assertTrue(self.space.use_stack_layer_pin)
        self.space.use_stack_layer_pin = False
        self.assertFalse(self.space.use_stack_layer_pin)

    def test_stack_layers_flags_defaults_all_shown(self):
        self.assertTrue(self.space.show_stack_layer_value)
        self.assertTrue(self.space.show_stack_layer_blend)
        self.assertTrue(self.space.show_stack_items)
        # A stack opens like a layer manager: tall rows, visibility toggle on the left.
        self.assertTrue(self.space.use_stack_layer_big_rows)
        self.assertTrue(self.space.use_stack_layer_visibility_left)

    def test_filter_panel_properties_exist(self):
        for name in (
            "show_stack_layer_value",
            "show_stack_layer_blend",
            "show_stack_items",
            "use_stack_layer_big_rows",
            "use_stack_layer_pair_sub_rows",
            "use_stack_layer_visibility_left",
            "use_stack_layer_pin",
            "stack_source",
        ):
            self.assertIn(name, bpy.types.SpaceOutliner.bl_rna.properties)

    def test_large_rows_and_columns_draw(self):
        # Both row heights draw the value column of a shape key stack without a fault.
        object = self.add_object_with_shape_keys()
        self.focus_and_draw_stack(object)
        for big_rows in (True, False):
            self.space.use_stack_layer_big_rows = big_rows
            for value_column in (True, False):
                self.space.show_stack_layer_value = value_column
                self.redraw_window()

    def test_stack_operators_are_registered(self):
        for name in (
            "stack_layer_focus",
            "stack_layers_back",
            "stack_layer_activate",
            "stack_layer_visibility_toggle",
            "stack_layer_move",
            "stack_layer_add",
            "stack_layer_remove",
            "stack_layer_drop",
        ):
            self.assertTrue(hasattr(bpy.ops.outliner, name), name)

    def test_shape_key_source_reorders(self):
        # Reorder goes through the same operator and the same ordinals as any other stack does;
        # only what happens at the bottom of the seam differs.
        object = self.add_object_with_shape_keys()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            result = bpy.ops.outliner.stack_layer_move(ordinal=1, to_ordinal=2)
        self.assertEqual(result, {'FINISHED'})
        self.assertEqual([key.name for key in object.data.shape_keys.key_blocks],
                         ["Basis", "Key 2", "Key 1"])

    def test_move_direction_steps_one_row(self):
        object = self.add_object_with_shape_keys()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            result = bpy.ops.outliner.stack_layer_move(ordinal=2, direction='DOWN')
        self.assertEqual(result, {'FINISHED'})
        self.assertEqual([key.name for key in object.data.shape_keys.key_blocks],
                         ["Basis", "Key 2", "Key 1"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
