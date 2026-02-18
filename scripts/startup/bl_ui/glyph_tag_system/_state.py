# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Single owner of all mutable state for the Category Tabs / Glyph / Tag system.

Historically the caches and flags lived as module-level globals in ``space_userpref.py``
and were mutated through ``global`` declarations scattered across ~68 functions. That made
it impossible to extract any one subsystem (tag CRUD, discovery, WM sync, persistence)
without creating a cyclic import or silently breaking the shared reference.

This module is the **single owner** of that state, held as one singleton instance
(``state``) of ``_GlyphTagState``. Every field is a property:

* **Containers** (dicts/lists/sets) expose their live object through the getter, so
  in-place mutation (``state.glyph_cache[key] = value``, ``.append()``, ``.clear()``,
  ``.update()`` ...) works exactly like a plain module global. The **setter never
  replaces the object** — ``state.glyph_cache = {}`` clears and refills the *same*
  dict in place. This is deliberate: earlier, reset helpers did ``global X; X = {}``,
  which rebound only this module's own name. Every other module that had already
  imported the old object by reference (``from _state import _glyph_cache``) kept
  observing the *stale* pre-reset object forever — a real bug: reordering tags
  (``set_tag_order()``) silently failed to persist, because ``operators.py`` and
  ``handlers.py`` never saw the replacement list. Mutating in place means object
  identity never changes, so every reference — however it was obtained, whenever it
  was captured — always reflects the current value.
* **Scalars** (bool/int/None/callables) are plain get/set through the property; there
  is no container-identity concern since they are read fresh from the singleton on
  every access.

