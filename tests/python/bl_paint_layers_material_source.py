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

    def test_the_source_pointer_swaps_the_users_and_clears(self):
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
        # Emptying the slot is a real state: the row stays a Material row and reads nothing, and
        # the source's user count drops with the reference. Clearing an empty slot is a no-op.
        layer.material = None
        self.assertIsNone(layer.material)
        self.assertEqual(second.users, second_users)
        self.assertEqual(layer.source, 'MATERIAL')
        layer.material = None
        self.assertIsNone(layer.material)
        # The pointer's poll or its setter refuses the owner; either way it is an error.
        with self.assertRaises((RuntimeError, TypeError)):
            layer.material = owner
        self.assertIsNone(layer.material)

    def test_the_source_row_picker_reads_rows_added_after_it_was_opened(self):
        # The picker's enum items callback is re-run at every collection; a row added to the source
        # material between collections must be in the list. The target is the identities the
        # operator carries, not a snapshot of the rows.
        from bl_operators import material_paint_layers as operators

        owner = make_material("Owner")
        source = make_material("Source")
        first = source.paint_layers.new(source='IMAGE', name="First")
        row = owner.paint_layers.new(source='MATERIAL', material=source)
        row.material_scope = 'ROW'
        row.material_row_marker = first.marker

        # The operator is named by plain attributes here: the callback only reads what the popup
        # copies onto the operator, so a stand-in with the same fields is a faithful caller.
        class FakeOperator:
            owner_name = owner.name
            owner_library = ""
            owner_uid = owner.session_uid
            owner_marker = row.marker
            source_name = source.name
            source_library = ""
            source_uid = source.session_uid

        items = operators._mat_source_row_items(FakeOperator(), None)
        self.assertEqual([item[1] for item in items], ["First"])

        second = source.paint_layers.new(source='IMAGE', name="Second")
        items = operators._mat_source_row_items(FakeOperator(), None)
        # The order is the stack's; the new row has to be in it either way.
        self.assertEqual(sorted(item[1] for item in items), ["First", "Second"])
        self.assertIn(first.marker, [item[0] for item in items])
        self.assertIn(second.marker, [item[0] for item in items])

    def test_the_source_row_picker_still_offers_the_stack_after_the_row_is_unlinked(self):
        # The owner row is the live truth while it resolves; once its source is cleared, the source
        # identity the operator captured still names the stack the popup was opened for.
        from bl_operators import material_paint_layers as operators

        owner = make_material("Owner")
        source = make_material("Source")
        source.paint_layers.new(source='IMAGE', name="Kept")
        row = owner.paint_layers.new(source='MATERIAL', material=source)
        row.material_scope = 'ROW'

        class FakeOperator:
            owner_name = owner.name
            owner_library = ""
            owner_uid = owner.session_uid
            owner_marker = row.marker
            source_name = source.name
            source_library = ""
            source_uid = source.session_uid

        row.material = None
        items = operators._mat_source_row_items(FakeOperator(), None)
        self.assertEqual([item[1] for item in items], ["Kept"])

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

    def test_a_source_of_plain_values_needs_no_render(self):
        owner = make_material("Owner")
        source = make_material("Source")
        layer = owner.paint_layers.new(source='MATERIAL', material=source)
        # The default node tree is a Principled BSDF with plain values: shown as they are. A fresh
        # row has no bake structure yet, and "valid" means a stored bake that still matches its row
        # (BKE_paint_layers_bake_is_valid returns false without one), so what says this source needs
        # no render is the status, not validity. Asking for a bake makes the row current at once
        # (#bake_request below), which is the same statement with a bake to check.
        self.assertEqual(layer.source_status, 'OK')
        self.assertFalse(layer.bake_is_valid)
        layer.bake_mode = 'NEVER'
        self.assertEqual(layer.bake_mode, 'NEVER')
        layer.bake_request()
        self.assertTrue(layer.bake_is_valid)
        layer.bake_mode = 'ALWAYS'
        layer.bake_size = 256
        self.assertEqual(layer.bake_size, 256)

    def test_a_source_graph_is_rendered_by_the_editor(self):
        owner = make_material("Owner")
        source = make_material("Source")
        nodes = source.node_tree.nodes
        principled = next(n for n in nodes if n.type == 'BSDF_PRINCIPLED')
        noise = nodes.new('ShaderNodeTexNoise')
        source.node_tree.links.new(noise.outputs['Color'], principled.inputs['Base Color'])
        layer = owner.paint_layers.new(source='MATERIAL', material=source)
        # No window manager in a background run, so nothing renders: the row owes its map.
        self.assertFalse(layer.bake_is_valid)
        layer.bake_mode = 'NEVER'
        self.assertFalse(layer.bake_is_valid)

    def test_the_clear_operator_empties_the_targeted_slot(self):
        # The Clear button edits the row named by the context pointer (a correction or a mask item
        # hangs off a row and is not the stack's active row). The poll gates on a real material, and
        # execute lays the slot empty through RNA.
        owner = make_material("Owner")
        source = make_material("Source")
        row = owner.paint_layers.new(source='MATERIAL', material=source)
        self.assertEqual(row.material, source)

        with bpy.context.temp_override(material_paint_layer_source_target=row):
            self.assertTrue(bpy.ops.material.paint_layer_source_material_clear.poll())
            self.assertEqual(bpy.ops.material.paint_layer_source_material_clear(), {'FINISHED'})
        self.assertIsNone(row.material)
        self.assertEqual(row.source, 'MATERIAL')
        # An empty slot offers nothing to clear.
        with bpy.context.temp_override(material_paint_layer_source_target=row):
            self.assertFalse(bpy.ops.material.paint_layer_source_material_clear.poll())

    def test_clearing_a_row_in_row_scope_reads_as_no_source_not_gone(self):
        # A ROW-scope row with an empty slot is the legal "no source chosen" state the Clear button
        # leaves; only a marker that no longer resolves against a real material is "gone".
        owner = make_material("Owner")
        source = make_material("Source")
        first = source.paint_layers.new(source='IMAGE', name="First")
        row = owner.paint_layers.new(source='MATERIAL', material=source)
        row.material_scope = 'ROW'
        row.material_row_marker = first.marker
        self.assertEqual(row.source_status, 'OK')

        row.material = None
        self.assertEqual(row.material, None)
        # No material means nothing to compare: the status is OK, and the UI's own "no source" state
        # takes over (the panel checks layer.material is None).
        self.assertEqual(row.source_status, 'OK')

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
