# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Category-discovery merge logic for the Tabs-System.

Split out of ``discovery.py`` (Candidate 6): this module contains the "merge" half —
functions that decide how discovered categories (from ``discovery_scan``) are folded
into the glyph cache: extension attribution, canonical-name dedup, pending-tag
bookkeeping, and icon assignment.

This module has no dependency on ``wm_sync`` or ``persistence``: it is a leaf, like
``discovery_scan``. ``extension_post_install_handler`` returns ``updated_existing``
(bool) instead of pushing the change to WM/JSON itself; the caller (via
``api.sync_and_save_after_extension_install()``) is responsible for that follow-up.
"""

import bpy
import os
import time

from bl_ui.glyph_tag_system.defaults import (
    BL_CONTEXT_TO_MODE_FLAG,
    DEFAULT_CATEGORY_GLYPHS,
    POPULAR_ADDONS_DB_ENABLED,
    SPACE_TO_FLAG,
)
from bl_ui.glyph_tag_system.conversions import (
    _is_single_glyph,
    _make_cache_key,
    _normalize_category_key,
    _space_type_id_to_str,
    flags_to_modes,
    flags_to_spaces,
    modes_to_flags,
    spaces_to_flags,
)
from bl_ui.glyph_tag_system.log import (
    _log_once,
    _pref_log_once,
    category_debug_print,
    tag_log,
)
from bl_ui.glyph_tag_system.migrations import (
    _normalize_category_data,
)
from bl_ui.glyph_tag_system.schema_keys import (
    KEY_BASE_TYPE,
    KEY_DEFAULT_DISPLAY_NAME,
    KEY_DEFAULT_GLYPH,
    KEY_DISPLAY_NAME,
    KEY_GLYPH,
    KEY_INSTALL_MODE_FLAG,
    KEY_SOURCE_EXTENSION,
)
from bl_ui.glyph_tag_system.schema_fields import (
    new_entry,
)
from bl_ui.glyph_tag_system.glyph_cache import (
    _find_panel_label_for_category,
    _save_glyph_mappings_to_file,
)
from bl_ui.glyph_tag_system.discovery_scan import (
    _auto_detect_extension_icon_path,
    _discover_active_categories,
    _extension_id_match_keys,
    _extension_ids_match,
    _extension_manifest_match_keys,
    _get_discovery_source_priority,
    _pick_canonical_category_name,
    _scan_extension_icon_path,
)

# Single owner of shared state; ``state.<field>`` is always the live object (see _state.py).
from bl_ui.glyph_tag_system._state import state
from bl_ui.glyph_tag_system._state import (
    set_extension_install_icon_cache_entry,
    set_pending_extension_context,
    # Reassignment accessors for shared state (call these instead of `name = ...` so
    # every importer sees the new value; a direct reassignment would orphan the single
    # owner object in bl_ui.glyph_tag_system._state).
    reset_icon_detection_session_checked,
    set_discovery_debounce_timer,
    set_install_from_disk_just_occurred,
    # Live getters for reassignable scalars/objects (module-level imports bind a
    # snapshot at import time; reading through these always resolves to _state).
    is_in_ui_draw,
    is_install_from_disk_just_occurred,
    get_discovery_debounce_timer,
    get_merge_discovery_cache,
    get_pending_extension_context,
)

# NOTE: ``_DISCOVERY_DEBOUNCE_INTERVAL`` is referenced below (cache-age check and the
# deferred-merge timer) but was never defined in the original ``discovery.py`` either.
# Preserved verbatim as a pre-existing latent NameError — not introduced by this split,
# not fixed here (see Candidate 6 notes: flagged for the user, out of scope for a
# behaviour-preserving file split).


# -----------------------------------------------------------------------------
# Extension post-install handler
# -----------------------------------------------------------------------------

def extension_post_install_handler(extension_id, space_type=-1, mode_flag=0, tag_already_assigned=False, is_install_from_disk=False):
    """Record the extension that was just installed so the next category discovery
    can mark any new categories as pending tag assignment.

    OPTIMIZATION: Now scans Python files for bl_category ONLY once at install time,
    not during every UI draw. Results are cached permanently.

    Call this immediately after an extension is installed (before the next
    sync/discovery cycle).

    Args:
        extension_id:         Extension package ID (e.g. "blender_org/brushstroke_tools").
        space_type:           Space type where the extension was activated, or -1 for global.
        mode_flag:            Bitmask of mode flags where the extension was activated.
        tag_already_assigned: If True, the category was dropped onto tabs and will be visible
                              without tag filtering. Don't show "New Add-ons!" button.
        is_install_from_disk: If True, the extension was installed via "Install from Disk" operator.
                              This triggers Python file scanning for bl_category values.
    """
    category_debug_print(f"[EXTENSION POST INSTALL] >>> START handler for {extension_id!r}")
    category_debug_print(f"[EXTENSION POST INSTALL] space_type={space_type}, mode_flag={mode_flag:#010x}, tag_assigned={tag_already_assigned}, from_disk={is_install_from_disk}")
    category_debug_print(f"[EXTENSION POST INSTALL] Current cache size: {len(state.glyph_cache)} categories")

    set_pending_extension_context({
        "extension_id": extension_id,
        "space_type": space_type,
        "mode_flag": mode_flag,
        "timestamp": time.time(),
        "tag_already_assigned": tag_already_assigned,
        "is_install_from_disk": is_install_from_disk,
    })

    # OPTIMIZATION: Scan for icon file and bl_category ONLY once at install time
    # This avoids repeated disk scans during UI draws
    # FIX: Also scan for drag-and-drop installs by discovering pkg_path from discovered categories
    pkg_path = None
    pkg_name = None

    if extension_id and extension_id.strip():
        try:
            import importlib
            module = importlib.import_module(extension_id.replace("add-on-", "bl_ext."))
            pkg_path = os.path.dirname(getattr(module, "__file__", ""))
            pkg_name = extension_id.replace("add-on-", "").replace(".", "_")
        except Exception:
            pass

    # FIX: If pkg_path not found (drag-and-drop), try to discover it from panel discovery
    if not pkg_path:
        # Look for recently discovered categories that might belong to this extension
        for cache_key, cat_data in state.glyph_cache.items():
            if isinstance(cache_key, tuple) and len(cache_key) == 2:
                _, cat_name = cache_key
                if isinstance(cat_data, dict) and cat_data.get("pending_tag_assignment", False):
                    # This is a newly discovered category - try to find its extension
                    discovered_spaces = cat_data.get("discovered_in_spaces", [])
                    if discovered_spaces:
                        # Try to find extension by matching category name to panel bl_category
                        for space_type_str in discovered_spaces:
                            try:
                                import bpy
                                space_type = getattr(bpy.types, space_type_str, None)
                                if space_type and hasattr(space_type, 'panels'):
                                    for panel_cls in space_type.panels:
                                        if getattr(panel_cls, 'bl_category', '') == cat_name:
                                            module = getattr(panel_cls, '__module__', '')
                                            if module.startswith('bl_ext.'):
                                                module_parts = module.split('.')
                                                if len(module_parts) >= 3:
                                                    pkg_name = '.'.join(module_parts[2:])
                                                    try:
                                                        import importlib
                                                        ext_module = importlib.import_module(module)
                                                        pkg_path = os.path.dirname(getattr(ext_module, "__file__", ""))
                                                        category_debug_print(f"[DRAG-DROP FIX] Discovered pkg_path={pkg_path!r} for category {cat_name!r}")
                                                        break
                                                    except Exception:
                                                        pass
                                    if pkg_path:
                                        break
                            except Exception:
                                pass
                    if pkg_path:
                        break

    if pkg_path and os.path.isdir(pkg_path):
        try:
            # Scan icon once
            icon_path, icon_name = _scan_extension_icon_path(pkg_path)
            if icon_path:
                category_debug_print(f"[ICON SCAN] Found icon for pkg_path {pkg_path!r}: {icon_path!r}")
                # Use pkg_path as cache key if extension_id is empty
                cache_key_icon = extension_id if extension_id else pkg_path
                set_extension_install_icon_cache_entry(cache_key_icon, icon_path)
            else:
                category_debug_print(f"[ICON SCAN] No icon found for pkg_path {pkg_path!r}")

            # [POPULAR ADDONS DB] BEGIN - Fallback: try Popular Addons Database
            # Can be removed when extensions bundle their own icons.
            if POPULAR_ADDONS_DB_ENABLED:
                try:
                    from bl_ext.user_default.popular_addons_database import api as _pad_api
                    import re as _re_mod
                    # Build addon_id candidates from extension_id (ALWAYS try both strategies)
                    raw_id = extension_id if extension_id else ""
                    if raw_id.startswith("add-on-"):
                        raw_id = raw_id[7:]
                    _pad_candidates = []
                    # Strategy 1: repo.pkg format
                    if "." in raw_id:
                        _pkg = raw_id.rsplit(".", 1)[-1]
                        if not _pkg[0:1].isdigit():
                            _pad_candidates.append(_pkg)
                    # Strategy 2: strip version suffix (ALWAYS try)
                    stripped = _re_mod.sub(r'-v?\d+(\.\d+)*$', '', raw_id)
                    if stripped and stripped not in _pad_candidates:
                        _pad_candidates.append(stripped)
                        _us = stripped.replace("-", "_")
                        if _us not in _pad_candidates:
                            _pad_candidates.append(_us)
                    # Strategy 3: raw_id as fallback
                    if raw_id not in _pad_candidates:
                        _pad_candidates.append(raw_id)

                    for _q_id in _pad_candidates:
                        result = _pad_api.query_popular_addon_icon(_q_id)
                        if result:
                            pad_icon_path = result.get("icon_path", "")
                            if pad_icon_path and os.path.isfile(pad_icon_path):
                                category_debug_print(f"[ICON SCAN] Popular Addons Database icon for {_q_id!r}: {pad_icon_path!r}")
                                cache_key_icon = extension_id if extension_id else pkg_path
                                set_extension_install_icon_cache_entry(cache_key_icon, pad_icon_path)
                            break
                except Exception as pad_ex:
                    category_debug_print(f"[ICON SCAN] Popular Addons Database lookup failed: {pad_ex}")
            # [POPULAR ADDONS DB] END

            # CRITICAL OPTIMIZATION: Scan Python files for bl_category ONCE at install time
            # Cache the result permanently (no expiration) to avoid repeated scanning
            if not pkg_name:
                pkg_name = extension_id.replace("add-on-", "").replace(".", "_") if extension_id else ""
            category_debug_print(f"[BL_CATEGORY SCAN] Scanning Python files for bl_category at install time: pkg_path={pkg_path!r}, pkg_name={pkg_name!r}")

            # Always scan Python files at install time (regardless of is_install_from_disk)
            # This is a one-time operation that happens only when extension is installed
            manifest_keys = _extension_manifest_match_keys(pkg_path, pkg_name, scan_python_files=True)

            # Cache permanently with no expiration (scan only once ever)
            cache_key_ext = extension_id if extension_id else pkg_path
            cache_key = (cache_key_ext, True)  # True = scanned Python files
            state.extension_manifest_keys_cache[cache_key] = {
                "keys": manifest_keys,
                "timestamp": time.time(),
                "pkg_path": pkg_path,
                "scanned_at_install": True,  # Mark as scanned at install
            }
            category_debug_print(f"[BL_CATEGORY SCAN] CACHED {len(manifest_keys)} keys for {cache_key_ext!r}: {manifest_keys}")

        except Exception as e:
            category_debug_print(f"[INSTALL SCAN] Failed to scan extension: {e}")

    # Opportunistically mark already-known categories from this extension as pending.
    # This is needed when the category already exists in the cache but is not rediscovered
    # in the current draw/update cycle (for example MPFB-like extensions).
    ext_match_keys = _extension_id_match_keys(extension_id)
    updated_existing = False
    for cache_key, cat_data in list(state.glyph_cache.items()):
        if not (isinstance(cache_key, tuple) and len(cache_key) == 2):
            continue
        _cache_space_type, category_name = cache_key
        if not isinstance(cat_data, dict):
            cat_data = _normalize_category_data(cat_data, category_name)
            state.glyph_cache[cache_key] = cat_data

        existing_ext = str(cat_data.get("source_extension", "") or "")
        category_key = _normalize_category_key(category_name)
        same_extension = (existing_ext == extension_id) or _extension_ids_match(existing_ext, extension_id)
        category_matches_pending_context = (
            bool(category_key) and bool(ext_match_keys) and (category_key in ext_match_keys)
        )

        if not (same_extension or category_matches_pending_context):
            continue

        # Already tagged categories must not surface in "New Add-ons!".
        if cat_data.get("tags"):
            cat_data["pending_tag_assignment"] = False
            continue

        if not existing_ext:
            cat_data["source_extension"] = extension_id

        # Only mark as pending if it hasn't been explicitly addressed yet (False).
        if cat_data.get("pending_tag_assignment", True):
            cat_data["pending_tag_assignment"] = True

        if space_type != -1:
            current_spaces = cat_data.get("discovered_in_spaces", [])
            if current_spaces is None:
                current_spaces = []
            spaces_flags = spaces_to_flags(current_spaces)
            space_str = _space_type_id_to_str(space_type)
            spaces_flags |= SPACE_TO_FLAG.get(space_str, 0)
            cat_data["discovered_in_spaces"] = flags_to_spaces(spaces_flags)

        if mode_flag:
            current_modes = cat_data.get("discovered_in_modes", [])
            if current_modes is None:
                current_modes = []
            modes_flags = modes_to_flags(current_modes)
            modes_flags |= mode_flag
            cat_data["discovered_in_modes"] = flags_to_modes(modes_flags)

        updated_existing = True
        tag_log(
            f"extension_post_install_handler: pre-marked existing category {category_name!r} "
            f"for extension {extension_id!r}"
        )

    if updated_existing:
        category_debug_print(f"[EXTENSION POST INSTALL] Updated {len([k for k, v in state.glyph_cache.items() if isinstance(k, tuple) and v.get('pending_tag_assignment')])} existing categories to pending")

    category_debug_print(f"[EXTENSION POST INSTALL] Final cache state: {len(state.glyph_cache)} total categories")
    category_debug_print(f"[EXTENSION POST INSTALL] Pending extension context set: {get_pending_extension_context()}")

    tag_log(
        f"extension_post_install_handler: extension={extension_id!r}, "
        f"space_type={space_type}, mode_flag={mode_flag:#010x}, set at {time.time()}"
    )

    category_debug_print(f"[EXTENSION POST INSTALL] <<< END handler for {extension_id!r}")

    # Caller (bl_pkg/bl_extension_ops.py, via the api facade) is responsible for pushing
    # the update to WM and persisting it — see api.sync_and_save_after_extension_install().
    # This keeps discovery_merge a leaf module with no dependency on wm_sync/persistence.
    return updated_existing


# -----------------------------------------------------------------------------
# Discovery / cache merge
# -----------------------------------------------------------------------------

def _merge_discovered_categories(force_refresh=False, skip_icon_detection=False):
    """Merge discovered categories with cached mappings, adding defaults for new ones.

    OPTIMIZATION: Added caching and debouncing to prevent repeated scanning during UI draws.
    OPTIMIZATION: skip_icon_detection parameter allows deferring icon detection to background sync.
    """
    # Live reads of reassignable _state names (avoid stale import-time bindings).
    _merge_discovery_cache = get_merge_discovery_cache()
    _pending_extension_context = get_pending_extension_context()
    _install_from_disk_just_occurred = is_install_from_disk_just_occurred()
    _in_ui_draw = is_in_ui_draw()
    _discovery_debounce_timer = get_discovery_debounce_timer()

    # PERF: Start timing
    _merge_start_time = time.perf_counter()

    # OPTIMIZATION: Skip if cache is valid and not forced refresh
    current_time = time.time()

    # Check if we should use cached results
    if not force_refresh and _merge_discovery_cache.get("cache_valid", False):
        cache_age = current_time - _merge_discovery_cache.get("last_check_time", 0)

        # Use cache if it's fresh (less than DEBOUNCE_INTERVAL seconds old)
        if cache_age < _DISCOVERY_DEBOUNCE_INTERVAL:
            category_debug_print(f"[MERGE OPTIMIZATION] Using cached discovery results (age={cache_age:.2f}s)")
            return False

        # Check if addon/extension count changed
        try:
            prefs = getattr(bpy.context, "preferences", None)
            addons = getattr(prefs, "addons", None) if prefs else None
            addon_count = len(addons) if addons else 0

            extensions_dir = bpy.utils.user_resource('EXTENSIONS') if bpy.utils.user_resource('EXTENSIONS') else ""
            extension_count = 0
            if extensions_dir and os.path.isdir(extensions_dir):
                extension_count = sum(1 for repo in os.listdir(extensions_dir)
                                     if os.path.isdir(os.path.join(extensions_dir, repo))
                                     and not repo.startswith('.'))

            # Use cache if counts haven't changed
            if (addon_count == _merge_discovery_cache.get("last_addon_count", 0) and
                extension_count == _merge_discovery_cache.get("last_extension_count", 0)):
                category_debug_print(f"[MERGE OPTIMIZATION] Using cached results (addon_count={addon_count}, ext_count={extension_count})")
                _merge_discovery_cache["last_check_time"] = current_time
                return False
        except Exception:
            pass  # Continue with merge if we can't check counts

    # OPTIMIZATION: Skip heavy operations during UI draw calls
    if is_in_ui_draw() and not force_refresh:
        category_debug_print(f"[MERGE OPTIMIZATION] Deferring merge - in UI draw call")
        # Schedule deferred merge if not already scheduled
        if get_discovery_debounce_timer() is None:
            def deferred_merge():
                set_discovery_debounce_timer(None)
                # Skip icon detection in deferred UI merge (will run in background)
                _merge_discovered_categories(force_refresh=True, skip_icon_detection=True)
                return None

            set_discovery_debounce_timer(deferred_merge)
            try:
                bpy.app.timers.register(deferred_merge, first_interval=_DISCOVERY_DEBOUNCE_INTERVAL)
                category_debug_print(f"[MERGE OPTIMIZATION] Scheduled deferred merge in {_DISCOVERY_DEBOUNCE_INTERVAL}s")
            except Exception:
                pass  # Timer registration failed, continue with merge
        return False

    category_debug_print(f"[MERGE DEBUG] _merge_discovered_categories START (force_refresh={force_refresh})")
    category_debug_print(f"[MERGE DEBUG] Pending extension context: {_pending_extension_context}")
    category_debug_print(f"[MERGE DEBUG] Current cache size before merge: {len(state.glyph_cache)} categories")
    category_debug_print(f"[MERGE DEBUG] Cache keys sample: {list(state.glyph_cache.keys())[:10]}")

    # OPTIMIZATION: Clear session-level icon detection tracker for fresh merge
    reset_icon_detection_session_checked()
    category_debug_print(f"[ICON SESSION CACHE] Cleared session tracker for new merge")

    result = _discover_active_categories()

    # Handle both old tuple return and new tuple return
    if isinstance(result, tuple):
        discovered, panel_samples = result
    else:
        discovered = result
        panel_samples = []

    category_debug_print(f"[MERGE DEBUG] Discovered {len(discovered) if discovered else 0} categories")
    if discovered:
        category_debug_print(f"[MERGE DEBUG] Discovered categories sample: {sorted(discovered)[:10]}")

    if not discovered:
        category_debug_print(f"[MERGE DEBUG] No categories discovered, returning False")
        return False

    # Build a mapping from category name to discovered space types from panel_samples
    # This is crucial for correctly setting discovered_in_spaces for extension categories
    # that may appear in different space types than where the extension was dropped
    category_to_spaces = {}  # category -> set of space_type strings
    category_to_contexts = {}  # category -> set of bl_context strings (for mode filtering)
    for source, panel_name, category, space_type, region_type, panel_label, bl_context in panel_samples:
        if category and space_type:
            if category not in category_to_spaces:
                category_to_spaces[category] = set()
            category_to_spaces[category].add(space_type)
            if bl_context:
                if category not in category_to_contexts:
                    category_to_contexts[category] = set()
                category_to_contexts[category].add(bl_context)

    # Debug: Log category_to_spaces for categories with NODE_EDITOR
    node_editor_categories = [cat for cat, spaces in category_to_spaces.items() if 'NODE_EDITOR' in spaces]
    if node_editor_categories:
        _log_once(f"[CATEGORY_SPACES] Found {len(node_editor_categories)} categories with NODE_EDITOR: {node_editor_categories[:10]}")
    else:
        _log_once(f"[CATEGORY_SPACES] WARNING: No categories found with NODE_EDITOR space type!")
        # Log some sample categories to see what space types we have
        sample_cats = list(category_to_spaces.items())[:5]
        for cat, spaces in sample_cats:
            _log_once(f"[CATEGORY_SPACES] Sample: {cat!r} -> {spaces}")

    # Build a mapping from category name to panel label (for display name)
    category_to_label = {}
    for source, panel_name, category, space_type, region_type, panel_label, bl_context in panel_samples:
        if panel_label and category not in category_to_label:
            category_to_label[category] = panel_label

    # Also lookup panel labels for glyph_only categories that may not be in panel_samples
    # This is important because glyph_only categories (e.g., "" for "Script 4")
    # need their display_name to be discovered from panel bl_label
    for category in discovered:
        if _is_single_glyph(category) and category not in category_to_label:
            panel_label = _find_panel_label_for_category(category)
            if panel_label:
                category_to_label[category] = panel_label
                category_debug_print(f"[GLYPH DISCOVER] Found panel label for glyph_only category: {category!r} -> {panel_label!r}")

    discovered_source_map = dict(state.last_discovered_category_sources)

    def _clone_category_data(data):
        if not isinstance(data, dict):
            return _normalize_category_data({})
        cloned = dict(data)
        if isinstance(cloned.get("color"), list):
            cloned["color"] = list(cloned["color"])
        if isinstance(cloned.get("tags"), list):
            cloned["tags"] = list(cloned["tags"])
        return cloned

    def _is_default_color(color):
        return not isinstance(color, list) or color == [0.0, 0.0, 0.0]

    def _get_category_names_from_cache():
        """Extract unique category names from cache keys (tuples: (space_type, category))."""
        names = set()
        for key in state.glyph_cache.keys():
            if isinstance(key, tuple) and len(key) == 2:
                names.add(key[1])  # category name
            else:
                names.add(key)  # fallback for old string keys
        return names

    def _get_cache_key_for_category(category_name):
        """Get cache key for a category (default to global: space_type=-1)."""
        return _make_cache_key(-1, category_name)

    def _merge_alias_into_canonical(canonical_name, alias_name):
        if canonical_name == alias_name:
            return False
        canonical_key = _get_cache_key_for_category(canonical_name)
        alias_key = _get_cache_key_for_category(alias_name)
        canonical_data = state.glyph_cache.get(canonical_key)
        alias_data = state.glyph_cache.get(alias_key)
        if alias_data is None:
            return False

        changed = False
        if canonical_data is None:
            state.glyph_cache[canonical_key] = _clone_category_data(alias_data)
            canonical_data = state.glyph_cache[canonical_key]
            changed = True

        # Prefer keeping existing canonical values, but fill missing important fields from alias.
        for field_name in ("glyph", "display_name", "icon_path", "icon_provider", "icon_source", "icon_key"):
            if not canonical_data.get(field_name) and alias_data.get(field_name):
                canonical_data[field_name] = alias_data.get(field_name)
                changed = True

        if _is_default_color(canonical_data.get("color")) and isinstance(alias_data.get("color"), list) and not _is_default_color(alias_data.get("color")):
            canonical_data["color"] = list(alias_data.get("color"))
            changed = True

        canonical_tags = canonical_data.get("tags", []) if isinstance(canonical_data.get("tags"), list) else []
        alias_tags = alias_data.get("tags", []) if isinstance(alias_data.get("tags"), list) else []
        merged_tags = []
        for tag in canonical_tags + alias_tags:
            if tag not in merged_tags:
                merged_tags.append(tag)
        if merged_tags != canonical_tags:
            canonical_data["tags"] = merged_tags
            changed = True

        # Remap category references in persisted category orders.
        for tag_key, order_list in state.category_orders_cache.items():
            if not isinstance(order_list, list):
                continue

            remapped = [canonical_name if category == alias_name else category for category in order_list]
            deduped = []
            for category in remapped:
                if category not in deduped:
                    deduped.append(category)
            if deduped != order_list:
                state.category_orders_cache[tag_key] = deduped
                changed = True

        if alias_key in state.glyph_cache:
            del state.glyph_cache[alias_key]
        _log_once(
            f"[GLYPH DISCOVER DEBUG] canonicalized alias in cache: alias={alias_name!r} -> canonical={canonical_name!r}"
        )
        return True

    # Group discovered names by normalized key and choose one canonical key per group.
    discovered_groups = {}
    for category in discovered:
        normalized_key = _normalize_category_key(category) or category
        discovered_groups.setdefault(normalized_key, []).append(category)

    canonical_by_group = {
        group_key: _pick_canonical_category_name(candidates, discovered_source_map)
        for group_key, candidates in discovered_groups.items()
    }

    # Get unique category names from cache (extract from tuple keys)
    cached_category_names = _get_category_names_from_cache()
    _pref_log_once(f"[GLYPH MERGE DEBUG] cached_category_names count: {len(cached_category_names)}")
    _pref_log_once(f"[GLYPH MERGE DEBUG] discovered count: {len(discovered)}")
    _pref_log_once(f"[GLYPH MERGE DEBUG] 'Brushstroke Tools' in cached: {'Brushstroke Tools' in cached_category_names}")
    if 'Brushstroke Tools' in cached_category_names:
        brush_key = _make_cache_key(-1, 'Brushstroke Tools')
        _pref_log_once(f"[GLYPH MERGE DEBUG] Brushstroke cache key: {brush_key}")
        _pref_log_once(f"[GLYPH MERGE DEBUG] Brushstroke in state.glyph_cache: {brush_key in state.glyph_cache}")
        if brush_key in state.glyph_cache:
            _pref_log_once(f"[GLYPH MERGE DEBUG] Brushstroke cached data: {state.glyph_cache[brush_key]}")

    # Existing cache aliases for active discovered groups are merged into the canonical entry.
    cache_changed = False
    for group_key, canonical_name in canonical_by_group.items():
        aliases_in_cache = [
            cache_name
            for cache_name in cached_category_names
            if (_normalize_category_key(cache_name) or cache_name) == group_key and cache_name != canonical_name
        ]
        for alias_name in aliases_in_cache:
            cache_changed = _merge_alias_into_canonical(canonical_name, alias_name) or cache_changed

    # Find categories that are in the discovered set but not in cache
    new_categories = discovered - cached_category_names
    _pref_log_once(f"[GLYPH MERGE DEBUG] new_categories count: {len(new_categories)}")
    _pref_log_once(f"[GLYPH MERGE DEBUG] 'Brushstroke Tools' in new_categories: {'Brushstroke Tools' in new_categories}")

    # Canonicalize only new candidates: one mapping per normalized key.
    canonical_new_categories = []
    suppressed_aliases = []
    for group_key, candidates in discovered_groups.items():
        if not any(candidate in new_categories for candidate in candidates):
            continue

        canonical_name = canonical_by_group[group_key]
        # Check if category exists in cache (using global key)
        canonical_key = _get_cache_key_for_category(canonical_name)
        if canonical_key not in state.glyph_cache:
            canonical_new_categories.append(canonical_name)

        for candidate in candidates:
            if candidate in new_categories and candidate != canonical_name:
                suppressed_aliases.append((candidate, canonical_name))

    new_categories = set(canonical_new_categories)

    if suppressed_aliases:
        _log_once(f"[GLYPH DISCOVER DEBUG] suppressed {len(suppressed_aliases)} alias categories during merge")
        for alias_name, canonical_name in sorted(suppressed_aliases)[:5]:  # Reduce from 40 to 5 samples
            _log_once(
                f"[GLYPH DISCOVER DEBUG] alias suppressed: alias={alias_name!r}, canonical={canonical_name!r}"
            )

    # Update icon_path for existing categories with icon_source='auto' and no icon_path
    # This ensures that extension icons are detected even for categories that were cached before
    # OPTIMIZATION: Use cached icon path from extension_post_install_handler if available
    # OPTIMIZATION: Session-level tracking to avoid redundant icon checks
    # OPTIMIZATION: Skip icon detection entirely if skip_icon_detection=True
    existing_categories_needing_icon_update = []
    icon_cache = state.extension_install_icon_cache

    if not skip_icon_detection:
        for category in discovered:
            if category in new_categories:
                continue  # Skip new categories, they are handled below
            cache_key = _get_cache_key_for_category(category)
            if cache_key in state.glyph_cache:
                cached_data = state.glyph_cache[cache_key]
                if isinstance(cached_data, dict):
                    icon_source = cached_data.get("icon_source", "auto")
                    icon_path = cached_data.get("icon_path", "")
                    if icon_source == "auto" and not icon_path:
                        # OPTIMIZATION: Skip if already checked in this session
                        if category in state.icon_detection_session_checked:
                            category_debug_print(f"[ICON SESSION CACHE] Skipping already-checked category {category!r}")
                            continue

                        # Try to get extension_id and use cached icon path
                        detected_icon_path = None
                        detected_provider = None
                        ext_id = cached_data.get("source_extension", "")

                        if ext_id and ext_id in icon_cache:
                            detected_icon_path = icon_cache[ext_id]
                            detected_provider = "extension_auto"
                            category_debug_print(f"[ICON CACHE] Using cached icon for existing category {category!r}: {detected_icon_path!r}")
                        elif ext_id:
                            # Try alternate extension_id formats in icon_cache
                            for _alt_pfx in ("add-on-blender_org.", "add-on-user_default.", "add-on-"):
                                _alt_id = _alt_pfx + ext_id.replace("add-on-", "")
                                if _alt_id in icon_cache:
                                    detected_icon_path = icon_cache[_alt_id]
                                    detected_provider = "extension_auto"
                                    category_debug_print(f"[ICON CACHE] Using cached icon (alt key {_alt_id!r}) for existing category {category!r}: {detected_icon_path!r}")
                                    break

                        # [POPULAR ADDONS DB] BEGIN - Fallback: try Popular Addons Database by extension_id or category name
                        # Can be removed when extensions bundle their own icons.
                        if POPULAR_ADDONS_DB_ENABLED and not detected_icon_path:
                            try:
                                from bl_ext.user_default.popular_addons_database import api as _pad_api
                                import re as _re_mod
                                _pad_candidates = []
                                if ext_id:
                                    raw_id = ext_id
                                    if raw_id.startswith("add-on-"):
                                        raw_id = raw_id[7:]
                                    # Strategy 1: repo.pkg format
                                    if "." in raw_id:
                                        _pkg = raw_id.rsplit(".", 1)[-1]
                                        if not _pkg[0:1].isdigit():
                                            _pad_candidates.append(_pkg)
                                    # Strategy 2: strip version suffix (ALWAYS try)
                                    stripped = _re_mod.sub(r'-v?\d+(\.\d+)*$', '', raw_id)
                                    if stripped and stripped not in _pad_candidates:
                                        _pad_candidates.append(stripped)
                                        _us = stripped.replace("-", "_")
                                        if _us not in _pad_candidates:
                                            _pad_candidates.append(_us)
                                    # Strategy 3: raw_id fallback
                                    if raw_id not in _pad_candidates:
                                        _pad_candidates.append(raw_id)
                                # Strategy 4: category name variants (original case + lowercase + separators)
                                for _ck in (category, category.lower(), category.lower().replace(" ", ""), category.lower().replace(" ", "_"), category.lower().replace(" ", "-")):
                                    if _ck not in _pad_candidates:
                                        _pad_candidates.append(_ck)
                                for _q_id in _pad_candidates:
                                    result = _pad_api.query_popular_addon_icon(_q_id)
                                    if result:
                                        pad_icon_path = result.get("icon_path", "")
                                        if pad_icon_path and os.path.isfile(pad_icon_path):
                                            detected_icon_path = pad_icon_path
                                            detected_provider = "popular_addons_database"
                                            category_debug_print(f"[POPULAR ADDONS] Icon for existing {category!r} via {_q_id!r}: {pad_icon_path!r}")
                                        break
                            except Exception as pad_ex:
                                category_debug_print(f"[POPULAR ADDONS] Lookup failed: {pad_ex}")
                        # [POPULAR ADDONS DB] END

                        # Fallback to disk scan only if no cached icon
                        if not detected_icon_path:
                            detected_icon_path, detected_provider = _auto_detect_extension_icon_path(category)

                        # Mark as checked in this session
                        state.icon_detection_session_checked.add(category)

                        if detected_icon_path:
                            cached_data["icon_path"] = detected_icon_path
                            cached_data["icon_provider"] = detected_provider or "extension_auto"
                            cache_changed = True
                            existing_categories_needing_icon_update.append(category)
                            category_debug_print(
                                f"[] update existing category auto-icon: "
                                f"category={category!r}, path={detected_icon_path!r}, provider={cached_data['icon_provider']!r}"
                            )
    else:
        category_debug_print("[ICON DETECTION] Skipping icon detection for faster startup (will run in background)")

    if existing_categories_needing_icon_update:
        category_debug_print(f"[GLYPH] Updated icons for {len(existing_categories_needing_icon_update)} existing categories")

    # Update default_display_name for existing glyph_only categories that are missing it
    # This happens when categories were created before panel labels were discovered
    existing_glyph_only_needing_name_update = []
    for category in discovered:
        if category in new_categories:
            continue  # Skip new categories, they are handled below
        cache_key = _get_cache_key_for_category(category)
        if cache_key in state.glyph_cache:
            cached_data = state.glyph_cache[cache_key]
            if isinstance(cached_data, dict):
                base_type = cached_data.get("base_type", "text_only")
                default_display_name = cached_data.get("default_display_name", "")
                # Only update glyph_only categories that are missing default_display_name
                if base_type == "glyph_only" and not default_display_name:
                    panel_label = category_to_label.get(category, "")
                    if panel_label:
                        cached_data["default_display_name"] = panel_label
                        # Also update display_name if it's empty
                        if not cached_data.get("display_name", ""):
                            cached_data["display_name"] = panel_label
                        cache_changed = True
                        existing_glyph_only_needing_name_update.append(category)
                        category_debug_print(
                            f"[GLYPH] Updated default_display_name for existing glyph_only category: "
                            f"category={category!r}, default_display_name={panel_label!r}"
                        )

    if existing_glyph_only_needing_name_update:
        category_debug_print(f"[GLYPH] Updated default_display_name for {len(existing_glyph_only_needing_name_update)} existing glyph_only categories")

    pending_extension_context = _pending_extension_context
    if pending_extension_context is not None:
        pending_ext_id = str(pending_extension_context.get("extension_id", "") or "").strip()
        category_debug_print(f"[MERGE DEBUG] Processing pending extension context: extension_id={pending_ext_id!r}")
        if not pending_ext_id:
            # Don't drop pending context for empty extension_id - this happens during drag-and-drop from file explorer
            # The fallback logic will try to match categories using state.last_discovered_ext_panel_categories
            tag_log("_merge_discovered_categories: pending extension context has empty extension_id - will use fallback matching")

    # Update existing categories if there is a pending extension context.
    # IMPORTANT: only touch categories that are already linked to this same extension,
    # and never mass-enable pending_tag_assignment for unrelated existing categories.
    existing_categories_updated_from_context = False
    if pending_extension_context is not None:
        ext_id = pending_extension_context.get("extension_id", "")
        ext_mode = pending_extension_context.get("mode_flag", 0)
        ext_match_keys = _extension_id_match_keys(ext_id)
        category_debug_print(f"[MERGE DEBUG] Processing {len(discovered)} discovered categories for extension {ext_id!r}")

        for category in discovered:
            if category in new_categories:
                continue
            # Skip RESERVED categories - they should never be linked to extensions
            if category in DEFAULT_CATEGORY_GLYPHS:
                continue
            cache_key = _get_cache_key_for_category(category)
            if cache_key in state.glyph_cache:
                cat_data = state.glyph_cache[cache_key]
                if isinstance(cat_data, dict):
                    existing_ext = cat_data.get("source_extension", "")
                    category_key = _normalize_category_key(category)

                    same_extension = (existing_ext == ext_id) or _extension_ids_match(existing_ext, ext_id)
                    category_matches_pending_context = (
                        bool(category_key) and
                        bool(ext_match_keys) and
                        (category_key in ext_match_keys)
                    )
                    can_claim_unowned = (not existing_ext) and category_matches_pending_context

                    # Refresh existing categories for the same extension, or claim only
                    # an unowned category whose normalized name matches this extension id.
                    if same_extension or can_claim_unowned or category_matches_pending_context:
                        category_debug_print(f"[MERGE DEBUG] MATCH: category={category!r}, existing_ext={existing_ext!r}, same_extension={same_extension}, can_claim_unowned={can_claim_unowned}")
                        if not existing_ext:
                            cat_data["source_extension"] = ext_id

                        # Check if category has tags in ANY space before setting pending
                        has_tags_current = bool(cat_data.get("tags", []))
                        has_tags_any_space = has_tags_current
                        if not has_tags_any_space:
                            for check_key, check_data in state.glyph_cache.items():
                                if isinstance(check_key, tuple) and len(check_key) == 2:
                                    check_cat = check_key[1]
                                    if isinstance(check_data, dict):
                                        is_same_cat = (check_cat == category and check_key[0] != -1)
                                        is_same_ext = bool(ext_id) and check_data.get("source_extension") == ext_id
                                        if is_same_cat or is_same_ext:
                                            check_tags = check_data.get("tags", [])
                                            if check_tags:
                                                has_tags_any_space = True
                                                category_debug_print(f"[MERGE DEBUG] Found tags in space {check_key[0]} for category {check_cat!r} (same cat/ext): {check_tags}")
                                                break

                        # Check if this was a tab drop (tag_already_assigned=True)
                        # For tab drops, don't set pending_tag_assignment since the category
                        # will be visible in the general list without tag filtering.
                        tag_already_assigned = pending_extension_context.get("tag_already_assigned", False) if pending_extension_context else False

                        # Check if user already processed this category (assigned tag or "Without Tag").
                        # pending_tag_assignment=False means user explicitly cleared it — don't override.
                        user_already_processed = "pending_tag_assignment" in cat_data and cat_data["pending_tag_assignment"] is False

                        if not cat_data.get("pending_tag_assignment") and not has_tags_any_space and not tag_already_assigned and not user_already_processed:
                            cat_data["pending_tag_assignment"] = True
                            category_debug_print(f"[MERGE DEBUG] SET pending_tag_assignment=True for {category!r} (first-time discovery)")
                        elif user_already_processed:
                            category_debug_print(f"[MERGE DEBUG] SKIP re-enable for {category!r}: user already processed (pending_tag_assignment=False)")
                        elif tag_already_assigned:
                            category_debug_print(f"[MERGE DEBUG] TAB DROP EXISTING: category={category!r}, tag_already_assigned=True, NOT setting pending_tag_assignment")
                        elif has_tags_any_space:
                            category_debug_print(f"[MERGE DEBUG] SKIP pending for {category!r}: has_tags_any_space={has_tags_any_space}")

                        # Use ACTUAL discovered spaces from panel_samples, NOT ext_space from drop context.
                        # This is crucial for extensions like Hot Node that are dropped in VIEW3D but
                        # register panels in Node Editor - the category must be tagged for NODE_EDITOR,
                        # not VIEW3D.
                        category_spaces = category_to_spaces.get(category, set())
                        if category_spaces:
                            current_spaces = cat_data.get("discovered_in_spaces", [])
                            if current_spaces is None: current_spaces = []
                            spaces_flags = spaces_to_flags(current_spaces)
                            for space_str in category_spaces:
                                spaces_flags |= SPACE_TO_FLAG.get(space_str, 0)
                            cat_data["discovered_in_spaces"] = flags_to_spaces(spaces_flags)
                            tag_log(
                                f"_merge_discovered_categories: updated discovered_in_spaces for {category!r} "
                                f"to {cat_data['discovered_in_spaces']} (from panel_samples)"
                            )

                        # Update discovered_in_modes from bl_context
                        category_contexts = category_to_contexts.get(category, set())
                        if category_contexts:
                            current_modes = cat_data.get("discovered_in_modes", [])
                            if current_modes is None: current_modes = []
                            modes_flags = modes_to_flags(current_modes)
                            for bl_ctx in category_contexts:
                                modes_flags |= BL_CONTEXT_TO_MODE_FLAG.get(bl_ctx, 0)
                            if modes_flags:
                                cat_data["discovered_in_modes"] = flags_to_modes(modes_flags)
                                tag_log(
                                    f"_merge_discovered_categories: updated discovered_in_modes for {category!r} "
                                    f"to {cat_data['discovered_in_modes']} (from bl_context={category_contexts})"
                                )

                        if ext_mode and not cat_data.get("discovered_in_modes"):
                            current_modes = []
                            modes_flags = modes_to_flags(current_modes)
                            modes_flags |= ext_mode
                            cat_data["discovered_in_modes"] = flags_to_modes(modes_flags)

                        cache_changed = True
                        existing_categories_updated_from_context = True
                        tag_log(
                            f"_merge_discovered_categories: updated EXISTING category {category!r} "
                            f"for extension {ext_id!r} (claimed={can_claim_unowned})"
                        )

    # NOTE: Existing-category auto-repair (source_extension backfill) lives in this block.
    # It must run even when there are no newly discovered categories, otherwise
    # orphan categories like "Edge Length" are never linked to their extension.
    if new_categories or pending_extension_context is None:
        category_debug_print(f"[GLYPH] Found {len(new_categories)} new categories: {sorted(new_categories)}")
        category_debug_print(f"[NEW CATEGORIES DEBUG] Processing new categories with pending_extension_context: {pending_extension_context}")
        category_debug_print(f"[NEW CATEGORIES DEBUG] Cache state before processing: {len(state.glyph_cache)} total categories")

        # Auto-detect extension for EACH category individually from enabled extensions.
        # This handles "Install from Disk" and startup discovery where pending_extension_context is not set.
        # IMPORTANT: Each category gets its own source_extension based on name matching,
        # NOT a global context that would incorrectly assign all categories to one extension.
        category_to_auto_extension = {}  # Maps category name to detected extension_id
        enabled_extensions = {}  # Map of extension_id -> {module_name, pkg_name, match_keys}
        if pending_extension_context is None:
            try:
                prefs = getattr(bpy.context, "preferences", None)
                addons = getattr(prefs, "addons", None) if prefs else None
                if addons:
                    # Build map of all enabled extensions for quick lookup
                    for addon in addons:
                        module_name = getattr(addon, "module", "")
                        if not isinstance(module_name, str) or not module_name.startswith("bl_ext."):
                            continue
                        module_parts = module_name.split(".")
                        if len(module_parts) < 3:
                            continue
                        pkg_name = ".".join(module_parts[2:])
                        ext_id = f"add-on-{pkg_name}"
                        enabled_extensions[ext_id] = {
                            "module_name": module_name,
                            "pkg_name": pkg_name,
                            "match_keys": _extension_id_match_keys(ext_id),
                        }

                    # For EACH category, find its matching extension individually
                    for category in new_categories:
                        # Skip RESERVED categories - they should never be linked to extensions
                        if category in DEFAULT_CATEGORY_GLYPHS:
                            continue
                        category_key = _normalize_category_key(category)
                        if not category_key:
                            continue

                        # Check if category matches any enabled extension
                        for ext_id, ext_info in enabled_extensions.items():
                            if category_key in ext_info["match_keys"]:
                                category_debug_print(f"[MERGE AUTO-EXT] Auto-detected extension for new category '{category}': ext_id={ext_id!r}")
                                category_to_auto_extension[category] = ext_id
                                break
                            # Prefix matching: "mpfbv2014" should match "mpfb" from "add-on-mpfb"
                            for ext_key in ext_info["match_keys"]:
                                if category_key.startswith(ext_key) and len(category_key) > len(ext_key):
                                    category_debug_print(f"[MERGE AUTO-EXT] Auto-detected extension for new category '{category}' via prefix: ext_id={ext_id!r} (key={ext_key!r})")
                                    category_to_auto_extension[category] = ext_id
                                    break
                            if category in category_to_auto_extension:
                                break
            except Exception as e:
                category_debug_print(f"[MERGE AUTO-EXT] Error auto-detecting extension: {e}")

        # Also update EXISTING categories without source_extension that match enabled extensions
        # This handles cases where category was discovered before extension context was available
        # (e.g., Hot Node installed via Get Extensions before pending logic was active)
        # CRITICAL FIX: Also repair categories that have pending_tag_assignment=false but NO source_extension.
        # These are orphaned categories from incomplete discovery (e.g., "MPFB v2.0.14", "Mixamo").
        # FIX: Use _extension_manifest_match_keys() to scan Python files for bl_category values
        # (e.g., "Edge Length Measure" extension uses bl_category="Edge Length" which differs from manifest name).
        try:
            prefs = getattr(bpy.context, "preferences", None)
            addons = getattr(prefs, "addons", None) if prefs else None
            if addons and enabled_extensions:
                category_debug_print(f"[MERGE AUTO-EXT DEBUG] Checking {len(state.glyph_cache)} cache entries for extension matching")
                category_debug_print(f"[MERGE AUTO-EXT DEBUG] enabled_extensions: {list(enabled_extensions.keys())}")

                # CRITICAL OPTIMIZATION: NEVER scan Python files during merge/discovery
                # Python files are scanned ONLY once at extension install time in extension_post_install_handler()
                # This eliminates the main cause of UI freezes during Get Extension/Add-ons panel draws
                extension_manifest_keys = {}
                for ext_id, ext_info in enabled_extensions.items():
                    # Always use cached keys (scanned at install time)
                    # Use cache key with True (scanned Python files) to get the install-time scan results
                    cache_key = (ext_id, True)

                    if cache_key in state.extension_manifest_keys_cache:
                        cache_entry = state.extension_manifest_keys_cache[cache_key]
                        # Use cached keys regardless of age - they were scanned at install and never change
                        extension_manifest_keys[ext_id] = cache_entry["keys"]
                        category_debug_print(f"[MERGE AUTO-EXT OPTIMIZATION] Using INSTALL-TIME cached manifest keys for {ext_id!r}: {cache_entry['keys']}")
                    else:
                        # Fallback: build keys from manifest ONLY (no Python file scanning)
                        # This handles extensions installed before this optimization was added
                        pkg_name = ext_info["pkg_name"]
                        module_name = ext_info["module_name"]
                        category_debug_print(f"[MERGE AUTO-EXT OPTIMIZATION] No install-time cache for {ext_id!r}, building from manifest only (no Python scan)")
                        try:
                            import importlib
                            module = importlib.import_module(module_name)
                            pkg_path = os.path.dirname(getattr(module, "__file__", ""))
                            if pkg_path and os.path.isdir(pkg_path):
                                # NEVER scan Python files here - use manifest fields only
                                keys = _extension_manifest_match_keys(pkg_path, pkg_name, scan_python_files=False)
                                extension_manifest_keys[ext_id] = keys
                                category_debug_print(f"[MERGE AUTO-EXT OPTIMIZATION] Built manifest-only keys for {ext_id!r}: {keys}")
                            else:
                                extension_manifest_keys[ext_id] = set()
                        except Exception as e:
                            extension_manifest_keys[ext_id] = set()
                            category_debug_print(f"[MERGE AUTO-EXT OPTIMIZATION] Failed to build keys for {ext_id!r}: {e}")

                for cache_key, cat_data in list(state.glyph_cache.items()):
                    if not isinstance(cache_key, tuple) or len(cache_key) != 2:
                        continue
                    space_type_val, category_name = cache_key
                    if space_type_val != -1:  # Only check global entries
                        continue
                    if not isinstance(cat_data, dict):
                        continue
                    # Skip RESERVED categories - they should never be linked to extensions
                    if category_name in DEFAULT_CATEGORY_GLYPHS:
                        continue
                    # Skip if already has source_extension (already linked)
                    if cat_data.get("source_extension"):
                        continue
                    # Skip if has tags (already distributed) - these don't need source_extension
                    if cat_data.get("tags"):
                        continue
                    # IMPORTANT: Do NOT skip categories with pending_tag_assignment=false!
                    # Categories like "MPFB v2.0.14" may have pending=false but still need source_extension.
                    # We need to repair these orphaned categories so they appear in "New Add-ons!" filter.

                    # DEBUG: Log categories that pass initial filters
                    if "MPFB" in category_name or "Mixamo" in category_name or "Edge" in category_name:
                        category_debug_print(f"[MERGE AUTO-EXT DEBUG] Checking category '{category_name}': "
                                           f"pending={cat_data.get('pending_tag_assignment')}, "
                                           f"has_tags={bool(cat_data.get('tags'))}, "
                                           f"has_source_ext={bool(cat_data.get('source_extension'))}")

                    category_key = _normalize_category_key(category_name)
                    if not category_key:
                        continue

                    # DEBUG: Log normalized key for MPFB/Mixamo/Edge categories
                    if "MPFB" in category_name or "Mixamo" in category_name or "Edge" in category_name:
                        category_debug_print(f"[MERGE AUTO-EXT DEBUG] Normalized key for '{category_name}': '{category_key}'")
                        for ext_id, ext_info in enabled_extensions.items():
                            if "mpfb" in ext_id.lower() or "mixamo" in ext_id.lower() or "edge" in ext_id.lower():
                                category_debug_print(f"[MERGE AUTO-EXT DEBUG] Extension {ext_id!r} match_keys: {ext_info['match_keys']}")
                                category_debug_print(f"[MERGE AUTO-EXT DEBUG]   Does '{category_key}' match? {category_key in ext_info['match_keys']}")
                                manifest_keys = extension_manifest_keys.get(ext_id, set())
                                category_debug_print(f"[MERGE AUTO-EXT DEBUG]   Extension {ext_id!r} manifest_keys: {manifest_keys}")
                                category_debug_print(f"[MERGE AUTO-EXT DEBUG]   Does '{category_key}' match manifest? {category_key in manifest_keys}")

                    # Check if category matches any enabled extension using manifest keys (includes bl_category)
                    for ext_id, ext_info in enabled_extensions.items():
                        manifest_keys = extension_manifest_keys.get(ext_id, set())
                        if category_key in manifest_keys:
                            category_debug_print(f"[MERGE AUTO-EXT] Updating existing category '{category_name}' with source_extension={ext_id!r} (via manifest bl_category)")
                            cat_data["source_extension"] = ext_id
                            # Only set pending_tag_assignment for categories that were NEVER processed.
                            # If pending_tag_assignment=False, user already assigned tag or "Without Tag".
                            user_already_processed = "pending_tag_assignment" in cat_data and cat_data["pending_tag_assignment"] is False
                            if not cat_data.get("pending_tag_assignment") and not user_already_processed:
                                cat_data["pending_tag_assignment"] = True
                                category_debug_print(f"[MERGE AUTO-EXT] Re-enabled pending_tag_assignment for '{category_name}' (first-time discovery)")
                            elif user_already_processed:
                                category_debug_print(f"[MERGE AUTO-EXT] SKIP for '{category_name}': user already processed")
                            cache_changed = True
                            break
                        # Fallback to ID-based matching for extensions without Python bl_category scanning
                        if category_key in ext_info["match_keys"]:
                            category_debug_print(f"[MERGE AUTO-EXT] Updating existing category '{category_name}' with source_extension={ext_id!r} (via ID match)")
                            cat_data["source_extension"] = ext_id
                            # Only set pending for categories that were NEVER processed by user
                            user_already_processed = "pending_tag_assignment" in cat_data and cat_data["pending_tag_assignment"] is False
                            if not cat_data.get("pending_tag_assignment") and not user_already_processed:
                                cat_data["pending_tag_assignment"] = True
                                category_debug_print(f"[MERGE AUTO-EXT] Re-enabled pending_tag_assignment for '{category_name}' (first-time discovery)")
                            elif user_already_processed:
                                category_debug_print(f"[MERGE AUTO-EXT] SKIP for '{category_name}': user already processed")
                            cache_changed = True
                            break
                        # Prefix matching: "mpfbv2014" should match "mpfb" from "add-on-mpfb"
                        for ext_key in ext_info["match_keys"]:
                            if category_key.startswith(ext_key) and len(category_key) > len(ext_key):
                                category_debug_print(f"[MERGE AUTO-EXT] Updating existing category '{category_name}' via prefix: source_extension={ext_id!r} (key={ext_key!r})")
                                cat_data["source_extension"] = ext_id
                                # Only set pending for categories that were NEVER processed by user
                                user_already_processed = "pending_tag_assignment" in cat_data and cat_data["pending_tag_assignment"] is False
                                if not cat_data.get("pending_tag_assignment") and not user_already_processed:
                                    cat_data["pending_tag_assignment"] = True
                                    category_debug_print(f"[MERGE AUTO-EXT] Re-enabled pending_tag_assignment for '{category_name}' (first-time discovery)")
                                elif user_already_processed:
                                    category_debug_print(f"[MERGE AUTO-EXT] SKIP for '{category_name}': user already processed")
                                cache_changed = True
                                break
                        if cat_data.get("source_extension"):
                            break
        except Exception as e:
            category_debug_print(f"[MERGE AUTO-EXT] Error updating existing categories: {e}")

        # Save immediately if we updated source_extension for existing categories
        # This is critical for categories like Hot Node that were discovered before extension context
        if cache_changed:
            if _save_glyph_mappings_to_file(force_discovery_skip=False):
                category_debug_print("[MERGE AUTO-EXT] Saved source_extension updates to JSON")
            else:
                category_debug_print("[MERGE AUTO-EXT] Failed to save source_extension updates")

        # Add new categories with appropriate glyphs from DEFAULT_CATEGORY_GLYPHS
        for category in new_categories:
            # First check if category exists in DEFAULT_CATEGORY_GLYPHS
            if category in DEFAULT_CATEGORY_GLYPHS:
                default_data = DEFAULT_CATEGORY_GLYPHS[category]
                glyph = default_data.get("glyph", "")
            else:
                glyph = None
            if glyph is None:
                # Check if category name is a single glyph (addon category with glyph as name)
                if _is_single_glyph(category):
                    glyph = category  # Use the glyph from category name
                else:
                    # No glyph - leave empty to use fallback letter (first char of category name)
                    glyph = ""

            # Determine base_type
            if glyph and _is_single_glyph(category):
                base_type = "glyph_only"
            elif glyph:
                base_type = "glyph_text"
            else:
                base_type = "text_only"

            # Get display name from panel label ONLY for glyph_only categories
            # (e.g., "Script 1" for category "" which is a single glyph)
            # For text_only/glyph_text categories, leave display_name empty to use category name as fallback
            if base_type == "glyph_only":
                default_display_name = category_to_label.get(category, "")
            elif base_type == "glyph_text":
                # For glyph_text categories, store original category name in default_display_name
                # This ensures tooltips show the correct original name when display_name is overridden
                default_display_name = category
            else:
                # For text_only categories, ALWAYS use category name as display name.
                # Panel labels are inappropriate here: they represent individual panels
                # ("Coloraide 1.5.0", "Credits", "Add walk cycle") not the addon/category name.
                # Only glyph_only categories should use panel labels for human-readable names.
                default_display_name = category

            # Use global key (-1) for newly discovered categories
            cache_key = _make_cache_key(-1, category)

            # Determine source_extension for this category:
            # 1. If pending_extension_context is set (extension just installed), all categories get that extension
            # 2. Otherwise, use auto-detected extension from category_to_auto_extension (individual per category)
            ext_id = ""
            ext_mode = 0
            tag_already_assigned = False
            is_from_pending_ext = pending_extension_context is not None

            # DEBUG: Log for MPFB/Mixamo categories
            if "MPFB" in category or "Mixamo" in category:
                category_debug_print(f"[MERGE DEBUG] NEW CATEGORY processing: category={category!r}")
                category_debug_print(f"[MERGE DEBUG]   pending_extension_context={pending_extension_context}")
                category_debug_print(f"[MERGE DEBUG]   category_to_auto_extension={category_to_auto_extension}")
                category_debug_print(f"[MERGE DEBUG]   category in category_to_auto_extension? {category in category_to_auto_extension}")

            # PRIORITY 1: Use specific extension-panel mapping (most accurate)
            # This handles cases where a category was discovered from a specific extension module
            # (e.g., bl_ext.blender_org.hyperfy_tools) even if pending context is from a different extension.
            specific_ext_id = state.last_discovered_ext_panel_categories.get(category)
            if specific_ext_id:
                ext_id = specific_ext_id
                # Still use pending context flags if available (for mode_flag etc.)
                if is_from_pending_ext:
                    ext_mode = pending_extension_context.get("mode_flag", 0)
                    tag_already_assigned = pending_extension_context.get("tag_already_assigned", False)
                category_debug_print(f"[MERGE DEBUG] NEW CATEGORY from panel mapping: category={category!r}, extension_id={ext_id!r}")
            elif is_from_pending_ext and pending_extension_context.get("extension_id", "").strip():
                # PRIORITY 2: Pending extension with valid extension_id
                ext_id = pending_extension_context.get("extension_id", "")
                ext_mode = pending_extension_context.get("mode_flag", 0)
                tag_already_assigned = pending_extension_context.get("tag_already_assigned", False)
                category_debug_print(f"[MERGE DEBUG] NEW CATEGORY from pending extension: category={category!r}, extension_id={ext_id!r}")
            elif category in category_to_auto_extension:
                # Individual auto-detection for this category
                ext_id = category_to_auto_extension[category]
                category_debug_print(f"[MERGE DEBUG] NEW CATEGORY auto-detected: category={category!r}, extension_id={ext_id!r}")
            else:
                # Fallback: Try to get extension_id from discovered extension panel categories
                # This handles cases where extension_post_install_handler was called with empty extension_id
                # but _discover_active_categories found the mapping between category and extension_id
                fallback_ext_id = state.last_discovered_ext_panel_categories.get(category)
                if fallback_ext_id:
                    ext_id = fallback_ext_id
                    # If we're in pending context but with empty extension_id, use the pending context flags
                    if is_from_pending_ext:
                        ext_mode = pending_extension_context.get("mode_flag", 0)
                        tag_already_assigned = pending_extension_context.get("tag_already_assigned", False)
                        category_debug_print(f"[MERGE DEBUG] NEW CATEGORY fallback from pending context: category={category!r}, extension_id={ext_id!r} (from state.last_discovered_ext_panel_categories)")
                    else:
                        category_debug_print(f"[MERGE DEBUG] NEW CATEGORY fallback: category={category!r}, extension_id={ext_id!r} (from state.last_discovered_ext_panel_categories)")
                else:
                    # DEBUG: Log why category didn't get extension
                    if "MPFB" in category or "Mixamo" in category or "Hyperfy" in category:
                        category_debug_print(f"[MERGE DEBUG] NEW CATEGORY WARNING: {category!r} did NOT get source_extension!")
                        category_debug_print(f"[MERGE DEBUG]   is_from_pending_ext={is_from_pending_ext}")
                        category_debug_print(f"[MERGE DEBUG]   in category_to_auto_extension={category in category_to_auto_extension}")
                        category_debug_print(f"[MERGE DEBUG]   fallback_ext_id from state.last_discovered_ext_panel_categories: {fallback_ext_id}")
                        if is_from_pending_ext:
                            category_debug_print(f"[MERGE DEBUG]   pending_extension_id: {pending_extension_context.get('extension_id', '')!r}")

            # Built from the field table rather than spelled out, so a newly discovered category
            # has exactly the fields an entry has. The literal this replaced had already fallen
            # behind: it was missing first_letter and tags.
            new_category = new_entry()
            new_category.update({
                KEY_GLYPH: glyph,
                KEY_DISPLAY_NAME: default_display_name,
                KEY_DEFAULT_GLYPH: glyph,
                KEY_DEFAULT_DISPLAY_NAME: default_display_name,
                KEY_BASE_TYPE: base_type,
                KEY_SOURCE_EXTENSION: ext_id,
            })
            state.glyph_cache[cache_key] = new_category

            # If an extension was detected (either from pending context or auto-detection), mark as pending.
            if ext_id:
                state.glyph_cache[cache_key]["source_extension"] = ext_id
                # Only set pending_tag_assignment if tag was NOT already assigned via tab drop.
                # For tab drops, the category will be visible in the general list without filtering,
                # so we don't need to show "New Add-ons!" button.
                if not tag_already_assigned:
                    state.glyph_cache[cache_key]["pending_tag_assignment"] = True
                    category_debug_print(f"[MERGE DEBUG] SET PENDING: category={category!r}, pending_tag_assignment=True, source_extension={ext_id!r}")
                else:
                    category_debug_print(f"[MERGE DEBUG] TAB DROP: category={category!r}, tag_already_assigned=True, NOT setting pending_tag_assignment, source_extension={ext_id!r}")

                # Use ACTUAL discovered spaces from panel_samples, NOT ext_space from drop context.
                # This is crucial for extensions like Hot Node that are dropped in VIEW3D but
                # register panels in Node Editor - the category must be tagged for NODE_EDITOR,
                # not VIEW3D.
                category_spaces = category_to_spaces.get(category, set())
                if category_spaces:
                    spaces_flags = 0
                    for space_str in category_spaces:
                        spaces_flags |= SPACE_TO_FLAG.get(space_str, 0)
                    state.glyph_cache[cache_key]["discovered_in_spaces"] = flags_to_spaces(spaces_flags)
                    tag_log(
                        f"_merge_discovered_categories: set discovered_in_spaces for new category {category!r} "
                        f"to {state.glyph_cache[cache_key]['discovered_in_spaces']} (from panel_samples)"
                    )

                # Use bl_context from panel_samples to determine mode filtering.
                # This is crucial for panels like "VCol Edit" that only show in EDIT mode.
                category_contexts = category_to_contexts.get(category, set())
                if category_contexts:
                    modes_flags = 0
                    for bl_ctx in category_contexts:
                        modes_flags |= BL_CONTEXT_TO_MODE_FLAG.get(bl_ctx, 0)
                    if modes_flags:
                        state.glyph_cache[cache_key]["discovered_in_modes"] = flags_to_modes(modes_flags)
                        tag_log(
                            f"_merge_discovered_categories: set discovered_in_modes for new category {category!r} "
                            f"to {state.glyph_cache[cache_key]['discovered_in_modes']} (from bl_context={category_contexts})"
                        )

                if ext_mode and not state.glyph_cache[cache_key].get("discovered_in_modes"):
                    state.glyph_cache[cache_key]["discovered_in_modes"] = flags_to_modes(ext_mode)

                # Store the install mode flag for mode-aware filtering of "New Add-ons!" button.
                # This is used when discovered_in_modes is empty (panels don't specify bl_context).
                # The category should only show "New Add-ons!" in the mode where it was installed.
                if ext_mode:
                    state.glyph_cache[cache_key][KEY_INSTALL_MODE_FLAG] = ext_mode
                    tag_log(
                        f"_merge_discovered_categories: set install_mode_flag={ext_mode:#x} for {category!r}"
                    )

                tag_log(
                    f"_merge_discovered_categories: marked new category {category!r} "
                    f"as pending from extension {ext_id!r}"
                )

            # OPTIMIZATION: Use cached icon path from extension_post_install_handler if available
            # This avoids repeated disk scans for icon detection
            # OPTIMIZATION: Session-level tracking to avoid redundant checks
            # OPTIMIZATION: Skip icon detection entirely if skip_icon_detection=True
            detected_icon_path = None
            detected_provider = None

            if not skip_icon_detection:
                # Check if already checked in this session
                if category in state.icon_detection_session_checked:
                    category_debug_print(f"[ICON SESSION CACHE] Skipping already-checked new category {category!r}")
                else:
                    # Check if we have a cached icon path from Install from Disk
                    icon_cache = state.extension_install_icon_cache
                    if ext_id and ext_id in icon_cache:
                        detected_icon_path = icon_cache[ext_id]
                        detected_provider = "extension_auto"
                        category_debug_print(f"[ICON CACHE] Using cached icon for category {category!r}: {detected_icon_path!r}")
                    elif ext_id:
                        # Try alternate extension_id formats (e.g., "add-on-hyperfy_tools" vs "add-on-blender_org.hyperfy_tools")
                        for _alt_prefix in ("add-on-blender_org.", "add-on-user_default.", "add-on-"):
                            _alt_id = _alt_prefix + ext_id.replace("add-on-", "")
                            if _alt_id in icon_cache:
                                detected_icon_path = icon_cache[_alt_id]
                                detected_provider = "extension_auto"
                                category_debug_print(f"[ICON CACHE] Using cached icon (alt key {_alt_id!r}) for category {category!r}: {detected_icon_path!r}")
                                break

                    # [POPULAR ADDONS DB] BEGIN - Fallback: try Popular Addons Database by extension_id or category name
                    # Can be removed when extensions bundle their own icons.
                    if POPULAR_ADDONS_DB_ENABLED and not detected_icon_path:
                        try:
                            from bl_ext.user_default.popular_addons_database import api as _pad_api
                            import re as _re_mod
                            # Build list of addon_id candidates to try (order matters)
                            _pad_candidates = []
                            if ext_id:
                                raw_id = ext_id
                                if raw_id.startswith("add-on-"):
                                    raw_id = raw_id[7:]
                                # Strategy 1: repo.pkg format -> extract "pkg" (e.g., "blender_org.ucupaint" -> "ucupaint")
                                if "." in raw_id:
                                    _pkg = raw_id.rsplit(".", 1)[-1]
                                    if not _pkg[0:1].isdigit():  # Not just a version number
                                        _pad_candidates.append(_pkg)
                                # Strategy 2: strip version suffix (e.g., "coloraide-v1.5.1" -> "coloraide")
                                stripped = _re_mod.sub(r'-v?\d+(\.\d+)*$', '', raw_id)
                                if stripped and stripped not in _pad_candidates:
                                    _pad_candidates.append(stripped)
                                    _us = stripped.replace("-", "_")
                                    if _us not in _pad_candidates:
                                        _pad_candidates.append(_us)
                                # Strategy 3: raw_id as fallback
                                if raw_id not in _pad_candidates:
                                    _pad_candidates.append(raw_id)
                            # Strategy 4: category name variants (original case + lowercase + separators)
                            for _ck in (category, category.lower(), category.lower().replace(" ", ""), category.lower().replace(" ", "_"), category.lower().replace(" ", "-")):
                                if _ck not in _pad_candidates:
                                    _pad_candidates.append(_ck)
                            for _q_id in _pad_candidates:
                                result = _pad_api.query_popular_addon_icon(_q_id)
                                if result:
                                    pad_icon_path = result.get("icon_path", "")
                                    if pad_icon_path and os.path.isfile(pad_icon_path):
                                        detected_icon_path = pad_icon_path
                                        detected_provider = "popular_addons_database"
                                        category_debug_print(f"[POPULAR ADDONS] Icon for {category!r} via {_q_id!r}: {pad_icon_path!r}")
                                    break
                        except Exception as pad_ex:
                            category_debug_print(f"[POPULAR ADDONS] Lookup failed: {pad_ex}")
                    # [POPULAR ADDONS DB] END

                    # Fallback to disk scan only if no cached icon (for backward compatibility)
                    if not detected_icon_path:
                        detected_icon_path, detected_provider = _auto_detect_extension_icon_path(category)

                    # Mark as checked in this session
                    state.icon_detection_session_checked.add(category)
            else:
                category_debug_print(f"[ICON DETECTION] Skipping icon detection for new category {category!r} (will run in background)")

            if detected_icon_path:
                state.glyph_cache[cache_key]["icon_path"] = detected_icon_path
                state.glyph_cache[cache_key]["icon_provider"] = detected_provider or "extension_auto"
                category_debug_print(
                    f"[] merge new category auto-icon: "
                    f"category={category!r}, path={detected_icon_path!r}, provider={state.glyph_cache[cache_key]['icon_provider']!r}"
                )
            else:
                category_debug_print(f"[] merge new category no icon: category={category!r}")

            category_debug_print(f"[GLYPH] Added new category '{category}' with glyph '{glyph}', base_type={base_type}")
            category_debug_print(f"[NEW CATEGORY DEBUG] Cache entry for '{category}': source_extension='{ext_id}', pending_tag_assignment={state.glyph_cache[cache_key].get('pending_tag_assignment', False)}, discovered_in_spaces={state.glyph_cache[cache_key].get('discovered_in_spaces', [])}")
            category_debug_print(f"[NEW CATEGORY DEBUG] Total cache size after adding '{category}': {len(state.glyph_cache)} categories")

    # Consume the pending extension context now that all categories have been processed
    # IMPORTANT: Only clear by timeout (30 seconds), NOT by finding new categories.
    # Extensions may register categories in different space types (e.g., Hot Node dropped
    # in VIEW3D but appears in Node Editor). We need the context to persist until all
    # space types have been discovered.
    if pending_extension_context is not None:
        ctx_time = pending_extension_context.get("timestamp", 0)
        # Clear only if 30 seconds passed - gives enough time for all space types to be discovered
        if time.time() - ctx_time > 30.0:
            tag_log(f"_merge_discovered_categories: clearing pending extension context (30s timeout)")
            set_pending_extension_context(None)

    if new_categories:
        # Save updated cache to file (skip if file exists - discovery mode)
        if _save_glyph_mappings_to_file(force_discovery_skip=True):
            category_debug_print(f"[GLYPH] Saved {len(new_categories)} new category mappings to JSON")
            return True
        else:
            category_debug_print(f"[GLYPH] Failed to save new category mappings")

    elif cache_changed or existing_categories_needing_icon_update or existing_glyph_only_needing_name_update or existing_categories_updated_from_context:
        if _save_glyph_mappings_to_file(force_discovery_skip=True):
            if cache_changed:
                category_debug_print("[GLYPH] Saved cache canonicalization updates to JSON")
            if existing_categories_needing_icon_update:
                category_debug_print(f"[GLYPH] Saved icon updates for {len(existing_categories_needing_icon_update)} existing categories to JSON")
            if existing_glyph_only_needing_name_update:
                category_debug_print(f"[GLYPH] Saved default_display_name updates for {len(existing_glyph_only_needing_name_update)} existing glyph_only categories to JSON")
            if existing_categories_updated_from_context:
                category_debug_print(f"[GLYPH] Saved pending_tag_assignment updates for existing categories to JSON")
            return True
        else:
            category_debug_print("[GLYPH] Failed to save cache updates")

    else:
        category_debug_print(f"[GLYPH] No new categories found (all {len(discovered)} are cached)")

    # Reset the install-from-disk flag after it has been consumed
    # This ensures Python file scanning only happens once per Install from Disk operation
    if _install_from_disk_just_occurred:
        category_debug_print("[MERGE] Resetting _install_from_disk_just_occurred flag after use")
        set_install_from_disk_just_occurred(False)

    # PERF: Log timing
    _merge_elapsed = (time.perf_counter() - _merge_start_time) * 1000
    category_debug_print(f"[PERF] _merge_discovered_categories completed in {_merge_elapsed:.2f}ms")

    return (len(new_categories) > 0 or cache_changed or
            bool(existing_categories_needing_icon_update) or
            bool(existing_glyph_only_needing_name_update))
