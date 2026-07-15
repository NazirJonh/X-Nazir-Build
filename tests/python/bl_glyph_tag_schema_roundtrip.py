# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Round-trip, validation and fuzz tests for the Category Tabs / ``glyph_tag_system``
# JSON schema layer.
#
# This test runs standalone (bpy-free): the schema-key and migration/normalization modules
# and their pure dependencies (``defaults``, ``conversions``, ``log``) import without ``bpy``.
# The persistence entry points in ``glyph_cache`` and the tag CRUD in ``tags_cache`` do import
# ``bpy`` and are therefore out of scope here (they need a Blender-hosted test); this covers the
# pure data pipeline that all of them funnel through.
#
# Registered via ``add_python_test`` in ``tests/python/CMakeLists.txt``. It is invoked with the
# bundled Python interpreter (no Blender launch):
#   <python> tests/python/bl_glyph_tag_schema_roundtrip.py --startup-dir scripts/startup

import argparse
import json
import os
import random
import sys
import unittest

# Debug helpers in the package print category glyphs (Private Use Area characters). Force UTF-8
# on stdout/stderr so those prints cannot raise ``UnicodeEncodeError`` under a non-UTF-8 console
# (e.g. Windows cp1251), which would otherwise fail the test for an unrelated reason.
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass


def _resolve_startup_dir():
    """Return the ``scripts/startup`` directory and the argv leftovers for unittest.

    ``--startup-dir`` is supplied by CMake; when run by hand it falls back to a path derived
    from this file's location so the test also works with a bare invocation.
    """
    argv = sys.argv[1:]
    # Tolerate a Blender-style ``--`` separator if the test is ever launched via ``blender``.
    if "--" in argv:
        argv = argv[argv.index("--") + 1:]
    parser = argparse.ArgumentParser()
    parser.add_argument("--startup-dir", dest="startup_dir", default=None)
    args, remaining = parser.parse_known_args(argv)

    startup_dir = args.startup_dir
    if not startup_dir:
        # tests/python/<this file> -> repository root -> scripts/startup
        repo_root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
        startup_dir = os.path.join(repo_root, "scripts", "startup")
    return startup_dir, remaining


_STARTUP_DIR, _UNITTEST_ARGV = _resolve_startup_dir()
sys.path.insert(0, os.path.join(_STARTUP_DIR, "bl_ui"))

from glyph_tag_system import schema_keys as sk  # noqa: E402
from glyph_tag_system import migrations as mig  # noqa: E402
from glyph_tag_system import defaults as dflt  # noqa: E402


class TestSchemaKeys(unittest.TestCase):
    """The SSOT key constants must equal the historical on-disk literals (zero drift)."""

    EXPECTED = {
        "KEY_VERSION": "version",
        "KEY_ALL_TAGS": "all_tags",
        "KEY_MAPPINGS": "mappings",
        "KEY_CATEGORY_ORDERS": "category_orders",
        "KEY_TAG_ORDER": "tag_order",
        "KEY_GLOBAL": "GLOBAL",
        "KEY_GLYPH": "glyph",
        "KEY_DISPLAY_NAME": "display_name",
        "KEY_FIRST_LETTER": "first_letter",
        "KEY_COLOR": "color",
        "KEY_DEFAULT_GLYPH": "default_glyph",
        "KEY_DEFAULT_DISPLAY_NAME": "default_display_name",
        "KEY_BASE_TYPE": "base_type",
        "KEY_TAGS": "tags",
        "KEY_GLYPH_MODE": "glyph_mode",
        "KEY_MODE_FLAGS": "mode_flags",
        "KEY_SOURCE_EXTENSION": "source_extension",
        "KEY_PENDING_TAG_ASSIGNMENT": "pending_tag_assignment",
        "KEY_DISCOVERED_IN_SPACES": "discovered_in_spaces",
        "KEY_DISCOVERED_IN_MODES": "discovered_in_modes",
        "KEY_INSTALL_MODE_FLAG": "install_mode_flag",
        "KEY_ICON_SOURCE": "icon_source",
        "KEY_ICON_KEY": "icon_key",
        "KEY_ICON_PATH": "icon_path",
        "KEY_ICON_PROVIDER": "icon_provider",
        "KEY_ICON": "icon",
        "ICON_BLOCK_SOURCE": "source",
        "ICON_BLOCK_KEY": "key",
        "ICON_BLOCK_PATH": "path",
        "ICON_BLOCK_PROVIDER": "provider",
    }

    def test_constants_match_literals(self):
        for name, literal in self.EXPECTED.items():
            self.assertEqual(getattr(sk, name), literal, "schema key {0} drifted".format(name))


