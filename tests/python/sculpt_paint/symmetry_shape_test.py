# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Unit tests for the Shape Only symmetry transfer (``sculpt_symmetry_shape``).

Everything here runs on bmesh objects built in the test -- no viewport, sculpt session or operator
context -- the same pattern as ``sculpt_mask_extract_test.py``. The mesh used is a subdivided cube:
symmetric through the X plane with a matching vertex count on both sides, which is what the
correspondence needs, and deforming one side then exercises the propagation of a correspondence the
geometry alone would not find.

blender -b --factory-startup --python tests/python/sculpt_paint/symmetry_shape_test.py -- \
    --startup-dir scripts/startup
"""

__all__ = (
    "main",
)

import math
import os
import sys
import unittest

import bmesh
import bpy
from mathutils import Matrix, Vector


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

import sculpt_symmetry_shape as sym  # noqa: E402
from sculpt_mesh_common import MASK_ATTRIBUTE  # noqa: E402

AXIS_X = 0
SOURCE_POSITIVE = 1
SOURCE_NEGATIVE = -1
# Small enough that a side moved by 0.4 has no geometric partner left: the correspondence then
# has to come from the connectivity, which is what this module promises.
TOLERANCE = 0.15


def _symmetric_box(cuts=2, size=2.0):
    """A cube subdivided into a grid -- mirror-symmetric through the X plane."""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=size)
    bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=cuts, use_grid_fill=True)
    bm.verts.index_update()
    bm.faces.index_update()
    bm.normal_update()
    return bm


def _sculpt_positive_x(bm, offset=0.4, wiggle=0.07):
    """Push the +X side out and shift it a little, as a sculpt on one side of the plane would."""
    for v in bm.verts:
        if v.co.x > 0.5:
            v.co.x += offset
            v.co.z += wiggle
    # A real edit leaves valid normals behind; write them so the transfer's baseline tessellation
    # starts from the same state.
    bm.normal_update()


def _mask_all(bm, predicate):
    """Add the sculpt mask and set it on every vertex `predicate` accepts."""
    mask = bm.verts.layers.float.new(MASK_ATTRIBUTE)
    count = 0
    for v in bm.verts:
        if predicate(v):
            v[mask] = 1.0
            count += 1
    return mask, count


def _transfer(bm, transition_width=0.15):
    return sym.transfer_checked(bm, Matrix.Identity(4), AXIS_X, SOURCE_POSITIVE, TOLERANCE,
                                selected_only=False, transition_width=transition_width)


class TransferTest(unittest.TestCase):
    """The deformed side is copied onto the other one, and nothing else moves."""

    def test_deformed_side_is_mirrored_onto_the_other(self):
        bm = _symmetric_box()
        _sculpt_positive_x(bm)
        source = [(v.co.copy()) for v in bm.verts if v.co.x > 0.5]
        mirrored = sorted([(-p.x, p.y, p.z) for p in source])

        count, _skipped, details = _transfer(bm)

        self.assertGreater(count, 0)
        self.assertEqual(details["method"], "topology")
        destination = sorted([tuple(v.co) for v in bm.verts if v.co.x < -0.5])
        self.assertEqual(len(destination), len(mirrored))
        for got, want in zip(destination, mirrored):
            for a, b in zip(got, want):
                self.assertAlmostEqual(a, b, places=5)
        bm.free()

    def test_the_source_side_keeps_its_own_shape(self):
        bm = _symmetric_box()
        _sculpt_positive_x(bm)
        before = [(v, tuple(v.co)) for v in bm.verts if v.co.x > 0.5]

        _transfer(bm)

        for v, co in before:
            self.assertEqual(tuple(v.co), co, "a source vertex was written")
        bm.free()

    def test_a_masked_vertex_is_preserved_bit_for_bit(self):
        bm = _symmetric_box()
        _sculpt_positive_x(bm)
        mask, protected = _mask_all(bm, lambda v: v.co.x < -0.5 and v.co.y > 0.5)
        before = [(v, tuple(v.co)) for v in bm.verts if v[mask] >= 1.0]

        _count, skipped, _details = _transfer(bm)

        self.assertGreater(protected, 0)
        self.assertEqual(skipped, protected, "the masked side was not reported as skipped")
        for v, co in before:
            self.assertEqual(tuple(v.co), co, "a masked vertex moved")
        bm.free()

    def test_a_fully_masked_destination_changes_nothing(self):
        bm = _symmetric_box()
        _sculpt_positive_x(bm)
        before = [tuple(v.co) for v in bm.verts]

        _mask_all(bm, lambda v: True)
        count, skipped, details = _transfer(bm)

        self.assertEqual(count, 0)
        self.assertEqual([tuple(v.co) for v in bm.verts], before)
        self.assertGreater(skipped, 0)
        self.assertEqual(details["adapted_vertices"], 0)
        bm.free()

    def test_the_blend_band_only_shapes_collisions(self):
        # Without a fold to work around, the band cannot be felt: both widths give the same mesh.
        results = []
        for width in (0.01, 1.0):
            bm = _symmetric_box()
            _sculpt_positive_x(bm)
            _transfer(bm, transition_width=width)
            results.append(sorted(tuple(v.co) for v in bm.verts))
            bm.free()
        self.assertEqual(results[0], results[1])


class CorrespondenceTest(unittest.TestCase):
    def test_a_hole_is_refused(self):
        # A face with no partner on the other side leaves the correspondence incomplete, and an
        # incomplete correspondence must never publish a partial transfer.
        bm = _symmetric_box()
        victim = next(f for f in bm.faces if all(v.co.x < -0.5 for v in f.verts))
        bmesh.ops.delete(bm, geom=[victim], context='FACES_ONLY')
        with self.assertRaises(sym.CorrespondenceError):
            _transfer(bm)
        bm.free()

    def test_two_sides_of_different_density_are_refused(self):
        # The same shape subdivided on one side only: the faces cannot be matched ring for ring.
        bm = _symmetric_box()
        edges = [e for e in bm.edges
                 if all(v.co.x < -0.5 and abs(v.co.y) < 0.9 and abs(v.co.z) < 0.9
                        for v in e.verts)]
        bmesh.ops.subdivide_edges(bm, edges=edges, cuts=1, use_grid_fill=True)
        bm.verts.index_update()
        bm.faces.index_update()
        bm.normal_update()
        with self.assertRaises(sym.CorrespondenceError):
            _transfer(bm)
        bm.free()

    def test_a_pulled_vertex_still_gets_its_partner(self):
        # The correspondence is topological, so a vertex sculpted far out of place is still paired
        # with the vertex across the plane -- that is the point of seeding then propagating.
        bm = _symmetric_box()
        far = next(v for v in bm.verts if v.co.x > 0.5 and v.co.y > 0.5 and v.co.z > 0.5)
        far.co = Vector((5.0, 5.0, 5.0))
        bm.normal_update()
        coordinates = [v.co.copy() for v in bm.verts]
        mirror = sym.topology_pairs(bm, coordinates, TOLERANCE)
        self.assertEqual(len(mirror), len(coordinates))
        self.assertTrue(all(i >= 0 for i in mirror))
        bm.free()

    def test_pairs_are_reciprocal_and_complete(self):
        bm = _symmetric_box()
        coordinates = [v.co.copy() for v in bm.verts]
        mirror = sym.topology_pairs(bm, coordinates, TOLERANCE)
        self.assertEqual(len(mirror), len(coordinates))
        self.assertTrue(all(i >= 0 for i in mirror))
        for i, j in enumerate(mirror):
            self.assertEqual(mirror[j], i, "the correspondence is not reciprocal")
        bm.free()


class SeedDistanceTest(unittest.TestCase):
    """The seed distance is a share of the mesh's own size, not an absolute number."""

    def _cube_mesh(self, edge):
        half = edge / 2.0
        mesh = bpy.data.meshes.new("SS_seed")
        corners = [(x * half, y * half, z * half) for x in (-1, 1) for y in (-1, 1)
                   for z in (-1, 1)]
        faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4),
                 (1, 5, 7, 3)]
        mesh.from_pydata(corners, [], faces)
        mesh.update()
        return mesh

    def test_percent_scales_with_the_bounding_box(self):
        small = self._cube_mesh(1.0)
        large = self._cube_mesh(100.0)
        try:
            # A cube of edge e has a bounding-box diagonal of e * sqrt(3).
            tolerance_small = sym._seed_tolerance(small, 3.0)
            tolerance_large = sym._seed_tolerance(large, 3.0)
            self.assertAlmostEqual(tolerance_small, 3.0 * math.sqrt(3) / 100.0, places=5)
            self.assertAlmostEqual(tolerance_large / tolerance_small, 100.0, places=3)
        finally:
            bpy.data.meshes.remove(small)
            bpy.data.meshes.remove(large)

    def test_a_degenerate_mesh_still_gets_a_usable_seed(self):
        # One vertex: no extent at all. A share of it must not become a seed of zero, which would
        # read to the user as a topology problem they cannot act on.
        point = bpy.data.meshes.new("SS_point")
        point.from_pydata([(0.0, 0.0, 0.0)], [], [])
        empty = bpy.data.meshes.new("SS_empty")
        empty.from_pydata([], [], [])
        try:
            self.assertGreater(sym._seed_tolerance(point, 1.0), 0.0)
            self.assertGreater(sym._seed_tolerance(empty, 1.0), 0.0)
        finally:
            bpy.data.meshes.remove(point)
            bpy.data.meshes.remove(empty)

    def test_the_seed_comes_from_the_given_coordinates_not_from_the_mesh(self):
        # In Edit Mode `Mesh.vertices` is stale, so the operator measures the bmesh coordinates.
        # The helper must therefore depend only on the points it is handed.
        mesh = self._cube_mesh(1.0)
        bm = bmesh.new()
        try:
            bm.from_mesh(mesh)
            for v in bm.verts:
                v.co *= 100.0
            moved = sym._diagonal_tolerance([v.co for v in bm.verts], 3.0)
            self.assertAlmostEqual(moved, 3.0 * 100.0 * math.sqrt(3) / 100.0, places=3)
            self.assertAlmostEqual(sym._seed_tolerance(mesh, 3.0), 3.0 * math.sqrt(3) / 100.0,
                                   places=5)
        finally:
            bm.free()
            bpy.data.meshes.remove(mesh)


def main():
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        sys.exit(1)


if __name__ == "__main__":
    main()
