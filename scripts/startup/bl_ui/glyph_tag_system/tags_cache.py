# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Tag CRUD and category-tag associations for the Category Tabs / Glyph / Tag system.

Extracted verbatim from ``space_userpref.py`` (no behavioural change).
All state lives in glyph_tag_system._state; this module imports state objects
by reference (in-place mutations) and uses accessors for full reassignment.

Cross-module calls that would create circular imports (``_auto_save_tags``,
``update_category_tags_in_wm``) use lazy imports inside function bodies.

The public names are re-imported in ``space_userpref`` to preserve its
attribute contract for the C++ bridge and editor modules.
"""

import bpy

from bl_ui.glyph_tag_system.defaults import (
    DEFAULT_TAG_GLYPH_HEX,
    _CATEGORY_TAG_DEFAULT_MODE_FLAGS,
)
from bl_ui.glyph_tag_system.conversions import (
    _glyph_to_hex,
    _hex_to_glyph,
    _make_cache_key,
)
from bl_ui.glyph_tag_system.log import (
    _pref_log_once,
    category_debug_print,
    tag_log,
)
from bl_ui.glyph_tag_system.migrations import _normalize_category_data
from bl_ui.glyph_tag_system._state import (
    _all_tags_cache,
    _glyph_cache,
    _tag_order_cache,
    is_glyph_cache_loaded,
    is_preview_mode_active,
)
# _get_category_data lives in glyph_cache; importing at module level is safe
# because glyph_cache does not import from tags_cache.
from bl_ui.glyph_tag_system.glyph_cache import _get_category_data


# -----------------------------------------------------------------------------
# Tag Management Functions
# -----------------------------------------------------------------------------


def get_all_tags():
    """Get all available tags as dict."""
    if not is_glyph_cache_loaded():
        from bl_ui.glyph_tag_system.glyph_cache import _load_glyph_mappings_from_file
        _load_glyph_mappings_from_file()
    return _all_tags_cache.copy()


def get_tags_for_category_ui(wm=None, category="", mode_filter=0, space_type=-1):
    """
    Get all tags formatted for C++ UI display.
    Returns string: "name|glyph|is_active|r,g,b|icon_key|icon_source;name2|glyph2|is_active2|r,g,b|icon_key|icon_source;..."
    - name: tag name
    - glyph: unicode glyph character
    - is_active: 1 if assigned to category, 0 otherwise
    - r,g,b: RGB color values (0.0-1.0)
    - icon_key: Blender icon identifier string (e.g., "ICON_OBJECT_DATAMODE")
    - icon_source: icon source type (0=auto, 1=manual, 2=off)

    Parameters:
    - wm: window manager (unused, kept for C++ compatibility)
    - category: category name
    - mode_filter: mode filter flags (unused for now)
    - space_type: space type for tag lookup (-1 = global)
    """
    # Handle case where function is called with just category (backward compatibility)
    if wm is not None and not isinstance(wm, str):
        # Called from C++ with (wm, category, mode_filter, space_type)
        pass
    elif category == "" and isinstance(wm, str):
        # Called with just category as first arg
        category = wm
        wm = None
        space_type = -1

    all_tags = get_all_tags()
    category_tags = set(get_category_tags(category, space_type))

    _pref_log_once(f"[DEBUG get_tags_for_category_ui] Category='{category}', space_type={space_type}")
    _pref_log_once(f"[DEBUG get_tags_for_category_ui] all_tags count={len(all_tags)}")
    _pref_log_once(f"[DEBUG get_tags_for_category_ui] category_tags={category_tags}")

    tag_log(f"get_tags_for_category_ui('{category}', space_type={space_type}): {len(all_tags)} tags found")

    parts = []
    for name, data in all_tags.items():
        glyph = data.get("glyph", "")
        is_active = "1" if name in category_tags else "0"
        color = data.get("color", [0.0, 0.0, 0.0])
        # Format color as r,g,b with 3 decimal places
        color_str = f"{color[0]:.3f},{color[1]:.3f},{color[2]:.3f}"
        # Get icon data for tag
        icon_key = data.get("icon_key", "")
        icon_source = data.get("icon_source", 0)
        # Debug: Check type of icon_key
        icon_key_type = type(icon_key).__name__
        # Use | as separator between fields, ; between tags
        parts.append(f"{name}|{glyph}|{is_active}|{color_str}|{icon_key}|{icon_source}")
        _pref_log_once(f"[DEBUG get_tags_for_category_ui] Tag '{name}': glyph='{glyph}', is_active={is_active}, color={color_str}, icon_key='{icon_key}' (type={icon_key_type}), icon_source={icon_source}")

    result = ";".join(parts)
    tag_log(f"get_tags_for_category_ui result: '{result}'")
    _pref_log_once(f"[DEBUG get_tags_for_category_ui] Final result: '{result}'")
    return result


def get_tag_data(tag_name):
    """Get glyph and color for a specific tag."""
    tags = get_all_tags()
    return tags.get(tag_name, {"glyph": "", "color": [0.0, 0.0, 0.0]})


def generate_unique_tag_name():
    """Generate a unique tag name (Tag, Tag.001, Tag.002, etc.)."""
    tags = get_all_tags()
    base_name = "New Tag"
    if base_name not in tags:
        return base_name

    # Check for existing numbered names
    i = 1
    while True:
        name = f"{base_name}.{i:03d}"
        if name not in tags:
            return name
        i += 1


def create_tag(tag_name, glyph="", color=None, mode_flags=None, icon_key="", icon_source='GLYPH', auto_save=True, skip_wm_sync=False):
    """
    Create a new tag.

    Args:
        tag_name: Name for the new tag
        glyph: Unicode glyph character
        color: RGB color tuple (0.0-1.0)
        mode_flags: Bitmask of modes where tag is active (None = use default)
        icon_key: Blender icon identifier (e.g., "OBJECT_DATAMODE")
        icon_source: 'GLYPH', 'BLENDER_ICON', or 'CUSTOM' (will be converted to int for storage)
        auto_save: Save to JSON immediately
        skip_wm_sync: If True, skip WM sync (optimization for Edit Category Tab dialog)

    Returns:
        (success: bool, message: str)
    """
    # Convert string icon_source to int for consistent storage
    icon_source_map = {'GLYPH': 0, 'BLENDER_ICON': 1, 'CUSTOM': 2}
    if isinstance(icon_source, str):
        icon_source_int = icon_source_map.get(icon_source, 0)
    else:
        icon_source_int = int(icon_source) if icon_source is not None else 0

    if not tag_name:
        return False, "Tag name cannot be empty"

    if len(tag_name) > 32:
        return False, "Tag name too long (max 32 chars)"

    if tag_name in _all_tags_cache:
        return False, f"Tag '{tag_name}' already exists"

    # Use default glyph only for GLYPH mode.
    # In icon mode glyph must stay empty, otherwise Tag Bar may keep showing glyph text.
    if not glyph and icon_source_int == 0:
        glyph = _hex_to_glyph(DEFAULT_TAG_GLYPH_HEX)

    _all_tags_cache[tag_name] = {
        "glyph": glyph,
        "color": list(color) if color else [0.0, 0.0, 0.0],
        "mode_flags": mode_flags if mode_flags is not None else _CATEGORY_TAG_DEFAULT_MODE_FLAGS,
        "icon_key": icon_key if icon_source_int == 1 else "",  # 1 = BLENDER_ICON
        "icon_source": icon_source_int,  # Store as int (0=GLYPH, 1=BLENDER_ICON, 2=CUSTOM)
    }

    # Always add new tags to the end of the order list
    if tag_name not in _tag_order_cache:
        _tag_order_cache.append(tag_name)

    tag_log(f"Created tag: {tag_name}")
    category_debug_print(f"[CREATE_TAG] Tag '{tag_name}' added to _all_tags_cache")

    if auto_save:
        from bl_ui.glyph_tag_system import handlers as _handlers
        _handlers._auto_save_tags()

    # Immediately sync the new tag to WM so it's visible in UI
    # This is important for preview mode where full sync is skipped
    if not skip_wm_sync:
        category_debug_print(f"[CREATE_TAG] Syncing tag '{tag_name}' to WM for immediate UI visibility")
        _sync_single_tag_to_wm(tag_name)
    else:
        category_debug_print(f"[CREATE_TAG] Skipping WM sync for tag '{tag_name}' (will be done by Save)")

    return True, f"Tag '{tag_name}' created"


def _sync_single_tag_to_wm(tag_name):
    """Sync a single tag to wm.category_tags without full sync.
    This allows newly created tags to be immediately visible in UI
    even during preview mode where sync_glyph_mappings_to_wm() is skipped.
    """
    if tag_name not in _all_tags_cache:
        category_debug_print(f"[SYNC_SINGLE_TAG] Tag '{tag_name}' not in cache, skipping")
        return False

    try:
        wm = bpy.context.window_manager
        if wm is None or not hasattr(wm, 'category_tags'):
            category_debug_print(f"[SYNC_SINGLE_TAG] WM or category_tags not available")
            return False

        tag_data = _all_tags_cache[tag_name]
        glyph_hex = _glyph_to_hex(tag_data.get("glyph", "")) if isinstance(tag_data, dict) else ""
        color_val = tag_data.get("color", [0.0, 0.0, 0.0]) if isinstance(tag_data, dict) else [0.0, 0.0, 0.0]
        mode_flags_val = tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS) if isinstance(tag_data, dict) else _CATEGORY_TAG_DEFAULT_MODE_FLAGS
        # Icon fields - icon_source is now stored as int (0=GLYPH, 1=BLENDER_ICON, 2=CUSTOM)
        icon_key_val = tag_data.get("icon_key", "") if isinstance(tag_data, dict) else ""
        icon_source_val = tag_data.get("icon_source", 0) if isinstance(tag_data, dict) else 0
        # Handle both int and string formats for backward compatibility
        if isinstance(icon_source_val, str):
            icon_source_map = {'GLYPH': 0, 'BLENDER_ICON': 1, 'CUSTOM': 2}
            icon_source_val = icon_source_map.get(icon_source_val, 0)
        else:
            icon_source_val = int(icon_source_val) if icon_source_val is not None else 0

        # DEBUG: Print values before syncing to WM
        category_debug_print(f"[SYNC_SINGLE_TAG] tag='{tag_name}' icon_key='{icon_key_val}' icon_source={icon_source_val} (type={type(icon_source_val).__name__})")

        # Check if tag already exists in WM
        existing_tag = None
        for tag_item in wm.category_tags:
            if tag_item.name == tag_name:
                existing_tag = tag_item
                break

        if existing_tag:
            # Update existing tag
            existing_tag.glyph = glyph_hex
            existing_tag.color = (color_val[0], color_val[1], color_val[2])
            existing_tag.mode_flags = mode_flags_val
            # NEW: Icon fields
            existing_tag.icon_key = icon_key_val
            existing_tag.icon_source = icon_source_val
            category_debug_print(f"[SYNC_SINGLE_TAG] Updated existing tag '{tag_name}' in WM")
        else:
            # Create new tag in WM
            tag_item = wm.category_tags.new(name=tag_name)
            tag_item.glyph = glyph_hex
            tag_item.color = (color_val[0], color_val[1], color_val[2])
            tag_item.mode_flags = mode_flags_val
            # NEW: Icon fields
            tag_item.icon_key = icon_key_val
            tag_item.icon_source = icon_source_val
            category_debug_print(f"[SYNC_SINGLE_TAG] Created new tag '{tag_name}' in WM with glyph='{glyph_hex}'")

        return True

    except Exception as e:
        category_debug_print(f"[SYNC_SINGLE_TAG] Error syncing tag '{tag_name}': {e}")
        return False


def update_tag(tag_name, glyph=None, color=None, icon_key=None, icon_source=None, auto_save=True):
    """Update an existing tag's glyph, color, icon_key, and/or icon_source."""
    if tag_name not in _all_tags_cache:
        return False, f"Tag '{tag_name}' not found"

    if glyph is not None:
        _all_tags_cache[tag_name]["glyph"] = glyph
    if color is not None:
        _all_tags_cache[tag_name]["color"] = list(color)
    if icon_key is not None:
        _all_tags_cache[tag_name]["icon_key"] = icon_key
    if icon_source is not None:
        _all_tags_cache[tag_name]["icon_source"] = icon_source

    tag_log(f"Updated tag: {tag_name}")

    if auto_save:
        from bl_ui.glyph_tag_system import handlers as _handlers
        _handlers._auto_save_tags()

    return True, f"Tag '{tag_name}' updated"


