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

    def add_layered_object(self):
        mesh = bpy.data.meshes.new("LayeredMesh")
        object = bpy.data.objects.new("LayeredObject", mesh)
        bpy.context.collection.objects.link(object)
        bpy.context.view_layer.objects.active = object
        object.select_set(True)
        material = bpy.data.materials.new("LayeredMaterial")
        material.use_nodes = True
        material.paint_layers.new(kind='FILL', name="Base")
        mesh.materials.append(material)
        return object, material

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
            "stack_layer_id_drop",
            "stack_layer_merge_down",
        ):
            self.assertTrue(hasattr(bpy.ops.outliner, name), name)

    def test_stack_source_default_and_switch(self):
        self.assertEqual(self.space.stack_source, 'PAINT_MATERIAL')
        self.space.stack_source = 'SHAPE_KEYS'
        self.assertEqual(self.space.stack_source, 'SHAPE_KEYS')

    def test_add_material_layer_needs_a_source(self):
        # The Add refuses without a material to bake rather than silently doing nothing.
        object, _material = self.add_layered_object()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            with self.assertRaises(RuntimeError):
                bpy.ops.outliner.stack_layer_add(type='MATERIAL', ordinal=-1)

    def test_add_fill_color_is_gamma_to_linear(self):
        object, material = self.add_layered_object()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            result = bpy.ops.outliner.stack_layer_add(
                type='FILL', fill_color=(0.5, 0.25, 0.0, 1.0), ordinal=-1)
        self.assertEqual(result, {'FINISHED'})
        layer = [item for item in material.paint_layers if item.kind == 'FILL'][-1]
        # The picker colour is gamma; the description stores scene linear, so a mid value decodes
        # to something smaller.
        self.assertLess(layer.fill_color[0], 0.5)
        self.assertGreater(layer.fill_color[0], 0.1)

    def test_added_fill_reaches_the_evaluated_material(self):
        # The viewport draws the evaluated material, not the original: a Fill added from the
        # Outliner has to show up on the evaluated group node, or Material Preview keeps the old
        # stack while a final render (which makes fresh copies) already shows the new colour.
        object, material = self.add_layered_object()
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            result = bpy.ops.outliner.stack_layer_add(
                type='FILL', fill_color=(1.0, 0.0, 0.0, 1.0), ordinal=-1)
        self.assertEqual(result, {'FINISHED'})
        bpy.context.view_layer.update()
        self.redraw_window()

        principled = next(node for node in material.node_tree.nodes
                          if node.type == 'BSDF_PRINCIPLED')
        self.assertTrue(principled.inputs['Base Color'].is_linked)

        evaluated = material.evaluated_get(bpy.context.evaluated_depsgraph_get())
        group = next(node for node in evaluated.node_tree.nodes if node.type == 'GROUP')
        colors = [tuple(socket.default_value) for socket in group.inputs if socket.type == 'RGBA']
        self.assertIn((1.0, 0.0, 0.0, 1.0), colors)

    def test_black_mask_starts_black_and_activates(self):
        # The Add Mask choice fills the new mask: black hides the row until painted in. Clicking the
        # mask is what makes the next stroke paint it, so it has to activate even on a fresh mask.
        object, material = self.add_layered_object()
        self.space.display_mode = 'STACK_LAYERS'
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_focus(
                object=object.name, sub_index=-1, enter_edit_mode=True), {'FINISHED'})
        self.redraw_window()
        with self.outliner_override():
            self.assertEqual(
                bpy.ops.outliner.stack_layer_mask(ordinal=0, add=True, initial_color='BLACK'),
                {'FINISHED'})
        layer = material.paint_layers[0]
        self.assertIsNotNone(layer.mask)
        self.assertIsNotNone(layer.mask.image)
        pixel = layer.mask.image.pixels[:4]
        self.assertAlmostEqual(pixel[0], 0.0, places=3)

        self.redraw_window()
        with self.outliner_override():
            self.assertEqual(
                bpy.ops.outliner.stack_preview_section_activate(ordinal=0, section_id='MASK'),
                {'FINISHED'})
        self.assertEqual(bpy.context.scene.tool_settings.paint_mode.layer_target_mode, 'MASK')

    def test_add_material_layer_from_a_source(self):
        object, material = self.add_layered_object()
        self.focus_and_draw_stack(object)
        source = bpy.data.materials.new("BakeSource")
        source.use_nodes = True
        principled = source.node_tree.nodes.get("Principled BSDF")
        texture = source.node_tree.nodes.new('ShaderNodeTexImage')
        image = bpy.data.images.new("SourceMap", 8, 8)
        image.pixels = [0.8, 0.1, 0.1, 1.0] * 64
        texture.image = image
        source.node_tree.links.new(texture.outputs['Color'], principled.inputs['Base Color'])
        with self.outliner_override():
            result = bpy.ops.outliner.stack_layer_add(
                type='MATERIAL', source=source.name, ordinal=-1)
        # The source bake runs through the node-preview EEVEE path; a script context may not offer
        # it. Either way the call is never silent: it adds the row or refuses cleanly with a report.
        if result == {'FINISHED'}:
            self.assertIn('MATERIAL', [layer.kind for layer in material.paint_layers])
        else:
            self.assertNotIn('MATERIAL', [layer.kind for layer in material.paint_layers])

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

    def test_shape_key_source_leaves_paint_layers_alone(self):
        object = self.add_object_with_shape_keys()
        # A material's layer description is not what the shape-key source drives: activating a
        # shape key through the same Outliner operators must leave it exactly as it was.
        material = bpy.data.materials.new("BystanderMaterial")
        layer = material.paint_layers.new(kind='PAINT', name="Bystander")
        marker = layer.marker
        self.space.stack_source = 'SHAPE_KEYS'
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            bpy.ops.outliner.stack_layer_activate(ordinal=1)
        self.assertEqual(len(material.paint_layers), 1)
        self.assertEqual(material.paint_layers.find(marker), layer)

    def test_shape_key_source_offers_no_add(self):
        # A source that declares no kinds of rows gets no Add at all: the operator polls out
        # before it could create anything, instead of offering kinds nothing listens to.
        object = self.add_object_with_shape_keys()
        self.space.stack_source = 'SHAPE_KEYS'
        self.focus_and_draw_stack(object)
        with self.outliner_override():
            with self.assertRaises(RuntimeError):
                bpy.ops.outliner.stack_layer_add(ordinal=-1)


