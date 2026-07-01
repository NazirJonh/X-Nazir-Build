# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Extension discovery and icon-detection helpers for the Tabs-System.

Extracted verbatim from ``space_userpref.py`` (no behavioural change).
Contains functions for:
- Scanning extension packages for icon files.
- Post-install handling (marking newly-installed categories as pending).
- Matching extension IDs and manifest fields to category names.
- Auto-detecting extension icon paths from the user EXTENSIONS directory.

The two large discovery functions (``_discover_active_categories`` and
``_merge_discovered_categories``) are appended in a separate step.

Cross-module calls to ``_auto_sync_to_wm``, ``_auto_save_tags`` use lazy
imports inside function bodies to avoid circular dependencies with
``space_userpref``.
"""

import bpy
import os
import re
import time

from bl_ui.glyph_tag_system.defaults import (
    BL_CONTEXT_TO_MODE_FLAG,
    DEFAULT_CATEGORY_GLYPHS,
    POPULAR_ADDONS_DB_ENABLED,
    SPACE_TO_FLAG,
)
from bl_ui.glyph_tag_system.conversions import (
    _is_single_edit_apart,
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
    _package_match_log_once,
    category_debug_print,
    tag_log,
)
from bl_ui.glyph_tag_system.migrations import (
    _normalize_category_data,
)
from bl_ui.glyph_tag_system.glyph_cache import (
    _find_panel_label_for_category,
    _save_glyph_mappings_to_file,
)

# State objects — imported by reference so in-place mutations are shared.
from bl_ui.glyph_tag_system._state import _glyph_cache as _glyph_cache  # noqa: F401
from bl_ui.glyph_tag_system._state import (
    _extension_manifest_keys_cache as _extension_manifest_keys_cache,  # noqa: F401
    _icon_detection_cache as _icon_detection_cache,  # noqa: F401
    _last_discovered_ext_panel_categories as _last_discovered_ext_panel_categories,  # noqa: F401
    _pending_extension_context as _pending_extension_context,  # noqa: F401
    _install_from_disk_just_occurred as _install_from_disk_just_occurred,  # noqa: F401
    # State objects read by discovery merge / ordering. Imported by reference so the
    # shared objects in bl_ui.glyph_tag_system._state are seen here (these are only
    # read/iterated in-place; reassignment happens via the accessors below).
    _category_orders_cache as _category_orders_cache,  # noqa: F401
    _discovery_debounce_timer as _discovery_debounce_timer,  # noqa: F401
    _icon_detection_session_checked as _icon_detection_session_checked,  # noqa: F401
    _in_ui_draw as _in_ui_draw,  # noqa: F401
    _last_discovered_category_sources as _last_discovered_category_sources,  # noqa: F401
    _merge_discovery_cache as _merge_discovery_cache,  # noqa: F401
)
from bl_ui.glyph_tag_system._state import (
    set_pending_extension_context,
    # Reassignment accessors for shared state (call these instead of `name = ...` so
    # every importer sees the new value; a direct reassignment would orphan the single
    # owner object in bl_ui.glyph_tag_system._state).
    reset_icon_detection_session_checked,
    reset_last_discovered_category_sources,
    reset_last_discovered_ext_panel_categories,
    set_discovery_debounce_timer,
    set_install_from_disk_just_occurred,
    set_last_discovered_category_sources,
    # Live getters for reassignable scalars/objects (module-level imports bind a
    # snapshot at import time; reading through these always resolves to _state).
    is_in_ui_draw,
    is_install_from_disk_just_occurred,
    get_discovery_debounce_timer,
    get_merge_discovery_cache,
    get_pending_extension_context,
)

# Maximum cache age for icon detection (in seconds).
_ICON_CACHE_MAX_AGE = 300.0  # 5 minutes

# Constant: priority ordering for category-discovery sources.
_CATEGORY_DISCOVERY_SOURCE_PRIORITY = {
    "panel_discovered": 0,
    "manifest_name": 1,
    "manifest_id": 2,
    "package_dir": 3,
    "unknown": 99,
}


# -----------------------------------------------------------------------------
# Preview mode thin wrapper
# -----------------------------------------------------------------------------

def set_preview_mode_active(active):
    """Set preview mode active state.

    Called from C++ when opening edit dialog to ensure tag changes
        only affect overrides, not mappings, until Save is clicked.
    """
    # Delegate to the single owner (bl_ui.glyph_tag_system._state) so every importer
    # observes the change. Kept here as a thin wrapper to preserve the public name.
    # NOTE: accessed via the module (not the imported name) to avoid the local
    # function recursing into itself.
    from bl_ui.glyph_tag_system import _state as _tag_state
    _tag_state.set_preview_mode_active(active)
    category_debug_print(f"[DEBUG set_preview_mode_active] Set to {active}")


# -----------------------------------------------------------------------------
# Icon file scanning
# -----------------------------------------------------------------------------

def _scan_extension_icon_path(pkg_path: str):
    """Scan extension package directory for icon file (icon.png, icon.webp, icon.jpg, icon.jpeg).

    Returns:
        Tuple of (icon_path, icon_filename) or (None, None) if not found.
    """
    icon_filenames = ("icon.png", "icon.webp", "icon.jpg", "icon.jpeg")
    for icon_name in icon_filenames:
        icon_path = os.path.join(pkg_path, icon_name)
        if os.path.isfile(icon_path):
            return icon_path, icon_name
    return None, None


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
    global _glyph_cache
    global _extension_manifest_keys_cache

    category_debug_print(f"[EXTENSION POST INSTALL] >>> START handler for {extension_id!r}")
    category_debug_print(f"[EXTENSION POST INSTALL] space_type={space_type}, mode_flag={mode_flag:#010x}, tag_assigned={tag_already_assigned}, from_disk={is_install_from_disk}")
    category_debug_print(f"[EXTENSION POST INSTALL] Current cache size: {len(_glyph_cache)} categories")

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
        for cache_key, cat_data in _glyph_cache.items():
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
                if not hasattr(extension_post_install_handler, "_icon_cache"):
                    extension_post_install_handler._icon_cache = {}
                # Use pkg_path as cache key if extension_id is empty
                cache_key_icon = extension_id if extension_id else pkg_path
                extension_post_install_handler._icon_cache[cache_key_icon] = icon_path
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
                                if not hasattr(extension_post_install_handler, "_icon_cache"):
                                    extension_post_install_handler._icon_cache = {}
                                cache_key_icon = extension_id if extension_id else pkg_path
                                extension_post_install_handler._icon_cache[cache_key_icon] = pad_icon_path
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
            _extension_manifest_keys_cache[cache_key] = {
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
    for cache_key, cat_data in list(_glyph_cache.items()):
        if not (isinstance(cache_key, tuple) and len(cache_key) == 2):
            continue
        _cache_space_type, category_name = cache_key
        if not isinstance(cat_data, dict):
            cat_data = _normalize_category_data(cat_data, category_name)
            _glyph_cache[cache_key] = cat_data

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
        category_debug_print(f"[EXTENSION POST INSTALL] Updated {len([k for k, v in _glyph_cache.items() if isinstance(k, tuple) and v.get('pending_tag_assignment')])} existing categories to pending")
        from bl_ui.glyph_tag_system.wm_sync import _auto_sync_to_wm
        _auto_sync_to_wm()
        from bl_ui.glyph_tag_system.persistence import _auto_save_tags
        _auto_save_tags()

    category_debug_print(f"[EXTENSION POST INSTALL] Final cache state: {len(_glyph_cache)} total categories")
    category_debug_print(f"[EXTENSION POST INSTALL] Pending extension context set: {get_pending_extension_context()}")

    tag_log(
        f"extension_post_install_handler: extension={extension_id!r}, "
        f"space_type={space_type}, mode_flag={mode_flag:#010x}, set at {time.time()}"
    )

    category_debug_print(f"[EXTENSION POST INSTALL] <<< END handler for {extension_id!r}")


# -----------------------------------------------------------------------------
# Extension ID / manifest matching helpers
# -----------------------------------------------------------------------------

def _extension_id_match_keys(extension_id: str):
    """Build normalized match keys from extension id/package tokens.

    Supports ids like:
    - "blender_org/brushstroke_tools"
    - "add-on-ucupaint-v2.4.5"
    - "ucupaint"
    """
    keys = set()
    if not isinstance(extension_id, str):
        return keys

    ext = extension_id.strip()
    if not ext:
        return keys

    def _add_key(text: str):
        key = _normalize_category_key(text)
        if key:
            keys.add(key)

    _add_key(ext)

    # repo/package form: use package component as strongest identity key.
    if "/" in ext:
        _repo, _sep, pkg = ext.rpartition("/")
        if pkg:
            _add_key(pkg)
            ext = pkg

    # Remove common archive stem prefixes.
    stem = re.sub(r"^(?:add[-_]?on[-_])", "", ext, flags=re.IGNORECASE)
    _add_key(stem)

    # Strip common version suffixes, e.g. "-v2.4.5", "_2_4_5", "-2.4.5".
    stem_no_ver = re.sub(r"(?:[-_]?v?\d+(?:[._-]\d+)*)$", "", stem, flags=re.IGNORECASE)
    _add_key(stem_no_ver)

    return keys


def _extension_ids_match(existing_extension_id: str, incoming_extension_id: str) -> bool:
    """Loose extension-id equivalence via normalized key overlap."""
    existing_keys = _extension_id_match_keys(existing_extension_id)
    incoming_keys = _extension_id_match_keys(incoming_extension_id)
    if not existing_keys or not incoming_keys:
        return False
    return bool(existing_keys.intersection(incoming_keys))


def _get_discovery_source_priority(source: str) -> int:
    return _CATEGORY_DISCOVERY_SOURCE_PRIORITY.get(source, _CATEGORY_DISCOVERY_SOURCE_PRIORITY["unknown"])


def _pick_canonical_category_name(candidates, source_map):
    """Pick one canonical category name among aliases sharing the same normalized key."""
    if not candidates:
        return ""

    def _style_rank(name: str) -> int:
        # Prefer human-readable/mixed-case variants over lowercase technical ids.
        if any(ch.isupper() for ch in name):
            return 0
        if any(ch.isspace() for ch in name):
            return 1
        return 2

    return sorted(
        candidates,
        key=lambda name: (
            _get_discovery_source_priority(source_map.get(name, "unknown")),
            _style_rank(name),
            len(name),
            name.lower(),
            name,
        ),
    )[0]


def _manifest_field_match_keys(field_value: str):
    """Build robust normalized match keys from a manifest text field."""
    keys = set()
    if not isinstance(field_value, str):
        return keys

    value = field_value.strip()
    if not value:
        return keys

    normalized_full = _normalize_category_key(value)
    if normalized_full:
        keys.add(normalized_full)

    # Support values like "Huge Menace <hello@hugemenace.co>".
    email = ""
    name_and_email = re.match(r"^\s*([^<]+?)\s*<([^>]+)>\s*$", value)
    if name_and_email:
        display_name = name_and_email.group(1).strip()
        email = name_and_email.group(2).strip()
        display_key = _normalize_category_key(display_name)
        if display_key:
            keys.add(display_key)
    else:
        email_match = re.search(r"([A-Za-z0-9._%+\-]+@[A-Za-z0-9.\-]+\.[A-Za-z]{2,})", value)
        if email_match:
            email = email_match.group(1).strip()

    if email and "@" in email:
        _local, _sep, domain = email.partition("@")
        domain_labels = [label for label in re.split(r"[^A-Za-z0-9]+", domain) if label]
        if domain_labels:
            # Prefer second-level domain key: "hugemenace.co" -> "hugemenace".
            if len(domain_labels) >= 2:
                sld_key = _normalize_category_key(domain_labels[-2])
                if sld_key:
                    keys.add(sld_key)
            # Also keep a compact domain form without TLD for subdomain cases.
            compact_domain = "".join(domain_labels[:-1]) if len(domain_labels) > 1 else domain_labels[0]
            compact_domain_key = _normalize_category_key(compact_domain)
            if compact_domain_key:
                keys.add(compact_domain_key)

    return keys


def _extension_manifest_match_keys(pkg_path: str, pkg_name: str, scan_python_files: bool = True):
    """Collect normalized keys from extension folder + manifest for category matching.

    Args:
        pkg_path: Path to the extension package directory.
        pkg_name: Package name from the manifest.
        scan_python_files: If True, scan Python files for bl_category values.
                          Set to False for performance optimization when scanning
                          is not needed (e.g., drag-and-drop installs).
    """
    keys = set()
    if pkg_name:
        keys.add(_normalize_category_key(pkg_name))

    manifest_path = os.path.join(pkg_path, "blender_manifest.toml")
    if not os.path.isfile(manifest_path):
        #Man category_debug_print(f"[MANIFEST] No manifest found at {manifest_path!r}, returning keys={keys}")
        return keys

    try:
        import tomllib

        with open(manifest_path, "rb") as fh:
            manifest = tomllib.load(fh)

        for field_name in ("id", "name", "tagline", "maintainer", "publisher"):
            field_value = manifest.get(field_name)
            if isinstance(field_value, str) and field_value.strip():
                keys.update(_manifest_field_match_keys(field_value))

        #Man category_debug_print(f"[MANIFEST] pkg_name={pkg_name!r}, pkg_path={pkg_path!r}, keys from manifest fields={keys}")

        # Also scan Python files for panel bl_category values.
        # This handles cases where panel bl_category differs from manifest name
        # (e.g., Edge Length Measure extension uses "Edge Length" as bl_category).
        # OPTIMIZATION: Only scan Python files when explicitly requested (Install from Disk).
        if scan_python_files:
            import re
            py_files_scanned = 0
            bl_categories_found = []
            for root, dirs, files in os.walk(pkg_path):
                # Skip __pycache__ and hidden directories
                dirs[:] = [d for d in dirs if not d.startswith('.') and d != '__pycache__']
                for filename in files:
                    if not filename.endswith('.py'):
                        continue
                    py_path = os.path.join(root, filename)
                    py_files_scanned += 1
                    try:
                        with open(py_path, 'r', encoding='utf-8', errors='ignore') as py_file:
                            content = py_file.read()
                        # Find bl_category assignments in Panel classes
                        for match in re.finditer(r'bl_category\s*=\s*["\']([^"\']+)["\']', content):
                            panel_category = match.group(1).strip()
                            if panel_category:
                                normalized = _normalize_category_key(panel_category)
                                keys.add(normalized)
                                bl_categories_found.append(f"{panel_category} -> {normalized}")
                                category_debug_print(f"[MANIFEST] Found bl_category='{panel_category}' in {os.path.relpath(py_path, pkg_path)} -> normalized='{normalized}'")
                    except Exception as e:
                        #Man category_debug_print(f"[MANIFEST] Failed to scan {py_path!r}: {e}")
                        pass

            if py_files_scanned > 0:
                # category_debug_print(f"[MANIFEST] Scanned {py_files_scanned} Python files in {pkg_path!r}, found bl_categories: {bl_categories_found}")
                pass
        #category_debug_print(f"[MANIFEST] Final keys for pkg_name={pkg_name!r}: {keys}")
    except Exception as e:
        pass  # category_debug_print(f"[MANIFEST] manifest parse failed: pkg_path={pkg_path!r}, error={e}")

    return keys


# -----------------------------------------------------------------------------
# Auto icon detection
# -----------------------------------------------------------------------------

def _auto_detect_extension_icon_path(category: str, force_refresh=False):
    """Try to find extension icon file for a category in user EXTENSIONS folders.

    OPTIMIZATION: Added caching to avoid repeated disk scans during UI draws.

    Returns: (icon_path, provider) or ("", "") when not found.
    """
    global _icon_detection_cache
    global _last_discovered_ext_panel_categories

    # PERF: Start timing
    _icon_detect_start = time.perf_counter()

    # OPTIMIZATION: Check cache first
    current_time = time.time()

    if not force_refresh and category in _icon_detection_cache:
        cache_entry = _icon_detection_cache[category]
        cache_age = current_time - cache_entry.get("timestamp", 0)

        # Use cache if it's fresh (less than ICON_CACHE_MAX_AGE seconds old)
        if cache_age < _ICON_CACHE_MAX_AGE:
            icon_path = cache_entry.get("icon_path", "")
            provider = cache_entry.get("provider", "")
            category_debug_print(f"[ICON DETECT OPTIMIZATION] Using cached icon for {category!r}: {icon_path!r} (age={cache_age:.1f}s)")
            return icon_path, provider

    _pref_log_once(f"[] detect start: category={category!r}")
    try:
        extensions_dir = bpy.utils.user_resource('EXTENSIONS')
    except Exception:
        _pref_log_once(f"[] detect abort: category={category!r}, reason=user_resource_exception")
        # Cache the negative result
        _icon_detection_cache[category] = {
            "icon_path": "",
            "provider": "",
            "timestamp": current_time,
        }
        return "", ""

    if not extensions_dir or not os.path.isdir(extensions_dir):
        _pref_log_once(f"[] detect abort: category={category!r}, extensions_dir={extensions_dir!r}, exists=False")
        # Cache the negative result
        _icon_detection_cache[category] = {
            "icon_path": "",
            "provider": "",
            "timestamp": current_time,
        }
        return "", ""

    target_key = _normalize_category_key(category)
    if not target_key:
        _pref_log_once(f"[] detect abort: category={category!r}, normalized_key is empty")
        # Cache the negative result
        _icon_detection_cache[category] = {
            "icon_path": "",
            "provider": "",
            "timestamp": current_time,
        }
        return "", ""

    _pref_log_once(f"[] detect context: category={category!r}, target_key={target_key!r}, extensions_dir={extensions_dir!r}")

    icon_filenames = ("icon.png", "icon.webp", "icon.jpg", "icon.jpeg")

    def _check_pkg_path_for_icon(pkg_path: str):
        for icon_name in icon_filenames:
            icon_path = os.path.join(pkg_path, icon_name)
            if os.path.isfile(icon_path):
                _pref_log_once(f"[] detect hit: category={category!r}, icon={icon_path!r}, provider='extension_auto'")
                _icon_detection_cache[category] = {
                    "icon_path": icon_path,
                    "provider": "extension_auto",
                    "timestamp": current_time,
                }
                _icon_detect_elapsed = (time.perf_counter() - _icon_detect_start) * 1000
                category_debug_print(
                    f"[PERF] _auto_detect_extension_icon_path({category!r}) FOUND in {_icon_detect_elapsed:.2f}ms: {icon_path!r}"
                )
                return icon_path, "extension_auto"
        return None

    preferred_ext_id = _last_discovered_ext_panel_categories.get(category, "")
    if preferred_ext_id:
        preferred_pkg_name = preferred_ext_id[7:] if preferred_ext_id.startswith("add-on-") else preferred_ext_id
        try:
            for repo_name in os.listdir(extensions_dir):
                repo_path = os.path.join(extensions_dir, repo_name)
                if (not os.path.isdir(repo_path)) or repo_name.startswith('.'):
                    continue

                pkg_path = os.path.join(repo_path, preferred_pkg_name)
                if not os.path.isdir(pkg_path):
                    continue

                detected_icon = _check_pkg_path_for_icon(pkg_path)
                if detected_icon:
                    if not hasattr(extension_post_install_handler, "_icon_cache"):
                        extension_post_install_handler._icon_cache = {}
                    extension_post_install_handler._icon_cache[preferred_ext_id] = detected_icon[0]
                    return detected_icon
        except Exception:
            pass

    try:
        for repo_name in os.listdir(extensions_dir):
            repo_path = os.path.join(extensions_dir, repo_name)
            if (not os.path.isdir(repo_path)) or repo_name.startswith('.'):
                continue

            for pkg_name in os.listdir(repo_path):
                pkg_path = os.path.join(repo_path, pkg_name)
                if not os.path.isdir(pkg_path):
                    continue

                # CRITICAL OPTIMIZATION: Use cached manifest keys instead of scanning
                # Manifest keys are scanned once at extension install time
                ext_id = f"add-on-{pkg_name}"
                cache_key = (ext_id, True)  # True = scanned Python files at install

                if cache_key in _extension_manifest_keys_cache:
                    match_keys = _extension_manifest_keys_cache[cache_key]["keys"]
                    category_debug_print(f"[ICON DETECT OPTIMIZATION] Using cached manifest keys for {pkg_name!r}: {match_keys}")
                else:
                    # Fallback: scan manifest only (no Python files) if not cached
                    # This should only happen for extensions installed before the optimization
                    match_keys = _extension_manifest_match_keys(pkg_path, pkg_name, scan_python_files=False)
                    # Cache for future use
                    _extension_manifest_keys_cache[cache_key] = {
                        "keys": match_keys,
                        "timestamp": time.time(),
                        "pkg_path": pkg_path,
                    }

                exact_match = target_key in match_keys
                fuzzy_match_key = ""
                if not exact_match:
                    # Prefix matching for extension categories with version suffix.
                    #
                    # ARCHITECTURAL NOTE: Many extensions create categories that include version
                    # numbers or other suffixes (e.g., "MPFB v2.0.14" from extension "mpfb").
                    # The normalized keys become "mpfbv2014" vs "mpfb" - these don't match exactly
                    # and the typo-based fuzzy matching below requires length difference <= 1,
                    # which fails here (diff = 6 characters).
                    #
                    # This prefix matching allows "mpfbv2014" to match "mpfb" by checking if
                    # the category key starts with the extension's normalized key.
                    #
                    # TODO: Consider a more robust architecture where extensions explicitly
                    # declare their panel categories in the manifest, enabling direct mapping
                    # without relying on string heuristics.
                    for key in match_keys:
                        if key and len(key) >= 3 and target_key.startswith(key):
                            fuzzy_match_key = key
                            break

                    if not fuzzy_match_key:
                        # Strict fuzzy fallback for near-typo category names.
                        # Example: "HugeMenance" -> "hugemenace".
                        if len(target_key) >= 8:
                            for key in match_keys:
                                if not key or abs(len(key) - len(target_key)) > 1:
                                    continue
                                if key[:5] != target_key[:5]:
                                    continue
                                if _is_single_edit_apart(target_key, key):
                                    fuzzy_match_key = key
                                    break
                    if not fuzzy_match_key:
                        continue

                # Log package match only once per category+repo to reduce noise
                _package_match_log_once(category, repo_name, pkg_name)

                detected_icon = _check_pkg_path_for_icon(pkg_path)
                if detected_icon:
                    return detected_icon
    except Exception:
        _pref_log_once(f"[] detect abort: category={category!r}, reason=scan_exception")
        # Cache the negative result
        _icon_detection_cache[category] = {
            "icon_path": "",
            "provider": "",
            "timestamp": current_time,
        }
        return "", ""

    _pref_log_once(f"[] detect miss: category={category!r}")
    # Cache the negative result
    _icon_detection_cache[category] = {
        "icon_path": "",
        "provider": "",
        "timestamp": current_time,
    }

    # PERF: Log timing
    _icon_detect_elapsed = (time.perf_counter() - _icon_detect_start) * 1000
    category_debug_print(f"[PERF] _auto_detect_extension_icon_path({category!r}) completed in {_icon_detect_elapsed:.2f}ms")

    return "", ""


def auto_detect_extension_icon_path_normalized(category: str):
    """As #_auto_detect_extension_icon_path, with backslashes normalized to '/'.

    Used by the C++ bridge, which forwards the tuple to ``json.dumps`` as-is.
    """
    icon_path, provider = _auto_detect_extension_icon_path(category)
    return icon_path.replace("\\", "/"), provider


def _discover_active_categories():
    """Discover all active categories from registered panels including addon panels."""
    global _last_discovered_category_sources, _last_discovered_ext_panel_categories
    discovered_categories = set()
    discovered_sources = {}
    panel_samples = []

    # Clear and rebuild extension panel categories mapping
    reset_last_discovered_ext_panel_categories()

    def _record_discovered(category, source):
        if not category:
            return

        discovered_categories.add(category)
        old_source = discovered_sources.get(category)
        if (old_source is None) or (
            _get_discovery_source_priority(source) < _get_discovery_source_priority(old_source)
        ):
            discovered_sources[category] = source

    def _append_panel_sample(source, panel_name, panel_obj):
        # Increased limit to 2000 to ensure all panels including Node Editor are captured.
        # The previous 200 limit was too low and could miss panels from certain space types.
        if len(panel_samples) >= 2000:
            return
        try:
            category = getattr(panel_obj, 'bl_category', '')
            if not category:
                return
            space_type = getattr(panel_obj, 'bl_space_type', '')
            region_type = getattr(panel_obj, 'bl_region_type', '')
            # Collect bl_label for display name (e.g., "Script 1" for category "")
            panel_label = getattr(panel_obj, 'bl_label', '') or ''
            # Collect bl_context for mode-specific panels (e.g., "mesh_edit" for EDIT mode)
            bl_context = getattr(panel_obj, 'bl_context', '') or ''

            # Also try to get addon name from the panel's module bl_info
            if not panel_label:
                try:
                    module = getattr(panel_obj, 'bl_module', None)
                    if module and hasattr(module, 'bl_info'):
                        bl_info = getattr(module, 'bl_info', {})
                        panel_label = bl_info.get('name', '') or ''
                except Exception:
                    pass

            panel_samples.append((source, panel_name, category, space_type, region_type, panel_label, bl_context))
        except Exception:
            return

    try:
        import bpy.types
        from bpy.types import Panel

        # Method 1: Check all registered types that are Panel subclasses
        for type_name in dir(bpy.types):
            try:
                type_obj = getattr(bpy.types, type_name)
                # Check if it's a Panel class with bl_category
                if hasattr(type_obj, 'bl_category') and type_obj.bl_category:
                    _record_discovered(type_obj.bl_category, "panel_discovered")
                    _append_panel_sample("bpy.types", type_name, type_obj)
            except (AttributeError, TypeError):
                continue

        # Method 2: Use Panel.__subclasses__() to find all registered panels
        try:
            for panel_class in Panel.__subclasses__():
                if hasattr(panel_class, 'bl_category') and panel_class.bl_category:
                    _record_discovered(panel_class.bl_category, "panel_discovered")
                    _append_panel_sample("Panel.__subclasses__", getattr(panel_class, "__name__", "<unknown>"), panel_class)
        except Exception as e:
            category_debug_print(f"[GLYPH] Warning: Could not get Panel subclasses: {e}")

        # Method 3: Check _bli_register_classes if available (internal Blender registry)
        try:
            import _bpy
            if hasattr(_bpy, 'types'):
                for attr_name in dir(_bpy.types):
                    try:
                        attr = getattr(_bpy.types, attr_name)
                        if hasattr(attr, 'bl_category') and attr.bl_category:
                            _record_discovered(attr.bl_category, "panel_discovered")
                            _append_panel_sample("_bpy.types", attr_name, attr)
                    except (AttributeError, TypeError):
                        continue
        except ImportError:
            pass

        # Method 4: Seed categories from enabled extension add-ons.
        # This covers cases where extension is enabled but its panels are not yet
        # discoverable through bpy.types / Panel subclasses at this moment.
        try:
            prefs = getattr(bpy.context, "preferences", None)
            addons = getattr(prefs, "addons", None) if prefs else None
            if addons:
                extensions_dir = bpy.utils.user_resource('EXTENSIONS')
                for addon in addons:
                    module_name = getattr(addon, "module", "")
                    if not isinstance(module_name, str) or not module_name.startswith("bl_ext."):
                        continue

                    module_parts = module_name.split(".")
                    # Expected format: bl_ext.<repo_name>.<package_name>
                    if len(module_parts) < 3:
                        continue

                    repo_name = module_parts[1]
                    pkg_name = ".".join(module_parts[2:])
                    # Only seed categories from extensions that have actual panels with bl_category.
                    # Extensions without panels (e.g., icon providers, databases) should not appear as categories.
                    _ext_has_panels = False
                    if extensions_dir:
                        _pkg_check_dir = os.path.join(extensions_dir, repo_name, pkg_name)
                        if os.path.isdir(_pkg_check_dir):
                            for _er, _eds, _efs in os.walk(_pkg_check_dir):
                                _eds[:] = [d for d in _eds if not d.startswith('.') and d != '__pycache__']
                                for _ef in _efs:
                                    if _ef.endswith('.py'):
                                        try:
                                            with open(os.path.join(_er, _ef), 'r', encoding='utf-8', errors='ignore') as _fp:
                                                if 'bl_category' in _fp.read():
                                                    _ext_has_panels = True
                                                    break
                                        except Exception:
                                            pass
                                if _ext_has_panels:
                                    break
                    if not _ext_has_panels:
                        continue

                    if pkg_name:
                        _record_discovered(pkg_name, "package_dir")
                        _log_once(
                            f"[GLYPH DISCOVER DEBUG] extension addon seed: "
                            f"module={module_name!r}, category={pkg_name!r}, source='module'"
                        )

                    if not extensions_dir:
                        continue

                    manifest_path = os.path.join(extensions_dir, repo_name, pkg_name, "blender_manifest.toml")
                    if not os.path.isfile(manifest_path):
                        continue

                    try:
                        import tomllib
                        with open(manifest_path, "rb") as fh:
                            manifest = tomllib.load(fh)

                        # Collect manifest names for comparison with panel categories
                        manifest_names = set()
                        ext_id = f"add-on-{repo_name}.{pkg_name}"
                        for key_name in ("name", "id"):
                            value = manifest.get(key_name)
                            if isinstance(value, str) and value.strip():
                                seed = value.strip()
                                manifest_names.add(seed)
                                # Store category->extension mapping for ALL manifest seeds
                                # (not just panel bl_category), so merge can find the correct
                                # source_extension even when category == manifest name.
                                _last_discovered_ext_panel_categories[seed] = ext_id
                                if key_name == "name":
                                    _record_discovered(seed, "manifest_name")
                                else:
                                    _record_discovered(seed, "manifest_id")
                                _log_once(
                                    f"[GLYPH DISCOVER DEBUG] extension addon seed: "
                                    f"module={module_name!r}, category={seed!r}, source='manifest.{key_name}'"
                                )

                        # Also scan Python files for panel bl_category values.
                        # This handles cases where panel bl_category differs from manifest name
                        # (e.g., Edge Length Measure extension uses "Edge Length" as bl_category).
                        pkg_dir = os.path.dirname(manifest_path)
                        for root, dirs, files in os.walk(pkg_dir):
                            # Skip __pycache__ and hidden directories
                            dirs[:] = [d for d in dirs if not d.startswith('.') and d != '__pycache__']
                            for filename in files:
                                if not filename.endswith('.py'):
                                    continue
                                py_path = os.path.join(root, filename)
                                try:
                                    with open(py_path, 'r', encoding='utf-8', errors='ignore') as py_file:
                                        content = py_file.read()
                                    # Find bl_category assignments in Panel classes
                                    import re
                                    for match in re.finditer(r'bl_category\s*=\s*["\']([^"\']+)["\']', content):
                                        panel_category = match.group(1).strip()
                                        # Only record panel categories that differ from manifest names
                                        if panel_category and panel_category not in manifest_names:
                                            _record_discovered(panel_category, "panel_bl_category")
                                            # Store the mapping between discovered extension panel categories and their extension_id
                                            _last_discovered_ext_panel_categories[panel_category] = ext_id
                                            _log_once(
                                                f"[GLYPH DISCOVER DEBUG] extension panel category: "
                                                f"module={module_name!r}, category={panel_category!r}, "
                                                f"source='panel_bl_category', ext_id={ext_id!r}"
                                            )
                                except Exception:
                                    pass
                    except Exception as e:
                        _log_once(
                            f"[GLYPH DISCOVER DEBUG] extension manifest read failed: "
                            f"module={module_name!r}, path={manifest_path!r}, error={e}"
                        )
        except Exception as e:
            _log_once(f"[GLYPH DISCOVER DEBUG] extension addon seeding failed: {e}")

        # Method 5: Seed categories from installed extension packages with a root icon file.
        # This is a fallback for cases where addon enable events do not immediately expose
        # panel classes or prefs.addons entries for extension packages.
        try:
            extensions_dir = bpy.utils.user_resource('EXTENSIONS')
            if extensions_dir and os.path.isdir(extensions_dir):
                icon_filenames = ("icon.png", "icon.webp", "icon.jpg", "icon.jpeg")

                for repo_name in os.listdir(extensions_dir):
                    repo_path = os.path.join(extensions_dir, repo_name)
                    if (not os.path.isdir(repo_path)) or repo_name.startswith('.'):
                        continue

                    for pkg_name in os.listdir(repo_path):
                        pkg_path = os.path.join(repo_path, pkg_name)
                        if not os.path.isdir(pkg_path):
                            continue

                        has_root_icon = any(os.path.isfile(os.path.join(pkg_path, icon_name)) for icon_name in icon_filenames)
                        if not has_root_icon:
                            continue

                        # Only seed categories from packages that have actual panels with bl_category.
                        _pkg_has_panels = False
                        for _pr, _pds, _pfs in os.walk(pkg_path):
                            _pds[:] = [d for d in _pds if not d.startswith('.') and d != '__pycache__']
                            for _pf in _pfs:
                                if _pf.endswith('.py'):
                                    try:
                                        with open(os.path.join(_pr, _pf), 'r', encoding='utf-8', errors='ignore') as _fp:
                                            if 'bl_category' in _fp.read():
                                                _pkg_has_panels = True
                                                break
                                    except Exception:
                                        pass
                            if _pkg_has_panels:
                                break
                        if not _pkg_has_panels:
                            continue

                        if pkg_name:
                            _record_discovered(pkg_name, "package_dir")
                            _log_once(
                                f"[GLYPH DISCOVER DEBUG] extension package seed: "
                                f"repo={repo_name!r}, category={pkg_name!r}, source='package_with_icon'"
                            )

                        manifest_path = os.path.join(pkg_path, "blender_manifest.toml")
                        if not os.path.isfile(manifest_path):
                            continue

                        try:
                            import tomllib
                            with open(manifest_path, "rb") as fh:
                                manifest = tomllib.load(fh)

                            # Collect manifest names for comparison with panel categories
                            manifest_names = set()
                            ext_id = f"add-on-{repo_name}.{pkg_name}"
                            for key_name in ("name", "id"):
                                value = manifest.get(key_name)
                                if isinstance(value, str) and value.strip():
                                    seed = value.strip()
                                    manifest_names.add(seed)
                                    # Store category->extension mapping for ALL manifest seeds
                                    _last_discovered_ext_panel_categories[seed] = ext_id
                                    if key_name == "name":
                                        _record_discovered(seed, "manifest_name")
                                    else:
                                        _record_discovered(seed, "manifest_id")
                                    _log_once(
                                        f"[GLYPH DISCOVER DEBUG] extension package seed: "
                                        f"repo={repo_name!r}, category={seed!r}, source='manifest.{key_name}_with_icon'"
                                    )

                            # Also scan Python files for panel bl_category values.
                            # This handles cases where panel bl_category differs from manifest name
                            # (e.g., Edge Length Measure extension uses "Edge Length" as bl_category).
                            for root, dirs, files in os.walk(pkg_path):
                                # Skip __pycache__ and hidden directories
                                dirs[:] = [d for d in dirs if not d.startswith('.') and d != '__pycache__']
                                for filename in files:
                                    if not filename.endswith('.py'):
                                        continue
                                    py_path = os.path.join(root, filename)
                                    try:
                                        with open(py_path, 'r', encoding='utf-8', errors='ignore') as py_file:
                                            content = py_file.read()
                                        # Find bl_category assignments in Panel classes
                                        # Match patterns like: bl_category = "Category Name" or bl_category="Category Name"
                                        import re
                                        for match in re.finditer(r'bl_category\s*=\s*["\']([^"\']+)["\']', content):
                                            panel_category = match.group(1).strip()
                                            # Only record panel categories that differ from manifest names
                                            if panel_category and panel_category not in manifest_names:
                                                _record_discovered(panel_category, "panel_bl_category")
                                                # Store the mapping between discovered extension panel categories and their extension_id
                                                _last_discovered_ext_panel_categories[panel_category] = ext_id
                                                _log_once(
                                                    f"[GLYPH DISCOVER DEBUG] extension panel category: "
                                                    f"repo={repo_name!r}, pkg={pkg_name!r}, category={panel_category!r}, "
                                                    f"source='panel_bl_category', ext_id={ext_id!r}"
                                                )
                                    except Exception:
                                        pass
                        except Exception as e:
                            _log_once(
                                f"[GLYPH DISCOVER DEBUG] extension package manifest read failed: "
                                f"pkg_path={pkg_path!r}, error={e}"
                            )
        except Exception as e:
            _log_once(f"[GLYPH DISCOVER DEBUG] extension package seeding failed: {e}")

        # Method 6: Also scan user addon directories for panels with glyph categories.
        # User addons (non-bl_ext.*) are not covered by Method 4, but their panels
        # should be discovered via bpy.types if they're registered. This is a fallback
        # to catch any glyph-based categories that might not have been discovered yet.
        try:
            user_addon_path = bpy.utils.script_path_user()
            if user_addon_path:
                user_addon_dir = os.path.join(user_addon_path, "addons")
                if os.path.isdir(user_addon_dir):
                    for filename in os.listdir(user_addon_dir):
                        if filename.startswith('.') or filename.startswith('__'):
                            continue
                        pkg_path = os.path.join(user_addon_dir, filename)
                        if os.path.isdir(pkg_path):
                            # Check for __init__.py or single .py file
                            init_file = os.path.join(pkg_path, "__init__.py")
                            py_file = os.path.join(pkg_path, filename + ".py")
                            if os.path.isfile(init_file) or os.path.isfile(py_file):
                                # This is a user addon - check if it has panels with glyph categories
                                # The panels should already be in bpy.types if registered
                                # But we can also use the addon name as a fallback category
                                addon_name = os.path.splitext(filename)[0]
                                if addon_name and _is_single_glyph(addon_name):
                                    _record_discovered(addon_name, "user_addon_glyph")
                                    _log_once(
                                        f"[GLYPH DISCOVER DEBUG] user addon glyph category: "
                                        f"addon={addon_name!r}"
                                    )
        except Exception as e:
            _log_once(f"[GLYPH DISCOVER DEBUG] user addon seeding failed: {e}")

        if panel_samples:
            _log_once(f"[GLYPH DISCOVER DEBUG] panel samples collected: {len(panel_samples)} (showing up to 20)")
            for source, panel_name, category, space_type, region_type, panel_label, bl_context in panel_samples[:20]:
                _log_once(
                    f"[GLYPH DISCOVER DEBUG] sample: source={source}, panel={panel_name}, "
                    f"bl_category={category!r}, bl_label={panel_label!r}, space={space_type!r}, region={region_type!r}, bl_context={bl_context!r}"
                )
        else:
            _log_once("[GLYPH DISCOVER DEBUG] panel samples collected: 0")

        set_last_discovered_category_sources(discovered_sources)
        _log_once(f"[GLYPH] Discovered {len(discovered_categories)} active categories: {sorted(discovered_categories)}")
        # Return both discovered categories and panel_samples (for display name lookup)
        return discovered_categories, panel_samples

    except Exception as e:
        reset_last_discovered_category_sources()
        category_debug_print(f"[GLYPH] Error discovering categories: {e}")
        import traceback
        traceback.print_exc()
        return set()


def _merge_discovered_categories(force_refresh=False, skip_icon_detection=False):
    """Merge discovered categories with cached mappings, adding defaults for new ones.

    OPTIMIZATION: Added caching and debouncing to prevent repeated scanning during UI draws.
    OPTIMIZATION: skip_icon_detection parameter allows deferring icon detection to background sync.
    """
    global _glyph_cache, _category_orders_cache
    global _icon_detection_session_checked

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
    category_debug_print(f"[MERGE DEBUG] Current cache size before merge: {len(_glyph_cache)} categories")
    category_debug_print(f"[MERGE DEBUG] Cache keys sample: {list(_glyph_cache.keys())[:10]}")

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
    # This is important because glyph_only categories (e.g., "" for "Script 4")
    # need their display_name to be discovered from panel bl_label
    for category in discovered:
        if _is_single_glyph(category) and category not in category_to_label:
            panel_label = _find_panel_label_for_category(category)
            if panel_label:
                category_to_label[category] = panel_label
                category_debug_print(f"[GLYPH DISCOVER] Found panel label for glyph_only category: {category!r} -> {panel_label!r}")

    discovered_source_map = dict(_last_discovered_category_sources)

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
        for key in _glyph_cache.keys():
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
        canonical_data = _glyph_cache.get(canonical_key)
        alias_data = _glyph_cache.get(alias_key)
        if alias_data is None:
            return False

        changed = False
        if canonical_data is None:
            _glyph_cache[canonical_key] = _clone_category_data(alias_data)
            canonical_data = _glyph_cache[canonical_key]
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
        for tag_key, order_list in _category_orders_cache.items():
            if not isinstance(order_list, list):
                continue

            remapped = [canonical_name if category == alias_name else category for category in order_list]
            deduped = []
            for category in remapped:
                if category not in deduped:
                    deduped.append(category)
            if deduped != order_list:
                _category_orders_cache[tag_key] = deduped
                changed = True

        if alias_key in _glyph_cache:
            del _glyph_cache[alias_key]
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
        _pref_log_once(f"[GLYPH MERGE DEBUG] Brushstroke in _glyph_cache: {brush_key in _glyph_cache}")
        if brush_key in _glyph_cache:
            _pref_log_once(f"[GLYPH MERGE DEBUG] Brushstroke cached data: {_glyph_cache[brush_key]}")

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
        if canonical_key not in _glyph_cache:
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
    icon_cache = getattr(extension_post_install_handler, "_icon_cache", {})

    if not skip_icon_detection:
        for category in discovered:
            if category in new_categories:
                continue  # Skip new categories, they are handled below
            cache_key = _get_cache_key_for_category(category)
            if cache_key in _glyph_cache:
                cached_data = _glyph_cache[cache_key]
                if isinstance(cached_data, dict):
                    icon_source = cached_data.get("icon_source", "auto")
                    icon_path = cached_data.get("icon_path", "")
                    if icon_source == "auto" and not icon_path:
                        # OPTIMIZATION: Skip if already checked in this session
                        if category in _icon_detection_session_checked:
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
                        _icon_detection_session_checked.add(category)

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
        if cache_key in _glyph_cache:
            cached_data = _glyph_cache[cache_key]
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
            # The fallback logic will try to match categories using _last_discovered_ext_panel_categories
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
            if cache_key in _glyph_cache:
                cat_data = _glyph_cache[cache_key]
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
                            for check_key, check_data in _glyph_cache.items():
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
        category_debug_print(f"[NEW CATEGORIES DEBUG] Cache state before processing: {len(_glyph_cache)} total categories")

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
                category_debug_print(f"[MERGE AUTO-EXT DEBUG] Checking {len(_glyph_cache)} cache entries for extension matching")
                category_debug_print(f"[MERGE AUTO-EXT DEBUG] enabled_extensions: {list(enabled_extensions.keys())}")

                # CRITICAL OPTIMIZATION: NEVER scan Python files during merge/discovery
                # Python files are scanned ONLY once at extension install time in extension_post_install_handler()
                # This eliminates the main cause of UI freezes during Get Extension/Add-ons panel draws
                extension_manifest_keys = {}
                for ext_id, ext_info in enabled_extensions.items():
                    # Always use cached keys (scanned at install time)
                    # Use cache key with True (scanned Python files) to get the install-time scan results
                    cache_key = (ext_id, True)

                    if cache_key in _extension_manifest_keys_cache:
                        cache_entry = _extension_manifest_keys_cache[cache_key]
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

                for cache_key, cat_data in list(_glyph_cache.items()):
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
            specific_ext_id = _last_discovered_ext_panel_categories.get(category)
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
                fallback_ext_id = _last_discovered_ext_panel_categories.get(category)
                if fallback_ext_id:
                    ext_id = fallback_ext_id
                    # If we're in pending context but with empty extension_id, use the pending context flags
                    if is_from_pending_ext:
                        ext_mode = pending_extension_context.get("mode_flag", 0)
                        tag_already_assigned = pending_extension_context.get("tag_already_assigned", False)
                        category_debug_print(f"[MERGE DEBUG] NEW CATEGORY fallback from pending context: category={category!r}, extension_id={ext_id!r} (from _last_discovered_ext_panel_categories)")
                    else:
                        category_debug_print(f"[MERGE DEBUG] NEW CATEGORY fallback: category={category!r}, extension_id={ext_id!r} (from _last_discovered_ext_panel_categories)")
                else:
                    # DEBUG: Log why category didn't get extension
                    if "MPFB" in category or "Mixamo" in category or "Hyperfy" in category:
                        category_debug_print(f"[MERGE DEBUG] NEW CATEGORY WARNING: {category!r} did NOT get source_extension!")
                        category_debug_print(f"[MERGE DEBUG]   is_from_pending_ext={is_from_pending_ext}")
                        category_debug_print(f"[MERGE DEBUG]   in category_to_auto_extension={category in category_to_auto_extension}")
                        category_debug_print(f"[MERGE DEBUG]   fallback_ext_id from _last_discovered_ext_panel_categories: {fallback_ext_id}")
                        if is_from_pending_ext:
                            category_debug_print(f"[MERGE DEBUG]   pending_extension_id: {pending_extension_context.get('extension_id', '')!r}")

            _glyph_cache[cache_key] = {
                "glyph": glyph,
                "display_name": default_display_name,
                "color": [0.0, 0.0, 0.0],
                "default_glyph": glyph,
                "default_display_name": default_display_name,
                "base_type": base_type,
                "glyph_mode": "auto",
                "icon_source": "auto",
                "icon_key": "",
                "icon_path": "",
                "icon_provider": "",
                # Extension pending-tag fields
                "source_extension": ext_id,
                "pending_tag_assignment": False,
                "discovered_in_spaces": [],
                "discovered_in_modes": [],
                "install_mode_flag": 0,  # Mode flag when extension was installed (for mode-aware filtering)
            }

            # If an extension was detected (either from pending context or auto-detection), mark as pending.
            if ext_id:
                _glyph_cache[cache_key]["source_extension"] = ext_id
                # Only set pending_tag_assignment if tag was NOT already assigned via tab drop.
                # For tab drops, the category will be visible in the general list without filtering,
                # so we don't need to show "New Add-ons!" button.
                if not tag_already_assigned:
                    _glyph_cache[cache_key]["pending_tag_assignment"] = True
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
                    _glyph_cache[cache_key]["discovered_in_spaces"] = flags_to_spaces(spaces_flags)
                    tag_log(
                        f"_merge_discovered_categories: set discovered_in_spaces for new category {category!r} "
                        f"to {_glyph_cache[cache_key]['discovered_in_spaces']} (from panel_samples)"
                    )

                # Use bl_context from panel_samples to determine mode filtering.
                # This is crucial for panels like "VCol Edit" that only show in EDIT mode.
                category_contexts = category_to_contexts.get(category, set())
                if category_contexts:
                    modes_flags = 0
                    for bl_ctx in category_contexts:
                        modes_flags |= BL_CONTEXT_TO_MODE_FLAG.get(bl_ctx, 0)
                    if modes_flags:
                        _glyph_cache[cache_key]["discovered_in_modes"] = flags_to_modes(modes_flags)
                        tag_log(
                            f"_merge_discovered_categories: set discovered_in_modes for new category {category!r} "
                            f"to {_glyph_cache[cache_key]['discovered_in_modes']} (from bl_context={category_contexts})"
                        )

                if ext_mode and not _glyph_cache[cache_key].get("discovered_in_modes"):
                    _glyph_cache[cache_key]["discovered_in_modes"] = flags_to_modes(ext_mode)

                # Store the install mode flag for mode-aware filtering of "New Add-ons!" button.
                # This is used when discovered_in_modes is empty (panels don't specify bl_context).
                # The category should only show "New Add-ons!" in the mode where it was installed.
                if ext_mode:
                    _glyph_cache[cache_key]["install_mode_flag"] = ext_mode
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
                if category in _icon_detection_session_checked:
                    category_debug_print(f"[ICON SESSION CACHE] Skipping already-checked new category {category!r}")
                else:
                    # Check if we have a cached icon path from Install from Disk
                    icon_cache = getattr(extension_post_install_handler, "_icon_cache", {})
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
                    _icon_detection_session_checked.add(category)
            else:
                category_debug_print(f"[ICON DETECTION] Skipping icon detection for new category {category!r} (will run in background)")

            if detected_icon_path:
                _glyph_cache[cache_key]["icon_path"] = detected_icon_path
                _glyph_cache[cache_key]["icon_provider"] = detected_provider or "extension_auto"
                category_debug_print(
                    f"[] merge new category auto-icon: "
                    f"category={category!r}, path={detected_icon_path!r}, provider={_glyph_cache[cache_key]['icon_provider']!r}"
                )
            else:
                category_debug_print(f"[] merge new category no icon: category={category!r}")

            category_debug_print(f"[GLYPH] Added new category '{category}' with glyph '{glyph}', base_type={base_type}")
            category_debug_print(f"[NEW CATEGORY DEBUG] Cache entry for '{category}': source_extension='{ext_id}', pending_tag_assignment={_glyph_cache[cache_key].get('pending_tag_assignment', False)}, discovered_in_spaces={_glyph_cache[cache_key].get('discovered_in_spaces', [])}")
            category_debug_print(f"[NEW CATEGORY DEBUG] Total cache size after adding '{category}': {len(_glyph_cache)} categories")

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
