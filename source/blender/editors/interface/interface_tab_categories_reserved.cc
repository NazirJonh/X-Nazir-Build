/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Category Tabs - Reserved-category predicates.
 *
 * Split out of interface_tab_categories.cc: the logic that decides whether a
 * category is "reserved" (a built-in Blender category such as "Item"/"Tool"
 * that add-ons may reuse but do not own) and whether it is protected from
 * reordering. Reserved status is authored on the Python side and mirrored into
 * `wm.category_glyph_mappings`; these predicates read that state.
 *
 * The shared lookup helpers `category_glyph_mapping_find()` and
 * `category_name_is_glyph()` remain in interface_tab_categories.cc and are
 * declared in interface_tab_categories_intern.hh.
 */

#include "BLI_string.h"

#include "DNA_windowmanager_types.h"

#include "interface_intern.hh"
#include "interface_tab_categories_intern.hh"

namespace blender::ui {

/* -------------------------------------------------------------------- */
/** \name Reserved Categories
 * \{ */

bool category_is_reserved(const wmWindowManager *wm, const char *category_id)
{
  /* Categories with glyph names (high Unicode) are from addons and NOT reserved */
  if (category_name_is_glyph(category_id)) {
    return false;
  }

  /* Single source of truth: Python marks reserved categories in wm.category_glyph_mappings. */
  if (const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category_id)) {
    return (item->is_reserved != 0);
  }

  return false;
}

bool category_has_pending_tag_assignment(const wmWindowManager *wm,
                                          const char *category_id,
                                          int space_type)
{
  if (const CategoryGlyphItem *item = category_glyph_mapping_find(wm, category_id, space_type)) {
    return (item->pending_tag_assignment != 0);
  }
  return false;
}

bool category_is_reserved_for_reorder(const wmWindowManager *wm, const char *category_id)
{
  if (category_name_is_glyph(category_id)) {
    return false;
  }

  if (category_is_reserved(wm, category_id)) {
    return true;
  }

  auto reserved_name_fallback = [](const char *idname) {
    static const char *k_reserved_fallback[] = {
        "Item",        "View",      "Edit",        "Tool",     "Asset",   "Options",
        "Animation",   "Texture",   "Mesh",        "Object",   "Scene",   "Render",
        "Node",        "Cache",     "Proxy",       "Metadata",
        // Reserved categories (disabled, kept for future use):
        // "Physics", "World", "Material", "Modifiers", "Particles", "Curve",
        // "Script", "Sound", "Surface", "Volume", "Constraints", "Data",
    };
    for (const char *reserved_id : k_reserved_fallback) {
      if (STREQ(idname, reserved_id)) {
        return true;
      }
    }
    return false;
  };

  /* If reserved markers were not populated yet (or failed to sync), keep reorder protection
   * for known built-in categories. */
  bool has_any_reserved_marker = false;
  if (wm && category_glyph_list_is_valid(&wm->category_glyph_mappings)) {
    for (const CategoryGlyphItem *item =
             static_cast<const CategoryGlyphItem *>(wm->category_glyph_mappings.first);
         item;
         item = static_cast<const CategoryGlyphItem *>(item->next))
    {
      if (item->is_reserved != 0) {
        has_any_reserved_marker = true;
        break;
      }
    }
  }

  if (!has_any_reserved_marker) {
    return reserved_name_fallback(category_id);
  }

  return false;
}

/** \} */

}  // namespace blender::ui