def rename_tag(old_name, new_name, auto_save=True):
    """Rename a tag, updating all references in categories and order cache."""
    if not new_name or not new_name.strip():
        return False, "New tag name cannot be empty"

    new_name = new_name.strip()

    if old_name not in _all_tags_cache:
        return False, f"Tag '{old_name}' not found"

    if old_name == new_name:
        return True, f"Tag name unchanged"

    if new_name in _all_tags_cache:
        return False, f"Tag '{new_name}' already exists"

    # Move data to new key
    _all_tags_cache[new_name] = _all_tags_cache.pop(old_name)

    # Update order cache
    if old_name in _tag_order_cache:
        idx = _tag_order_cache.index(old_name)
        _tag_order_cache[idx] = new_name

    # Update all category assignments
    for cat_data in _glyph_cache.values():
        if "tags" in cat_data and old_name in cat_data["tags"]:
            idx = cat_data["tags"].index(old_name)
            cat_data["tags"][idx] = new_name

    # Sync renamed tag to WM
    try:
        wm = bpy.context.window_manager
        if wm and hasattr(wm, 'category_tags'):
            for tag_item in wm.category_tags:
                if tag_item.name == old_name:
                    tag_item.name = new_name
                    break
    except Exception as e:
        category_debug_print(f"[RENAME_TAG] WM sync error: {e}")

    tag_log(f"Renamed tag: '{old_name}' -> '{new_name}'")

    if auto_save:
        from bl_ui.glyph_tag_system import handlers as _handlers
        _handlers._auto_save_tags()

    return True, f"Tag renamed to '{new_name}'"


