/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Read-only queries and scroll control for grids driven by a #GridViewSettings, exposed to Python
 * as methods on that struct (see `rna_ui.cc`).
 *
 * A drawn grid only exists for the duration of a redraw, so an add-on cannot ask the view itself
 * "what comes after this item". These functions instead rebuild the same #GridDataSource the
 * drawing path builds, from the same settings, and answer from it -- so the order an add-on steps
 * through is exactly the order the user sees. See #asset_grid_source_from_settings.
 *
 * Scroll control works on the process-lifetime session registry keyed by grid_id
 * (`views/grid_view_session.cc`), because that is where a grid's scroll position lives between
 * redraws.
 */

#include <algorithm>
#include <memory>
#include <string>

#include "BLI_math_base.h"
#include "BLI_string_ref.hh"

#include "DNA_screen_types.h"

#include "AS_asset_library.hh"
#include "AS_asset_representation.hh"

#include "ED_asset.hh"
#include "ED_asset_list.hh"
#include "ED_asset_menu_utils.hh"

#include "RNA_access.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_grid_view.hh"

#include "interface_grid_view.hh"
#include "interface_grid_view_sources.hh"

namespace blender::ui::grid_query {

/* -------------------------------------------------------------------- */
/** \name Asset list queries
 * \{ */

/**
 * The source is thrown away after each call rather than cached: it is cheap to build (it only
 * copies the resolved filters), and caching would have to be invalidated on every settings,
 * library, catalog and asset-list change -- exactly the events that already force a rebuild of
 * the drawn grid.
 */
static std::unique_ptr<AssetGridDataSource> source_for(const bContext &C,
                                                       PointerRNA &settings,
                                                       const QueryParams &params)
{
  AssetGridSourceParams source_params;
  source_params.membership_shelf_idname = params.membership_shelf_idname;
  source_params.catalog_memory_domain = params.catalog_memory_domain;
  source_params.catalog_filter_domain = params.catalog_filter_domain;

  AssetLibraryReference lib_ref;
  std::unique_ptr<AssetGridDataSource> source = asset_grid_source_from_settings(
      settings, source_params, &lib_ref);

  /* Start the library read if it has not happened yet. Every query below reports "not ready"
   * (0 / -1 / empty) until it finishes, and the #ND_ASSET_LIST notifier redraws then, so an
   * add-on polling from a draw or a timer converges without blocking here. */
  ed::asset::list::storage_fetch(&lib_ref, &C);
  return source;
}

int item_count(const bContext &C, PointerRNA &settings, const QueryParams &params)
{
  return source_for(C, settings, params)->item_count(C);
}

bool is_ready(const bContext &C, PointerRNA &settings, const QueryParams &params)
{
  return source_for(C, settings, params)->item_count_ready(C);
}

int index_of(const bContext &C,
             PointerRNA &settings,
             const StringRef identifier,
             const QueryParams &params)
{
  return source_for(C, settings, params)->filtered_index_of(C, identifier);
}

std::string identifier_at(const bContext &C,
                          PointerRNA &settings,
                          const int index,
                          const QueryParams &params)
{
  return source_for(C, settings, params)->filtered_identifier_at(C, index);
}

std::string step(const bContext &C,
                 PointerRNA &settings,
                 const StringRef identifier,
                 const int offset,
                 const bool wrap,
                 const QueryParams &params)
{
  const std::unique_ptr<AssetGridDataSource> source = source_for(C, settings, params);

  /* One walk of the filtered list yields both. An unknown or empty identifier means "no current
   * item": stepping forward from there lands on the first item and stepping back on the last,
   * which is what a hotkey pressed before anything is assigned should do. */
  int count = 0;
  const int current = source->filtered_index_of_with_count(C, identifier, &count);
  if (count <= 0) {
    return "";
  }

  int target;
  if (current < 0) {
    target = (offset >= 0) ? 0 : count - 1;
  }
  else {
    target = current + offset;
    if (wrap) {
      /* Positive modulo: `offset` may be any magnitude, in either direction. */
      target = ((target % count) + count) % count;
    }
    else {
      target = std::clamp(target, 0, count - 1);
      if (target == current) {
        /* Already at the end in that direction; report "nowhere to go" rather than handing back
         * the identifier the caller passed in, so a caller can tell the two apart. */
        return "";
      }
    }
  }

  return source->filtered_identifier_at(C, target);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Session lookup and scroll control
 * \{ */

/**
 * Session ids are region-scoped and minted by the templates (`<kind>:<region ptr>:<grid_id>`), so
 * an add-on that only knows its grid_id cannot name one. Match on the id's trailing `:<grid_id>`
 * instead, and prefer the session whose recorded region is \a region: two areas showing the same
 * grid_id keep separate scroll positions, and a request made from one region should move that
 * region's grid rather than whichever happened to be registered first.
 */
static GridSessionState *session_for_grid_id(const StringRef grid_id, const ARegion *region)
{
  if (grid_id.is_empty()) {
    return nullptr;
  }
  const std::string suffix = ":" + std::string(grid_id);

  GridSessionState *fallback = nullptr;
  GridSessionState *in_region = nullptr;
  grid_session_state_foreach([&](const StringRef id, GridSessionState &session) {
    if (!id.endswith(suffix)) {
      return true;
    }
    if (region != nullptr && session.region == region) {
      in_region = &session;
      return false;
    }
    if (fallback == nullptr) {
      fallback = &session;
    }
    return true;
  });
  return in_region ? in_region : fallback;
}

std::string active_identifier(const StringRef grid_id, const ARegion *region)
{
  const GridSessionState *session = session_for_grid_id(grid_id, region);
  return session ? session->active_identifier : "";
}

bool activate_identifier(bContext &C,
                         PointerRNA &settings,
                         const StringRef identifier,
                         const StringRef activate_operator,
                         const StringRef activate_context_id,
                         const QueryParams &params)
{
  if (identifier.is_empty() || activate_operator.is_empty()) {
    return false;
  }
  wmOperatorType *ot = WM_operatortype_find(std::string(activate_operator).c_str(), true);
  if (ot == nullptr) {
    return false;
  }

  AssetGridSourceParams source_params;
  source_params.membership_shelf_idname = params.membership_shelf_idname;
  source_params.catalog_memory_domain = params.catalog_memory_domain;
  source_params.catalog_filter_domain = params.catalog_filter_domain;
  const AssetLibraryReference lib_ref = asset_grid_library_from_settings(settings, source_params);
  if (!ed::asset::list::is_loaded(&lib_ref)) {
    return false;
  }

  asset_system::AssetRepresentation *found = nullptr;
  ed::asset::list::iterate(lib_ref, [&](asset_system::AssetRepresentation &asset) {
    if (asset.library_relative_identifier() == identifier) {
      found = &asset;
      return false;
    }
    return true;
  });
  if (found == nullptr) {
    return false;
  }

  /* Deliberately the same call #AssetGridItem::on_activate makes for a click, so an add-on's
   * activate operator cannot tell a stepped selection from a clicked one. */
  PointerRNA op_props = WM_operator_properties_create_ptr(ot);
  ed::asset::operator_asset_reference_props_set(*found, op_props);
  if (!activate_context_id.is_empty() && RNA_struct_find_property(&op_props, "context_id")) {
    RNA_string_set(&op_props, "context_id", std::string(activate_context_id).c_str());
  }
  WM_operator_name_call_ptr(&C, ot, wm::OpCallContext::InvokeRegionWin, &op_props, nullptr);
  WM_operator_properties_free(&op_props);
  return true;
}

int layout_columns(const StringRef grid_id, const ARegion *region)
{
  const GridSessionState *session = session_for_grid_id(grid_id, region);
  return session ? session->cols : 0;
}

bool reset_scroll(const StringRef grid_id, const ARegion *region)
{
  GridSessionState *session = session_for_grid_id(grid_id, region);
  if (session == nullptr) {
    return false;
  }
  session->scroll_px = 0;
  session->scroll_px_by_cols.clear();
  /* Clearing the focus key lets a host's active item be revealed again on the next build; without
   * it the grid would consider that reveal already spent and stay at the top. */
  session->active_focus_key.clear();
  return true;
}

bool scroll_to_index(const bContext & /*C*/,
                     PointerRNA & /*settings*/,
                     const StringRef grid_id,
                     const int index,
                     const bool center,
                     const ARegion *region,
                     const QueryParams & /*params*/)
{
  GridSessionState *session = session_for_grid_id(grid_id, region);
  if (session == nullptr || index < 0) {
    /* Never drawn, so there is no layout to scroll: nothing to do that would survive. */
    return false;
  }
  const int cols = max_ii(1, session->cols);
  const int tile_h = max_ii(1, session->tile_h);
  const int viewport = max_ii(session->viewport_px, tile_h);

  const int row_top = (index / cols) * tile_h;
  int target;
  if (center) {
    target = row_top - (viewport - tile_h) / 2;
  }
  else if (row_top < session->scroll_px) {
    target = row_top;
  }
  else if (row_top + tile_h > session->scroll_px + viewport) {
    target = row_top + tile_h - viewport;
  }
  else {
    /* Already fully visible; leave the user's position alone. */
    return true;
  }

  session->scroll_px = max_ii(0, target);
  /* The clamp against the real content height needs the item count, which the next build knows;
   * #grid_clamp_scroll_px runs there. Setting a too-large value here is corrected then. */
  session->scroll_px_by_cols.add_overwrite(cols, session->scroll_px);
  return true;
}

bool scroll_to_item(const bContext &C,
                    PointerRNA &settings,
                    const StringRef grid_id,
                    const StringRef identifier,
                    const bool center,
                    const ARegion *region,
                    const QueryParams &params)
{
  const int index = index_of(C, settings, identifier, params);
  if (index < 0) {
    return false;
  }
  return scroll_to_index(C, settings, grid_id, index, center, region, params);
}

/** \} */

}  // namespace blender::ui::grid_query
