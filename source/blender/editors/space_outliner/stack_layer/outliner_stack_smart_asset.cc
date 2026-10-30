/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Saving a part of a paint-layer stack as a Smart asset: a Material asset carrying copies of the
 * selected rows (or of one row's mask), tagged so the stack's drop actions can insert it again.
 */

#include "BKE_asset.hh"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_report.hh"

#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_space_types.h"

#include "ED_asset.hh"

#include "MEM_guardedalloc.h"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_layers_intern.hh"
#include "outliner_stack_source.hh"
#include "outliner_stack_source_paint_layers_intern.hh"

namespace blender::ed::outliner {

namespace {

const EnumPropertyItem smart_asset_save_type_items[] = {
    {int(PaintLayerAssetType::Material),
     "MATERIAL",
     0,
     "Smart Material",
     "Save the selected rows (a folder included) as a reusable material asset"},
    {int(PaintLayerAssetType::Layer),
     "LAYER",
     0,
     "Smart Layer",
     "Save one row or folder as a reusable layer asset"},
    {int(PaintLayerAssetType::Mask),
     "MASK",
     0,
     "Smart Mask",
     "Save the selected row's mask stack as a reusable mask asset"},
    {0, nullptr, 0, nullptr, nullptr},
};

/** Whether \a row hangs on the stack itself; corrections and mask items do not. */
bool row_is_stack_row(Material &ma, const MaterialPaintLayer *row)
{
  return row != nullptr && BKE_paint_layers_owner_list(ma, *row) != nullptr;
}

/** The selected rows that are rows of the stack, bottom to top, without rows nested in a selected
 * folder. Falls back to the active row when nothing is selected. */
Vector<const MaterialPaintLayer *> selected_stack_rows(Material &ma,
                                                       SpaceOutliner &space_outliner)
{
  Vector<int> ordinals;
  stack_selected_ordinals_get(space_outliner, ordinals);
  if (!ordinals.is_empty()) {
    stack_ordinals_drop_covered_descendants(space_outliner, ordinals);
  }

  Vector<const MaterialPaintLayer *> rows;
  for (const int ordinal : ordinals) {
    const MaterialPaintLayer *row = paint_description_row_for_ordinal(ma, ordinal);
    if (row_is_stack_row(ma, row) && !rows.contains(row)) {
      rows.append(row);
    }
  }
  if (rows.is_empty()) {
    if (const MaterialPaintLayer *active = BKE_paint_layers_active_layer_get(ma)) {
      if (row_is_stack_row(ma, active)) {
        rows.append(active);
      }
    }
  }
  return rows;
}

/** The one row whose mask the operator saves, or null. */
const MaterialPaintLayer *selected_mask_row(const StackReadContext &ctx,
                                            Material &ma,
                                            SpaceOutliner &space_outliner,
                                            const Span<const MaterialPaintLayer *> rows)
{
  if (!rows.is_empty()) {
    return (rows.size() == 1 && rows[0]->mask_stack.first != nullptr) ? rows[0] : nullptr;
  }
  const int active_ordinal = outliner_stack_active_ordinal_get(ctx, space_outliner);
  const MaterialPaintLayer *active = paint_description_row_for_ordinal(ma, active_ordinal);
  if (row_is_stack_row(ma, active) && active->mask_stack.first != nullptr) {
    return active;
  }
  return nullptr;
}

/**
 * The metadata a freshly marked Smart asset carries: the type mirror, a description and the two
 * tags the Asset Browser groups it by. Kept in one place so every save type reads alike.
 */
void smart_asset_metadata_apply(Material &asset, const PaintLayerAssetType type)
{
  /* Mirror the tag into the asset metadata now that it exists, so the Asset Browser sees it too. */
  BKE_paint_layers_asset_type_set(asset, type);
  if (asset.id.asset_data == nullptr) {
    return;
  }
  if (asset.id.asset_data->description != nullptr) {
    MEM_delete_void(static_cast<void *>(asset.id.asset_data->description));
    asset.id.asset_data->description = nullptr;
  }
  asset.id.asset_data->description =
      BLI_strdup("Smart paint asset saved from a layered material's stack");
  BKE_asset_metadata_tag_ensure(asset.id.asset_data, "Paint");
  BKE_asset_metadata_tag_ensure(asset.id.asset_data, BKE_paint_layers_asset_type_label(type));
}

/** Warn about everything the saved branch carries along or loses; none of it blocks the save. */
void smart_asset_portability_report(wmOperator &op, const PaintLayerAssetPortability &portability)
{
  if (portability.foreign_reader_num > 0) {
    BKE_reportf(op.reports,
                RPT_WARNING,
                "%d Anchor Point Reader(s) point outside the saved layers and were cleared",
                portability.foreign_reader_num);
  }
  if (portability.external_image_num > 0) {
    BKE_reportf(op.reports,
                RPT_WARNING,
                "%d image(s) are linked to files; pack them or the asset will not move with the "
                "library",
                portability.external_image_num);
  }
  if (portability.movie_image_num > 0) {
    BKE_reportf(op.reports,
                RPT_WARNING,
                "%d image(s) of a sequence/movie type were saved without pixels",
                portability.movie_image_num);
  }
  if (portability.material_source_num > 0) {
    BKE_reportf(op.reports,
                RPT_WARNING,
                "%d layer(s) read other materials; those materials are copied into the asset",
                portability.material_source_num);
  }
  if (portability.object_mask_num > 0) {
    BKE_report(op.reports, RPT_WARNING, "Object masks are not saved");
  }
}

bool smart_asset_save_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  return owner != nullptr && GS(owner->name) == ID_MA &&
         stack_source_for_space(*space_outliner)->is_editable(*owner);
}

wmOperatorStatus smart_asset_save_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  const StackReadContext ctx = outliner_stack_read_context(*C);
  ID *owner = outliner_stack_owner_get(ctx, *space_outliner);
  if (owner == nullptr || GS(owner->name) != ID_MA) {
    return OPERATOR_CANCELLED;
  }
  Material &material = *id_cast<Material *>(owner);
  Main &bmain = *CTX_data_main(C);

  const PaintLayerAssetType asset_type = PaintLayerAssetType(RNA_enum_get(op->ptr, "type"));

  Vector<const MaterialPaintLayer *> rows = selected_stack_rows(material, *space_outliner);
  if (asset_type == PaintLayerAssetType::Mask) {
    const MaterialPaintLayer *mask_row = selected_mask_row(ctx, material, *space_outliner, rows);
    if (mask_row == nullptr) {
      BKE_report(
          op->reports, RPT_ERROR, "Select one layer with a mask to save as a Smart Mask");
      return OPERATOR_CANCELLED;
    }
    rows.clear();
    rows.append(mask_row);
  }
  if (rows.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "Select a layer or a folder to save");
    return OPERATOR_CANCELLED;
  }
  if (asset_type == PaintLayerAssetType::Layer && rows.size() != 1) {
    BKE_report(op->reports, RPT_ERROR, "Select one layer or folder to save as a Smart Layer");
    return OPERATOR_CANCELLED;
  }

  const PaintLayerAssetPortability portability = BKE_paint_layers_asset_portability_check(
      material,
      Span<const MaterialPaintLayer *>(rows.data(), rows.size()),
      asset_type);
  if (portability.blocking()) {
    BKE_report(
        op->reports, RPT_ERROR, "Curve Patch effects cannot be saved in a Smart asset");
    return OPERATOR_CANCELLED;
  }

  char name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "name", name);
  if (name[0] == '\0') {
    SNPRINTF(name,
             "%s %s",
             BKE_paint_layers_asset_type_label(asset_type),
             rows[0]->name);
  }

  Material *asset = BKE_paint_layers_asset_create(
      bmain,
      material,
      Span<const MaterialPaintLayer *>(rows.data(), rows.size()),
      asset_type,
      name);
  if (asset == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "Could not save the selected rows as a Smart asset");
    return OPERATOR_CANCELLED;
  }

  /* Mark after the rows are in place: a failed import must not leave an asset behind. mark_id only
   * fails before it creates asset_data, so a plain free is enough here. */
  if (!asset::mark_id(&asset->id)) {
    BKE_report(op->reports, RPT_ERROR, "The saved material could not be marked as an asset");
    BKE_id_free(&bmain, asset);
    return OPERATOR_CANCELLED;
  }
  smart_asset_metadata_apply(*asset, asset_type);
  smart_asset_portability_report(*op, portability);

  /* Synchronous: the asset has no user yet, so nothing else would ever build its stack for the
   * preview. The operator's OPTYPE_UNDO step is pushed after exec and includes the new material,
   * as it does for the other operators here that create an ID. */
  BKE_paint_layers_regenerate(bmain, *asset);
  asset::generate_preview(C, &asset->id);

  WM_main_add_notifier(NC_ID | NA_ADDED, &asset->id);
  WM_main_add_notifier(NC_ASSET | ND_ASSET_LIST | NA_ADDED, nullptr);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &asset->id);

  BKE_reportf(op->reports,
              RPT_INFO,
              "Saved %s '%s' from %d row(s)",
              BKE_paint_layers_asset_type_label(asset_type),
              asset->id.name + 2,
              int(rows.size()));
  return OPERATOR_FINISHED;
}

}  // namespace

void OUTLINER_OT_stack_layer_smart_asset_save(wmOperatorType *ot)
{
  ot->name = "Save as Smart Asset";
  ot->idname = "OUTLINER_OT_stack_layer_smart_asset_save";
  ot->description =
      "Save the selected rows, folder or mask as a reusable Smart asset in this file";
  ot->exec = smart_asset_save_exec;
  ot->poll = smart_asset_save_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "type",
               smart_asset_save_type_items,
               int(PaintLayerAssetType::Material),
               "Type",
               "What part of the stack the asset holds");
  RNA_def_string(ot->srna,
                 "name",
                 nullptr,
                 MAX_ID_NAME - 2,
                 "Name",
                 "Asset name; empty derives one from the selected row");
}

}  // namespace blender::ed::outliner
