# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""JSON schema migrations for the Category Tabs / Glyph / Tag system.

Pure data transforms extracted from ``space_userpref.py``: they normalize a single
category entry (:func:`_normalize_category_data`) and migrate a whole mappings file
from any older on-disk schema version up to ``CURRENT_JSON_VERSION``
(:func:`migrate_json_data`, dispatched through :data:`MIGRATORS`).

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
    tag_log,
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


# -----------------------------------------------------------------------------
# JSON Migration Functions

def migrate_v1_to_v2(data):
    """Migrate v1 (string values) to v2 (dict with glyph/color)."""
    tag_log("Migrating JSON v1 → v2")
    mappings = {}
    for cat_name, glyph_str in data.get("mappings", {}).items():
        if isinstance(glyph_str, str):
            mappings[cat_name] = _normalize_category_data(glyph_str)
    return {"version": 2, "mappings": mappings}


def migrate_v2_to_v3(data):
    """Migrate v2 to v3 (add all_tags and tags arrays)."""
    tag_log("Migrating JSON v2 → v3")
    data["all_tags"] = {}  # Empty tag registry
    for cat_data in data.get("mappings", {}).values():
        if isinstance(cat_data, dict):
            cat_data["tags"] = []
    data["version"] = 3
    return data


def migrate_v3_to_v4(data):
    """Migrate v3 to v4 (add tag_order for manual ordering)."""
    tag_log("Migrating JSON v3 → v4")
    # tag_order will be populated on next save from WM collection order
    data["tag_order"] = []
    data["version"] = 4
    return data


def migrate_v4_to_v5(data):
    """Migrate from v4 to v5: add mode_flags to tags."""
    tag_log("Migrating JSON v4 → v5")
    # Add empty mode_flags to all tags (empty = all modes active)
    if "tags" in data:
        for tag in data["tags"]:
            if isinstance(tag, dict) and "mode_flags" not in tag:
                tag["mode_flags"] = []  # Empty = all modes
    data["version"] = 5
    return data


def migrate_v5_to_v6(data):
    """Migrate version 5 to 6: Add category_orders support."""
    # Add empty category_orders dict if not present
    if "category_orders" not in data:
        data["category_orders"] = {}
        category_debug_print("[MIGRATION] v5->v6: Added category_orders section")
    data["version"] = 6
    return data


def migrate_v6_to_v7(data):
    """Migrate version 6 to 7: Add icon persistence block for each category mapping."""
    mappings = data.get("mappings", {})
    if isinstance(mappings, dict):
        for _category, category_data in mappings.items():
            if not isinstance(category_data, dict):
                continue

            icon = category_data.get("icon")
            if not isinstance(icon, dict):
                icon = {}

            icon_source = icon.get("source", category_data.get("icon_source", "auto"))
            if not isinstance(icon_source, str):
                icon_source = "auto"
            icon_source = icon_source.lower()
            if icon_source not in {"auto", "manual", "off"}:
                icon_source = "auto"

            icon["source"] = icon_source
            icon["key"] = str(icon.get("key", category_data.get("icon_key", "")) or "")
            icon["path"] = str(icon.get("path", category_data.get("icon_path", "")) or "")
            icon["provider"] = str(icon.get("provider", category_data.get("icon_provider", "")) or "")
            category_data["icon"] = icon

    data["version"] = 7
    return data


def migrate_v7_to_v8(data):
    """Migrate v7 to v8: Add space_type prefixes to category order keys.

    Old format: "tag1;tag2" or "" (empty string)
    New format: "VIEW3D:tag1;tag2", "IMAGE:", "NODE:", etc.

    This ensures each editor type has its own independent category order storage.
    """
    old_orders = data.get("category_orders", {})
    if not old_orders:
        data["version"] = 8
        return data

    new_orders = {}

    # List of space type prefixes used in C++ get_space_type_prefix()
    space_prefixes = [
        "VIEW3D:",
        "IMAGE:",
        "NODE:",
        "PROPS:",
        "OUTLINER:",
        "FILE:",
        "SEQUENCE:",
        "TEXT:",
        "CLIP:",
        "SPREADSHEET:",
        "OTHER:",
    ]

    for old_key, category_list in old_orders.items():
        # Check if this key already has a space prefix (new format)
        has_space_prefix = any(old_key.startswith(prefix) for prefix in space_prefixes)

        if has_space_prefix:
            # Already in new format - keep as is
            new_orders[old_key] = category_list
        else:
            # Old format - migrate to all space types
            # This preserves existing order for all editor types
            for space_prefix in space_prefixes:
                new_key = space_prefix + old_key
                new_orders[new_key] = list(category_list) if isinstance(category_list, list) else []

    data["category_orders"] = new_orders
    data["version"] = 8
    return data


def migrate_v8_to_v9(data):
    """Migrate v8 to v9: Add extension pending-tag fields to existing categories.

    New fields per category:
      source_extension  - ID of the extension that introduced this category (empty string)
      pending_tag_assignment - whether the category is awaiting tag assignment (False)
      discovered_in_spaces   - list of space type strings where category was discovered ([])
      discovered_in_modes    - list of mode name strings where category was discovered ([])
    """
    tag_log("Migrating JSON v8 → v9")
    mappings = data.get("mappings", {})
    if isinstance(mappings, dict):
        for _space_key, categories in mappings.items():
            if not isinstance(categories, dict):
                continue
            for _cat_name, cat_data in categories.items():
                if not isinstance(cat_data, dict):
                    continue
                cat_data.setdefault("source_extension", "")
                cat_data.setdefault("pending_tag_assignment", False)
                cat_data.setdefault("discovered_in_spaces", [])
                cat_data.setdefault("discovered_in_modes", [])
    data["version"] = 9
    return data


