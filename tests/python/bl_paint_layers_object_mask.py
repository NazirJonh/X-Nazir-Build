# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Object / UV Tiles mask of layered-material rows: the RNA round trip, the key the shader's
Attribute node resolves on the evaluated object, copies, the thin operators and the save cleanup.
These drive the real RNA/operators, not the DNA directly."""

import sys
import unittest

import bpy


def make_layered(name):
    context = bpy.context
    ob = bpy.data.objects.new(name, bpy.data.meshes.new(name))
    context.scene.collection.objects.link(ob)
    context.view_layer.objects.active = ob
    ma = bpy.data.materials.new(name)
    ma.use_nodes = True
    ob.data.materials.append(ma)
    layer = ma.paint_layers.new(source='IMAGE', name="Layer")
    return ob, ma, layer


class PaintLayersObjectMaskTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_mesh_mask_round_trip(self):
        ob, _ma, layer = make_layered("Mesh")
        layer.object_mask_type = 'MESH'
        self.assertTrue(layer.object_mask_visible_get(ob))
        layer.object_mask_visible_set(ob, False)
        self.assertFalse(layer.object_mask_visible_get(ob))
        self.assertEqual(len(ob.paint_layer_masks), 1)
        layer.object_mask_visible_set(ob, True)
        self.assertEqual(len(ob.paint_layer_masks), 0)

    def test_key_lookup_on_evaluated_object(self):
        # The exact path the shader Attribute node resolves, on the evaluated object.
        ob, _ma, layer = make_layered("Eval")
        layer.object_mask_type = 'MESH'
        layer.object_mask_visible_set(ob, False)
        key = layer.object_mask_key
        self.assertEqual(len(key), 16)
        depsgraph = bpy.context.evaluated_depsgraph_get()
        ob_eval = ob.evaluated_get(depsgraph)
        value = ob_eval.path_resolve('paint_layer_masks["{:s}"].excluded_value'.format(key))
        self.assertEqual(value, 1.0)
        layer.object_mask_visible_set(ob, True)
        depsgraph = bpy.context.evaluated_depsgraph_get()
        ob_eval = ob.evaluated_get(depsgraph)
        with self.assertRaises(ValueError):
            ob_eval.path_resolve('paint_layer_masks["{:s}"].excluded_value'.format(key))

    def test_tile_bits_float_exact(self):
        # 24 bits survive the float mirror.
        ob, _ma, layer = make_layered("Tiles")
        layer.object_mask_type = 'UV_TILES'
        layer.object_mask_tile_set(ob, 1001, False)
        layer.object_mask_tile_set(ob, 1024, False)
        rec = ob.paint_layer_masks[layer.object_mask_key]
        self.assertEqual(int(rec.excluded_tiles_value), (1 << 0) | (1 << 23))
        self.assertEqual(rec.excluded_tiles, (1 << 0) | (1 << 23))
        self.assertFalse(layer.object_mask_tile_get(ob, 1024))
        self.assertTrue(layer.object_mask_tile_get(ob, 1025))

    def test_copied_material_is_independent(self):
        ob, ma, layer = make_layered("Copy")
        layer.object_mask_type = 'MESH'
        layer.object_mask_visible_set(ob, False)
        ma_copy = ma.copy()
        ob.data.materials.append(ma_copy)
        copy_layer = ma_copy.paint_layers.find(layer.marker)
        self.assertIsNotNone(copy_layer)
        self.assertNotEqual(copy_layer.object_mask_key, layer.object_mask_key)
        self.assertTrue(copy_layer.object_mask_visible_get(ob))

    def test_tiles_cached_and_refresh(self):
        ob, _ma, layer = make_layered("Tiles")
        layer.object_mask_type = 'UV_TILES'
        self.assertEqual(len(layer.object_mask_tiles(ob)), 0)
        self.assertEqual(len(layer.object_mask_tiles(ob, refresh=True)), 0)

    def test_edit_flag_property(self):
        paint_mode = bpy.context.scene.tool_settings.paint_mode
        paint_mode.layer_object_mask_edit = True
        self.assertTrue(paint_mode.layer_object_mask_edit)
        paint_mode.layer_object_mask_edit = False
        self.assertFalse(paint_mode.layer_object_mask_edit)

    def test_toggle_operator_mesh(self):
        ob, ma, layer = make_layered("OpMesh")
        layer.object_mask_type = 'MESH'
        with bpy.context.temp_override(material=ma):
            bpy.ops.material.paint_layer_object_mask_toggle(
                object_name=ob.name, marker=layer.marker)
        self.assertFalse(layer.object_mask_visible_get(ob))

    def test_object_listed_once_with_two_slots(self):
        from bl_ui.properties_material import paint_layer_mask_objects
        ob, ma, _layer = make_layered("TwoSlots")
        ob.data.materials.append(ma)
        self.assertEqual([o.name for o in paint_layer_mask_objects(bpy.context, ma)], [ob.name])

    def test_row_deletion_drops_records_after_save_reload(self):
        import os
        import tempfile
        ob, _ma, layer = make_layered("RowDel")
        layer.object_mask_type = 'MESH'
        layer.object_mask_visible_set(ob, False)
        self.assertEqual(len(ob.paint_layer_masks), 1)
        _ma.paint_layers.remove(layer)
        with tempfile.TemporaryDirectory() as tmpdir:
            path = os.path.join(tmpdir, "rowdel.blend")
            bpy.ops.wm.save_as_mainfile(filepath=path, copy=True)
            bpy.ops.wm.open_mainfile(filepath=path)
        ob = bpy.data.objects["RowDel"]
        self.assertEqual(len(ob.paint_layer_masks), 0)

    def test_tile_out_of_range_query_stays_visible(self):
        ob, _ma, layer = make_layered("TileRange")
        layer.object_mask_type = 'UV_TILES'
        self.assertEqual(len(ob.paint_layer_masks), 0)
        # Out-of-range tiles read as visible and never create records.
        self.assertTrue(layer.object_mask_tile_get(ob, 1025))
        self.assertEqual(len(ob.paint_layer_masks), 0)

    def test_toggle_operator_prefers_session_uid(self):
        ob, ma, layer = make_layered("SessUid")
        layer.object_mask_type = 'MESH'
        with bpy.context.temp_override(material=ma):
            bpy.ops.material.paint_layer_object_mask_toggle(
                object_name=ob.name, object_session_uid=ob.session_uid, marker=layer.marker)
        self.assertFalse(layer.object_mask_visible_get(ob))

    def test_bke_setter_alone_enables_attribute(self):
        ob, _ma, layer = make_layered("BkeAlone")
        layer.object_mask_type = 'MESH'
        layer.object_mask_visible_set(ob, False)
        key = layer.object_mask_key
        depsgraph = bpy.context.evaluated_depsgraph_get()
        ob_eval = ob.evaluated_get(depsgraph)
        value = ob_eval.path_resolve('paint_layer_masks["{:s}"].excluded_value'.format(key))
        self.assertEqual(value, 1.0)

    def test_toggle_operator_unknown_marker_cancels(self):
        ob, ma, layer = make_layered("BadMarker")
        layer.object_mask_type = 'MESH'
        # An error report makes a scripted operator call raise instead of returning CANCELLED.
        with bpy.context.temp_override(material=ma):
            with self.assertRaises(RuntimeError):
                bpy.ops.material.paint_layer_object_mask_toggle(
                    object_name=ob.name, marker="00000000-0000-0000-0000-000000000001")
        self.assertTrue(layer.object_mask_visible_get(ob))
        self.assertEqual(len(ob.paint_layer_masks), 0)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []))
