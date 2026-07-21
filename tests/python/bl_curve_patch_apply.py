# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# ./blender.bin --background --python tests/python/bl_curve_patch_apply.py -- --verbose
#
# Also runnable from Blender's Text editor: the fixture builds its own objects and removes them
# again instead of resetting the file, because resetting it from a running script destroys the very
# text block being executed.

__all__ = (
    "main",
)

import unittest

import bpy

# Identifier of the bundled Draw brush. `Paint.brush` is read-only (brushes are assets), so this
# operator is the only way to put a known brush in the active slot.
DRAW_BRUSH_ASSET = 'brushes/essentials_brushes-mesh_sculpt.blend/Brush/Draw'


def active_object_get():
    """`bpy.context.active_object` is a screen-context member and is absent in `--background`."""
    return bpy.context.view_layer.objects.active


def active_object_set(ob):
    bpy.context.view_layer.objects.active = ob


def object_mode_ensure():
    ob = active_object_get()
    if ob is not None and ob.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')


def source_curve_object_add(name="CurvePatchSource", y_offsets=(0.0,)):
    """One three-point bezier per entry in `y_offsets`, laid out in the grid's own plane.

    Each spline runs along X at its own Y, so a patch stamped along one of them is told apart from
    a patch stamped along another by where the displaced vertices sit.
    """
    curve = bpy.data.curves.new(name, 'CURVE')
    curve.dimensions = '3D'
    for y in y_offsets:
        spline = curve.splines.new('BEZIER')
        spline.bezier_points.add(2)
        for point, x in zip(spline.bezier_points, (-1.0, 0.0, 1.0)):
            point.co = (x, y, 0.0)
            point.handle_left_type = 'AUTO'
            point.handle_right_type = 'AUTO'

    ob = bpy.data.objects.new(name, curve)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def vertex_positions(ob):
    return [v.co.copy() for v in ob.data.vertices]


