# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Public facade for the Category Tabs / Glyph / Tag system.

This module is the single, explicit entry point to the system for every external
consumer:

* the C++ side (``interface_category_py_bridge.cc``, ``rna_wm.cc``,
  ``screen_ops.cc``) resolves ``bl_ui.glyph_tag_system.api.<name>``;
* the editor space modules (``space_view3d``, ``space_node``, ``space_image``)
  and any future code import what they need from here;
* ``space_userpref`` keeps only the Preferences UI and no longer acts as the
  system's aggregation hub.

It aggregates the public names from the internal submodules (``defaults``,
``conversions``, ``glyph_cache``, ``tags_cache``, ``wm_sync``, ``discovery``,
``handlers``, ``modes``, ``ordering``, ``persistence``, ``unassigned``,
``properties``, ``operators``, ``tag_ui``) plus the shared state objects owned by
``_state``. Nothing inside the package imports this module, so aggregating every
submodule here cannot create an import cycle; the internal submodules call each
other directly.

State objects that get reassigned (caches, flags) are re-exported by reference
for attribute-contract compatibility; full reassignments must always go through
the ``_state`` accessor helpers so every importer observes the new value.
"""

# -- Pure-data constants -----------------------------------------------------
from .defaults import (
    BL_CONTEXT_TO_MODE_FLAG,
    CURRENT_JSON_VERSION,
    DEFAULT_CATEGORY_GLYPHS,
    DEFAULT_TAG_GLYPH_HEX,
    GLYPHS_FILENAME,
    MODE_TO_FLAG,
    POPULAR_ADDONS_DB_ENABLED,
    RESERVED_CATEGORY_PRIORITY,
    SAVE_DEBUG,
    SPACE_TO_FLAG,
    TAG_BACKUP_ENABLED,
    TAG_DEBUG,
    _CATEGORY_TAG_ALL_MODE_FLAGS,
    _CATEGORY_TAG_DEFAULT_MODE_FLAGS,
    _CATEGORY_TAG_EDIT_MODE_MASK,
    _CATEGORY_TAG_FILTER_ENUM_TO_FLAG,
    _CATEGORY_TAG_MODES,
    _CATEGORY_TAG_MODE_FLAG_TO_NAME,
    _CATEGORY_TAG_MODE_ID_TO_BIT,
    _CATEGORY_TAG_MODE_NAME_TO_FLAG,
    _FLAG_TO_MODE,
    _FLAG_TO_SPACE,
)

# -- Pure conversions --------------------------------------------------------
from .conversions import (
    _category_order_decode,
    _category_order_encode,
    _flags_to_mode_names,
    _glyph_to_hex,
    _glyph_to_unicode_escape,
    _hex_to_glyph,
    _is_single_edit_apart,
    _is_single_glyph,
    _is_valid_category_name,
    _make_cache_key,
    _mode_names_to_flags,
    _normalize_category_key,
    _space_type_id_to_str,
    _space_type_str_to_id,
    _unicode_escape_to_glyph,
    flags_to_modes,
    flags_to_spaces,
    modes_to_flags,
    spaces_to_flags,
    _tag_display_mode_from_data,
    _tag_icon_source_from_display_mode,
    get_reserved_category_priority,
)

# -- Deduplicating logging ---------------------------------------------------
from .log import (
    _MAX_LOG_CACHE_SIZE,
    _log_once,
    _logged_messages_cache,
    _package_match_log_once,
    _package_match_logged,
    _pref_log_once,
    _pref_logged_messages,
    _sync_miss_log_once,
    _sync_miss_logged,
    _tag_logged_messages,
    category_debug_print,
    save_debug_print,
    tag_log,
)

# -- JSON schema migrations --------------------------------------------------
from .migrations import (
    _normalize_category_data,
    migrate_json_data,
)

# -- Glyph library integration (optional) ------------------------------------
try:
    from bl_ui.glyph_library import get_glyph_library
except ImportError:
    get_glyph_library = None

# -- Category glyph cache: persistence, data, setters ------------------------
from .glyph_cache import (
    _ensure_category_panel_label,
    _find_panel_label_for_category,
    _get_category_data,
    _get_glyphs_filepath,
    _integrate_glyph_library,
    _is_collection_safe,
    _is_popular_addons_database_extension,
    _is_reserved_category_name,
    _load_glyph_mappings_from_file,
    _save_glyph_mappings_to_file,
    _set_category_data_internal,
    assign_tag_to_category,
    filter_categories_by_tags,
    get_all_category_glyphs,
    get_categories_for_tag,
    get_category_data,
    get_category_display_name,
    get_category_glyph,
    get_category_glyph_data,
    get_category_icon_data,
    get_default_display_name,
    get_default_glyph,
    is_category_visible_by_tags,
    mark_all_unassigned_categories_as_without_tag,
    mark_category_from_extension,
    reset_category_glyphs_to_defaults,
    reset_category_to_defaults,
    set_category_data,
    set_category_glyph,
)

# -- Category ordering -------------------------------------------------------
from .ordering import (
    clear_category_order,
    get_all_category_orders,
    get_category_order,
    set_category_order,
)

# -- Tag CRUD and category-tag associations ----------------------------------
from .tags_cache import (
    _generate_unique_tag_name,
    _get_mode_flags_for_tag,
    _set_mode_flags_for_tag,
    _sync_single_tag_to_wm,
    _validate_icon_key,
    add_category_tag,
    create_tag,
    delete_tag,
    generate_unique_tag_name,
    get_all_tags,
    get_category_tags,
    get_tag_data,
    get_tag_name_by_index,
    get_tag_names,
    get_tags_for_category_ui,
    remove_category_tag,
    rename_tag,
    set_category_tags,
    toggle_category_tag,
    update_tag,
)

# -- Mode-flag resolution ----------------------------------------------------
from .modes import (
    _get_tag_filter_mode_flag_from_wm,
    get_current_tag_mode_flag,
)

# -- Unassigned-category predicates ("New Add-ons!" filter) ------------------
from .unassigned import (
    _get_unassigned_categories_count_for_space,
    _extension_has_tagged_category,
    _extension_has_only_reserved_categories,
)

# -- Atomic / safe JSON persistence ------------------------------------------
from .persistence import (
    safe_file_write,
    load_json_safely,
    create_backup,
)

# -- Shared state objects (imported by reference; single owner is _state) -----
# Full reassignments MUST go through the _state accessor helpers below.
from ._state import (
    _glyph_cache as _glyph_cache,
    _glyph_cache_loaded as _glyph_cache_loaded,
    _all_tags_cache as _all_tags_cache,
    _tag_order_cache as _tag_order_cache,
    _category_orders_cache as _category_orders_cache,
    _preview_mode_active as _preview_mode_active,
    _pending_extension_context as _pending_extension_context,
    _install_from_disk_just_occurred as _install_from_disk_just_occurred,
    _merge_discovery_cache as _merge_discovery_cache,
    _icon_detection_cache as _icon_detection_cache,
    _icon_detection_session_checked as _icon_detection_session_checked,
    _discovery_debounce_timer as _discovery_debounce_timer,
    _in_ui_draw as _in_ui_draw,
    _extension_manifest_keys_cache as _extension_manifest_keys_cache,
    _last_discovered_ext_panel_categories as _last_discovered_ext_panel_categories,
)

# -- State reassignment accessors --------------------------------------------
from ._state import (
    reset_glyph_cache,
    reset_all_tags_cache,
    set_tag_order,
    reset_category_orders_cache,
    reset_icon_detection_session_checked,
    reset_last_discovered_ext_panel_categories,
    set_last_discovered_category_sources,
    reset_last_discovered_category_sources,
    set_glyph_cache_loaded,
    set_pending_extension_context,
    set_install_from_disk_just_occurred,
    set_sync_in_progress,
    set_initial_load_complete,
    set_auto_sync_timer,
    set_background_sync_timer,
    increment_background_sync_run_count,
    reset_background_sync_run_count,
    set_auto_save_pending,
    set_auto_save_glyph_pending,
    set_auto_save_glyph_skip_wm_sync,
    set_glyph_save_lock,
    set_pending_display_mode_change,
    set_display_mode_debounce_timer_running,
    set_discovery_debounce_timer,
    set_in_ui_draw,
    set_glyph_system_registered,
    reset_merge_discovery_cache,
    reset_icon_detection_cache,
    is_glyph_system_registered,
)

# -- Extension discovery / merge ---------------------------------------------
from .discovery import (
    set_preview_mode_active,
    _scan_extension_icon_path,
    extension_post_install_handler,
    _extension_id_match_keys,
    _extension_ids_match,
    _get_discovery_source_priority,
    _pick_canonical_category_name,
    _manifest_field_match_keys,
    _extension_manifest_match_keys,
    _auto_detect_extension_icon_path,
    _discover_active_categories,
    _merge_discovered_categories,
)

# -- WM synchronization ------------------------------------------------------
from .wm_sync import (
    toggle_category_tag_no_save,
    clear_category_tags_no_save,
    restore_category_tags_from_string,
    restore_category_glyph_from_snapshot,
    update_category_tags_in_wm,
    _set_without_tag_preview_state_in_wm,
    _is_without_tag_preview_selected_in_wm,
    finalize_category_tag_changes,
    sync_glyph_mappings_to_wm,
    _background_discovery_sync,
    _start_background_sync,
    _stop_background_sync,
    _auto_sync_to_wm,
    _sync_glyph_mappings_to_wm_impl,
    register_category_glyph_mappings,
    sync_wm_to_glyph_cache,
    _sync_wm_to_glyph_cache_impl,
)

# -- App handlers / auto-save ------------------------------------------------
from .handlers import (
    is_zero_v3,
    _cancel_deferred_auto_save,
    _auto_save_tags,
    _deferred_save,
    _auto_save_glyph_mappings,
    _deferred_save_glyphs,
    _schedule_display_mode_change,
    _process_pending_display_mode_change,
    _sync_mode_flags_from_wm_to_cache,
    _save_tags_to_json,
    _save_tag_order_only,
    tag_enum_items_callback,
    _on_load_post,
    _on_save_pre,
    _on_extension_repos_update_post,
    _on_version_update,
    _register_glyph_handlers,
    _unregister_glyph_handlers,
)

# -- RNA property groups -----------------------------------------------------
from .properties import (
    CategoryTagItem,
    CategoryTagAssignment,
    TagModeItem,
    get_tag_mode_item,
    with_context_check,
)

# -- Operators ---------------------------------------------------------------
from .operators import (
    USERPREF_OT_category_tag_remove_from_category,
    USERPREF_OT_save_category_glyphs,
    USERPREF_OT_sync_category_glyphs,
    USERPREF_OT_category_tag_filter_set_mode,
    USERPREF_OT_category_tag_create,
    USERPREF_OT_category_tag_add,
    USERPREF_OT_category_tag_edit,
    WM_OT_category_tag_set_display_mode,
    WM_OT_category_tag_pick_icon,
    USERPREF_OT_category_tag_delete,
    USERPREF_OT_mark_all_unassigned_as_distributed,
    USERPREF_OT_category_tag_move,
    USERPREF_OT_category_tag_toggle,
    USERPREF_OT_category_clear_tags,
    USERPREF_OT_category_tag_filter_set,
)

# -- Tag management UI -------------------------------------------------------
from .tag_ui import (
    VIEW3D_OT_category_tabs_settings,
    USERPREF_UL_category_tags,
    TagsPanel,
    USERPREF_OT_tag_mode_toggle,
    USERPREF_OT_tag_mode_select_all,
    USERPREF_OT_tag_mode_select_none,
    USERPREF_OT_category_tag_set_display_mode,
    USERPREF_PT_tag_mode_filter_popover,
    USERPREF_PT_tag_management,
    USERPREF_PT_custom_icon_picker,
    WM_OT_debug_tag_bar_state,
)
