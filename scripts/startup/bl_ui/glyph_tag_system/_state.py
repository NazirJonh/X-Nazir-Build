# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Single owner of all mutable state for the Category Tabs / Glyph / Tag system.

Historically the caches and flags lived as module-level globals in ``space_userpref.py``
and were mutated through ``global`` declarations scattered across ~68 functions. That made
it impossible to extract any one subsystem (tag CRUD, discovery, WM sync, persistence)
without creating a cyclic import or silently breaking the shared reference.

This module is the **single owner** of that state. Two access patterns are supported:

1. **In-place mutation** (preferred, zero call-site changes): import the object directly.
   ``from bl_ui.glyph_tag_system._state import _glyph_cache`` then ``_glyph_cache[key] = value``
   mutates the shared dict through Python's reference semantics. The same holds for
   ``.append()``, ``.clear()``, ``.discard()``, ``.update()`` etc.

2. **Full reassignment** (``X = {}``): this would create a *new* object and orphan every
   existing reference. Such call sites MUST go through the accessor helpers below
   (``reset_glyph_cache()``, ``set_tag_order(...)``, ...), which perform the reassignment
   inside this module so all importers observe the new value.

The names are intentionally kept identical so that ``space_userpref.py`` can re-export them
verbatim (``from bl_ui.glyph_tag_system._state import _glyph_cache``) and preserve its
existing attribute contract for the C++ bridge and the editor modules.
"""

# ---------------------------------------------------------------------------
# Core caches (the source of truth for category/tag data)
# ---------------------------------------------------------------------------

# {cache_key: cat_data} where cache_key == (-1, category_name) (Global-First architecture).
# Mutated in-place by most of the system; full reset only via reset_glyph_cache().
_glyph_cache = {}

# True once _load_glyph_mappings_from_file() has populated the caches. Guard used by lazy
# loaders to avoid re-entrancy.
_glyph_cache_loaded = False

# {tag_name: {"glyph", "color", "mode_flags", "icon_key", "icon_source"}}.
_all_tags_cache = {}

# Manual tag display order (list of tag names). New tags are appended.
_tag_order_cache = []

# {tag_combination_key: [category_name, ...]} — per-filter category order overrides.
_category_orders_cache = {}

# True while the edit dialog suppresses mappings sync (until the user clicks Save).
_preview_mode_active = False

# ---------------------------------------------------------------------------
# Extension / discovery state
# ---------------------------------------------------------------------------

# Pending extension install context: {"extension_id", "space_type", "mode_flag"} or None.
_pending_extension_context = None

# Set True right after an on-disk extension install so discovery can attribute categories.
_install_from_disk_just_occurred = False

# Accumulator for merge_discovered_categories(): discovered categories not yet committed.
# Note: initial structure mirrors the one historically authored in space_userpref.py so that
# readers that key off these fields keep working without a separate initialisation step.
_merge_discovery_cache = {
    "last_discovered": None,
    "last_panel_samples": None,
    "last_addon_count": 0,
    "last_extension_count": 0,
    "last_check_time": 0,
    "cache_valid": False,
}

# {category: (icon_path, provider, timestamp)} — auto-detected extension icons.
_icon_detection_cache = {}

# Categories already checked for icon detection in the current merge session.
_icon_detection_session_checked = set()

# Handles for the debounced background discovery (None when idle).
_discovery_debounce_timer = None

# Reentrancy guard set while inside a UI draw pass (prevents discovery re-entry).
_in_ui_draw = False

# {(ext_id, scan_python_files): {"keys", "timestamp", "pkg_path"}}.
_extension_manifest_keys_cache = {}

# {category_name: extension_id} — reverse map populated during discovery.
_last_discovered_ext_panel_categories = {}

# {category_name: source_label} — provenance of each discovered category.
_last_discovered_category_sources = {}

# {extension_id or pkg_path: icon_path} — icons found by extension_post_install_handler()
# (install-time disk scan + Popular Addons Database fallback), consumed by discovery merge to
# attach icons to new/existing categories without re-scanning disk. Was previously stashed as
# an ad-hoc attribute on the handler function itself; owned here so scan and merge helpers can
# share it without either importing the other's module.
_extension_install_icon_cache = {}

# ---------------------------------------------------------------------------
# Sync / background-task state
# ---------------------------------------------------------------------------

# Reentrancy guard for sync_glyph_mappings_to_wm().
_sync_in_progress = False

# True after the first initial WM sync, so handlers can short-circuit.
_initial_load_complete = False

# Timer handle for the deferred _auto_sync_to_wm() run.
_auto_sync_timer = None

# Timer handle for the background discovery sync.
_background_sync_timer = None

# Number of background sync runs performed (for self-throttling / debugging).
_background_sync_run_count = 0

# ---------------------------------------------------------------------------
# Save state
# ---------------------------------------------------------------------------

# True when a tag save has been deferred (debounced) and is awaiting a timer tick.
_auto_save_pending = False

# True when a glyph-mappings save has been deferred.
_auto_save_glyph_pending = False

# When True, the deferred glyph save must skip the WM sync step.
_auto_save_glyph_skip_wm_sync = False

# Mutex preventing parallel file writes (atomic-save guard).
_glyph_save_lock = False

# Pending (tag_name, icon_source) to apply on the next debounce tick, or None.
_pending_display_mode_change = None

# True while the display-mode debounce timer is armed.
_display_mode_debounce_timer_running = False

# ---------------------------------------------------------------------------
# Registration flag (set by space_userpref.register())
# ---------------------------------------------------------------------------

_glyph_system_registered = False


# ===========================================================================
# Reassignment accessors
#
# Call these instead of ``X = ...`` for any state object that other modules
# import by reference. They mutate the owner module so every importer sees the
# new value.
# ===========================================================================

def reset_glyph_cache(new_cache=None):
    """Replace the glyph cache. Pass ``None`` for an empty dict, or a pre-built
    mapping (e.g. ``DEFAULT_CATEGORY_GLYPHS.copy()``) to seed defaults.
    """
    global _glyph_cache
    _glyph_cache = new_cache if new_cache is not None else {}


def reset_all_tags_cache(new_cache=None):
    """Replace the all-tags cache (defaults to an empty dict)."""
    global _all_tags_cache
    _all_tags_cache = new_cache if new_cache is not None else {}


def set_tag_order(order):
    """Replace the manual tag display order with ``order`` (a list of tag names)."""
    global _tag_order_cache
    _tag_order_cache = list(order)


def reset_category_orders_cache(new_cache=None):
    """Replace the category-orders cache (defaults to an empty dict)."""
    global _category_orders_cache
    _category_orders_cache = new_cache if new_cache is not None else {}


def reset_icon_detection_session_checked():
    """Clear the per-session icon-detection tracker (start of a new merge session)."""
    global _icon_detection_session_checked
    _icon_detection_session_checked = set()


def reset_last_discovered_ext_panel_categories():
    """Clear the category -> extension_id reverse map."""
    global _last_discovered_ext_panel_categories
    _last_discovered_ext_panel_categories = {}


def set_last_discovered_category_sources(new_sources):
    """Replace the category provenance map with ``new_sources``."""
    global _last_discovered_category_sources
    _last_discovered_category_sources = new_sources


def reset_last_discovered_category_sources():
    """Clear the category provenance map."""
    global _last_discovered_category_sources
    _last_discovered_category_sources = {}


def set_glyph_cache_loaded(value):
    """Mark the caches as loaded (or not). ``value`` must be a bool."""
    global _glyph_cache_loaded
    _glyph_cache_loaded = bool(value)


def set_preview_mode_active(value):
    """Enable/disable preview mode (suppresses mappings sync until Save)."""
    global _preview_mode_active
    _preview_mode_active = bool(value)


def set_pending_extension_context(value):
    """Set the pending extension install context (dict or None)."""
    global _pending_extension_context
    _pending_extension_context = value


def set_install_from_disk_just_occurred(value):
    """Toggle the 'on-disk install just happened' discovery flag."""
    global _install_from_disk_just_occurred
    _install_from_disk_just_occurred = bool(value)


def set_sync_in_progress(value):
    """Toggle the WM-sync reentrancy guard."""
    global _sync_in_progress
    _sync_in_progress = bool(value)


def set_initial_load_complete(value):
    """Mark the initial load/sync as complete."""
    global _initial_load_complete
    _initial_load_complete = bool(value)


def set_auto_sync_timer(timer):
    """Store the deferred auto-sync timer handle (or None)."""
    global _auto_sync_timer
    _auto_sync_timer = timer


def set_background_sync_timer(timer):
    """Store the background sync timer handle (or None)."""
    global _background_sync_timer
    _background_sync_timer = timer


def increment_background_sync_run_count():
    """Increment the background sync run counter; returns the new value."""
    global _background_sync_run_count
    _background_sync_run_count += 1
    return _background_sync_run_count


def reset_background_sync_run_count():
    """Reset the background sync run counter to zero."""
    global _background_sync_run_count
    _background_sync_run_count = 0


def set_auto_save_pending(value):
    """Toggle the deferred tag-save flag."""
    global _auto_save_pending
    _auto_save_pending = bool(value)


def set_auto_save_glyph_pending(value):
    """Toggle the deferred glyph-mappings-save flag."""
    global _auto_save_glyph_pending
    _auto_save_glyph_pending = bool(value)


def set_auto_save_glyph_skip_wm_sync(value):
    """Toggle whether the deferred glyph save should skip WM sync."""
    global _auto_save_glyph_skip_wm_sync
    _auto_save_glyph_skip_wm_sync = bool(value)


def set_glyph_save_lock(value):
    """Acquire/release the atomic-save mutex."""
    global _glyph_save_lock
    _glyph_save_lock = bool(value)


def set_pending_display_mode_change(value):
    """Set the pending (tag_name, icon_source) tuple, or None to clear."""
    global _pending_display_mode_change
    _pending_display_mode_change = value


def set_display_mode_debounce_timer_running(value):
    """Toggle the display-mode debounce timer armed flag."""
    global _display_mode_debounce_timer_running
    _display_mode_debounce_timer_running = bool(value)


def set_discovery_debounce_timer(timer):
    """Store the discovery debounce timer handle (or None)."""
    global _discovery_debounce_timer
    _discovery_debounce_timer = timer


def set_in_ui_draw(value):
    """Toggle the UI-draw reentrancy guard."""
    global _in_ui_draw
    _in_ui_draw = bool(value)


def set_glyph_system_registered(value):
    """Toggle the system-registered flag (used by space_userpref.register/unregister)."""
    global _glyph_system_registered
    _glyph_system_registered = bool(value)


def reset_merge_discovery_cache():
    """Clear the merge-discovery accumulator."""
    global _merge_discovery_cache
    _merge_discovery_cache = {}


def set_extension_manifest_keys_cache(key, value):
    """Set one entry in the extension manifest keys cache."""
    _extension_manifest_keys_cache[key] = value


def set_extension_install_icon_cache_entry(key, value):
    """Set one entry in the extension install-time icon cache."""
    _extension_install_icon_cache[key] = value


def reset_icon_detection_cache():
    """Clear the auto-detected extension icon cache."""
    global _icon_detection_cache
    _icon_detection_cache = {}


# ===========================================================================
# Batch reset — used at the start of _load_glyph_mappings_from_file()
# ===========================================================================

def reset_all_caches_for_load():
    """Clear every data cache before a fresh load from disk.

    Centralises the reassignments that ``_load_glyph_mappings_from_file`` previously
    scattered as ``global`` declarations, so they all touch this owner module.
    """
    global _glyph_cache, _all_tags_cache, _tag_order_cache, _category_orders_cache
    _glyph_cache = {}
    _all_tags_cache = {}
    _tag_order_cache = []
    _category_orders_cache = {}


# ===========================================================================
# Scalar getters
#
# Reassignable scalar flags (bool/None) MUST be read through these getters.
# ``from ._state import _glyph_cache_loaded`` binds the value that existed at
# import time; after ``set_glyph_cache_loaded(True)`` mutates this module, the
# importer still observes the stale copy. That desync is the root cause of the
# glyph-cache being reloaded on every UI draw (and of fresh edits being wiped
# by a reload that runs between an edit and its save). Reading through a getter
# always resolves against this owner module, so importers see the live value.
# ===========================================================================

def is_glyph_cache_loaded():
    """Live read of the ``_glyph_cache_loaded`` guard (used by lazy loaders)."""
    return _glyph_cache_loaded


def is_glyph_save_locked():
    """Live read of the ``_glyph_save_lock`` atomic-save mutex."""
    return _glyph_save_lock


def is_preview_mode_active():
    """Live read of ``_preview_mode_active`` (suppresses mappings sync until Save)."""
    return _preview_mode_active


def is_sync_in_progress():
    """Live read of the ``_sync_in_progress`` WM-sync reentrancy guard."""
    return _sync_in_progress


def is_initial_load_complete():
    """Live read of the ``_initial_load_complete`` flag."""
    return _initial_load_complete


def is_auto_save_pending():
    """Live read of the deferred tag-save flag."""
    return _auto_save_pending


def is_auto_save_glyph_pending():
    """Live read of the deferred glyph-mappings-save flag."""
    return _auto_save_glyph_pending


def get_auto_save_glyph_skip_wm_sync():
    """Live read of the deferred glyph-save WM-sync skip flag."""
    return _auto_save_glyph_skip_wm_sync


def is_in_ui_draw():
    """Live read of the UI-draw reentrancy guard."""
    return _in_ui_draw


def is_install_from_disk_just_occurred():
    """Live read of the 'on-disk install just happened' discovery flag."""
    return _install_from_disk_just_occurred


def is_display_mode_debounce_timer_running():
    """Live read of the display-mode debounce timer armed flag."""
    return _display_mode_debounce_timer_running


def is_glyph_system_registered():
    """Live read of the system-registered flag."""
    return _glyph_system_registered


def get_pending_display_mode_change():
    """Live read of the pending (tag_name, icon_source) tuple, or None."""
    return _pending_display_mode_change


def get_auto_sync_timer():
    """Live read of the deferred auto-sync timer handle (or None)."""
    return _auto_sync_timer


def get_background_sync_timer():
    """Live read of the background discovery sync timer handle (or None)."""
    return _background_sync_timer


def get_background_sync_run_count():
    """Live read of the background sync run counter."""
    return _background_sync_run_count


def get_discovery_debounce_timer():
    """Live read of the discovery debounce timer handle (or None)."""
    return _discovery_debounce_timer


def get_pending_extension_context():
    """Live read of the pending extension install context (dict or None)."""
    return _pending_extension_context


def get_merge_discovery_cache():
    """Live read of the merge-discovery accumulator dict.

    Returned by reference so in-place mutations (``cache["last_check_time"] = ...``)
    land on the single owner object. Use ``reset_merge_discovery_cache()`` for a
    full reassignment.
    """
    return _merge_discovery_cache
