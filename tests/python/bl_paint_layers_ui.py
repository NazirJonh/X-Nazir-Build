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


class _MockLayout:
    """Records the draw calls a widget makes, so the panel helpers run without a window."""

    def __init__(self):
        object.__setattr__(self, "calls", [])
        object.__setattr__(self, "_values", {})

    def _record(self, kind, args, kwargs):
        self.calls.append((kind, args, kwargs))
        return self

    def row(self, *args, **kwargs):
        return self._record("row", args, kwargs)

    def column(self, *args, **kwargs):
        return self._record("column", args, kwargs)

    def grid_flow(self, *args, **kwargs):
        return self._record("grid_flow", args, kwargs)

    def split(self, *args, **kwargs):
        return self._record("split", args, kwargs)

    def panel(self, *args, **kwargs):
        self._record("panel", args, kwargs)
        return self, self

    def label(self, *args, **kwargs):
        return self._record("label", args, kwargs)

    def separator(self, *args, **kwargs):
        return self._record("separator", args, kwargs)

    def operator(self, idname, *args, **kwargs):
        return self._record("operator", (idname,), kwargs)

    def prop(self, *args, **kwargs):
        return self._record("prop", args, kwargs)

    def prop_enum(self, *args, **kwargs):
        return self._record("prop_enum", args, kwargs)

    def template_ID(self, *args, **kwargs):
        return self._record("template_ID", args, kwargs)

    def template_ID_browser(self, *args, **kwargs):
        return self._record("template_ID_browser", args, kwargs)

    def template_node_socket(self, *args, **kwargs):
        return self._record("template_node_socket", args, kwargs)

    def template_material_paint_value_slider(self, *args, **kwargs):
        return self._record("value_slider", args, kwargs)

    def context_pointer_set(self, *args, **kwargs):
        return self._record("context_pointer_set", args, kwargs)

    def __setattr__(self, name, value):
        self._values[name] = value

    def operator_ids(self):
        return [call[1][0] for call in self.calls if call[0] == "operator"]


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

    def test_layer_widget_draws_a_toggle_per_visible_channel(self):
        from bl_ui.properties_material_paint_layers import draw_material_layer_channels

        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        layout = _MockLayout()
        draw_material_layer_channels(layout, None, self.material, layer)
        ids = layout.operator_ids()
        self.assertTrue(ids)
        self.assertTrue(
            all(idname == "material.paint_layer_channel_toggle" for idname in ids))
        # One toggle per channel the set shows (the five built-in channels).
        self.assertEqual(len(ids), 5)

    def test_layer_widget_hides_a_channel_out_of_the_set(self):
        from bl_ui.properties_material_paint_layers import draw_material_layer_channels

        layer = self.material.paint_layers.new(source='IMAGE', name="Layer")
        self.material.paint_layers_channels = {'BASE_COLOR', 'SPECULAR'}
        layout = _MockLayout()
        draw_material_layer_channels(layout, None, self.material, layer)
        self.assertEqual(len(layout.operator_ids()), 2)

    def test_channel_set_widget_draws_a_checkbox_per_channel(self):
        from bl_ui.properties_material_paint_layers import draw_material_channel_set

        self.material.paint_layers.new(source='IMAGE', name="Layer")
        layout = _MockLayout()
        draw_material_channel_set(layout, self.material)
        drawn = [call[1][2] for call in layout.calls if call[0] == "prop_enum"]
        self.assertEqual(drawn, ['BASE_COLOR', 'METALLIC', 'ROUGHNESS', 'NORMAL', 'ALPHA',
                                 'AO', 'EMISSION', 'SPECULAR'])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