class TestMigrateJsonData(unittest.TestCase):
    """``migrate_json_data`` guards the structure and stamps the current version."""

    def test_guards_and_normalizes(self):
        migrated = mig.migrate_json_data({
            sk.KEY_ALL_TAGS: {"Foo": {sk.KEY_GLYPH: "e866",
                                      sk.KEY_COLOR: [2.0, -1.0, "x"],
                                      sk.KEY_MODE_FLAGS: 7}},
            sk.KEY_MAPPINGS: {sk.KEY_GLOBAL: {"Cat": {sk.KEY_COLOR: [0.5, 0.5, 0.5],
                                                      sk.KEY_TAGS: ["Foo"]}}},
            sk.KEY_CATEGORY_ORDERS: {"Foo": ["Cat"], "Bad": "notalist"},
            sk.KEY_TAG_ORDER: ["Foo", 123, "Bar"],
        })
        self.assertEqual(migrated[sk.KEY_VERSION], dflt.CURRENT_JSON_VERSION)
        self.assertEqual(migrated[sk.KEY_TAG_ORDER], ["Foo", "Bar"])
        self.assertNotIn("Bad", migrated[sk.KEY_CATEGORY_ORDERS])
        self.assertEqual(migrated[sk.KEY_ALL_TAGS]["Foo"][sk.KEY_COLOR], [1.0, 0.0, 0.0])
        self.assertEqual(
            migrated[sk.KEY_MAPPINGS][sk.KEY_GLOBAL]["Cat"][sk.KEY_COLOR], [0.5, 0.5, 0.5])

    def test_non_dict_input_yields_empty_sections(self):
        for garbage in ("not a dict", None, 42, ["a", "b"]):
            out = mig.migrate_json_data(garbage)
            self.assertEqual(out[sk.KEY_ALL_TAGS], {})
            self.assertEqual(out[sk.KEY_MAPPINGS], {})
            self.assertEqual(out[sk.KEY_CATEGORY_ORDERS], {})
            self.assertEqual(out[sk.KEY_TAG_ORDER], [])
            self.assertEqual(out[sk.KEY_VERSION], dflt.CURRENT_JSON_VERSION)

    def test_wrong_typed_sections_are_reset(self):
        out = mig.migrate_json_data({
            sk.KEY_ALL_TAGS: ["not", "a", "dict"],
            sk.KEY_MAPPINGS: "nope",
            sk.KEY_CATEGORY_ORDERS: 5,
            sk.KEY_TAG_ORDER: "not a list",
        })
        self.assertEqual(out[sk.KEY_ALL_TAGS], {})
        self.assertEqual(out[sk.KEY_MAPPINGS], {})
        self.assertEqual(out[sk.KEY_CATEGORY_ORDERS], {})
        self.assertEqual(out[sk.KEY_TAG_ORDER], [])


class TestNormalizeColor(unittest.TestCase):
    """``_normalize_color`` always yields three floats clamped to [0, 1]."""

    def test_vectors(self):
        cases = {
            "good": ([0.2, 0.4, 0.6], [0.2, 0.4, 0.6]),
            "out_of_range": ([2.0, -1.0, 0.5], [1.0, 0.0, 0.5]),
            "two_channels": ([0.1, 0.2], [0.1, 0.2, 0.0]),
            "string_channel": (["x", 0.3, 0.7], [0.0, 0.3, 0.7]),
            "four_channels": ([0.1, 0.2, 0.3, 0.9], [0.1, 0.2, 0.3]),
            "none": (None, [0.0, 0.0, 0.0]),
            "string": ("abc", [0.0, 0.0, 0.0]),
            "tuple": ((0.1, 0.2, 0.3), [0.1, 0.2, 0.3]),
        }
        for name, (inp, expected) in cases.items():
            got = mig._normalize_color(inp)
            self.assertEqual(got, expected, name)
            self.assertEqual(len(got), 3)
            for channel in got:
                self.assertIsInstance(channel, float)
                self.assertGreaterEqual(channel, 0.0)
                self.assertLessEqual(channel, 1.0)