The pre-existing free functions (``reset_glyph_cache()``, ``is_preview_mode_active()``,
...) are kept as thin wrappers around ``state`` so every existing call site elsewhere
in the package keeps working unchanged.
"""


class _GlyphTagState:
    """Single owner instance; see module docstring. Access via the ``state`` singleton below."""

    def __init__(self):
        # -- Core caches (the source of truth for category/tag data) --------
        # {cache_key: cat_data} where cache_key == (-1, category_name) (Global-First architecture).
        self._glyph_cache = {}
        # True once _load_glyph_mappings_from_file() has populated the caches. Guard used by lazy
        # loaders to avoid re-entrancy.
        self._glyph_cache_loaded = False
        # {tag_name: {"glyph", "color", "mode_flags", "icon_key", "icon_source"}}.
        self._all_tags_cache = {}
        # Manual tag display order (list of tag names). New tags are appended.
        self._tag_order_cache = []
        # {tag_combination_key: [category_name, ...]} — per-filter category order overrides.
        self._category_orders_cache = {}
        # True while the edit dialog suppresses mappings sync (until the user clicks Save).
        self._preview_mode_active = False

        # -- Extension / discovery state -------------------------------------
        # Pending extension install context: {"extension_id", "space_type", "mode_flag"} or None.
        self._pending_extension_context = None
        # Set True right after an on-disk extension install so discovery can attribute categories.
        self._install_from_disk_just_occurred = False
        # Accumulator for merge_discovered_categories(): discovered categories not yet committed.
        self._merge_discovery_cache = {
            "last_discovered": None,
            "last_panel_samples": None,
            "last_addon_count": 0,
            "last_extension_count": 0,
            "last_check_time": 0,
            "cache_valid": False,
        }
        # {category: (icon_path, provider, timestamp)} — auto-detected extension icons.
        self._icon_detection_cache = {}
        # Categories already checked for icon detection in the current merge session.
        self._icon_detection_session_checked = set()
        # Handles for the debounced background discovery (None when idle).
        self._discovery_debounce_timer = None
        # Reentrancy guard set while inside a UI draw pass (prevents discovery re-entry).
        self._in_ui_draw = False
        # {(ext_id, scan_python_files): {"keys", "timestamp", "pkg_path"}}.
        self._extension_manifest_keys_cache = {}
        # {category_name: extension_id} — reverse map populated during discovery.
        self._last_discovered_ext_panel_categories = {}
        # {category_name: source_label} — provenance of each discovered category.
        self._last_discovered_category_sources = {}
        # {extension_id or pkg_path: icon_path} — icons found by extension_post_install_handler()
        # (install-time disk scan + Popular Addons Database fallback), consumed by discovery merge
        # to attach icons to new/existing categories without re-scanning disk.
        self._extension_install_icon_cache = {}

        # -- Sync / background-task state ------------------------------------
        # Reentrancy guard for sync_glyph_mappings_to_wm().
        self._sync_in_progress = False
        # True after the first initial WM sync, so handlers can short-circuit.
        self._initial_load_complete = False
        # Timer handle for the deferred _auto_sync_to_wm() run.
        self._auto_sync_timer = None
        # Timer handle for the background discovery sync.
        self._background_sync_timer = None
        # Number of background sync runs performed (for self-throttling / debugging).
        self._background_sync_run_count = 0

        # -- Save state --------------------------------------------------------
        # True when a tag save has been deferred (debounced) and is awaiting a timer tick.
        self._auto_save_pending = False
        # True when a glyph-mappings save has been deferred.
        self._auto_save_glyph_pending = False
        # When True, the deferred glyph save must skip the WM sync step.
        self._auto_save_glyph_skip_wm_sync = False
        # Mutex preventing parallel file writes (atomic-save guard).
        self._glyph_save_lock = False
        # Pending (tag_name, icon_source) to apply on the next debounce tick, or None.
        self._pending_display_mode_change = None
        # True while the display-mode debounce timer is armed.
        self._display_mode_debounce_timer_running = False

        # -- Registration flag (set by space_userpref.register()) --------------
        self._glyph_system_registered = False

    # =========================================================================
    # Container properties — setter always mutates the existing object in place.
    # =========================================================================

    @property
    def glyph_cache(self):
        return self._glyph_cache

    @glyph_cache.setter
    def glyph_cache(self, new_cache):
        self._glyph_cache.clear()
        if new_cache:
            self._glyph_cache.update(new_cache)

    @property
    def all_tags_cache(self):
        return self._all_tags_cache

    @all_tags_cache.setter
    def all_tags_cache(self, new_cache):
        self._all_tags_cache.clear()
        if new_cache:
            self._all_tags_cache.update(new_cache)

    @property
    def tag_order_cache(self):
        return self._tag_order_cache

    @tag_order_cache.setter
    def tag_order_cache(self, order):
        self._tag_order_cache[:] = list(order) if order else []

    @property
    def category_orders_cache(self):
        return self._category_orders_cache

    @category_orders_cache.setter
    def category_orders_cache(self, new_cache):
        self._category_orders_cache.clear()
        if new_cache:
            self._category_orders_cache.update(new_cache)

    @property
    def merge_discovery_cache(self):
        """Returned by reference so in-place mutations (``cache["last_check_time"] = ...``)
        land on the single owner object."""
        return self._merge_discovery_cache

    @merge_discovery_cache.setter
    def merge_discovery_cache(self, new_cache):
        self._merge_discovery_cache.clear()
        if new_cache:
            self._merge_discovery_cache.update(new_cache)

    @property
    def icon_detection_cache(self):
        return self._icon_detection_cache

    @icon_detection_cache.setter
    def icon_detection_cache(self, new_cache):
        self._icon_detection_cache.clear()
        if new_cache:
            self._icon_detection_cache.update(new_cache)

    @property
    def icon_detection_session_checked(self):
        return self._icon_detection_session_checked

    @icon_detection_session_checked.setter
    def icon_detection_session_checked(self, new_set):
        self._icon_detection_session_checked.clear()
        if new_set:
            self._icon_detection_session_checked.update(new_set)

    @property
    def extension_manifest_keys_cache(self):
        return self._extension_manifest_keys_cache

    @extension_manifest_keys_cache.setter
    def extension_manifest_keys_cache(self, new_cache):
        self._extension_manifest_keys_cache.clear()
        if new_cache:
            self._extension_manifest_keys_cache.update(new_cache)

    @property
    def last_discovered_ext_panel_categories(self):
        return self._last_discovered_ext_panel_categories

    @last_discovered_ext_panel_categories.setter
    def last_discovered_ext_panel_categories(self, new_map):
        self._last_discovered_ext_panel_categories.clear()
        if new_map:
            self._last_discovered_ext_panel_categories.update(new_map)

    @property
    def last_discovered_category_sources(self):
        return self._last_discovered_category_sources

    @last_discovered_category_sources.setter
    def last_discovered_category_sources(self, new_map):
        self._last_discovered_category_sources.clear()
        if new_map:
            self._last_discovered_category_sources.update(new_map)

    @property
    def extension_install_icon_cache(self):
        return self._extension_install_icon_cache

    @extension_install_icon_cache.setter
    def extension_install_icon_cache(self, new_cache):
        self._extension_install_icon_cache.clear()
        if new_cache:
            self._extension_install_icon_cache.update(new_cache)

    # =========================================================================
    # Scalar properties — plain get/set, always resolved live against the singleton.
    # =========================================================================

    @property
    def glyph_cache_loaded(self):
        return self._glyph_cache_loaded

    @glyph_cache_loaded.setter
    def glyph_cache_loaded(self, value):
        self._glyph_cache_loaded = bool(value)

    @property
    def preview_mode_active(self):
        return self._preview_mode_active

    @preview_mode_active.setter
    def preview_mode_active(self, value):
        self._preview_mode_active = bool(value)

    @property
    def pending_extension_context(self):
        return self._pending_extension_context

    @pending_extension_context.setter
    def pending_extension_context(self, value):
        self._pending_extension_context = value

    @property
    def install_from_disk_just_occurred(self):
        return self._install_from_disk_just_occurred

    @install_from_disk_just_occurred.setter
    def install_from_disk_just_occurred(self, value):
        self._install_from_disk_just_occurred = bool(value)

    @property
    def discovery_debounce_timer(self):
        return self._discovery_debounce_timer

    @discovery_debounce_timer.setter
    def discovery_debounce_timer(self, timer):
        self._discovery_debounce_timer = timer

    @property
    def in_ui_draw(self):
        return self._in_ui_draw

    @in_ui_draw.setter
    def in_ui_draw(self, value):
        self._in_ui_draw = bool(value)

    @property
    def sync_in_progress(self):
        return self._sync_in_progress

    @sync_in_progress.setter
    def sync_in_progress(self, value):
        self._sync_in_progress = bool(value)

    @property
    def initial_load_complete(self):
        return self._initial_load_complete

    @initial_load_complete.setter
    def initial_load_complete(self, value):
        self._initial_load_complete = bool(value)

    @property
    def auto_sync_timer(self):
        return self._auto_sync_timer

    @auto_sync_timer.setter
    def auto_sync_timer(self, timer):
        self._auto_sync_timer = timer

    @property
    def background_sync_timer(self):
        return self._background_sync_timer

    @background_sync_timer.setter
    def background_sync_timer(self, timer):
        self._background_sync_timer = timer

    @property
    def background_sync_run_count(self):
        return self._background_sync_run_count

    @background_sync_run_count.setter
    def background_sync_run_count(self, value):
        self._background_sync_run_count = value

    @property
    def auto_save_pending(self):
        return self._auto_save_pending

    @auto_save_pending.setter
    def auto_save_pending(self, value):
        self._auto_save_pending = bool(value)

    @property
    def auto_save_glyph_pending(self):
        return self._auto_save_glyph_pending

    @auto_save_glyph_pending.setter
    def auto_save_glyph_pending(self, value):
        self._auto_save_glyph_pending = bool(value)

    @property
    def auto_save_glyph_skip_wm_sync(self):
        return self._auto_save_glyph_skip_wm_sync

    @auto_save_glyph_skip_wm_sync.setter
    def auto_save_glyph_skip_wm_sync(self, value):
        self._auto_save_glyph_skip_wm_sync = bool(value)

    @property
    def glyph_save_lock(self):
        return self._glyph_save_lock

    @glyph_save_lock.setter
    def glyph_save_lock(self, value):
        self._glyph_save_lock = bool(value)

    @property
    def pending_display_mode_change(self):
        return self._pending_display_mode_change

    @pending_display_mode_change.setter
    def pending_display_mode_change(self, value):
        self._pending_display_mode_change = value

    @property
    def display_mode_debounce_timer_running(self):
        return self._display_mode_debounce_timer_running

    @display_mode_debounce_timer_running.setter
    def display_mode_debounce_timer_running(self, value):
        self._display_mode_debounce_timer_running = bool(value)

    @property
    def glyph_system_registered(self):
        return self._glyph_system_registered

    @glyph_system_registered.setter
    def glyph_system_registered(self, value):
        self._glyph_system_registered = bool(value)


# Single owner instance. Import this and use ``state.<field>`` for all access.
state = _GlyphTagState()


# ===========================================================================
# Reassignment accessors — thin wrappers preserved for existing call sites.
#
# Call these instead of ``X = ...`` for any state object that other modules
# import by reference. They resolve against the ``state`` singleton above,
# which mutates containers in place (see module docstring).
# ===========================================================================

def reset_glyph_cache(new_cache=None):
    """Replace the glyph cache. Pass ``None`` for an empty dict, or a pre-built
    mapping (e.g. ``DEFAULT_CATEGORY_GLYPHS.copy()``) to seed defaults.
    """
    state.glyph_cache = new_cache


def reset_all_tags_cache(new_cache=None):
    """Replace the all-tags cache (defaults to an empty dict)."""
    state.all_tags_cache = new_cache


def set_tag_order(order):
    """Replace the manual tag display order with ``order`` (a list of tag names)."""
    state.tag_order_cache = order


def reset_category_orders_cache(new_cache=None):
    """Replace the category-orders cache (defaults to an empty dict)."""
    state.category_orders_cache = new_cache


def reset_icon_detection_session_checked():
    """Clear the per-session icon-detection tracker (start of a new merge session)."""
    state.icon_detection_session_checked = None


def reset_last_discovered_ext_panel_categories():
    """Clear the category -> extension_id reverse map."""
    state.last_discovered_ext_panel_categories = None


def set_last_discovered_category_sources(new_sources):
    """Replace the category provenance map with ``new_sources``."""
    state.last_discovered_category_sources = new_sources


def reset_last_discovered_category_sources():
    """Clear the category provenance map."""
    state.last_discovered_category_sources = None


def set_glyph_cache_loaded(value):
    """Mark the caches as loaded (or not). ``value`` must be a bool."""
    state.glyph_cache_loaded = value


def set_preview_mode_active(value):
    """Enable/disable preview mode (suppresses mappings sync until Save)."""
    state.preview_mode_active = value


def set_pending_extension_context(value):
    """Set the pending extension install context (dict or None)."""
    state.pending_extension_context = value


def set_install_from_disk_just_occurred(value):
    """Toggle the 'on-disk install just happened' discovery flag."""
    state.install_from_disk_just_occurred = value


def set_sync_in_progress(value):
    """Toggle the WM-sync reentrancy guard."""
    state.sync_in_progress = value


def set_initial_load_complete(value):
    """Mark the initial load/sync as complete."""
    state.initial_load_complete = value


def set_auto_sync_timer(timer):
    """Store the deferred auto-sync timer handle (or None)."""
    state.auto_sync_timer = timer


def set_background_sync_timer(timer):
    """Store the background sync timer handle (or None)."""
    state.background_sync_timer = timer


def increment_background_sync_run_count():
    """Increment the background sync run counter; returns the new value."""
    state.background_sync_run_count += 1
    return state.background_sync_run_count


def reset_background_sync_run_count():
    """Reset the background sync run counter to zero."""
    state.background_sync_run_count = 0


def set_auto_save_pending(value):
    """Toggle the deferred tag-save flag."""
    state.auto_save_pending = value


def set_auto_save_glyph_pending(value):
    """Toggle the deferred glyph-mappings-save flag."""
    state.auto_save_glyph_pending = value


def set_auto_save_glyph_skip_wm_sync(value):
    """Toggle whether the deferred glyph save should skip WM sync."""
    state.auto_save_glyph_skip_wm_sync = value


def set_glyph_save_lock(value):
    """Acquire/release the atomic-save mutex."""
    state.glyph_save_lock = value


def set_pending_display_mode_change(value):
    """Set the pending (tag_name, icon_source) tuple, or None to clear."""
    state.pending_display_mode_change = value


def set_display_mode_debounce_timer_running(value):
    """Toggle the display-mode debounce timer armed flag."""
    state.display_mode_debounce_timer_running = value


def set_discovery_debounce_timer(timer):
    """Store the discovery debounce timer handle (or None)."""
    state.discovery_debounce_timer = timer


def set_in_ui_draw(value):
    """Toggle the UI-draw reentrancy guard."""
    state.in_ui_draw = value


def set_glyph_system_registered(value):
    """Toggle the system-registered flag (used by space_userpref.register/unregister)."""
    state.glyph_system_registered = value


def reset_merge_discovery_cache():
    """Clear the merge-discovery accumulator."""
    state.merge_discovery_cache = None


def set_extension_manifest_keys_cache(key, value):
    """Set one entry in the extension manifest keys cache."""
    state.extension_manifest_keys_cache[key] = value


def set_extension_install_icon_cache_entry(key, value):
    """Set one entry in the extension install-time icon cache."""
    state.extension_install_icon_cache[key] = value


def reset_icon_detection_cache():
    """Clear the auto-detected extension icon cache."""
    state.icon_detection_cache = None


# ===========================================================================
# Batch reset — used at the start of _load_glyph_mappings_from_file()
# ===========================================================================

def reset_all_caches_for_load():
    """Clear every data cache before a fresh load from disk.

    Centralises the reassignments that ``_load_glyph_mappings_from_file`` previously
    scattered as ``global`` declarations, so they all touch this owner module.
    """
    state.glyph_cache = None
    state.all_tags_cache = None
    state.tag_order_cache = None
    state.category_orders_cache = None


# ===========================================================================
# Scalar getters
#
# Reassignable scalar flags (bool/None) MUST be read through these getters.
# Reading through a getter always resolves against the live ``state`` singleton,
# so importers never see a stale snapshot bound at their own import time.
# ===========================================================================

def is_glyph_cache_loaded():
    """Live read of the ``glyph_cache_loaded`` guard (used by lazy loaders)."""
    return state.glyph_cache_loaded


def is_glyph_save_locked():
    """Live read of the ``glyph_save_lock`` atomic-save mutex."""
    return state.glyph_save_lock


def is_preview_mode_active():
    """Live read of ``preview_mode_active`` (suppresses mappings sync until Save)."""
    return state.preview_mode_active


def is_sync_in_progress():
    """Live read of the ``sync_in_progress`` WM-sync reentrancy guard."""
    return state.sync_in_progress


def is_initial_load_complete():
    """Live read of the ``initial_load_complete`` flag."""
    return state.initial_load_complete


def is_auto_save_pending():
    """Live read of the deferred tag-save flag."""
    return state.auto_save_pending


def is_auto_save_glyph_pending():
    """Live read of the deferred glyph-mappings-save flag."""
    return state.auto_save_glyph_pending


def get_auto_save_glyph_skip_wm_sync():
    """Live read of the deferred glyph-save WM-sync skip flag."""
    return state.auto_save_glyph_skip_wm_sync


def is_in_ui_draw():
    """Live read of the UI-draw reentrancy guard."""
    return state.in_ui_draw


def is_install_from_disk_just_occurred():
    """Live read of the 'on-disk install just happened' discovery flag."""
    return state.install_from_disk_just_occurred


def is_display_mode_debounce_timer_running():
    """Live read of the display-mode debounce timer armed flag."""
    return state.display_mode_debounce_timer_running


def is_glyph_system_registered():
    """Live read of the system-registered flag."""
    return state.glyph_system_registered


def get_pending_display_mode_change():
    """Live read of the pending (tag_name, icon_source) tuple, or None."""
    return state.pending_display_mode_change


def get_auto_sync_timer():
    """Live read of the deferred auto-sync timer handle (or None)."""
    return state.auto_sync_timer


def get_background_sync_timer():
    """Live read of the background discovery sync timer handle (or None)."""
    return state.background_sync_timer


def get_background_sync_run_count():
    """Live read of the background sync run counter."""
    return state.background_sync_run_count


def get_discovery_debounce_timer():
    """Live read of the discovery debounce timer handle (or None)."""
    return state.discovery_debounce_timer


def get_pending_extension_context():
    """Live read of the pending extension install context (dict or None)."""
    return state.pending_extension_context


def get_merge_discovery_cache():
    """Live read of the merge-discovery accumulator dict.

    Returned by reference so in-place mutations (``cache["last_check_time"] = ...``)
    land on the single owner object. Use ``reset_merge_discovery_cache()`` for a
    full reassignment.
    """
    return state.merge_discovery_cache