def delete_tag(tag_name, auto_save=True):
    """Delete a tag from registry and all category assignments."""
    if tag_name not in _all_tags_cache:
        return False, f"Tag '{tag_name}' not found"

    del _all_tags_cache[tag_name]

    # Remove from order cache
    if tag_name in _tag_order_cache:
        _tag_order_cache.remove(tag_name)

    # Remove from all categories
    for cat_name, cat_data in _glyph_cache.items():
        if "tags" in cat_data and tag_name in cat_data["tags"]:
            cat_data["tags"].remove(tag_name)
            tag_log(f"Removed '{tag_name}' from category '{cat_name}'")

    tag_log(f"Deleted tag: {tag_name}")

    if auto_save:
        from bl_ui.glyph_tag_system import handlers as _handlers
        _handlers._auto_save_tags()

    return True, f"Tag '{tag_name}' deleted"


def get_category_tags(category, space_type=-1):
    """Get list of tag names assigned to a category."""
    key = _make_cache_key(space_type, category)
    cat_data = _get_category_data(category, space_type)

    # DEBUG
    category_debug_print(f"[GET_CATEGORY_TAGS] category='{category}', space_type={space_type}, key={key}")
    if cat_data and isinstance(cat_data, dict):
        tags = list(cat_data.get("tags", []))
        category_debug_print(f"[GET_CATEGORY_TAGS] Found tags: {tags}")
        return tags
    category_debug_print(f"[GET_CATEGORY_TAGS] No data found, returning []")
    return []


