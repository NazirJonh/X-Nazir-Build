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
        "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0],
        "default_glyph": "", "default_display_name": "", "base_type": "text_only",
        "first_letter": "",
        "tags": [],  # NEW: array of tag names
        "mode_flags": [],  # NEW: array of mode names
        "glyph_mode": "auto",
        "icon_source": "auto",
        "icon_key": "",
        "icon_path": "",
        "icon_provider": "",
        # Extension-related fields for "New Add-ons!" feature
        "source_extension": "",
        "pending_tag_assignment": False,
        "discovered_in_spaces": [],
        "discovered_in_modes": [],
    }

    if isinstance(category_data, str):
        # Old format: just a glyph string (may be in \uXXXX format)
        glyph = _unicode_escape_to_glyph(category_data) if '\\u' in category_data else category_data
        base_type = "glyph_only" if _is_single_glyph(glyph) else "glyph_text"
        # For glyph_text categories, use category name as default_display_name for tooltip fallback
        default_display_name = "" if base_type == "glyph_only" else category_name
        return {"glyph": glyph, "display_name": "", "color": [0.0, 0.0, 0.0],
                "default_glyph": glyph, "default_display_name": default_display_name, "base_type": base_type,
                "first_letter": "",
                "tags": [],
                "glyph_mode": "auto",
                "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                "source_extension": "", "pending_tag_assignment": False,
                "discovered_in_spaces": [], "discovered_in_modes": []}
    elif isinstance(category_data, dict):
        # New format: dict with glyph, display_name, color, default_glyph, default_display_name, base_type, tags
        entry = default_entry.copy()

        # Current values
        if "glyph" in category_data:
            glyph_str = category_data["glyph"]
            if glyph_str and '\\u' in glyph_str:
                decoded_glyph = _unicode_escape_to_glyph(glyph_str)
                entry["glyph"] = decoded_glyph
                if 'Brushstroke' in str(category_name):
                    category_debug_print(f"[NORMALIZE] Brushstroke glyph decoded: '{glyph_str}' -> '{decoded_glyph}' (len={len(decoded_glyph)})")
            else:
                entry["glyph"] = glyph_str
        if "display_name" in category_data:
            entry["display_name"] = category_data["display_name"]
        if "first_letter" in category_data:
            entry["first_letter"] = category_data.get("first_letter", "") or ""
        # Derive first_letter for legacy data when missing but display_name is available.
        if not entry["first_letter"] and entry.get("display_name"):
            entry["first_letter"] = entry["display_name"][:1]
        if "color" in category_data:
            color = category_data["color"]
            if isinstance(color, (list, tuple)) and len(color) >= 3:
                entry["color"] = list(color[:3])

        # Default values (for reset)
        # IMPORTANT: Distinguish between "field is missing" vs "field is present but intentionally empty".
        # Empty default_glyph is meaningful for text_only categories (fallback letter behavior).
        if "default_glyph" in category_data:
            glyph_str = category_data["default_glyph"]
            if glyph_str and '\\u' in glyph_str:
                decoded_default = _unicode_escape_to_glyph(glyph_str)
                entry["default_glyph"] = decoded_default
                if 'Brushstroke' in str(category_name):
                    category_debug_print(f"[NORMALIZE] Brushstroke default_glyph decoded: '{glyph_str}' -> '{decoded_default}'")
            else:
                entry["default_glyph"] = glyph_str or ""
        else:
            # Backward compatibility for legacy data where default_glyph field does not exist.
            # IMPORTANT: Only set default_glyph from glyph for glyph_only categories.
            # For text_only/glyph_text categories, default_glyph should be empty (reset returns first letter).
            if entry.get("base_type") == "glyph_only":
                entry["default_glyph"] = entry["glyph"]
            else:
                entry["default_glyph"] = ""

        if "default_display_name" in category_data:
            entry["default_display_name"] = category_data["default_display_name"]
        else:
            # If no default_display_name, set based on base_type
            # For glyph_text categories, use category name for tooltip fallback
            if entry.get("base_type") == "glyph_text":
                entry["default_display_name"] = category_name
            else:
                entry["default_display_name"] = ""

        # Base type (for reset): glyph_only, glyph_text, or text_only
        if "base_type" in category_data:
            entry["base_type"] = category_data["base_type"]
        else:
            # Determine base_type from current values
            # Priority: 1) glyph field is not empty -> glyph_text or glyph_only
            #           2) category_name is a single glyph -> glyph_only
            #           3) otherwise -> text_only
            if entry["glyph"]:
                entry["base_type"] = "glyph_only" if _is_single_glyph(entry["glyph"]) else "glyph_text"
            elif category_name and _is_single_glyph(category_name):
                # Category name itself is a glyph (e.g., "" for Script 1)
                entry["base_type"] = "glyph_only"
            else:
                entry["base_type"] = "text_only"

        # Safety correction for previously serialized incorrect state:
        # text_only AND glyph_text categories must reset to fallback letter, so default_glyph must be empty.
        # Only glyph_only categories should have default_glyph set (to category name).
        if entry["base_type"] in ("text_only", "glyph_text"):
            entry["default_glyph"] = ""

        # For glyph_only categories, ensure default_glyph is set to category name (original glyph)
        if entry["base_type"] == "glyph_only" and category_name and _is_single_glyph(category_name):
            if not entry.get("default_glyph"):
                entry["default_glyph"] = category_name
                _pref_log_once(f"[GLYPH LOAD] Set default_glyph for glyph_only category '{category_name}'")

        # NEW: For reserved categories (in DEFAULT_CATEGORY_GLYPHS), restore default_glyph
        # even if glyph field is empty in JSON. This ensures Reset works correctly.
        if category_name and category_name in DEFAULT_CATEGORY_GLYPHS:
            default_data = DEFAULT_CATEGORY_GLYPHS[category_name]
            reserved_glyph = default_data.get("glyph", "")
            if reserved_glyph:
                # Only restore if default_glyph is actually empty or incorrect
                current_default = entry.get("default_glyph", "")
                if not current_default or current_default != reserved_glyph:
                    entry["default_glyph"] = reserved_glyph
                    entry["base_type"] = "glyph_text"
                    category_debug_print(f"[GLYPH] Restored default_glyph for reserved category '{category_name}': '{reserved_glyph}'")

        # NEW: Tags
        if "tags" in category_data:
            tags = category_data["tags"]
            if isinstance(tags, list):
                entry["tags"] = [str(t) for t in tags]

        # Extension-related fields for "New Add-ons!" feature
        if "source_extension" in category_data:
            entry["source_extension"] = str(category_data["source_extension"])
        if "pending_tag_assignment" in category_data:
            val = category_data["pending_tag_assignment"]
            entry["pending_tag_assignment"] = bool(val) if val else False
        if "discovered_in_spaces" in category_data:
            spaces = category_data["discovered_in_spaces"]
            if isinstance(spaces, list):
                entry["discovered_in_spaces"] = [str(s) for s in spaces]
        if "discovered_in_modes" in category_data:
            modes = category_data["discovered_in_modes"]
            if isinstance(modes, list):
                entry["discovered_in_modes"] = [str(m) for m in modes]

        # Icon persistence (v7): accept both nested block and legacy flat keys.
        icon_block = category_data.get("icon", {}) if isinstance(category_data.get("icon", {}), dict) else {}

        glyph_mode = category_data.get("glyph_mode", "auto")
        if not isinstance(glyph_mode, str):
            glyph_mode = "auto"
        glyph_mode = glyph_mode.lower()
        if glyph_mode not in {"auto", "first_letter"}:
            glyph_mode = "auto"
        entry["glyph_mode"] = glyph_mode

        icon_source = icon_block.get("source", category_data.get("icon_source", "auto"))
        if not isinstance(icon_source, str):
            icon_source = "auto"
        icon_source = icon_source.lower()
        if icon_source not in {"auto", "manual", "off"}:
            icon_source = "auto"
        entry["icon_source"] = icon_source

        icon_key = icon_block.get("key", category_data.get("icon_key", ""))
        entry["icon_key"] = str(icon_key) if icon_key is not None else ""

        icon_path = icon_block.get("path", category_data.get("icon_path", ""))
        entry["icon_path"] = str(icon_path) if icon_path is not None else ""

        icon_provider = icon_block.get("provider", category_data.get("icon_provider", ""))
        entry["icon_provider"] = str(icon_provider) if icon_provider is not None else ""

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
    if not isinstance(data.get("all_tags"), dict):
        data["all_tags"] = {}
    if not isinstance(data.get("mappings"), dict):
        data["mappings"] = {}
    if not isinstance(data.get("category_orders"), dict):
        data["category_orders"] = {}

    # ``tag_order`` must be a list of strings.
    tag_order = data.get("tag_order")
    data["tag_order"] = (
        [t for t in tag_order if isinstance(t, str)] if isinstance(tag_order, list) else []
    )

    # ``category_orders`` values must be lists (the loader decodes them as lists).
    data["category_orders"] = {
        key: value for key, value in data["category_orders"].items() if isinstance(value, list)
    }

    # Normalize tag colors.
    for tag_data in data["all_tags"].values():
        if isinstance(tag_data, dict) and "color" in tag_data:
            tag_data["color"] = _normalize_color(tag_data.get("color"))

    # Normalize category colors within the GLOBAL mappings block.
    for categories in data["mappings"].values():
        if not isinstance(categories, dict):
            continue
        for cat_data in categories.values():
            if isinstance(cat_data, dict) and "color" in cat_data:
                cat_data["color"] = _normalize_color(cat_data.get("color"))

    data["version"] = CURRENT_JSON_VERSION
    return data
