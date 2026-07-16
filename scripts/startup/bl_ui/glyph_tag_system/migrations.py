# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""JSON schema normalization for the Category Tabs / Glyph / Tag system.

Pure data transforms extracted from ``space_userpref.py``: they normalize a single
category entry (:func:`_normalize_category_data`) and validate a whole mappings file
(:func:`migrate_json_data`). The on-disk schema starts at version 1 (baseline), so
there is no cross-version migration chain.

This module is bpy-free and free of module-level mutable state; it depends only on the
lower pure layers of the package (defaults / conversions / log).
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
    KEY_DISCOVERED_IN_MODES,
    KEY_DISCOVERED_IN_SPACES,
    KEY_DISPLAY_NAME,
    KEY_FIRST_LETTER,
    KEY_GLYPH,
    KEY_GLYPH_MODE,
    KEY_ICON,
    KEY_ICON_KEY,
    KEY_ICON_PATH,
    KEY_ICON_PROVIDER,
    KEY_ICON_SOURCE,
    KEY_INSTALL_MODE_FLAG,
    KEY_MAPPINGS,
    KEY_MODE_FLAGS,
    KEY_PENDING_TAG_ASSIGNMENT,
    KEY_SOURCE_EXTENSION,
    KEY_TAGS,
    KEY_TAG_ORDER,
    KEY_VERSION,
    ICON_BLOCK_KEY,
    ICON_BLOCK_PATH,
    ICON_BLOCK_PROVIDER,
    ICON_BLOCK_SOURCE,
)
from .conversions import (
    _is_single_glyph,
    _unicode_escape_to_glyph,
)
from .log import (
    _pref_log_once,
    category_debug_print,
)