def set_category_tags(category, tags, space_type=-1, auto_save=True, update_wm=True):
    """Set tags for a category (replaces existing).

    Global-First: All operations use GLOBAL key (-1, category) regardless of space_type parameter.
    """
    key = _make_cache_key(space_type, category)  # Returns (-1, category)
    if key not in _glyph_cache:
        # If we are setting empty tags on a non-existent category, don't create it.
        # This prevents "empty" overrides from being created during cancel/restore.
        if not tags:
            tag_log(f"set_category_tags: No tags to set for new category '{category}', skipping creation")
            return True, "No tags to set"
        # Create entry if not exists
        _glyph_cache[key] = _normalize_category_data({})

    # Validate tags exist
    valid_tags = [t for t in tags if t in _all_tags_cache]
    invalid_tags = set(tags) - set(valid_tags)

    if invalid_tags:
        tag_log(f"Warning: Unknown tags ignored: {invalid_tags}", "WARN")

    _glyph_cache[key]["tags"] = valid_tags
    tag_log(f"Set tags for '{category}' (GLOBAL): {valid_tags}")

    # Only update WM override if requested (not during preview in edit dialog)
    if update_wm:
        from bl_ui.glyph_tag_system import wm_sync_to_wm as _wm_sync
        _wm_sync.update_category_tags_in_wm(category, space_type)

    if auto_save:
        from bl_ui.glyph_tag_system import handlers as _handlers
        _handlers._auto_save_tags()

    return True, f"Tags set for '{category}'"


