# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Glyph cache — persistence, category data, and glyph setters for the Tabs-System.

Extracted verbatim from ``space_userpref.py`` (no behavioural change).
All state lives in glyph_tag_system._state; this module imports state objects
by reference (in-place mutations) and uses accessors for full reassignment.

Cross-module calls to ``_auto_sync_to_wm``, ``_auto_save_tags``,
``_auto_save_glyph_mappings``, ``sync_glyph_mappings_to_wm`` and
``_auto_detect_extension_icon_path`` use lazy imports inside function bodies
to avoid a circular dependency with space_userpref.

The public names are re-imported in space_userpref to preserve its attribute
contract for the C++ bridge and editor modules.
"""

import bpy
import json
import os
import time

from bl_ui.glyph_tag_system.defaults import (
    CURRENT_JSON_VERSION,
    DEFAULT_CATEGORY_GLYPHS,
    GLYPHS_FILENAME,
    SPACE_TO_FLAG,
    _CATEGORY_TAG_DEFAULT_MODE_FLAGS,
)
from bl_ui.glyph_tag_system.conversions import (
    _category_order_decode,
    _category_order_encode,
    _glyph_to_hex,
    _glyph_to_unicode_escape,
    _hex_to_glyph,
    _is_single_glyph,
    _is_valid_category_name,
    _make_cache_key,
    _normalize_category_key,
    _space_type_id_to_str,
    _unicode_escape_to_glyph,
    flags_to_modes,
    flags_to_spaces,
    modes_to_flags,
    spaces_to_flags,
)
from bl_ui.glyph_tag_system.log import (
    category_debug_print,
    save_debug_print,
    tag_log,
    _pref_log_once,
)
from bl_ui.glyph_tag_system.migrations import (
    _normalize_category_data,
    migrate_json_data,
)
from bl_ui.glyph_tag_system.persistence import (
    safe_file_write,
    load_json_safely,
    create_backup,
)
from bl_ui.glyph_tag_system._state import (
    state,
    reset_glyph_cache,
    reset_all_tags_cache,
    reset_category_orders_cache,
    set_glyph_cache_loaded,
    set_glyph_save_lock,
    set_tag_order,
    is_glyph_cache_loaded,
    is_glyph_save_locked,
)

# Import glyph library for integration.
try:
    from bl_ui.glyph_library import get_glyph_library
except ImportError:
    get_glyph_library = None


# -----------------------------------------------------------------------------
# Small utility helpers (no external deps beyond conversions / _state)
# -----------------------------------------------------------------------------


def _is_popular_addons_database_extension(extension_id):
    """Check if extension_id is Popular Addons Database."""
    if not isinstance(extension_id, str):
        return False
    normalized = _normalize_category_key(extension_id)
    return normalized.endswith("popularaddonsdatabase") or normalized == "popularaddonsdatabase"


def _is_collection_safe(collection):
    """Check if a bpy_prop_collection is safe to access without triggering crashes."""
    try:
        # Test access without triggering iteration - just check if we can get the RNA type
        # This is a minimal operation that shouldn't trigger ListBase traversal
        _ = collection.bl_rna
        return True
    except (AttributeError, RuntimeError, ReferenceError):
        return False


def _get_glyphs_filepath():
    """Get the path to the glyph mappings JSON file."""
    import bpy.utils
    config_dir = bpy.utils.user_resource('CONFIG')
    if config_dir:
        return os.path.join(config_dir, GLYPHS_FILENAME)
    return None


# -----------------------------------------------------------------------------
# Panel label helpers
# -----------------------------------------------------------------------------


def _find_panel_label_for_category(category):
    """Find human-readable panel label for a given bl_category using registered panels."""
    try:
        import bpy
        from bpy.types import Panel
    except ImportError:
        return None

    if not category:
        return None

    # First, search through Panel subclasses (typically contains all registered panels).
    try:
        for panel_cls in Panel.__subclasses__():
            try:
                if getattr(panel_cls, "bl_category", None) == category:
                    label = getattr(panel_cls, "bl_label", "") or ""
                    if label:
                        return label
            except Exception:
                continue
    except Exception:
        pass

    # Fallback: iterate over bpy.types attributes to find Panel-like classes with matching category.
    try:
        for type_name in dir(bpy.types):
            try:
                type_obj = getattr(bpy.types, type_name)
            except AttributeError:
                continue

            try:
                if getattr(type_obj, "bl_category", None) == category:
                    label = getattr(type_obj, "bl_label", "") or ""
                    if label:
                        return label
            except Exception:
                continue
    except Exception:
        pass

    return None


def _ensure_category_panel_label(category, space_type, entry):
    """Ensure category has a default_display_name using Panel bl_label as fallback.

    This is used when glyph mappings were created before addon panels registered,
    so JSON lacks human-readable names for glyph-only categories.
    """
    if not category:
        return entry

    # Decide if we need to look up a panel label.
    needs_lookup = False
    if isinstance(entry, dict):
        glyph = entry.get("glyph", "") or ""
        default_glyph = entry.get("default_glyph", "") or ""
        effective_glyph = glyph or default_glyph
        default_display_name = entry.get("default_display_name", "") or ""

        glyph_equals_category = bool(effective_glyph and effective_glyph == category)
        # Fallback conditions:
        # - No default_display_name yet
        # - OR glyph/category suggests glyph-only category (glyph == category)
        if not default_display_name or glyph_equals_category:
            needs_lookup = True
    else:
        # No cache entry at all, try to discover from panels.
        needs_lookup = True

    if not needs_lookup:
        return entry

    panel_label = _find_panel_label_for_category(category)
    if not panel_label:
        return entry

    # Create a normalized entry if none exists yet.
    if not isinstance(entry, dict):
        entry = _normalize_category_data({}, category_name=category)

    # Populate default_display_name / display_name ONLY for glyph_only categories
    # (e.g., "Script 1" for category "" which is a single glyph)
    # For text_only/glyph_text categories, leave display_name empty to use category name as fallback
    is_glyph_only = _is_single_glyph(category)
    if is_glyph_only:
        if not entry.get("default_display_name"):
            entry["default_display_name"] = panel_label
        if not entry.get("display_name"):
            entry["display_name"] = panel_label

    # For glyph-only categories, also ensure glyph/default_glyph/base_type are set.
    if is_glyph_only:
        if not entry.get("glyph"):
            entry["glyph"] = category
        if not entry.get("default_glyph"):
            entry["default_glyph"] = category
        if not entry.get("base_type"):
            entry["base_type"] = "glyph_only"

    # Persist the discovered label into the cache for all matching category entries.
    updated_any = False
    for cache_key, cache_data in list(state.glyph_cache.items()):
        if isinstance(cache_key, tuple) and len(cache_key) >= 2 and cache_key[1] == category:
            if not isinstance(cache_data, dict):
                cache_data = _normalize_category_data(cache_data, category_name=category)

            # Only set display_name for glyph_only categories
            if is_glyph_only:
                if not cache_data.get("default_display_name"):
                    cache_data["default_display_name"] = panel_label
                if not cache_data.get("display_name"):
                    cache_data["display_name"] = panel_label

            if is_glyph_only:
                if not cache_data.get("glyph"):
                    cache_data["glyph"] = category
                if not cache_data.get("default_glyph"):
                    cache_data["default_glyph"] = category
                if not cache_data.get("base_type"):
                    cache_data["base_type"] = "glyph_only"

            state.glyph_cache[cache_key] = cache_data
            updated_any = True

    if not updated_any:
        # If there was no existing cache entry, store under the requested space_type
        # (or global if none specified).
        target_space = space_type if space_type != -1 else -1
        cache_key = _make_cache_key(target_space, category)
        state.glyph_cache[cache_key] = entry

    return entry


# -----------------------------------------------------------------------------
# Category data accessors
# -----------------------------------------------------------------------------


def _get_category_data(category, space_type=-1):
    """Get category data using GLOBAL-only lookup (Global-First architecture).

    All category data is stored under GLOBAL key (-1, category) for a single
    source of truth across all editor types. The space_type parameter is
    preserved for API compatibility but is ignored for cache lookup.
    """
    if not is_glyph_cache_loaded():
        _load_glyph_mappings_from_file()

    # Helper function to check if data has meaningful content
    def has_meaningful_data(data):
        if not isinstance(data, dict):
            return False
        # CRITICAL FIX: Consider glyph, tags, AND icon as "meaningful" for fallback logic
        # This prevents entries with empty glyphs but valid icons from being skipped
        glyph = data.get("glyph", "")
        default_glyph = data.get("default_glyph", "")
        has_glyph = bool(glyph or default_glyph)

        # Check for tags as well - tags are meaningful data too
        tags = data.get("tags", [])
        has_tags = bool(tags)

        # Check for icon customization - icon_key/icon_path are meaningful
        icon_source = data.get("icon_source", "auto")
        icon_key = data.get("icon_key", "")
        icon_path = data.get("icon_path", "")
        has_icon = (icon_source in ("manual", "off")) or bool(icon_key) or bool(icon_path)

        # glyph OR tags OR icon presence determines "meaningful" data
        return has_glyph or has_tags or has_icon

    # Global-First: Always use GLOBAL key (-1, category)
    key = _make_cache_key(space_type, category)  # Returns (-1, category) regardless of space_type
    category_debug_print(f"[_get_category_data] Looking for GLOBAL key={key}, category='{category}'")
    if key in state.glyph_cache:
        data = state.glyph_cache[key]
        meaningful = has_meaningful_data(data)
        category_debug_print(f"[_get_category_data] FOUND GLOBAL key={key}, has_meaningful={meaningful}")
        if meaningful:
            return _ensure_category_panel_label(category, space_type, data)

    # Canonicalization fallback: search for category by normalized key
    # This handles cases where category names have different spellings/unicode
    normalized_target = _normalize_category_key(category)
    if normalized_target:
        for cache_key, cache_data in state.glyph_cache.items():
            if isinstance(cache_key, tuple) and len(cache_key) >= 2:
                cache_category = cache_key[1]
                if _normalize_category_key(cache_category) == normalized_target:
                    if has_meaningful_data(cache_data):
                        category_debug_print(f"[_get_category_data] Found via canonicalization: {cache_key}")
                        return _ensure_category_panel_label(category, space_type, cache_data)

    category_debug_print(f"[_get_category_data] No entry found for '{category}', returning None")
    return _ensure_category_panel_label(category, space_type, None)


def _set_category_data_internal(category, data, space_type=-1):
    """Set category data for specific space_type."""
    key = _make_cache_key(space_type, category)
    state.glyph_cache[key] = data


def mark_category_from_extension(category_id, extension_id, space_type=-1, mode_flag=0):
    """Mark a category as originating from an extension and pending tag assignment.

    Called when a new extension is installed and introduces a previously unknown category.

    Args:
        category_id:   Category name (e.g. "Brushstroke Tools").
        extension_id:  Extension package ID (e.g. "blender_org/brushstroke_tools").
        space_type:    Integer space type (eSpace_Type), or -1 for global.
        mode_flag:     Bitmask of mode flags where the category was discovered.
    """
    if _is_popular_addons_database_extension(extension_id):
        tag_log(f"mark_category_from_extension: skipped PAD source extension for category={category_id!r}")
        return

    key = _make_cache_key(space_type, category_id)
    cat_data = state.glyph_cache.get(key)
    if cat_data is None:
        # Also try global key as fallback
        global_key = _make_cache_key(-1, category_id)
        cat_data = state.glyph_cache.get(global_key)
        if cat_data is not None:
            key = global_key
        else:
            # Category not yet in cache — create a minimal entry
            cat_data = _normalize_category_data({})
            # New categories from extensions start as pending tag assignment.
            cat_data["pending_tag_assignment"] = True
            state.glyph_cache[key] = cat_data

    if not isinstance(cat_data, dict):
        cat_data = _normalize_category_data(cat_data)
        state.glyph_cache[key] = cat_data

    cat_data["source_extension"] = extension_id

    # Do not re-enable pending state for categories that are already tagged.
    # This prevents re-discovery (e.g. in another editor space) from undoing
    # a successful deferred tag assignment.
    has_assigned_tags = bool(cat_data.get("tags", []))
    if has_assigned_tags:
        if cat_data.get("pending_tag_assignment", False):
            cat_data["pending_tag_assignment"] = False
        tag_log(
            f"mark_category_from_extension: preserved pending=False for tagged category "
            f"{category_id!r} (tags={cat_data.get('tags', [])})"
        )
    else:
        # Only mark as pending if it hasn't been explicitly addressed yet (False).
        # This prevents distributed categories from reappearing in "New Add-ons!"
        # if all tags are subsequently removed.
        if cat_data.get("pending_tag_assignment", True):
            cat_data["pending_tag_assignment"] = True

    # Merge discovered_in_spaces
    existing_spaces_flags = spaces_to_flags(cat_data.get("discovered_in_spaces", []))
    if space_type != -1:
        space_str = _space_type_id_to_str(space_type)
        existing_spaces_flags |= SPACE_TO_FLAG.get(space_str, 0)
    cat_data["discovered_in_spaces"] = flags_to_spaces(existing_spaces_flags)

    # Merge discovered_in_modes
    existing_modes_flags = modes_to_flags(cat_data.get("discovered_in_modes", []))
    existing_modes_flags |= mode_flag
    cat_data["discovered_in_modes"] = flags_to_modes(existing_modes_flags)

    tag_log(f"mark_category_from_extension: category={category_id!r}, extension={extension_id!r}, "
            f"space_type={space_type}, mode_flag={mode_flag:#010x}")

    # Debounced sync and save to prevent lags during rapid discovery
    from bl_ui.glyph_tag_system import wm_sync_to_wm as _wm_sync
    from bl_ui.glyph_tag_system import handlers as _handlers
    _wm_sync._auto_sync_to_wm()
    _handlers._auto_save_tags()


def mark_all_unassigned_categories_as_without_tag(space_type=-1, mode_flag=0):
    """Mark all unassigned categories as 'Without Tag' (pending=False).

    This is called when Alt+Click on "New Add-ons!" button.
    Handles both categories with source_extension and categories without extension
    (using prefix matching for siblings like "Home Builder" <-> "Home Builder 5").
    """
    updated = 0
    categories_to_clear = []  # Collect categories that were cleared for sibling processing

    # First pass: mark all unassigned categories as without tag
    for cache_key, cat_data in list(state.glyph_cache.items()):
        if not (isinstance(cache_key, tuple) and len(cache_key) == 2):
            continue
        cache_space_type, category_name = cache_key
        if cache_space_type != -1:
            continue
        if not isinstance(cat_data, dict):
            continue
        if category_name in DEFAULT_CATEGORY_GLYPHS:
            continue
        # CRITICAL: Process ALL categories with pending=True, regardless of source_extension
        # This includes categories like "Home Builder" (ext='') and "Home Builder 5" (ext='...')
        if not cat_data.get("pending_tag_assignment", False):
            continue
        if cat_data.get("tags"):
            continue

        discovered_spaces = spaces_to_flags(cat_data.get("discovered_in_spaces", []))
        if space_type != -1 and discovered_spaces and not (discovered_spaces & SPACE_TO_FLAG.get(_space_type_id_to_str(space_type), 0)):
            continue

        effective_mode_flags = modes_to_flags(cat_data.get("discovered_in_modes", []))
        if effective_mode_flags == 0:
            effective_mode_flags = int(cat_data.get("install_mode_flag", 0))
        if mode_flag and effective_mode_flags and not (effective_mode_flags & mode_flag):
            continue

        cat_data["pending_tag_assignment"] = False
        categories_to_clear.append(category_name)
        updated += 1

    # Second pass: clear pending for sibling categories
    # This ensures that when one category from an extension is cleared,
    # all siblings are also cleared (even if they weren't in the original list)
    cleared_siblings = set()
    for category_name in categories_to_clear:
        key = _make_cache_key(-1, category_name)
        cat_data = state.glyph_cache.get(key)
        if not isinstance(cat_data, dict):
            continue

        source_ext = cat_data.get("source_extension", "")
        cleared_count = 0

        if source_ext:
            # Primary path: match by source_extension
            for other_key, other_data in state.glyph_cache.items():
                if isinstance(other_data, dict) and other_data.get("source_extension") == source_ext:
                    other_name = other_key[1] if isinstance(other_key, tuple) else other_key
                    if other_name != category_name:  # Skip self
                        if other_data.get("pending_tag_assignment", False) and not other_data.get("tags"):
                            other_data["pending_tag_assignment"] = False
                            cleared_siblings.add(other_name)
                            cleared_count += 1
        else:
            # FALLBACK: If source_extension is empty, find categories by name prefix matching
            # This handles cases like "Home Builder" (ext='') and "Home Builder 5" (ext='add-on-...')
            base_name = category_name.rstrip('0123456789').rstrip()  # Remove trailing numbers
            if not base_name:
                base_name = category_name

            for other_key, other_data in state.glyph_cache.items():
                if not isinstance(other_data, dict):
                    continue
                other_name = other_key[1] if isinstance(other_key, tuple) else other_key
                # Skip self
                if other_name == category_name:
                    continue
                # Skip if already has tags or not pending
                if other_data.get("tags") or not other_data.get("pending_tag_assignment", False):
                    continue
                # Check if other category starts with base_name or base_name starts with other
                other_ext = other_data.get("source_extension", "")
                if other_ext and (other_name.startswith(base_name) or base_name.startswith(other_name)):
                    other_data["pending_tag_assignment"] = False
                    cleared_siblings.add(other_name)
                    cleared_count += 1

        if cleared_count > 0:
            category_debug_print(f"[ALT_CLICK] Cleared {cleared_count} siblings for '{category_name}'")

    if updated or cleared_siblings:
        from bl_ui.glyph_tag_system import wm_sync_to_wm as _wm_sync
        from bl_ui.glyph_tag_system import handlers as _handlers
        _wm_sync._auto_sync_to_wm()
        _handlers._auto_save_tags()

    total_cleared = updated + len(cleared_siblings)
    tag_log(f"mark_all_unassigned_categories_as_without_tag: updated={updated}, siblings_cleared={len(cleared_siblings)}, total={total_cleared}")
    return total_cleared


def assign_tag_to_category(category_id, tag_name, space_type=-1):
    """Assign a tag to a category and clear its pending_tag_assignment flag.

    Operates on the GLOBAL mapping (space_type=-1) so the assignment is
    visible across all editor types.

    If the category doesn't exist in the cache yet (e.g., new extension category),
    creates a new entry automatically.

    Args:
        category_id:  Category name.
        tag_name:     Tag name to assign.
        space_type:   Space type to look up first; falls back to global (-1).
    """
    matching_keys = []
    for cache_key in state.glyph_cache.keys():
        if isinstance(cache_key, tuple) and len(cache_key) == 2:
            _st, key_category = cache_key
            if key_category == category_id:
                matching_keys.append(cache_key)

    global_key = _make_cache_key(-1, category_id)
    if global_key not in state.glyph_cache:
        source_data = None
        if matching_keys:
            source_data = state.glyph_cache.get(matching_keys[0])

        if isinstance(source_data, dict):
            state.glyph_cache[global_key] = _normalize_category_data(dict(source_data), category_id)
        else:
            state.glyph_cache[global_key] = {
                "tags": [],
                "pending_tag_assignment": False,
                "source_extension": "",
                "discovered_in_spaces": [],
                "discovered_in_modes": [],
            }

    if global_key not in matching_keys:
        matching_keys.append(global_key)

    if not matching_keys:
        tag_log(f"assign_tag_to_category: creating new cache entry for {category_id!r}")
        matching_keys = [global_key]

    # First, get the source_extension for this category to find all sibling categories
    source_ext = None
    category_name_for_prefix_match = None
    for key in matching_keys:
        cat_data = state.glyph_cache.get(key)
        if isinstance(cat_data, dict):
            source_ext = cat_data.get("source_extension", "")
            category_name_for_prefix_match = key[1]  # Store category name for prefix matching
            break

    for key in matching_keys:
        cat_data = state.glyph_cache.get(key)
        if not isinstance(cat_data, dict):
            cat_data = _normalize_category_data(cat_data)
            state.glyph_cache[key] = cat_data

        # CRITICAL: If an entry was NOT in cache and is now being assigned
        # a tag, ensure it starts with pending=True so it can be cleared below.
        if "pending_tag_assignment" not in cat_data:
             cat_data["pending_tag_assignment"] = True

        tags = cat_data.setdefault("tags", [])
        if tag_name not in tags:
            tags.append(tag_name)

        cat_data["pending_tag_assignment"] = False

    # CRITICAL: Clear pending_tag_assignment for ALL categories from the same extension
    # This ensures that when a user assigns a tag to any category from an extension,
    # all other categories from that extension disappear from "New Add-ons!" filter
    cleared_count = 0
    if source_ext:
        # Primary path: match by source_extension
        for other_key, other_data in state.glyph_cache.items():
            if isinstance(other_data, dict) and other_data.get("source_extension") == source_ext:
                other_data["pending_tag_assignment"] = False
                category_debug_print(f"[ASSIGN TAG] Cleared pending for sibling category: {other_key[1]!r} (extension={source_ext!r})")
                cleared_count += 1
    elif category_name_for_prefix_match:
        # FALLBACK: If source_extension is empty, find categories by name prefix matching
        # This handles cases like "Home Builder" (ext='') and "Home Builder 5" (ext='add-on-...')
        # Strategy: Find all categories that start with the same base name or contain it as a prefix
        base_name = category_name_for_prefix_match.rstrip('0123456789').rstrip()  # Remove trailing numbers
        if not base_name:
            base_name = category_name_for_prefix_match

        for other_key, other_data in state.glyph_cache.items():
            if not isinstance(other_data, dict):
                continue
            other_name = other_key[1]
            # Skip self
            if other_name == category_name_for_prefix_match:
                continue
            # Check if other category starts with base_name or base_name starts with other
            # This catches: "Home Builder" <-> "Home Builder 5", "MPFB" <-> "MPFB v2.0.14"
            other_ext = other_data.get("source_extension", "")
            if other_ext and (other_name.startswith(base_name) or base_name.startswith(other_name)):
                other_data["pending_tag_assignment"] = False
                category_debug_print(f"[ASSIGN TAG] Cleared pending for prefix-matched sibling: {other_name!r} (base={base_name!r}, extension={other_ext!r})")
                cleared_count += 1
            # Also check: if other category has no extension but similar name, clear it too
            elif not other_ext and (other_name.startswith(base_name) or base_name.startswith(other_name)):
                other_data["pending_tag_assignment"] = False
                category_debug_print(f"[ASSIGN TAG] Cleared pending for prefix-matched sibling (no ext): {other_name!r} (base={base_name!r})")
                cleared_count += 1

    # Sync to window manager (DNA) so the change is reflected in C++ code
    from bl_ui.glyph_tag_system import wm_sync_to_wm as _wm_sync
    _wm_sync.sync_glyph_mappings_to_wm()

    tag_log(f"assign_tag_to_category: category={category_id!r}, tag={tag_name!r}, pending cleared keys={len(matching_keys)}, extension={source_ext!r}, siblings_cleared={cleared_count}")
    return True


# -----------------------------------------------------------------------------
# Persistence: load / save
# -----------------------------------------------------------------------------


def _load_glyph_mappings_from_file():
    """Load glyph mappings from JSON file with migration support."""
    filepath = _get_glyphs_filepath()
    category_debug_print(f"[GLYPH] JSON storage path: {filepath}")
    category_debug_print(f"[GLYPH] File exists: {os.path.exists(filepath) if filepath else 'N/A'}")

    default_structure = {
        "version": CURRENT_JSON_VERSION,
        "all_tags": {},
        "mappings": {},
        "category_orders": {}
    }

    if not filepath:
        category_debug_print(f"[GLYPH] No valid config path, using defaults")
        reset_glyph_cache(DEFAULT_CATEGORY_GLYPHS.copy())
        reset_all_tags_cache()
        set_glyph_cache_loaded(True)
        return False

    data = load_json_safely(filepath, default_structure)

    # Normalize the loaded structure (ensure required sections exist, coerce colors).
    # The schema starts at version 1 (baseline), so there is no cross-version migration.
    data = migrate_json_data(data)

    # Load into caches with space_type support
    reset_glyph_cache()
    raw_mappings = data.get('mappings', {})
    raw_orders = data.get("category_orders", {})
    order_categories = set()
    for _tag_key, category_list in raw_orders.items():
        if isinstance(category_list, list):
            decoded_list = _category_order_decode(category_list)
            for category in decoded_list:
                if isinstance(category, str):
                    order_categories.add(category)

    # Load mappings - Global-First Architecture: only GLOBAL entries are persisted.
    # The schema starts at version 1 (baseline), so `mappings` always has the form
    # {"GLOBAL": {category: data}}; no legacy space-specific or bare-category handling.
    if isinstance(raw_mappings, dict):
        global_mappings = raw_mappings.get("GLOBAL", {})
        if isinstance(global_mappings, dict) and global_mappings:
            _pref_log_once(f"[GLYPH LOAD DEBUG] Loading GLOBAL mappings -> {len(global_mappings)} categories")
            for category, cat_data in global_mappings.items():
                if isinstance(cat_data, (str, dict)):
                    decoded_category = _unicode_escape_to_glyph(category) if '\\u' in category else category
                    cache_key = _make_cache_key(-1, decoded_category)  # Always GLOBAL key
                    normalized_data = _normalize_category_data(cat_data, decoded_category)
                    state.glyph_cache[cache_key] = normalized_data
                    # DEBUG: Log Pivot Tools loading
                    if 'Pivot' in str(category):
                        category_debug_print(f"[GLYPH LOAD DEBUG] Loaded 'Pivot Tools': cache_key={cache_key}, tags={normalized_data.get('tags', [])}")

    # Ensure invalid categories referenced in order lists are kept in cache (as GLOBAL)
    for category in order_categories:
        global_key = _make_cache_key(-1, category)
        if global_key not in state.glyph_cache and not _is_valid_category_name(category):
            state.glyph_cache[global_key] = _normalize_category_data({}, category)
            category_debug_print(f"[GLYPH] Adding missing invalid category from order list: {repr(category)}")

    # Load all_tags cache - convert hex glyphs to Unicode
    raw_tags = data.get("all_tags", {})
    category_debug_print(f"[TAGS LOAD] Loading {len(raw_tags)} tags from JSON")
    category_debug_print(f"[TAGS LOAD] raw_tags keys: {list(raw_tags.keys())}")
    reset_all_tags_cache()
    for tag_name, tag_data in raw_tags.items():
        category_debug_print(f"[TAGS LOAD] Processing tag '{tag_name}': {tag_data}")
        if isinstance(tag_data, dict):
            state.all_tags_cache[tag_name] = {
                "glyph": _hex_to_glyph(tag_data.get("glyph", "")),
                "color": tag_data.get("color", [0.0, 0.0, 0.0]),
                "mode_flags": tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS),
                "icon_key": tag_data.get("icon_key", ""),
                "icon_source": tag_data.get("icon_source", 0),
            }
            category_debug_print(f"[TAGS LOAD] Loaded tag '{tag_name}' -> icon_key='{state.all_tags_cache[tag_name]['icon_key']}' icon_source={state.all_tags_cache[tag_name]['icon_source']}")
        else:
            state.all_tags_cache[tag_name] = tag_data

    # Load tag order for preserving manual ordering
    set_tag_order(data.get("tag_order", []))

    # Load category orders
    reset_category_orders_cache()
    for tag_key, category_list in raw_orders.items():
        if isinstance(category_list, list):
            state.category_orders_cache[tag_key] = _category_order_decode(category_list)

    set_glyph_cache_loaded(True)
    category_debug_print(f"[GLYPH] Loaded {len(state.glyph_cache)} GLOBAL mappings, {len(state.all_tags_cache)} tags from {filepath}")
    return True


def _save_glyph_mappings_to_file(data=None, force_discovery_skip=False, skip_wm_sync=False):
    """Save glyph mappings to JSON file with glyphs in \\uXXXX format.

    Args:
        data: JSON data to save (if None, builds from current cache)
        force_discovery_skip: If True, skip saving when file exists (used for discovery)
        skip_wm_sync: If True, skip WM sync operations after saving (optimization for Save button)
    """
    import time
    # CRITICAL FIX: Declare ALL global variables at the very beginning, before any usage

    # CRITICAL DEBUG: Always log save attempts (even if TAG_DEBUG=False)
    save_debug_print(f"[GLYPH SAVE] >>>>>> START _save_glyph_mappings_to_file <<<<<<")
    save_debug_print(f"[GLYPH SAVE] data={data is not None}, force_discovery_skip={force_discovery_skip}, skip_wm_sync={skip_wm_sync}")
    save_debug_print(f"[GLYPH SAVE] state.glyph_cache entries: {len(state.glyph_cache)}")
    save_debug_print(f"[GLYPH SAVE] state.all_tags_cache entries: {len(state.all_tags_cache)}")

    # Prevent parallel saves - if already saving, skip this call
    if is_glyph_save_locked():
        save_debug_print(f"[GLYPH SAVE] SKIPPED - another save is in progress (_glyph_save_lock={is_glyph_save_locked()})")
        category_debug_print("[GLYPH SAVE] SKIPPED - another save is in progress")
        return False

    set_glyph_save_lock(True)
    save_start_time = time.perf_counter()
    category_debug_print(f"[GLYPH SAVE] >>>>>> START _save_glyph_mappings_to_file <<<<<<")

    try:
        filepath = _get_glyphs_filepath()
        if not filepath:
            save_debug_print(f"[GLYPH SAVE] ERROR: No filepath returned from _get_glyphs_filepath()")
            set_glyph_save_lock(False)  # Release lock before returning to avoid blocking future saves
            tag_log("No filepath for saving", "ERROR")
            return False
        save_debug_print(f"[GLYPH SAVE] Filepath: {filepath!r}")
    except Exception as e:
        save_debug_print(f"[GLYPH SAVE] ERROR getting filepath: {type(e).__name__}: {e}")
        set_glyph_save_lock(False)
        tag_log(f"Failed to get filepath: {e}", "ERROR")
        return False

    # CRITICAL: Only block discovery saves, allow user changes to be saved
    if force_discovery_skip and data is None and os.path.exists(filepath):
        save_debug_print(f"[GLYPH SAVE] Skipping auto-discovery save (force_discovery_skip=True and file exists)")
        save_debug_print(f"[GLYPH SAVE] Releasing lock before return (_glyph_save_lock={is_glyph_save_locked()})")
        set_glyph_save_lock(False)  # CRITICAL FIX: Release lock before returning!
        tag_log(f"Skipping auto-discovery save - preserving existing customizations in {filepath}", "INFO")
        return True  # Return True to indicate "success" (preservation is the goal)

    # Backup and disk writing is deferred to background thread using thread_task

    if data is None:
        # Ensure directory exists
        config_dir = os.path.dirname(filepath)
        if not os.path.exists(config_dir):
            category_debug_print(f"[GLYPH] Creating config directory: {config_dir}")
            os.makedirs(config_dir, exist_ok=True)

        def _has_user_customizations(category_data):
            """Check if category has user customizations (display_name, color, glyph, or tags)."""
            if isinstance(category_data, dict):
                display_name = category_data.get("display_name", "")
                color = category_data.get("color", [0.0, 0.0, 0.0])
                tags = category_data.get("tags", [])
                glyph = category_data.get("glyph", "")
                default_glyph = category_data.get("default_glyph", "")
                glyph_mode = str(category_data.get("glyph_mode", "auto")).lower()
                icon_source = str(category_data.get("icon_source", "auto")).lower()
                icon_key = category_data.get("icon_key", "")
                icon_path = category_data.get("icon_path", "")
                icon_provider = category_data.get("icon_provider", "")
                icon_customized = (icon_source != "auto") or bool(icon_key) or bool(icon_path) or bool(icon_provider)
                glyph_mode_customized = glyph_mode != "auto"
                # Check if display_name is not empty, color is not default black, has glyph, has tags, or icon customized.
                # IMPORTANT: glyph and default_glyph must be checked to save glyph_only categories properly.
                has_glyph_customization = bool(glyph) or bool(default_glyph)
                return bool(display_name) or color != [0.0, 0.0, 0.0] or has_glyph_customization or bool(tags) or icon_customized or glyph_mode_customized
            return False

        # Convert glyphs to Unicode escape format for reliable storage
        # Use nested structure: {space_type: {category: data}}
        mappings_to_save = {}
        skipped_count = 0
        order_categories = set()
        for _tag_key, category_list in state.category_orders_cache.items():
            if isinstance(category_list, list):
                for category in category_list:
                    if isinstance(category, str):
                        order_categories.add(category)

        # DEBUG: Log total cache entries before save with timing
        prepare_start = time.perf_counter()
        category_debug_print(f"[GLYPH SAVE] Step 1: Building data structure from {len(state.glyph_cache)} cache entries...")

        # Iterate over tuple keys (space_type, category) from state.glyph_cache
        for cache_key, category_data in state.glyph_cache.items():
            # Unpack tuple key: (space_type, category)
            if not isinstance(cache_key, tuple) or len(cache_key) != 2:
                # Skip invalid keys (shouldn't happen with new cache)
                category_debug_print(f"[GLYPH] Skipping invalid cache key: {repr(cache_key)}")
                continue

            space_type, category = cache_key
            # Convert space_type ID to string (e.g., 1 -> "SPACE_VIEW3D", -1 -> "GLOBAL")
            space_type_str = _space_type_id_to_str(space_type)

            # Global-First Architecture: Only save GLOBAL entries (space_type == -1)
            # Skip any entries that somehow have space_type != -1 (shouldn't happen after fix)
            if space_type != -1:
                category_debug_print(f"[GLYPH SAVE] SKIPPED non-GLOBAL entry: key={cache_key}, category='{category}'")
                continue

            # DEBUG: Log each category being processed
            cat_tags = category_data.get("tags", []) if isinstance(category_data, dict) else []
            _pref_log_once(f"[GLYPH SAVE] Processing: key={cache_key}, category='{category}'")

            # Initialize GLOBAL dict if needed (Global-First: always use "GLOBAL")
            if "GLOBAL" not in mappings_to_save:
                mappings_to_save["GLOBAL"] = {}

            # Skip invalid category names (glyphs as names) ONLY if they have no user customizations
            if not _is_valid_category_name(category):
                if (not _has_user_customizations(category_data)) and (category not in order_categories):
                    skipped_count += 1
                    category_debug_print(f"[GLYPH SAVE] SKIPPED invalid category '{category}' (no customizations)")
                    continue
                # If referenced by order list or has user customizations, save it even with invalid name
                if category in order_categories:
                    category_debug_print(f"[GLYPH] Saving invalid category from order list: {repr(category)}")
                else:
                    category_debug_print(f"[GLYPH] Saving category with user customizations: {repr(category)}")

            if isinstance(category_data, dict):
                # Debug: print tags for all categories with tags
                cat_tags = category_data.get("tags", [])
                category_debug_print(f"[GLYPH SAVE] SAVING: '{category}' (GLOBAL) tags={cat_tags}")

                entry_to_save = {
                    "glyph": _glyph_to_unicode_escape(category_data.get("glyph", "")),
                    "display_name": category_data.get("display_name", ""),
                    "first_letter": category_data.get("first_letter", ""),
                    "color": list(category_data.get("color", [0.0, 0.0, 0.0])),
                    "default_glyph": _glyph_to_unicode_escape(category_data.get("default_glyph", "")),
                    "default_display_name": category_data.get("default_display_name", ""),
                    "base_type": category_data.get("base_type", "text_only"),
                    "tags": list(category_data.get("tags", [])),  # Save tags
                    "glyph_mode": category_data.get("glyph_mode", "auto"),
                    "icon": {
                        "source": category_data.get("icon_source", "auto"),
                        "key": category_data.get("icon_key", ""),
                        "path": category_data.get("icon_path", ""),
                        "provider": category_data.get("icon_provider", ""),
                    },
                }

                # Save extension-related fields for "New Add-ons!" feature
                if category_data.get("source_extension"):
                    entry_to_save["source_extension"] = category_data.get("source_extension")
                # Always save pending_tag_assignment if key exists (including False for "Without Tag")
                if "pending_tag_assignment" in category_data:
                    entry_to_save["pending_tag_assignment"] = category_data.get("pending_tag_assignment")
                if category_data.get("discovered_in_spaces"):
                    entry_to_save["discovered_in_spaces"] = category_data.get("discovered_in_spaces")
                if category_data.get("discovered_in_modes"):
                    entry_to_save["discovered_in_modes"] = category_data.get("discovered_in_modes")

                mappings_to_save["GLOBAL"][category] = entry_to_save
            elif isinstance(category_data, str):
                # Old format - convert to new format
                glyph = _unicode_escape_to_glyph(category_data) if '\\u' in category_data else category_data
                base_type = "glyph_only" if _is_single_glyph(glyph) else "glyph_text"
                mappings_to_save["GLOBAL"][category] = {
                    "glyph": _glyph_to_unicode_escape(glyph),
                    "display_name": "",
                    "first_letter": "",
                    "color": [0.0, 0.0, 0.0],
                    "default_glyph": _glyph_to_unicode_escape(glyph),
                    "default_display_name": "",
                    "base_type": base_type,
                    "tags": [],
                    "glyph_mode": "auto",
                    "icon": {
                        "source": "auto",
                        "key": "",
                        "path": "",
                        "provider": "",
                    },
                }

        if skipped_count > 0:
            category_debug_print(f"[GLYPH] Skipped {skipped_count} categories with invalid names and no customizations")

        # Convert tag glyphs to hex for storage
        tags_to_save = {}
        for tag_name, tag_data in state.all_tags_cache.items():
            if isinstance(tag_data, dict):
                tags_to_save[tag_name] = {
                    "glyph": _glyph_to_hex(tag_data.get("glyph", "")),
                    "color": list(tag_data.get("color", [0.0, 0.0, 0.0])),
                    "mode_flags": tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS),
                    "icon_key": tag_data.get("icon_key", ""),
                    "icon_source": tag_data.get("icon_source", 0),
                }
            else:
                tags_to_save[tag_name] = tag_data

        # Get tag order from wm.category_tags (preserves manual ordering)
        tag_order = []
        try:
            wm = bpy.context.window_manager
            if hasattr(wm, 'category_tags') and _is_collection_safe(wm.category_tags):
                tag_order = [tag.name for tag in wm.category_tags]
        except Exception:
            pass

        data = {
            'version': CURRENT_JSON_VERSION,
            'all_tags': tags_to_save,
            'tag_order': tag_order,  # Save tag order
            'mappings': mappings_to_save,
            'category_orders': {
                tag_key: _category_order_encode(category_list)
                for tag_key, category_list in state.category_orders_cache.items()
            }  # Save category orders (glyphs as \uXXXX)
        }

        prepare_end = time.perf_counter()
        category_debug_print(f"[GLYPH SAVE] Step 1 COMPLETE: Data structure built in {(prepare_end - prepare_start)*1000:.2f}ms")

    # SIMPLIFIED: Synchronous save (no background thread)
    cat_count = len(state.glyph_cache)
    tag_count = len(state.all_tags_cache)

    try:
        save_debug_print(f"[GLYPH SAVE] Step 2: Serializing JSON data...")
        serialize_start = time.perf_counter()
        json_str = json.dumps(data, indent=2, ensure_ascii=False)
        serialize_end = time.perf_counter()
        save_debug_print(f"[GLYPH SAVE] Step 2: JSON serialized in {(serialize_end - serialize_start)*1000:.2f}ms, len={len(json_str)}")

        # Create backup before overwriting
        save_debug_print(f"[GLYPH SAVE] Step 3a: Creating backup...")
        backup_start = time.perf_counter()
        create_backup(filepath)
        backup_end = time.perf_counter()
        save_debug_print(f"[GLYPH SAVE] Step 3a: Backup created in {(backup_end - backup_start)*1000:.2f}ms")

        # Write to disk synchronously
        save_debug_print(f"[GLYPH SAVE] Step 3b: Writing file...")
        write_start = time.perf_counter()
        with safe_file_write(filepath) as f:
            f.write(json_str)
        write_end = time.perf_counter()
        save_debug_print(f"[GLYPH SAVE] Step 3b: File written in {(write_end - write_start)*1000:.2f}ms")

        total_end = time.perf_counter()
        save_debug_print(f"[GLYPH SAVE] <<<<<<<< COMPLETE: Total time {(total_end - save_start_time)*1000:.2f}ms <<<<<<<<")
        save_debug_print(f"[GLYPH SAVE] Successfully saved {cat_count} categories, {tag_count} tags to {filepath}")
        tag_log(f"Saved {cat_count} categories, {tag_count} tags in {(total_end - save_start_time)*1000:.2f}ms")

        # Release lock
        set_glyph_save_lock(False)
        return True

    except Exception as e:
        save_debug_print(f"[GLYPH SAVE] <<<<<<<< EXCEPTION: {type(e).__name__}: {e}")
        tag_log(f"Save failed: {e}", "ERROR")
        category_debug_print(f"[GLYPH SAVE] Error: {e}")
        import traceback
        traceback.print_exc()
        set_glyph_save_lock(False)  # Release lock on error
        return False


# -----------------------------------------------------------------------------
# Category data getters
# -----------------------------------------------------------------------------


def get_categories_for_tag(tag_name):
    """Get a list of all categories that use a specific tag.

    Returns a list of category info dictionaries with keys:
    - 'name': original category name (may be empty)
    - 'display_name': display name for UI
    - 'space_type': space type ID
    """
    # Ensure cache is loaded
    if not is_glyph_cache_loaded():
        _load_glyph_mappings_from_file()

    categories = []
    for cat_key, cat_data in state.glyph_cache.items():
        if isinstance(cat_data, dict) and tag_name in cat_data.get("tags", []):
            # cat_key is a tuple (space_type, category), extract info
            if isinstance(cat_key, tuple) and len(cat_key) >= 2:
                space_type_id, category_name = cat_key

                # Global-First: Only process GLOBAL entries (space_type = -1)
                if space_type_id != -1:
                    continue

                display_name = cat_data.get("display_name", "")

                # Use display_name if available, otherwise use category_name
                ui_name = display_name if display_name else category_name

                if ui_name:  # Only include if we have something to display
                    categories.append({
                        'name': category_name,
                        'display_name': ui_name,
                        'space_type': space_type_id
                    })
                    category_debug_print(f"[GET_CATEGORIES_FOR_TAG] Found category '{category_name}' (display: '{ui_name}') with tag '{tag_name}', space_type={space_type_id}")
                else:
                    category_debug_print(f"[GET_CATEGORIES_FOR_TAG] Skipping category with empty name and display_name, space_type={space_type_id}")

    # Remove duplicates based on display_name
    seen = set()
    result = []
    for cat_info in categories:
        if cat_info['display_name'] not in seen:
            seen.add(cat_info['display_name'])
            result.append(cat_info)

    # Sort by display_name
    result.sort(key=lambda x: x['display_name'])
    category_debug_print(f"[GET_CATEGORIES_FOR_TAG] tag_name='{tag_name}', result count={len(result)}")
    return result


def get_category_display_name(category, space_type=-1):
    """Get the display name for a category (user-defined or default)."""
    cat_data = _get_category_data(category, space_type)

    if cat_data:
        if isinstance(cat_data, dict):
            # 1. User defined display name
            display_name = cat_data.get("display_name", "")
            if display_name:
                return display_name
            # 2. Default display name
            default_name = cat_data.get("default_display_name", "")
            if default_name:
                return default_name
    # 3. Fallback to ID
    return category


def get_category_glyph_data(category, space_type=-1):
    """Get glyph, color, and display name for a category.

    Rules:
    - For glyph-only / glyph-text categories: use stored glyph (or category if key itself is the glyph).
    - For text-only categories: show the first letter of the display name (or category) as glyph,
      matching the behavior in `interface_tab_categories_edit.cc`.
    - For color: if the primary entry has no color, fall back to any other entry for the same
      category name that has a non-zero color (e.g. space-specific mapping like SPACE_VIEW3D).
    """
    category_debug_print(f"[get_category_glyph_data] START: category='{category}', space_type={space_type}")

    cat_data = _get_category_data(category, space_type)

    category_debug_print(f"[get_category_glyph_data] cat_data={cat_data}")

    if not cat_data:
        category_debug_print(f"[get_category_glyph_data] RETURN: no cat_data")
        return "", [0.0, 0.0, 0.0], category

    # Legacy: plain string means "glyph only".
    if isinstance(cat_data, str):
        category_debug_print(f"[get_category_glyph_data] RETURN: legacy string '{cat_data}'")
        return cat_data, [0.0, 0.0, 0.0], category

    if not isinstance(cat_data, dict):
        category_debug_print(f"[get_category_glyph_data] RETURN: not dict")
        return "", [0.0, 0.0, 0.0], category

    base_type = cat_data.get("base_type", "text_only")
    glyph_mode = cat_data.get("glyph_mode", "auto")
    category_debug_print(f"[get_category_glyph_data] base_type={base_type}, glyph_mode={glyph_mode}")

    # Resolve display name first (used both for label and first-letter glyph).
    display_name = (
        cat_data.get("display_name", "")
        or cat_data.get("default_display_name", "")
        or category
    )

    # Get first_letter for potential use (needed for glyph_mode == "first_letter" and fallback)
    first_letter = cat_data.get("first_letter", "") or (display_name[0] if display_name else category[0] if category else "")

    # If glyph_mode is "first_letter", always use first letter regardless of stored glyph
    if glyph_mode == "first_letter":
        category_debug_print(f"[get_category_glyph_data] glyph_mode='first_letter', using first_letter='{first_letter}'")
        glyph = first_letter
        color = cat_data.get("color", [0.0, 0.0, 0.0])
        category_debug_print(f"[get_category_glyph_data] RETURN (first_letter mode): glyph='{glyph}'")
        return glyph, color, display_name

    # Resolve glyph according to base_type (normal mode)
    glyph = cat_data.get("glyph", "") or ""
    category_debug_print(f"[get_category_glyph_data] initial glyph='{glyph}' (len={len(glyph)})")

    # If glyph is empty in space-specific entry, try GLOBAL fallback
    category_debug_print(f"[get_category_glyph_data] Checking GLOBAL fallback: not glyph={not glyph}, space_type != -1={space_type != -1}")
    if not glyph and space_type != -1:
        category_debug_print(f"[get_category_glyph_data] Trying GLOBAL fallback...")
        global_data = _get_category_data(category, -1)  # -1 = GLOBAL
        category_debug_print(f"[get_category_glyph_data] global_data type={type(global_data)}, is dict={isinstance(global_data, dict)}")
        if global_data and isinstance(global_data, dict):
            global_glyph = global_data.get("glyph", "") or ""
            category_debug_print(f"[get_category_glyph_data] global_glyph='{global_glyph}' (len={len(global_glyph)})")
            if global_glyph:
                glyph = global_glyph
                # Also update base_type from GLOBAL if it has a glyph
                base_type = global_data.get("base_type", base_type)
                category_debug_print(f"[get_category_glyph_data] Using GLOBAL glyph, new base_type={base_type}")
        else:
            category_debug_print(f"[get_category_glyph_data] GLOBAL data not found or not dict!")

    if not glyph:
        # For text_only categories, use first letter as glyph (matching C++ behavior)
        category_debug_print(f"[get_category_glyph_data] glyph is still empty, base_type={base_type}")
        if base_type == "text_only":
            # Use pre-computed first_letter
            glyph = first_letter
            category_debug_print(f"[get_category_glyph_data] Using first letter fallback: '{glyph}'")
        else:
            # For glyph_only/glyph_text categories: check if category has tags and use the first tag's glyph
            category_tags = cat_data.get("tags", [])
            if category_tags:
                # Get the first tag's glyph
                first_tag = category_tags[0]
                from bl_ui.glyph_tag_system import tags_cache as _tags_cache
                tag_data = _tags_cache.get_tag_data(first_tag)
                category_debug_print(f"[get_category_glyph_data] Trying tag glyph: tag='{first_tag}', tag_data={tag_data}")
                if tag_data and tag_data.get("glyph"):
                    glyph = tag_data["glyph"]

            if not glyph:
                # For glyph-only / glyph-text categories where key itself is the glyph.
                glyph = category

    # Primary color from this entry.
    color = cat_data.get("color", [0.0, 0.0, 0.0])

    # If color is still all zeros, try to find a non-zero color from any other mapping
    # for the same category (e.g. a SPACE_VIEW3D-specific entry).
    if not any(c > 0.0 for c in color):
        for cache_key, cache_data in state.glyph_cache.items():
            if (
                isinstance(cache_key, tuple)
                and len(cache_key) >= 2
                and cache_key[1] == category
                and isinstance(cache_data, dict)
            ):
                alt_color = cache_data.get("color", [0.0, 0.0, 0.0])
                if any(c > 0.0 for c in alt_color):
                    color = alt_color
                    break

    category_debug_print(f"[get_category_glyph_data] RETURN: glyph='{glyph}' (len={len(glyph)}), base_type={base_type}")
    return glyph, color, display_name


def get_category_icon_data(category, space_type=-1):
    """Get icon key and path for a category.

    Returns: (icon_key, icon_path) tuple.
    - icon_key: Blender internal icon identifier (e.g. 'PLAY', 'SELECT_EXTEND') or ""
    - icon_path: Path to external icon file or ""

    Icon takes priority over glyph when icon_key is set or icon_path is set.
    The C++ side resolves icon_id from icon_key using RNA_enum_value_from_identifier.
    """
    category_debug_print(f"[get_category_icon_data] START: category='{category}', space_type={space_type}")
    cat_data = _get_category_data(category, space_type)

    if not cat_data:
        category_debug_print(f"[get_category_icon_data] RETURN: no cat_data")
        return "", ""

    if isinstance(cat_data, str):
        category_debug_print(f"[get_category_icon_data] RETURN: cat_data is string")
        return "", ""

    if not isinstance(cat_data, dict):
        category_debug_print(f"[get_category_icon_data] RETURN: cat_data is not dict")
        return "", ""

    icon_source = cat_data.get("icon_source", "auto")
    icon_key = cat_data.get("icon_key", "")
    icon_path = cat_data.get("icon_path", "")
    category_debug_print(f"[get_category_icon_data] icon_source='{icon_source}', icon_key='{icon_key}', icon_path='{icon_path}'")

    if icon_source == "off":
        category_debug_print(f"[get_category_icon_data] RETURN: icon_source is 'off'")
        return "", ""

    # Prefer any explicit icon payload so the UI stays in sync even if icon_source
    # was not refreshed yet in the in-memory cache.
    if icon_key:
        category_debug_print(f"[get_category_icon_data] RETURN: manual mode, icon_key='{icon_key}'")
        return icon_key, ""

    if icon_path:
        category_debug_print(f"[get_category_icon_data] RETURN: manual mode, icon_path='{icon_path}'")
        return "", icon_path

    return "", ""


# -----------------------------------------------------------------------------
# Visibility predicates
# -----------------------------------------------------------------------------


def _is_reserved_category_name(category_name):
    """Single source-of-truth check for reserved categories.

    Reserved = categories defined in DEFAULT_CATEGORY_GLYPHS.
    """
    return category_name in DEFAULT_CATEGORY_GLYPHS


# TODO: Add preference U.category_filter_hide_reserved (bool, default false)
# When true, reserved tabs also respect tag filtering


def is_category_visible_by_tags(category_name, active_filter_tags):
    """
    Check if a category should be visible based on tag filtering.

    Rules:
    1. Reserved categories are ALWAYS visible (by default)
    2. If active_filter_tags is empty or None, show all categories
    3. Category is visible if it has at least one of the active tags (OR logic)

    Args:
        category_name: Name of the category to check
        active_filter_tags: List/set of active tag names, or None/empty for all

    Returns:
        bool: True if category should be visible
    """
    # Reserved categories are always visible
    # TODO: Check U.category_filter_hide_reserved preference
    if _is_reserved_category_name(category_name):
        return True

    # No filtering - show all
    if not active_filter_tags:
        return True

    # Convert to set for fast lookup
    active_set = set(active_filter_tags)

    # "All" filter shows everything
    if "All" in active_set:
        return True

    # Check tag intersection (OR logic)
    from bl_ui.glyph_tag_system import tags_cache as _tags_cache
    category_tags = set(_tags_cache.get_category_tags(category_name))

    # Categories without tags are only visible with "All" filter
    if not category_tags:
        return False

    return bool(category_tags & active_set)


def filter_categories_by_tags(category_list, active_filter_tags):
    """
    Filter a list of categories by active tags.

    Args:
        category_list: List of category names
        active_filter_tags: List/set of active tag names

    Returns:
        List of visible category names
    """
    return [
        cat for cat in category_list
        if is_category_visible_by_tags(cat, active_filter_tags)
    ]


# -----------------------------------------------------------------------------
# Glyph getters / setters
# -----------------------------------------------------------------------------


def get_category_glyph(category, space_type=-1):
    """Get the glyph for a category name."""
    entry = _get_category_data(category, space_type)
    if entry and isinstance(entry, dict):
        return entry.get("glyph", None)
    return None


def get_category_data(category, space_type=-1):
    """Get the full data (glyph, display_name, color, defaults) for a category name."""
    return _get_category_data(category, space_type)


def get_default_glyph(category, space_type=-1):
    """Get the default glyph for a category name."""
    entry = _get_category_data(category, space_type)
    if entry and isinstance(entry, dict):
        result = entry.get("default_glyph", entry.get("glyph", ""))
        category_debug_print(f"[GLYPH] get_default_glyph('{category}', space_type={space_type}) = '{result}'")
        return result
    category_debug_print(f"[GLYPH] get_default_glyph('{category}', space_type={space_type}) = '' (not found)")
    return ""


def get_default_display_name(category, space_type=-1):
    """Get the default display name for a category name."""
    entry = _get_category_data(category, space_type)
    if entry and isinstance(entry, dict):
        result = entry.get("default_display_name", "")
        category_debug_print(f"[GLYPH] get_default_display_name('{category}', space_type={space_type}) = '{result}'")
        return result
    category_debug_print(f"[GLYPH] get_default_display_name('{category}', space_type={space_type}) = '' (not found)")
    return ""


def set_category_glyph(category, glyph, space_type=-1, save=True):
    """Set the glyph for a category name."""
    key = _make_cache_key(space_type, category)
    if key not in state.glyph_cache:
        state.glyph_cache[key] = {
            "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
            "default_glyph": "", "default_display_name": "", "base_type": "text_only",
            "first_letter": "",
            "glyph_mode": "auto",
            "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
        }

    # Ensure the entry has all required fields
    entry = state.glyph_cache[key]
    if "default_glyph" not in entry:
        entry["default_glyph"] = entry.get("glyph", "")
    if "default_display_name" not in entry:
        # For glyph_text categories, use category name as default_display_name for tooltip fallback
        if entry.get("base_type") == "glyph_text" or (glyph and not _is_single_glyph(category)):
            entry["default_display_name"] = category
        else:
            # NEVER use entry["display_name"] here — it may be a user-renamed value.
            # Use panel label or category name as the original name for Reset.
            panel_label = _find_panel_label_for_category(category)
            entry["default_display_name"] = panel_label if panel_label else category
    if "glyph_mode" not in entry:
        entry["glyph_mode"] = "auto"

    if glyph:
        state.glyph_cache[key]["glyph"] = glyph
        # For glyph_only categories, default_glyph must be the category name (original glyph)
        is_glyph_only = _is_single_glyph(category)
        if is_glyph_only:
            if not state.glyph_cache[key].get("default_glyph"):
                state.glyph_cache[key]["default_glyph"] = category
            state.glyph_cache[key]["base_type"] = "glyph_only"
        elif glyph:
            # For non-glyph_only with a glyph set, update base_type
            state.glyph_cache[key]["base_type"] = "glyph_only" if _is_single_glyph(glyph) else "glyph_text"
    else:
        # Remove the category if glyph is empty
        if key in state.glyph_cache:
            del state.glyph_cache[key]

    if save:
        from bl_ui.glyph_tag_system import handlers as _handlers
        _handlers._auto_save_glyph_mappings()


def set_category_data(category,
                      glyph=None,
                      display_name=None,
                      first_letter=None,
                      color=None,
                      tags=None,
                      glyph_mode=None,
                      icon_source=None,
                      icon_key=None,
                      icon_path=None,
                      icon_provider=None,
                      space_type=-1,
                      save=True,
                      skip_wm_sync=False):
    """Set the full data (glyph, display_name, color, tags, icon fields) for a category name."""
    import time
    set_start = time.perf_counter()
    category_debug_print(f"[SET_CATEGORY_DATA] >>>>>> START set_category_data for '{category}' <<<<<<")

    key = _make_cache_key(space_type, category)

    # DEBUG: Log incoming call
    category_debug_print(f"[SET_CATEGORY_DATA] CALLED: category='{category}', space_type={space_type}, key={key}")
    category_debug_print(f"[SET_CATEGORY_DATA] PARAMS: tags={repr(tags)}, glyph={repr(glyph)}, display_name={repr(display_name)}, icon_source={repr(icon_source)}, glyph_mode={repr(glyph_mode)}")

    # DEBUG: Show current tags BEFORE changes
    if key in state.glyph_cache:
        current_tags = state.glyph_cache[key].get("tags", [])
        category_debug_print(f"[SET_CATEGORY_DATA] CURRENT_TAGS in cache: {current_tags}")
    else:
        category_debug_print(f"[SET_CATEGORY_DATA] KEY NOT in cache - will create new entry")

    if key not in state.glyph_cache:
        inherited_data = None

        if space_type != -1:
            global_key = _make_cache_key(-1, category)
            global_data = state.glyph_cache.get(global_key)
            if isinstance(global_data, dict):
                inherited_data = dict(global_data)

        if inherited_data is None:
            for cache_key, cache_data in state.glyph_cache.items():
                if not (isinstance(cache_key, tuple) and len(cache_key) >= 2):
                    continue
                if cache_key[1] != category:
                    continue
                if isinstance(cache_data, dict):
                    inherited_data = dict(cache_data)
                    break

        if isinstance(inherited_data, dict):
            state.glyph_cache[key] = _normalize_category_data(inherited_data, category)
            category_debug_print(f"[SET_CATEGORY_DATA] Initialized key={key} from existing category data")
        else:
            state.glyph_cache[key] = {
                "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                "default_glyph": "", "default_display_name": "", "base_type": "text_only",
                "first_letter": "",
                "glyph_mode": "auto",
                "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
            }

    # Ensure the entry has all required fields
    entry = state.glyph_cache[key]
    # Note: default_glyph should NOT be set from glyph for text_only/glyph_text categories.
    # For text_only categories, reset should return first letter (nullptr in C++),
    # not a previously assigned glyph. default_glyph is only meaningful for glyph_only categories.
    if "default_glyph" not in entry:
        # Only set default_glyph for glyph_only categories
        if _is_single_glyph(category):
            entry["default_glyph"] = category
        else:
            entry["default_glyph"] = ""
    if "default_display_name" not in entry:
        # For glyph_text categories, use category name as default_display_name for tooltip fallback
        if entry.get("base_type") == "glyph_text" or (glyph and not _is_single_glyph(category)):
            entry["default_display_name"] = category
        else:
            # For text_only/glyph_only categories, try to find Panel bl_label first,
            # then fall back to category name. Never use entry["display_name"] here
            # because it may already be a user-renamed value (e.g., "ZZZ" instead of "Hyperfy").
            panel_label = _find_panel_label_for_category(category)
            entry["default_display_name"] = panel_label if panel_label else category
    if "glyph_mode" not in entry:
        entry["glyph_mode"] = "auto"
    if "first_letter" not in entry:
        entry["first_letter"] = ""

    if glyph is not None:
        # Convert hex string to Unicode glyph if needed
        if glyph and len(glyph) <= 6 and all(c in '0123456789abcdefABCDEF' for c in glyph):
            # Looks like a hex string, convert to glyph
            glyph = _hex_to_glyph(glyph)
        state.glyph_cache[key]["glyph"] = glyph
        # For glyph_only categories, default_glyph must be the category name (original glyph)
        is_glyph_only = _is_single_glyph(category)
        if is_glyph_only:
            # For glyph_only: default_glyph = category name (the original glyph)
            state.glyph_cache[key]["default_glyph"] = category
            state.glyph_cache[key]["base_type"] = "glyph_only"
        elif glyph:
            # For text_only categories with a glyph assigned: base_type = glyph_text
            # But default_glyph stays EMPTY - reset should return to first letter
            state.glyph_cache[key]["base_type"] = "glyph_only" if _is_single_glyph(glyph) else "glyph_text"
            # Do NOT set default_glyph for text_only/glyph_text categories
            # Reset should return to first letter (nullptr in C++)
    if display_name is not None:
        state.glyph_cache[key]["display_name"] = display_name
        # NOTE: Do NOT update default_display_name here. It must be set only once
        # during initial category discovery to preserve the original name for Reset.
        # Updating it here causes renamed categories (e.g., "Hyperfy" → "ZZZ") to
        # lose their original name, breaking the Reset functionality.
    # Keep first_letter in sync with display_name when explicit value is not provided.
    if first_letter is not None:
        state.glyph_cache[key]["first_letter"] = first_letter
    elif display_name is not None:
        src = display_name if isinstance(display_name, str) else ""
        state.glyph_cache[key]["first_letter"] = src[:1] if src else ""
    if color is not None:
        # Ensure color is stored as floats, converting strings if needed
        # Handle hex string format (e.g., "029c05" from C++)
        if isinstance(color, str) and len(color) >= 6:
            # Parse as hex color (RRGGBB)
            try:
                r = int(color[0:2], 16) / 255.0
                g = int(color[2:4], 16) / 255.0
                b = int(color[4:6], 16) / 255.0
                state.glyph_cache[key]["color"] = [r, g, b]
                category_debug_print(f"[SET_CATEGORY_DATA] Parsed hex color '{color}' to RGB [{r:.4f}, {g:.4f}, {b:.4f}]")
            except ValueError:
                state.glyph_cache[key]["color"] = [0.0, 0.0, 0.0]
                category_debug_print(f"[SET_CATEGORY_DATA] Failed to parse hex color '{color}', using black")
        elif len(color) >= 3:
            color_list = []
            for c in color[:3]:
                if isinstance(c, str):
                    try:
                        color_list.append(float(c))
                    except ValueError:
                        color_list.append(0.0)
                else:
                    color_list.append(float(c) if c is not None else 0.0)
            state.glyph_cache[key]["color"] = color_list
        else:
            state.glyph_cache[key]["color"] = [0.0, 0.0, 0.0]
    if tags is not None:
        # DEBUG: Log tags update
        category_debug_print(f"[SET_CATEGORY_DATA] UPDATING TAGS: old={state.glyph_cache[key].get('tags', [])} -> new_param={repr(tags)}")
        # Parse tags string (comma-separated or single tag)
        if isinstance(tags, str):
            if tags:
                state.glyph_cache[key]["tags"] = [t.strip() for t in tags.split(',') if t.strip()]
            else:
                state.glyph_cache[key]["tags"] = []
        elif isinstance(tags, list):
            state.glyph_cache[key]["tags"] = tags
        category_debug_print(f"[SET_CATEGORY_DATA] TAGS AFTER UPDATE: {state.glyph_cache[key].get('tags', [])}")
    else:
        # DEBUG: tags is None - not updating
        category_debug_print(f"[SET_CATEGORY_DATA] TAGS is None - NOT updating (keeping existing tags)")

    if glyph_mode is not None:
        glyph_mode_norm = str(glyph_mode).lower()
        if glyph_mode_norm not in {"auto", "first_letter"}:
            glyph_mode_norm = "auto"
        state.glyph_cache[key]["glyph_mode"] = glyph_mode_norm

    if icon_source is not None:
        icon_source_norm = str(icon_source).lower()
        if icon_source_norm not in {"auto", "manual", "off"}:
            icon_source_norm = "auto"
        state.glyph_cache[key]["icon_source"] = icon_source_norm
    if icon_key is not None:
        state.glyph_cache[key]["icon_key"] = str(icon_key)
    if icon_path is not None:
        state.glyph_cache[key]["icon_path"] = str(icon_path)
    if icon_provider is not None:
        state.glyph_cache[key]["icon_provider"] = str(icon_provider)

    global_key = _make_cache_key(-1, category)
    global_synced_from_space = False

    # STEP 1: Sync from space-specific to GLOBAL (propagate user changes)
    # Priority: 'manual' > 'off' > 'auto'
    if space_type != -1 and (
        glyph_mode is not None or
        icon_source is not None or
        color is not None or
        display_name is not None or
        first_letter is not None
    ):
        if global_key not in state.glyph_cache:
            state.glyph_cache[global_key] = {
                "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                "default_glyph": "", "default_display_name": "", "base_type": "text_only",
                "first_letter": "",
                "glyph_mode": "auto",
                "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
            }
        global_entry = state.glyph_cache[global_key]

        # Determine if we should update global entry
        # Priority: 'manual' > 'off' > 'auto'
        # - 'manual' always updates (explicit icon choice)
        # - 'off' always updates (explicit glyph/first_letter choice)
        # - 'auto' gets overwritten by anything
        global_icon_source = global_entry.get("icon_source", "auto")
        space_icon_source = state.glyph_cache[key].get("icon_source", "auto")
        should_update = (
            global_icon_source == "auto" or
            space_icon_source == "manual" or
            space_icon_source == "off"
        )

        if should_update:
            if glyph_mode is not None:
                global_entry["glyph_mode"] = glyph_mode_norm
                category_debug_print(f"[SET_CATEGORY_DATA] Propagated glyph_mode '{glyph_mode_norm}' to GLOBAL for '{category}'")
            if icon_source is not None:
                global_entry["icon_source"] = icon_source_norm
                category_debug_print(f"[SET_CATEGORY_DATA] Propagated icon_source '{icon_source_norm}' to GLOBAL for '{category}'")
            if icon_key is not None:
                global_entry["icon_key"] = str(icon_key)
            if icon_path is not None:
                global_entry["icon_path"] = str(icon_path)
            if icon_provider is not None:
                global_entry["icon_provider"] = str(icon_provider)
            # Sync color from SPACE to GLOBAL
            if color is not None:
                global_entry["color"] = state.glyph_cache[key]["color"]
                category_debug_print(f"[SET_CATEGORY_DATA] Propagated color to GLOBAL for '{category}'")
            if display_name is not None:
                global_entry["display_name"] = state.glyph_cache[key].get("display_name", "")
                if not global_entry.get("default_display_name"):
                    # NEVER use display_name as fallback — it may be a user-renamed value.
                    # Use panel label or category name as the original name for Reset.
                    panel_label = _find_panel_label_for_category(category)
                    global_entry["default_display_name"] = panel_label if panel_label else category
                category_debug_print(f"[SET_CATEGORY_DATA] Propagated display_name to GLOBAL for '{category}'")
            if first_letter is not None or display_name is not None:
                global_entry["first_letter"] = state.glyph_cache[key].get("first_letter", "")
            global_synced_from_space = True

    # STEP 2: Sync from GLOBAL to all space-specific entries (consistency)
    if space_type == -1 and (
        glyph_mode is not None or
        icon_source is not None or
        color is not None or
        display_name is not None or
        first_letter is not None
    ):
        for cache_key in list(state.glyph_cache.keys()):
            if isinstance(cache_key, tuple) and len(cache_key) >= 2:
                cache_space_type, cache_category = cache_key
                if cache_category == category and cache_space_type != -1:
                    space_entry = state.glyph_cache[cache_key]
                    if glyph_mode is not None:
                        space_entry["glyph_mode"] = glyph_mode_norm
                        category_debug_print(f"[SET_CATEGORY_DATA] Synced glyph_mode to space_type={cache_space_type} for '{category}'")
                    if icon_source is not None:
                        space_entry["icon_source"] = icon_source_norm
                        category_debug_print(f"[SET_CATEGORY_DATA] Synced icon_source to space_type={cache_space_type} for '{category}'")
                    if icon_key is not None:
                        space_entry["icon_key"] = str(icon_key)
                    if icon_path is not None:
                        space_entry["icon_path"] = str(icon_path)
                    if icon_provider is not None:
                        space_entry["icon_provider"] = str(icon_provider)
                    # Sync color from GLOBAL to SPACE
                    if color is not None:
                        space_entry["color"] = state.glyph_cache[key]["color"]
                        category_debug_print(f"[SET_CATEGORY_DATA] Synced color to space_type={cache_space_type} for '{category}'")
                    if display_name is not None:
                        space_entry["display_name"] = state.glyph_cache[key].get("display_name", "")
                        if not space_entry.get("default_display_name"):
                            # NEVER use display_name — it may be a user-renamed value.
                            panel_label = _find_panel_label_for_category(category)
                            space_entry["default_display_name"] = panel_label if panel_label else category
                        category_debug_print(f"[SET_CATEGORY_DATA] Synced display_name to space_type={cache_space_type} for '{category}'")
                    if first_letter is not None or display_name is not None:
                        space_entry["first_letter"] = state.glyph_cache[key].get("first_letter", "")

    # Sync glyph to space-specific entries when saving to GLOBAL
    # This ensures that categories with tags in space-specific entries get the glyph from GLOBAL
    if space_type == -1 and glyph is not None and glyph:
        for cache_key in list(state.glyph_cache.keys()):
            if isinstance(cache_key, tuple) and len(cache_key) >= 2:
                cache_space_type, cache_category = cache_key
                if cache_category == category and cache_space_type != -1:
                    # Found a space-specific entry for this category
                    space_entry = state.glyph_cache[cache_key]
                    # Only update if the space-specific entry has empty glyph
                    if not space_entry.get("glyph"):
                        space_entry["glyph"] = glyph
                        category_debug_print(f"[SET_CATEGORY_DATA] Synced glyph to space_type={cache_space_type} for '{category}'")

    # STEP 3: Sync to WM for C++ UI display
    # Global-First: Always use GLOBAL space_type (-1) for WM override to prevent
    # C++ from finding stale space-specific overrides first
    try:
        wm = bpy.context.window_manager
        if wm is not None and hasattr(wm, 'category_glyph_overrides'):
            # Global-First: Remove any stale space-specific overrides for this category
            items_to_remove = []
            for item in wm.category_glyph_overrides:
                if item.category == category:
                    item_st = getattr(item, 'space_type', -1)
                    if item_st != -1:  # Not GLOBAL
                        items_to_remove.append(item)
            for item in items_to_remove:
                wm.category_glyph_overrides.remove(item)
                category_debug_print(f"[SET_CATEGORY_DATA] Removed stale space-specific WM override for '{category}'")

            # Find or create GLOBAL override entry in WM
            override_item = None
            for item in wm.category_glyph_overrides:
                item_space_type = getattr(item, 'space_type', -1)
                if item.category == category and item_space_type == -1:  # GLOBAL only
                    override_item = item
                    break

            if override_item is None:
                # Create new GLOBAL override entry
                override_item = wm.category_glyph_overrides.new()
                override_item.category = category
                if hasattr(override_item, 'space_type'):
                    override_item.space_type = -1  # Always GLOBAL

            # Update override with data from cache
            entry = state.glyph_cache[key]

            # Convert lowercase values to uppercase for WM enum
            icon_source_val = entry.get("icon_source", "auto").upper()
            glyph_mode_val = entry.get("glyph_mode", "auto").upper()
            # WM expects FIRST_LETTER, not FIRST_LETTER
            if glyph_mode_val == "FIRST_LETTER":
                glyph_mode_val = "FIRST_LETTER"

            if hasattr(override_item, 'icon_source'):
                override_item.icon_source = icon_source_val
            if hasattr(override_item, 'glyph_mode'):
                override_item.glyph_mode = glyph_mode_val
            if hasattr(override_item, 'icon_key'):
                override_item.icon_key = entry.get("icon_key", "")
            if hasattr(override_item, 'icon_path'):
                override_item.icon_path = entry.get("icon_path", "")
            if hasattr(override_item, 'icon_provider'):
                override_item.icon_provider = entry.get("icon_provider", "")
            if hasattr(override_item, 'glyph'):
                override_item.glyph = entry.get("glyph", "")
            if hasattr(override_item, 'display_name'):
                override_item.display_name = entry.get("display_name", "")
            # Sync color to WM override
            if hasattr(override_item, 'color'):
                color_val = entry.get("color", [0.0, 0.0, 0.0])
                override_item.color = color_val
            category_debug_print(f"[SET_CATEGORY_DATA] Synced to WM override for '{category}' (space_type=-1 GLOBAL)")

            # Also update mappings (for C++ panel_category_icon_data_lookup)
            if hasattr(wm, 'category_glyph_mappings'):
                mapping_item = None
                for item in wm.category_glyph_mappings:
                    item_space_type = getattr(item, 'space_type', -1)
                    if item.category == category and item_space_type == -1:
                        mapping_item = item
                        break

                if mapping_item is None:
                    mapping_item = wm.category_glyph_mappings.new()
                    mapping_item.category = category
                    if hasattr(mapping_item, 'space_type'):
                        mapping_item.space_type = -1

                if hasattr(mapping_item, 'icon_source'):
                    mapping_item.icon_source = icon_source_val
                if hasattr(mapping_item, 'glyph_mode'):
                    mapping_item.glyph_mode = glyph_mode_val
                if hasattr(mapping_item, 'icon_key'):
                    mapping_item.icon_key = entry.get("icon_key", "")
                if hasattr(mapping_item, 'icon_path'):
                    mapping_item.icon_path = entry.get("icon_path", "")
                if hasattr(mapping_item, 'icon_provider'):
                    mapping_item.icon_provider = entry.get("icon_provider", "")
                if hasattr(mapping_item, 'glyph'):
                    mapping_item.glyph = entry.get("glyph", "")
                if hasattr(mapping_item, 'display_name'):
                    mapping_item.display_name = entry.get("display_name", "")
                # Sync color to WM mapping
                if hasattr(mapping_item, 'color'):
                    color_val = entry.get("color", [0.0, 0.0, 0.0])
                    mapping_item.color = color_val
                category_debug_print(f"[SET_CATEGORY_DATA] Synced to WM mapping for '{category}' (space_type=-1)")

            if global_synced_from_space and global_key in state.glyph_cache:
                global_entry = state.glyph_cache[global_key]

                global_override_item = None
                for item in wm.category_glyph_overrides:
                    item_space_type = getattr(item, 'space_type', -1)
                    if item.category == category and item_space_type == -1:
                        global_override_item = item
                        break

                if global_override_item is None:
                    global_override_item = wm.category_glyph_overrides.new()
                    global_override_item.category = category
                    if hasattr(global_override_item, 'space_type'):
                        global_override_item.space_type = -1

                global_icon_source_val = global_entry.get("icon_source", "auto").upper()
                global_glyph_mode_val = global_entry.get("glyph_mode", "auto").upper()

                if hasattr(global_override_item, 'icon_source'):
                    global_override_item.icon_source = global_icon_source_val
                if hasattr(global_override_item, 'glyph_mode'):
                    global_override_item.glyph_mode = global_glyph_mode_val
                if hasattr(global_override_item, 'icon_key'):
                    global_override_item.icon_key = global_entry.get("icon_key", "")
                if hasattr(global_override_item, 'icon_path'):
                    global_override_item.icon_path = global_entry.get("icon_path", "")
                if hasattr(global_override_item, 'icon_provider'):
                    global_override_item.icon_provider = global_entry.get("icon_provider", "")
                if hasattr(global_override_item, 'glyph'):
                    global_override_item.glyph = global_entry.get("glyph", "")
                if hasattr(global_override_item, 'display_name'):
                    global_override_item.display_name = global_entry.get("display_name", "")
                if hasattr(global_override_item, 'color'):
                    global_override_item.color = global_entry.get("color", [0.0, 0.0, 0.0])

                if hasattr(wm, 'category_glyph_mappings'):
                    global_mapping_item = None
                    for item in wm.category_glyph_mappings:
                        item_space_type = getattr(item, 'space_type', -1)
                        if item.category == category and item_space_type == -1:
                            global_mapping_item = item
                            break

                    if global_mapping_item is None:
                        global_mapping_item = wm.category_glyph_mappings.new()
                        global_mapping_item.category = category
                        if hasattr(global_mapping_item, 'space_type'):
                            global_mapping_item.space_type = -1

                    if hasattr(global_mapping_item, 'icon_source'):
                        global_mapping_item.icon_source = global_icon_source_val
                    if hasattr(global_mapping_item, 'glyph_mode'):
                        global_mapping_item.glyph_mode = global_glyph_mode_val
                    if hasattr(global_mapping_item, 'icon_key'):
                        global_mapping_item.icon_key = global_entry.get("icon_key", "")
                    if hasattr(global_mapping_item, 'icon_path'):
                        global_mapping_item.icon_path = global_entry.get("icon_path", "")
                    if hasattr(global_mapping_item, 'icon_provider'):
                        global_mapping_item.icon_provider = global_entry.get("icon_provider", "")
                    if hasattr(global_mapping_item, 'glyph'):
                        global_mapping_item.glyph = global_entry.get("glyph", "")
                    if hasattr(global_mapping_item, 'display_name'):
                        global_mapping_item.display_name = global_entry.get("display_name", "")
                    if hasattr(global_mapping_item, 'color'):
                        global_mapping_item.color = global_entry.get("color", [0.0, 0.0, 0.0])

                category_debug_print(f"[SET_CATEGORY_DATA] Synced propagated GLOBAL entry to WM for '{category}'")
    except Exception as e:
        category_debug_print(f"[SET_CATEGORY_DATA] Failed to sync to WM: {e}")

    if save:
        save_start = time.perf_counter()
        from bl_ui.glyph_tag_system import handlers as _handlers
        _handlers._auto_save_glyph_mappings(skip_wm_sync=skip_wm_sync)
        save_end = time.perf_counter()
        category_debug_print(f"[SET_CATEGORY_DATA] Step 4: _auto_save_glyph_mappings() scheduled in {(save_end - save_start)*1000:.2f}ms")

    set_end = time.perf_counter()
    category_debug_print(f"[SET_CATEGORY_DATA] <<<<<<<< COMPLETE: Total time {(set_end - set_start)*1000:.2f}ms <<<<<<<<")


def reset_category_to_defaults(category, space_type=-1, save=True):
    """Reset a single category to its default values (glyph and display_name).

    Color is always reset to [0.0, 0.0, 0.0].
    Returns the default glyph and display_name.

    For text_only categories, this also clears GLOBAL mappings to prevent
    stale glyph data from appearing in "Categories using this tag" panel.
    """
    category_debug_print(f"[GLYPH RESET] reset_category_to_defaults called for: '{category}' (space_type={space_type})")

    if not is_glyph_cache_loaded():
        _load_glyph_mappings_from_file()

    key = _make_cache_key(space_type, category)
    if key not in state.glyph_cache:
        category_debug_print(f"[GLYPH RESET] Category '{category}' (space_type={space_type}) not found in cache!")
        return "", ""

    entry = state.glyph_cache[key]
    category_debug_print(f"[GLYPH RESET] Current entry: {entry}")

    default_glyph = entry.get("default_glyph", entry.get("glyph", ""))
    default_display_name = entry.get("default_display_name", "")

    category_debug_print(f"[GLYPH RESET] default_glyph='{default_glyph}', default_display_name='{default_display_name}'")

    # Reset to defaults
    entry["glyph"] = default_glyph
    entry["display_name"] = default_display_name
    entry["color"] = [0.0, 0.0, 0.0]

    category_debug_print(f"[GLYPH RESET] After reset entry: {entry}")

    # CRITICAL FIX: For text_only/glyph_text categories, also clear GLOBAL mappings
    # to prevent stale glyph data from appearing in "Categories using this tag" panel.
    # The issue: get_category_glyph_data uses GLOBAL fallback (lines 3512-3527) which
    # finds old glyph in GLOBAL mappings even after resetting space-specific entry.
    #
    # IMPORTANT: Check base_type from GLOBAL entry, not space-specific entry!
    # Space-specific entry may have base_type="glyph_only" (set when user assigns glyph),
    # but GLOBAL entry has the original base_type="glyph_text" or "text_only".
    global_key = _make_cache_key(-1, category)
    global_entry = state.glyph_cache.get(global_key, {})
    # Use GLOBAL base_type if available, otherwise fallback to space-specific entry
    base_type = global_entry.get("base_type", entry.get("base_type", "text_only"))
    category_debug_print(f"[GLYPH RESET] base_type='{base_type}' (from {'GLOBAL' if global_entry else 'space-specific'} entry)")

    # Check if this is NOT a glyph_only category (glyph_only categories should keep their glyphs)
    is_glyph_only = base_type == "glyph_only"
    category_debug_print(f"[GLYPH RESET] is_glyph_only={is_glyph_only}")

    if not is_glyph_only:
        # Reserved categories always have default glyphs in DEFAULT_CATEGORY_GLYPHS.
        # On reset, restore their glyph and use AUTO mode (not First Letter).
        is_reserved = category in DEFAULT_CATEGORY_GLYPHS
        reset_glyph_mode = "auto" if is_reserved else "first_letter"
        reset_glyph_val = DEFAULT_CATEGORY_GLYPHS[category].get("default_glyph", "") if is_reserved else ""

        # Clear GLOBAL mappings (space_type=-1) for this category
        global_key = _make_cache_key(-1, category)
        if global_key in state.glyph_cache:
            global_entry = state.glyph_cache[global_key]
            category_debug_print(f"[GLYPH RESET] Clearing GLOBAL entry (not glyph_only): {global_entry}")
            # For reserved categories, restore default glyph; for others, clear to empty.
            global_entry["glyph"] = reset_glyph_val
            global_entry["color"] = [0.0, 0.0, 0.0]
            # Reserved categories use AUTO (glyph), others use first_letter fallback.
            global_entry["glyph_mode"] = reset_glyph_mode
            # Clear icon data, then re-detect for extension categories
            global_entry["icon_source"] = "off"
            global_entry["icon_key"] = ""
            global_entry["icon_path"] = ""
            global_entry["icon_provider"] = ""
            # If category belongs to an extension, re-detect icon.png
            if global_entry.get("source_extension"):
                from bl_ui.glyph_tag_system import discovery_scan as _discovery_scan
                detected_path, detected_provider = _discovery_scan._auto_detect_extension_icon_path(category)
                if detected_path:
                    global_entry["icon_source"] = "auto"
                    global_entry["icon_path"] = detected_path
                    global_entry["icon_provider"] = detected_provider or "extension_auto"
                    category_debug_print(f"[GLYPH RESET] Re-detected extension icon for '{category}': {detected_path}")
            category_debug_print(f"[GLYPH RESET] After clearing GLOBAL entry: {global_entry}")

        # Clear only current space-specific mapping (tags are handled separately by reset_tag).
        if space_type != -1 and key in state.glyph_cache:
            cache_entry = state.glyph_cache[key]
            category_debug_print(f"[GLYPH RESET] Clearing current space-specific entry: space_type={space_type}")
            cache_entry["glyph"] = reset_glyph_val
            cache_entry["color"] = [0.0, 0.0, 0.0]
            cache_entry["glyph_mode"] = reset_glyph_mode
            cache_entry["icon_source"] = "off"
            cache_entry["icon_key"] = ""
            cache_entry["icon_path"] = ""
            cache_entry["icon_provider"] = ""
            # Re-detect extension icon for space-specific entry too
            if cache_entry.get("source_extension"):
                from bl_ui.glyph_tag_system import discovery_scan as _discovery_scan
                detected_path, detected_provider = _discovery_scan._auto_detect_extension_icon_path(category)
                if detected_path:
                    cache_entry["icon_source"] = "auto"
                    cache_entry["icon_path"] = detected_path
                    cache_entry["icon_provider"] = detected_provider or "extension_auto"

    category_debug_print(f"[GLYPH RESET] Final cache state for '{category}':")
    for cache_key, cache_entry in state.glyph_cache.items():
        if isinstance(cache_key, tuple) and len(cache_key) >= 2 and cache_key[1] == category:
            category_debug_print(f"  {cache_key}: glyph='{cache_entry.get('glyph', '')}', glyph_mode='{cache_entry.get('glyph_mode', 'auto')}'")

    if save:
        from bl_ui.glyph_tag_system import handlers as _handlers
        _handlers._auto_save_glyph_mappings()

    return default_glyph, default_display_name


def get_all_category_glyphs():
    """Get all category glyph mappings."""
    if not is_glyph_cache_loaded():
        _load_glyph_mappings_from_file()

    return state.glyph_cache.copy()


def reset_category_glyphs_to_defaults():
    """Reset all glyph mappings to defaults."""
    reset_glyph_cache(DEFAULT_CATEGORY_GLYPHS.copy())
    _save_glyph_mappings_to_file()


def _integrate_glyph_library():
    """Integrate glyph library with DEFAULT_CATEGORY_GLYPHS."""
    if not get_glyph_library:
        return

    try:
        library = get_glyph_library()
        if not library or not library.is_loaded:
            return

        # For each category in DEFAULT_CATEGORY_GLYPHS, try to get the glyph name from library
        for category, glyph_data in DEFAULT_CATEGORY_GLYPHS.items():
            glyph_unicode = glyph_data.get('glyph', '')
            if glyph_unicode:
                # Try to find the glyph name in the library
                glyph_obj = library.get_by_codepoint(glyph_unicode)
                if glyph_obj:
                    # Store the glyph name for reference
                    glyph_data['glyph_name'] = glyph_obj.get('name', '')
                    glyph_data['glyph_category'] = glyph_obj.get('category', '')
    except Exception as e:
        category_debug_print(f"[GLYPH] Warning: Could not integrate glyph library: {e}")
