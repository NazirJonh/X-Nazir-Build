# SPDX-FileCopyrightText: 2009-2023 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Unassigned-extension-category predicates for the "New Add-ons!" tag-bar filter.

Extracted verbatim from ``space_userpref.py`` (no behavioural change). These
functions mirror the C++ unassigned-category predicate so Python headers stay in
sync with the shared tag-bar behavior. They read the global ``_glyph_cache``
(rather than ``wm.category_glyph_mappings``) so that preview mode is respected:
in preview mode WM may already carry tags while ``pending_tag_assignment`` stays
True in the cache, keeping categories visible in "New Add-ons!" until Save.

The public names are re-exported through ``glyph_tag_system.api`` to preserve the
module-attribute contract used by the editor space modules.
"""

import time

from .conversions import (
    spaces_to_flags,
    modes_to_flags,
)
from .defaults import (
    DEFAULT_CATEGORY_GLYPHS,
    SPACE_TO_FLAG,
)
from .modes import get_current_tag_mode_flag
from .log import category_debug_print

# In-memory cache of glyph mappings imported by reference from the single owner
# in ``_state``. Mutations are shared; these predicates only read it.
from ._state import _glyph_cache as _glyph_cache  # noqa: F401


def _get_unassigned_categories_count_for_space(context,
                                               space_type,
                                               space_flag,
                                               _order_key=None):
    """Count unassigned extension categories for a specific editor space.

    Mirrors the C++ unassigned-category predicate so Python headers stay in sync
    with the shared tag-bar behavior.

    IMPORTANT: This function checks _glyph_cache instead of wm.category_glyph_mappings
    to respect preview mode. In preview mode, WM may have tags but pending_tag_assignment
    stays True in cache, so categories remain visible in "New Add-ons!" until Save.
    """
    global _glyph_cache

    # PERF: Start timing
    _count_start = time.perf_counter()

    wm = getattr(context, "window_manager", None)
    if wm is None:
        return 0

    # OPTIMIZATION: REMOVED calls to _merge_discovered_categories() and sync_glyph_mappings_to_wm()
    # These were being called EVERY time category count was checked (multiple times per UI draw)
    # causing severe performance issues. Discovery now only happens at initial load.
    # CRITICAL FIX: This was causing 790 panel scans on EVERY count check!

    current_mode_flag = get_current_tag_mode_flag(context)
    count = 0

    # Check cache instead of WM to respect preview mode
    _cache_iterations = 0
    for cache_key, cat_data in _glyph_cache.items():
        _cache_iterations += 1
        if not isinstance(cache_key, tuple) or len(cache_key) != 2:
            continue

        # Only check global entries (space_type=-1)
        cache_space_type, category_name = cache_key
        if cache_space_type != -1:
            continue

        if not isinstance(cat_data, dict):
            continue

        # Get values from cache
        is_reserved = category_name in DEFAULT_CATEGORY_GLYPHS
        source_extension = cat_data.get("source_extension", "")
        pending_assignment = cat_data.get("pending_tag_assignment", False)
        tags = cat_data.get("tags", [])

        # Convert discovered_spaces from list to flags
        disc_spaces_list = cat_data.get("discovered_in_spaces", [])
        if isinstance(disc_spaces_list, list):
            discovered_spaces = spaces_to_flags(disc_spaces_list)
        else:
            discovered_spaces = 0

        # Convert discovered_modes from list to flags
        disc_modes_list = cat_data.get("discovered_in_modes", [])
        if isinstance(disc_modes_list, list):
            discovered_modes = modes_to_flags(disc_modes_list)
        else:
            discovered_modes = 0

        # Get install_mode_flag for mode-aware filtering
        # This is set when extension is installed and panels don't specify bl_context
        install_mode_flag = cat_data.get("install_mode_flag", 0)

        has_no_tags = not tags or (hasattr(tags, "__len__") and len(tags) == 0)

        # Mirror C++ category_is_unassigned_for_context logic:
        # - discovered_spaces == 0 means "any space" (legacy/unknown source)
        # - discovered_spaces != 0 must match the current space_flag
        space_matches = (discovered_spaces == 0) or (discovered_spaces & space_flag)

        # Detailed logging for debugging "New Add-ons!" filter issues
        if source_extension and pending_assignment:
            category_debug_print(f"[NEW ADDONS DEBUG] Checking category={category_name!r}: "
                               f"source_ext={source_extension!r}, pending={pending_assignment}, "
                               f"tags={tags}, discovered_spaces={disc_spaces_list}, "
                               f"discovered_modes={disc_modes_list}")

        if (not is_reserved and
                source_extension and
                pending_assignment and
                has_no_tags and
                space_matches and
                not _extension_has_tagged_category(wm, source_extension) and
                not _extension_has_only_reserved_categories(wm, source_extension)):
            # Mode check: skip for SPACE_NODE (16), or if no mode filtering
            # Mirror C++: current_mode_flag == 0 or discovered_modes == 0 means "any mode"
            # BUT: for categories from extensions with discovered_modes == 0, use install_mode_flag
            # to filter by the mode where extension was installed
            effective_mode_flags = discovered_modes if discovered_modes != 0 else install_mode_flag
            if space_type == 16 or current_mode_flag == 0 or effective_mode_flags == 0 or (effective_mode_flags & current_mode_flag):
                count += 1
                category_debug_print(f"[NEW ADDONS DEBUG] ✓ COUNTED: {category_name!r}")
            else:
                category_debug_print(f"[NEW ADDONS DEBUG] ✗ FAILED mode check: {category_name!r} "
                                   f"(current_mode_flag={current_mode_flag:#x}, discovered_modes={discovered_modes:#x}, "
                                   f"install_mode_flag={install_mode_flag:#x})")
        elif source_extension and pending_assignment:
            # Log why category was NOT counted
            if is_reserved:
                category_debug_print(f"[NEW ADDONS DEBUG] ✗ FAILED: is_reserved={is_reserved}")
            elif not has_no_tags:
                category_debug_print(f"[NEW ADDONS DEBUG] ✗ FAILED: has_tags={not has_no_tags}")
            elif not space_matches:
                category_debug_print(f"[NEW ADDONS DEBUG] ✗ FAILED: space_matches={space_matches} "
                                   f"(discovered_spaces={discovered_spaces:#x}, space_flag={space_flag:#x})")
            elif _extension_has_tagged_category(wm, source_extension):
                category_debug_print(f"[NEW ADDONS DEBUG] ✗ FAILED: extension has tagged category")
            elif _extension_has_only_reserved_categories(wm, source_extension):
                category_debug_print(f"[NEW ADDONS DEBUG] ✗ FAILED: extension has only reserved categories")

    # PERF: Log timing for EVERY call to capture Get Extension panel performance
    _count_elapsed = (time.perf_counter() - _count_start) * 1000
    if not hasattr(_get_unassigned_categories_count_for_space, '_call_count'):
        _get_unassigned_categories_count_for_space._call_count = 0
    _get_unassigned_categories_count_for_space._call_count += 1

    # Log every call when in Preferences context (space_type 19 = USERPREF)
    # This captures performance when Get Extension panel is opened
    if space_type == 19 or _get_unassigned_categories_count_for_space._call_count <= 10:
        category_debug_print(f"[PERF] _get_unassigned_categories_count_for_space: {_count_elapsed:.3f}ms (cache iterations={_cache_iterations}, call #{_get_unassigned_categories_count_for_space._call_count}, space_type={space_type})")

    return count


def _extension_has_tagged_category(wm, source_extension: str) -> bool:
    """Check if an extension has at least one NON-RESERVED category that was processed by user.

    A category is considered "processed" if:
    - It has tags assigned (tags list is not empty)
    - OR pending_tag_assignment is False (user selected "Without Tag" or assigned a tag AND saved)

    RESERVED categories (like "Item", "Tool", "View") are IGNORED because they are
    standard Blender categories that extensions may use but don't "own". An extension
    shouldn't be considered "processed" just because it uses a reserved category.

    This handles the case where an extension creates multiple category aliases
    (e.g., "Texel Density" and "Texel Density Checker") and user processes only one.

    IMPORTANT: This function checks _glyph_cache instead of wm.category_glyph_mappings
    because in preview mode, WM is updated for C++ UI visibility but pending_tag_assignment
    remains True in cache. This prevents categories from disappearing from "New Add-ons!"
    before user clicks Save.
    """
    global _glyph_cache

    if not source_extension:
        return False

    # Check cache instead of WM to respect preview mode
    for cache_key, cat_data in _glyph_cache.items():
        if not isinstance(cache_key, tuple) or len(cache_key) != 2:
            continue

        # Only check global entries (space_type=-1)
        space_type_val, category_name = cache_key
        if space_type_val != -1:
            continue

        if not isinstance(cat_data, dict):
            continue

        if cat_data.get("source_extension", "") != source_extension:
            continue

        # Skip reserved categories - extensions don't "own" them
        if category_name in DEFAULT_CATEGORY_GLYPHS:
            continue

        # Has explicit tags assigned AND saved (pending_tag_assignment is False)
        # In preview mode, tags may be added but pending_tag_assignment stays True
        if cat_data.get("tags", []) and not cat_data.get("pending_tag_assignment", True):
            return True

        # Was processed by user AND saved (pending_tag_assignment=False)
        # This means user clicked Save after assigning tag or selecting "Without Tag"
        if not cat_data.get("pending_tag_assignment", True):
            return True

    return False


def _extension_has_only_reserved_categories(wm, source_extension: str) -> bool:
    """Check if extension only has reserved categories (with panels) or non-reserved without panels.

    IMPORTANT: This function checks _glyph_cache instead of wm.category_glyph_mappings
    to be consistent with _extension_has_tagged_category and respect preview mode.
    """
    global _glyph_cache

    if not source_extension:
        return False

    has_any_category = False
    has_reserved_with_panels = False
    has_non_reserved_without_panels = False
    has_non_reserved_with_panels = False

    for cache_key, cat_data in _glyph_cache.items():
        if not isinstance(cache_key, tuple) or len(cache_key) != 2:
            continue

        # Only check global entries (space_type=-1)
        space_type_val, category_name = cache_key
        if space_type_val != -1:
            continue

        if not isinstance(cat_data, dict):
            continue

        if cat_data.get("source_extension", "") != source_extension:
            continue

        has_any_category = True
        is_reserved = category_name in DEFAULT_CATEGORY_GLYPHS

        # Get discovered_spaces as flags
        disc_spaces = cat_data.get("discovered_in_spaces", [])
        if isinstance(disc_spaces, list):
            discovered_spaces = 0
            for space_str in disc_spaces:
                discovered_spaces |= SPACE_TO_FLAG.get(space_str, 0)
        else:
            discovered_spaces = 0

        if is_reserved:
            if discovered_spaces != 0:
                has_reserved_with_panels = True
        else:
            if discovered_spaces == 0:
                has_non_reserved_without_panels = True
            else:
                has_non_reserved_with_panels = True

    if has_reserved_with_panels and has_non_reserved_without_panels and not has_non_reserved_with_panels:
        return True

    return has_any_category and not has_non_reserved_without_panels and not has_non_reserved_with_panels