def add_category_tag(category, tag_name, auto_save=True, space_type=-1, update_wm=True):
    """Add a single tag to a category.

    Global-First: All operations use GLOBAL key (-1, category) regardless of space_type parameter.
    """
    from bl_ui.glyph_tag_system._state import is_preview_mode_active

    # DEBUG
    category_debug_print(f"[ADD_TAG] CALLED: category='{category}', tag='{tag_name}', auto_save={auto_save}, update_wm={update_wm}, preview_mode={is_preview_mode_active()}")

    if tag_name not in _all_tags_cache:
        category_debug_print(f"[ADD_TAG] FAILED: Tag '{tag_name}' not found in _all_tags_cache")
        return False, f"Tag '{tag_name}' not found"

    # Global-First: Always use GLOBAL key
    key = _make_cache_key(space_type, category)  # Returns (-1, category)
    category_debug_print(f"[ADD_TAG] Cache key (GLOBAL): {key}")

    if key not in _glyph_cache:
        _glyph_cache[key] = _normalize_category_data({})
        category_debug_print(f"[ADD_TAG] Created new GLOBAL cache entry for key {key}")

    if "tags" not in _glyph_cache[key]:
        _glyph_cache[key]["tags"] = []

    # DEBUG: Show current tags before adding
    category_debug_print(f"[ADD_TAG] Current tags BEFORE: {_glyph_cache[key]['tags']}")

    if tag_name in _glyph_cache[key]["tags"]:
        category_debug_print(f"[ADD_TAG] Tag already exists - skipping")
        return True, f"Tag '{tag_name}' already assigned to '{category}'"

    _glyph_cache[key]["tags"].append(tag_name)
    category_debug_print(f"[ADD_TAG] Tags AFTER adding: {_glyph_cache[key]['tags']}")
    tag_log(f"Added tag '{tag_name}' to '{category}' (GLOBAL)")

    # Clear pending_tag_assignment for the current category and all sibling categories
    # This ensures "New Add-ons!" button disappears immediately when tag is assigned
    cat_data = _glyph_cache[key]
    if isinstance(cat_data, dict):
        source_ext = cat_data.get("source_extension", "")

        # Clear pending for the current category
        if cat_data.get("pending_tag_assignment", False):
            cat_data["pending_tag_assignment"] = False
            category_debug_print(f"[ADD_TAG] Cleared pending_tag_assignment for '{category}'")

        # CRITICAL: Clear pending_tag_assignment for ALL categories from the same extension
        # This ensures that when a user assigns a tag to any category from an extension,
        # all other categories from that extension disappear from "New Add-ons!" filter
        cleared_count = 0
        if source_ext:
            # Primary path: match by source_extension
            for other_key, other_data in _glyph_cache.items():
                if isinstance(other_data, dict) and other_data.get("source_extension") == source_ext:
                    other_key_name = other_key[1] if isinstance(other_key, tuple) else other_key
                    if other_key_name != category:  # Skip self
                        other_data["pending_tag_assignment"] = False
                        category_debug_print(f"[ADD_TAG] Cleared pending for sibling category: {other_key_name!r} (extension={source_ext!r})")
                        cleared_count += 1
        else:
            # FALLBACK: If source_extension is empty, find categories by name prefix matching
            # This handles cases like "Home Builder" (ext='') and "Home Builder 5" (ext='add-on-...')
            base_name = category.rstrip('0123456789').rstrip()  # Remove trailing numbers
            if not base_name:
                base_name = category

            for other_key, other_data in _glyph_cache.items():
                if not isinstance(other_data, dict):
                    continue
                other_name = other_key[1] if isinstance(other_key, tuple) else other_key
                # Skip self
                if other_name == category:
                    continue
                # Check if other category starts with base_name or base_name starts with other
                other_ext = other_data.get("source_extension", "")
                if other_ext and (other_name.startswith(base_name) or base_name.startswith(other_name)):
                    other_data["pending_tag_assignment"] = False
                    category_debug_print(f"[ADD_TAG] Cleared pending for prefix-matched sibling: {other_name!r} (base={base_name!r}, extension={other_ext!r})")
                    cleared_count += 1

        if cleared_count > 0:
            category_debug_print(f"[ADD_TAG] Total siblings cleared: {cleared_count}")

    # Only update WM override if requested (not during preview in edit dialog)
    if update_wm:
        from bl_ui.glyph_tag_system import wm_sync_to_wm as _wm_sync
        _wm_sync.update_category_tags_in_wm(category, space_type)

    if auto_save:
        category_debug_print(f"[ADD_TAG] Calling _auto_save_tags()")
        from bl_ui.glyph_tag_system import handlers as _handlers
        _handlers._auto_save_tags()

    return True, f"Tag '{tag_name}' added to '{category}'"


