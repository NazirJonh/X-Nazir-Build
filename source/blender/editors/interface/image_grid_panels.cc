/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 *
 * Image-grid header popover panels (catalog selector, display, name-match filter).
 * The catalog tree is the shared checkbox-SET host (#catalog_checkbox_set_tree_create);
 * this file only supplies image-grid callbacks and panel registration.
 */

#include "BLI_listbase.h"
#include "BLI_string_utf8.h"

#include "AS_asset_library.hh"

#include "BKE_context.hh"
#include "BKE_name_matching.hh"
#include "BKE_screen.hh"

#include "BLT_translation.hh"

#include "ED_asset_list.hh"
#include "ED_asset_name_matching.hh"
#include "ED_image_grid.hh"
#include "ED_screen.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "UI_interface.hh"
#include "UI_interface_c.hh"
#include "UI_interface_layout.hh"
#include "UI_tree_view.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "interface_grid_view_settings_utils.hh"

#include <memory>
#include <string>
#include <utility>

namespace blender {

using namespace ed::image_grid;

/* -------------------------------------------------------------------- */
/** \name Display Settings Popover
 * \{ */

static void image_grid_display_panel_draw(const bContext *C, Panel *panel)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return;
  }

  PointerRNA owner_ptr = owner->owner_rna();
  if (owner_ptr.data == nullptr || owner_ptr.type == nullptr) {
    return;
  }

  ui::Layout &layout = *panel->layout;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  layout.prop(&owner_ptr, "image_grid_preview_size", UI_ITEM_NONE, IFACE_("Size"), ICON_NONE);
}

void image_grid_display_panel_register(ARegionType *region_type)
{
  if (WM_paneltype_find(IMAGE_GRID_PT_DISPLAY, true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, IMAGE_GRID_PT_DISPLAY);
  STRNCPY_UTF8(pt->label, N_("Display Settings"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Adjust display settings for the image grid");
  pt->draw = image_grid_display_panel_draw;
  BLI_addtail(&region_type->paneltypes, pt);
  WM_paneltype_add(pt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Catalog Selector Popover
 * \{ */

static void image_grid_catalog_selector_region_listen(const wmRegionListenerParams *params)
{
  /* Same restriction as #grid_catalog_selector_region_listen: catalog identity does not change
   * on #ND_ASSET_LIST_READING / #ND_ASSET_LIST_PREVIEW, and rebuilding this nested popover on
   * those notifiers is the window in which a chevron click can resolve against a freed #uiBlock. */
  const wmNotifier *wmn = params->notifier;
  if (wmn->category == NC_ASSET && wmn->data == ND_ASSET_CATALOGS) {
    ED_region_tag_redraw(params->region);
    ED_region_tag_refresh_ui(params->region);
  }
}

static void image_grid_catalog_selector_draw(const bContext *C, Panel *panel)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return;
  }

  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(
      *owner, ed::image_grid::image_grid_slot_from_context(*C));

  ui::Layout &layout = *panel->layout;
  layout.operator_context_set(wm::OpCallContext::InvokeDefault);

  if (ed::image_grid::image_grid_library_is_missing(
          *owner, ed::image_grid::image_grid_slot_from_context(*C)))
  {
    layout.label(IFACE_("Library not found"), ICON_ERROR);
    return;
  }

  ed::asset::list::storage_fetch(&state.filter.lib_ref, C);

  ui::CatalogCheckboxSetConfig config;
  config.catalog_memory_domain = image_grid_catalog_memory_domain;
  config.all_libraries_mode = (state.filter.lib_ref.type == ASSET_LIBRARY_ALL);
  config.all_mode_libraries_fn = [] {
    return image_grid_all_mode_libraries();
  };
  config.after_change = [](bContext *C) { image_grid_catalog_selector_after_change(*C); };
  config.on_cleared_all = [](bContext *C) { image_grid_catalog_selector_cleared_all(*C); };
  config.is_all_active = [](bContext *C) {
    return image_grid_catalog_selector_is_all_active(*C);
  };
  config.is_section_expanded = [](bContext *C, const char *key) {
    return image_grid_catalog_section_is_expanded(*C, key);
  };
  config.set_section_expanded = [](bContext *C, const char *key, const bool expanded) {
    image_grid_catalog_section_set_expanded(*C, key, expanded);
  };

  if (state.filter.lib_ref.type == ASSET_LIBRARY_ALL) {
    /* Warm every real image library so All-mode sections appear as they load, without blocking
     * the popover behind a single "Loading…" label. */
    image_grid_fetch_all_mode_libraries(*C);
    image_grid_catalog_sanitize_selection(state);
  }
  else {
    const asset_system::AssetLibrary *library = ed::asset::list::library_get_once_available(
        state.filter.lib_ref);
    if (!library) {
      layout.label(IFACE_("Loading\xe2\x80\xa6"), ICON_NONE);
      return;
    }
    image_grid_catalog_sanitize_selection(state);
    config.single_library = library;
  }

  ui::Block *block = layout.block();
  /* Distinct view idnames per mode: All-Libraries inserts #LibrarySectionItem rows the
   * single-library tree never has, and #AbstractTreeViewItem::matches_single keys only on label. */
  const char *view_idname = (state.filter.lib_ref.type == ASSET_LIBRARY_ALL) ?
                                "image_grid_catalog_selector_all_libraries" :
                                "image_grid_catalog_selector";
  ui::AbstractTreeView *tree_view = ui::block_add_view(
      *block, view_idname, ui::catalog_checkbox_set_tree_create(*C, config));
  ui::TreeViewBuilder::build_tree_view(*C, *tree_view, layout);
}

void image_grid_catalog_selector_panel_register(ARegionType *region_type)
{
  if (WM_paneltype_find(IMAGE_GRID_PT_CATALOG_SELECTOR, true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, IMAGE_GRID_PT_CATALOG_SELECTOR);
  STRNCPY_UTF8(pt->label, N_("Catalog Selector"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Select the asset library and catalog to display in the image grid");
  pt->draw = image_grid_catalog_selector_draw;
  pt->listener = image_grid_catalog_selector_region_listen;
  BLI_addtail(&region_type->paneltypes, pt);
  WM_paneltype_add(pt);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Name-match filter (Map Types only)
 * \{ */

static wmOperatorStatus image_grid_name_match_enabled_toggle_exec(bContext *C, wmOperator * /*op*/)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return OPERATOR_CANCELLED;
  }

  const ImageGridSlot grid_slot = ed::image_grid::image_grid_slot_from_context(*C);
  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             grid_slot);
  state.filter.name_match.enabled = !state.filter.name_match.enabled;
  ed::image_grid::image_grid_state_persist(*owner, state, grid_slot);
  ed::image_grid::image_grid_notify_change(*C, grid_slot);
  return OPERATOR_FINISHED;
}

void IMAGE_GRID_OT_name_match_enabled_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Image Grid Name Match Filter";
  ot->idname = "IMAGE_GRID_OT_name_match_enabled_toggle";
  ot->description = "Enable or disable name matching for the image grid";
  ot->exec = image_grid_name_match_enabled_toggle_exec;
  ot->flag = OPTYPE_INTERNAL;
}

static wmOperatorStatus image_grid_name_match_map_type_toggle_exec(bContext *C, wmOperator *op)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return OPERATOR_CANCELLED;
  }

  char identifier[64];
  RNA_string_get(op->ptr, "identifier", identifier);

  const ImageGridSlot grid_slot = ed::image_grid::image_grid_slot_from_context(*C);
  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             grid_slot);
  BKE_name_match_filter_toggle_map_type(state.filter.name_match, identifier);
  state.filter.name_match.enabled = true;
  ed::image_grid::image_grid_state_persist(*owner, state, grid_slot);
  ed::image_grid::image_grid_notify_change(*C, grid_slot);
  return OPERATOR_FINISHED;
}

void IMAGE_GRID_OT_name_match_map_type_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Image Grid Name Match Map Type";
  ot->idname = "IMAGE_GRID_OT_name_match_map_type_toggle";
  ot->description = "Toggle a map type in the image grid name matching filter";
  ot->exec = image_grid_name_match_map_type_toggle_exec;
  ot->flag = OPTYPE_INTERNAL;

  RNA_def_string(ot->srna, "identifier", nullptr, 64, "Identifier", "");
}

static wmOperatorStatus image_grid_name_match_clear_exec(bContext *C, wmOperator * /*op*/)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return OPERATOR_CANCELLED;
  }

  const ImageGridSlot grid_slot = ed::image_grid::image_grid_slot_from_context(*C);
  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             grid_slot);
  BKE_name_match_filter_clear_selection(state.filter.name_match);
  ed::image_grid::image_grid_state_persist(*owner, state, grid_slot);
  ed::image_grid::image_grid_notify_change(*C, grid_slot);
  return OPERATOR_FINISHED;
}

