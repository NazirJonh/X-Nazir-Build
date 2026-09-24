# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression tests for the layered-material UI: authored defaults, Principled wiring and the
per-channel Outliner columns. These drive the real RNA/operators, not the DNA directly."""

import sys
import unittest

import bpy


class PaintLayersUiTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        self.material = bpy.data.materials.new("LayeredUi")
        self.material.use_nodes = True

    def regenerate(self):
        # The scheduling point is the depsgraph update; an explicit RNA call covers the case where
        # a script's context has no scene to update.
        self.material.paint_layers.regenerate()

    def principled(self):
        return self.material.node_tree.nodes.get("Principled BSDF")

    def base_color_input_linked(self):
        principled = self.principled()
        self.assertIsNotNone(principled)
        return len(principled.inputs['Base Color'].links) > 0

    def input_linked(self, name):
        principled = self.principled()
        return len(principled.inputs[name].links) > 0

    def composite_pixel(self, channel="BASE_COLOR", size=4):
        image = bpy.data.images.new("Composite", size, size, float_buffer=True)
        result = self.material.paint_layers.composite(channel=channel, image=image)
        self.assertTrue(result)
        pixels = image.pixels
        return tuple(pixels[i] for i in range(4))

    def set_channel_map(self, layer, channel, rgba):
        record = next(c for c in layer.channels if c.channel == channel)
        image = bpy.data.images.new(channel + "Map", 4, 4, float_buffer=True)
        image.pixels = list(rgba) * 16
        record.image = image

    def set_default_maps(self, layer):
        # A map on every default channel, so each channel has a pixel size to composite into.
        self.set_channel_map(layer, 'BASE_COLOR', (0.0, 0.0, 1.0, 1.0))
        self.set_channel_map(layer, 'METALLIC', (0.0, 0.0, 0.0, 1.0))
        self.set_channel_map(layer, 'ROUGHNESS', (0.5, 0.5, 0.5, 1.0))

    def test_authored_fill_wires_the_default_channels(self):
        # A bottom Paint map gives the stack a pixel size; the Fill layers its values over it.
        bottom = self.material.paint_layers.new(source='IMAGE', name="Bottom")
        self.set_default_maps(bottom)
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        self.assertEqual(len(fill.channels), 3)
        fill.fill_color = (1.0, 0.0, 0.0, 1.0)
        self.regenerate()
        self.assertTrue(self.base_color_input_linked(), "Base Color must reach the Principled BSDF")
        # Alpha and Emission are not part of the default set: they would change the whole material.
        self.assertFalse(self.input_linked('Alpha'))
        self.assertFalse(self.input_linked('Emission Color'))
        red = self.composite_pixel()
        self.assertGreater(red[0], 0.5)
        self.assertLess(red[1], 0.2)
        # The other default channels carry the Principled defaults, not the fill colour.
        metallic = self.composite_pixel(channel="METALLIC")
        self.assertLess(metallic[0], 0.05)
        roughness = self.composite_pixel(channel="ROUGHNESS")
        self.assertAlmostEqual(roughness[0], 0.5, delta=0.02)

    def test_fill_has_a_value_per_channel(self):
        bottom = self.material.paint_layers.new(source='IMAGE', name="Bottom")
        self.set_default_maps(bottom)
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        fill.fill_color = (1.0, 0.0, 0.0, 1.0)
        rough = next(c for c in fill.channels if c.channel == 'ROUGHNESS')
        rough.value = (0.75, 0.75, 0.75, 1.0)
        self.regenerate()
        self.assertAlmostEqual(self.composite_pixel(channel="ROUGHNESS")[0], 0.75, delta=0.02)
        # The Base Color channel still reads fill_color.
        self.assertGreater(self.composite_pixel()[0], 0.5)

    def test_fill_channel_value_is_value_only(self):
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        self.regenerate()
        self.assertFalse(self.material.paint_layers_tree_is_stale)
        rough = next(c for c in fill.channels if c.channel == 'ROUGHNESS')
        rough.value = (0.75, 0.75, 0.75, 1.0)
        # A value edit must not rebuild the generated tree.
        self.assertFalse(self.material.paint_layers_tree_is_stale)

    def test_authored_paint_participates_without_changing_the_render(self):
        bottom = self.material.paint_layers.new(source='IMAGE', name="Bottom")
        self.set_default_maps(bottom)
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        fill.fill_color = (1.0, 0.0, 0.0, 1.0)
        self.regenerate()
        before = self.composite_pixel()

        paint = self.material.paint_layers.new(source='IMAGE', name="Paint")
        self.assertEqual(len(paint.channels), 3)
        self.regenerate()
        self.assertTrue(self.base_color_input_linked())
        after = self.composite_pixel()
        for i in range(3):
            self.assertAlmostEqual(before[i], after[i], delta=2.0 / 255.0)

    def test_fresh_paint_alone_does_not_wire_a_false_constant(self):
        paint = self.material.paint_layers.new(source='IMAGE', name="Paint")
        self.assertEqual(len(paint.channels), 3)
        self.regenerate()
        # The channel output exists and is wired to the Principled BSDF even before the first
        # stroke, because the row participates.
        self.assertTrue(self.base_color_input_linked())

    def test_authored_default_channels_are_enabled_without_maps(self):
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        self.assertEqual(len(fill.channels), 3)
        for record in fill.channels:
            self.assertIsNone(record.image)
            self.assertEqual(record.state, 'ENABLED')
            self.assertIn(record.channel, {'BASE_COLOR', 'METALLIC', 'ROUGHNESS'})

    def test_folder_and_correction_get_no_default_channels(self):
        folder = self.material.paint_layers.new(source='STACK', name="Folder")
        self.assertEqual(len(folder.channels), 0)
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        correction = layer.correction_add(role='EFFECT', source='IMAGE', name="C")
        self.assertEqual(len(correction.channels), 0)

    def channel_settings(self, layer, channel):
        return next(item for item in layer.channel_settings if item.channel == channel)

    def test_channel_settings_have_a_stable_path_for_every_channel(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        # Every channel has an entry, record or not, so the column and a keyframe path always exist.
        self.assertEqual(len(layer.channel_settings), 10)
        base_color = self.channel_settings(layer, 'BASE_COLOR')
        self.assertEqual(base_color.blend_type, 'INHERIT')
        self.assertEqual(base_color.opacity, 100.0)
        # Editing the pair never creates a channel record.
        self.assertEqual(len(layer.channels), 3)
        base_color.blend_type = 'MULTIPLY'
        base_color.opacity = 50.0
        self.assertEqual(base_color.blend_type, 'MULTIPLY')
        self.assertEqual(base_color.opacity, 50.0)
        self.assertEqual(len(layer.channels), 3)

    def test_channel_settings_normal_has_no_blend(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        normal = self.channel_settings(layer, 'NORMAL')
        normal.blend_type = 'MULTIPLY'
        # The setter refuses the Normal channel, which forces its own combine.
        self.assertEqual(normal.blend_type, 'INHERIT')

    def test_channel_settings_opacity_keyframe_path_resolves(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        base_color = self.channel_settings(layer, 'BASE_COLOR')
        base_color.opacity = 25.0
        self.assertTrue(base_color.keyframe_insert('opacity', frame=1))
        self.assertIsNotNone(self.material.animation_data)
        action = self.material.animation_data.action
        fcurves = []
        for layer in action.layers:
            for strip in layer.strips:
                for channelbag in strip.channelbags:
                    fcurves.extend(channelbag.fcurves)
        paths = [fcurve.data_path for fcurve in fcurves]
        self.assertTrue(
            any('channel_settings' in path and path.endswith('.opacity') for path in paths), paths)
        # Re-resolving the path finds a value again, the way playback does.
        resolved = self.material.path_resolve(paths[0])
        self.assertAlmostEqual(resolved, 25.0, places=4)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