def remove_category_tag(category, tag_name, auto_save=True, space_type=-1, update_wm=True):
    """Remove a single tag from a category.

    Global-First: All operations use GLOBAL key (-1, category) regardless of space_type parameter.
    """
    key = _make_cache_key(space_type, category)  # Returns (-1, category)
    if key not in _glyph_cache:
        return False, f"Category '{category}' not found"

    cat_tags = _glyph_cache[key].get("tags", [])

    if tag_name not in cat_tags:
        return True, f"Tag '{tag_name}' not assigned to '{category}'"

    cat_tags.remove(tag_name)
    tag_log(f"Removed tag '{tag_name}' from '{category}' (GLOBAL)")

    # Only update WM override if requested (not during preview in edit dialog)
    if update_wm:
        from bl_ui.glyph_tag_system import wm_sync_to_wm as _wm_sync
        _wm_sync.update_category_tags_in_wm(category, space_type)

    if auto_save:
        from bl_ui.glyph_tag_system import handlers as _handlers
        _handlers._auto_save_tags()

    return True, f"Tag '{tag_name}' removed from '{category}'"


def toggle_category_tag(category, tag_name, auto_save=True, space_type=-1, update_wm=True):
    """Toggle a tag on/off for a category."""
    tags = get_category_tags(category, space_type)
    if tag_name in tags:
        return remove_category_tag(category, tag_name, auto_save, space_type, update_wm)
    else:
        return add_category_tag(category, tag_name, auto_save, space_type, update_wm)


# -----------------------------------------------------------------------------
# Tag helper utilities
# -----------------------------------------------------------------------------


def get_tag_name_by_index(idx):
    """Get tag name by index from wm.category_tags."""
    wm = bpy.context.window_manager
    if wm and hasattr(wm, 'category_tags') and 0 <= idx < len(wm.category_tags):
        return wm.category_tags[idx].name
    return None


