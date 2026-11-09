# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Unit tests for the pure geometry behind the live Extract.

Runs inside Blender (the geometry helpers import ``bmesh``/numpy), so no 3D-viewport or operator
context is needed -- only the ``extract_geometry`` / ``shape_geometry`` modules of the ``sculpt_mask``
startup package, whose bpy-free functions are exercised here.

blender -b --factory-startup --python tests/python/sculpt_paint/sculpt_mask_extract_test.py -- \
    --startup-dir scripts/startup
"""

__all__ = (
    "main",
)

import os
import sys
import unittest

import bpy
import numpy as np


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

import bl_operators.sculpt_mask.extract_geometry as geometry  # noqa: E402


def _make_grid(nx, ny, name="grid"):
    """An ``nx`` x ``ny`` quad grid (one unit per cell) as a fresh Mesh."""
    verts = [(x, y, 0.0) for y in range(ny + 1) for x in range(nx + 1)]
    faces = []
    for y in range(ny):
        for x in range(nx):
            a = y * (nx + 1) + x
            b = a + 1
            c = a + (nx + 1) + 1
            d = a + (nx + 1)
            faces.append((a, b, c, d))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.update()
    return mesh


def _set_point_mask(mesh, values):
    """Add/overwrite the ``.sculpt_mask`` per-vertex mask attribute."""
    attr = mesh.attributes.get(".sculpt_mask") or mesh.attributes.new(".sculpt_mask", 'FLOAT',
                                                                      'POINT')
    data = attr.data
    for i, value in enumerate(values):
        data[i].value = value


def _set_face_hidden(mesh, flags):
    """Add/overwrite the ``.hide_poly`` per-face hide attribute."""
    attr = mesh.attributes.get(".hide_poly") or mesh.attributes.new(".hide_poly", 'BOOLEAN',
                                                                    'FACE')
    data = attr.data
    for i, flag in enumerate(flags):
        data[i].value = bool(flag)


class MaskedFacesTest(unittest.TestCase):
    def test_no_mask_returns_none(self):
        mesh = _make_grid(2, 1)
        self.assertIsNone(geometry.masked_faces(mesh))

    def test_all_masked_keeps_every_face(self):
        mesh = _make_grid(2, 1)  # 2 faces, 6 verts.
        _set_point_mask(mesh, [1.0] * 6)
        keep = geometry.masked_faces(mesh)
        self.assertTrue(np.array_equal(keep, np.ones(2, dtype=bool)))

    def test_partial_mask_drops_face_with_an_unmasked_corner(self):
        mesh = _make_grid(2, 1)
        # Grid indexing: face 0 = verts {0,1,4,3}, face 1 = {1,2,5,4}. Unmask vert 5 (only in
        # face 1) -> face 1 dropped, face 0 kept.
        _set_point_mask(mesh, [1.0, 1.0, 1.0, 1.0, 1.0, 0.0])
        keep = geometry.masked_faces(mesh)
        self.assertEqual(list(keep), [True, False])

    def test_threshold_at_exact_value_counts_as_masked(self):
        mesh = _make_grid(2, 1)
        threshold = 0.5
        _set_point_mask(mesh, [threshold] * 6)  # `mask < threshold` is strict -> all kept.
        keep = geometry.masked_faces(mesh, threshold=threshold)
        self.assertTrue(np.array_equal(keep, np.ones(2, dtype=bool)))
        # Just below the boundary -> unmasked.
        _set_point_mask(mesh, [threshold - 1e-6] * 6)
        keep = geometry.masked_faces(mesh, threshold=threshold)
        self.assertFalse(keep.any())

    def test_hidden_faces_are_never_kept(self):
        mesh = _make_grid(2, 1)
        _set_point_mask(mesh, [1.0] * 6)
        _set_face_hidden(mesh, [False, True])
        keep = geometry.masked_faces(mesh)
        self.assertEqual(list(keep), [True, False])


class GrowShrinkTest(unittest.TestCase):
    def setUp(self):
        # Two quads sharing verts 2 and 3: F0=(0,1,2,3), F1=(3,2,5,4).
        mesh = bpy.data.meshes.new("two_quads")
        verts = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (2, 0, 0), (2, 1, 0)]
        mesh.from_pydata(verts, [], [(0, 1, 2, 3), (3, 2, 5, 4)])
        mesh.update()
        self.mesh = mesh

    def tearDown(self):
        bpy.data.meshes.remove(self.mesh)

    def test_grow_adds_vertex_adjacent_faces(self):
        keep = np.array([True, False], dtype=bool)
        grown = geometry.grow_faces(self.mesh, keep, 1)
        self.assertEqual(list(grown), [True, True])

    def test_shrink_keeps_only_interior_faces(self):
        # Both faces kept, all verts shared only by kept faces -> nothing is boundary.
        keep = np.array([True, True], dtype=bool)
        self.assertEqual(list(geometry.shrink_faces(self.mesh, keep, 1)), [True, True])
        # Shrink one face against a non-kept neighbour -> would empty -> stops at original.
        keep = np.array([True, False], dtype=bool)
        self.assertEqual(list(geometry.shrink_faces(self.mesh, keep, -1)), [True, False])

    def test_grow_never_includes_hidden(self):
        _set_face_hidden(self.mesh, [False, True])
        keep = np.array([True, False], dtype=bool)
        grown = geometry.grow_faces(self.mesh, keep, 1)
        # F1 would be grown in but is hidden -> excluded.
        self.assertEqual(list(grown), [True, False])


class EdgeWeightsTest(unittest.TestCase):
    def setUp(self):
        self.mesh = _make_grid(3, 3)

    def tearDown(self):
        bpy.data.meshes.remove(self.mesh)

    def test_edge_rings_zero_yields_empty_band(self):
        keep = np.ones(len(self.mesh.polygons), dtype=bool)
        _mesh, weights = geometry.extracted_mesh(self.mesh, keep, edge_rings=0)
        bpy.data.meshes.remove(_mesh)
        self.assertEqual(weights, {})

    def test_edge_rings_three_has_full_strength_boundary(self):
        keep = np.ones(len(self.mesh.polygons), dtype=bool)
        _mesh, weights = geometry.extracted_mesh(self.mesh, keep, edge_rings=3)
        bpy.data.meshes.remove(_mesh)
        self.assertTrue(weights)
        self.assertAlmostEqual(max(weights.values()), 1.0, places=6)


class PolygroupColorTest(unittest.TestCase):
    def test_default_face_set_uses_default_hue(self):
        self.assertAlmostEqual(geometry.polygroup_hue(3, 3, 0), geometry.DEFAULT_HUE, places=9)

    def test_hue_stable_and_distinct(self):
        self.assertEqual(geometry.polygroup_hue(1, 0, 4), geometry.polygroup_hue(1, 0, 4))
        self.assertNotAlmostEqual(geometry.polygroup_hue(1, 0, 4), geometry.polygroup_hue(2, 0, 4),
                                  places=3)

    def test_distinct_seed_in_range_and_not_worse(self):
        ids = [1, 2, 3]
        avoid = [geometry.polygroup_hue(9, 0, 0)]
        seed = geometry.distinct_seed(ids, avoid)
        self.assertIn(seed, range(10))

        def min_dist(candidate):
            hues = [geometry.polygroup_hue(i, 0, candidate) for i in ids]
            return min(geometry._hue_distance(h, a) for h in hues for a in avoid)

        self.assertGreaterEqual(min_dist(seed), min_dist(0))


def main():
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        sys.exit(1)


if __name__ == "__main__":
    main()
