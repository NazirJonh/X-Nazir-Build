# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# ./blender.bin --background --python tests/python/bl_paint_curve_rna.py -- --verbose

import unittest

import bpy


class PaintCurveRNATest(unittest.TestCase):
    """Python access to a #PaintCurve's control points, splines and attributes."""

    def setUp(self):
        self.pc = bpy.data.paint_curves.new("TestCurve")

    def tearDown(self):
        try:
            bpy.data.paint_curves.remove(self.pc)
        except (ReferenceError, RuntimeError):
            pass

    def test_new_curve_is_empty(self):
        self.assertEqual(0, len(self.pc.points))
        self.assertEqual(0, len(self.pc.curves))

    def test_add_point_reports_index_and_grows_collections(self):
        self.assertEqual(0, self.pc.add_point((0.0, 0.0, 0.0)))
        self.assertEqual(1, self.pc.add_point((1.0, 0.0, 0.0)))
        self.assertEqual(2, self.pc.add_point((2.0, 1.0, 0.0)))
        self.assertEqual(3, len(self.pc.points))
        self.assertEqual(1, len(self.pc.curves))
        self.assertEqual(3, self.pc.curves[0].points_length)
        self.assertEqual(0, self.pc.curves[0].first_point_index)

    def test_points_keep_the_order_they_were_added_in(self):
        self.pc.add_point((0.0, 0.0, 0.0))
        self.pc.add_point((1.0, 0.0, 0.0))
        self.assertAlmostEqual(0.0, self.pc.points[0].position[0])
        self.assertAlmostEqual(1.0, self.pc.points[1].position[0])

    def test_radius_default_is_one_not_the_hair_default(self):
        # `bke::CurvesGeometry::radius()` falls back to 0.01 with no attribute, which would make a
        # Curve Patch ribbon a hundred times narrower than intended.
        self.pc.add_point((0.0, 0.0, 0.0))
        self.assertAlmostEqual(1.0, self.pc.points[0].radius)

    def test_radius_is_writable(self):
        self.pc.add_point((0.0, 0.0, 0.0), radius=0.25)
        self.assertAlmostEqual(0.25, self.pc.points[0].radius)
        self.pc.points[0].radius = 2.0
        self.assertAlmostEqual(2.0, self.pc.points[0].radius)

    def test_position_is_writable(self):
        self.pc.add_point((0.0, 0.0, 0.0))
        self.pc.add_point((1.0, 0.0, 0.0))
        self.pc.points[0].position = (0.0, 2.0, 0.0)
        self.assertAlmostEqual(2.0, self.pc.points[0].position[1])

    def test_moving_a_point_carries_its_bezier_handles(self):
        # Handle positions are absolute. Left behind at the old location they would bend the curve
        # around a point that is no longer there, and every consumer -- the Curve Patch ribbon above
        # all -- would follow the stale shape.
        self.pc.add_point((0.0, 0.0, 0.0))
        self.pc.add_point((1.0, 0.0, 0.0))
        self.pc.add_point((2.0, 0.0, 0.0))
        self.pc.points[1].position = (1.0, 5.0, 0.0)

        def distance_squared(a, b):
            return sum((a[i] - b[i]) ** 2 for i in range(3))

        handle = tuple(self.pc.attributes["handle_right"].data[1].vector)
        # The handle followed the point rather than staying at the old location.
        self.assertLess(
            distance_squared(handle, (1.0, 5.0, 0.0)),
            distance_squared(handle, (1.0, 0.0, 0.0)),
        )

    def test_added_points_get_auto_handles_off_the_point(self):
        # A handle left sitting on its own control point makes the segment straight, so a curve
        # built from a script would be a polyline no matter where its points are.
        self.pc.add_point((0.0, 0.0, 0.0))
        self.pc.add_point((1.0, 1.0, 0.0))
        self.pc.add_point((2.0, 0.0, 0.0))
        handle = tuple(self.pc.attributes["handle_right"].data[1].vector)
        point = tuple(self.pc.points[1].position)
        self.assertNotAlmostEqual(0.0, sum((handle[i] - point[i]) ** 2 for i in range(3)))

    def test_attributes_expose_the_geometry(self):
        self.pc.add_point((0.0, 0.0, 0.0))
        self.assertIn("position", self.pc.attributes)
        self.pc.attributes.new("weight", 'FLOAT', 'POINT')
        self.assertEqual(1, len(self.pc.attributes["weight"].data))
        self.pc.attributes["weight"].data[0].value = 0.5
        self.assertAlmostEqual(0.5, self.pc.attributes["weight"].data[0].value)

    def test_positions_cannot_be_removed(self):
        self.pc.add_point((0.0, 0.0, 0.0))
        with self.assertRaises(RuntimeError):
            self.pc.attributes.remove(self.pc.attributes["position"])

    def test_active_curve_is_clamped_to_the_spline_count(self):
        self.pc.add_point((0.0, 0.0, 0.0))
        self.pc.active_curve = 9999
        self.assertEqual(0, self.pc.active_curve)

    def test_clear_empties_the_curve(self):
        self.pc.add_point((0.0, 0.0, 0.0))
        self.pc.add_point((1.0, 0.0, 0.0))
        self.pc.clear()
        self.assertEqual(0, len(self.pc.points))
        self.assertEqual(0, len(self.pc.curves))

    def test_points_set_builds_the_whole_curve(self):
        self.pc.points_set((0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 2.0, 0.0, 0.0))
        self.assertEqual(3, len(self.pc.points))
        self.assertEqual(1, len(self.pc.curves))
        self.assertAlmostEqual(1.0, self.pc.points[1].position[0])
        self.assertAlmostEqual(1.0, self.pc.points[1].position[1])

    def test_points_set_defaults_the_radius_to_one(self):
        # Not 0.01: `bke::CurvesGeometry::radius()`'s hair default would make the ribbon a hundred
        # times narrower than intended.
        self.pc.points_set((0.0, 0.0, 0.0, 1.0, 0.0, 0.0))
        for point in self.pc.points:
            self.assertAlmostEqual(1.0, point.radius)

    def test_points_set_takes_a_radius_per_point(self):
        self.pc.points_set((0.0, 0.0, 0.0, 1.0, 0.0, 0.0), radii=(0.25, 0.75))
        self.assertAlmostEqual(0.25, self.pc.points[0].radius)
        self.assertAlmostEqual(0.75, self.pc.points[1].radius)

    def test_points_set_can_close_the_spline(self):
        self.pc.points_set((0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0), cyclic=True)
        self.assertTrue(self.pc.attributes["cyclic"].data[0].value)

    def test_points_set_rejects_a_mismatched_radius_count(self):
        # Refusing beats filling the gap with a default: a caller that miscounted would otherwise
        # get a curve whose tail is silently full-width.
        with self.assertRaises(RuntimeError):
            self.pc.points_set((0.0, 0.0, 0.0, 1.0, 0.0, 0.0), radii=(0.5,))

    def test_points_set_replaces_rather_than_appends(self):
        self.pc.add_point((5.0, 5.0, 5.0))
        self.pc.points_set((0.0, 0.0, 0.0, 1.0, 0.0, 0.0))
        self.assertEqual(2, len(self.pc.points))
        self.assertEqual(1, len(self.pc.curves))
        self.assertAlmostEqual(0.0, self.pc.points[0].position[0])

    def test_points_set_gives_auto_handles_off_the_point(self):
        # The one-call recompute has to do what the per-point path does; without it the curve would
        # be a polyline whatever its points say.
        self.pc.points_set((0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 2.0, 0.0, 0.0))
        handle = tuple(self.pc.attributes["handle_right"].data[1].vector)
        point = tuple(self.pc.points[1].position)
        self.assertNotAlmostEqual(0.0, sum((handle[i] - point[i]) ** 2 for i in range(3)))

    def test_position_data_reads_the_same_values_as_points(self):
        self.pc.points_set((0.0, 0.0, 0.0, 1.0, 2.0, 3.0))
        flat = [0.0] * (len(self.pc.points) * 3)
        self.pc.position_data.foreach_get("vector", flat)
        self.assertEqual([0.0, 0.0, 0.0, 1.0, 2.0, 3.0], flat)

    def test_position_data_writes_and_recomputes_handles_once(self):
        # The batch counterpart of `points[i].position`: `pyrna` runs the collection's update at the
        # end of a `foreach_set` rather than per element, so N points cost one handle recompute
        # instead of N.
        self.pc.points_set((0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 2.0, 0.0, 0.0))
        self.pc.position_data.foreach_set("vector", [0.0, 0.0, 0.0, 1.0, 5.0, 0.0, 2.0, 0.0, 0.0])
        self.assertAlmostEqual(5.0, self.pc.points[1].position[1])

        handle = tuple(self.pc.attributes["handle_right"].data[1].vector)
        point = tuple(self.pc.points[1].position)
        # An AUTO handle derived from the new neighbours, not one left at the old shape.
        self.assertNotAlmostEqual(0.0, sum((handle[i] - point[i]) ** 2 for i in range(3)))
        self.assertAlmostEqual(5.0, handle[1], places=3)

    def test_rna_edits_are_covered_by_the_global_undo(self):
        # Deliberately NOT a paint-curve undo step: RNA property writes do not push undo in Blender,
        # and a `PaintCurve` is an ID the memfile undo already covers. Guards that the write is
        # reversible at all, which is what a user actually needs.
        name = self.pc.name
        self.pc.points_set((0.0, 0.0, 0.0, 1.0, 0.0, 0.0))
        bpy.ops.ed.undo_push()

        self.pc.position_data.foreach_set("vector", [0.0, 0.0, 0.0, 1.0, 9.0, 0.0])
        self.assertAlmostEqual(9.0, self.pc.points[1].position[1])
        bpy.ops.ed.undo_push()
        bpy.ops.ed.undo()

        # A memfile undo reallocates every ID, so the curve has to be looked up again by name.
        self.pc = bpy.data.paint_curves[name]
        self.assertAlmostEqual(0.0, self.pc.points[1].position[1])

    def test_hair_curves_position_data_still_works(self):
        # `position_data` is shared with `Curves` down to the callbacks; only the update differs.
        curves = bpy.data.hair_curves.new("TestHairPositions")
        try:
            curves.add_curves((2,))
            curves.position_data.foreach_set("vector", [0.0, 0.0, 0.0, 1.0, 2.0, 3.0])
            flat = [0.0] * 6
            curves.position_data.foreach_get("vector", flat)
            self.assertEqual([0.0, 0.0, 0.0, 1.0, 2.0, 3.0], flat)
        finally:
            bpy.data.hair_curves.remove(curves)

    def test_hair_curves_still_work(self):
        # The point and spline RNA structs are shared with `Curves`; this is the regression guard
        # for that generalization.
        curves = bpy.data.hair_curves.new("TestHair")
        try:
            self.assertEqual(0, len(curves.points))
            self.assertEqual(0, len(curves.curves))
            self.assertIn("position", curves.attributes)
        finally:
            bpy.data.hair_curves.remove(curves)


