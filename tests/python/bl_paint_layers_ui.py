# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Regression tests for the layered material's global paint channel set: the RNA round trip, the
per-channel filter and the thin channel operators. These drive the real RNA/operators, not the DNA
directly."""

import sys
import unittest
from types import SimpleNamespace

import bpy


class PaintLayersUiTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        self.material = bpy.data.materials.new("LayeredUi")
        self.material.use_nodes = True

    def channel_record(self, layer, channel):
        return next(c for c in layer.channels if c.channel == channel)

    def test_authored_default_channels_are_enabled_without_maps(self):
        fill = self.material.paint_layers.new(source='CONSTANT', name="Fill")
        self.assertEqual(len(fill.channels), 5)
        for record in fill.channels:
            self.assertIsNone(record.image)
            self.assertEqual(record.state, 'ENABLED')
            self.assertIn(record.channel, {'BASE_COLOR', 'METALLIC', 'ROUGHNESS', 'NORMAL', 'AO'})

    def test_material_channel_set_round_trip(self):
        self.material.paint_layers.new(source='IMAGE', name="Layer")
        # The build default is the effective set until one is authored.
        self.assertEqual(set(self.material.paint_layers_channels),
                         {'BASE_COLOR', 'METALLIC', 'ROUGHNESS', 'NORMAL', 'AO'})
        self.assertTrue(self.material.paint_layers_channel_in_set(channel='BASE_COLOR'))
        self.material.paint_layers_channels = {'METALLIC', 'ROUGHNESS'}
        # Base Color cannot be removed: it is always in the set.
        self.assertIn('BASE_COLOR', set(self.material.paint_layers_channels))
        self.assertNotIn('NORMAL', set(self.material.paint_layers_channels))
        self.assertFalse(self.material.paint_layers_channel_in_set(channel='NORMAL'))
        self.assertTrue(self.material.paint_layers_channel_in_set(channel='METALLIC'))

    def test_layer_widget_channels_follow_the_material_set(self):
        from bl_ui.properties_material_paint_layers import material_layer_visible_channels

        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        self.assertEqual(material_layer_visible_channels(self.material),
                         ['BASE_COLOR', 'METALLIC', 'ROUGHNESS', 'NORMAL', 'AO'])
        # Removing a channel from the set hides it from the widget, but the row keeps its record.
        self.material.paint_layers_channels = {'BASE_COLOR', 'METALLIC', 'ROUGHNESS'}
        self.assertNotIn('NORMAL', material_layer_visible_channels(self.material))
        self.assertIn('NORMAL', {record.channel for record in layer.channels})
        self.material.paint_layers_channels = {'BASE_COLOR', 'NORMAL'}
        self.assertIn('NORMAL', material_layer_visible_channels(self.material))

    def test_channel_out_of_set_keeps_record_and_map(self):
        paint = self.material.paint_layers.new(source='IMAGE', name="Paint")
        normal = self.channel_record(paint, 'NORMAL')
        image = bpy.data.images.new("NormalMap", 4, 4)
        normal.image = image
        self.material.paint_layers_channels = {'BASE_COLOR', 'METALLIC', 'ROUGHNESS'}
        self.assertNotIn('NORMAL', set(self.material.paint_layers_channels))
        self.assertIs(normal.image, image)
        self.assertEqual(normal.state, 'ENABLED')

    def test_channel_toggle_creates_the_record_lazily(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        self.material.paint_layers.active = layer
        self.material.paint_layers_channels = {'BASE_COLOR', 'SPECULAR'}
        self.assertNotIn('SPECULAR', {record.channel for record in layer.channels})
        with bpy.context.temp_override(material=self.material):
            result = bpy.ops.material.paint_layer_channel_toggle(
                marker=layer.marker, channel='SPECULAR', enable=True)
        self.assertEqual(result, {'FINISHED'})
        record = self.channel_record(layer, 'SPECULAR')
        self.assertEqual(record.state, 'ENABLED')

    def test_channel_toggle_disable_keeps_the_record_and_map(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        self.material.paint_layers.active = layer
        record = self.channel_record(layer, 'ROUGHNESS')
        image = bpy.data.images.new("RoughMap", 4, 4)
        record.image = image
        with bpy.context.temp_override(material=self.material):
            result = bpy.ops.material.paint_layer_channel_toggle(
                marker=layer.marker, channel='ROUGHNESS', enable=False)
        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(record.state, 'DISABLED')
        self.assertIs(record.image, image)
        with bpy.context.temp_override(material=self.material):
            bpy.ops.material.paint_layer_channel_toggle(
                marker=layer.marker, channel='ROUGHNESS', enable=True)
        self.assertEqual(record.state, 'ENABLED')
        self.assertIs(record.image, image)

    def test_channel_add_and_remove_accept_ao_emission_specular(self):
        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        for channel_id in ('AO', 'EMISSION', 'SPECULAR'):
            self.assertNotIn(channel_id, {record.channel for record in layer.channels})
            record = layer.channel_add(channel=channel_id)
            self.assertIsNotNone(record, channel_id)
            self.assertEqual(record.channel, channel_id)
            layer.channel_remove(channel=channel_id)
            self.assertNotIn(channel_id, {record.channel for record in layer.channels})

    def test_channel_vocabulary_covers_every_widget_channel(self):
        from bl_operators.material_paint_layers import _paint_channel_enum_items
        from bl_ui.properties_paint_common import _MATERIAL_PAINT_CHANNEL_UI_ORDER

        offered = {item[0] for item in _paint_channel_enum_items()}
        for channel_id in _MATERIAL_PAINT_CHANNEL_UI_ORDER:
            self.assertIn(channel_id, offered, channel_id)

    def test_add_channel_menu_offers_only_set_channels_without_a_record(self):
        from bl_operators.material_paint_layers import _paint_layer_addable_channel_items

        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        self.material.paint_layers.active = layer
        self.material.paint_layers_channels = {'BASE_COLOR', 'SPECULAR'}
        items = {item[0] for item in _paint_layer_addable_channel_items(
            SimpleNamespace(material=self.material))}
        self.assertIn('SPECULAR', items)
        self.assertNotIn('METALLIC', items)
        self.assertNotIn('EMISSION', items)

    def test_base_color_cannot_leave_the_set(self):
        self.material.paint_layers.new(source='IMAGE', name="Layer")
        self.material.paint_layers_channels = {'METALLIC'}
        self.assertEqual(set(self.material.paint_layers_channels), {'BASE_COLOR', 'METALLIC'})

    def test_heavy_hint_counts_set_records_including_disabled(self):
        from bl_ui.properties_material_paint_layers import material_layer_weight_record_count

        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        self.assertEqual(material_layer_weight_record_count(self.material, layer), 5)
        self.material.paint_layers_channels = {
            'BASE_COLOR', 'METALLIC', 'ROUGHNESS', 'NORMAL', 'AO', 'SPECULAR'}
        layer.channel_add(channel='SPECULAR')
        layer.channel_set_enabled(channel='SPECULAR', enabled=False)
        self.assertEqual(material_layer_weight_record_count(self.material, layer), 6)
        # Dropping the set to one channel leaves the records on the row but counts only that channel.
        self.material.paint_layers_channels = {'BASE_COLOR'}
        self.assertEqual(material_layer_weight_record_count(self.material, layer), 1)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
