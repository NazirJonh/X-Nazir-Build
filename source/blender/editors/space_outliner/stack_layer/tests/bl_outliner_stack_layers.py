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
        for area in self.window.screen.areas:
            area.tag_redraw()
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

    def add_plain_object(self, name):
        """An object the stack sources have nothing to show: no shape keys, no layered material."""
        mesh = bpy.data.meshes.new(name + "Mesh")
        mesh.vertices.add(1)
        object = bpy.data.objects.new(name, mesh)
        bpy.context.collection.objects.link(object)
        return object

    def add_layered_material_object(self, name):
        """An object whose first material slot holds a paint-layer material, and that material."""
        object = self.add_plain_object(name)
        material = bpy.data.materials.new(name + "Material")
        object.data.materials.append(material)
        material.paint_layers.new(name="Row")
        self.assertTrue(material.is_layered)
        return object, material

    def focus_and_pin_stack(self, object, area=None, stack_source='PAINT_MATERIAL'):
        """Focus the object's stack in an Outliner area and pin it there."""
        area = self.area if area is None else area
        space = area.spaces.active
        space.display_mode = 'STACK_LAYERS'
        space.stack_source = stack_source
        space.use_stack_layer_pin = True
        region = next(region for region in area.regions if region.type == 'WINDOW')
        with bpy.context.temp_override(
                window=self.window, screen=self.window.screen, area=area, region=region):
            self.assertEqual(
                bpy.ops.outliner.stack_layer_focus(
                    object=object.name, sub_index=-1, enter_edit_mode=False),
                {'FINISHED'})
        self.redraw_window()

    def split_outliner_area(self):
        """Split the Outliner area in two and return the new one, which carries a copy of the
        space."""
        with self.outliner_override():
            self.assertEqual(
                bpy.ops.screen.area_split(direction='VERTICAL', factor=0.5), {'FINISHED'})
        outliner_areas = [area for area in self.window.screen.areas if area.type == 'OUTLINER']
        self.assertEqual(len(outliner_areas), 2)
        # RNA wrappers are not guaranteed to keep a one-to-one Python identity, so the new area is
        # told from the original by its own address rather than by `is`.
        original_area_ptr = self.area.as_pointer()
        return next(area for area in outliner_areas if area.as_pointer() != original_area_ptr)

    def properties_area(self):
        return next(area for area in self.window.screen.areas if area.type == 'PROPERTIES')

    def layer_material_tab_available(self):
        """Whether the Properties editor offers the Layer Material tab right now.

        The available tabs are recomputed when the editor is drawn, so the window is redrawn
        first. Selecting the tab then answers the question: a tab the editor does not offer
        refuses the selection outright, and one it loses after a redraw falls back to another
        tab (see buttons_context_compute).
        """
        self.redraw_window()
        space = self.properties_area().spaces.active
        try:
            space.context = 'LAYER_MATERIAL'
        except TypeError:
            return False
        self.redraw_window()
        return space.context == 'LAYER_MATERIAL'

    def properties_context_material(self):
        """The `material` member of the Properties editor's own context: its current tab's path."""
        area = self.properties_area()
        region = next(region for region in area.regions if region.type == 'WINDOW')
        with bpy.context.temp_override(
                window=self.window, screen=self.window.screen, area=area, region=region):
            return bpy.context.material

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

    def tool_header_region(self):
        return next((region for region in self.area.regions if region.type == 'TOOL_HEADER'),
                    None)

    def test_tool_header_region_exists(self):
        # The Stack Layers controls live in a dedicated tool header rather than the main header.
        # The region belongs to the Outliner area whether or not the mode is active; the display
        # mode only shows or hides it.
        region = self.tool_header_region()
        self.assertIsNotNone(region)
        self.assertEqual(region.type, 'TOOL_HEADER')
        self.assertEqual(bpy.types.OUTLINER_HT_tool_header.bl_region_type, 'TOOL_HEADER')

    def test_tool_header_polls_only_in_stack_layers(self):
        # The tool header is drawn only in the Stack Layers display mode, so its poll answers for
        # that one mode and no other.
        with self.outliner_override():
            self.space.display_mode = 'STACK_LAYERS'
            self.assertTrue(bpy.types.OUTLINER_HT_tool_header.poll(bpy.context))
            self.space.display_mode = 'VIEW_LAYER'
            self.assertFalse(bpy.types.OUTLINER_HT_tool_header.poll(bpy.context))

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

    def test_follow_object_defaults_on_and_flips(self):
        # The view follows what the active object has by default: its stack while it has one, and the
        # object list while it has none. The switch is the user's way out of that, and Pin Stack
        # answers the same question the other way.
        self.assertTrue(self.space.use_stack_layer_follow_object)
        self.space.use_stack_layer_follow_object = False
        self.assertFalse(self.space.use_stack_layer_follow_object)
        self.space.use_stack_layer_follow_object = True
        self.assertTrue(self.space.use_stack_layer_follow_object)

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
            "use_stack_layer_visibility_left",
            "use_stack_layer_pin",
            "use_stack_layer_follow_object",
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

    def test_layer_material_tab_follows_the_active_object(self):
        # The Properties tab is built from the active object's active material slot: available
        # while that slot holds a paint-layer material, and pointing at that material.
        object, material = self.add_layered_material_object("Layered")
        bpy.context.view_layer.objects.active = object
        self.assertTrue(self.layer_material_tab_available())
        self.assertEqual(self.properties_context_material(), material)

    def test_layer_material_tab_absent_without_a_layered_material(self):
        object = self.add_plain_object("Plain")
        bpy.context.view_layer.objects.active = object
        self.assertFalse(self.layer_material_tab_available())
        self.assertIsNone(self.properties_context_material())

    def test_layer_material_tab_ignores_a_pinned_outliner(self):
        # A pinned Outliner shows another object's stack, but that display is not the Properties
        # editor's context: with no layered material on the active object the tab is absent, no
        # matter whose stack is pinned.
        pinned_object, pinned_material = self.add_layered_material_object("Pinned")
        self.focus_and_pin_stack(pinned_object)
        bpy.context.view_layer.objects.active = self.add_plain_object("Active")
        self.assertFalse(self.layer_material_tab_available())
        self.assertNotEqual(self.properties_context_material(), pinned_material)

    def test_layer_material_tab_ignores_two_pinned_outliners(self):
        # Two Outliners pinned to two different objects: whichever of them comes first in the
        # screen's area list, neither object's material may become the tab's context while the
        # active object has no layered material.
        bpy.context.view_layer.objects.active = self.add_plain_object("Active")
        first_object, first_material = self.add_layered_material_object("First")
        second_object, second_material = self.add_layered_material_object("Second")
        second_area = self.split_outliner_area()
        self.focus_and_pin_stack(first_object)
        self.focus_and_pin_stack(second_object, second_area)
        self.assertFalse(self.layer_material_tab_available())
        self.assertNotIn(self.properties_context_material(), (first_material, second_material))
        # The same two pins, the other way around: the screen order did not decide the answer.
        self.focus_and_pin_stack(second_object)
        self.focus_and_pin_stack(first_object, second_area)
        self.assertFalse(self.layer_material_tab_available())
        self.assertNotIn(self.properties_context_material(), (first_material, second_material))

    def test_pinned_outliner_rows_operate_on_the_displayed_object(self):
        # Row actions of a pinned Outliner still belong to the object whose stack it shows, not to
        # whatever object is active: the pin's context stays local to the Outliner.
        pinned_object = self.add_object_with_shape_keys()
        self.focus_and_pin_stack(pinned_object, stack_source='SHAPE_KEYS')
        bpy.context.view_layer.objects.active = self.add_plain_object("Active")
        self.redraw_window()
        with self.outliner_override():
            self.assertEqual(bpy.ops.outliner.stack_layer_activate(ordinal=2), {'FINISHED'})
        self.assertEqual(pinned_object.active_shape_key_index, 2)
        self.assertEqual(pinned_object.active_shape_key.name, "Key 2")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
