# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""App handlers, auto-save logic, and persistence helpers for the Tabs-System.

Extracted verbatim from ``space_userpref.py`` (no behavioural change).
All mutable state lives in glyph_tag_system._state; this module imports
state objects by reference (in-place mutations) and uses accessor functions
for full reassignment.

Functions that call back into space_userpref (``sync_glyph_mappings_to_wm``,
``register_category_glyph_mappings``, ``sync_wm_to_glyph_cache``,
``_discover_active_categories``, ``_merge_discovered_categories``,
``_extension_has_only_reserved_categories``) use lazy imports inside their
bodies to break the circular-import cycle.

``_register_glyph_handlers`` and ``_unregister_glyph_handlers`` are the
public API of this module; they are called from space_userpref.register()
and space_userpref.unregister().
"""

import bpy
import json

from bl_ui.glyph_tag_system.conversions import _hex_to_glyph
from bl_ui.glyph_tag_system.schema_keys import KEY_TAG_ORDER
from bl_ui.glyph_tag_system.log import (
    category_debug_print,
    save_debug_print,
)
from bl_ui.glyph_tag_system.persistence import (
    safe_file_write,
    create_backup,
)
from bl_ui.glyph_tag_system._state import (
    state,
    set_auto_save_pending,
    set_auto_save_glyph_pending,
    set_auto_save_glyph_skip_wm_sync,
    set_pending_display_mode_change,
    set_display_mode_debounce_timer_running,
    is_auto_save_pending,
    is_auto_save_glyph_pending,
    get_auto_save_glyph_skip_wm_sync,
    is_display_mode_debounce_timer_running,
    is_preview_mode_active,
    get_pending_display_mode_change,
    get_pending_extension_context,
)
from bl_ui.glyph_tag_system.glyph_cache import (
    _get_glyphs_filepath,
    _save_glyph_mappings_to_file,
)
from bl_ui.glyph_tag_system.tags_cache import get_tag_names


# -----------------------------------------------------------------------------
# Small math helper
# -----------------------------------------------------------------------------


def is_zero_v3(color):
    """Check if a 3-component color is all zeros."""
    return color[0] == 0.0 and color[1] == 0.0 and color[2] == 0.0


# -----------------------------------------------------------------------------
# Deferred auto-save — tags
# -----------------------------------------------------------------------------


def _cancel_deferred_auto_save(reason=""):
    """Cancel queued deferred auto-save (used by Cancel flow)."""
    set_auto_save_pending(False)
    if bpy.app.timers.is_registered(_deferred_save):
        try:
            bpy.app.timers.unregister(_deferred_save)
            if reason:
                category_debug_print(f"[AUTO_SAVE_TAGS] Deferred save canceled ({reason})")
            else:
                category_debug_print("[AUTO_SAVE_TAGS] Deferred save canceled")
        except Exception as e:
            category_debug_print(f"[AUTO_SAVE_TAGS] Failed to cancel deferred save: {e}")


def _auto_save_tags():
    """Mark that tags need to be saved (called from update callbacks).
    Does NOT sync to WM - only schedules a JSON save."""
    category_debug_print("[AUTO_SAVE_TAGS] CALLED - scheduling deferred save")
    set_auto_save_pending(True)
    # Use timer to batch saves (no sync - WM already has the data)
    if not bpy.app.timers.is_registered(_deferred_save):
        bpy.app.timers.register(_deferred_save, first_interval=0.5)
        category_debug_print("[AUTO_SAVE_TAGS] Timer registered for deferred save")
    else:
        category_debug_print("[AUTO_SAVE_TAGS] Timer already registered")


def _deferred_save():
    """Deferred save to batch multiple changes."""
    category_debug_print(f"[DEFERRED_SAVE] Called, _auto_save_pending={is_auto_save_pending()}")
    if is_auto_save_pending():
        category_debug_print("[DEFERRED_SAVE] Calling _save_tags_to_json()")
        _save_tags_to_json()
        set_auto_save_pending(False)
    return None  # Don't repeat timer


# -----------------------------------------------------------------------------
# Deferred auto-save — glyph mappings
# -----------------------------------------------------------------------------


def _auto_save_glyph_mappings(skip_wm_sync=False):
    """Mark that glyph mappings need to be saved (called from UI callbacks).
    Schedules a deferred save to prevent UI freezes."""
    set_auto_save_glyph_pending(True)
    set_auto_save_glyph_skip_wm_sync(skip_wm_sync)
    if not bpy.app.timers.is_registered(_deferred_save_glyphs):
        bpy.app.timers.register(_deferred_save_glyphs, first_interval=0.5)

def _deferred_save_glyphs():
    if is_auto_save_glyph_pending():
        _save_glyph_mappings_to_file(skip_wm_sync=get_auto_save_glyph_skip_wm_sync())
        set_auto_save_glyph_pending(False)
        set_auto_save_glyph_skip_wm_sync(False)
    return None


# -----------------------------------------------------------------------------
# Debounce mechanism for display mode changes (Icon/Glyph toggle)
# -----------------------------------------------------------------------------


def _schedule_display_mode_change(tag_name, icon_source):
    """Schedule a deferred display mode change with debouncing.
    Updates WM immediately for visual feedback, but defers cache/JSON update."""
    set_pending_display_mode_change((tag_name, icon_source))
    category_debug_print(f"[DISPLAY_MODE_DEBOUNCE] Scheduled: tag='{tag_name}', icon_source={icon_source}")

    if not is_display_mode_debounce_timer_running():
        set_display_mode_debounce_timer_running(True)
        if not bpy.app.timers.is_registered(_process_pending_display_mode_change):
            bpy.app.timers.register(_process_pending_display_mode_change, first_interval=0.3)
            category_debug_print("[DISPLAY_MODE_DEBOUNCE] Timer registered")

def _process_pending_display_mode_change():
    """Process the pending display mode change (called by timer)."""
    _pending_display_mode_change = get_pending_display_mode_change()
    category_debug_print(f"[DISPLAY_MODE_DEBOUNCE] Timer fired, pending={_pending_display_mode_change}")

    if _pending_display_mode_change is not None:
        tag_name, icon_source = _pending_display_mode_change

        if tag_name in state.all_tags_cache:
            state.all_tags_cache[tag_name]["icon_source"] = icon_source
            category_debug_print(f"[DISPLAY_MODE_DEBOUNCE] Updated cache: tag='{tag_name}', icon_source={icon_source}")

            # Now save to JSON
            _auto_save_tags()

        set_pending_display_mode_change(None)

    set_display_mode_debounce_timer_running(False)
    return None  # Don't repeat timer


# -----------------------------------------------------------------------------
# WM → cache sync helpers
# -----------------------------------------------------------------------------


def _sync_mode_flags_from_wm_to_cache():
    """Sync mode flags and icon properties from Python CategoryTagItem to state.all_tags_cache.
    This captures UI changes to mode checkboxes and icon settings before saving."""
    try:
        wm = bpy.context.window_manager
        if not wm or not hasattr(wm, 'category_tags'):
            return
        for tag_item in wm.category_tags:
            tag_name = tag_item.name
            if tag_name in state.all_tags_cache and isinstance(state.all_tags_cache[tag_name], dict):
                state.all_tags_cache[tag_name]["mode_flags"] = tag_item.mode_flags
                # Sync icon properties for tags with icons
                if hasattr(tag_item, 'icon_key'):
                    wm_icon_key = tag_item.icon_key
                    wm_icon_key_type = type(wm_icon_key).__name__
                    category_debug_print(f"[SYNC_WM_CACHE] tag='{tag_name}' WM icon_key='{wm_icon_key}' (type={wm_icon_key_type})")
                    state.all_tags_cache[tag_name]["icon_key"] = wm_icon_key
                if hasattr(tag_item, 'icon_path'):
                    state.all_tags_cache[tag_name]["icon_path"] = tag_item.icon_path
                if hasattr(tag_item, 'icon_source'):
                    state.all_tags_cache[tag_name]["icon_source"] = tag_item.icon_source
    except Exception as e:
        category_debug_print(f"[GLYPH] Error syncing mode flags: {e}")


# -----------------------------------------------------------------------------
# JSON persistence — tags
# -----------------------------------------------------------------------------


def _save_tags_to_json():
    """Save tags to JSON file."""

    # CRITICAL DEBUG: Always log (even if TAG_DEBUG=False)
    save_debug_print(f"[SAVE_TAGS_TO_JSON] === CALLED ===")
    save_debug_print(f"[SAVE_TAGS_TO_JSON] _preview_mode_active={is_preview_mode_active()}")

    category_debug_print("[SAVE_TAGS_TO_JSON] === CALLED ===")

    # Skip saving during preview mode to prevent premature WM sync
    if is_preview_mode_active():
        save_debug_print(f"[SAVE_TAGS_TO_JSON] SKIPPED: Preview mode active")
        category_debug_print("[SAVE_TAGS_TO_JSON] Preview mode active - skipping save to avoid WM sync during preview")
        return

    # НОВОЕ: First sync mode flags from WM items to cache (captures UI changes)
    save_debug_print(f"[SAVE_TAGS_TO_JSON] Step 1: Syncing mode flags from WM to cache")
    category_debug_print("[SAVE_TAGS_TO_JSON] Step 1: Syncing mode flags from WM to cache")
    _sync_mode_flags_from_wm_to_cache()
    save_debug_print(f"[SAVE_TAGS_TO_JSON] Step 2: Syncing glyph mappings to WM")
    category_debug_print("[SAVE_TAGS_TO_JSON] Step 2: Syncing glyph mappings to WM")
    # OPTIMIZATION: Skip icon detection during save (will run in background)
    from bl_ui.glyph_tag_system.wm_sync_to_wm import sync_glyph_mappings_to_wm
    sync_glyph_mappings_to_wm(skip_icon_detection=True)
    save_debug_print(f"[SAVE_TAGS_TO_JSON] Step 3: Saving to JSON file")
    category_debug_print("[SAVE_TAGS_TO_JSON] Step 3: Saving to JSON file")
    _save_glyph_mappings_to_file()
    save_debug_print(f"[SAVE_TAGS_TO_JSON] === COMPLETED ===")
    category_debug_print("[SAVE_TAGS_TO_JSON] === COMPLETED ===")


def _save_tag_order_only():
    """Save only tag order to JSON without rebuilding WM collection.
    This avoids potential memory issues when moving tags."""

    filepath = _get_glyphs_filepath()
    if not filepath:
        category_debug_print("[TAG ORDER] No filepath for saving")
        return False

    # Load existing data to preserve the other sections (mappings / all_tags /
    # category_orders) that this lightweight path does not rebuild. Atomic writes
    # guarantee a reader sees a complete file, never a partial one.
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            data = json.load(f)
    except FileNotFoundError:
        # No file on disk yet: nothing to preserve, and writing a tag_order-only
        # dict would drop the cache's mappings/tags on the next load. Delegate to the
        # full save, which persists every section from the cache.
        return _save_glyph_mappings_to_file()
    except Exception as e:
        # A readable-but-corrupt file must NOT be overwritten with a tag_order-only
        # dict: that would wipe mappings / all_tags / category_orders. Abort instead.
        category_debug_print(f"[TAG ORDER] Aborting: cannot read existing file: {e}")
        return False

    if not isinstance(data, dict):
        category_debug_print("[TAG ORDER] Aborting: existing file is not a JSON object")
        return False

    # Back up only once we know there is valid existing data worth preserving.
    create_backup(filepath)

    # Update tag_order from the cache (source of truth for manual ordering here).
    data[KEY_TAG_ORDER] = list(state.tag_order_cache)

    # Save back to file
    try:
        with safe_file_write(filepath) as f:
            json.dump(data, f, indent=2, ensure_ascii=False)
        category_debug_print(f"[TAG ORDER] Saved order: {state.tag_order_cache}")
        return True
    except Exception as e:
        category_debug_print(f"[TAG ORDER] Save failed: {e}")
        return False


# -----------------------------------------------------------------------------
# Dynamic enum callback for tag selection
# -----------------------------------------------------------------------------


def tag_enum_items_callback(self, context):
    """Dynamic enum callback for tag selection."""
    tags = get_tag_names()
    if not tags:
        return [('__none__', "No tags available", "Create a tag first")]
    return [(tag, tag, f"Tag: {tag}") for tag in tags]


# -----------------------------------------------------------------------------
# App handlers
# -----------------------------------------------------------------------------


@bpy.app.handlers.persistent
def _on_load_post(dummy):
    """Load glyph mappings after file load."""
    from bl_ui.glyph_tag_system import wm_sync_to_wm as _wm_sync_to_wm
    _wm_sync_to_wm.register_category_glyph_mappings()


@bpy.app.handlers.persistent
def _on_save_pre(dummy):
    """Sync glyph mappings from WM to JSON before saving preferences."""
    from bl_ui.glyph_tag_system import wm_sync_from_wm as _wm_sync_from_wm
    _wm_sync_from_wm.sync_wm_to_glyph_cache()


@bpy.app.handlers.persistent
def _on_extension_repos_update_post(dummy=None):
    """Sync category glyphs after extension installation/update."""
    # Re-discover categories after extension changes to find new bl_category panels
    category_debug_print("=" * 80)
    category_debug_print("[GLYPH EXTENSION UPDATE] >>> START _on_extension_repos_update_post handler")
    _pending_extension_context = get_pending_extension_context()
    category_debug_print(f"[GLYPH EXTENSION UPDATE] Pending extension context: {_pending_extension_context}")
    category_debug_print("=" * 80)
    try:
        from bl_ui.glyph_tag_system import discovery_scan as _discovery_scan
        from bl_ui.glyph_tag_system import discovery_merge as _discovery_merge
        from bl_ui.glyph_tag_system import unassigned as _unassigned
        result_before = _discovery_scan._discover_active_categories()
        # Handle both old and new return format
        if isinstance(result_before, tuple):
            discovered_before, _ = result_before
        else:
            discovered_before = result_before
        cache_before = set(state.glyph_cache.keys())
        category_debug_print(
            f"[GLYPH EXTENSION UPDATE DEBUG] before merge: "
            f"discovered={len(discovered_before)}, cache={len(cache_before)}, "
            f"missing_in_cache={sorted(discovered_before - cache_before)}"
        )

        category_debug_print("[GLYPH EXTENSION UPDATE] >>> Calling _merge_discovered_categories()...")
        # Enable icon detection for immediate response in extension updates
        merge_result = _discovery_merge._merge_discovered_categories(skip_icon_detection=False)
        category_debug_print(f"[GLYPH EXTENSION UPDATE] >>> _merge_discovered_categories() returned: {merge_result}")

        result_after = _discovery_scan._discover_active_categories()
        if isinstance(result_after, tuple):
            discovered_after, _ = result_after
        else:
            discovered_after = result_after
        cache_after = set(state.glyph_cache.keys())
        category_debug_print(
            f"[GLYPH EXTENSION UPDATE DEBUG] after merge: "
            f"merge_result={merge_result}, discovered={len(discovered_after)}, cache={len(cache_after)}, "
            f"added_to_cache={sorted(cache_after - cache_before)}, "
            f"still_missing={sorted(discovered_after - cache_after)}"
        )

        category_debug_print("[GLYPH EXTENSION UPDATE] >>> Calling sync_glyph_mappings_to_wm()...")
        # OPTIMIZATION: Enable icon detection in extension update handler for immediate updates
        from bl_ui.glyph_tag_system.wm_sync_to_wm import sync_glyph_mappings_to_wm
        sync_glyph_mappings_to_wm(skip_icon_detection=False)
        category_debug_print("[GLYPH EXTENSION UPDATE] >>> sync_glyph_mappings_to_wm() completed")

        # Check if this is a reserved-only extension and switch to reserved category
        if _pending_extension_context and _pending_extension_context.get("extension_id"):
            extension_id = _pending_extension_context["extension_id"]
            wm = bpy.context.window_manager
            if wm and _unassigned._extension_has_only_reserved_categories(wm, extension_id):
                category_debug_print(f"[GLYPH EXTENSION UPDATE] >>> Reserved-only extension detected: {extension_id}")
                category_debug_print("[GLYPH EXTENSION UPDATE] >>> Scheduling switch to reserved category...")
                # Set deferred activation for reserved category.
                # This is handled by C++ code in deferred_category_activation_execute()
                # which runs during panel draw when context is safe.
                category_debug_print(f"[RESERVED SWITCH] Setting deferred activation for extension '{extension_id}'")
                # Store in pending context for C++ to pick up
                _pending_extension_context["activate_reserved"] = True
                bpy.app.timers.register(lambda: None, first_interval=0.1)  # Trigger UI refresh

        # Trigger tag bar update to show/hide "New Add-ons!" button
        # Use timer to ensure context is available
        category_debug_print("[GLYPH EXTENSION UPDATE] >>> Scheduling tag bar update via timer...")
        def _trigger_tag_bar_update():
            try:
                import bpy
                # Force refresh of category glyphs by toggling a dummy property
                # This triggers the tag bar listener to update
                wm = bpy.context.window_manager
                if wm:
                    # Send notification via operator that triggers ND_CATEGORY_GLYPHS
                    bpy.ops.wm.sync_category_glyphs('EXEC_DEFAULT')
                    category_debug_print("[GLYPH EXTENSION UPDATE] >>> Tag bar update triggered successfully")
                return None  # Stop timer
            except Exception as err:
                category_debug_print(f"[GLYPH EXTENSION UPDATE] Warning: Could not trigger update: {err}")
                return None  # Stop timer anyway
        bpy.app.timers.register(_trigger_tag_bar_update, first_interval=0.1)

        category_debug_print("=" * 80)
        category_debug_print("[GLYPH EXTENSION UPDATE] >>> END _on_extension_repos_update_post handler")
        category_debug_print("=" * 80)
    except Exception as e:
        category_debug_print(f"[GLYPH] Error during extension repos update sync: {e}")
        import traceback
        traceback.print_exc()

@bpy.app.handlers.persistent
def _on_version_update(dummy):
    """Sync category glyphs after Blender version update or addon enable/disable."""
    # Re-discover categories in case new addons were enabled
    try:
        from bl_ui.glyph_tag_system import discovery_scan as _discovery_scan
        from bl_ui.glyph_tag_system import discovery_merge as _discovery_merge
        result_before = _discovery_scan._discover_active_categories()
        # Handle both old and new return format
        if isinstance(result_before, tuple):
            discovered_before, _ = result_before
        else:
            discovered_before = result_before
        cache_before = set(state.glyph_cache.keys())
        category_debug_print(
            f"[GLYPH VERSION UPDATE DEBUG] before merge: "
            f"discovered={len(discovered_before)}, cache={len(cache_before)}, "
            f"missing_in_cache={sorted(discovered_before - cache_before)}"
        )

        # Enable icon detection for immediate response in version updates
        merge_result = _discovery_merge._merge_discovered_categories(skip_icon_detection=False)

        result_after = _discovery_scan._discover_active_categories()
        if isinstance(result_after, tuple):
            discovered_after, _ = result_after
        else:
            discovered_after = result_after
        cache_after = set(state.glyph_cache.keys())
        category_debug_print(
            f"[GLYPH VERSION UPDATE DEBUG] after merge: "
            f"merge_result={merge_result}, discovered={len(discovered_after)}, cache={len(cache_after)}, "
            f"added_to_cache={sorted(cache_after - cache_before)}, "
            f"still_missing={sorted(discovered_after - cache_after)}"
        )

        # OPTIMIZATION: Enable icon detection in version update handler for immediate updates
        from bl_ui.glyph_tag_system.wm_sync_to_wm import sync_glyph_mappings_to_wm
        sync_glyph_mappings_to_wm(skip_icon_detection=False)
    except Exception as e:
        category_debug_print(f"[GLYPH] Error during version update sync: {e}")


# -----------------------------------------------------------------------------
# Handler registration / unregistration
# -----------------------------------------------------------------------------


def _register_glyph_handlers():
    """Register application handlers for the category/tag glyph system."""
    if _on_load_post not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_on_load_post)

    if _on_save_pre not in bpy.app.handlers.save_pre:
        bpy.app.handlers.save_pre.append(_on_save_pre)

    if hasattr(bpy.app.handlers, "version_update"):
        if _on_version_update not in bpy.app.handlers.version_update:
            bpy.app.handlers.version_update.append(_on_version_update)
            category_debug_print("[GLYPH SYNC] Registered version_update handler for addon/category rediscovery")
    else:
        category_debug_print("[GLYPH SYNC] WARNING: bpy.app.handlers.version_update is unavailable")

    # Register extension repos update post handler for category discovery after addon installation
    if hasattr(bpy.app.handlers, "_extension_repos_update_post"):
        if _on_extension_repos_update_post not in bpy.app.handlers._extension_repos_update_post:
            bpy.app.handlers._extension_repos_update_post.append(_on_extension_repos_update_post)
            category_debug_print("[GLYPH SYNC] Registered _extension_repos_update_post handler for extension category discovery")
    else:
        category_debug_print("[GLYPH SYNC] WARNING: bpy.app.handlers._extension_repos_update_post is unavailable")


def _unregister_glyph_handlers():
    """Remove application handlers registered by the category/tag glyph system."""
    if _on_load_post in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_on_load_post)

    if _on_save_pre in bpy.app.handlers.save_pre:
        bpy.app.handlers.save_pre.remove(_on_save_pre)

    if hasattr(bpy.app.handlers, "version_update"):
        if _on_version_update in bpy.app.handlers.version_update:
            bpy.app.handlers.version_update.remove(_on_version_update)

    if hasattr(bpy.app.handlers, "_extension_repos_update_post"):
        if _on_extension_repos_update_post in bpy.app.handlers._extension_repos_update_post:
            bpy.app.handlers._extension_repos_update_post.remove(_on_extension_repos_update_post)
