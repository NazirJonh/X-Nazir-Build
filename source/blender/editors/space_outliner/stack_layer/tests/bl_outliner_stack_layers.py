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

    def test_display_mode_roundtrip(self):
        self.space.display_mode = 'STACK_LAYERS'
        self.assertEqual(self.space.display_mode, 'STACK_LAYERS')
        self.space.display_mode = 'VIEW_LAYER'
        self.assertEqual(self.space.display_mode, 'VIEW_LAYER')

    def test_stack_source_default_is_shape_keys(self):
        # Shape keys are the one stack source of this mode so far, and the one a space starts at.
        self.assertEqual(self.space.stack_source, 'SHAPE_KEYS')
        items = [item.identifier for item in
                 bpy.types.SpaceOutliner.bl_rna.properties['stack_source'].enum_items]
        self.assertEqual(items, ['SHAPE_KEYS'])

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
        self.space.stack_layers_view = 'STACK'
        self.redraw_window()
        self.assertEqual(self.space.stack_focus_name, "Key")
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_activate(ordinal=2), {'FINISHED'})
        self.assertEqual(object.active_shape_key_index, 2)
        self.assertEqual(object.active_shape_key.name, "Key 2")

    def test_stack_operators_are_registered(self):
        for name in (
            "stack_layers_back",
            "stack_layer_activate",
        ):
            self.assertTrue(hasattr(bpy.ops.outliner, name), name)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
