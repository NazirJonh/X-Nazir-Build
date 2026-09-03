/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Share between `interface/templates/` files.
 */

#pragma once

#include <functional>
#include <optional>
#include <string>

#include "MEM_guardedalloc.h"

#include "BLI_function_ref.hh"
#include "BLI_span.hh"

#include "RNA_access.hh"
#include "RNA_types.hh"

#include "UI_interface_c.hh" /* For #TEMPLATE_ID_FILTER_ALL. */
#include "UI_interface_layout.hh"

#include "interface_grid_view_settings_utils.hh"

#include "DNA_uuid_types.h"

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
 * Make hovering \a but show \a id's full preview and info, the same tooltip the ID browser's own
 * items use. No-op for a null button or ID.
 */
void id_preview_tooltip_set(Button *but, ID *id);
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
 * Layout variant built by #blender::ui::template_id_browser. Derived once by
 * #image_browser_mode_get and passed down, so the browser row and the appended #template_ID
 * controls cannot disagree about which variant they are building.
 */
enum class ImageBrowserMode {
  /** Ordinary browse row: browser button, editable name, New/Open/Unlink. */
  Standard,
  /** Empty paint slot: labelled drop button beside an icon-only Open, no New, no rename. */
  PaintSlotEmpty,
  /** Assigned paint slot: preview thumbnail before a read-only name, no New, no Fake User. */
  PaintSlotAssigned,
};

/**
 * The paint-slot variants are opt-in: they need an Image property and a non-empty \a drop_text
 * (the label of the drop button shown while the slot is empty).
 */
ImageBrowserMode image_browser_mode_get(PointerRNA *ptr, PropertyRNA *prop, const char *drop_text);

/**
 * Which of the optional #template_ID controls an image browse row keeps.
 *
 * Grouped into a struct rather than trailing boolean parameters so a host that only cares about
 * one of them does not have to spell out the ones before it, and so a new control can be added
 * without shifting an argument list its callers pass positionally.
 *
 * The defaults describe an ordinary data-block row; every field is an opt-out. New and Open are
 * not represented here -- they are already opt-out by passing a null operator name. Unlink is,
 * because its button appears whether or not \a unlinkop names an operator.
 */
struct ImageIDRowParams {
  /** Which layout variant the row is part of, see #ImageBrowserMode. */
  ImageBrowserMode mode = ImageBrowserMode::Standard;
  /** Editable name field. Off leaves the name visible but disabled. */
  bool use_rename = true;
  /**
   * The built-in name field. Off when the host draws its own name button in that spot, so the row
   * does not show the name twice.
   */
  bool use_name = true;
  /**
   * The built-in unlink (X) button. Hosts that offer their own way back to "no source" (e.g. a
   * Clear Source operator beside the row) turn it off, so the row does not present two different
   * ways to drop the assignment.
   */
  bool use_unlink = true;
  /** Users-count button (#ID_REAL_USERS > 1). */
  bool use_users = true;
  /** Fake-user toggle. */
  bool use_fake_user = true;
};

/**
 * Append the standard #template_ID controls (rename, new, open, users, etc.) without the browse
 * search-menu button, in the variant \a params asks for. Used with #id_browser_add_popover_button
 * for image paint browsing.
 */
void template_id_image_row_append_standard(const bContext *C,
                                           Layout &layout,
                                           PointerRNA *ptr,
                                           PropertyRNA *prop,
                                           const char *newop,
                                           const char *openop,
                                           const char *unlinkop,
                                           const ImageIDRowParams &params = {});

enum class IDBrowserImageFilter {
  Default = 0,
  PaintSource,
};

IDBrowserImageFilter id_browser_image_filter_from_context(const bContext &C);

/**
 * Everything #image_id_passes_paint_filter needs to judge an image besides the image itself.
 *
 * \note #filter_mode's two bits are not independent: set together they do not mean "both filters",
 * they select the PBR paint-layer view. The predicate resolves them into a single mode.
 */
struct ImagePaintFilterParams {
  /** Mask of #TEMPLATE_ID_FILTER_CURRENT_MATERIAL and/or #TEMPLATE_ID_FILTER_SLOT_TYPE. */
  int filter_mode = TEMPLATE_ID_FILTER_ALL;
  /** Material the material-aware modes compare against; nothing passes them without one. */
  const Material *material = nullptr;
  /** #NODE_TEX_IMAGE_SLOT_NONE means "any slot type". */
  char slot_type = 0;
  /**
   * Layer the PBR paint-layer view is centered on -- the assigned image's layer, or a UUID
   * pushed from Python. A nil value widens that view to "any managed paint canvas
   * (#IMA_PAINT_CANVAS) carrying a layer UUID". Only read for that view.
   */
  bUUID reference_layer_id = {};
};

