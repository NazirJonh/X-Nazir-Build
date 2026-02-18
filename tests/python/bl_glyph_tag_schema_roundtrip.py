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
from glyph_tag_system import schema_fields as sf  # noqa: E402
from glyph_tag_system import migrations as mig  # noqa: E402
from glyph_tag_system import modes as md  # noqa: E402
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
        "KEY_WITHOUT_TAG_PREVIEW": "without_tag_preview",
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

    def test_glyph_encoding_backward_compat(self):
        # Category glyphs are now written as raw UTF-8, but legacy files stored \uXXXX escapes.
        # Both encodings must normalize to the same glyph character on read.
        glyph = ""
        raw = mig._normalize_category_data(
            {sk.KEY_GLYPH: glyph, sk.KEY_DEFAULT_GLYPH: glyph}, category_name="Enc")
        legacy = mig._normalize_category_data(
            {sk.KEY_GLYPH: "\\ue3c9", sk.KEY_DEFAULT_GLYPH: "\\ue3c9"}, category_name="Enc")
        self.assertEqual(raw[sk.KEY_GLYPH], glyph)
        self.assertEqual(legacy[sk.KEY_GLYPH], glyph)
        self.assertEqual(raw[sk.KEY_DEFAULT_GLYPH], glyph)
        self.assertEqual(legacy[sk.KEY_DEFAULT_GLYPH], glyph)

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


class TestInstallModeFlag(unittest.TestCase):
    """``install_mode_flag`` must cross the serialization boundary in both directions.

    ``_normalize_category_data`` rebuilds the entry from a white-list of fields, so a field that
    is merely present in the cache is not enough: it has to be copied explicitly or it is dropped
    on the way through. That matters here because the C++ panel filter falls back to this flag for
    panels that declare no ``bl_context``, and ``wm_sync_to_wm`` writes the normalized value into a
    DNA ``uint32_t`` via RNA — hence the range clamp.
    """

    def test_value_survives_normalization(self):
        entry = mig._normalize_category_data(
            {sk.KEY_GLYPH: "X", sk.KEY_INSTALL_MODE_FLAG: 5}, category_name="Ext")
        self.assertEqual(entry[sk.KEY_INSTALL_MODE_FLAG], 5)

    def test_absent_defaults_to_zero(self):
        # Files written before the flag was persisted must keep loading unchanged.
        entry = mig._normalize_category_data({sk.KEY_GLYPH: "X"}, category_name="Plain")
        self.assertEqual(entry[sk.KEY_INSTALL_MODE_FLAG], 0)

    def test_string_input_branch_defaults_to_zero(self):
        entry = mig._normalize_category_data("\\ue3c9", category_name="StrCat")
        self.assertEqual(entry[sk.KEY_INSTALL_MODE_FLAG], 0)

    def test_clamped_to_uint32_range(self):
        cases = {
            "non_numeric": ("abc", 0),
            "none": (None, 0),
            "list": ([1, 2], 0),
            "negative": (-1, 0),
            "overflow": (2 ** 40, 0xFFFFFFFF),
            "truncated_float": (3.9, 3),
            "max": (0xFFFFFFFF, 0xFFFFFFFF),
        }
        for name, (value, expected) in cases.items():
            entry = mig._normalize_category_data(
                {sk.KEY_INSTALL_MODE_FLAG: value}, category_name="Clamp")
            self.assertEqual(entry[sk.KEY_INSTALL_MODE_FLAG], expected, name)

    def test_idempotent_on_normalized_output(self):
        first = mig._normalize_category_data(
            {sk.KEY_GLYPH: "X", sk.KEY_INSTALL_MODE_FLAG: 7}, category_name="Idem")
        second = mig._normalize_category_data(first, category_name="Idem")
        self.assertEqual(second[sk.KEY_INSTALL_MODE_FLAG], 7)