void IMAGE_GRID_OT_name_match_clear(wmOperatorType *ot)
{
  ot->name = "Clear Image Grid Name Match Filter";
  ot->idname = "IMAGE_GRID_OT_name_match_clear";
  ot->description = "Clear active name matching map-type selections on the image grid";
  ot->exec = image_grid_name_match_clear_exec;
  ot->flag = OPTYPE_INTERNAL;
}

static void image_grid_name_match_filter_panel_draw(const bContext *C, Panel *panel)
{
  const std::optional<ed::image_grid::ImageGridOwner> owner =
      ed::image_grid::image_grid_owner_from_context(*C);
  if (!owner) {
    return;
  }

  const ImageGridSlot grid_slot = ed::image_grid::image_grid_slot_from_context(*C);
  ed::image_grid::ImageGridUIState &state = ed::image_grid::image_grid_state_get(*owner,
                                                                             grid_slot);
  ed::asset::name_match_filter_draw(
      *panel->layout,
      state.filter.name_match.enabled,
      "IMAGE_GRID_OT_name_match_map_type_toggle",
      "IMAGE_GRID_OT_name_match_clear",
      [&](const StringRef identifier) {
        return BKE_name_match_filter_map_type_is_active(state.filter.name_match, identifier);
      });
}

void image_grid_name_match_filter_panel_register(ARegionType *region_type)
{
  if (WM_paneltype_find(IMAGE_GRID_PT_NAME_MATCH_FILTER, true)) {
    return;
  }

  PanelType *pt = MEM_new_zeroed<PanelType>(__func__);
  STRNCPY_UTF8(pt->idname, IMAGE_GRID_PT_NAME_MATCH_FILTER);
  STRNCPY_UTF8(pt->label, N_("Name Match Filter"));
  STRNCPY_UTF8(pt->translation_context, BLT_I18NCONTEXT_DEFAULT_BPYRNA);
  pt->description = N_("Select map types for name matching in the image grid");
  pt->draw = image_grid_name_match_filter_panel_draw;
  BLI_addtail(&region_type->paneltypes, pt);
  WM_paneltype_add(pt);
}

/** \} */

}  // namespace blender