class TestNormalizeCategoryData(unittest.TestCase):
    """``_normalize_category_data`` accepts nested- and flat-icon shapes and is idempotent."""

    def test_nested_icon_block(self):
        entry = mig._normalize_category_data(
            {
                sk.KEY_GLYPH: "\\ue3c9",
                sk.KEY_DISPLAY_NAME: "Edit",
                sk.KEY_COLOR: [0.1, 0.2, 0.3],
                sk.KEY_BASE_TYPE: "glyph_text",
                sk.KEY_TAGS: ["a", "b"],
                sk.KEY_ICON: {sk.ICON_BLOCK_SOURCE: "manual", sk.ICON_BLOCK_KEY: "PLAY",
                              sk.ICON_BLOCK_PATH: "/p", sk.ICON_BLOCK_PROVIDER: "prov"},
                sk.KEY_SOURCE_EXTENSION: "ext",
                sk.KEY_PENDING_TAG_ASSIGNMENT: True,
            },
            category_name="EditCat",
        )
        self.assertEqual(entry[sk.KEY_ICON_SOURCE], "manual")
        self.assertEqual(entry[sk.KEY_ICON_KEY], "PLAY")
        self.assertEqual(entry[sk.KEY_ICON_PATH], "/p")
        self.assertEqual(entry[sk.KEY_ICON_PROVIDER], "prov")
        self.assertEqual(entry[sk.KEY_TAGS], ["a", "b"])
        self.assertEqual(entry[sk.KEY_SOURCE_EXTENSION], "ext")
        self.assertTrue(entry[sk.KEY_PENDING_TAG_ASSIGNMENT])

    def test_flat_icon_keys(self):
        entry = mig._normalize_category_data(
            {sk.KEY_GLYPH: "X", sk.KEY_ICON_SOURCE: "off", sk.KEY_ICON_KEY: "K",
             sk.KEY_ICON_PATH: "P", sk.KEY_ICON_PROVIDER: "V"},
            category_name="Flat",
        )
        self.assertEqual(entry[sk.KEY_ICON_SOURCE], "off")
        self.assertEqual(entry[sk.KEY_ICON_KEY], "K")
        self.assertEqual(entry[sk.KEY_ICON_PATH], "P")
        self.assertEqual(entry[sk.KEY_ICON_PROVIDER], "V")

    def test_string_input_branch(self):
        entry = mig._normalize_category_data("\\ue3c9", category_name="StrCat")
        self.assertIn(sk.KEY_GLYPH, entry)
        for key in (sk.KEY_ICON_SOURCE, sk.KEY_ICON_KEY, sk.KEY_ICON_PATH, sk.KEY_ICON_PROVIDER):
            self.assertIn(key, entry)

    def test_idempotent_on_normalized_output(self):
        # Normalizing an already-normalized entry must be stable for the core fields.
        first = mig._normalize_category_data(
            {sk.KEY_GLYPH: "X", sk.KEY_DISPLAY_NAME: "Name", sk.KEY_COLOR: [0.3, 0.4, 0.5],
             sk.KEY_TAGS: ["t1"]},
            category_name="Idem",
        )
        second = mig._normalize_category_data(first, category_name="Idem")
        for key in (sk.KEY_GLYPH, sk.KEY_DISPLAY_NAME, sk.KEY_COLOR, sk.KEY_BASE_TYPE,
                    sk.KEY_TAGS, sk.KEY_ICON_SOURCE):
            self.assertEqual(first[key], second[key], "field {0} not idempotent".format(key))


class TestFuzz(unittest.TestCase):
    """Random/garbage input must never crash the normalization pipeline."""

    ATOMS = [None, True, False, 0, 1, -5, 3.14, "", "text", "\\ue3c9", "e866",
             [0.0, 0.0, 0.0], [2.0, -1.0], "GLOBAL", {}]

    def _random_value(self, rng, depth):
        if depth <= 0:
            return rng.choice(self.ATOMS)
        kind = rng.randint(0, 3)
        if kind == 0:
            return rng.choice(self.ATOMS)
        if kind == 1:
            return [self._random_value(rng, depth - 1) for _ in range(rng.randint(0, 4))]
        if kind == 2:
            keys = rng.sample(list(TestSchemaKeys.EXPECTED.values()),
                              rng.randint(0, min(6, len(TestSchemaKeys.EXPECTED))))
            return {k: self._random_value(rng, depth - 1) for k in keys}
        return {"".join(rng.choice("abc_ ") for _ in range(rng.randint(0, 5))):
                self._random_value(rng, depth - 1) for _ in range(rng.randint(0, 3))}

    def test_migrate_json_data_never_crashes(self):
        # Seeded for reproducibility; no wall-clock/entropy dependency.
        rng = random.Random(0xC0FFEE)
        for _ in range(500):
            payload = self._random_value(rng, depth=4)
            out = mig.migrate_json_data(payload)
            # Post-conditions always hold regardless of the garbage in.
            self.assertIsInstance(out[sk.KEY_ALL_TAGS], dict)
            self.assertIsInstance(out[sk.KEY_MAPPINGS], dict)
            self.assertIsInstance(out[sk.KEY_CATEGORY_ORDERS], dict)
            self.assertIsInstance(out[sk.KEY_TAG_ORDER], list)

    def test_normalize_category_data_never_crashes(self):
        rng = random.Random(0x1234)
        for _ in range(500):
            payload = self._random_value(rng, depth=3)
            name = rng.choice([None, "", "Item", "Edit", "\\ue3c9", "Random Cat"])
            mig._normalize_category_data(payload, category_name=name)

    def test_malformed_json_text_then_migrate(self):
        # Text that survives json.loads but is structurally wrong must still be handled.
        for text in ("null", "true", "42", '"a string"', "[1, 2, 3]",
                     '{"all_tags": null}', '{"mappings": [1,2]}', "{}"):
            data = json.loads(text)
            out = mig.migrate_json_data(data)
            self.assertIsInstance(out[sk.KEY_MAPPINGS], dict)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]] + _UNITTEST_ARGV)
