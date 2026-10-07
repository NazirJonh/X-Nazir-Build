# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

r"""Addressing and verb checks for the Stack Layers Outliner (refactor plan 3.6.3, 9.5).

Run through the same windowed harness as the rest of the Stack Layers suite -- the tree only
exists under a real draw -- so use `bl_stack_layers_test_runner.py` with this module's name:

    <binary> --factory-startup --no-native-pixels --no-window-frame -p 0 0 800 600 \
        --python tests/utils/bl_stack_layers_test_runner.py -- bl_paint_layers_addressing

The scenarios:
- Row addressing survives edits (3.6.3): a marker keeps naming its row after rows above it are
  added, and the Remove a call recorded -- marker first, ordinal only as the fallback it carried --
  still removes the same row when replayed, where a remembered position would have slid.
- A correction cannot be moved between owners (9.5): the move is refused rather than silently
  re-parenting the correction onto a different row's stack.
"""

import unittest

import bpy


def make_layered(name="Addressed"):
    """An object with a layered material and a paint row, the suite's starting state."""
    context = bpy.context
    ob = bpy.data.objects.new(name, bpy.data.meshes.new(name))
    context.scene.collection.objects.link(ob)
    context.view_layer.objects.active = ob
    ma = bpy.data.materials.new(name)
    ob.data.materials.append(ma)
    layer = ma.paint_layers.new(source='IMAGE', name="Bottom")
    return ob, ma, layer


class StackLayersAddressingTest(unittest.TestCase):
    def setUp(self):
        bpy.ops.wm.read_factory_settings(use_empty=True)

    def test_marker_survives_insert_above(self):
        _ob, ma, bottom = make_layered("InsertAbove")
        ma.paint_layers.new(source='IMAGE', name="Top")
        marker = bottom.marker

        # A new row anchored above the tracked row renumbers everything in between; the marker
        # still names the same row, by identity, not by position.
        added = ma.paint_layers.new(source='IMAGE', name="Added", anchor=bottom, place='ABOVE')
        self.assertIsNotNone(added)
        self.assertIsNot(added, bottom)
        self.assertIs(ma.paint_layers.find(marker), bottom)

    def test_remove_repeat_removes_the_same_row(self):
        """3.6.3: the recorded Remove, replayed after an insert above it, removes the same row."""
        _ob, ma, first = make_layered("RemoveRepeat")
        second = ma.paint_layers.new(source='IMAGE', name="Second")
        first_marker = first.marker

        # Insert above the tracked row, the way an operator repeat of an Add would land.
        ma.paint_layers.new(source='IMAGE', name="Added", anchor=first, place='ABOVE')

        # The Remove the first call recorded resolves through the marker; the replay must still
        # name `first` -- not whichever row now sits where `first` used to.
        resolved = ma.paint_layers.find(first_marker)
        self.assertIs(resolved, first)
        ma.paint_layers.remove(resolved)
        self.assertIsNone(ma.paint_layers.find(first_marker))
        # And the sibling the stale position would have named is still here, untouched.
        self.assertIsNotNone(second)
        self.assertIsNotNone(ma.paint_layers.find(second.marker))

    def test_correction_move_between_owners_refused(self):
        """9.5: a correction belongs to one row; moving it under another is refused."""
        _ob, ma, owner = make_layered("CorrectionOwner")
        other = ma.paint_layers.new(source='IMAGE', name="Other")
        correction = owner.correction_add(role='EFFECT', source='IMAGE')
        self.assertIsNotNone(correction)

        with self.assertRaises(RuntimeError):
            ma.paint_layers.move(correction, anchor=other, place='ABOVE')
        # Still resolvable, still owned by its row: nothing was silently re-parented.
        self.assertIs(ma.paint_layers.find(correction.marker), correction)


if __name__ == "__main__":
    unittest.main()