/**
 * Shared predicate for the image paint-slot filters, used by both the ID search menu
 * (#template_id_browse_with_context) and the image-browser popover. Returns true when \a image
 * passes \a params. Render-result and compositor images never pass. The cached usage index is
 * rebuilt from \a bmain on demand.
 */
bool image_id_passes_paint_filter(Main &bmain,
                                  const Image &image,
                                  const ImagePaintFilterParams &params);

/**
 * What an ID Browser popover browses for. Every helper that adds a browser button carries this
 * along unchanged and publishes it into the layout context (#id_browser_popover_context_set); the
 * popover's own draw callback reads it back from there, since a popover panel gets no arguments.
 *
 * Grouped rather than passed as five parallel arguments through the whole helper family: a new
 * filter would otherwise have to be threaded through each of their signatures.
 */
struct IDBrowserTarget {
  /** The pointer property to browse for, and its owner. */
  PointerRNA *ptr = nullptr;
  const char *propname = nullptr;
  /** Seeds the "Current Material" filter context (may be null). */
  Material *material = nullptr;
  /** Registered #IDFilterType narrowing the popover's contents (may be null/empty). */
  const char *filter_type = nullptr;
  /** Built-in image filtering preset, see #IDBrowserImageFilter (may be null/empty). */
  const char *image_filter = nullptr;
};

/**
 * Add the ID-browser popover button to \a row (replaces the browse search-menu button). Named for
 * its primary use (image paint slots) but generic over the target property's ID type. The browser
 * button always has its normal icon-only label, unless \a use_preview_icon is set: it then shows
 * the assigned data-block's preview as a thumbnail, which only fits a row that is scaled taller
 * than one unit.
 */
void id_browser_add_popover_button(Layout &row,
                                   const bContext *C,
                                   const IDBrowserTarget &target,
                                   bool use_preview_icon = false);
/** Add the context needed by the ID Browser popover to \a layout without creating a button. */
void id_browser_popover_context_set(Layout &layout, const IDBrowserTarget &target);

/**
 * `interface_template_grid_selectors.cc`
 * Ctrl-Wheel cycling shared by the library selectors.
 */

/**
 * How one library selector menu differs from the other: where its entries come from and what
 * picking one does. Everything else about the menu (heading, Recent/Favorites rows, folder
 * grouping, active highlight) is shared, see #library_selector_menu_draw_items.
 */
struct LibrarySelectorMenuParams {
  /** Enum entries to lay out, in Preferences order. Not owned; must outlive the call. */
  const EnumPropertyItem *items;
  /** Heading above the choices (e.g. "Asset Library"). Empty draws no heading. */
  StringRef title;
  /** Enum value of the browsed library, drawn as the active entry. */
  int current_library_value;
  /** Places the active mark on Recent/Favorites instead of a library when in membership. */
  grid_settings::CatalogMode current_mode = grid_settings::CatalogMode::All;
  /** Whether to offer the Recent/Favorites entries above the libraries. */
  bool show_membership = false;
  /**
   * Applies a picked library. Left unset by a host whose button is a real RNA enum property: there
   * the value returned through the popup handle is applied to the property on close, and setting it
   * twice would be wrong.
   */
  std::function<void(bContext &C, int library_enum_value)> apply_library;
  /** Applies a picked Recent/Favorites entry. Required when #show_membership is set. */
  std::function<void(bContext &C, grid_settings::CatalogMode mode)> apply_membership;
  /**
   * Leaves Recent/Favorites when #ASSET_LIBRARY_ALL is picked while in membership. Needed only by
   * hosts that store membership under that same library reference (the catalog-memory ones): for
   * them the pick changes no enum value, so nothing else would end membership and the entry would
   * do nothing. A host whose #apply_library already handles that case (the image grid's
   * #image_grid_set_library) leaves this unset.
   */
  std::function<void(bContext &C)> exit_membership;
};

/**
 * Draw the shared library selector menu into \a layout: heading, optional Recent/Favorites, then
 * the libraries with Preferences folders as headings (root libraries first, so a root library
 * listed after a folder is not drawn under it).
 *
 * Entries are #ButtonType::ButMenu rows bound to the popup handle's return value, like the built-in
 * enum dropdown (#def_but_rna__menu): an RNA-backed host gets its property applied on close for
 * free, while a host with no RNA property passes callbacks in \a params and the returned value is
 * simply ignored (such a button has no pointer type, see #button_value_set).
 */
