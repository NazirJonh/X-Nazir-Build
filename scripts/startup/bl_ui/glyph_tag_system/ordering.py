# SPDX-FileCopyrightText: 2026 Nazir Galimov
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Category order management for the Category Tabs / Glyph / Tag system.

Extracted verbatim from ``space_userpref.py`` (no behavioural change).
All state lives in glyph_tag_system._state; this module imports
``_category_orders_cache`` by reference (in-place mutations) and uses
``reset_category_orders_cache`` for full reassignment.

Cross-module calls to ``_load_glyph_mappings_from_file`` and
``_save_glyph_mappings_to_file`` are lazy-imported from ``glyph_cache``
inside the function bodies that need them.

The public names are re-imported in ``space_userpref`` to preserve its
attribute contract.
"""

from bl_ui.glyph_tag_system._state import (
    _category_orders_cache,
    is_glyph_cache_loaded,
)
from bl_ui.glyph_tag_system.log import (
    _log_once,
    category_debug_print,
)


# -----------------------------------------------------------------------------
# Category Order Management Functions
# -----------------------------------------------------------------------------


def get_category_order(tag_combination):
    """
    Get category order for a specific tag combination.
    Args:
        tag_combination: String like "" (no filter), "Modeling", or "Animation;Modeling"
    Returns:
        List of category IDs in order, or empty list if not found
    """
    if not is_glyph_cache_loaded():
        from bl_ui.glyph_tag_system.glyph_cache import _load_glyph_mappings_from_file
        _load_glyph_mappings_from_file()
    result = _category_orders_cache.get(tag_combination, []).copy()
    # Debug logging for NODE: specifically (use _log_once to avoid flooding)
    if tag_combination == "NODE:":
        _log_once(f"[CAT ORDER][PY] get_category_order('NODE:') = {result[:5]}... (total {len(result)} entries)")
        _log_once(f"[CAT ORDER][PY] _category_orders_cache keys: {list(_category_orders_cache.keys())[:10]}")
    return result


def set_category_order(tag_combination, category_list):
    """
    Set category order for a specific tag combination.
    Args:
        tag_combination: String like "" (no filter), "Modeling", or "Animation;Modeling"
        category_list: List of category IDs in order
    """
    try:
        category_debug_print(f"[CAT ORDER][PY] set_category_order: tag='{tag_combination}' count={len(category_list)}")
        # Print a compact preview to avoid huge logs
        preview = ", ".join([repr(x) for x in category_list[:12]])
        if len(category_list) > 12:
            preview += f", ... (+{len(category_list)-12})"
        category_debug_print(f"[CAT ORDER][PY] order: [{preview}]")
    except Exception as e:
        category_debug_print(f"[CAT ORDER][PY] set_category_order log failed: {e}")
    _category_orders_cache[tag_combination] = category_list.copy()
    # Trigger save
    from bl_ui.glyph_tag_system.glyph_cache import _save_glyph_mappings_to_file
    _save_glyph_mappings_to_file()


def clear_category_order(tag_combination):
    """Clear category order for a specific tag combination."""
    if tag_combination in _category_orders_cache:
        del _category_orders_cache[tag_combination]
        from bl_ui.glyph_tag_system.glyph_cache import _save_glyph_mappings_to_file
        _save_glyph_mappings_to_file()


def get_all_category_orders():
    """Get all category orders (for debugging/export)."""
    if not is_glyph_cache_loaded():
        from bl_ui.glyph_tag_system.glyph_cache import _load_glyph_mappings_from_file
        _load_glyph_mappings_from_file()
    return _category_orders_cache.copy()
