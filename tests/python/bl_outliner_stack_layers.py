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
        self.area = next(area for area in bpy.context.screen.areas if area.type == 'OUTLINER')
        self.region = next(region for region in self.area.regions if region.type == 'WINDOW')
        self.space = self.area.spaces.active

    def outliner_override(self):
        return bpy.context.temp_override(
            window=bpy.context.window,
            screen=bpy.context.screen,
            area=self.area,
            region=self.region,
        )

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
                enter_paint_mode=False,
            )
        self.assertEqual(result, {'FINISHED'})
        self.area.tag_redraw()
        bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)

    def test_display_mode_roundtrip(self):
        self.space.display_mode = 'STACK_LAYERS'
        self.assertEqual(self.space.display_mode, 'STACK_LAYERS')
        self.space.display_mode = 'VIEW_LAYER'
        self.assertEqual(self.space.display_mode, 'VIEW_LAYER')

    def test_stack_layers_flags_defaults_all_shown(self):
        self.assertTrue(self.space.show_stack_layer_opacity)
        self.assertTrue(self.space.show_stack_layer_blend)
        self.assertTrue(self.space.show_stack_layer_channels)
        self.assertFalse(self.space.use_stack_layer_big_rows)
        self.assertFalse(self.space.use_stack_layer_sort_by_name)

    def test_filter_panel_properties_exist(self):
        for name in (
            "show_stack_layer_opacity",
            "show_stack_layer_blend",
            "show_stack_layer_channels",
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

    def test_activate_is_one_undo_step(self):
        object, _material, image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        bpy.ops.ed.undo_push(message="Stack Layers test baseline")
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_activate(ordinal=0), {'FINISHED'})
        self.assertEqual(bpy.context.scene.tool_settings.paint_mode.channel_image_bindings[0].image, image)
        bpy.ops.ed.undo()
        self.assertIsNone(bpy.context.scene.tool_settings.paint_mode.channel_image_bindings[0].image)

    def test_clear_target_after_undo(self):
        object, _material, _image = self.add_object_with_image_material()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_activate(ordinal=0), {'FINISHED'})
        bpy.ops.ed.undo()
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_clear_target(), {'FINISHED'})
        self.assertTrue(
            all(binding.image is None for binding in bpy.context.scene.tool_settings.paint_mode.channel_image_bindings)
        )

    def test_linked_material_operators_cancel(self):
        self.skipTest("Requires a linked-library fixture; covered by the C++ editability gate.")

    def test_two_outliners_single_owner(self):
        self.skipTest("Requires two simultaneously drawn Outliner areas; covered manually in UI validation.")

    def test_no_active_object_does_not_crash(self):
        self.space.display_mode = 'STACK_LAYERS'
        self.space.stack_layers_view = 'STACK'
        bpy.context.view_layer.objects.active = None
        self.area.tag_redraw()
        bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)

    def test_material_without_nodetree_does_not_crash(self):
        object, material, _image = self.add_object_with_image_material()
        material.use_nodes = False
        self.focus_and_draw_stack(object)
        self.area.tag_redraw()
        bpy.ops.wm.redraw_timer(type='DRAW_WIN_SWAP', iterations=1)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