class TestEntryRoundTrip(unittest.TestCase):
    """save -> load must preserve every field in the table, for all fields at once.

    The save path used to be a hand-written literal that had to be kept in step with the load
    path by eye, and it lost ``install_mode_flag`` that way. These tests are driven by
    :data:`schema_fields.CATEGORY_FIELDS` rather than by a list of field names, so a field added
    to the table is covered here from the moment it exists — which is the property that makes the
    table a single source of truth rather than a fourth place to forget something.
    """

    # A category name that is neither reserved nor a single glyph, so no derivation rule
    # overrides the probe values below.
    NAME = "ProbeCat"

    PROBES_BY_KIND = {
        sf.KIND_GLYPH: "X",
        sf.KIND_STR: "probe",
        sf.KIND_STR_COERCE: "probe",
        sf.KIND_BOOL: True,
        sf.KIND_U32: 5,
        sf.KIND_STR_LIST: ["p1", "p2"],
        sf.KIND_COLOR: [0.25, 0.5, 0.75],
    }

    # Kind alone cannot pick a probe here: the only base_type that survives derivation with a
    # non-empty default_glyph is glyph_only (the other two reset it to the fallback letter).
    PROBE_OVERRIDES = {
        sk.KEY_BASE_TYPE: "glyph_only",
    }

    def _probe_value(self, field):
        if field.key in self.PROBE_OVERRIDES:
            return self.PROBE_OVERRIDES[field.key]
        if field.kind == sf.KIND_ENUM:
            for choice in field.choices:
                if choice != field.default:
                    return choice
            self.fail("enum field {0} has no non-default choice".format(field.key))
        return self.PROBES_BY_KIND[field.kind]

    def _probe_entry(self):
        raw = {field.key: self._probe_value(field) for field in sf.CATEGORY_FIELDS}
        return mig._normalize_category_data(raw, category_name=self.NAME)

    def test_every_field_has_a_probe(self):
        # A kind with no probe would silently skip its fields in the round-trip test below.
        for field in sf.CATEGORY_FIELDS:
            if field.kind == sf.KIND_ENUM or field.key in self.PROBE_OVERRIDES:
                continue
            self.assertIn(field.kind, self.PROBES_BY_KIND,
                          "field {0} has kind {1} with no probe".format(field.key, field.kind))

    def test_probe_entry_is_non_default_everywhere(self):
        # Guards the round-trip test against going vacuous: a field whose probe survives
        # normalization as its own default would pass even if the save path dropped it.
        entry = self._probe_entry()
        for field in sf.CATEGORY_FIELDS:
            self.assertNotEqual(
                entry[field.key], field.make_default(),
                "probe for {0} collapsed to its default; add a PROBE_OVERRIDES entry".format(field.key))

    def test_all_persisted_fields_survive_save_load(self):
        # The regression that motivated the table: a field present in the entry but absent from
        # the writer is invisible until something downstream reads a zero.
        entry = self._probe_entry()
        on_disk = json.loads(json.dumps(sf.entry_to_disk(entry)))
        reloaded = mig._normalize_category_data(on_disk, category_name=self.NAME)
        for field in sf.CATEGORY_FIELDS:
            if field.persist:
                self.assertEqual(reloaded[field.key], entry[field.key], field.key)
            else:
                self.assertEqual(reloaded[field.key], field.make_default(), field.key)

    def test_transient_fields_never_reach_the_file(self):
        # A transient field restored from disk would act on stale intent (see
        # KEY_WITHOUT_TAG_PREVIEW), so its absence from the file is a requirement, not an
        # optimization.
        on_disk = sf.entry_to_disk(self._probe_entry())
        transient = [field.key for field in sf.CATEGORY_FIELDS if not field.persist]
        self.assertTrue(transient, "expected at least one transient field to guard")
        for key in transient:
            self.assertNotIn(key, on_disk)

    def test_transient_fields_survive_normalization(self):
        # They are dropped from the file but must live through a normalize round inside the
        # cache: dropping them there is what silently killed the "Without Tag" Save handler.
        for field in sf.CATEGORY_FIELDS:
            if field.persist:
                continue
            probe = self._probe_value(field)
            entry = mig._normalize_category_data(
                {sk.KEY_GLYPH: "X", field.key: probe}, category_name=self.NAME)
            self.assertEqual(entry[field.key], probe, field.key)

    def test_disk_shape_nests_icon_block(self):
        on_disk = sf.entry_to_disk(self._probe_entry())
        self.assertEqual(on_disk[sk.KEY_ICON][sk.ICON_BLOCK_SOURCE], "manual")
        self.assertEqual(on_disk[sk.KEY_ICON][sk.ICON_BLOCK_KEY], "probe")
        # The flat cache keys must not leak into the file alongside the nested block.
        for key in (sk.KEY_ICON_SOURCE, sk.KEY_ICON_KEY, sk.KEY_ICON_PATH, sk.KEY_ICON_PROVIDER):
            self.assertNotIn(key, on_disk)

    def test_disk_shape_writes_every_persisted_field(self):
        # Checked for a default entry too, not just a populated one: the writer used to omit
        # fields whose value was falsy, and a probe entry is non-default everywhere by
        # construction, so it alone would never exercise that branch.
        for label, entry in (("probe", self._probe_entry()),
                             ("default", mig._normalize_category_data({}, category_name=self.NAME))):
            on_disk = sf.entry_to_disk(entry)
            for field in sf.CATEGORY_FIELDS:
                if not field.persist:
                    continue
                if field.icon_key is not None:
                    self.assertIn(field.icon_key, on_disk[sk.KEY_ICON],
                                  "{0}/{1}".format(label, field.key))
                else:
                    self.assertIn(field.key, on_disk, "{0}/{1}".format(label, field.key))

    def test_normalization_is_a_fixed_point(self):
        # Re-normalizing an entry must change nothing, or a save/load cycle would drift a
        # category's appearance without the user touching it.
        entry = self._probe_entry()
        self.assertEqual(mig._normalize_category_data(entry, category_name=self.NAME), entry)

    def test_absent_fields_load_as_defaults(self):
        # Files written before a field existed must keep loading; absent means default.
        entry = mig._normalize_category_data({}, category_name=self.NAME)
        for field in sf.CATEGORY_FIELDS:
            self.assertEqual(entry[field.key], field.make_default(), field.key)

    def test_legacy_string_matches_equivalent_object(self):
        # The bare-glyph shape is expanded through the same path as an object, so the two cannot
        # disagree about the entry's composition the way two literals did.
        glyph = ""
        from_string = mig._normalize_category_data(glyph, category_name="LegacyCat")
        from_object = mig._normalize_category_data(
            {sk.KEY_GLYPH: glyph, sk.KEY_DEFAULT_GLYPH: glyph}, category_name="LegacyCat")
        self.assertEqual(from_string, from_object)
        self.assertEqual(set(from_string), {field.key for field in sf.CATEGORY_FIELDS})


