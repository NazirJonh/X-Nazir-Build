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


def source_curve_object_add():
    """A three-point bezier lying in the grid's own plane, to be imported as the paint curve."""
    curve = bpy.data.curves.new("CurvePatchSource", 'CURVE')
    curve.dimensions = '3D'
    spline = curve.splines.new('BEZIER')
    spline.bezier_points.add(2)
    for point, x in zip(spline.bezier_points, (-1.0, 0.0, 1.0)):
        point.co = (x, 0.0, 0.0)
        point.handle_left_type = 'AUTO'
        point.handle_right_type = 'AUTO'

    ob = bpy.data.objects.new("CurvePatchSource", curve)
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
        for name in (self.mesh_name, self.source_name):
            ob = bpy.data.objects.get(name)
            if ob is None:
                continue
            data = ob.data
            bpy.data.objects.remove(ob)
            if isinstance(data, bpy.types.Mesh):
                bpy.data.meshes.remove(data)
            else:
                bpy.data.curves.remove(data)

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
        texels through `SculptSession::tex_pool`, which only a live stroke ever creates, so a brush
        WITHOUT a texture passes either way and a brush with one dereferences null. Asserts that the
        call completes rather than what it painted -- the failure mode is a crash, not a magnitude.
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
