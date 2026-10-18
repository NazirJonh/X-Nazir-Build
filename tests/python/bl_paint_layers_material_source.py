# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Material source of a layered material's rows: the RNA way to add one, the source pointer and
its cycle rule, the user count, and what a row that reads a material does not have (channel
records, a conversion to another source). These drive the real RNA, not the DNA directly."""

import sys
import unittest

import bpy


def make_material(name):
    ma = bpy.data.materials.new(name)
    ma.use_nodes = True
    return ma


class PaintLayersMaterialSourceTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_new_material_layer_reads_its_source(self):
        owner = make_material("Owner")
        source = make_material("Rust")
        users = source.users
        layer = owner.paint_layers.new(source='MATERIAL', material=source)
        self.assertEqual(layer.source, 'MATERIAL')
        self.assertEqual(layer.material, source)
        # The row goes by the name of what it reads, and it is one more user of it.
        self.assertEqual(layer.name, "Rust")
        self.assertEqual(source.users, users + 1)
        self.assertEqual(len(layer.channels), 0)

    def test_new_material_layer_needs_a_source(self):
        owner = make_material("Owner")
        with self.assertRaises(RuntimeError):
            owner.paint_layers.new(source='MATERIAL')
        self.assertEqual(len(owner.paint_layers), 0)

    def test_a_material_cannot_read_itself_or_a_material_that_reads_it(self):
        a = make_material("A")
        b = make_material("B")
        a.paint_layers.new(source='MATERIAL', material=b)
        with self.assertRaises(RuntimeError):
            a.paint_layers.new(source='MATERIAL', material=a)
        with self.assertRaises(RuntimeError):
            b.paint_layers.new(source='MATERIAL', material=a)
        self.assertEqual(len(a.paint_layers), 1)
        self.assertEqual(len(b.paint_layers), 0)

    def test_the_source_pointer_swaps_the_users_and_refuses_none(self):
        owner = make_material("Owner")
        first = make_material("First")
        second = make_material("Second")
        layer = owner.paint_layers.new(source='MATERIAL', material=first)
        first_users = first.users
        second_users = second.users
        layer.material = second
        self.assertEqual(layer.material, second)
        self.assertEqual(first.users, first_users - 1)
        self.assertEqual(second.users, second_users + 1)
        with self.assertRaises(RuntimeError):
            layer.material = None
        # The pointer's poll or its setter refuses the owner; either way it is an error.
        with self.assertRaises((RuntimeError, TypeError)):
            layer.material = owner
        self.assertEqual(layer.material, second)

    def test_removing_the_layer_lets_go_of_the_source(self):
        owner = make_material("Owner")
        source = make_material("Source")
        users = source.users
        layer = owner.paint_layers.new(source='MATERIAL', material=source)
        self.assertEqual(source.users, users + 1)
        owner.paint_layers.remove(layer)
        self.assertEqual(source.users, users)

    def test_a_material_layer_has_no_channel_record_and_keeps_its_source_kind(self):
        owner = make_material("Owner")
        source = make_material("Source")
        layer = owner.paint_layers.new(source='MATERIAL', material=source)
        with self.assertRaises(RuntimeError):
            layer.channel_add(channel='BASE_COLOR')
        self.assertEqual(len(layer.channels), 0)
        layer.source = 'IMAGE'
        self.assertEqual(layer.source, 'MATERIAL')

    def test_a_material_layer_is_not_rendered_until_the_editor_does_it(self):
        owner = make_material("Owner")
        source = make_material("Source")
        layer = owner.paint_layers.new(source='MATERIAL', material=source)
        # No window manager in a background run, so nothing renders: the row owes its maps.
        self.assertFalse(layer.bake_is_valid)
        layer.bake_mode = 'NEVER'
        self.assertEqual(layer.bake_mode, 'NEVER')
        layer.bake_request()
        self.assertFalse(layer.bake_is_valid)
        layer.bake_mode = 'ALWAYS'
        layer.bake_size = 256
        self.assertEqual(layer.bake_size, 256)

    def test_a_correction_does_not_read_a_material(self):
        owner = make_material("Owner")
        source = make_material("Source")
        layer = owner.paint_layers.new(source='IMAGE', name="Layer")
        with self.assertRaises(RuntimeError):
            layer.effect_add(source='MATERIAL')
        with self.assertRaises(RuntimeError):
            layer.mask_item_add(source='MATERIAL')
        self.assertIsNotNone(source)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