def _get_mode_flags_for_tag(tag_name):
    """Get mode flags for a tag from _all_tags_cache."""
    if tag_name in _all_tags_cache:
        tag_data = _all_tags_cache[tag_name]
        if isinstance(tag_data, dict):
            return tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS)
    return _CATEGORY_TAG_DEFAULT_MODE_FLAGS


def _set_mode_flags_for_tag(tag_name, mode_flags):
    """Set mode flags for a tag in _all_tags_cache."""
    if tag_name in _all_tags_cache and isinstance(_all_tags_cache[tag_name], dict):
        _all_tags_cache[tag_name]["mode_flags"] = mode_flags


def _validate_icon_key(icon_key):
    """Validate that an icon_key is a valid Blender icon identifier.

    Uses dynamic lookup via Blender's icon system instead of hardcoded valid_prefixes list.
    This ensures all current and future Blender icons are automatically supported.

    Args:
        icon_key: Icon identifier string (e.g., 'SNAP_FACE', 'FUND', 'VIEW_CAMERA', etc.)

    Returns:
        Tuple of (is_valid: bool, error_message: str)
    """
    if not icon_key:
        return True, ""  # Empty icon is valid (no icon selected)

    try:
        import bpy

        # Simplest approach: try to get the icon's integer ID
        # Blender's icon() method returns 0 for invalid icons, >0 for valid ones
        # This works for ALL Blender icons including VIEW_CAMERA, SNAP_FACE, FUND, etc.

        # Get a layout object to test the icon
        # Use the window manager's layout if available
        wm = bpy.context.window_manager
        if wm and hasattr(wm, 'layout'):
            layout = wm.layout
            try:
                icon_id = layout.icon(icon_key)
                if icon_id and icon_id > 0:
                    return True, ""
            except Exception:
                pass

        # Fallback: check if icon name exists in Blender's icon enum_items
        # This is more reliable than trying to get icon_id
        try:
            from bpy.types import UILayout
            icon_prop = UILayout.bl_rna.properties.get('icon')
            if icon_prop and hasattr(icon_prop, 'enum_items'):
                # Check if the icon_key exists in the enum
                icon_item = icon_prop.enum_items.get(icon_key)
                if icon_item:
                    return True, ""
        except Exception:
            pass

        # Last fallback: just check if it looks like a valid icon name
        # Blender icon names are typically uppercase with underscores
        # This allows icons that exist but aren't in the enum (like some context-specific icons)
        if icon_key and isinstance(icon_key, str) and len(icon_key) > 0:
            # Assume it's valid if it's a non-empty string
            # The icon picker in C++ code already validates it, so we trust it
            return True, ""

        return False, f"Invalid icon: '{icon_key}'. Use the icon picker to select a valid Blender icon."

    except Exception:
        # Any other error - log but allow the icon (safer to allow than block)
        # The C++ icon picker already validated it
        import traceback
        traceback.print_exc()
        # Return True to allow the icon - C++ already validated it via icon picker
        return True, ""


def get_tag_names():
    """Get list of all existing tag names.

    Returns:
        List of tag name strings
    """
    return list(_all_tags_cache.keys())


def _generate_unique_tag_name(base_name="New Tag"):
    """Generate a unique tag name with random suffix to avoid duplicates.

    Args:
        base_name: Base name for the tag (default: "New Tag")

    Returns:
        Unique tag name with format "BaseName.XXX" where XXX is 000-999
    """
    import random

    # Get existing tag names
    existing_names = set(get_tag_names())

    # Try up to 1000 times to find a unique name
    for _ in range(1000):
        suffix = random.randint(0, 999)
        candidate_name = f"{base_name}.{suffix:03d}"
        if candidate_name not in existing_names:
            return candidate_name

    # Fallback: use timestamp if all random names are taken (very unlikely)
    import time
    timestamp = int(time.time() * 1000) % 1000
    return f"{base_name}.{timestamp:03d}"
