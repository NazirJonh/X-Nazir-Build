# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

r"""Add-on API checks for the paint-layers RNA (refactor plan 2.5.4).

An add-on builds a whole stack through ``Material.paint_layers`` alone -- no Outliner
operators -- and the result carries the same shape the UI would have built: anchored adds land
where asked, the active cursor only moves on request, every verb resolves rows by marker, and
refusals report instead of silently mis-linking.

Run through the windowed harness like the rest of the suite:

    <binary> --factory-startup --no-native-pixels --no-window-frame -p 0 0 800 600 \
        --python tests/utils/bl_stack_layers_test_runner.py -- bl_paint_layers_addon_api
"""

import unittest

import bpy


def make_layered(name="AddonApi"):
    """A layered material with nothing on it: the add-on starts from an empty stack."""
    context = bpy.context
    ob = bpy.data.objects.new(name, bpy.data.meshes.new(name))
    context.scene.collection.objects.link(ob)
    context.view_layer.objects.active = ob
    ma = bpy.data.materials.new(name)
    ob.data.materials.append(ma)
    return ob, ma


class PaintLayersAddonApiTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_anchored_add_and_marker_round_trip(self):
        _ob, ma = make_layered("AnchoredAdd")
        bottom = ma.paint_layers.new(source='IMAGE', name="Bottom")
        # Anchored adds land where asked, without touching anything else.
        top = ma.paint_layers.new(source='IMAGE', name="Top", anchor=bottom, place='ABOVE')
        self.assertIsNotNone(top)
        self.assertIs(ma.paint_layers.find(top.marker), top)
        self.assertIs(ma.paint_layers.find(bottom.marker), bottom)
        # A foreign anchor is refused, never linked under another material's row.
        _ob2, other_ma = make_layered("Foreign")
        foreign = other_ma.paint_layers.new(source='IMAGE', name="Foreign")
        with self.assertRaises(RuntimeError):
            ma.paint_layers.new(source='IMAGE', name="Bad", anchor=foreign, place='ABOVE')

    def test_make_active_leaves_the_cursor_alone_unless_asked(self):
        _ob, ma = make_layered("MakeActive")
        first = ma.paint_layers.new(source='IMAGE', name="First")
        self.assertIs(ma.paint_layers.active, first)
        second = ma.paint_layers.new(source='IMAGE', name="Second", make_active=False)
        self.assertIsNotNone(second)
        self.assertIs(ma.paint_layers.active, first)

    def test_group_merge_down_mask_toggle_through_rna_only(self):
        _ob, ma = make_layered("Verbs")
        bottom = ma.paint_layers.new(source='IMAGE', name="Bottom")
        top = ma.paint_layers.new(source='IMAGE', name="Top")
        folder = ma.paint_layers.merge_down(upper=top, lower=bottom)
        self.assertIsNotNone(folder)
        self.assertEqual(len(folder.children), 2)
        # The folder holds both rows; ungroup hands them back.
        ma.paint_layers.ungroup(folder=folder)
        self.assertIsNotNone(ma.paint_layers.find(bottom.marker))
        self.assertIsNotNone(ma.paint_layers.find(top.marker))

        # Mask add/toggle through RNA: the toggle flips the base item's flag.
        mask = bottom.mask_add(value=1.0)
        self.assertIsNotNone(mask)
        self.assertTrue(bottom.mask_stack[0].enabled)
        bottom.mask_toggle()
        self.assertFalse(bottom.mask_stack[0].enabled)
        bottom.mask_toggle()
        self.assertTrue(bottom.mask_stack[0].enabled)

        # Corrections come off through remove(); the row stays.
        correction = bottom.correction_add(role='EFFECT', source='IMAGE', name="C")
        marker = correction.marker
        ma.paint_layers.remove(correction)
        self.assertIsNone(ma.paint_layers.find(marker))
        self.assertIsNotNone(ma.paint_layers.find(bottom.marker))

    def test_reading_by_marker_active_and_lists(self):
        # 2.5.3: marker/find/active plus children/effects/mask_stack iteration.
        _ob, ma = make_layered("Reading")
        folder = ma.paint_layers.new(source='STACK', name="Folder")
        child = ma.paint_layers.new(source='IMAGE', name="Child", anchor=folder, place='INTO')
        layer = ma.paint_layers.new(source='IMAGE', name="Layer")
        effect = layer.correction_add(role='EFFECT', source='IMAGE', name="E")
        mask = layer.mask_add(value=1.0)
        self.assertIn(child, list(folder.children))
        self.assertIn(effect, list(layer.effects))
        self.assertIn(mask, list(layer.mask_stack))
        for row in (folder, child, layer, effect, mask):
            self.assertTrue(row.marker)
            self.assertIs(ma.paint_layers.find(row.marker), row)


if __name__ == "__main__":
    unittest.main()
