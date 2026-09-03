# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Single source of truth for the on-disk JSON schema key names.

The ``category_glyphs.json`` file is written by :mod:`glyph_cache` and read back by
:mod:`glyph_cache` / :mod:`migrations`; a typo in one key literal on the write side that
is not mirrored on the read side silently drops data on the next round-trip. Centralising
the key names here removes that drift for the file-persistence boundary (save / load /
migrations / handlers).

Scope note: these constants are applied at the serialization boundary. Other cache readers
(``glyph_cache`` getters, ``wm_sync_*``, ``discovery_*``) keep plain string literals; they
depend only on the *values* below, which are pinned to the historical key strings, so
nothing changes at runtime. Do not change a value here without updating every reader.

This module is intentionally free of ``bpy`` and package dependencies so it can be imported
standalone (including from bpy-free tests).
"""

# ----------------------------------------------------------------------------
# Top-level sections of the mappings file.

KEY_VERSION = "version"
KEY_ALL_TAGS = "all_tags"
KEY_MAPPINGS = "mappings"
KEY_CATEGORY_ORDERS = "category_orders"
KEY_TAG_ORDER = "tag_order"

# Sub-container inside ``mappings``. Global-First architecture stores every category under
# this single key (``mappings["GLOBAL"][category] = entry``).
KEY_GLOBAL = "GLOBAL"

# ----------------------------------------------------------------------------
# Category entry fields (shared by the in-memory cache dict and the on-disk entry, except
# for the icon block which is nested on disk but flat in the cache — see below).
#
# This module names the keys; it does not define which of them make up an entry. That
# composition — kind, default and sanitizer per field — lives in :mod:`schema_fields`, which
# both the load and the save path build from. Add a field there, not only here.

KEY_GLYPH = "glyph"
KEY_DISPLAY_NAME = "display_name"
KEY_FIRST_LETTER = "first_letter"
KEY_COLOR = "color"
KEY_DEFAULT_GLYPH = "default_glyph"
KEY_DEFAULT_DISPLAY_NAME = "default_display_name"
KEY_BASE_TYPE = "base_type"
KEY_TAGS = "tags"
KEY_GLYPH_MODE = "glyph_mode"
KEY_SOURCE_EXTENSION = "source_extension"
KEY_PENDING_TAG_ASSIGNMENT = "pending_tag_assignment"
KEY_DISCOVERED_IN_SPACES = "discovered_in_spaces"
KEY_DISCOVERED_IN_MODES = "discovered_in_modes"
KEY_INSTALL_MODE_FLAG = "install_mode_flag"

# Transient preview state: set when the user picks "Without Tag" in the dialog and consumed on
# Save. It lives in the entry but must never reach the file — a stale flag restored from disk
# would finalize a choice the user never confirmed. See ``persist`` in :mod:`schema_fields`.
KEY_WITHOUT_TAG_PREVIEW = "without_tag_preview"

# ----------------------------------------------------------------------------
# Tag entry fields (inside ``all_tags``). Tags share ``glyph``/``color``/``icon_*`` with
# categories but own the bitmask below, which categories never carry: a category is filtered
# by the modes it was discovered in, a tag by the modes it was declared for.

KEY_MODE_FLAGS = "mode_flags"

# ----------------------------------------------------------------------------
# Icon fields.
#
# The in-memory cache (and legacy on-disk data) stores icon fields flat: ``icon_source``,
# ``icon_key``, ``icon_path``, ``icon_provider``. The current on-disk format nests them under
# an ``icon`` object with short keys: ``{"source", "key", "path", "provider"}``.
# :func:`schema_fields.coerce_entry` accepts both shapes on load.

KEY_ICON_SOURCE = "icon_source"
KEY_ICON_KEY = "icon_key"
KEY_ICON_PATH = "icon_path"
KEY_ICON_PROVIDER = "icon_provider"

# Nested icon block (on-disk).
KEY_ICON = "icon"
ICON_BLOCK_SOURCE = "source"
ICON_BLOCK_KEY = "key"
ICON_BLOCK_PATH = "path"
ICON_BLOCK_PROVIDER = "provider"
