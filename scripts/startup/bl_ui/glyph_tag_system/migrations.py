# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""JSON schema normalization for the Category Tabs / Glyph / Tag system.

Pure data transforms extracted from ``space_userpref.py``: they normalize a single
category entry (:func:`_normalize_category_data`) and validate a whole mappings file
(:func:`migrate_json_data`). The on-disk schema starts at version 1 (baseline), so
there is no cross-version migration chain.

The composition of an entry — which fields exist, their kind and default — is not decided
here but in :mod:`schema_fields`, which the save path shares. This module owns only the part
that cannot be expressed field by field: the derivation of ``base_type`` and the invariants
that tie ``glyph``, ``default_glyph``, ``default_display_name`` and ``first_letter`` to it.

This module is bpy-free and free of module-level mutable state; it depends only on the
lower pure layers of the package (defaults / conversions / schema_fields / log).
"""

from .defaults import (
    CURRENT_JSON_VERSION,
    DEFAULT_CATEGORY_GLYPHS,
)
from .schema_keys import (
    KEY_ALL_TAGS,
    KEY_BASE_TYPE,
    KEY_CATEGORY_ORDERS,
    KEY_COLOR,
    KEY_DEFAULT_DISPLAY_NAME,
    KEY_DEFAULT_GLYPH,
    KEY_DISPLAY_NAME,
    KEY_FIRST_LETTER,
    KEY_GLYPH,
    KEY_MAPPINGS,
    KEY_TAG_ORDER,
    KEY_VERSION,
)
from .schema_fields import (
    _normalize_color,
    coerce_entry,
    coerce_tag_entry,
    field_present,
    new_entry,
    new_tag_entry,
)
from .conversions import (
    _is_single_glyph,
    _unicode_escape_to_glyph,
)
from .log import (
    _pref_log_once,
    category_debug_print,
)


def _legacy_string_to_raw(category_data):
    """Expand a legacy glyph-string category value into raw entry data.

    The oldest files stored a category as a bare glyph (``{"Edit": "\\ue3c9"}``) instead of an
    object. Such a value carries two facts: the glyph, and that the same glyph is the reset
    default. Expressing it as raw data lets the normal path handle it, so the legacy shape needs
    no entry literal of its own to drift out of sync.
    """
    glyph = _unicode_escape_to_glyph(category_data) if '\\u' in category_data else category_data
    return {KEY_GLYPH: glyph, KEY_DEFAULT_GLYPH: glyph}


def _derive_entry_invariants(entry, raw, category_name):
    """Apply the cross-field rules that :func:`schema_fields.coerce_entry` cannot.

    Runs on a fully coerced entry. ``raw`` is consulted only to tell an absent field from one
    that was explicitly stored: an empty ``default_glyph`` is meaningful for text_only categories
    (reset falls back to the first letter), so it must not be re-derived.

    Order matters and is the reason this is a pass of its own: ``base_type`` is derived first
    because every rule below reads it.
    """
    name = category_name if isinstance(category_name, str) else ""

    # Base type (for reset): glyph_only, glyph_text, or text_only.
    # Priority: 1) glyph field is not empty -> glyph_text or glyph_only
    #           2) category_name is a single glyph -> glyph_only
    #           3) otherwise -> text_only
    if not field_present(raw, KEY_BASE_TYPE):
        if entry[KEY_GLYPH]:
            entry[KEY_BASE_TYPE] = "glyph_only" if _is_single_glyph(entry[KEY_GLYPH]) else "glyph_text"
        elif name and _is_single_glyph(name):
            # Category name itself is a glyph (e.g., "" for Script 1).
            entry[KEY_BASE_TYPE] = "glyph_only"
        else:
            entry[KEY_BASE_TYPE] = "text_only"

    # Derive first_letter for legacy data when missing but display_name is available.
    if not entry[KEY_FIRST_LETTER] and entry[KEY_DISPLAY_NAME]:
        entry[KEY_FIRST_LETTER] = entry[KEY_DISPLAY_NAME][:1]

    # Backward compatibility for legacy data where the default_glyph field does not exist.
    # Only glyph_only categories inherit it from glyph; the others reset to the fallback letter.
    if not field_present(raw, KEY_DEFAULT_GLYPH):
        entry[KEY_DEFAULT_GLYPH] = entry[KEY_GLYPH] if entry[KEY_BASE_TYPE] == "glyph_only" else ""

    # For glyph_text categories the category name is the tooltip fallback.
    if not field_present(raw, KEY_DEFAULT_DISPLAY_NAME):
        entry[KEY_DEFAULT_DISPLAY_NAME] = name if entry[KEY_BASE_TYPE] == "glyph_text" else ""

    # Safety correction for previously serialized incorrect state:
    # text_only AND glyph_text categories must reset to fallback letter, so default_glyph must be
    # empty. Only glyph_only categories should have default_glyph set (to category name).
    if entry[KEY_BASE_TYPE] in ("text_only", "glyph_text"):
        entry[KEY_DEFAULT_GLYPH] = ""

    # For glyph_only categories, ensure default_glyph is set to category name (original glyph).
    if entry[KEY_BASE_TYPE] == "glyph_only" and name and _is_single_glyph(name):
        if not entry[KEY_DEFAULT_GLYPH]:
            entry[KEY_DEFAULT_GLYPH] = name
            _pref_log_once(f"[GLYPH LOAD] Set default_glyph for glyph_only category '{name}'")

    # For reserved categories (in DEFAULT_CATEGORY_GLYPHS), restore default_glyph even if the
    # glyph field is empty in JSON. This ensures Reset works correctly.
    if name and name in DEFAULT_CATEGORY_GLYPHS:
        reserved_glyph = DEFAULT_CATEGORY_GLYPHS[name].get(KEY_GLYPH, "")
        if reserved_glyph and entry.get(KEY_DEFAULT_GLYPH) != reserved_glyph:
            entry[KEY_DEFAULT_GLYPH] = reserved_glyph
            entry[KEY_BASE_TYPE] = "glyph_text"
            category_debug_print(
                f"[GLYPH] Restored default_glyph for reserved category '{name}': '{reserved_glyph}'")


def _normalize_category_data(category_data, category_name=None):
    """Normalize category data to a complete, well-typed category entry.

    Accepts the current object shape, the legacy bare-glyph string, and anything else (which
    yields pure defaults). Both on-disk icon shapes — the nested ``icon`` block and the legacy
    flat keys — are accepted; the result always uses the flat form.

    Args:
        category_data: The category data (string or dict)
        category_name: Optional category name (key) for determining base_type when glyph is empty.
                       If category_name is a single glyph, base_type should be glyph_only.
    """
    if isinstance(category_data, str):
        raw = _legacy_string_to_raw(category_data)
    elif isinstance(category_data, dict):
        raw = category_data
    else:
        return new_entry()

    entry = coerce_entry(raw)
    _derive_entry_invariants(entry, raw, category_name)
    return entry


def _normalize_tag_data(tag_data):
    """Normalize raw tag data (file or cache) to a well-typed tag entry.

    The entry point callers should use to turn raw tag data trustworthy, mirroring
    :func:`_normalize_category_data`. Tags have no legacy bare-value shape and, unlike
    categories, no cross-field derivation today, so this is a thin type guard around
    :func:`schema_fields.coerce_tag_entry` — kept here rather than inlined at call sites so a
    future derivation rule has a single place to land, and so callers do not need to know that
    one exists for categories and not (yet) for tags.
    """
    if isinstance(tag_data, dict):
        return coerce_tag_entry(tag_data)
    return new_tag_entry()


def migrate_json_data(data):
    """Validate and normalize a loaded mappings structure.

    The on-disk schema starts at version 1 (baseline), so there is no cross-version
    migration. This guards the structure so a valid-but-malformed file cannot poison
    the caches: required sections are forced to the right container type, ``tag_order``
    to a list of strings, ``category_orders`` values to lists, and every color to three
    floats in ``[0, 1]``. Finally it stamps the current version.
    """
    if not isinstance(data, dict):
        data = {}

    # Required top-level sections must be dictionaries (a non-dict here would crash
    # the loader, e.g. ``all_tags.items()`` if it arrived as a list).
    if not isinstance(data.get(KEY_ALL_TAGS), dict):
        data[KEY_ALL_TAGS] = {}
    if not isinstance(data.get(KEY_MAPPINGS), dict):
        data[KEY_MAPPINGS] = {}
    if not isinstance(data.get(KEY_CATEGORY_ORDERS), dict):
        data[KEY_CATEGORY_ORDERS] = {}

    # ``tag_order`` must be a list of strings.
    tag_order = data.get(KEY_TAG_ORDER)
    data[KEY_TAG_ORDER] = (
        [t for t in tag_order if isinstance(t, str)] if isinstance(tag_order, list) else []
    )

    # ``category_orders`` values must be lists (the loader decodes them as lists).
    data[KEY_CATEGORY_ORDERS] = {
        key: value for key, value in data[KEY_CATEGORY_ORDERS].items() if isinstance(value, list)
    }

    # Normalize tag colors.
    for tag_data in data[KEY_ALL_TAGS].values():
        if isinstance(tag_data, dict) and KEY_COLOR in tag_data:
            tag_data[KEY_COLOR] = _normalize_color(tag_data.get(KEY_COLOR))

    # Normalize category colors within the GLOBAL mappings block.
    for categories in data[KEY_MAPPINGS].values():
        if not isinstance(categories, dict):
            continue
        for cat_data in categories.values():
            if isinstance(cat_data, dict) and KEY_COLOR in cat_data:
                cat_data[KEY_COLOR] = _normalize_color(cat_data.get(KEY_COLOR))

    data[KEY_VERSION] = CURRENT_JSON_VERSION
    return data
