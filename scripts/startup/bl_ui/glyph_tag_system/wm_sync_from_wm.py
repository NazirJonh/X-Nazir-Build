# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""WindowManager -> cache synchronisation for the Tabs-System.

Split out of ``wm_sync.py`` (Candidate 6): this module owns the WM -> cache direction —
reading user changes from ``wm.category_glyph_overrides``/``category_glyph_mappings``/
``category_tags`` back into the Python cache and persisting them to JSON.

Unlike ``wm_sync_to_wm.py``, this module has NO dependency on discovery (scan or merge):
it only ever reads from WM and writes into the cache/JSON.
"""

import bpy

from bl_ui.glyph_tag_system.conversions import (
    _hex_to_glyph,
    _is_single_glyph,
    _make_cache_key,
    flags_to_modes,
    flags_to_spaces,
)
from bl_ui.glyph_tag_system.log import (
    _pref_log_once,
    category_debug_print,
)
from bl_ui.glyph_tag_system.defaults import (
    _CATEGORY_TAG_DEFAULT_MODE_FLAGS,
)
from bl_ui.glyph_tag_system._state import (
    state,
    reset_all_tags_cache,
    set_sync_in_progress,
    is_initial_load_complete,
    is_sync_in_progress,
)
from bl_ui.glyph_tag_system.glyph_cache import (
    _is_collection_safe,
    _save_glyph_mappings_to_file,
)


def sync_wm_to_glyph_cache():
    """Sync glyph mappings from window manager collection back to cache and JSON.

    This function reads user changes from category_glyph_overrides and
    category_glyph_mappings in WM and saves them to the JSON file.
    """

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
                if cache_key not in state.glyph_cache:
                    state.glyph_cache[cache_key] = {
                        "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                        "default_glyph": "", "default_display_name": "", "base_type": "text_only",
                        "glyph_mode": "auto",
                        "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                    }

                cached_entry = state.glyph_cache[cache_key]

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
                # Tags are stored only in state.glyph_cache and JSON

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
                    if global_cache_key not in state.glyph_cache:
                        state.glyph_cache[global_cache_key] = {
                            "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                            "default_glyph": "", "default_display_name": "", "base_type": "text_only",
                            "glyph_mode": "auto",
                            "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                        }

                    global_entry = state.glyph_cache[global_cache_key]
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
                if new_tags_cache and state.all_tags_cache != new_tags_cache:
                    # Detect tag renames and update state.tag_order_cache before replacing cache.
                    # When name is changed via box.prop(tag, "name"), the RNA update fires
                    # and rebuilds cache here. Without this, the old name stays in state.tag_order_cache
                    # and the new name gets appended at the end on next WM sync.
                    if state.tag_order_cache:
                        old_keys = set(state.all_tags_cache.keys())
                        new_keys = set(new_tags_cache.keys())
                        disappeared = old_keys - new_keys
                        appeared = new_keys - old_keys
                        # Same count means likely a rename, not add/delete
                        if disappeared and len(disappeared) == len(appeared):
                            for old_name in list(disappeared):
                                if old_name in state.tag_order_cache:
                                    idx = state.tag_order_cache.index(old_name)
                                    # Pick a new name not already in order cache
                                    for new_name in list(appeared):
                                        if new_name not in state.tag_order_cache:
                                            state.tag_order_cache[idx] = new_name
                                            appeared.discard(new_name)
                                            break

                    reset_all_tags_cache(new_tags_cache)
                    changes_detected = True
                    category_debug_print(f"[GLYPH] Synced {len(state.all_tags_cache)} tag definitions from WM")
                elif not new_tags_cache and not state.all_tags_cache:
                    # Both empty, nothing to do
                    pass
                elif not new_tags_cache and state.all_tags_cache:
                    # WM is empty but cache has data - sync cache TO WM instead
                    category_debug_print(f"[GLYPH] WM category_tags empty, preserving {len(state.all_tags_cache)} cached tags")
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
                    if cache_key not in state.glyph_cache:
                        # Create initial entry with default base_type (will be updated when glyph is set)
                        state.glyph_cache[cache_key] = {
                            "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                            "default_glyph": "", "default_display_name": "", "base_type": "text_only",
                            "glyph_mode": "auto",
                            "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                        }

                    cached_entry = state.glyph_cache[cache_key]

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
                    # Tags are stored only in state.glyph_cache and JSON

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
                        if global_cache_key not in state.glyph_cache:
                            state.glyph_cache[global_cache_key] = {
                                "glyph": "", "display_name": "", "color": [0.0, 0.0, 0.0], "tags": [],
                                "default_glyph": "", "default_display_name": "", "base_type": "text_only",
                                "glyph_mode": "auto",
                                "icon_source": "auto", "icon_key": "", "icon_path": "", "icon_provider": "",
                            }

                        global_entry = state.glyph_cache[global_cache_key]
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
        for cache_key in state.glyph_cache.keys():
            if isinstance(cache_key, tuple) and len(cache_key) == 2:
                space_type_id, category = cache_key
                if space_type_id == -1:  # GLOBAL entry
                    categories_in_global.add(category)

        for category in categories_in_global:
            global_cache_key = (-1, category)
            if global_cache_key not in state.glyph_cache:
                continue
            global_entry = state.glyph_cache[global_cache_key]

            # Get global values
            global_glyph_mode = global_entry.get("glyph_mode", "auto")
            global_icon_source = global_entry.get("icon_source", "auto")
            global_icon_key = global_entry.get("icon_key", "")
            global_icon_path = global_entry.get("icon_path", "")
            global_icon_provider = global_entry.get("icon_provider", "")

            # Sync to all space-specific entries for this category
            for cache_key in state.glyph_cache.keys():
                if isinstance(cache_key, tuple) and len(cache_key) == 2:
                    space_type_id, cat = cache_key
                    if space_type_id != -1 and cat == category:  # Space-specific entry for this category
                        space_entry = state.glyph_cache[cache_key]

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
                category_debug_print(f"[GLYPH] Saved {len(state.glyph_cache)} category mappings from WM to JSON")
            else:
                category_debug_print("[GLYPH] Failed to save category mappings from WM")
        else:
            # No changes detected, but still save to ensure tags are persisted
            # This is important when Save is clicked after tag changes
            if _save_glyph_mappings_to_file():
                category_debug_print(f"[GLYPH] Saved {len(state.glyph_cache)} category mappings to JSON (no changes detected)")
            else:
                category_debug_print("[GLYPH] Failed to save category mappings to JSON")

        return changes_detected

    except Exception as e:
        category_debug_print(f"[GLYPH] Error syncing from WM: {e}")
        return False