void library_selector_menu_draw_items(bContext &C,
                                      Layout &layout,
                                      const LibrarySelectorMenuParams &params);

/** What #library_selector_step landed on: a membership pseudo-entry or a real library. */
struct LibraryStepResult {
  /** True when #mode holds the entry (Recent/Favorites); otherwise #library_enum_value does. */
  bool is_membership;
  grid_settings::CatalogMode mode;
  int library_enum_value;
};

/**
 * Step one entry through the library selector's virtual list, which is ordered exactly like the
 * selector menus draw it: `[Recent, Favorites,] library0 .. libraryN` (the membership entries only
 * when \a include_membership). Cycles at both ends.
 *
 * Pure: the caller builds \a library_values from whichever item source its own menu uses (so the
 * cycled set can never diverge from the drawn one) and applies the result its own way. Returns
 * nothing when there is nothing to step through.
 *
 * \param current_mode: #CatalogMode::Recent or #CatalogMode::Favorites places the current position
 * on that entry; any other mode places it on \a current_library_value.
 */
std::optional<LibraryStepResult> library_selector_step(Span<int> library_values,
                                                       bool include_membership,
                                                       grid_settings::CatalogMode current_mode,
                                                       int current_library_value,
                                                       int direction);

/**
 * `interface_template_id_browser_asset.cc`
 * Asset-library item source for the ID browser popover.
 */

/** Grid session key shared by the ID browser's grid build (#use_session_scroll) and its scroll
 * reset (#blender::ui::grid_view_session_reset_scroll), so the two cannot drift apart. */
constexpr StringRef id_browser_grid_session_key = "id_browser_grid";

/** Pointer to #wmWindowManager::id_browser_grid_view_settings (the ID browser's grid settings). */
PointerRNA id_browser_grid_settings_ptr(wmWindowManager &wm);
/**
 * Return the browsed asset library by value from #id_browser_grid_view_settings, first resolving
 * it against the current Preferences (see #blender::ed::asset::library_reference_ensure_resolved).
 */
AssetLibraryReference id_browser_library_ref_get(wmWindowManager &wm);
/** Store an ID Browser asset library reference in its grid settings. */
void id_browser_library_ref_set(wmWindowManager &wm, const AssetLibraryReference &library_ref);
/** True when the browsed library no longer exists in the Preferences (§5). */
bool id_browser_library_is_missing(wmWindowManager &wm);
/** UI name of the browsed asset library ("Current File", "All Libraries", custom name, ...). */
const char *id_browser_library_ui_name(const AssetLibraryReference &lib_ref);
/**
 * Iterate assets of \a lib_ref whose ID type is \a idcode and which pass the catalog filter stored
 * in #UserDef.catalog_memory for domain #"id_browser". Iteration stops early when \a fn returns
 * false.
 */
void id_browser_foreach_asset(const bContext &C,
                              const AssetLibraryReference &lib_ref,
                              short idcode,
                              FunctionRef<bool(asset_system::AssetRepresentation &)> fn);

/**
 * Synthetic per-idcode shelf idname the ID browser uses to key into the Recent/Favorites asset
 * list registry (`ED_asset_shelf.hh`'s `shelf_asset_lists_*` functions) -- the ID browser has no
 * real #AssetShelf of its own. One list per browsed ID type (e.g. Recent Materials is independent
 * of Recent Images).
 */
std::string id_browser_shelf_idname(short idcode);
/**
 * Enter Recent or Favorites membership mode: points the library at #ASSET_LIBRARY_ALL, writes the
 * mode via #BKE_asset_catalog_memory_set_mode (domain #"id_browser", never clears catalog_id_set),
 * and resets scroll. \a mode must be #CatalogMode::Recent or #CatalogMode::Favorites.
 */
void id_browser_set_membership(wmWindowManager &wm, grid_settings::CatalogMode mode);
/**
 * Iterate the ID browser's Recent or Favorites list (idcode-filtered), in list order (most
 * recent/favorited first). \a mode must be #CatalogMode::Recent or #CatalogMode::Favorites.
 * Iteration stops early when \a fn returns false.
 */
void id_browser_foreach_membership_asset(const bContext &C,
                                         grid_settings::CatalogMode mode,
                                         short idcode,
                                         FunctionRef<bool(asset_system::AssetRepresentation &)> fn);

}  // namespace ui
}  // namespace blender
