/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Share between `interface/templates/` files.
 */

#pragma once

#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_function_ref.hh"
#include "BLI_set.hh"

#include "RNA_access.hh"
#include "RNA_types.hh"

#include "UI_interface_layout.hh"

struct Image;
struct Main;

namespace blender {

struct AssetLibraryReference;
struct bContext;
struct Material;
struct wmWindowManager;

namespace asset_system {
class AssetRepresentation;
}  // namespace asset_system

namespace ui {

#define CURVE_ZOOM_MAX (1.0f / 25.0f)
#define ERROR_LIBDATA_MESSAGE N_("Cannot edit external library data")

/* Defines for templateID/TemplateSearch. */
#define TEMPLATE_SEARCH_TEXTBUT_MIN_WIDTH (UI_UNIT_X * 4)
#define TEMPLATE_SEARCH_TEXTBUT_HEIGHT UI_UNIT_Y

struct RNAUpdateCb {
  PointerRNA ptr = {};
  PropertyRNA *prop;
};

static inline void rna_update_cb(bContext &C, const RNAUpdateCb &cb)
{
  /* we call update here on the pointer property, this way the
   * owner of the curve mapping can still define its own update
   * and notifier, even if the CurveMapping struct is shared. */
  RNA_property_update(&C, &const_cast<PointerRNA &>(cb.ptr), cb.prop);
}

static inline void rna_update_cb(bContext *C, void *arg_cb, void * /*arg*/)
{
  RNAUpdateCb *cb = static_cast<RNAUpdateCb *>(arg_cb);
  rna_update_cb(*C, *cb);
}

/* `interface_template.cc` */
int template_search_textbut_width(PointerRNA *ptr, PropertyRNA *name_prop);
int template_search_textbut_height();
/**
 * Add a block button for the search menu for templateID and templateSearch.
 */
void template_add_button_search_menu(const bContext *C,
                                     Layout &layout,
                                     Block *block,
                                     PointerRNA *ptr,
                                     PropertyRNA *prop,
                                     BlockCreateFunc block_func,
                                     void *block_argN,
                                     std::optional<StringRef> tip,
                                     const bool use_previews,
                                     const bool editable,
                                     const bool live_icon,
                                     ButtonArgNFree func_argN_free_fn = MEM_delete_void,
                                     ButtonArgNCopy func_argN_copy_fn = MEM_dupalloc);

Block *template_common_search_menu(const bContext *C,
                                   ARegion *region,
                                   ButtonSearchUpdateFn search_update_fn,
                                   void *search_arg,
                                   ButtonHandleFunc search_exec_fn,
                                   void *active_item,
                                   ButtonSearchTooltipFn item_tooltip_fn,
                                   const int preview_rows,
                                   const int preview_cols,
                                   float scale);

/**
 * Append the standard #template_ID controls (rename, new, open, users, etc.) without the browse
 * search-menu button. Used with #id_browser_add_popover_button for image paint browsing.
 */
void template_id_image_row_append_standard(const bContext *C,
                                           Layout &layout,
                                           PointerRNA *ptr,
                                           PropertyRNA *prop,
                                           const char *newop,
                                           const char *openop,
                                           const char *unlinkop);

/**
 * Shared predicate for the image paint-slot filters, used by both the ID search menu
 * (#template_id_browse_with_context) and the image-browser popover. Returns true when \a image
 * passes \a filter_mode (a mask of #TEMPLATE_ID_FILTER_CURRENT_MATERIAL and/or
 * #TEMPLATE_ID_FILTER_SLOT_TYPE). Render-result and compositor images never pass. The cached
 * usage index is rebuilt from \a bmain on demand.
 */
bool image_id_passes_paint_filter(
    Main &bmain, const Image &image, int filter_mode, const Material *material, char slot_type);

/**
 * Add the ID-browser popover button to \a row (replaces the browse search-menu button). Named for
 * its primary use (image paint slots) but generic over the target property's ID type.
 * \a filter_type optionally names a registered #IDFilterType to narrow the popover's contents
 * (may be null/empty).
 */
void id_browser_add_popover_button(Layout &row,
                                      const bContext *C,
                                      PointerRNA *ptr,
                                      const char *propname,
                                      Material *material,
                                      const char *filter_type);

/**
 * `interface_template_id_browser_asset.cc`
 * Asset-library item source for the ID browser popover.
 */

/** Grid session key shared by the ID browser's grid build (#use_session_scroll) and its scroll
 * reset (#blender::ui::grid_view_session_reset_scroll), so the two cannot drift apart. */
constexpr StringRef id_browser_grid_session_key = "id_browser_grid";

/**
 * Return the browsed asset library, first resolving it against the current Preferences (see
 * #blender::ed::asset::library_reference_ensure_resolved). Use instead of reading
 * #wmWindowManager::id_browser_asset_library_ref directly: it is stored in DNA, so its cached
 * members can go stale (renamed/reordered library) or the library can be gone entirely.
 */
const AssetLibraryReference &id_browser_library_ref_ensure_valid(wmWindowManager &wm);
/** True when the browsed library no longer exists in the Preferences (§5). */
bool id_browser_library_is_missing(wmWindowManager &wm);
/** Enabled catalog paths of the ID browser, as a set (the DNA list is the source of truth). */
Set<std::string> id_browser_catalog_paths_get(const wmWindowManager &wm);
/** Replace the ID browser's enabled catalog list with \a paths. */
void id_browser_catalog_paths_set(wmWindowManager &wm, const Set<std::string> &paths);
/** UI name of the browsed asset library ("Current File", "All Libraries", custom name, ...). */
const char *id_browser_library_ui_name(const AssetLibraryReference &lib_ref);
/**
 * Iterate assets of \a lib_ref whose ID type is \a idcode and which pass the catalog filter.
 * An empty \a enabled_catalog_paths means "all catalogs". Iteration stops early when \a fn
 * returns false.
 */
void id_browser_foreach_asset(const bContext &C,
                              const AssetLibraryReference &lib_ref,
                              short idcode,
                              const Set<std::string> &enabled_catalog_paths,
                              FunctionRef<bool(asset_system::AssetRepresentation &)> fn);
/**
 * Register the `UI_PT_id_browser_catalog_selector` popover panel (catalog checkbox tree).
 * Idempotent; safe to call every time #id_browser_popover_register runs.
 */
void id_browser_catalog_selector_register();

}  // namespace ui
}  // namespace blender