def _normalize_category_data(category_data, category_name=None):
    """Normalize category data to the new format with glyph, display_name, color, defaults, and tags.

    Args:
        category_data: The category data (string or dict)
        category_name: Optional category name (key) for determining base_type when glyph is empty.
                       If category_name is a single glyph, base_type should be glyph_only.
    """
    default_entry = {
        KEY_GLYPH: "", KEY_DISPLAY_NAME: "", KEY_COLOR: [0.0, 0.0, 0.0],
        KEY_DEFAULT_GLYPH: "", KEY_DEFAULT_DISPLAY_NAME: "", KEY_BASE_TYPE: "text_only",
        KEY_FIRST_LETTER: "",
        KEY_TAGS: [],  # NEW: array of tag names
        KEY_MODE_FLAGS: [],  # NEW: array of mode names
        KEY_GLYPH_MODE: "auto",
        KEY_ICON_SOURCE: "auto",
        KEY_ICON_KEY: "",
        KEY_ICON_PATH: "",
        KEY_ICON_PROVIDER: "",
        # Extension-related fields for "New Add-ons!" feature
        KEY_SOURCE_EXTENSION: "",
        KEY_PENDING_TAG_ASSIGNMENT: False,
        KEY_DISCOVERED_IN_SPACES: [],
        KEY_DISCOVERED_IN_MODES: [],
        KEY_INSTALL_MODE_FLAG: 0,
    }

    if isinstance(category_data, str):
        # Old format: just a glyph string (may be in \uXXXX format)
        glyph = _unicode_escape_to_glyph(category_data) if '\\u' in category_data else category_data
        base_type = "glyph_only" if _is_single_glyph(glyph) else "glyph_text"
        # For glyph_text categories, use category name as default_display_name for tooltip fallback
        default_display_name = "" if base_type == "glyph_only" else category_name
        return {KEY_GLYPH: glyph, KEY_DISPLAY_NAME: "", KEY_COLOR: [0.0, 0.0, 0.0],
                KEY_DEFAULT_GLYPH: glyph, KEY_DEFAULT_DISPLAY_NAME: default_display_name, KEY_BASE_TYPE: base_type,
                KEY_FIRST_LETTER: "",
                KEY_TAGS: [],
                KEY_GLYPH_MODE: "auto",
                KEY_ICON_SOURCE: "auto", KEY_ICON_KEY: "", KEY_ICON_PATH: "", KEY_ICON_PROVIDER: "",
                KEY_SOURCE_EXTENSION: "", KEY_PENDING_TAG_ASSIGNMENT: False,
                KEY_DISCOVERED_IN_SPACES: [], KEY_DISCOVERED_IN_MODES: [],
                KEY_INSTALL_MODE_FLAG: 0}
    elif isinstance(category_data, dict):
        # New format: dict with glyph, display_name, color, default_glyph, default_display_name, base_type, tags
        entry = default_entry.copy()

        # Current values
        if KEY_GLYPH in category_data:
            glyph_str = category_data[KEY_GLYPH]
            # Corrupt/hand-edited files may store a non-string glyph (e.g. a list); coerce it
            # away so downstream single-glyph checks cannot crash on it.
            if not isinstance(glyph_str, str):
                glyph_str = ""
            if glyph_str and '\\u' in glyph_str:
                decoded_glyph = _unicode_escape_to_glyph(glyph_str)
                entry[KEY_GLYPH] = decoded_glyph
                if 'Brushstroke' in str(category_name):
                    category_debug_print(f"[NORMALIZE] Brushstroke glyph decoded: '{glyph_str}' -> '{decoded_glyph}' (len={len(decoded_glyph)})")
            else:
                entry[KEY_GLYPH] = glyph_str
        if KEY_DISPLAY_NAME in category_data:
            display_name = category_data[KEY_DISPLAY_NAME]
            # Guard against non-string display names from corrupt files (they are sliced below).
            entry[KEY_DISPLAY_NAME] = display_name if isinstance(display_name, str) else ""
        if KEY_FIRST_LETTER in category_data:
            first_letter = category_data.get(KEY_FIRST_LETTER, "")
            entry[KEY_FIRST_LETTER] = first_letter if isinstance(first_letter, str) else ""
        # Derive first_letter for legacy data when missing but display_name is available.
        if not entry[KEY_FIRST_LETTER] and entry.get(KEY_DISPLAY_NAME):
            entry[KEY_FIRST_LETTER] = entry[KEY_DISPLAY_NAME][:1]
        if KEY_COLOR in category_data:
            color = category_data[KEY_COLOR]
            if isinstance(color, (list, tuple)) and len(color) >= 3:
                entry[KEY_COLOR] = list(color[:3])

        # Default values (for reset)
        # IMPORTANT: Distinguish between "field is missing" vs "field is present but intentionally empty".
        # Empty default_glyph is meaningful for text_only categories (fallback letter behavior).
        if KEY_DEFAULT_GLYPH in category_data:
            glyph_str = category_data[KEY_DEFAULT_GLYPH]
            if not isinstance(glyph_str, str):
                glyph_str = ""
            if glyph_str and '\\u' in glyph_str:
                decoded_default = _unicode_escape_to_glyph(glyph_str)
                entry[KEY_DEFAULT_GLYPH] = decoded_default
                if 'Brushstroke' in str(category_name):
                    category_debug_print(f"[NORMALIZE] Brushstroke default_glyph decoded: '{glyph_str}' -> '{decoded_default}'")
            else:
                entry[KEY_DEFAULT_GLYPH] = glyph_str or ""
        else:
            # Backward compatibility for legacy data where default_glyph field does not exist.
            # IMPORTANT: Only set default_glyph from glyph for glyph_only categories.
            # For text_only/glyph_text categories, default_glyph should be empty (reset returns first letter).
            if entry.get(KEY_BASE_TYPE) == "glyph_only":
                entry[KEY_DEFAULT_GLYPH] = entry[KEY_GLYPH]
            else:
                entry[KEY_DEFAULT_GLYPH] = ""

        if KEY_DEFAULT_DISPLAY_NAME in category_data:
            entry[KEY_DEFAULT_DISPLAY_NAME] = category_data[KEY_DEFAULT_DISPLAY_NAME]
        else:
            # If no default_display_name, set based on base_type
            # For glyph_text categories, use category name for tooltip fallback
            if entry.get(KEY_BASE_TYPE) == "glyph_text":
                entry[KEY_DEFAULT_DISPLAY_NAME] = category_name
            else:
                entry[KEY_DEFAULT_DISPLAY_NAME] = ""

        # Base type (for reset): glyph_only, glyph_text, or text_only
        if KEY_BASE_TYPE in category_data:
            entry[KEY_BASE_TYPE] = category_data[KEY_BASE_TYPE]
        else:
            # Determine base_type from current values
            # Priority: 1) glyph field is not empty -> glyph_text or glyph_only
            #           2) category_name is a single glyph -> glyph_only
            #           3) otherwise -> text_only
            if entry[KEY_GLYPH]:
                entry[KEY_BASE_TYPE] = "glyph_only" if _is_single_glyph(entry[KEY_GLYPH]) else "glyph_text"
            elif category_name and _is_single_glyph(category_name):
                # Category name itself is a glyph (e.g., "" for Script 1)
                entry[KEY_BASE_TYPE] = "glyph_only"
            else:
                entry[KEY_BASE_TYPE] = "text_only"

        # Safety correction for previously serialized incorrect state:
        # text_only AND glyph_text categories must reset to fallback letter, so default_glyph must be empty.
        # Only glyph_only categories should have default_glyph set (to category name).
        if entry[KEY_BASE_TYPE] in ("text_only", "glyph_text"):
            entry[KEY_DEFAULT_GLYPH] = ""

        # For glyph_only categories, ensure default_glyph is set to category name (original glyph)
        if entry[KEY_BASE_TYPE] == "glyph_only" and category_name and _is_single_glyph(category_name):
            if not entry.get(KEY_DEFAULT_GLYPH):
                entry[KEY_DEFAULT_GLYPH] = category_name
                _pref_log_once(f"[GLYPH LOAD] Set default_glyph for glyph_only category '{category_name}'")

        # NEW: For reserved categories (in DEFAULT_CATEGORY_GLYPHS), restore default_glyph
        # even if glyph field is empty in JSON. This ensures Reset works correctly.
        if category_name and category_name in DEFAULT_CATEGORY_GLYPHS:
            default_data = DEFAULT_CATEGORY_GLYPHS[category_name]
            reserved_glyph = default_data.get(KEY_GLYPH, "")
            if reserved_glyph:
                # Only restore if default_glyph is actually empty or incorrect
                current_default = entry.get(KEY_DEFAULT_GLYPH, "")
                if not current_default or current_default != reserved_glyph:
                    entry[KEY_DEFAULT_GLYPH] = reserved_glyph
                    entry[KEY_BASE_TYPE] = "glyph_text"
                    category_debug_print(f"[GLYPH] Restored default_glyph for reserved category '{category_name}': '{reserved_glyph}'")

        # NEW: Tags
        if KEY_TAGS in category_data:
            tags = category_data[KEY_TAGS]
            if isinstance(tags, list):
                entry[KEY_TAGS] = [str(t) for t in tags]

        # Extension-related fields for "New Add-ons!" feature
        if KEY_SOURCE_EXTENSION in category_data:
            entry[KEY_SOURCE_EXTENSION] = str(category_data[KEY_SOURCE_EXTENSION])
        if KEY_PENDING_TAG_ASSIGNMENT in category_data:
            val = category_data[KEY_PENDING_TAG_ASSIGNMENT]
            entry[KEY_PENDING_TAG_ASSIGNMENT] = bool(val) if val else False
        if KEY_DISCOVERED_IN_SPACES in category_data:
            spaces = category_data[KEY_DISCOVERED_IN_SPACES]
            if isinstance(spaces, list):
                entry[KEY_DISCOVERED_IN_SPACES] = [str(s) for s in spaces]
        if KEY_DISCOVERED_IN_MODES in category_data:
            modes = category_data[KEY_DISCOVERED_IN_MODES]
            if isinstance(modes, list):
                entry[KEY_DISCOVERED_IN_MODES] = [str(m) for m in modes]

        # Mode flag captured when the extension was installed. It is the fallback the C++ side
        # uses for panels that declare no ``bl_context`` (see #interface_panel.cc), so dropping it
        # here would silently disable mode-aware filtering for those panels. DNA stores it in a
        # ``uint32_t``: a corrupt or negative value must never reach the RNA assignment.
        if KEY_INSTALL_MODE_FLAG in category_data:
            try:
                install_mode_flag = int(category_data[KEY_INSTALL_MODE_FLAG])
            except (TypeError, ValueError):
                install_mode_flag = 0
            entry[KEY_INSTALL_MODE_FLAG] = min(max(install_mode_flag, 0), 0xFFFFFFFF)

        # Icon persistence: accept both the nested block and the legacy flat keys.
        icon_block = category_data.get(KEY_ICON, {}) if isinstance(category_data.get(KEY_ICON, {}), dict) else {}

        glyph_mode = category_data.get(KEY_GLYPH_MODE, "auto")
        if not isinstance(glyph_mode, str):
            glyph_mode = "auto"
        glyph_mode = glyph_mode.lower()
        if glyph_mode not in {"auto", "first_letter"}:
            glyph_mode = "auto"
        entry[KEY_GLYPH_MODE] = glyph_mode

        icon_source = icon_block.get(ICON_BLOCK_SOURCE, category_data.get(KEY_ICON_SOURCE, "auto"))
        if not isinstance(icon_source, str):
            icon_source = "auto"
        icon_source = icon_source.lower()
        if icon_source not in {"auto", "manual", "off"}:
            icon_source = "auto"
        entry[KEY_ICON_SOURCE] = icon_source

        icon_key = icon_block.get(ICON_BLOCK_KEY, category_data.get(KEY_ICON_KEY, ""))
        entry[KEY_ICON_KEY] = str(icon_key) if icon_key is not None else ""

        icon_path = icon_block.get(ICON_BLOCK_PATH, category_data.get(KEY_ICON_PATH, ""))
        entry[KEY_ICON_PATH] = str(icon_path) if icon_path is not None else ""

        icon_provider = icon_block.get(ICON_BLOCK_PROVIDER, category_data.get(KEY_ICON_PROVIDER, ""))
        entry[KEY_ICON_PROVIDER] = str(icon_provider) if icon_provider is not None else ""

        # DEBUG: Log icon data loading for Brushstroke
        if 'Brushstroke' in str(category_name):
            category_debug_print(f"[NORMALIZE] Brushstroke icon data: source='{icon_source}', key='{icon_key}', path='{icon_path}'")

        return entry
    else:
        return default_entry


def _normalize_color(color):
    """Coerce an RGB value to a list of three floats clamped to ``[0, 1]``.

    Guards against legacy string-encoded channels, missing/extra channels and
    out-of-range values that would otherwise reach the UI swatches unchanged.
    """
    result = [0.0, 0.0, 0.0]
    if isinstance(color, (list, tuple)):
        for i in range(min(3, len(color))):
            try:
                c = float(color[i])
            except (TypeError, ValueError):
                c = 0.0
            result[i] = min(1.0, max(0.0, c))
    return result


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
