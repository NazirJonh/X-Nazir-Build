# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Unit tests for the mask-protected remesh (``sculpt_masked_remesh``).

The solve itself is exercised on a bmesh built here, so no viewport or sculpt session is needed --
the same pattern as ``sculpt_mask_extract_test.py``. Only the guard, which reads the real context,
runs against an object linked into the scene in Object Mode.

blender -b --factory-startup --python tests/python/sculpt_paint/masked_remesh_test.py -- \
    --startup-dir scripts/startup
"""

__all__ = (
    "main",
)

import os
import sys
import unittest

import bmesh
import bpy


def _startup_dir_from_args_or_tree():
    """The ``scripts/startup`` directory: from ``--startup-dir`` if given, else derived from this
    file's location in the source tree."""
    if "--" in sys.argv:
        extra = sys.argv[sys.argv.index("--") + 1:]
        for i, token in enumerate(extra):
            if token == "--startup-dir" and i + 1 < len(extra):
                return extra[i + 1]
    # tests/python/sculpt_paint/<this file> -> repo root.
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    return os.path.join(root, "scripts", "startup")


_STARTUP = _startup_dir_from_args_or_tree()
if _STARTUP not in sys.path:
    sys.path.insert(0, _STARTUP)

import sculpt_masked_remesh as remesh  # noqa: E402
from sculpt_mesh_common import MASK_ATTRIBUTE  # noqa: E402


def _sphere(sections=16, rings=8, radius=1.0):
    """A UV sphere bmesh: symmetric enough that a half mask leaves a real free area."""
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=sections, v_segments=rings, radius=radius)
    bm.verts.index_update()
    bm.faces.index_update()
    return bm


def _mask_positive_x(bm):
    """Mask the +X half of `bm` (the area the remesh must not touch)."""
    mask = bm.verts.layers.float.new(MASK_ATTRIBUTE)
    for v in bm.verts:
        v[mask] = 1.0 if v.co.x > 0.0 else 0.0
    return mask


def _snapshot(bm):
    """The coordinates and the face rings the way ``_prepare`` tags them: the element that has
    index ``n`` when the solve starts is written as ``n + 1`` into the id layers, and 0 means
    "free" -- so a ring of ids is one greater than the ring of indices it was made from."""
    return ([tuple(v.co) for v in bm.verts],
            [tuple(v.index + 1 for v in f.verts) for f in bm.faces])


class ProtectedAreaTest(unittest.TestCase):
    """The masked area must come out of the solve exactly as it went in."""

    def _check_frozen(self, bm, coords, rings):
        vertex_id = bm.verts.layers.int[remesh.VERTEX_ID_LAYER]
        face_id = bm.faces.layers.int[remesh.FACE_ID_LAYER]
        protect = bm.faces.layers.int[remesh.PROTECT_LAYER]
        for v in bm.verts:
            if v[vertex_id]:
                # Tag `n` was written from the vertex that had index `n - 1` before the solve.
                self.assertEqual(tuple(v.co), coords[v[vertex_id] - 1],
                                 "a frozen vertex moved")
        frozen = 0
        for f in bm.faces:
            if f[protect]:
                frozen += 1
                self.assertEqual(tuple(v[vertex_id] for v in f.verts), rings[f[face_id] - 1],
                                 "a protected face changed its topology")
        self.assertGreater(frozen, 0, "the mask protected no face at all")

    def test_masked_area_is_bit_exact_after_the_solve(self):
        bm = _sphere()
        mask = _mask_positive_x(bm)
        coords, rings = _snapshot(bm)
        frozen = {v.index for f in bm.faces
                  if any(v[mask] > remesh.MASK_PROTECT_EPSILON for v in f.verts)
                  for v in f.verts}

        _result, info = remesh._core(bm, voxel=0.4)

        self._check_frozen(bm, coords, rings)
        self.assertEqual(info["problems"], [])
        self.assertEqual(info["protected_coordinates_exact"], True)
        self.assertEqual(info["protected_topology_exact"], True)
        self.assertEqual(info["protected_vertices"], len(frozen))
        bm.free()

    def test_the_free_area_is_actually_rebuilt(self):
        bm = _sphere()
        _mask_positive_x(bm)
        start_vertices = len(bm.verts)

        _result, info = remesh._core(bm, voxel=0.4)

        worked = any(row["split"] or row["collapsed"] for row in info["iterations"])
        self.assertTrue(worked, "the unmasked area was left untouched")
        self.assertEqual(info["final_vertices"], len(bm.verts))
        self.assertNotEqual(len(bm.verts), start_vertices,
                            "a voxel four times the edge length changed nothing")
        bm.free()


