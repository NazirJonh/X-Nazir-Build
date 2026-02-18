# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Read-only extension/category discovery helpers for the Tabs-System.

Split out of ``discovery.py`` (Candidate 6): this module contains the "scan" half —
functions that inspect the filesystem, registered panels, and manifests, but never
decide how discovered data should be merged into the glyph cache. It has zero
dependency on ``wm_sync`` or ``discovery_merge``, making it a leaf module.

Contains:
- Icon file scanning (on-disk + auto-detection with caching).
- Extension ID / manifest matching helpers (used by discovery_merge).
- ``_discover_active_categories`` — scans registered panels/extensions for categories.
"""

import bpy
import os
import re
import time

from bl_ui.glyph_tag_system.conversions import (
    _is_single_edit_apart,
    _is_single_glyph,
    _normalize_category_key,
)
from bl_ui.glyph_tag_system.log import (
    _log_once,
    _pref_log_once,
    _package_match_log_once,
    category_debug_print,
)

# Single owner of shared state; ``state.<field>`` is always the live object (see _state.py).
from bl_ui.glyph_tag_system._state import state
from bl_ui.glyph_tag_system._state import (
    set_extension_install_icon_cache_entry,
    # Reassignment accessors for shared state (call these instead of `name = ...` so
    # every importer sees the new value; a direct reassignment would orphan the single
    # owner object in bl_ui.glyph_tag_system._state).
    reset_last_discovered_category_sources,
    reset_last_discovered_ext_panel_categories,
    set_last_discovered_category_sources,
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

    # PERF: Start timing
    _icon_detect_start = time.perf_counter()

    # OPTIMIZATION: Check cache first
    current_time = time.time()

    if not force_refresh and category in state.icon_detection_cache:
        cache_entry = state.icon_detection_cache[category]
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
        state.icon_detection_cache[category] = {
            "icon_path": "",
            "provider": "",
            "timestamp": current_time,
        }
        return "", ""

    if not extensions_dir or not os.path.isdir(extensions_dir):
        _pref_log_once(f"[] detect abort: category={category!r}, extensions_dir={extensions_dir!r}, exists=False")
        # Cache the negative result
        state.icon_detection_cache[category] = {
            "icon_path": "",
            "provider": "",
            "timestamp": current_time,
        }
        return "", ""

    target_key = _normalize_category_key(category)
    if not target_key:
        _pref_log_once(f"[] detect abort: category={category!r}, normalized_key is empty")
        # Cache the negative result
        state.icon_detection_cache[category] = {
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
                state.icon_detection_cache[category] = {
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

    preferred_ext_id = state.last_discovered_ext_panel_categories.get(category, "")
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
                    set_extension_install_icon_cache_entry(preferred_ext_id, detected_icon[0])
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

                if cache_key in state.extension_manifest_keys_cache:
                    match_keys = state.extension_manifest_keys_cache[cache_key]["keys"]
                    category_debug_print(f"[ICON DETECT OPTIMIZATION] Using cached manifest keys for {pkg_name!r}: {match_keys}")
                else:
                    # Fallback: scan manifest only (no Python files) if not cached
                    # This should only happen for extensions installed before the optimization
                    match_keys = _extension_manifest_match_keys(pkg_path, pkg_name, scan_python_files=False)
                    # Cache for future use
                    state.extension_manifest_keys_cache[cache_key] = {
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
        state.icon_detection_cache[category] = {
            "icon_path": "",
            "provider": "",
            "timestamp": current_time,
        }
        return "", ""

    _pref_log_once(f"[] detect miss: category={category!r}")
    # Cache the negative result
    state.icon_detection_cache[category] = {
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
                                state.last_discovered_ext_panel_categories[seed] = ext_id
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
                                            state.last_discovered_ext_panel_categories[panel_category] = ext_id
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
                                    state.last_discovered_ext_panel_categories[seed] = ext_id
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
                                                state.last_discovered_ext_panel_categories[panel_category] = ext_id
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