class TestTagEntryRoundTrip(unittest.TestCase):
    """save -> load must preserve every field in :data:`schema_fields.TAG_FIELDS`.

    Mirrors :class:`TestEntryRoundTrip`. The tag composition used to be three hand-written
    literals (load in ``glyph_cache``, save in ``glyph_cache``, ``create_tag`` in
    ``tags_cache``) that had to be kept in step by eye — the same shape of bug that dropped
    ``install_mode_flag`` from the category save path. These tests are driven by
    :data:`schema_fields.TAG_FIELDS` rather than a list of field names, so a field added to the
    table is covered from the moment it exists.

    A tag has a field the category table does not: the glyph is a raw Unicode character in the
    entry/cache but a bare hex codepoint on disk (DNA stores it in a fixed ``char[8]``, not a
    Python string). The probe below exercises that encode/decode along with every other field.
    """

    PROBES_BY_KIND = {
        sf.KIND_GLYPH_HEX: "",
        # The glyph probe above is the decoded form of DEFAULT_TAG_GLYPH_HEX ("e866" in
        # defaults.py); this doubles test_disk_shape_stores_glyph_as_hex below as a check
        # against that constant drifting.
        sf.KIND_COLOR: [0.25, 0.5, 0.75],
        # Deliberately not 7 (0b111): that is _CATEGORY_TAG_DEFAULT_MODE_FLAGS, and a probe equal
        # to its own field's default would make test_probe_entry_is_non_default_everywhere
        # vacuous for this field.
        sf.KIND_U32: 1024,
        sf.KIND_STR_COERCE: "OBJECT_DATAMODE",
    }

    def _probe_value(self, field):
        if field.kind == sf.KIND_INT_ENUM:
            for choice in field.choices:
                if choice != field.default:
                    return choice
            self.fail("enum field {0} has no non-default choice".format(field.key))
        return self.PROBES_BY_KIND[field.kind]

    def _probe_entry(self):
        raw = {field.key: self._probe_value(field) for field in sf.TAG_FIELDS}
        return mig._normalize_tag_data(raw)

    def test_every_field_has_a_probe(self):
        # A kind with no probe would silently skip its fields in the round-trip test below.
        for field in sf.TAG_FIELDS:
            if field.kind == sf.KIND_INT_ENUM:
                continue
            self.assertIn(field.kind, self.PROBES_BY_KIND,
                          "field {0} has kind {1} with no probe".format(field.key, field.kind))

    def test_probe_entry_is_non_default_everywhere(self):
        # Guards the round-trip test against going vacuous: a field whose probe survives
        # normalization as its own default would pass even if the save path dropped it.
        entry = self._probe_entry()
        for field in sf.TAG_FIELDS:
            self.assertNotEqual(
                entry[field.key], field.make_default(),
                "probe for {0} collapsed to its default".format(field.key))

    def test_all_fields_survive_save_load(self):
        entry = self._probe_entry()
        on_disk = json.loads(json.dumps(sf.tag_entry_to_disk(entry)))
        reloaded = mig._normalize_tag_data(on_disk)
        self.assertEqual(reloaded, entry)

    def test_disk_shape_writes_every_field(self):
        # Checked for a default entry too, not just a populated one: a writer that omits a
        # falsy-valued field would never be caught by the probe entry alone, since the probe is
        # non-default (and therefore truthy where it matters) by construction.
        for label, entry in (("probe", self._probe_entry()), ("default", mig._normalize_tag_data({}))):
            on_disk = sf.tag_entry_to_disk(entry)
            for field in sf.TAG_FIELDS:
                self.assertIn(field.key, on_disk, "{0}/{1}".format(label, field.key))

    def test_disk_shape_stores_glyph_as_hex(self):
        # DEFAULT_TAG_GLYPH_HEX in defaults.py is "e866"; the probe glyph above is its decoded
        # character, chosen so this assertion doubles as a check against that constant drifting.
        on_disk = sf.tag_entry_to_disk(self._probe_entry())
        self.assertEqual(on_disk[sk.KEY_GLYPH], "e866")

    def test_normalization_is_a_fixed_point(self):
        # Re-normalizing an already-normalized entry (raw Unicode glyph, not hex) must change
        # nothing. This is the test that would catch the hex/glyph shape-detection in
        # _coerce_tag_glyph drifting into a double-decode.
        entry = self._probe_entry()
        self.assertEqual(mig._normalize_tag_data(entry), entry)

    def test_absent_fields_load_as_defaults(self):
        entry = mig._normalize_tag_data({})
        for field in sf.TAG_FIELDS:
            self.assertEqual(entry[field.key], field.make_default(), field.key)

    def test_non_dict_tag_data_normalizes_instead_of_poisoning_cache(self):
        # The load-time hole that motivated the table: raw non-dict tag data used to be written
        # into the cache verbatim (glyph_cache.py's ``else: state.all_tags_cache[tag_name] =
        # tag_data``), where it would crash the first update_tag/create_tag call or any UI code
        # indexing into it as a dict.
        entry = mig._normalize_tag_data("corrupt-legacy-value")
        self.assertEqual(entry, sf.new_tag_entry())

    def test_icon_source_domain_is_int_only(self):
        # The table only understands the int domain DNA stores; the 'GLYPH'/'BLENDER_ICON'/
        # 'CUSTOM' convenience strings are a tags_cache.create_tag/update_tag call-site concern
        # (_TAG_ICON_SOURCE_NAME_TO_INT there), not something the table itself resolves.
        self.assertEqual(mig._normalize_tag_data({sk.KEY_ICON_SOURCE: 1})[sk.KEY_ICON_SOURCE], 1)
        self.assertEqual(mig._normalize_tag_data({sk.KEY_ICON_SOURCE: 99})[sk.KEY_ICON_SOURCE], 0)
        self.assertEqual(
            mig._normalize_tag_data({sk.KEY_ICON_SOURCE: "BLENDER_ICON"})[sk.KEY_ICON_SOURCE], 0)