class CurvePatchResultTest(unittest.TestCase):
    """Reading a Curve Patch back out as geometry, without stamping anything."""

    # `Paint.brush` is read-only (brushes are assets), so this operator is the only way to put a
    # known brush in the active slot.
    DRAW_BRUSH_ASSET = 'brushes/essentials_brushes-mesh_sculpt.blend/Brush/Draw'

    def setUp(self):
        # `--background` leaves `wmWindowManager::undo_stack` null and mode switching pushes undo
        # steps; see the note in `bl_curve_patch_apply.py`.
        bpy.ops.ed.undo_push()

        bpy.ops.mesh.primitive_grid_add(x_subdivisions=16, y_subdivisions=16, size=4.0)
        self.target = bpy.context.view_layer.objects.active

        bpy.ops.object.mode_set(mode='SCULPT')
        self.assertEqual(
            {'FINISHED'},
            bpy.ops.brush.asset_activate(
                asset_library_type='ESSENTIALS',
                relative_asset_identifier=self.DRAW_BRUSH_ASSET,
            ),
        )
        self.brush = bpy.context.tool_settings.sculpt.brush
        self.stamp_mode_orig = self.brush.curve_patch.stamp_mode

        self.pc = bpy.data.paint_curves.new("ResultCurve")
        for x in (-1.0, 0.0, 1.0):
            self.pc.add_point((x, 0.0, 0.0))

    def tearDown(self):
        try:
            self.brush.curve_patch.stamp_mode = self.stamp_mode_orig
        except (ReferenceError, AttributeError):
            pass
        if bpy.context.view_layer.objects.active is not None:
            if bpy.context.view_layer.objects.active.mode != 'OBJECT':
                bpy.ops.object.mode_set(mode='OBJECT')

    def test_to_mesh_builds_a_ribbon_with_uvs(self):
        mesh = self.pc.curve_patch_to_mesh(self.brush)
        self.assertIsNotNone(mesh)
        self.assertGreater(len(mesh.vertices), 0)
        self.assertGreater(len(mesh.polygons), 0)
        self.assertIn("UVMap", mesh.uv_layers)
        self.assertEqual(len(mesh.loops), len(mesh.uv_layers["UVMap"].data))

    def test_to_mesh_of_a_single_point_curve_is_none(self):
        pc = bpy.data.paint_curves.new("Degenerate")
        pc.add_point((0.0, 0.0, 0.0))
        self.assertIsNone(pc.curve_patch_to_mesh(self.brush))
        bpy.data.paint_curves.remove(pc)

    def test_stamps_are_none_in_ribbon_mode(self):
        self.brush.curve_patch.stamp_mode = 'RIBBON'
        self.assertIsNone(self.pc.curve_patch_stamps(self.brush))

    def test_stamps_carry_their_layout_in_stamps_mode(self):
        self.brush.curve_patch.stamp_mode = 'STAMPS'
        points = self.pc.curve_patch_stamps(self.brush)
        self.assertIsNotNone(points)
        self.assertGreater(len(points.points), 0)
        for name in ("radius", "rotation", "strength", "texture_index"):
            self.assertIn(name, points.attributes)
        # SINGLE texture source: every stamp samples the brush's own texture.
        self.assertEqual(-1, points.attributes["texture_index"].data[0].value)

    def test_target_lays_the_ribbon_onto_the_surface(self):
        # The grid sits in Z = 0, and the curve with it, so a plane-built ribbon and a
        # surface-built one differ only by the shrinkwrap -- what is checked is that passing a
        # target is accepted and still produces a ribbon over the same span.
        flat = self.pc.curve_patch_to_mesh(self.brush)
        wrapped = self.pc.curve_patch_to_mesh(self.brush, target=self.target)
        self.assertIsNotNone(wrapped)
        self.assertEqual(len(flat.vertices), len(wrapped.vertices))

    def test_curve_patch_session_is_none_outside_a_patch(self):
        # The live session is what a running modal publishes; nothing is running here, and a script
        # must be able to ask without guarding.
        self.assertIsNone(self.target.curve_patch_session)

    def test_spline_index_defaults_to_the_active_spline(self):
        # On a single-spline curve every way of naming the spline has to land on the same one.
        default = self.pc.curve_patch_to_mesh(self.brush)
        explicit = self.pc.curve_patch_to_mesh(self.brush, spline_index=0)
        clamped = self.pc.curve_patch_to_mesh(self.brush, spline_index=99)
        try:
            self.assertEqual(len(default.vertices), len(explicit.vertices))
            self.assertEqual(len(default.vertices), len(clamped.vertices))
        finally:
            for mesh in (default, explicit, clamped):
                bpy.data.meshes.remove(mesh)

    def test_radius_survives_the_spline_slice(self):
        # The build reads `radius` off the SLICED spline, not off the paint curve. A slice that
        # dropped the attribute would fall back to a uniform 1.0 and both ribbons would come out
        # the same width -- which is exactly what this asserts does not happen.
        def ribbon_width():
            mesh = self.pc.curve_patch_to_mesh(self.brush)
            try:
                return max(v.co.y for v in mesh.vertices) - min(v.co.y for v in mesh.vertices)
            finally:
                bpy.data.meshes.remove(mesh)

        for point in self.pc.points:
            point.radius = 1.0
        wide = ribbon_width()
        for point in self.pc.points:
            point.radius = 0.25
        narrow = ribbon_width()
        self.assertLess(narrow, wide)

    def test_reading_does_not_modify_the_curve(self):
        before = [tuple(p.position) for p in self.pc.points]
        self.pc.curve_patch_to_mesh(self.brush)
        after = [tuple(p.position) for p in self.pc.points]
        self.assertEqual(before, after)


def main():
    import sys
    argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    unittest.main(argv=argv)


if __name__ == "__main__":
    main()
