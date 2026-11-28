# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The Mesh Maps tooling operators: pairing high-poly sources by name suffix, and the cage
preview overlay's draw handler and its `load_post` reset. These drive the real RNA and the real
handlers, not the C++ bake."""

import unittest

import bpy


def make_mesh_object(name, material):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(
        [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0), (0.0, 1.0, 0.0)],
        [],
        [(0, 1, 2, 3)],
    )
    mesh.update()
    ob = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(ob)
    ob.data.materials.append(material)
    return ob


def make_layered_material(name):
    material = bpy.data.materials.new(name)
    material.use_nodes = True
    material.paint_layers.new(source='CONSTANT', name="Fill")
    return material


class MeshMapToolsTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)
        self.material = make_layered_material("MapsMat")
        self.low = make_mesh_object("Body_low", self.material)
        self.high = make_mesh_object("Body_high", self.material)
        bpy.context.view_layer.objects.active = self.low
        self.low.select_set(True)

    def source(self):
        return self.low.mesh_map_sources.ensure(material=self.material)

    def test_match_by_name_pairs_a_low_object_with_its_high_suffix(self):
        source = self.source()
        self.assertIsNotNone(source)
        self.assertIsNone(source.high_poly)

        result = bpy.ops.object.mesh_map_source_match_by_name(
            low_suffix="_low", high_suffix="_high")
        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(source.high_poly, self.high)
        # The paired form is the object, never the collection.
        self.assertIsNone(source.high_poly_collection)

    def test_match_by_name_reports_when_nothing_matches(self):
        self.source()
        result = bpy.ops.object.mesh_map_source_match_by_name(
            low_suffix="_low", high_suffix="_missing")
        self.assertEqual(result, {'CANCELLED'})

    def test_match_by_name_replaces_a_collection_source_with_the_object_pair(self):
        source = self.source()
        collection = bpy.data.collections.new("Highs")
        bpy.context.scene.collection.children.link(collection)
        source.high_poly_collection = collection
        self.assertIsNotNone(source.high_poly_collection)

        result = bpy.ops.object.mesh_map_source_match_by_name(
            low_suffix="_low", high_suffix="_high")
        self.assertEqual(result, {'FINISHED'})
        self.assertEqual(source.high_poly, self.high)
        # The replaced collection source is cleared by the same object-XOR-collection rule.
        self.assertIsNone(source.high_poly_collection)

    def test_cage_preview_toggles_and_load_post_resets_it(self):
        from bl_operators import material_mesh_maps as ops

        source = self.source()
        self.assertIsNotNone(source)
        source.cage = make_mesh_object("Cage", self.material)

        # The cheap key reads the evaluated cage without building GPU batches; it must not raise.
        key, cages = ops._mesh_map_cage_preview_geometry_key(bpy.context)
        self.assertIsNotNone(key)
        self.assertEqual(cages, (source.cage,))

        result = bpy.ops.object.mesh_map_cage_preview()
        self.assertEqual(result, {'FINISHED'})
        self.assertTrue(ops.OBJECT_OT_mesh_map_cage_preview.is_running())

        # A new file's cages are not the previewed ones: load_post drops the overlay.
        ops._mesh_map_cage_preview_load_post(None, None)
        self.assertFalse(ops.OBJECT_OT_mesh_map_cage_preview.is_running())

        result = bpy.ops.object.mesh_map_cage_preview()
        self.assertEqual(result, {'FINISHED'})
        self.assertTrue(ops.OBJECT_OT_mesh_map_cage_preview.is_running())

        # Unregistering the module must not leave the draw handler or its hooks behind.
        ops.unregister()
        self.assertFalse(ops.OBJECT_OT_mesh_map_cage_preview.is_running())
        self.assertNotIn(ops._mesh_map_cage_preview_load_post, bpy.app.handlers.load_post)
        self.assertNotIn(ops._mesh_map_cage_preview_depsgraph_update,
                         bpy.app.handlers.depsgraph_update_post)
        ops.register()
        self.assertIn(ops._mesh_map_cage_preview_load_post, bpy.app.handlers.load_post)
        self.assertIn(ops._mesh_map_cage_preview_depsgraph_update,
                      bpy.app.handlers.depsgraph_update_post)


if __name__ == "__main__":
    unittest.main(argv=["-"] if not hasattr(bpy.app, "background") else None)