class TestCategoryMatchesContext(unittest.TestCase):
    """The space/mode predicate shared by the tag-bar count and the bulk-clear.

    C++ evaluates the same rule independently in ``category_is_unassigned_for_context``
    (``interface_panel.cc``) because it cannot call Python while drawing panels. That
    duplication is deliberate but not free: the two implementations must agree, or the
    "New Add-ons!" count will not match the categories the panel filter actually shows.
    """

    # Bits are opaque to the predicate; any two distinct flags exercise it.
    MODE_A = 1 << 0
    MODE_B = 1 << 1
    SPACE_VIEW3D = 1
    SPACE_NODE = 16
    VIEW3D_FLAG = 1 << 0
    NODE_FLAG = 1 << 11

    def _match(self, **kwargs):
        params = {
            "discovered_spaces": 0,
            "discovered_modes": 0,
            "install_mode_flag": 0,
            "space_type": self.SPACE_VIEW3D,
            "current_mode_flag": self.MODE_A,
        }
        params.update(kwargs)
        return md.category_matches_context(**params)

    def test_zero_means_any(self):
        self.assertTrue(self._match())
        self.assertTrue(self._match(space_type=-1, discovered_spaces=self.NODE_FLAG))
        self.assertTrue(self._match(current_mode_flag=0, discovered_modes=self.MODE_B))

    def test_space_mismatch_rejects(self):
        self.assertFalse(self._match(discovered_spaces=self.NODE_FLAG))
        self.assertTrue(self._match(discovered_spaces=self.VIEW3D_FLAG))

    def test_mode_mismatch_rejects(self):
        self.assertFalse(self._match(discovered_modes=self.MODE_B))
        self.assertTrue(self._match(discovered_modes=self.MODE_A))
        self.assertTrue(self._match(discovered_modes=self.MODE_A | self.MODE_B))

    def test_install_flag_is_the_fallback_only_when_nothing_was_discovered(self):
        # Panels without bl_context discover no mode, so the install mode is all there is.
        self.assertFalse(self._match(install_mode_flag=self.MODE_B))
        self.assertTrue(self._match(install_mode_flag=self.MODE_A))
        # A discovered mode wins; the install flag must not override or widen it.
        self.assertTrue(self._match(discovered_modes=self.MODE_A, install_mode_flag=self.MODE_B))
        self.assertFalse(self._match(discovered_modes=self.MODE_B, install_mode_flag=self.MODE_A))

    def test_node_editor_skips_mode_filtering(self):
        # Node Editor modes are unlike 3D View modes and were often recorded incorrectly during
        # drag-drop. Both C++ and the bulk-clear must agree to skip the check here.
        self.assertTrue(self._match(space_type=self.SPACE_NODE,
                                    discovered_spaces=self.NODE_FLAG,
                                    discovered_modes=self.MODE_B))
        # The space check still applies in the Node Editor.
        self.assertFalse(self._match(space_type=self.SPACE_NODE,
                                     discovered_spaces=self.VIEW3D_FLAG))


