# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""WM synchronisation — bidirectional sync between glyph/tags cache and WindowManager RNA.

Extracted verbatim from ``space_userpref.py`` (no behavioural change).
All state lives in glyph_tag_system._state; this module imports state objects
by reference (in-place mutations) and uses accessors for full reassignment.

Cross-module calls to ``_auto_save_tags``, ``_cancel_deferred_auto_save``,
``_merge_discovered_categories``, and ``_auto_detect_extension_icon_path``
use lazy imports inside function bodies to avoid circular dependencies.

The public names are re-imported in space_userpref to preserve its attribute
contract for the C++ bridge and editor modules.
"""

import bpy
import time

from bl_ui.glyph_tag_system.defaults import (
    DEFAULT_CATEGORY_GLYPHS,
    TAG_DEBUG,
    _CATEGORY_TAG_DEFAULT_MODE_FLAGS,
)
from bl_ui.glyph_tag_system.conversions import (
    _glyph_to_hex,
    _hex_to_glyph,
    _is_single_glyph,
    _is_valid_category_name,
    _make_cache_key,
    _unicode_escape_to_glyph,
    flags_to_modes,
    flags_to_spaces,
    modes_to_flags,
    spaces_to_flags,
)
from bl_ui.glyph_tag_system.log import (
    _pref_log_once,
    _sync_miss_log_once,
    category_debug_print,
    tag_log,
)
from bl_ui.glyph_tag_system._state import (
    _all_tags_cache,
    _category_orders_cache,
    _glyph_cache,
    _tag_order_cache,
    reset_all_tags_cache,
    set_auto_sync_timer,
    set_background_sync_timer,
    set_initial_load_complete,
    set_preview_mode_active,
    set_sync_in_progress,
    set_auto_save_pending,
    increment_background_sync_run_count,
    is_glyph_cache_loaded,
    is_initial_load_complete,
    is_preview_mode_active,
    is_sync_in_progress,
    get_auto_sync_timer,
    get_background_sync_timer,
)
from bl_ui.glyph_tag_system.glyph_cache import (
    _is_collection_safe,
    _integrate_glyph_library,
    _load_glyph_mappings_from_file,
    _normalize_category_data,
    _save_glyph_mappings_to_file,
)
from bl_ui.glyph_tag_system.tags_cache import (
    add_category_tag,
    get_category_tags,
    remove_category_tag,
    set_category_tags,
)


# -----------------------------------------------------------------------------
# Tag manipulation + WM update functions (cache -> WM direction)
# -----------------------------------------------------------------------------


def toggle_category_tag_no_save(category, tag_name, space_type=-1):
    """Toggle a tag on/off for a category WITHOUT auto-saving to JSON.

    This is used for live preview in the edit dialog. Changes are only
    persisted when the user clicks Save.

    NOTE: We DO update wm.category_glyph_overrides so that the C++ UI
    (category_tags_string_lookup) can see tag assignments immediately.
    We only skip sync_glyph_mappings_to_wm() to prevent premature filtering.
    """

    # Set preview mode to prevent automatic WM sync during tag operations
    set_preview_mode_active(True)
    if TAG_DEBUG:
        category_debug_print(f"[DEBUG TOGGLE_NO_SAVE] SET _preview_mode_active=True for category='{category}', tag='{tag_name}'")
    try:
        # DEBUG
        category_debug_print(f"[TOGGLE_NO_SAVE] CALLED: category='{category}', tag='{tag_name}', space_type={space_type} (preview_mode=True)")

        tags = get_category_tags(category, space_type)
        category_debug_print(f"[TOGGLE_NO_SAVE] Current tags for '{category}': {tags}")

        if tag_name in tags:
            category_debug_print(f"[TOGGLE_NO_SAVE] Tag exists - will REMOVE")
            result = remove_category_tag(category, tag_name, auto_save=False, space_type=space_type, update_wm=False)
            category_debug_print(f"[TOGGLE_NO_SAVE] REMOVE result: {result}")
            # Verify the tag was removed
            tags_after = get_category_tags(category, space_type)
            category_debug_print(f"[TOGGLE_NO_SAVE] Tags after REMOVE: {tags_after}")
        else:
            category_debug_print(f"[TOGGLE_NO_SAVE] Tag not exists - will ADD")
            result = add_category_tag(category, tag_name, auto_save=False, space_type=space_type, update_wm=False)
            category_debug_print(f"[TOGGLE_NO_SAVE] ADD result: {result}")
            # Verify the tag was added
            tags_after = get_category_tags(category, space_type)
            category_debug_print(f"[TOGGLE_NO_SAVE] Tags after ADD: {tags_after}")

        # Any regular tag toggle means "Without Tag" preview must be visually OFF.
        # This will also call update_category_tags_in_wm internally to sync WM override.
        _set_without_tag_preview_state_in_wm(category, space_type, is_selected=False)

        return result
    finally:
        # Keep preview mode active until Save/Cancel - do NOT reset here
        # This prevents WM sync during context.area.tag_redraw() call
        category_debug_print(f"[TOGGLE_NO_SAVE] Keeping preview mode active until Save/Cancel")


def clear_category_tags_no_save(category, space_type=-1):
    """Clear all tags for a category WITHOUT auto-saving to JSON.

    This is used for live preview in the edit dialog. Changes are only
    persisted when the user clicks Save.

    Global-First: All operations use GLOBAL key (-1, category).

    NOTE: We DO update wm.category_glyph_overrides so that the C++ UI
    (category_tags_string_lookup) can see tag assignments immediately.
    We only skip sync_glyph_mappings_to_wm() to prevent premature filtering.
    """
    global _glyph_cache

    # Set preview mode to prevent automatic WM sync during tag operations
    set_preview_mode_active(True)
    try:
        # DEBUG
        category_debug_print(f"[CLEAR_NO_SAVE] CALLED: category='{category}' (preview_mode=True)")

        # Global-First: Always use GLOBAL key
        key = _make_cache_key(space_type, category)  # Returns (-1, category)
        cat_data = _glyph_cache.get(key)

        if cat_data is None:
            category_debug_print(f"[CLEAR_NO_SAVE] Category '{category}' not found in cache")
            return False, f"Category '{category}' not found"

        if not isinstance(cat_data, dict):
            cat_data = _normalize_category_data(cat_data)
            _glyph_cache[key] = cat_data

        without_tag_selected = _is_without_tag_preview_selected_in_wm(category, space_type)

        if without_tag_selected:
            # Toggle OFF: return to pending/unassigned state for extension categories.
            if cat_data.get("source_extension", ""):
                cat_data["pending_tag_assignment"] = True
            # Clear the without_tag_preview flag since user deselected
            if cat_data.get("without_tag_preview", False):
                cat_data["without_tag_preview"] = False
            # This will also call update_category_tags_in_wm internally to sync WM override.
            _set_without_tag_preview_state_in_wm(category, space_type, is_selected=False)
            category_debug_print(f"[CLEAR_NO_SAVE] Toggled OFF 'Without Tag' for '{category}'")
            return True, f"Without Tag deselected for '{category}' (preview mode)"

        # Toggle ON: clear all tags in cache (but don't save yet)
        old_tags = cat_data.get("tags", [])
        cat_data["tags"] = []

        # CRITICAL: Keep pending_tag_assignment=True so category stays visible in "New Add-ons!"
        # until Save. We mark this as a "without_tag_preview" selection which will be processed
        # on Save to set pending_tag_assignment=False.
        # This ensures consistent behavior with regular tags - category disappears only after Save.
        cat_data["without_tag_preview"] = True

        category_debug_print(f"[CLEAR_NO_SAVE] Cleared tags {old_tags} for '{category}' (keeping pending=True for preview)")

        # Mark explicit "Without Tag" selection for dialog preview UI.
        # This will also call update_category_tags_in_wm internally to sync WM override.
        _set_without_tag_preview_state_in_wm(category, space_type, is_selected=True)

        return True, f"Tags cleared for '{category}' (preview mode)"
    finally:
        # Keep preview mode active until Save/Cancel - do NOT reset here
        # This prevents WM sync during context.area.tag_redraw() call
        category_debug_print(f"[CLEAR_NO_SAVE] Keeping preview mode active until Save/Cancel")


def restore_category_tags_from_string(category, tags_string, space_type=-1):
    """Restore category tags from a semicolon-separated string.

    This is used when cancelling the edit dialog to revert changes.
    Properly restores pending_tag_assignment=True for unassigned extension categories.
    Also handles cleanup of any tags that were created during preview but should be cancelled.
    """

    # Exit preview mode when restoring (Cancel operation)
    set_preview_mode_active(False)
    category_debug_print(f"[RESTORE] Preview mode disabled for Cancel operation on '{category}'")

    # Cancel any queued deferred save from preview-time updates.
    from bl_ui.glyph_tag_system.handlers import _cancel_deferred_auto_save
    _cancel_deferred_auto_save(reason=f"cancel_restore_tags:{category}")

    if not tags_string:
        tags = []
    else:
        tags = [t.strip() for t in tags_string.split(';') if t.strip()]

    tag_log(f"Restoring tags for '{category}' from string: '{tags_string}' -> {tags} (space_type={space_type})")

    # Save original pending_tag_assignment BEFORE any modifications.
    # This prevents incorrectly setting pending=True for categories that were
    # intentionally saved as "Without Tag" (pending=False).
    original_pending = None
    _pending_key = _make_cache_key(space_type, category)
    if _pending_key not in _glyph_cache:
        _pending_key = _make_cache_key(-1, category)
    if _pending_key in _glyph_cache:
        original_pending = _glyph_cache[_pending_key].get("pending_tag_assignment", False)

    # Use update_wm=False during restore to prevent premature filtering changes
    result = set_category_tags(category, tags, space_type=space_type, auto_save=False, update_wm=False)

    # If restoring empty tags and category has source_extension, restore pending_tag_assignment=True
    # BUT ONLY if the category is not already in category_orders AND was originally pending=True.
    # Categories that were intentionally saved as "Without Tag" (pending=False) must stay False.
    if not tags:
        key = _make_cache_key(space_type, category)
        if key not in _glyph_cache:
            key = _make_cache_key(-1, category)  # Try global key

        if key in _glyph_cache:
            cat_data = _glyph_cache[key]
            if isinstance(cat_data, dict) and cat_data.get("source_extension", ""):
                # Check if category is already in any category_orders (i.e., was dropped on tabs)
                global _category_orders_cache
                if not is_glyph_cache_loaded():
                    _load_glyph_mappings_from_file()

                is_in_orders = False
                for tag_key, order_list in _category_orders_cache.items():
                    if category in order_list:
                        is_in_orders = True
                        tag_log(f"restore_category_tags_from_string: category '{category}' found in category_orders['{tag_key}'], NOT restoring pending=True")
                        break

                if not is_in_orders and original_pending:
                    cat_data["pending_tag_assignment"] = True
                    # Also clear without_tag_preview flag if it was set during preview
                    if cat_data.get("without_tag_preview", False):
                        cat_data["without_tag_preview"] = False
                        tag_log(f"restore_category_tags_from_string: cleared without_tag_preview for '{category}'")
                    tag_log(f"restore_category_tags_from_string: restored pending=True for '{category}' (space_type={space_type})")
                    # Clear "Without Tag" selection in WM
                    _set_without_tag_preview_state_in_wm(category, space_type, is_selected=False)
                    # Trigger WM sync to update "New Add-ons!" filter after restoring pending status
                    # OPTIMIZATION: Skip icon detection (will run in background)
                    sync_glyph_mappings_to_wm(skip_icon_detection=True)
                elif not is_in_orders and not original_pending:
                    tag_log(f"restore_category_tags_from_string: NOT restoring pending=True for '{category}' (was intentionally saved as Without Tag, original_pending={original_pending})")

    # Note: We don't delete tags that were created during preview mode here,
    # as they might be used by other categories. The tag creation itself is permanent,
    # only the assignment to this specific category is being cancelled.

    return result


def restore_category_glyph_from_snapshot(category, glyph_hex, glyph_mode, color, space_type=-1,
                                          icon_source='GLYPH', icon_key="", icon_path="", icon_provider=""):
    """Restore category glyph data from snapshot values.

    This is used when cancelling the edit dialog to revert Reset changes.
    Restores glyph, glyph_mode, color, and icon fields in both GLOBAL and space-specific entries.

    Args:
        category: Category name
        glyph_hex: Glyph hex code (e.g., "f722" or empty string)
        glyph_mode: Glyph mode (0=auto, 1=first_letter)
        color: Color as [r, g, b] list
        space_type: Space type (-1 for global)
        icon_source: Icon source ('GLYPH', 'BLENDER_ICON', 'CUSTOM')
        icon_key: Blender icon key (e.g., "FUND" or empty string)
        icon_path: Custom icon file path (or empty string)
        icon_provider: Icon provider (e.g., "extension_auto" or empty string)
    """
    global _glyph_cache

    # Cancel any queued deferred save from preview-time updates.
    from bl_ui.glyph_tag_system.handlers import _cancel_deferred_auto_save
    _cancel_deferred_auto_save(reason=f"cancel_restore_glyph:{category}")

    if not is_glyph_cache_loaded():
        _load_glyph_mappings_from_file()

    category_debug_print(f"[RESTORE GLYPH] Restoring glyph for '{category}': glyph_hex='{glyph_hex}', glyph_mode={glyph_mode}, color={color}, space_type={space_type}")
    category_debug_print(f"[RESTORE ICON] Restoring icon: icon_source={icon_source}, icon_key='{icon_key}', icon_path='{icon_path}', icon_provider='{icon_provider}'")

    # Convert glyph_hex to glyph character if needed
    glyph = ""
    if glyph_hex:
        if glyph_hex.startswith("\\u"):
            glyph = _unicode_escape_to_glyph(glyph_hex)
        elif glyph_hex.startswith("U+"):
            glyph = _hex_to_glyph(glyph_hex[2:])
        else:
            glyph = _hex_to_glyph(glyph_hex)

    category_debug_print(f"[RESTORE GLYPH] Converted glyph_hex '{glyph_hex}' to glyph '{glyph}'")

    # Map icon_source int to string
    icon_source_map = {0: "auto", 1: "manual", 2: "off"}
    icon_source_str = icon_source_map.get(icon_source, "auto")

    # Restore in GLOBAL entry
    global_key = _make_cache_key(-1, category)
    if global_key in _glyph_cache:
        global_entry = _glyph_cache[global_key]
        category_debug_print(f"[RESTORE GLYPH] Before restore GLOBAL: glyph='{global_entry.get('glyph', '')}', glyph_mode='{global_entry.get('glyph_mode', 'auto')}'")
        global_entry["glyph"] = glyph
        global_entry["glyph_mode"] = "auto" if glyph_mode == 0 else "first_letter"
        global_entry["color"] = list(color) if color else [0.0, 0.0, 0.0]
        # Restore icon fields
        if "icon" not in global_entry:
            global_entry["icon"] = {}
        global_entry["icon"]["source"] = icon_source_str
        global_entry["icon"]["key"] = icon_key if icon_key else ""
        global_entry["icon"]["path"] = icon_path if icon_path else ""
        global_entry["icon"]["provider"] = icon_provider if icon_provider else ""
        category_debug_print(f"[RESTORE GLYPH] After restore GLOBAL: glyph='{global_entry.get('glyph', '')}', glyph_mode='{global_entry.get('glyph_mode', 'auto')}'")
        category_debug_print(f"[RESTORE ICON] After restore GLOBAL icon: source='{global_entry['icon'].get('source', 'auto')}', key='{global_entry['icon'].get('key', '')}'")

    # Restore in space-specific entry if space_type is provided
    if space_type != -1:
        space_key = _make_cache_key(space_type, category)
        if space_key in _glyph_cache:
            space_entry = _glyph_cache[space_key]
            category_debug_print(f"[RESTORE GLYPH] Before restore SPACE({space_type}): glyph='{space_entry.get('glyph', '')}', glyph_mode='{space_entry.get('glyph_mode', 'auto')}'")
            space_entry["glyph"] = glyph
            space_entry["glyph_mode"] = "auto" if glyph_mode == 0 else "first_letter"
            space_entry["color"] = list(color) if color else [0.0, 0.0, 0.0]
            # Restore icon fields
            if "icon" not in space_entry:
                space_entry["icon"] = {}
            space_entry["icon"]["source"] = icon_source_str
            space_entry["icon"]["key"] = icon_key if icon_key else ""
            space_entry["icon"]["path"] = icon_path if icon_path else ""
            space_entry["icon"]["provider"] = icon_provider if icon_provider else ""
            category_debug_print(f"[RESTORE GLYPH] After restore SPACE({space_type}): glyph='{space_entry.get('glyph', '')}', glyph_mode='{space_entry.get('glyph_mode', 'auto')}'")
            category_debug_print(f"[RESTORE ICON] After restore SPACE({space_type}) icon: source='{space_entry['icon'].get('source', 'auto')}', key='{space_entry['icon'].get('key', '')}'")
    category_debug_print(f"[RESTORE GLYPH] Restored glyph data for '{category}'")
    return True


def update_category_tags_in_wm(category, space_type=-1):
    """Update the tags for a category in WM for UI display.

    Tags are stored in _glyph_cache and JSON for persistence.
    They are also synced to WM category_glyph_overrides for C++ UI display.

    Global-First: Always use GLOBAL space_type (-1) for override lookup to ensure
    consistency with _glyph_cache which stores all data under GLOBAL key.
    """
    try:
        wm = bpy.context.window_manager
        if wm is None or not hasattr(wm, 'category_glyph_overrides'):
            category_debug_print(f"[UPDATE_WM_TAGS] ERROR: WM or overrides not available for '{category}'")
            _pref_log_once(f"[DEBUG update_category_tags_in_wm] ERROR: WM or overrides not available")
            tag_log(f"update_category_tags_in_wm: WM or overrides not available", "ERROR")
            return

        category_debug_print(f"[UPDATE_WM_TAGS] START: category='{category}', space_type={space_type}")

        # DEBUG: Print _preview_mode_active state
        category_debug_print(f"[DEBUG UPDATE_WM_TAGS] category='{category}', _preview_mode_active={is_preview_mode_active()}")

        # Global-First: Always use GLOBAL space_type (-1) for cache lookup
        global_space_type = -1
        current_tags = get_category_tags(category, global_space_type)
        category_debug_print(f"[UPDATE_WM_TAGS] current_tags={current_tags} (from global_space_type={global_space_type})")
        _pref_log_once(f"[DEBUG update_category_tags_in_wm] category='{category}', space_type={space_type}, global_space_type={global_space_type}, current_tags={current_tags}")
        tag_log(f"update_category_tags_in_wm: category='{category}', space_type={space_type}, tags={current_tags}")

        # Debug: List all overrides for this category
        all_overrides_for_cat = []
        for item in wm.category_glyph_overrides:
            if item.category == category:
                item_st = getattr(item, 'space_type', -1)
                item_tags = getattr(item, 'tags', '')
                all_overrides_for_cat.append(f"(space_type={item_st}, tags='{item_tags}')")
        category_debug_print(f"[UPDATE_WM_TAGS] Existing WM overrides for '{category}': {all_overrides_for_cat}")

        # Global-First Migration: Remove all stale space-specific overrides for this category
        # This ensures only GLOBAL override exists, preventing C++ from finding empty space-specific first
        items_to_remove = []
        for item in wm.category_glyph_overrides:
            if item.category == category:
                item_st = getattr(item, 'space_type', -1)
                if item_st != global_space_type:  # Not GLOBAL (-1)
                    items_to_remove.append(item)
                    category_debug_print(f"[UPDATE_WM_TAGS] Marking stale space-specific override for removal: space_type={item_st}")

        for item in items_to_remove:
            wm.category_glyph_overrides.remove(item)
            category_debug_print(f"[UPDATE_WM_TAGS] Removed stale space-specific override (space_type={getattr(item, 'space_type', '?')})")

        override_item = None
        for item in wm.category_glyph_overrides:
            # Global-First: Always use GLOBAL space_type (-1) for override lookup
            # This ensures we find the override regardless of what space_type was passed in
            item_space_type = getattr(item, 'space_type', -1)
            if item.category == category and item_space_type == global_space_type:
                override_item = item
                category_debug_print(f"[UPDATE_WM_TAGS] Found existing GLOBAL override for '{category}' with tags='{getattr(item, 'tags', '')}'")
                _pref_log_once(f"[DEBUG update_category_tags_in_wm] Found existing override for '{category}' (space_type={global_space_type})")
                break

        if override_item is None:
            category_debug_print(f"[UPDATE_WM_TAGS] No GLOBAL override found for '{category}'")

            # Check if this is "Without Tag" preview mode - need to create override for C++ UI
            key = _make_cache_key(global_space_type, category)
            without_tag_preview = False
            if key in _glyph_cache:
                cat_data = _glyph_cache[key]
                if isinstance(cat_data, dict):
                    without_tag_preview = cat_data.get("without_tag_preview", False)

            # In preview mode, keep/create empty override even when there are no tags,
            # so C++ dialog does not fall back to stale mappings values.
            if is_preview_mode_active() and not current_tags and not without_tag_preview:
                override_item = wm.category_glyph_overrides.new(category=category)
                if hasattr(override_item, 'space_type'):
                    override_item.space_type = global_space_type
                if hasattr(override_item, 'tags'):
                    override_item.tags = ""
                category_debug_print(f"[UPDATE_WM_TAGS] Preview mode: created empty GLOBAL override for '{category}'")
                return

            # Only create a new override if there are tags OR "Without Tag" preview mode
            if not current_tags and not without_tag_preview:
                category_debug_print(f"[UPDATE_WM_TAGS] No override and no tags, skipping creation for '{category}'")
                _pref_log_once(f"[DEBUG update_category_tags_in_wm] No override and no tags, skipping creation for '{category}' (global_space_type={global_space_type})")
                tag_log(f"update_category_tags_in_wm: No override and no tags, skipping creation for '{category}' (global_space_type={global_space_type})")
                return

            override_item = wm.category_glyph_overrides.new(category=category)
            # Global-First: Always use GLOBAL space_type (-1) for consistency
            if hasattr(override_item, 'space_type'):
                override_item.space_type = global_space_type
            category_debug_print(f"[UPDATE_WM_TAGS] Created new GLOBAL override for '{category}'")
            _pref_log_once(f"[DEBUG update_category_tags_in_wm] Created new override for '{category}' (global_space_type={global_space_type})")
            tag_log(f"update_category_tags_in_wm: Created new override for '{category}' (global_space_type={global_space_type})")
        else:
            # Override exists - check if we need to clear it (no tags left)
            if not current_tags:
                # In preview mode we must keep an explicit empty override so C++ dialog
                # does not fall back to stale tags from mappings.
                if is_preview_mode_active():
                    if hasattr(override_item, 'tags'):
                        old_tags = override_item.tags
                        override_item.tags = ""
                        category_debug_print(
                            f"[UPDATE_WM_TAGS] Preview mode: kept empty override for '{category}' (old tags='{old_tags}')")
                    return

                # Check if this is "Without Tag" preview state in cache
                # If without_tag_preview=True, keep override with pending=True so category stays visible
                # in "New Add-ons!" until Save. This ensures consistent behavior with regular tags.
                key = _make_cache_key(global_space_type, category)
                without_tag_preview = False
                if key in _glyph_cache:
                    cat_data = _glyph_cache[key]
                    if isinstance(cat_data, dict):
                        without_tag_preview = cat_data.get("without_tag_preview", False)

                if without_tag_preview:
                    # "Without Tag" preview mode - keep override with pending=True for visibility
                    if hasattr(override_item, 'tags'):
                        override_item.tags = ""
                    if hasattr(override_item, 'pending_tag_assignment'):
                        override_item.pending_tag_assignment = True  # Keep True for "New Add-ons!" visibility
                    category_debug_print(f"[UPDATE_WM_TAGS] Keeping override for '{category}' (Without Tag preview: pending=True, tags empty)")
                    # Also update mappings to sync state
                    if hasattr(wm, 'category_glyph_mappings'):
                        for item in wm.category_glyph_mappings:
                            item_space_type = getattr(item, 'space_type', -1)
                            if item.category == category and item_space_type == global_space_type:
                                if hasattr(item, 'tags'):
                                    item.tags = ""
                                if hasattr(item, 'pending_tag_assignment'):
                                    item.pending_tag_assignment = True  # Keep True for visibility
                                category_debug_print(f"[UPDATE_WM_TAGS] Updated mapping for '{category}' (Without Tag preview)")
                                break
                    return

                # Not "Without Tag" preview - remove the override since there are no tags
                category_debug_print(f"[UPDATE_WM_TAGS] Removing GLOBAL override for '{category}' (no tags left)")
                _pref_log_once(f"[DEBUG update_category_tags_in_wm] Removing override for '{category}' (no tags left, global_space_type={global_space_type})")
                tag_log(f"update_category_tags_in_wm: Removing override for '{category}' (no tags left, global_space_type={global_space_type})")
                wm.category_glyph_overrides.remove(override_item)
                # CRITICAL: Also clear tags in category_glyph_mappings so C++ doesn't find stale tags
                # when override is removed. Without this, toggle appears broken because mappings
                # still contain old tag values.
                if hasattr(wm, 'category_glyph_mappings'):
                    for item in wm.category_glyph_mappings:
                        item_space_type = getattr(item, 'space_type', -1)
                        if item.category == category and item_space_type == global_space_type:
                            if hasattr(item, 'tags'):
                                old_tags = item.tags
                                item.tags = ""
                                category_debug_print(f"[UPDATE_WM_TAGS] Cleared mapping tags for '{category}' from '{old_tags}' to '' (override removed)")
                            break
                return

        if hasattr(override_item, 'tags'):
            old_tags = override_item.tags
            override_item.tags = ";".join(current_tags)
            category_debug_print(f"[UPDATE_WM_TAGS] Set WM override tags for '{category}' from '{old_tags}' to '{override_item.tags}'")
            _pref_log_once(f"[DEBUG update_category_tags_in_wm] Set WM override tags for '{category}' to '{override_item.tags}'")
            tag_log(f"update_category_tags_in_wm: Set WM override tags for '{category}' (global_space_type={global_space_type}) to '{override_item.tags}'")

        # CRITICAL: In preview mode, only update overrides, NOT mappings.
        # This keeps "New Add-ons!" button visible until user clicks Save.
        # Mappings are only updated when changes are actually saved.
        category_debug_print(f"[DEBUG UPDATE_WM_TAGS] Checking preview mode: _preview_mode_active={is_preview_mode_active()}")
        if is_preview_mode_active():
            category_debug_print(f"[DEBUG UPDATE_WM_TAGS] Preview mode: SKIPPING mappings update for '{category}'")
            category_debug_print(f"[UPDATE_WM_TAGS] Preview mode: skipping mappings update for '{category}'")
        elif hasattr(wm, 'category_glyph_mappings'):
            mapping_item = None
            for item in wm.category_glyph_mappings:
                item_space_type = getattr(item, 'space_type', -1)
                if item.category == category and item_space_type == global_space_type:
                    mapping_item = item
                    break

            if mapping_item is not None:
                # Update existing mapping
                if hasattr(mapping_item, 'tags'):
                    old_mapping_tags = mapping_item.tags
                    mapping_item.tags = ";".join(current_tags)
                    category_debug_print(f"[UPDATE_WM_TAGS] Updated mapping tags for '{category}' from '{old_mapping_tags}' to '{mapping_item.tags}'")
                # CRITICAL: Also sync pending_tag_assignment from cache to mappings
                # so C++ can correctly determine "New Add-ons!" visibility.
                key = _make_cache_key(global_space_type, category)
                if key in _glyph_cache:
                    cat_data = _glyph_cache[key]
                    if isinstance(cat_data, dict) and hasattr(mapping_item, 'pending_tag_assignment'):
                        pending_val = cat_data.get("pending_tag_assignment", False)
                        if mapping_item.pending_tag_assignment != pending_val:
                            mapping_item.pending_tag_assignment = pending_val
                            category_debug_print(f"[UPDATE_WM_TAGS] Updated mapping pending_tag_assignment for '{category}' to {pending_val}")
            elif current_tags:
                # Create new mapping only if there are tags to store
                mapping_item = wm.category_glyph_mappings.new(category=category)
                if hasattr(mapping_item, 'space_type'):
                    mapping_item.space_type = global_space_type
                if hasattr(mapping_item, 'tags'):
                    mapping_item.tags = ";".join(current_tags)
                category_debug_print(f"[UPDATE_WM_TAGS] Created new mapping for '{category}' with tags '{mapping_item.tags}'")

        category_debug_print(f"[UPDATE_WM_TAGS] COMPLETED for '{category}' (global_space_type={global_space_type})")
        _pref_log_once(f"[DEBUG update_category_tags_in_wm] Completed for '{category}' (global_space_type={global_space_type})")
        tag_log(f"update_category_tags_in_wm: Completed for '{category}' (global_space_type={global_space_type})")
    except Exception as e:
        category_debug_print(f"[UPDATE_WM_TAGS] EXCEPTION for '{category}': {e}")
        _pref_log_once(f"[DEBUG update_category_tags_in_wm] Error: {e}")
        tag_log(f"update_category_tags_in_wm: Error: {e}", "ERROR")
        import traceback
        traceback.print_exc()


def _set_without_tag_preview_state_in_wm(category, space_type=-1, is_selected=False):
    """Set preview-only visual state for the "Without Tag" button in edit dialog.

    Uses category_glyph_overrides only, so main category filtering (mappings/pending)
    remains unchanged until Save.

    Global-First: Always use GLOBAL space_type (-1) for consistency with tag storage.
    """
    try:
        wm = bpy.context.window_manager
        if wm is None or not hasattr(wm, 'category_glyph_overrides'):
            return

        # Global-First: Always use GLOBAL space_type (-1) for lookup
        global_space_type = -1
        override_item = None
        for item in wm.category_glyph_overrides:
            item_space_type = getattr(item, 'space_type', -1)
            if item.category == category and item_space_type == global_space_type:
                override_item = item
                break

        if override_item is None and is_selected:
            override_item = wm.category_glyph_overrides.new(category=category)
            if hasattr(override_item, 'space_type'):
                override_item.space_type = global_space_type

        if override_item is not None:
            # CRITICAL: Keep pending=True for "New Add-ons!" visibility.
            # The without_tag_preview flag in WM is used by C++ to determine button active state.
            if hasattr(override_item, 'pending_tag_assignment'):
                override_item.pending_tag_assignment = True

            # Set without_tag_preview flag for C++ UI button state
            if hasattr(override_item, 'without_tag_preview'):
                override_item.without_tag_preview = is_selected

            if is_selected and hasattr(override_item, 'tags'):
                override_item.tags = ""

        # CRITICAL: Update WM override to make the without_tag_preview state visible in C++ UI.
        # This must be called AFTER setting without_tag_preview flag above, otherwise
        # update_category_tags_in_wm won't create the override (see line 3448 condition).
        # IMPORTANT: Call this even if override_item is None to sync tags from Python cache to WM.
        category_debug_print(f"[DEBUG _set_without_tag] category='{category}', _preview_mode_active={is_preview_mode_active()}")
        update_category_tags_in_wm(category, space_type)
    except Exception:
        # Non-critical preview UI hint; ignore failures silently.
        pass


def _is_without_tag_preview_selected_in_wm(category, space_type=-1):
    """Return True when preview state for "Without Tag" is currently selected.

    Global-First: Always use GLOBAL space_type (-1) for consistency with tag storage.

    NOTE: This function checks the CACHE for without_tag_preview flag, not WM.
    The WM check was incorrect because we now keep pending=True for "New Add-ons!"
    visibility. The without_tag_preview flag in cache is the authoritative source.
    """
    global _glyph_cache
    try:
        # Global-First: Always use GLOBAL space_type (-1) for lookup
        key = _make_cache_key(-1, category)
        if key in _glyph_cache:
            cat_data = _glyph_cache[key]
            if isinstance(cat_data, dict):
                # Check without_tag_preview flag AND empty tags
                without_tag = cat_data.get("without_tag_preview", False)
                tags = cat_data.get("tags", [])
                return without_tag and not tags
    except Exception:
        pass

    return False


def finalize_category_tag_changes(category, space_type=-1, sync_wm=True):
    """Finalize tag changes when user clicks Save in edit dialog.

    This function:
    1. Exits preview mode to allow WM synchronization
    2. Updates WM with the new tags (makes changes visible in main UI)
    3. Clears pending_tag_assignment flag (category is no longer "new")
    4. Syncs changes to WM mappings for persistence (optional)
    5. Saves any new tags created during preview to JSON
    6. Resets tag_bar_manually_hidden if no more unassigned categories
    """
    import time
    finalize_start = time.perf_counter()
    category_debug_print(f"[FINALIZE] >>>>>> START finalize_category_tag_changes for '{category}' <<<<<<")

    global _glyph_cache

    # Ensure preview mode is disabled for finalization
    set_preview_mode_active(False)
    category_debug_print(f"[FINALIZE] Preview mode disabled for '{category}' (space_type={space_type})")

    # Get source_extension for this category to clear pending for all sibling categories
    source_ext = None
    key = _make_cache_key(space_type, category)
    if key in _glyph_cache:
        cat_data = _glyph_cache[key]
        if isinstance(cat_data, dict):
            source_ext = cat_data.get("source_extension", "")

    # Clear pending_tag_assignment in ALL cache entries for this category
    # (both space-specific and global) so the category is no longer "new"
    keys_to_clear = []
    for key in _glyph_cache:
        if isinstance(key, tuple) and len(key) == 2:
            key_st, key_cat = key
            if key_cat == category:
                keys_to_clear.append(key)

    for key in keys_to_clear:
        cat_data = _glyph_cache[key]
        if isinstance(cat_data, dict):
            if cat_data.get("pending_tag_assignment", False):
                cat_data["pending_tag_assignment"] = False
                tag_log(f"finalize_category_tag_changes: cleared pending for '{category}' (key={key})")

    # Also clear pending_tag_assignment for all categories from the same extension
    # When one category of an extension gets a tag, the whole extension is "processed"
    cleared_count = 0
    if source_ext:
        # Primary path: match by source_extension
        for cache_key, cat_data in _glyph_cache.items():
            if isinstance(cat_data, dict):
                if cat_data.get("source_extension") == source_ext:
                    if cat_data.get("pending_tag_assignment", False):
                        cat_data["pending_tag_assignment"] = False
                        cleared_count += 1
                        category_debug_print(f"[FINALIZE] Cleared pending for sibling category '{cache_key[1]}' (same extension: {source_ext})")
        if cleared_count > 0:
            tag_log(f"finalize_category_tag_changes: cleared pending for {cleared_count} sibling categories of extension '{source_ext}'")
    else:
        # FALLBACK: If source_extension is empty, find categories by name prefix matching
        # This handles cases like "Home Builder" (ext='') and "Home Builder 5" (ext='add-on-...')
        base_name = category.rstrip('0123456789').rstrip()  # Remove trailing numbers
        if not base_name:
            base_name = category

        for cache_key, cat_data in _glyph_cache.items():
            if not isinstance(cat_data, dict):
                continue
            other_name = cache_key[1] if isinstance(cache_key, tuple) else cache_key
            # Skip self
            if other_name == category:
                continue
            # Check if other category starts with base_name or base_name starts with other
            # This catches: "Home Builder" <-> "Home Builder 5", "MPFB" <-> "MPFB v2.0.14"
            other_ext = cat_data.get("source_extension", "")
            if other_ext and (other_name.startswith(base_name) or base_name.startswith(other_name)):
                if cat_data.get("pending_tag_assignment", False):
                    cat_data["pending_tag_assignment"] = False
                    cleared_count += 1
                    category_debug_print(f"[FINALIZE] Cleared pending for prefix-matched sibling: {other_name!r} (base={base_name!r}, extension={other_ext!r})")

        if cleared_count > 0:
            tag_log(f"finalize_category_tag_changes: cleared pending for {cleared_count} prefix-matched sibling categories (base={base_name!r})")

    # Update WM override to make changes visible
    wm_update_start = time.perf_counter()
    update_category_tags_in_wm(category, space_type)
    wm_update_end = time.perf_counter()
    category_debug_print(f"[FINALIZE] Step 1: update_category_tags_in_wm() completed in {(wm_update_end - wm_update_start)*1000:.2f}ms")

    # Save any new tags that were created during preview mode
    # This ensures tags created via "Create Tag" button are persisted
    tags_save_start = time.perf_counter()
    from bl_ui.glyph_tag_system.handlers import _auto_save_tags
    _auto_save_tags()
    tags_save_end = time.perf_counter()
    category_debug_print(f"[FINALIZE] Step 2: _auto_save_tags() completed in {(tags_save_end - tags_save_start)*1000:.2f}ms")

    # Sync to WM mappings for persistence (now that preview mode is off)
    if sync_wm:
        wm_sync_start = time.perf_counter()
        _auto_sync_to_wm()
        wm_sync_end = time.perf_counter()
        category_debug_print(f"[FINALIZE] Step 3: _auto_sync_to_wm() completed in {(wm_sync_end - wm_sync_start)*1000:.2f}ms")

    # Reset tag_bar_manually_hidden if no more unassigned categories remain.
    # This allows future auto-show to work if new unassigned categories appear.
    try:
        import bpy
        context = bpy.context
        if context and hasattr(context, 'space_data') and context.space_data:
            # Check if this was the last unassigned category
            from bl_ui.space_node import _get_unassigned_categories_count_for_node_editor
            unassigned_count = _get_unassigned_categories_count_for_node_editor(context)
            if unassigned_count == 0:
                snode = context.space_data
                if hasattr(snode, 'tag_bar_manually_hidden') and snode.tag_bar_manually_hidden:
                    snode.tag_bar_manually_hidden = False
                    category_debug_print(f"[FINALIZE] Reset tag_bar_manually_hidden - no more unassigned categories")
    except Exception as e:
        category_debug_print(f"[FINALIZE] Error checking unassigned count: {e}")

    finalize_end = time.perf_counter()
    category_debug_print(f"[FINALIZE] <<<<<<<< COMPLETE: Total time {(finalize_end - finalize_start)*1000:.2f}ms <<<<<<<<")

    tag_log(f"finalize_category_tag_changes: completed for '{category}' (space_type={space_type}, time: {(finalize_end - finalize_start)*1000:.2f}ms)")


# -----------------------------------------------------------------------------
# Sync + background sync functions (cache <-> WM bidirectional)
# -----------------------------------------------------------------------------


def sync_glyph_mappings_to_wm(force_discovery_merge=False, skip_icon_detection=False):
    """Sync in-memory glyph mappings to window manager collection.

    OPTIMIZATION: Added force_discovery_merge parameter to control when heavy discovery runs.
    OPTIMIZATION: Added skip_icon_detection parameter to defer icon detection to background.

    Note: The collections are cleared in C++ code before file save and after file load
    to prevent crashes from garbage pointers. This function only adds new items.
    """
    global _glyph_cache

    # Skip sync during preview mode to prevent premature category filtering
    if is_preview_mode_active():
        category_debug_print("[GLYPH SYNC] sync_glyph_mappings_to_wm: preview mode active, skipping sync")
        return False

    # Prevent recursive calls
    if is_sync_in_progress():
        category_debug_print("[GLYPH SYNC] sync_glyph_mappings_to_wm: sync already in progress, skipping")
        return False

    set_sync_in_progress(True)
    try:
        return _sync_glyph_mappings_to_wm_impl(force_discovery_merge=force_discovery_merge, skip_icon_detection=skip_icon_detection)
    finally:
        set_sync_in_progress(False)


def _background_discovery_sync():
    """Background periodic sync with discovery merge to keep categories updated.

    OPTIMIZATION: Runs discovery merge periodically in background instead of on every UI draw.
    First run uses shorter interval for quick icon detection, then switches to longer interval.
    """
    run_count = increment_background_sync_run_count()

    # Run discovery merge with force_refresh and icon detection enabled
    try:
        sync_glyph_mappings_to_wm(force_discovery_merge=True, skip_icon_detection=False)
        category_debug_print(f"[BACKGROUND SYNC] Completed discovery sync with icon detection (run #{run_count})")
    except Exception as e:
        category_debug_print(f"[BACKGROUND SYNC] Error: {e}")

    # Re-register timer for next run
    # First run: 5 seconds (quick icon detection)
    # Subsequent runs: 60 seconds (reduce CPU usage)
    set_background_sync_timer(_background_discovery_sync)
    next_interval = 5.0 if run_count == 1 else 60.0
    try:
        bpy.app.timers.register(get_background_sync_timer(), first_interval=next_interval)
        category_debug_print(f"[BACKGROUND SYNC] Next sync scheduled in {next_interval} seconds")
    except Exception:
        set_background_sync_timer(None)

    return None


def _start_background_sync():
    """Start background periodic sync timer."""
    if get_background_sync_timer() is not None:
        return  # Already running

    try:
        set_background_sync_timer(_background_discovery_sync)
        bpy.app.timers.register(get_background_sync_timer(), first_interval=5.0)
        category_debug_print("[BACKGROUND SYNC] Started background sync timer (5s initial, 10s interval)")
    except Exception as e:
        category_debug_print(f"[BACKGROUND SYNC] Failed to start: {e}")
        set_background_sync_timer(None)


def _stop_background_sync():
    """Stop background periodic sync timer."""
    if get_background_sync_timer() is not None:
        try:
            bpy.app.timers.unregister(get_background_sync_timer())
        except Exception:
            pass
        set_background_sync_timer(None)
        category_debug_print("[BACKGROUND SYNC] Stopped background sync timer")


def _auto_sync_to_wm():
    """Trigger an automatic sync to WM DNA structures with debouncing.

    Prevents UI stuttering when multiple categories are updated (e.g. discovery)
    by aggregating DNA clearing/rebuilding cycles into a single deferred call.
    """
    _auto_sync_timer = get_auto_sync_timer()

    # If not in preview mode and initial load is done, debounce the sync.
    # We use a short interval (100ms) to maintain responsiveness while preventing per-category stutter.
    if is_initial_load_complete() and not is_preview_mode_active():
        if _auto_sync_timer is not None:
            try:
                bpy.app.timers.unregister(_auto_sync_timer)
            except Exception:
                pass

        def delayed_sync():
            set_auto_sync_timer(None)
            # OPTIMIZATION: Don't force discovery merge in delayed syncs (UI updates)
            sync_glyph_mappings_to_wm(force_discovery_merge=False)
            return None

        set_auto_sync_timer(delayed_sync)
        bpy.app.timers.register(delayed_sync, first_interval=0.1)
    else:
        # Immediate sync if we're not debouncing (e.g. startup or preview)
        # OPTIMIZATION: Don't force discovery merge in immediate syncs (UI updates)
        sync_glyph_mappings_to_wm(force_discovery_merge=False)


def _sync_glyph_mappings_to_wm_impl(force_discovery_merge=False, skip_icon_detection=False):
    """Implementation of sync_glyph_mappings_to_wm.

    OPTIMIZATION: Added force_discovery_merge parameter to control when heavy discovery runs.
    OPTIMIZATION: Added skip_icon_detection parameter to defer icon detection to background.
    """
    global _glyph_cache

    cache_changed = False  # Track if cache was modified during sync

    # PERF: Start timing
    _sync_start_time = time.perf_counter()

    _pref_log_once(f"[GLYPH SYNC] sync_glyph_mappings_to_wm called, cache has {len(_glyph_cache)} entries, force_discovery={force_discovery_merge}, skip_icon={skip_icon_detection}")

    # OPTIMIZATION: Only run heavy discovery merge when explicitly requested
    # This prevents UI stuttering during panel draws (Get Extensions, Add-ons)
    if force_discovery_merge:
        _pref_log_once(f"[GLYPH SYNC] Running forced discovery merge")
        try:
            from bl_ui.glyph_tag_system.discovery import _merge_discovered_categories
            discovered_changes = _merge_discovered_categories(force_refresh=True, skip_icon_detection=skip_icon_detection)
            _pref_log_once(f"[GLYPH SYNC] late discovery merge result: {discovered_changes}")
        except Exception as e:
            _pref_log_once(f"[GLYPH SYNC] late discovery merge failed: {e}")
    else:
        # Skip discovery merge for regular syncs (UI draws)
        # Discovery will run periodically via timer or when explicitly requested
        _pref_log_once(f"[GLYPH SYNC] Skipping discovery merge (not forced)")

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

    try:
        wm = bpy.context.window_manager
        if wm is None or not hasattr(wm, 'category_glyph_mappings'):
            category_debug_print("[GLYPH] WindowManager or collections not available")
            return False

        # Clear existing mappings to avoid duplicates.
        # IMPORTANT: use RNA clear() (C-side) to handle potentially corrupted ListBase
        # safely after loading older/foreign blend files.
        wm.category_glyph_mappings.clear()
        _pref_log_once("[GLYPH SYNC] Cleared existing mappings")

        # Sync available tags (definitions) into wm.category_tags if available
        if hasattr(wm, "category_tags"):
            # Same safety rationale as above: do not iterate Python-side over a potentially
            # corrupted RNA collection right after file load.
            wm.category_tags.clear()
            category_debug_print("[GLYPH SYNC] Cleared existing tag definitions")

            # Use tag_order if available, otherwise use cache insertion order (added to end)
            if _tag_order_cache:
                # Use saved order, but only include tags that still exist
                tag_names = [t for t in _tag_order_cache if t in _all_tags_cache]
                # Add any new tags not in order list (at the end)
                new_tags = [t for t in _all_tags_cache.keys() if t not in _tag_order_cache]
                tag_names.extend(new_tags)
            else:
                # Default: insertion order (added to end)
                tag_names = list(_all_tags_cache.keys())

            category_debug_print(f"[TAGS SYNC] Starting tag sync: _all_tags_cache has {len(_all_tags_cache)} entries: {list(_all_tags_cache.keys())}")
            category_debug_print(f"[TAGS SYNC] tag_names to sync: {tag_names}")

            for tag_name in tag_names:
                category_debug_print(f"[TAGS SYNC] Processing tag '{tag_name}' from _all_tags_cache")
                tag_data = _all_tags_cache.get(tag_name)
                if tag_data is None:
                    category_debug_print(f"[TAGS SYNC] WARNING: Tag '{tag_name}' not found in _all_tags_cache!")
                    continue
                glyph_hex = _glyph_to_hex(tag_data.get("glyph", "")) if isinstance(tag_data, dict) else ""
                color_val = tag_data.get("color", [0.0, 0.0, 0.0]) if isinstance(tag_data, dict) else [0.0, 0.0, 0.0]
                # Icon fields
                icon_key_val = tag_data.get("icon_key", "") if isinstance(tag_data, dict) else ""
                icon_source_val = tag_data.get("icon_source", 0) if isinstance(tag_data, dict) else 0
                # Handle both int and string formats for backward compatibility
                if isinstance(icon_source_val, str):
                    icon_source_map = {'GLYPH': 0, 'BLENDER_ICON': 1, 'CUSTOM': 2}
                    icon_source_val = icon_source_map.get(icon_source_val, 0)
                else:
                    icon_source_val = int(icon_source_val) if icon_source_val is not None else 0

                category_debug_print(f"[DEBUG PY] Creating tag '{tag_name}' with glyph='{glyph_hex}' icon_key='{icon_key_val}' icon_source={icon_source_val}")
                tag_item = wm.category_tags.new(name=tag_name)
                tag_item.glyph = glyph_hex
                tag_item.color = (color_val[0], color_val[1], color_val[2])
                # Sync mode flags
                mode_flags_val = tag_data.get("mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS) if isinstance(tag_data, dict) else _CATEGORY_TAG_DEFAULT_MODE_FLAGS
                tag_item.mode_flags = mode_flags_val
                # Sync icon fields
                has_icon_key_attr = hasattr(tag_item, "icon_key")
                has_icon_source_attr = hasattr(tag_item, "icon_source")
                category_debug_print(f"[DEBUG PY] tag_item '{tag_name}' has icon_key attr: {has_icon_key_attr}, has icon_source attr: {has_icon_source_attr}")

                if has_icon_key_attr:
                    tag_item.icon_key = icon_key_val
                if has_icon_source_attr:
                    tag_item.icon_source = icon_source_val
                # DEBUG: Verify values were set
                category_debug_print(f"[DEBUG PY] Set tag '{tag_name}' icon_key='{icon_key_val}' icon_source={icon_source_val} -> tag_item.icon_key='{tag_item.icon_key if has_icon_key_attr else 'NO_ATTR'}' tag_item.icon_source={tag_item.icon_source if has_icon_source_attr else 'NO_ATTR'}")

                # Force notification to update Tag Bar - direct assignment may not trigger PROP_CONTEXT_UPDATE
                try:
                    wm = bpy.context.window_manager
                    if hasattr(wm, 'tag_update'):
                        # Try to trigger an update by re-assigning
                        pass
                except:
                    pass
            _pref_log_once(f"[GLYPH SYNC] Synced {len(wm.category_tags)} tag definitions to WM")

        # Add current mappings from cache
        # Cache keys are tuples: (space_type, category_name)
        added_count = 0
        skipped_invalid = 0
        try:
            for cache_key, category_data in _glyph_cache.items():
                try:
                    # Unpack tuple key: (space_type, category_name)
                    if isinstance(cache_key, tuple) and len(cache_key) == 2:
                        space_type_val, category = cache_key
                    else:
                        # Fallback for old string keys (shouldn't happen with new cache)
                        category = cache_key
                        space_type_val = -1

                    # Skip invalid category names ONLY if they have no user customizations
                    if not _is_valid_category_name(category):
                        if not _has_user_customizations(category_data):
                            skipped_invalid += 1
                            continue

                    # Normalize data to ensure it has all required fields (glyph, display_name, color, tags, extension fields).
                    # Also migrating legacy string entries in _glyph_cache to dicts.
                    if isinstance(category_data, (str, dict)):
                        normalized_data = _normalize_category_data(category_data, category)

                        # Store normalized version back to cache if data was old format or missing fields.
                        # This allows detected icon paths and inherited extension info to be persisted back to JSON.
                        is_legacy_string = isinstance(category_data, str)
                        is_missing_fields = isinstance(category_data, dict) and any(field not in category_data for field in normalized_data)

                        if is_legacy_string or is_missing_fields:
                            _glyph_cache[cache_key] = normalized_data
                            cache_changed = True
                    else:
                        continue

                    glyph_val = normalized_data.get("glyph", "")
                    display_name_val = normalized_data.get("display_name", "")
                    color_val = normalized_data.get("color", [0.0, 0.0, 0.0])
                    default_glyph_val = normalized_data.get("default_glyph", glyph_val)
                    default_display_name_val = normalized_data.get("default_display_name", "")
                    tags_val = normalized_data.get("tags", [])
                    icon_source_str = str(normalized_data.get("icon_source", "auto")).lower()
                    glyph_mode_str = str(normalized_data.get("glyph_mode", "auto")).lower()
                    icon_key_val = str(normalized_data.get("icon_key", ""))
                    icon_path_val = str(normalized_data.get("icon_path", ""))
                    icon_provider_val = str(normalized_data.get("icon_provider", ""))

                    # Debug: log all cache entries being processed (use _log_once to avoid flooding)
                    _pref_log_once(
                        f"[GLYPH SYNC] Processing cache_key={cache_key!r}, "
                        f"category={category!r}, icon_source={icon_source_str!r}, "
                        f"icon_key={icon_key_val!r}, icon_path={icon_path_val!r}"
                    )

                    if icon_source_str == "auto" and (not icon_key_val) and (not icon_path_val):
                        # OPTIMIZATION: Skip icon detection during startup (will run in background)
                        if not skip_icon_detection:
                            _pref_log_once(
                                f"[] sync attempt: category={category!r}, "
                                f"icon_source={icon_source_str!r}"
                            )
                            from bl_ui.glyph_tag_system.discovery import _auto_detect_extension_icon_path
                            detected_icon_path, detected_provider = _auto_detect_extension_icon_path(category)
                            if detected_icon_path:
                                icon_path_val = detected_icon_path
                                if not icon_provider_val:
                                    icon_provider_val = detected_provider
                                _pref_log_once(
                                    f"[] sync hit: category={category!r}, "
                                    f"path={icon_path_val!r}, provider={icon_provider_val!r}"
                                )
                                # Update cache with detected icon path so it persists
                                if isinstance(normalized_data, dict):
                                    normalized_data["icon_path"] = icon_path_val
                                    normalized_data["icon_provider"] = icon_provider_val
                                    cache_changed = True
                            else:
                                _sync_miss_log_once(category)
                        else:
                            category_debug_print(f"[ICON DETECTION] Skipping icon detection for {category!r} during sync (will run in background)")

                    icon_source_to_enum = {
                        "auto": "AUTO",
                        "manual": "MANUAL",
                        "off": "OFF",
                    }
                    icon_source_val = icon_source_to_enum.get(icon_source_str, "AUTO")

                    glyph_mode_to_enum = {
                        "auto": "AUTO",
                        "first_letter": "FIRST_LETTER",
                    }
                    glyph_mode_val = glyph_mode_to_enum.get(glyph_mode_str, "AUTO")

                    item = wm.category_glyph_mappings.new(category=category)
                    # Set space_type for space-specific category lookup
                    if hasattr(item, "space_type"):
                        item.space_type = -1
                    item.glyph = glyph_val
                    item.display_name = display_name_val
                    item.color = (color_val[0], color_val[1], color_val[2])
                    item.default_glyph = default_glyph_val
                    item.default_display_name = default_display_name_val
                    if hasattr(item, "icon_source"):
                        item.icon_source = icon_source_val
                    if hasattr(item, "glyph_mode"):
                        item.glyph_mode = glyph_mode_val
                    if hasattr(item, "icon_key"):
                        item.icon_key = icon_key_val
                    if hasattr(item, "icon_path"):
                        item.icon_path = icon_path_val
                    if hasattr(item, "icon_provider"):
                        item.icon_provider = icon_provider_val
                    # Mark as reserved if category is in DEFAULT_CATEGORY_GLYPHS
                    if hasattr(item, "is_reserved"):
                        item.is_reserved = category in DEFAULT_CATEGORY_GLYPHS
                    # Sync tags to WM for UI display (semicolon-separated string)
                    if hasattr(item, "tags") and isinstance(tags_val, (list, tuple)):
                        tags_str = ";".join([t for t in tags_val if isinstance(t, str) and t])
                        item.tags = tags_str
                        # Debug: log categories with tags
                        if tags_str:
                            _pref_log_once(f"[GLYPH SYNC] Synced tags for category={category!r} (GLOBAL)")
                    # Sync extension pending-tag fields
                    source_ext_val = normalized_data.get("source_extension", "")

                    pending_val = bool(normalized_data.get("pending_tag_assignment", False))
                    without_tag_preview = bool(normalized_data.get("without_tag_preview", False))
                    disc_spaces_val = normalized_data.get("discovered_in_spaces", [])
                    disc_modes_val = normalized_data.get("discovered_in_modes", [])

                    # Handle "Without Tag" preview finalization on Save:
                    # If without_tag_preview=True, this means user selected "Without Tag" in preview
                    # and now clicked Save. Set pending=False to finalize the choice.
                    if without_tag_preview:
                        pending_val = False
                        normalized_data["pending_tag_assignment"] = False
                        normalized_data["without_tag_preview"] = False  # Clear preview flag
                        _glyph_cache[cache_key] = normalized_data
                        cache_changed = True
                        category_debug_print(f"[GLYPH SYNC] Finalized 'Without Tag' for {category!r}: pending=False")

                        # CRITICAL: Clear pending_tag_assignment for ALL sibling categories from the same extension
                        # This ensures that when a user selects "Without Tag" for any category from an extension,
                        # all other categories from that extension disappear from "New Add-ons!" filter
                        source_ext = source_ext_val
                        cleared_count = 0
                        if source_ext:
                            # Primary path: match by source_extension
                            for other_key, other_data in _glyph_cache.items():
                                if isinstance(other_data, dict) and other_data.get("source_extension") == source_ext:
                                    other_name = other_key[1] if isinstance(other_key, tuple) else other_key
                                    if other_name != category:  # Skip self
                                        if other_data.get("pending_tag_assignment", False):
                                            other_data["pending_tag_assignment"] = False
                                            cleared_count += 1
                                            category_debug_print(f"[GLYPH SYNC] Cleared pending for sibling (Without Tag): {other_name!r} (extension={source_ext!r})")
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
                                    if other_data.get("pending_tag_assignment", False):
                                        other_data["pending_tag_assignment"] = False
                                        cleared_count += 1
                                        category_debug_print(f"[GLYPH SYNC] Cleared pending for prefix-matched sibling (Without Tag): {other_name!r} (base={base_name!r})")

                        if cleared_count > 0:
                            category_debug_print(f"[GLYPH SYNC] Total siblings cleared for 'Without Tag': {cleared_count}")

                    # NORMALIZE pending: clear if category has tags or no source_extension
                    # This prevents stale pending flags from showing "New Add-ons!" button
                    # when there's nothing to distribute in the current context.
                    # Also clear for RESERVED categories - they should never be linked to extensions.
                    # Global-First: All entries are GLOBAL, so no need to check other space types.
                    is_reserved = category in DEFAULT_CATEGORY_GLYPHS
                    has_tags = bool(tags_val) if isinstance(tags_val, (list, tuple)) else bool(tags_str)

                    # Reserved categories should not surface as pending, but keep their
                    # source_extension so the extension can still be recognized as already
                    # distributed when its only visible panels live in reserved tabs.
                    if is_reserved and (pending_val or source_ext_val):
                        pending_val = False
                        normalized_data["pending_tag_assignment"] = False
                        # CRITICAL: Update cache so it gets saved to JSON.
                        _glyph_cache[cache_key] = normalized_data
                        cache_changed = True
                        category_debug_print(f"[GLYPH SYNC NORMALIZE] Cleared pending for RESERVED category {category!r} while keeping source_extension")

                        # CRITICAL: Clear pending_tag_assignment for ALL sibling categories from the same extension
                        cleared_count = 0
                        if source_ext_val:
                            for other_key, other_data in _glyph_cache.items():
                                if isinstance(other_data, dict) and other_data.get("source_extension") == source_ext_val:
                                    other_name = other_key[1] if isinstance(other_key, tuple) else other_key
                                    if other_name != category:  # Skip self
                                        if other_data.get("pending_tag_assignment", False):
                                            other_data["pending_tag_assignment"] = False
                                            cleared_count += 1
                                            category_debug_print(f"[GLYPH SYNC NORMALIZE] Cleared pending for sibling (RESERVED): {other_name!r} (extension={source_ext_val!r})")

                        if cleared_count > 0:
                            category_debug_print(f"[GLYPH SYNC NORMALIZE] Total siblings cleared for RESERVED: {cleared_count}")
                    elif pending_val and (has_tags or not source_ext_val):
                        pending_val = False
                        normalized_data["pending_tag_assignment"] = False
                        # CRITICAL: Update cache so it gets saved to JSON.
                        _glyph_cache[cache_key] = normalized_data
                        cache_changed = True
                        category_debug_print(f"[GLYPH SYNC NORMALIZE] Cleared stale pending for {category!r}: "
                              f"has_tags={has_tags}, source_ext={source_ext_val!r}")

                        # CRITICAL: Clear pending_tag_assignment for ALL sibling categories from the same extension
                        # This ensures that when a category gets tags (or has no extension), all siblings are cleared too
                        cleared_count = 0
                        if source_ext_val:
                            # Primary path: match by source_extension
                            for other_key, other_data in _glyph_cache.items():
                                if isinstance(other_data, dict) and other_data.get("source_extension") == source_ext_val:
                                    other_name = other_key[1] if isinstance(other_key, tuple) else other_key
                                    if other_name != category:  # Skip self
                                        if other_data.get("pending_tag_assignment", False):
                                            other_data["pending_tag_assignment"] = False
                                            cleared_count += 1
                                            category_debug_print(f"[GLYPH SYNC NORMALIZE] Cleared pending for sibling: {other_name!r} (extension={source_ext_val!r})")
                        else:
                            # FALLBACK: If source_extension is empty, find categories by name prefix matching
                            base_name = category.rstrip('0123456789').rstrip()
                            if not base_name:
                                base_name = category

                            for other_key, other_data in _glyph_cache.items():
                                if not isinstance(other_data, dict):
                                    continue
                                other_name = other_key[1] if isinstance(other_key, tuple) else other_key
                                if other_name == category:
                                    continue
                                other_ext = other_data.get("source_extension", "")
                                if other_ext and (other_name.startswith(base_name) or base_name.startswith(other_name)):
                                    if other_data.get("pending_tag_assignment", False):
                                        other_data["pending_tag_assignment"] = False
                                        cleared_count += 1
                                        category_debug_print(f"[GLYPH SYNC NORMALIZE] Cleared pending for prefix-matched sibling: {other_name!r} (base={base_name!r})")

                        if cleared_count > 0:
                            category_debug_print(f"[GLYPH SYNC NORMALIZE] Total siblings cleared: {cleared_count}")

                    # Debug: log ALL categories with pending_tag_assignment or source_extension
                    if pending_val or source_ext_val:
                        disc_spaces_flags = spaces_to_flags(disc_spaces_val)
                        category_debug_print(f"[GLYPH SYNC] >>> PENDING CATEGORY: {category!r}, pending={pending_val}, "
                              f"source_ext={source_ext_val!r}, disc_spaces={disc_spaces_val} -> flags={disc_spaces_flags}")
                        category_debug_print(f"[GLYPH SYNC] >>> Setting item.pending_tag_assignment={pending_val} for {category!r}")
                    if hasattr(item, "source_extension"):
                        item.source_extension = source_ext_val
                    if hasattr(item, "pending_tag_assignment"):
                        item.pending_tag_assignment = pending_val
                        category_debug_print(f"[GLYPH SYNC] >>> WM item.pending_tag_assignment set to {item.pending_tag_assignment} for {category!r}")
                    if hasattr(item, "discovered_in_spaces"):
                        item.discovered_in_spaces = spaces_to_flags(disc_spaces_val)
                    if hasattr(item, "discovered_in_modes"):
                        item.discovered_in_modes = modes_to_flags(disc_modes_val)
                    # Sync install_mode_flag for mode-aware filtering
                    if hasattr(item, "install_mode_flag"):
                        item.install_mode_flag = normalized_data.get("install_mode_flag", 0)
                    added_count += 1

                    # Debug: show what was synced for key categories
                    if category in ["Item", "View", "Tool", "Edit"]:
                        _pref_log_once(f"[GLYPH SYNC] Synced '{category}' (GLOBAL): glyph='{glyph_val}'")
                except Exception as e:
                    category_debug_print(f"[GLYPH] Error adding mapping for {cache_key}: {e}")

        except Exception as e:
            category_debug_print(f"[GLYPH] Critical error during mapping addition: {e}")
            import traceback
            traceback.print_exc()
            return False

        if skipped_invalid > 0:
            category_debug_print(f"[GLYPH] Skipped {skipped_invalid} categories with invalid names and no customizations")
        category_debug_print(f"[GLYPH] Successfully synced {added_count}/{len(_glyph_cache)} mappings to WM")

        # Save cache to file if any icon paths were updated during sync
        if cache_changed:
            if _save_glyph_mappings_to_file(force_discovery_skip=True):
                category_debug_print("[GLYPH SYNC] Saved updated icon paths to JSON file")
            else:
                category_debug_print("[GLYPH SYNC] Failed to save updated icon paths to JSON file")

        # PERF: Log timing
        _sync_elapsed = (time.perf_counter() - _sync_start_time) * 1000
        category_debug_print(f"[PERF] _sync_glyph_mappings_to_wm_impl completed in {_sync_elapsed:.2f}ms")

        return added_count > 0
    except Exception as e:
        category_debug_print(f"[GLYPH] Error syncing to WM: {e}")
        import traceback
        traceback.print_exc()
        return False


def register_category_glyph_mappings():
    """Register glyph mappings. Loads from file, discovers addon categories, and syncs to WM."""

    # Reset flag at start of registration
    set_initial_load_complete(False)

    if not is_glyph_cache_loaded():
        _load_glyph_mappings_from_file()

    # Integrate with glyph library if available
    _integrate_glyph_library()

    # OPTIMIZATION: REMOVED duplicate _merge_discovered_categories() call
    # sync_glyph_mappings_to_wm(force_discovery_merge=True) below already calls it
    # This was causing DOUBLE discovery (2x 790 panel scans) at startup!
    category_debug_print("[GLYPH] Skipping duplicate discovery - will be done by sync_glyph_mappings_to_wm")

    # OPTIMIZATION: Force discovery merge on initial load to ensure all categories are found
    # OPTIMIZATION: Skip icon detection during startup for faster load (will run in background)
    result = sync_glyph_mappings_to_wm(force_discovery_merge=True, skip_icon_detection=True)

    # Mark initial load as complete - callbacks can now save
    set_initial_load_complete(True)
    category_debug_print("[GLYPH] Initial load complete, auto-save callbacks enabled")

    # OPTIMIZATION: Start background sync with delay to handle icon detection
    # This allows startup to be fast, then icons are detected in the background
    def start_background_sync_delayed():
        category_debug_print("[BACKGROUND SYNC] Starting delayed background sync for icon detection")
        _start_background_sync()
        return None

    # Start background sync after 2 seconds to allow UI to settle
    try:
        bpy.app.timers.register(start_background_sync_delayed, first_interval=2.0)
        category_debug_print("[BACKGROUND SYNC] Scheduled to start in 2 seconds")
    except Exception as e:
        category_debug_print(f"[BACKGROUND SYNC] Failed to schedule delayed start: {e}")

    return result


def sync_wm_to_glyph_cache():
    """Sync glyph mappings from window manager collection back to cache and JSON.

    This function reads user changes from category_glyph_overrides and
    category_glyph_mappings in WM and saves them to the JSON file.
    """
    global _glyph_cache, _all_tags_cache

    # Don't save during initial load
    if not is_initial_load_complete():
        category_debug_print("[GLYPH SYNC] sync_wm_to_glyph_cache: initial load not complete, skipping save")
        return False

    # Prevent recursive calls
    if is_sync_in_progress():
        category_debug_print("[GLYPH SYNC] sync_wm_to_glyph_cache: sync already in progress, skipping")
        return False

    set_sync_in_progress(True)
    try:
        return _sync_wm_to_glyph_cache_impl()
    finally:
        set_sync_in_progress(False)


def _sync_wm_to_glyph_cache_impl():
    """Implementation of sync_wm_to_glyph_cache."""
    global _glyph_cache, _all_tags_cache

    try:
        wm = bpy.context.window_manager
        if wm is None or not hasattr(wm, 'category_glyph_mappings'):
            category_debug_print("[GLYPH] Cannot sync from WM: WindowManager not available")
            return False

        # Check if collections are safe to access
        if not _is_collection_safe(wm.category_glyph_mappings):
            category_debug_print("[GLYPH] Cannot sync from WM: collections not safe")
            return False

        changes_detected = False
        category_debug_print("[GLYPH] Starting sync from WM to cache...")

        # Sync from category_glyph_mappings (default mappings)
        try:
            mappings_count = 0
            for item in wm.category_glyph_mappings:
                category = item.category
                if not category or category == "__test__":
                    continue

                mappings_count += 1

                # Global-First: ignore any non-GLOBAL WM entries.
                space_type = getattr(item, 'space_type', -1)
                if space_type != -1:
                    continue
                cache_key = _make_cache_key(-1, category)

                # Get current cached data or create new entry
                if cache_key not in _glyph_cache:
                    _glyph_cache[cache_key] = {
                        "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                        "default_glyph": "", "default_display_name": "", "base_type": "text_only",
                        "glyph_mode": "auto",
                        "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                    }

                cached_entry = _glyph_cache[cache_key]

                # Check if any values changed
                if cached_entry.get("glyph", "") != item.glyph:
                    cached_entry["glyph"] = item.glyph
                    # Update default_glyph only for non-glyph_only categories
                    # For glyph_only, default_glyph must stay as category name (original glyph)
                    if item.glyph:
                        is_glyph_only = _is_single_glyph(category)
                        if not is_glyph_only:
                            cached_entry["default_glyph"] = item.glyph
                        # Determine base_type based on glyph content
                        if _is_single_glyph(item.glyph):
                            cached_entry["base_type"] = "glyph_only"
                        else:
                            cached_entry["base_type"] = "glyph_text"
                    changes_detected = True

                if cached_entry.get("display_name", "") != item.display_name:
                    cached_entry["display_name"] = item.display_name
                    changes_detected = True

                cached_color = cached_entry.get("color", [0.0, 0.0, 0.0])
                item_color = list(item.color[:3])
                if cached_color != item_color:
                    cached_entry["color"] = item_color
                    changes_detected = True

                # Tags are NOT synced from WM to cache to avoid truncation issues
                # Tags are stored only in _glyph_cache and JSON

                # Also sync default values.
                # Must sync even when empty string to allow clearing stale values.
                if hasattr(item, 'default_glyph'):
                    item_default_glyph = item.default_glyph or ""
                    if cached_entry.get("default_glyph", "") != item_default_glyph:
                        cached_entry["default_glyph"] = item_default_glyph
                        changes_detected = True

                if hasattr(item, 'default_display_name'):
                    item_default_display_name = item.default_display_name or ""
                    # Only sync FROM WM if cache doesn't already have a valid value.
                    # WM items may have empty/stale default_display_name because the C++
                    # commit path does not set it on override items. The Python cache
                    # (loaded from JSON) is the authoritative source for default_display_name.
                    if not cached_entry.get("default_display_name") and item_default_display_name:
                        cached_entry["default_display_name"] = item_default_display_name
                        changes_detected = True

                if hasattr(item, 'icon_source'):
                    icon_source_raw = getattr(item, "icon_source", "AUTO")
                    if isinstance(icon_source_raw, str):
                        icon_source_val = icon_source_raw.lower()
                        if icon_source_val not in {"auto", "manual", "off"}:
                            icon_source_val = "auto"
                    else:
                        icon_source_from_int = {
                            0: "auto",
                            1: "manual",
                            2: "off",
                        }
                        icon_source_val = icon_source_from_int.get(int(icon_source_raw), "auto")
                    if cached_entry.get("icon_source", "auto") != icon_source_val:
                        cached_entry["icon_source"] = icon_source_val
                        changes_detected = True

                if hasattr(item, 'glyph_mode'):
                    glyph_mode_raw = getattr(item, "glyph_mode", "AUTO")
                    if isinstance(glyph_mode_raw, str):
                        glyph_mode_val = glyph_mode_raw.lower()
                        if glyph_mode_val not in {"auto", "first_letter"}:
                            glyph_mode_val = "auto"
                    else:
                        glyph_mode_from_int = {0: "auto", 1: "first_letter"}
                        glyph_mode_val = glyph_mode_from_int.get(int(glyph_mode_raw), "auto")
                    if cached_entry.get("glyph_mode", "auto") != glyph_mode_val:
                        cached_entry["glyph_mode"] = glyph_mode_val
                        changes_detected = True

                if hasattr(item, 'icon_key'):
                    icon_key_val = item.icon_key or ""
                    if cached_entry.get("icon_key", "") != icon_key_val:
                        cached_entry["icon_key"] = icon_key_val
                        changes_detected = True

                if hasattr(item, 'icon_path'):
                    icon_path_val = item.icon_path or ""
                    if cached_entry.get("icon_path", "") != icon_path_val:
                        cached_entry["icon_path"] = icon_path_val
                        changes_detected = True

                if hasattr(item, 'icon_provider'):
                    icon_provider_val = item.icon_provider or ""
                    if cached_entry.get("icon_provider", "") != icon_provider_val:
                        cached_entry["icon_provider"] = icon_provider_val
                        changes_detected = True

                # Sync extension-related fields for "New Add-ons!" feature
                # These are critical for showing unassigned categories
                if hasattr(item, 'source_extension'):
                    source_ext_val = item.source_extension or ""
                    if cached_entry.get("source_extension", "") != source_ext_val:
                        cached_entry["source_extension"] = source_ext_val
                        changes_detected = True

                if hasattr(item, 'pending_tag_assignment'):
                    pending_val = bool(item.pending_tag_assignment)
                    if cached_entry.get("pending_tag_assignment", False) != pending_val:
                        cached_entry["pending_tag_assignment"] = pending_val
                        changes_detected = True

                if hasattr(item, 'discovered_in_spaces'):
                    # WM stores as int (flags), cache stores as list of strings
                    disc_spaces_flags = getattr(item, 'discovered_in_spaces', 0)
                    if isinstance(disc_spaces_flags, int):
                        disc_spaces = flags_to_spaces(disc_spaces_flags)
                    else:
                        disc_spaces = list(disc_spaces_flags) if disc_spaces_flags else []
                    if cached_entry.get("discovered_in_spaces", []) != disc_spaces:
                        cached_entry["discovered_in_spaces"] = disc_spaces
                        changes_detected = True

                if hasattr(item, 'discovered_in_modes'):
                    # WM stores as int (flags), cache stores as list of strings
                    disc_modes_flags = getattr(item, 'discovered_in_modes', 0)
                    if isinstance(disc_modes_flags, int):
                        disc_modes = flags_to_modes(disc_modes_flags)
                    else:
                        disc_modes = list(disc_modes_flags) if disc_modes_flags else []
                    if cached_entry.get("discovered_in_modes", []) != disc_modes:
                        cached_entry["discovered_in_modes"] = disc_modes
                        changes_detected = True

                # If this is a space-specific entry with customizations, also update GLOBAL entry
                # This ensures glyph, glyph_mode (first_letter), and icon persist across all spaces
                item_glyph = item.glyph or ""
                has_customizations = (
                    (icon_source_val in ("manual", "off") or icon_key_val or icon_path_val) or
                    (glyph_mode_val != "auto") or
                    (item_glyph and item_glyph != cached_entry.get("default_glyph", ""))
                )
                if space_type != -1 and has_customizations:
                    global_cache_key = _make_cache_key(-1, category)
                    if global_cache_key not in _glyph_cache:
                        _glyph_cache[global_cache_key] = {
                            "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                            "default_glyph": "", "default_display_name": "", "base_type": "text_only",
                            "glyph_mode": "auto",
                            "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                        }

                    global_entry = _glyph_cache[global_cache_key]
                    # Copy glyph data to global entry
                    if item_glyph and global_entry.get("glyph", "") != item_glyph:
                        global_entry["glyph"] = item_glyph
                        changes_detected = True
                        _pref_log_once(f"[GLYPH SYNC] Updated global glyph for '{category}' to '{item_glyph}' from mappings")

                    # Determine if we should update global entry based on icon_source priority
                    # Priority: 'manual' > 'off' > 'auto'
                    # - 'manual' (user explicitly chose an icon) has HIGHEST priority
                    # - 'off' (user chose first_letter mode) wins over 'auto'
                    # - 'auto' is the default and gets overwritten by anything
                    global_icon_source = global_entry.get("icon_source", "auto")
                    should_update = (
                        global_icon_source == "auto" or
                        icon_source_val == "manual" or
                        icon_source_val == "off"
                    )

                    # Copy glyph_mode to global entry only when allowed to update
                    if should_update and global_entry.get("glyph_mode", "auto") != glyph_mode_val:
                        global_entry["glyph_mode"] = glyph_mode_val
                        changes_detected = True
                        _pref_log_once(f"[GLYPH SYNC] Updated global glyph_mode for '{category}' to '{glyph_mode_val}' from mappings")

                    # Copy icon data to global entry
                    if should_update:
                        if global_entry.get("icon_source", "auto") != icon_source_val:
                            global_entry["icon_source"] = icon_source_val
                            changes_detected = True
                            _pref_log_once(f"[GLYPH SYNC] Updated global icon_source for '{category}' from mappings")
                        if global_entry.get("icon_key", "") != icon_key_val:
                            global_entry["icon_key"] = icon_key_val
                            changes_detected = True
                            _pref_log_once(f"[GLYPH SYNC] Updated global icon_key for '{category}' to '{icon_key_val}' from mappings")
                        if global_entry.get("icon_path", "") != icon_path_val:
                            global_entry["icon_path"] = icon_path_val
                            changes_detected = True
                        if global_entry.get("icon_provider", "") != icon_provider_val:
                            global_entry["icon_provider"] = icon_provider_val
                            changes_detected = True

            category_debug_print(f"[GLYPH] Processed {mappings_count} items from category_glyph_mappings")
        except Exception as e:
            category_debug_print(f"[GLYPH] Error reading from category_glyph_mappings: {e}")

        # Sync tag definitions from wm.category_tags (if available)
        # Only update cache if WM has data - don't wipe cache with empty WM data
        if hasattr(wm, "category_tags") and _is_collection_safe(wm.category_tags):
            try:
                new_tags_cache = {}
                for tag_item in wm.category_tags:
                    tag_name = getattr(tag_item, "name", "")
                    if not tag_name:
                        continue
                    glyph_hex = getattr(tag_item, "glyph", "") or ""
                    glyph = _hex_to_glyph(glyph_hex) if glyph_hex else ""
                    color = list(getattr(tag_item, "color", (0.0, 0.0, 0.0))[:3])
                    # НОВОЕ: Sync mode flags
                    mode_flags = getattr(tag_item, "mode_flags", _CATEGORY_TAG_DEFAULT_MODE_FLAGS)
                    # Keep icon fields when syncing WM -> cache.
                    # Without this, save path drops icon data back to defaults ("", 0).
                    icon_key = getattr(tag_item, "icon_key", "") or ""
                    icon_source_raw = getattr(tag_item, "icon_source", 0)
                    if isinstance(icon_source_raw, str):
                        icon_source_map = {'GLYPH': 0, 'BLENDER_ICON': 1, 'CUSTOM': 2}
                        icon_source = icon_source_map.get(icon_source_raw, 0)
                    else:
                        icon_source = int(icon_source_raw) if icon_source_raw is not None else 0

                    new_tags_cache[tag_name] = {
                        "glyph": glyph,
                        "color": color,
                        "mode_flags": mode_flags,
                        "icon_key": icon_key,
                        "icon_source": icon_source,
                    }

                # Only update if WM has tags OR our cache is empty (initial load)
                if new_tags_cache and _all_tags_cache != new_tags_cache:
                    # Detect tag renames and update _tag_order_cache before replacing cache.
                    # When name is changed via box.prop(tag, "name"), the RNA update fires
                    # and rebuilds cache here. Without this, the old name stays in _tag_order_cache
                    # and the new name gets appended at the end on next WM sync.
                    if _tag_order_cache:
                        old_keys = set(_all_tags_cache.keys())
                        new_keys = set(new_tags_cache.keys())
                        disappeared = old_keys - new_keys
                        appeared = new_keys - old_keys
                        # Same count means likely a rename, not add/delete
                        if disappeared and len(disappeared) == len(appeared):
                            for old_name in list(disappeared):
                                if old_name in _tag_order_cache:
                                    idx = _tag_order_cache.index(old_name)
                                    # Pick a new name not already in order cache
                                    for new_name in list(appeared):
                                        if new_name not in _tag_order_cache:
                                            _tag_order_cache[idx] = new_name
                                            appeared.discard(new_name)
                                            break

                    reset_all_tags_cache(new_tags_cache)
                    changes_detected = True
                    category_debug_print(f"[GLYPH] Synced {len(_all_tags_cache)} tag definitions from WM")
                elif not new_tags_cache and not _all_tags_cache:
                    # Both empty, nothing to do
                    pass
                elif not new_tags_cache and _all_tags_cache:
                    # WM is empty but cache has data - sync cache TO WM instead
                    category_debug_print(f"[GLYPH] WM category_tags empty, preserving {len(_all_tags_cache)} cached tags")
            except Exception as e:
                category_debug_print(f"[GLYPH] Error reading from category_tags: {e}")

        # Sync from category_glyph_overrides (user overrides)
        if _is_collection_safe(wm.category_glyph_overrides):
            try:
                overrides_count = 0
                for item in wm.category_glyph_overrides:
                    category = item.category
                    if not category or category == "__test__":
                        continue

                    overrides_count += 1
                    _pref_log_once(f"[GLYPH SYNC] Override found: category='{category}', glyph='{item.glyph}'")

                    # Persist overrides into the canonical GLOBAL cache key only.
                    space_type = getattr(item, 'space_type', -1)
                    cache_key = _make_cache_key(-1, category)

                    # Get current cached data or create new entry
                    if cache_key not in _glyph_cache:
                        # Create initial entry with default base_type (will be updated when glyph is set)
                        _glyph_cache[cache_key] = {
                            "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                            "default_glyph": "", "default_display_name": "", "base_type": "text_only",
                            "glyph_mode": "auto",
                            "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                        }

                    cached_entry = _glyph_cache[cache_key]

                    # Overrides take precedence
                    if item.glyph:
                        if cached_entry.get("glyph", "") != item.glyph:
                            cached_entry["glyph"] = item.glyph
                            # Update default_glyph only for non-glyph_only categories
                            # For glyph_only, default_glyph must stay as category name (original glyph)
                            is_glyph_only = _is_single_glyph(category)
                            if not is_glyph_only:
                                cached_entry["default_glyph"] = item.glyph
                            # Determine base_type based on glyph content, not category name
                            if _is_single_glyph(item.glyph):
                                # Single glyph - check if category name is also single glyph (true glyph_only)
                                base_type = "glyph_only" if is_glyph_only else "glyph_text"
                            else:
                                base_type = "glyph_text"
                            cached_entry["base_type"] = base_type
                            changes_detected = True
                            _pref_log_once(f"[GLYPH SYNC] Updated glyph for '{category}' (base_type={base_type})")

                    if item.display_name:
                        if cached_entry.get("display_name", "") != item.display_name:
                            cached_entry["display_name"] = item.display_name
                            changes_detected = True
                            _pref_log_once(f"[GLYPH SYNC] Updated display_name for '{category}'")

                    # Always save color from override (even if zero - user explicitly set it)
                    item_color = list(item.color[:3])
                    if cached_entry.get("color", [0.0, 0.0, 0.0]) != item_color:
                        cached_entry["color"] = item_color
                        changes_detected = True
                        _pref_log_once(f"[GLYPH SYNC] Updated color for '{category}' to {item_color}")

                    # Tags are NOT synced from WM to cache to avoid truncation issues
                    # Tags are stored only in _glyph_cache and JSON

                    if hasattr(item, 'icon_source'):
                        icon_source_raw = getattr(item, "icon_source", "AUTO")
                        if isinstance(icon_source_raw, str):
                            icon_source_val = icon_source_raw.lower()
                            if icon_source_val not in {"auto", "manual", "off"}:
                                icon_source_val = "auto"
                        else:
                            icon_source_from_int = {
                                0: "auto",
                                1: "manual",
                                2: "off",
                            }
                            icon_source_val = icon_source_from_int.get(int(icon_source_raw), "auto")
                        if cached_entry.get("icon_source", "auto") != icon_source_val:
                            cached_entry["icon_source"] = icon_source_val
                            changes_detected = True

                    if hasattr(item, 'glyph_mode'):
                        glyph_mode_raw = getattr(item, "glyph_mode", "AUTO")
                        if isinstance(glyph_mode_raw, str):
                            glyph_mode_val = glyph_mode_raw.lower()
                            if glyph_mode_val not in {"auto", "first_letter"}:
                                glyph_mode_val = "auto"
                        else:
                            glyph_mode_from_int = {0: "auto", 1: "first_letter"}
                            glyph_mode_val = glyph_mode_from_int.get(int(glyph_mode_raw), "auto")
                        if cached_entry.get("glyph_mode", "auto") != glyph_mode_val:
                            cached_entry["glyph_mode"] = glyph_mode_val
                            changes_detected = True

                    if hasattr(item, 'icon_key'):
                        icon_key_val = item.icon_key or ""
                        if cached_entry.get("icon_key", "") != icon_key_val:
                            cached_entry["icon_key"] = icon_key_val
                            changes_detected = True

                    if hasattr(item, 'icon_path'):
                        icon_path_val = item.icon_path or ""
                        if cached_entry.get("icon_path", "") != icon_path_val:
                            cached_entry["icon_path"] = icon_path_val
                            changes_detected = True

                    if hasattr(item, 'icon_provider'):
                        icon_provider_val = item.icon_provider or ""
                        if cached_entry.get("icon_provider", "") != icon_provider_val:
                            cached_entry["icon_provider"] = icon_provider_val
                            changes_detected = True

                    # If this is a space-specific entry with customizations, also update GLOBAL entry
                    # This ensures glyph, glyph_mode (first_letter), and icon persist across all spaces
                    has_customizations = (
                        (icon_source_val in ("manual", "off") or icon_key_val or icon_path_val) or
                        (glyph_mode_val != "auto") or
                        (item.glyph and item.glyph != cached_entry.get("default_glyph", ""))
                    )
                    if space_type != -1 and has_customizations:
                        global_cache_key = _make_cache_key(-1, category)
                        if global_cache_key not in _glyph_cache:
                            _glyph_cache[global_cache_key] = {
                                "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                                "default_glyph": "", "default_display_name": "", "base_type": "text_only",
                                "glyph_mode": "auto",
                                "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                            }

                        global_entry = _glyph_cache[global_cache_key]
                        item_glyph = item.glyph or ""

                        # Copy glyph data to global entry
                        if item_glyph and global_entry.get("glyph", "") != item_glyph:
                            global_entry["glyph"] = item_glyph
                            changes_detected = True
                            _pref_log_once(f"[GLYPH SYNC] Updated global glyph for '{category}' to '{item_glyph}'")

                        # Determine if we should update global entry based on icon_source priority
                        # Priority: 'manual' > 'off' > 'auto'
                        # - 'manual' always updates (explicit icon choice)
                        # - 'off' always updates (explicit glyph/first_letter choice)
                        # - 'auto' gets overwritten by anything
                        global_icon_source = global_entry.get("icon_source", "auto")
                        should_update = (
                            global_icon_source == "auto" or
                            icon_source_val == "manual" or
                            icon_source_val == "off"
                        )

                        # Copy glyph_mode to global entry only when allowed to update
                        if should_update and global_entry.get("glyph_mode", "auto") != glyph_mode_val:
                            global_entry["glyph_mode"] = glyph_mode_val
                            changes_detected = True
                            _pref_log_once(f"[GLYPH SYNC] Updated global glyph_mode for '{category}' to '{glyph_mode_val}'")

                        # Copy icon data to global entry
                        if should_update:
                            if global_entry.get("icon_source", "auto") != icon_source_val:
                                global_entry["icon_source"] = icon_source_val
                                changes_detected = True
                                _pref_log_once(f"[GLYPH SYNC] Updated global icon_source for '{category}'")
                            if global_entry.get("icon_key", "") != icon_key_val:
                                global_entry["icon_key"] = icon_key_val
                                changes_detected = True
                                _pref_log_once(f"[GLYPH SYNC] Updated global icon_key for '{category}' to '{icon_key_val}'")
                            if global_entry.get("icon_path", "") != icon_path_val:
                                global_entry["icon_path"] = icon_path_val
                                changes_detected = True
                                _pref_log_once(f"[GLYPH SYNC] Updated global icon_path for '{category}'")
                            if global_entry.get("icon_provider", "") != icon_provider_val:
                                global_entry["icon_provider"] = icon_provider_val
                                changes_detected = True
                                _pref_log_once(f"[GLYPH SYNC] Updated global icon_provider for '{category}'")

                _pref_log_once(f"[GLYPH SYNC] Processed {overrides_count} items from category_glyph_overrides")
            except Exception as e:
                category_debug_print(f"[GLYPH SYNC] Error reading from category_glyph_overrides: {e}")
                import traceback
                traceback.print_exc()

        # STEP 2: Sync from GLOBAL back to all SPACE entries for consistency
        # This ensures all space-specific entries have the same icon/glyph_mode data as GLOBAL
        categories_in_global = set()
        for cache_key in _glyph_cache.keys():
            if isinstance(cache_key, tuple) and len(cache_key) == 2:
                space_type_id, category = cache_key
                if space_type_id == -1:  # GLOBAL entry
                    categories_in_global.add(category)

        for category in categories_in_global:
            global_cache_key = (-1, category)
            if global_cache_key not in _glyph_cache:
                continue
            global_entry = _glyph_cache[global_cache_key]

            # Get global values
            global_glyph_mode = global_entry.get("glyph_mode", "auto")
            global_icon_source = global_entry.get("icon_source", "auto")
            global_icon_key = global_entry.get("icon_key", "")
            global_icon_path = global_entry.get("icon_path", "")
            global_icon_provider = global_entry.get("icon_provider", "")

            # Sync to all space-specific entries for this category
            for cache_key in _glyph_cache.keys():
                if isinstance(cache_key, tuple) and len(cache_key) == 2:
                    space_type_id, cat = cache_key
                    if space_type_id != -1 and cat == category:  # Space-specific entry for this category
                        space_entry = _glyph_cache[cache_key]

                        # Sync glyph_mode
                        if space_entry.get("glyph_mode", "auto") != global_glyph_mode:
                            space_entry["glyph_mode"] = global_glyph_mode
                            changes_detected = True
                            _pref_log_once(f"[GLYPH SYNC] Synced glyph_mode '{global_glyph_mode}' from GLOBAL to space {space_type_id} for '{category}'")

                        # Sync icon data
                        if space_entry.get("icon_source", "auto") != global_icon_source:
                            space_entry["icon_source"] = global_icon_source
                            changes_detected = True
                            _pref_log_once(f"[GLYPH SYNC] Synced icon_source '{global_icon_source}' from GLOBAL to space {space_type_id} for '{category}'")
                        if space_entry.get("icon_key", "") != global_icon_key:
                            space_entry["icon_key"] = global_icon_key
                            changes_detected = True
                        if space_entry.get("icon_path", "") != global_icon_path:
                            space_entry["icon_path"] = global_icon_path
                            changes_detected = True
                        if space_entry.get("icon_provider", "") != global_icon_provider:
                            space_entry["icon_provider"] = global_icon_provider
                            changes_detected = True

        # Save to JSON if changes were detected
        if changes_detected:
            if _save_glyph_mappings_to_file():
                category_debug_print(f"[GLYPH] Saved {len(_glyph_cache)} category mappings from WM to JSON")
            else:
                category_debug_print("[GLYPH] Failed to save category mappings from WM")
        else:
            # No changes detected, but still save to ensure tags are persisted
            # This is important when Save is clicked after tag changes
            if _save_glyph_mappings_to_file():
                category_debug_print(f"[GLYPH] Saved {len(_glyph_cache)} category mappings to JSON (no changes detected)")
            else:
                category_debug_print("[GLYPH] Failed to save category mappings to JSON")

        return changes_detected

    except Exception as e:
        category_debug_print(f"[GLYPH] Error syncing from WM: {e}")
        return False
