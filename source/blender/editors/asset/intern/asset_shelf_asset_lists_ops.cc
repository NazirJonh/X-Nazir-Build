/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edasset
 *
 * ASSETSHELF_OT_* operators for Recent/Favorites shelf asset lists, plus D7 context → shelf
 * idname resolution.
 */

#include <algorithm>

#include "BLI_string_ref.hh"

#include "DNA_object_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_paint.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"
#include "BKE_screen.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"

#include "ED_asset.hh"
#include "ED_asset_menu_utils.hh"
#include "ED_asset_shelf.hh"
#include "ED_view3d.hh"

#include "BLT_translation.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "asset_shelf_asset_lists.hh"

namespace blender::ed::asset::shelf {

static constexpr const char *poll_msg_no_lists_shelf =
    "No asset shelf with Favorites/Recent for this context";

/**
 * Paint-mode → brush shelf idname, only when the context is actually in a brush paint mode.
 * Used as resolver step 4 (never while a temporary stamped popup region is the active region).
 */
static const char *brush_shelf_idname_from_paint_context(const bContext *C)
{
  if (const SpaceImage *sima = CTX_wm_space_image(C)) {
    if (sima->mode != SI_MODE_PAINT) {
      return nullptr;
    }
  }
  else {
    const Object *ob = CTX_data_active_object(C);
    const eObjectMode brush_modes = OB_MODE_ALL_PAINT | OB_MODE_ALL_PAINT_GPENCIL |
                                    OB_MODE_SCULPT_CURVES;
    if (!ob || !(ob->mode & brush_modes)) {
      return nullptr;
    }
  }

  const PaintMode mode = BKE_paintmode_get_active_from_context(C);
  const char *idname = brush_shelf_idname_from_paint_mode(mode);
  if (idname && shelf_supports_asset_lists(idname)) {
    return idname;
  }
  return nullptr;
}

const char *shelf_asset_lists_idname_from_context(const bContext *C)
{
  /* 1. PointerRNA CTX "asset_shelf". */
  {
    PointerRNA shelf_ptr = CTX_data_pointer_get_type(C, "asset_shelf", RNA_AssetShelf);
    if (const AssetShelf *shelf = static_cast<const AssetShelf *>(shelf_ptr.data)) {
      const char *idname = (shelf->type) ? shelf->type->idname : shelf->idname;
      if (shelf_supports_asset_lists(idname)) {
        return idname;
      }
    }
  }

  /* 2. CTX string "asset_shelf_idname". */
  if (const std::optional<StringRefNull> idname = CTX_data_string_get(C, "asset_shelf_idname")) {
    if (shelf_supports_asset_lists(*idname)) {
      return idname->c_str();
    }
  }

  /* 3. Temporary-region stamp (D7) — before paint-mode. */
  const ARegion *region = CTX_wm_region_popup(C) ? CTX_wm_region_popup(C) : CTX_wm_region(C);
  if (region && region->regiontype == RGN_TYPE_TEMPORARY) {
    if (const std::optional<StringRefNull> stamped = shelf_popup_region_idname_get(*region)) {
      if (shelf_supports_asset_lists(*stamped)) {
        return stamped->c_str();
      }
    }
    /* In a temporary region without a usable stamp: do not fall through to paint-mode (D7). */
    return nullptr;
  }

  /* 4. Brush paint-mode fallback. */
  return brush_shelf_idname_from_paint_context(C);
}

static bool shelf_asset_lists_poll(bContext *C)
{
  if (!shelf_asset_lists_idname_from_context(C)) {
    CTX_wm_operator_poll_msg_set(C, poll_msg_no_lists_shelf);
    return false;
  }
  return true;
}

/**
 * Build a weak-ref from the operator's asset-reference RNA props (already stamped by the star /
 * drag source from a live #AssetRepresentation).
 *
 * Do **not** re-resolve through #operator_asset_reference_props_get_asset_from_all_library here:
 * custom image libraries are often fetched only for the shelf, and may not yet appear in the
 * combined ALL-library storage (same race #image_shelf_activate_asset_exec documents). List ops
 * only need the weak-ref identity for JSON membership.
 */
static AssetWeakReference weak_ref_from_operator_asset_props(PointerRNA &ptr)
{
  AssetWeakReference weak_ref{};
  weak_ref.asset_library_type = eAssetLibraryType(RNA_enum_get(&ptr, "asset_library_type"));
  weak_ref.asset_library_identifier = RNA_string_get_alloc(
      &ptr, "asset_library_identifier", nullptr, 0, nullptr);
  weak_ref.relative_asset_identifier = RNA_string_get_alloc(
      &ptr, "relative_asset_identifier", nullptr, 0, nullptr);
  return weak_ref;
}

/**
 * Resolve the asset weak-ref for toggle/reorder when operator asset-reference props are unset.
 * Brush shelves use #Paint::brush_asset_reference; image shelf uses the type's
 * #get_active_asset_from_context (same source as the shelf grid highlight).
 */
static bool active_asset_weak_ref_from_shelf(const bContext *C,
                                            const char *shelf_idname,
                                            AssetWeakReference &r_weak_ref,
                                            ReportList *reports)
{
  if (shelf_idname_is_brush_shelf(shelf_idname)) {
    const Paint *paint = BKE_paint_get_active_from_context(C);
    if (!paint || !paint->brush_asset_reference) {
      BKE_report(reports, RPT_ERROR, "No active asset");
      return false;
    }
    r_weak_ref = *paint->brush_asset_reference;
    return true;
  }

  if (StringRef(shelf_idname) == ed::view3d::IMAGE_TEXTURE_SHELF_IDNAME) {
    AssetShelfType *type = type_find_from_idname(shelf_idname);
    if (type && type->get_active_asset_from_context) {
      if (const AssetWeakReference *weak_ref = type->get_active_asset_from_context(type, C)) {
        r_weak_ref = *weak_ref;
        return true;
      }
    }
    BKE_report(reports, RPT_ERROR, "No active asset");
    return false;
  }

  BKE_report(reports, RPT_ERROR, "No active asset");
  return false;
}

static wmOperatorStatus asset_favorite_toggle_exec(bContext *C, wmOperator *op)
{
  const char *shelf_idname = shelf_asset_lists_idname_from_context(C);
  if (!shelf_idname) {
    BKE_report(op->reports, RPT_ERROR, poll_msg_no_lists_shelf);
    return OPERATOR_CANCELLED;
  }

  AssetWeakReference weak_ref;
  if (operator_asset_reference_props_is_set(*op->ptr)) {
    weak_ref = weak_ref_from_operator_asset_props(*op->ptr);
  }
  else if (!active_asset_weak_ref_from_shelf(C, shelf_idname, weak_ref, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  shelf_asset_lists_toggle_favorite(shelf_idname, weak_ref);
  WM_main_add_notifier(NC_ASSET | ND_ASSET_CATALOGS, nullptr);

  return OPERATOR_FINISHED;
}

void ASSETSHELF_OT_asset_favorite_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Asset Favorite";
  ot->description = "Add or remove the asset from the shelf's Favorites list";
  ot->idname = "ASSETSHELF_OT_asset_favorite_toggle";

  ot->exec = asset_favorite_toggle_exec;
  ot->poll = shelf_asset_lists_poll;

  /* No #OPTYPE_UNDO: favorites live in a JSON file next to the user config, outside of the
   * .blend, so an undo step could not restore them anyway. */

  operator_asset_reference_props_register(*ot->srna);
}

enum class FavoriteMove {
  Left = 0,
  Right = 1,
  Front = 2,
  Back = 3,
};

static const EnumPropertyItem favorite_move_items[] = {
    {int(FavoriteMove::Left), "LEFT", 0, "Move Left", "Move the asset one position to the left"},
    {int(FavoriteMove::Right),
     "RIGHT",
     0,
     "Move Right",
     "Move the asset one position to the right"},
    {int(FavoriteMove::Front), "FRONT", 0, "Reorder to Front", "Move the asset to the front"},
    {int(FavoriteMove::Back), "BACK", 0, "Reorder to Back", "Move the asset to the back"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus asset_favorite_reorder_exec(bContext *C, wmOperator *op)
{
  const char *shelf_idname = shelf_asset_lists_idname_from_context(C);
  if (!shelf_idname) {
    BKE_report(op->reports, RPT_ERROR, poll_msg_no_lists_shelf);
    return OPERATOR_CANCELLED;
  }

  AssetWeakReference weak_ref;
  if (operator_asset_reference_props_is_set(*op->ptr)) {
    weak_ref = weak_ref_from_operator_asset_props(*op->ptr);
  }
  else if (!active_asset_weak_ref_from_shelf(C, shelf_idname, weak_ref, op->reports)) {
    return OPERATOR_CANCELLED;
  }

  const ShelfAssetRef ref = ShelfAssetRef::from_weak_reference(weak_ref);
  const Span<ShelfAssetRef> favorites = shelf_asset_lists_favorites(shelf_idname);

  int current = -1;
  for (const int i : favorites.index_range()) {
    if (favorites[i] == ref) {
      current = i;
      break;
    }
  }
  if (current < 0) {
    BKE_report(op->reports, RPT_ERROR, "Asset is not in Favorites");
    return OPERATOR_CANCELLED;
  }

  const int count = int(favorites.size());
  int new_index = current;
  switch (FavoriteMove(RNA_enum_get(op->ptr, "direction"))) {
    case FavoriteMove::Left:
      new_index = current - 1;
      break;
    case FavoriteMove::Right:
      new_index = current + 1;
      break;
    case FavoriteMove::Front:
      new_index = 0;
      break;
    case FavoriteMove::Back:
      new_index = count - 1;
      break;
  }

  const int target = std::clamp(new_index, 0, count - 1);
  if (target == current) {
    return OPERATOR_CANCELLED;
  }

  shelf_asset_lists_reorder_favorite(shelf_idname, weak_ref, target);
  WM_main_add_notifier(NC_ASSET | ND_ASSET_CATALOGS, nullptr);

  return OPERATOR_FINISHED;
}

void ASSETSHELF_OT_asset_favorite_reorder(wmOperatorType *ot)
{
  ot->name = "Reorder Asset Favorite";
  ot->description =
      "Change the position of an asset within the shelf Favorites list. Hold Shift while "
      "dragging in the Favorites catalog to reorder";
  ot->idname = "ASSETSHELF_OT_asset_favorite_reorder";

  ot->exec = asset_favorite_reorder_exec;
  ot->poll = shelf_asset_lists_poll;

  operator_asset_reference_props_register(*ot->srna);
  RNA_def_enum(ot->srna,
               "direction",
               favorite_move_items,
               int(FavoriteMove::Left),
               "Direction",
               "Where to move the asset within Favorites");
}

/**
 * Active shelf settings from context: layout `context_ptr_set("asset_shelf")`, or for a temporary
 * popup region the stamped shelf via #shelf_asset_lists_idname_from_context +
 * #popup_shelf_get_or_create (D7; works for image and brush popovers).
 */
static const AssetShelfSettings *shelf_active_settings_from_context(const bContext *C)
{
  PointerRNA shelf_ptr = CTX_data_pointer_get_type(C, "asset_shelf", RNA_AssetShelf);
  const AssetShelf *shelf = static_cast<const AssetShelf *>(shelf_ptr.data);

  if (!shelf) {
    const ARegion *region = CTX_wm_region_popup(C) ? CTX_wm_region_popup(C) : CTX_wm_region(C);
    if (region && region->regiontype == RGN_TYPE_TEMPORARY) {
      if (const char *shelf_idname = shelf_asset_lists_idname_from_context(C)) {
        if (AssetShelfType *shelf_type = type_find_from_idname(shelf_idname)) {
          shelf = popup_shelf_get_or_create(*C, *shelf_type);
        }
      }
    }
  }

  return shelf ? &shelf->settings : nullptr;
}

static bool asset_favorite_reorder_to_poll(bContext *C)
{
  if (!shelf_asset_lists_poll(C)) {
    return false;
  }
  const AssetShelfSettings *settings = shelf_active_settings_from_context(C);
  if (!settings || !settings_is_favorites_catalog_active(*settings)) {
    CTX_wm_operator_poll_msg_set(C, "Favorites must be the active catalog to reorder by drag");
    return false;
  }
  return true;
}

static wmOperatorStatus asset_favorite_reorder_to_exec(bContext *C, wmOperator *op)
{
  const char *shelf_idname = shelf_asset_lists_idname_from_context(C);
  if (!shelf_idname) {
    BKE_report(op->reports, RPT_ERROR, poll_msg_no_lists_shelf);
    return OPERATOR_CANCELLED;
  }

  if (!operator_asset_reference_props_is_set(*op->ptr)) {
    BKE_report(op->reports, RPT_ERROR, "No source asset for reorder");
    return OPERATOR_CANCELLED;
  }
  const AssetWeakReference weak_ref = weak_ref_from_operator_asset_props(*op->ptr);

  const ShelfAssetRef ref = ShelfAssetRef::from_weak_reference(weak_ref);
  const Span<ShelfAssetRef> favorites = shelf_asset_lists_favorites(shelf_idname);

  int current = -1;
  for (const int i : favorites.index_range()) {
    if (favorites[i] == ref) {
      current = i;
      break;
    }
  }
  if (current < 0) {
    BKE_report(op->reports, RPT_ERROR, "Asset is not in Favorites");
    return OPERATOR_CANCELLED;
  }

  char target_identifier[RNA_DYN_DESCR_MAX];
  RNA_string_get(op->ptr, "target_identifier", target_identifier);
  const int target_index = favorite_index_from_identifier(favorites, target_identifier);
  if (target_index < 0) {
    return OPERATOR_CANCELLED;
  }

  const int count = int(favorites.size());
  const bool after = RNA_enum_get(op->ptr, "drop_location") != 0; /* 0 == BEFORE, 1 == AFTER */
  int target = after ? target_index + (current > target_index ? 1 : 0) :
                       target_index - (current < target_index ? 1 : 0);
  target = std::clamp(target, 0, count - 1);

  if (target == current) {
    return OPERATOR_CANCELLED;
  }

  shelf_asset_lists_reorder_favorite(shelf_idname, weak_ref, target);
  WM_main_add_notifier(NC_ASSET | ND_ASSET_CATALOGS, nullptr);

  return OPERATOR_FINISHED;
}

static const EnumPropertyItem reorder_drop_location_items[] = {
    {0, "BEFORE", 0, "Before", "Insert before the target item"},
    {1, "AFTER", 0, "After", "Insert after the target item"},
    {0, nullptr, 0, nullptr, nullptr},
};

void ASSETSHELF_OT_asset_favorite_reorder_to(wmOperatorType *ot)
{
  ot->name = "Reorder Asset Favorite To";
  ot->description = "Move an asset next to another Favorites entry (drag-and-drop reorder)";
  ot->idname = "ASSETSHELF_OT_asset_favorite_reorder_to";

  ot->exec = asset_favorite_reorder_to_exec;
  ot->poll = asset_favorite_reorder_to_poll;

  operator_asset_reference_props_register(*ot->srna);
  RNA_def_string(ot->srna,
                 "target_identifier",
                 nullptr,
                 RNA_DYN_DESCR_MAX,
                 "Target Identifier",
                 "Move next to the Favorites entry with this identifier");
  RNA_def_enum(ot->srna,
               "drop_location",
               reorder_drop_location_items,
               0,
               "Drop Location",
               "Insert before or after the target identifier");
}

static wmOperatorStatus asset_recent_clear_exec(bContext *C, wmOperator *op)
{
  const char *shelf_idname = shelf_asset_lists_idname_from_context(C);
  if (!shelf_idname) {
    BKE_report(op->reports, RPT_ERROR, poll_msg_no_lists_shelf);
    return OPERATOR_CANCELLED;
  }

  shelf_asset_lists_clear_recent(shelf_idname);
  WM_main_add_notifier(NC_ASSET | ND_ASSET_CATALOGS, nullptr);

  return OPERATOR_FINISHED;
}

void ASSETSHELF_OT_asset_recent_clear(wmOperatorType *ot)
{
  ot->name = "Clear Recent";
  ot->description = "Remove all assets from the shelf's Recent list";
  ot->idname = "ASSETSHELF_OT_asset_recent_clear";

  ot->invoke = WM_operator_confirm;
  ot->exec = asset_recent_clear_exec;
  ot->poll = shelf_asset_lists_poll;
}

static wmOperatorStatus asset_favorites_clear_exec(bContext *C, wmOperator *op)
{
  const char *shelf_idname = shelf_asset_lists_idname_from_context(C);
  if (!shelf_idname) {
    BKE_report(op->reports, RPT_ERROR, poll_msg_no_lists_shelf);
    return OPERATOR_CANCELLED;
  }

  shelf_asset_lists_clear_favorites(shelf_idname);
  WM_main_add_notifier(NC_ASSET | ND_ASSET_CATALOGS, nullptr);

  return OPERATOR_FINISHED;
}

void ASSETSHELF_OT_asset_favorites_clear(wmOperatorType *ot)
{
  ot->name = "Clear Favorites";
  ot->description = "Remove all assets from the shelf's Favorites list";
  ot->idname = "ASSETSHELF_OT_asset_favorites_clear";

  ot->invoke = WM_operator_confirm;
  ot->exec = asset_favorites_clear_exec;
  ot->poll = shelf_asset_lists_poll;
}

}  // namespace blender::ed::asset::shelf