class SolveRefusalTest(unittest.TestCase):
    def test_no_mask_is_refused(self):
        bm = _sphere()
        with self.assertRaises(ValueError) as caught:
            remesh._core(bm, voxel=0.4)
        self.assertEqual(str(caught.exception), "No mask to protect")
        bm.free()

    def test_leftover_temporary_attributes_are_refused(self):
        mesh = bpy.data.meshes.new("MR_temp")
        mesh.from_pydata([(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0)], [], [(0, 1, 3, 2)])
        mesh.attributes.new(remesh.PROTECT_LAYER, 'INT', 'FACE')
        with self.assertRaises(ValueError) as caught:
            remesh.local_remesh(mesh, 0.1)
        self.assertEqual(str(caught.exception),
                         "Temporary attributes already present; nothing changed")
        bpy.data.meshes.remove(mesh)


class PublishTest(unittest.TestCase):
    def test_the_source_mesh_has_no_temporary_layers_after_one_write(self):
        bm = _sphere()
        _mask_positive_x(bm)
        mesh = bpy.data.meshes.new("MR_publish")
        try:
            bm.to_mesh(mesh)
            bm.free()
            bm, _info = remesh.local_remesh(mesh, 0.4)
            remesh._remove_temporary_layers(bm)
            bm.to_mesh(mesh)
            names = {attribute.name for attribute in mesh.attributes}
            self.assertFalse(names & set(remesh._TEMP_NAMES))
            self.assertIn(MASK_ATTRIBUTE, names)
        finally:
            bm.free()
            bpy.data.meshes.remove(mesh)


class GuardTest(unittest.TestCase):
    """The mesh-wide part of the guard, through a real context."""

    def setUp(self):
        mesh = bpy.data.meshes.new("MR_guard")
        # Two triangles sharing an edge: one masked vertex protects one face and leaves the other
        # free, which is what a partial mask has to look like to be usable.
        mesh.from_pydata([(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)], [],
                         [(0, 1, 2), (0, 2, 3)])
        mesh.update()
        mesh.remesh_voxel_size = 0.1
        self.mesh = mesh
        self.ob = bpy.data.objects.new("MR_guard", mesh)
        bpy.context.scene.collection.objects.link(self.ob)
        bpy.context.view_layer.objects.active = self.ob

    def tearDown(self):
        bpy.data.objects.remove(self.ob)
        bpy.data.meshes.remove(self.mesh)

    def _set_mask(self, values):
        attr = self.mesh.attributes.get(MASK_ATTRIBUTE) or self.mesh.attributes.new(
            MASK_ATTRIBUTE, 'FLOAT', 'POINT')
        for i, value in enumerate(values):
            attr.data[i].value = value

    def test_no_mask_is_refused(self):
        self.assertEqual(remesh._guard(bpy.context),
                         "Paint a mask first: it marks the area to protect")

    def test_full_mask_is_refused(self):
        self._set_mask([1.0, 1.0, 1.0, 1.0])
        self.assertEqual(remesh._guard(bpy.context),
                         "The mask covers the whole surface: nothing would change")

    def test_invalid_voxel_size_is_refused(self):
        self.mesh.remesh_voxel_size = 0.0
        self.assertEqual(remesh._guard(bpy.context), "The Voxel Size is not valid")

    def test_shared_mesh_is_refused(self):
        self._set_mask([0.0, 1.0, 0.0, 0.0])
        other = bpy.data.objects.new("MR_guard_share", self.mesh)
        bpy.context.scene.collection.objects.link(other)
        try:
            self.assertEqual(
                remesh._guard(bpy.context),
                "Use a single-user mesh without shape keys, Multires or Dyntopo")
        finally:
            bpy.data.objects.remove(other)

    def test_a_partial_mask_passes(self):
        # Vertex 1 only belongs to the first face, so the second face stays free.
        self._set_mask([0.0, 1.0, 0.0, 0.0])
        self.assertIsNone(remesh._guard(bpy.context))


def main():
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        sys.exit(1)


if __name__ == "__main__":
    main()