class TestCppMirrorsPythonPredicate(unittest.TestCase):
    """Pin the C++ copy of the rule against this source tree.

    Reading the C++ is crude, but the alternative is a comment asking the next person to keep
    two files in step by hand — which is how the third copy of this rule drifted in the first
    place (the bulk-clear had lost the Node Editor exemption). This does not prove the two
    agree; it fails when the C++ side is edited, so the Python side gets revisited with it.
    """

    CPP_PATH = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "source", "blender", "editors", "interface", "interface_panel.cc")

    def _predicate_source(self):
        if not os.path.exists(self.CPP_PATH):
            self.skipTest("interface_panel.cc not found (running outside a source checkout)")
        with open(self.CPP_PATH, "r", encoding="utf-8") as handle:
            text = handle.read()
        start = text.find("bool category_is_unassigned_for_context(const wmWindowManager")
        self.assertNotEqual(start, -1, "category_is_unassigned_for_context not found in C++")
        # The definition ends at the first closing brace in column 0; nested braces are indented.
        end = text.find("\n}\n", start)
        self.assertNotEqual(end, -1, "could not find the end of category_is_unassigned_for_context")
        return text[start:end]

    def test_cpp_still_exempts_node_editor_from_mode_filtering(self):
        source = self._predicate_source()
        self.assertIn("space_type != SPACE_NODE", source,
                      "C++ dropped the Node Editor exemption; modes.category_matches_context must follow")

    def test_cpp_still_falls_back_to_install_mode_flag(self):
        source = self._predicate_source()
        self.assertIn("install_mode_flag", source,
                      "C++ dropped the install_mode_flag fallback; modes.category_matches_context must follow")

    def test_cpp_still_treats_zero_as_any(self):
        source = self._predicate_source()
        for token in ("current_mode_flag != 0", "discovered_in_spaces != 0"):
            self.assertIn(token, source,
                          "C++ changed its zero-means-any handling ({0}); "
                          "modes.category_matches_context must follow".format(token))


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
