# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Unit tests for the pure helpers in ``brush_settings_persistence``.

Runs inside Blender, but exercises only the state-free helpers (``_collect_props`` /
``_apply_props`` / ``_coerce`` / ``_payload_writable`` / ``_valid_active`` /
``_merge_records``) so no timer,
handler or file I/O is involved -- the same pattern used by ``sculpt_mask_extract_test.py``.

blender -b --factory-startup --python tests/python/sculpt_paint/brush_settings_persistence_test.py -- \
    --startup-dir scripts/startup
"""

__all__ = (
    "main",
)

import os
import sys
import unittest

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

import brush_settings_persistence as persistence  # noqa: E402


def _make_sculpt_brush(name):
    """A minimal sculpt-mode brush, owned by the test and removed on tearDown."""
    brush = bpy.data.brushes.new(name)
    brush.use_paint_sculpt = True
    return brush


class CollectApplyRoundTripTest(unittest.TestCase):
    def setUp(self):
        self.brush = _make_sculpt_brush("BSPTest_RoundTrip")
        # Move a few well-known simple properties off their defaults so the snapshot has data.
        self.brush.size = 33
        self.brush.strength = 0.77
        self.brush.spacing = 25
        self.brush.blend = 'ADD'

    def tearDown(self):
        bpy.data.brushes.remove(self.brush)

    def test_snapshot_apply_snapshot_roundtrip(self):
        snapshot_a = persistence._collect_props(self.brush)
        self.assertTrue(snapshot_a, "expected a non-empty snapshot of a configured brush")
        # Wipe the brush, then put the snapshot back and re-collect: must be identical.
        self.brush.size = 5
        self.brush.strength = 0.05
        self.brush.spacing = 50
        self.brush.blend = 'MIX'
        persistence._apply_props(self.brush, snapshot_a)
        snapshot_b = persistence._collect_props(self.brush)
        self.assertEqual(snapshot_a, snapshot_b)

    def test_apply_props_accepts_the_same_shape_as_collect_produces(self):
        snapshot = persistence._collect_props(self.brush)
        # Applying a snapshot produced by the same helper must be idempotent.
        persistence._apply_props(self.brush, snapshot)
        persistence._apply_props(self.brush, snapshot)
        self.assertEqual(persistence._collect_props(self.brush), snapshot)


class InvalidTypesTest(unittest.TestCase):
    def setUp(self):
        self.brush = _make_sculpt_brush("BSPTest_InvalidTypes")

    def tearDown(self):
        bpy.data.brushes.remove(self.brush)

    def test_apply_props_ignores_bad_values_and_unknown_names(self):
        before = persistence._collect_props(self.brush)
        bad = {
            "strength": "not-a-number",     # a str for a float property
            "blend": 123,                    # an int for an enum property
            "not_a_real_property": 1.0,     # unknown identifier
            "size": None,                    # None for an int
        }
        # Must not raise; nothing valid should change.
        persistence._apply_props(self.brush, bad)
        after = persistence._collect_props(self.brush)
        self.assertEqual(before, after)

    def test_coerce_returns_none_when_the_value_does_not_fit(self):
        float_prop = bpy.types.Brush.bl_rna.properties["strength"]
        self.assertIsNone(persistence._coerce(float_prop, "abc"))
        self.assertIsNone(persistence._coerce(float_prop, {"x": 1}))
        # Valid values coerce cleanly.
        self.assertAlmostEqual(persistence._coerce(float_prop, 0.4), 0.4, places=6)
        int_prop = bpy.types.Brush.bl_rna.properties["size"]
        self.assertIsNone(persistence._coerce(int_prop, None))
        self.assertEqual(persistence._coerce(int_prop, 12), 12)


class CoerceStringTest(unittest.TestCase):
    def test_empty_string_is_kept(self):
        # Clearing a text setting must survive a round trip.
        prop = bpy.types.Brush.bl_rna.properties["name"]
        self.assertEqual(persistence._coerce(prop, ""), "")


class VersionHandlingTest(unittest.TestCase):
    """The pure gate the load and save paths share."""

    def test_current_version_is_readable_and_writable(self):
        self.assertTrue(persistence._payload_writable(
            {"version": persistence.FORMAT_VERSION}))

    def test_missing_version_defaults_to_old_and_is_readable(self):
        self.assertTrue(persistence._payload_writable({}))

    def test_newer_version_is_refused(self):
        self.assertFalse(persistence._payload_writable(
            {"version": persistence.FORMAT_VERSION + 1}))

    def test_non_int_version_is_refused(self):
        self.assertFalse(persistence._payload_writable({"version": "1"}))

    def test_non_dict_payload_is_refused(self):
        self.assertFalse(persistence._payload_writable(None))
        self.assertFalse(persistence._payload_writable([1, 2]))


class ActiveBrushParsingTest(unittest.TestCase):
    def test_valid_name_only_entry(self):
        self.assertEqual(
            persistence._valid_active({"name": "SculptDraw"}), {"name": "SculptDraw"})

    def test_valid_asset_entry(self):
        entry = {
            "name": "My Brush",
            "asset": {
                "type": "LOCAL",
                "identifier": "",
                "relative": "Brush/My Brush",
            },
        }
        self.assertEqual(persistence._valid_active(entry), entry)

    def test_missing_name_is_rejected(self):
        self.assertIsNone(persistence._valid_active({"asset": {}}))

    def test_non_string_relative_asset_identifier_is_rejected(self):
        entry = {"name": "X", "asset": {"type": "LOCAL", "identifier": "", "relative": ""}}
        self.assertIsNone(persistence._valid_active(entry))
        entry = {"name": "X", "asset": {"type": "LOCAL", "identifier": "", "relative": None}}
        self.assertIsNone(persistence._valid_active(entry))


class BrushRecordsTest(unittest.TestCase):
    def test_only_settings_shaped_entries_are_kept(self):
        payload = {
            "brushes": {
                "Keep": {"props": {"size": 50}, "textures": {}},
                "SkipEmpty": {"textures": {}},
                "SkipNotDict": [1, 2, 3],
            }
        }
        records = persistence._brush_records(payload)
        self.assertEqual(list(records.keys()), ["Keep"])


class MergeRecordsTest(unittest.TestCase):
    """The pure merge of this session's records into what is on disk."""

    @staticmethod
    def _state(data):
        st = persistence._State()
        st.data = dict(data)
        return st

    def test_untouched_disk_records_are_kept_and_changed_ones_replaced(self):
        st = self._state({"A": {"props": {"size": 2}}})
        persistence._mark_dirty(st, "A")
        merged = persistence._merge_records({"A": {"props": {"size": 1}}, "B": {"props": {}}}, st)
        self.assertEqual(merged, {"A": {"props": {"size": 2}}, "B": {"props": {}}})

    def test_a_reset_record_is_removed(self):
        st = self._state({})
        st.deleted_keys.add("A")
        self.assertEqual(persistence._merge_records({"A": {"props": {}}}, st), {})

    def test_a_brush_changed_after_a_reset_is_kept(self):
        st = self._state({"A": {"props": {"size": 7}}})
        st.deleted_keys.add("A")
        persistence._mark_dirty(st, "A")
        self.assertNotIn("A", st.deleted_keys)
        merged = persistence._merge_records({"A": {"props": {"size": 1}}}, st)
        self.assertEqual(merged["A"], {"props": {"size": 7}})

    def test_a_dirty_brush_unknown_to_the_session_is_dropped(self):
        st = self._state({})
        persistence._mark_dirty(st, "Gone")
        self.assertEqual(persistence._merge_records({}, st), {})
        self.assertNotIn("Gone", st.dirty_keys)


def main():
    loader = unittest.TestLoader()
    suite = loader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        sys.exit(1)


if __name__ == "__main__":
    main()