class TestCurvePatchApply(unittest.TestCase):
    """`SCULPT_OT_curve_patch_apply` is the whole headless Curve Patch path in one call."""

    def setUp(self):
        # FIRST, before anything that pushes an undo step. `--background` leaves
        # `wmWindowManager::undo_stack` null (`wm_files.cc`, guarded by `!G.background`), and every
        # `*_undo_push_begin()` dereferences it without a check -- the paint-curve import below and
        # the patch's own commit both do. `ed.undo_push` is the documented way to create it on
        # demand from a script (`ed_undo.cc`, issue #60934); the second push gives the undo in
        # `test_apply_costs_exactly_one_undo_step` a state to return to.
        bpy.ops.ed.undo_push()
        bpy.ops.ed.undo_push()

        object_mode_ensure()

        # Sources a single test builds for itself, removed alongside the fixture's own.
        self.extra_names = []

        bpy.ops.mesh.primitive_grid_add(x_subdivisions=48, y_subdivisions=48, size=4.0)
        self.mesh_ob = active_object_get()
        self.source_ob = source_curve_object_add()
        active_object_set(self.mesh_ob)
        # Held by name as well: should the undo below fall back to a memfile step instead of the
        # sculpt step the patch pushed, every ID is reallocated and these references go stale.
        self.mesh_name = self.mesh_ob.name
        self.source_name = self.source_ob.name

        bpy.ops.object.mode_set(mode='SCULPT')
        self.assertEqual(
            {'FINISHED'},
            bpy.ops.brush.asset_activate(
                asset_library_type='ESSENTIALS',
                relative_asset_identifier=DRAW_BRUSH_ASSET,
            ),
        )

        brush = bpy.context.tool_settings.sculpt.brush
        self.assertIsNotNone(brush, "no active sculpt brush to stamp with")
        # Restored in tearDown: these are properties of a shared brush asset, not of the fixture.
        self.brush = brush
        self.stroke_method_orig = brush.stroke_method
        self.paint_curve_orig = brush.paint_curve
        self.source_object_orig = bpy.context.scene.tool_settings.sculpt.paint_curve_source_object

        # Assigning the source object runs the import through its own RNA update, and that import
        # accepts no stroke method other than Curve. The patch itself does not care which one is
        # set, so it is switched to Curve Patch afterwards for realism only.
        brush.stroke_method = 'CURVE'
        bpy.context.scene.tool_settings.sculpt.paint_curve_source_object = self.source_ob
        self.assertIsNotNone(brush.paint_curve, "the source curve was not imported")
        brush.stroke_method = 'CURVE_PATCH'

        # The state the undo test returns to: the scene fully built, nothing stamped yet.
        bpy.ops.ed.undo_push()

    def tearDown(self):
        # Best effort: a memfile undo would have replaced every ID these references point at, and
        # failing to restore a brush property must not mask the assertion that actually failed.
        try:
            brush = bpy.context.tool_settings.sculpt.brush
            if brush is not None:
                brush.stroke_method = self.stroke_method_orig
                brush.paint_curve = self.paint_curve_orig
            bpy.context.scene.tool_settings.sculpt.paint_curve_source_object = self.source_object_orig
        except (ReferenceError, AttributeError):
            pass

        object_mode_ensure()
        for name in [self.mesh_name, self.source_name] + self.extra_names:
            ob = bpy.data.objects.get(name)
            if ob is None:
                continue
            data = ob.data
            bpy.data.objects.remove(ob)
            if isinstance(data, bpy.types.Mesh):
                bpy.data.meshes.remove(data)
            else:
                bpy.data.curves.remove(data)

    def use_two_spline_source(self):
        """Re-import the paint curve from a source holding two splines, at Y = -1 and Y = +1.

        There is no RNA path that adds a spline to a `PaintCurve` -- `add_point()` always appends to
        the active one -- so a multi-spline curve can only come from a source object, the same route
        the fixture uses for its single-spline one.
        """
        ob = source_curve_object_add("CurvePatchSourceMulti", (-1.0, 1.0))
        self.extra_names.append(ob.name)
        # The import refuses any stroke method other than Curve; see `setUp`.
        self.brush.stroke_method = 'CURVE'
        bpy.context.scene.tool_settings.sculpt.paint_curve_source_object = ob
        self.brush.stroke_method = 'CURVE_PATCH'

        paint_curve = self.brush.paint_curve
        self.assertEqual(2, len(paint_curve.curves), "the two-spline source imported as one spline")
        # The state `apply_and_measure_y()`'s undo returns to: re-imported, nothing stamped yet.
        bpy.ops.ed.undo_push()
        return paint_curve

    def apply_and_measure_y(self, **kwargs):
        """Stamp, report where the displaced vertices sit along Y, then undo.

        Which spline was used is not observable directly -- the patch leaves no record of it -- but
        the two splines sit a unit apart, so the centroid of what moved names the spline.
        """
        before = vertex_positions(self.mesh_ob)
        self.assertEqual({'FINISHED'}, bpy.ops.sculpt.curve_patch_apply(**kwargs))
        after = vertex_positions(self.mesh_ob)
        moved = [b for a, b in zip(before, after) if (a - b).length > 1e-6]
        self.assertGreater(len(moved), 0, "the patch displaced nothing")
        mean_y = sum(v.y for v in moved) / len(moved)

        bpy.ops.ed.undo()
        # A memfile undo reallocates every ID, so the held reference may be stale from here on.
        self.mesh_ob = bpy.data.objects[self.mesh_name]
        return mean_y

    def test_multi_spline_curve_is_accepted(self):
        """Used to be refused outright with "the paint curve must hold a single spline"."""
        self.use_two_spline_source()
        self.assertEqual({'FINISHED'}, bpy.ops.sculpt.curve_patch_apply())

    def test_spline_index_selects_which_spline_is_stamped(self):
        self.use_two_spline_source()
        first = self.apply_and_measure_y(spline_index=0)
        second = self.apply_and_measure_y(spline_index=1)
        self.assertLess(first, second, "both spline indices stamped the same spline")

    def test_spline_index_is_clamped_rather_than_refused(self):
        self.use_two_spline_source()
        last = self.apply_and_measure_y(spline_index=1)
        clamped = self.apply_and_measure_y(spline_index=99)
        self.assertAlmostEqual(last, clamped, places=5)

    def test_active_curve_chooses_the_spline_by_default(self):
        paint_curve = self.use_two_spline_source()
        paint_curve.active_curve = 1
        bpy.ops.ed.undo_push()

        default = self.apply_and_measure_y()
        explicit = self.apply_and_measure_y(spline_index=1)
        self.assertAlmostEqual(default, explicit, places=5)

    def test_to_mesh_builds_one_spline_not_the_weld_of_both(self):
        """The read-back path never refused a multi-spline curve -- it tessellated the whole
        geometry at once, silently welding the splines into a single strip."""
        paint_curve = self.use_two_spline_source()
        first = paint_curve.curve_patch_to_mesh(self.brush, spline_index=0)
        second = paint_curve.curve_patch_to_mesh(self.brush, spline_index=1)
        try:
            self.assertIsNotNone(first)
            self.assertIsNotNone(second)
            self.assertEqual(len(first.vertices), len(second.vertices))

            def mean_y(mesh):
                return sum(v.co.y for v in mesh.vertices) / len(mesh.vertices)

            # A ribbon welded from both splines would sit halfway between them, so both calls would
            # report the same centroid instead of one per spline.
            self.assertLess(mean_y(first), mean_y(second))
        finally:
            bpy.data.meshes.remove(first)
            bpy.data.meshes.remove(second)

    def test_apply_displaces_vertices(self):
        before = vertex_positions(self.mesh_ob)

        self.assertEqual({'FINISHED'}, bpy.ops.sculpt.curve_patch_apply())

        after = vertex_positions(self.mesh_ob)
        self.assertEqual(len(before), len(after), "the patch must not change the vertex count")
        moved = sum(1 for a, b in zip(before, after) if (a - b).length > 1e-6)
        self.assertGreater(moved, 0, "the patch displaced nothing")

    def test_apply_costs_exactly_one_undo_step(self):
        """The failure this guards against reaches the user, not the developer: the patch's undo
        step is left parked for the operator's own `OPTYPE_UNDO` to file, so a missing flag shows up
        as a Ctrl+Z that does nothing (or one that has to be pressed twice)."""
        before = vertex_positions(self.mesh_ob)

        self.assertEqual({'FINISHED'}, bpy.ops.sculpt.curve_patch_apply())
        self.assertNotEqual(before, vertex_positions(self.mesh_ob))

        bpy.ops.ed.undo()

        restored = vertex_positions(bpy.data.objects[self.mesh_name])
        self.assertEqual(len(before), len(restored))
        for original, current in zip(before, restored):
            self.assertAlmostEqual((original - current).length, 0.0, places=5)

    def test_apply_with_an_image_texture(self):
        """Guards the one blocker that is invisible without a texture: the sampler reads image
        texels through the session's `ImagePool` (`SculptSession::tex_pool_ensure`), which a brush
        WITHOUT a texture never touches, so only a brush with one can expose a missing pool. Asserts
        that the call completes rather than what it painted -- the failure mode is a crash, not a
        magnitude.
        """
        image = bpy.data.images.new("CurvePatchTestImage", 32, 32)
        texture = bpy.data.textures.new("CurvePatchTestTexture", 'IMAGE')
        texture.image = image
        texture_orig = self.brush.texture
        self.brush.texture = texture
        try:
            before = vertex_positions(self.mesh_ob)
            self.assertEqual({'FINISHED'}, bpy.ops.sculpt.curve_patch_apply())
            self.assertEqual(len(before), len(vertex_positions(self.mesh_ob)))
        finally:
            self.brush.texture = texture_orig
            bpy.data.textures.remove(texture)
            bpy.data.images.remove(image)

    def test_apply_without_paint_curve_is_refused(self):
        self.brush.paint_curve = None
        with self.assertRaises(RuntimeError):
            bpy.ops.sculpt.curve_patch_apply()


def main():
    # `exit=True` is what turns a failure into a non-zero return code for ctest; from the Text
    # editor it would instead tear the whole session down on the first failure.
    unittest.main(exit=bpy.app.background)


if __name__ == '__main__':
    import sys
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    main()