def migrate_v9_to_v11(data):
    """Migrate from v9 (space-specific entries) to v11 (Global-First architecture).

    Consolidates all space-specific entries (SPACE_VIEW3D, SPACE_NODE, etc.)
    into a single GLOBAL entry for each category. Customizations from
    space-specific entries are merged into GLOBAL with priority given to
    manual icon settings and non-default values.
    """
    mappings = data.get("mappings", {})
    if not isinstance(mappings, dict):
        mappings = {}

    global_mappings = mappings.get("GLOBAL", {})
    if not isinstance(global_mappings, dict):
        global_mappings = {}

    # Consolidate space-specific entries into GLOBAL
    for space_key, categories in mappings.items():
        if space_key == "GLOBAL":
            continue  # Already processed
        if not isinstance(categories, dict):
            continue

        for cat_name, cat_data in categories.items():
            if not isinstance(cat_data, dict):
                continue

            if cat_name not in global_mappings:
                # Create new GLOBAL entry from space-specific
                global_mappings[cat_name] = dict(cat_data)
            else:
                # Merge customizations into existing GLOBAL entry
                existing = global_mappings[cat_name]
                if not isinstance(existing, dict):
                    existing = {}
                    global_mappings[cat_name] = existing

                # Priority: manual icon > off > auto
                migrated_icon_source = cat_data.get("icon_source", "auto")
                existing_icon_source = existing.get("icon_source", "auto")

                should_update_icon = (
                    existing_icon_source == "auto" or
                    migrated_icon_source == "manual" or
                    migrated_icon_source == "off"
                )

                if should_update_icon:
                    if migrated_icon_source in ("manual", "off"):
                        existing["icon_source"] = migrated_icon_source
                    if cat_data.get("icon_key"):
                        existing["icon_key"] = cat_data.get("icon_key")
                    if cat_data.get("icon_path"):
                        existing["icon_path"] = cat_data.get("icon_path")
                    if cat_data.get("icon_provider"):
                        existing["icon_provider"] = cat_data.get("icon_provider")

                # Merge glyph_mode if not auto
                migrated_glyph_mode = cat_data.get("glyph_mode", "auto")
                if migrated_glyph_mode != "auto" and existing.get("glyph_mode", "auto") == "auto":
                    existing["glyph_mode"] = migrated_glyph_mode

                # Merge glyph if GLOBAL doesn't have one
                migrated_glyph = cat_data.get("glyph", "")
                if migrated_glyph and not existing.get("glyph"):
                    existing["glyph"] = migrated_glyph

                # Merge color if GLOBAL doesn't have one
                migrated_color = cat_data.get("color", [0.0, 0.0, 0.0])
                if migrated_color and any(c != 0.0 for c in migrated_color):
                    if not any(c != 0.0 for c in existing.get("color", [0.0, 0.0, 0.0])):
                        existing["color"] = migrated_color

                # Merge tags (union)
                migrated_tags = cat_data.get("tags", [])
                existing_tags = existing.get("tags", [])
                if migrated_tags:
                    existing["tags"] = list(set(existing_tags) | set(migrated_tags))

                # Preserve extension-related fields if present
                for ext_field in ["source_extension", "pending_tag_assignment", "discovered_in_spaces", "discovered_in_modes"]:
                    if ext_field in cat_data and ext_field not in existing:
                        existing[ext_field] = cat_data[ext_field]

    # Replace mappings with only GLOBAL
    data["mappings"] = {"GLOBAL": global_mappings}
    data["version"] = 11
    return data


def migrate_v11_to_v12(data):
    """Migrate v11 to v12: Add icon_key and icon_source to all_tags."""
    tag_log("Migrating JSON v11 → v12 (adding tag icon support)")
    for tag_name, tag_data in data.get("all_tags", {}).items():
        if isinstance(tag_data, dict):
            # Add default icon fields
            tag_data.setdefault("icon_key", "")
            tag_data.setdefault("icon_source", 0)
    data["version"] = 12
    return data


MIGRATORS = {
    1: migrate_v1_to_v2,
    2: migrate_v2_to_v3,
    3: migrate_v3_to_v4,
    4: migrate_v4_to_v5,
    5: migrate_v5_to_v6,
    6: migrate_v6_to_v7,
    7: migrate_v7_to_v8,
    8: migrate_v8_to_v9,
    9: migrate_v9_to_v11,  # Global-First architecture migration
    11: migrate_v11_to_v12,  # Tag icon support
}


def migrate_json_data(data):
    """Migrate data to current version with validation."""
    version = data.get("version", 1)

    while version < CURRENT_JSON_VERSION:
        migrator = MIGRATORS.get(version)
        if not migrator:
            raise ValueError(f"Unknown JSON version: {version}")
        data = migrator(data)
        version = data.get("version", version + 1)
        tag_log(f"Migrated to version {version}")

    # Validate final structure
    if "all_tags" not in data:
        data["all_tags"] = {}
    if "mappings" not in data:
        data["mappings"] = {}
    if "category_orders" not in data:
        data["category_orders"] = {}

    # Fix: Convert color strings to floats if needed
    for space_type_str, categories in data.get("mappings", {}).items():
        for category, cat_data in categories.items():
            if isinstance(cat_data, dict) and "color" in cat_data:
                color = cat_data["color"]
                if isinstance(color, list) and len(color) == 3:
                    fixed_color = []
                    for c in color:
                        if isinstance(c, str):
                            try:
                                fixed_color.append(float(c))
                            except ValueError:
                                fixed_color.append(0.0)
                        else:
                            fixed_color.append(float(c) if c is not None else 0.0)
                    cat_data["color"] = fixed_color

    return data