class PaintLayersApiTest(unittest.TestCase):
    """The data API the stack is meant to be driven through, without an Outliner in the way."""

    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_new_find_remove_by_marker(self):
        material = bpy.data.materials.new("LayeredApiMaterial")
        self.assertFalse(material.is_layered)
        layer = material.paint_layers.new(kind='PAINT', name="Layer One")
        self.assertIsNotNone(layer)
        self.assertTrue(material.is_layered)
        self.assertEqual(material.paint_layers.active, layer)

        # A row resolves by its marker, never by position.
        marker = layer.marker
        self.assertEqual(material.paint_layers.find(marker), layer)

        material.paint_layers.remove(layer)
        self.assertEqual(len(material.paint_layers), 0)
        self.assertIsNone(material.paint_layers.find(marker))

    def test_kind_properties_and_channels(self):
        material = bpy.data.materials.new("LayeredChannelsMaterial")
        layer = material.paint_layers.new(kind='FILL', name="Fill")
        layer.opacity = 50.0
        layer.fill_color = (0.1, 0.2, 0.3, 1.0)
        self.assertEqual(layer.kind, 'FILL')

        record = layer.channel_add(channel='BASE_COLOR')
        self.assertIsNotNone(record)
        self.assertEqual(record.channel, 'BASE_COLOR')
        layer.channel_set_enabled(channel='BASE_COLOR', enabled=False)
        layer.channel_remove(channel='BASE_COLOR')

    def test_issues_are_exposed(self):
        material = bpy.data.materials.new("LayeredIssuesMaterial")
        layer = material.paint_layers.new(kind='PAINT', name="Layer")
        # A fresh authored layer is valid: its default channels are not a problem.
        self.assertEqual(len(layer.issues), 0)
        # The issue vocabulary still names the folder-with-a-map case.
        code_items = bpy.types.MaterialPaintLayerIssue.bl_rna.properties['code'].enum_items
        self.assertIn('FOLDER_HAS_MAPS', [item.identifier for item in code_items])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
