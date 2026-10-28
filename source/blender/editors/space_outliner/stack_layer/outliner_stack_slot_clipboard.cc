/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/** \file
 * \ingroup spoutliner
 *
 * Copy & Paste a slot assignment between Stack Layers rows: a channel's map (an image) or a
 * Material row's source (a material), carried on the shared paint-slot clipboard so a paste also
 * lands in the panels' own slots.
 */

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
/* #ID_IS_EDITABLE needs the #LibraryRuntime definition and #LIBRARY_ASSET_EDITABLE. */
#include "BKE_library.hh"
#include "BKE_paint_layers.hh"
#include "BKE_report.hh"

#include "BLI_index_range.hh"
#include "BLI_utildefines.h"

#include "ED_paint_slot_clipboard.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"
#include "RNA_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_layers_intern.hh"
#include "outliner_stack_source.hh"
#include "outliner_stack_source_paint_layers_intern.hh"

namespace blender::ed::outliner {

/* -------------------------------------------------------------------- */
/** \name Copy & Paste a Slot Assignment Between Rows
 *
 * The context-menu verbs that carry a slot's assigned data-block from one row to another: a
 * channel's map (an image) or a Material row's source (a material). The copy puts the data-block
 * on the shared paint-slot clipboard and reports it (#ED_paint_slot_clipboard_copy_report), so a
 * paste also lands in the panels' own slots; the channel menu reuses the assign popup's items, so
 * a script names a channel the same way everywhere.
 * \{ */

/** The channel record of \a layer for \a channel, or null when the layer carries none. */
static const MaterialPaintLayerChannel *stack_layer_channel_find(
    const MaterialPaintLayer &layer, const eMaterialPaintChannel channel)
{
  for (const int index : IndexRange(layer.channels_num)) {
    if (eMaterialPaintChannel(layer.channels[index].channel) == channel) {
      return &layer.channels[index];
    }
  }
  return nullptr;
}

/**
 * The layered material the clipboard verbs act on, with the Outliner they run in. Null when that
 * editor or its runtime is gone, or when the data-block it shows is not a material.
 */
static Material *stack_clipboard_material_get(bContext &C, SpaceOutliner **r_space_outliner)
{
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return nullptr;
  }
  ID *owner = outliner_stack_owner_get(outliner_stack_read_context(C), *space_outliner);
  if (owner == nullptr || GS(owner->name) != ID_MA) {
    return nullptr;
  }
  *r_space_outliner = space_outliner;
  /* The owner of a layered paint stack is always a material. */
  return &id_cast<Material &>(*owner);
}

/** True when \a material takes a paste; reports why it does not otherwise. */
bool stack_clipboard_material_editable_check(const Material &material, ReportList *reports)
{
  if (ID_IS_EDITABLE(&material.id) && !ID_IS_OVERRIDE_LIBRARY(&material.id)) {
    return true;
  }
  BKE_report(reports, RPT_ERROR, "The layered material is linked or overridden");
  return false;
}

bool stack_image_assign_check(const Material &material,
                              const MaterialPaintLayer *layer,
                              const eMaterialPaintChannel channel,
                              ReportList *reports)
{
  /* An image cannot be assigned to a channel outside the material's set: the record would take
   * part nowhere. */
  if (!BKE_paint_layers_channel_in_set(material, channel)) {
    BKE_report(reports, RPT_ERROR, "The channel is not part of the material's channel set");
    return false;
  }
  if (layer == nullptr) {
    return true;
  }
  /* The same BKE rule the drop gate applies: only a Fill row takes a map; a painted row's maps
   * belong to its brush. */
  switch (BKE_paint_layers_row_image_map_issue(*layer)) {
    case PaintLayerImageMapIssue::None:
      return true;
    case PaintLayerImageMapIssue::PaintRowOwnsMaps:
      BKE_report(reports, RPT_ERROR, "The maps of a Paint layer cannot be replaced");
      return false;
    case PaintLayerImageMapIssue::TakesNoImage:
      BKE_report(reports, RPT_ERROR, "Only a Fill row takes an image");
      return false;
  }
  return true;
}

/**
 * The layer the clipboard verbs act on when no row was named: the selection, else the active row,
 * the same row the row copy/paste verbs act on. \a r_material receives the layered material the
 * row belongs to, when asked for.
 */
static MaterialPaintLayer *stack_clipboard_target_row(bContext *C,
                                                      Material **r_material = nullptr)
{
  SpaceOutliner *space_outliner = nullptr;
  Material *material = stack_clipboard_material_get(*C, &space_outliner);
  if (material == nullptr) {
    return nullptr;
  }
  if (r_material != nullptr) {
    *r_material = material;
  }
  Vector<int> selected;
  stack_selected_ordinals_get(*space_outliner, selected);
  const int ordinal = !selected.is_empty() ?
                          selected.first() :
                          outliner_stack_active_ordinal_get(outliner_stack_read_context(*C),
                                                            *space_outliner);
  return (ordinal >= 0) ? paint_description_row_for_ordinal(*material, ordinal) : nullptr;
}

/** The layer the operator was given, or the one the clipboard verbs default to. */
static MaterialPaintLayer *stack_clipboard_operator_layer(bContext &C,
                                                          SpaceOutliner &space_outliner,
                                                          wmOperator &op,
                                                          Material &material)
{
  const int ordinal = stack_operator_ordinal_get(C, space_outliner, op);
  return (ordinal >= 0) ? paint_description_row_for_ordinal(material, ordinal) : nullptr;
}

/**
 * The row a poll acts on, when the editor is ready for a clipboard verb at all. \a r_material
 * receives the layered material the row belongs to, when asked for.
 */
static const MaterialPaintLayer *stack_clipboard_poll_row(bContext *C,
                                                          Material **r_material = nullptr)
{
  if (!stack_row_clipboard_poll(C)) {
    return nullptr;
  }
  return stack_clipboard_target_row(C, r_material);
}

/**
 * A paste replaced an assignment, not a position: the rows rebuild so the previews follow the new
 * data-block, and the Properties editor and its own previews are told as well.
 */
static void stack_clipboard_paste_finished(bContext &C,
                                           SpaceOutliner &space_outliner,
                                           Material &material,
                                           MaterialPaintLayer &layer)
{
  const int ordinal = paint_layers_ordinal_of(material, &layer);
  stack_row_edit_finished(C, space_outliner, material.id, ordinal, false);
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
}

/** The RNA channel enum's item for \a channel, with the channel's own icon. */
static EnumPropertyItem stack_clipboard_channel_enum_item(const eMaterialPaintChannel channel)
{
  for (const EnumPropertyItem *channel_item = rna_enum_material_paint_channel_items;
       channel_item->identifier != nullptr;
       channel_item++)
  {
    if (channel_item->identifier[0] != '\0' && channel_item->value == int(channel)) {
      EnumPropertyItem item = *channel_item;
      item.icon = paint_channel_icon(int(channel));
      return item;
    }
  }
  return {};
}

/** The channels of the row whose map can be copied: every record that carries an image. */
static const EnumPropertyItem *stack_layer_image_copy_channel_itemf(bContext *C,
                                                                    PointerRNA * /*ptr*/,
                                                                    PropertyRNA * /*prop*/,
                                                                    bool *r_free)
{
  *r_free = true;
  EnumPropertyItem *items = nullptr;
  int items_num = 0;
  const MaterialPaintLayer *layer = stack_clipboard_target_row(C);
  if (layer != nullptr) {
    for (const int index : IndexRange(layer->channels_num)) {
      const MaterialPaintLayerChannel &record = layer->channels[index];
      if (record.image == nullptr) {
        continue;
      }
      EnumPropertyItem item = stack_clipboard_channel_enum_item(
          eMaterialPaintChannel(record.channel));
      RNA_enum_item_add(&items, &items_num, &item);
    }
  }
  RNA_enum_item_end(&items, &items_num);
  return items;
}

static bool stack_layer_image_copy_poll(bContext *C)
{
  const MaterialPaintLayer *layer = stack_clipboard_poll_row(C);
  if (layer == nullptr) {
    return false;
  }
  for (const int index : IndexRange(layer->channels_num)) {
    if (layer->channels[index].image != nullptr) {
      return true;
    }
  }
  return false;
}

static wmOperatorStatus stack_layer_image_copy_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = nullptr;
  Material *material = stack_clipboard_material_get(*C, &space_outliner);
  if (material == nullptr) {
    return OPERATOR_CANCELLED;
  }
  MaterialPaintLayer *layer = stack_clipboard_operator_layer(*C, *space_outliner, *op, *material);
  if (layer == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "The row to copy from is gone");
    return OPERATOR_CANCELLED;
  }
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  const MaterialPaintLayerChannel *record = stack_layer_channel_find(*layer, channel);
  if (record == nullptr || record->image == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "The channel has no image to copy");
    return OPERATOR_CANCELLED;
  }
  ED_paint_slot_clipboard_copy_report(C, record->image->id, op->reports);
  return OPERATOR_FINISHED;
}

static bool stack_layer_image_paste_poll(bContext *C)
{
  const MaterialPaintLayer *layer = stack_clipboard_poll_row(C);
  if (layer == nullptr) {
    return false;
  }
  if (BKE_paint_layers_row_image_map_issue(*layer) != PaintLayerImageMapIssue::None) {
    return false;
  }
  return ED_paint_slot_clipboard_resolve(C, ID_IM) != nullptr;
}

static wmOperatorStatus stack_layer_image_paste_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = nullptr;
  Material *material = stack_clipboard_material_get(*C, &space_outliner);
  if (material == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (!stack_clipboard_material_editable_check(*material, op->reports)) {
    return OPERATOR_CANCELLED;
  }
  MaterialPaintLayer *layer = stack_clipboard_operator_layer(*C, *space_outliner, *op, *material);
  if (layer == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "The row to paste into is gone");
    return OPERATOR_CANCELLED;
  }
  Image *image = id_cast<Image *>(ED_paint_slot_clipboard_resolve(C, ID_IM));
  if (image == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "No copied image to paste");
    return OPERATOR_CANCELLED;
  }
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  if (!stack_image_assign_check(*material, layer, channel, op->reports)) {
    return OPERATOR_CANCELLED;
  }
  if (BKE_paint_layers_channel_add(*material, layer, channel) == nullptr ||
      !BKE_paint_layers_channel_set_image(*material, layer, channel, image))
  {
    BKE_report(op->reports, RPT_ERROR, "The image could not be assigned to the channel");
    return OPERATOR_CANCELLED;
  }
  stack_clipboard_paste_finished(*C, *space_outliner, *material, *layer);
  return OPERATOR_FINISHED;
}

static bool stack_layer_material_copy_poll(bContext *C)
{
  const MaterialPaintLayer *layer = stack_clipboard_poll_row(C);
  return layer != nullptr && layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL &&
         layer->material != nullptr;
}

static wmOperatorStatus stack_layer_material_copy_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = nullptr;
  Material *material = stack_clipboard_material_get(*C, &space_outliner);
  if (material == nullptr) {
    return OPERATOR_CANCELLED;
  }
  MaterialPaintLayer *layer = stack_clipboard_operator_layer(*C, *space_outliner, *op, *material);
  if (layer == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "The row to copy from is gone");
    return OPERATOR_CANCELLED;
  }
  if (layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL || layer->material == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "The row is not a Material layer");
    return OPERATOR_CANCELLED;
  }
  ED_paint_slot_clipboard_copy_report(C, layer->material->id, op->reports);
  return OPERATOR_FINISHED;
}

static bool stack_layer_material_paste_poll(bContext *C)
{
  Material *material = nullptr;
  const MaterialPaintLayer *layer = stack_clipboard_poll_row(C, &material);
  if (layer == nullptr || layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return false;
  }
  const Material *source = id_cast<const Material *>(ED_paint_slot_clipboard_resolve(C, ID_MA));
  if (source == nullptr || material == nullptr) {
    return false;
  }
  return BKE_paint_layers_material_source_issue(*material, *source) ==
         PaintLayerMaterialSourceIssue::None;
}

static wmOperatorStatus stack_layer_material_paste_exec(bContext *C, wmOperator *op)
{
  SpaceOutliner *space_outliner = nullptr;
  Material *material = stack_clipboard_material_get(*C, &space_outliner);
  if (material == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (!stack_clipboard_material_editable_check(*material, op->reports)) {
    return OPERATOR_CANCELLED;
  }
  MaterialPaintLayer *layer = stack_clipboard_operator_layer(*C, *space_outliner, *op, *material);
  if (layer == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "The row to paste into is gone");
    return OPERATOR_CANCELLED;
  }
  Material *source = id_cast<Material *>(ED_paint_slot_clipboard_resolve(C, ID_MA));
  if (source == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "No copied material to paste");
    return OPERATOR_CANCELLED;
  }
  if (layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
    BKE_report(op->reports, RPT_WARNING, "The row is not a Material layer");
    return OPERATOR_CANCELLED;
  }
  switch (BKE_paint_layers_material_source_issue(*material, *source)) {
    case PaintLayerMaterialSourceIssue::None:
      break;
    case PaintLayerMaterialSourceIssue::Self:
    case PaintLayerMaterialSourceIssue::Cycle:
      BKE_report(
          op->reports, RPT_ERROR, "The copied material already reads this material: a cycle");
      return OPERATOR_CANCELLED;
  }
  if (!BKE_paint_layers_set_material(*material, layer, source)) {
    BKE_report(op->reports, RPT_ERROR, "The material could not be assigned to the row");
    return OPERATOR_CANCELLED;
  }
  stack_clipboard_paste_finished(*C, *space_outliner, *material, *layer);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_image_copy(wmOperatorType *ot)
{
  ot->name = "Copy Image";
  ot->idname = "OUTLINER_OT_stack_layer_image_copy";
  ot->description = "Copy a channel's image, to paste into another slot";
  ot->exec = stack_layer_image_copy_exec;
  ot->poll = stack_layer_image_copy_poll;
  ot->flag = OPTYPE_INTERNAL;

  rna_def_stack_row_ordinal(ot, "Row to copy from; -1 uses the selection, else the active one");
  rna_def_stack_row_marker(ot);
  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "channel", rna_enum_dummy_NULL_items, 0, "Channel", "Channel whose image to copy");
  RNA_def_enum_funcs(prop, stack_layer_image_copy_channel_itemf);
}

void OUTLINER_OT_stack_layer_image_paste(wmOperatorType *ot)
{
  ot->name = "Paste Image";
  ot->idname = "OUTLINER_OT_stack_layer_image_paste";
  ot->description = "Assign the copied image to a channel of the row";
  ot->exec = stack_layer_image_paste_exec;
  ot->poll = stack_layer_image_paste_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  rna_def_stack_row_ordinal(ot, "Row to paste into; -1 uses the selection, else the active one");
  rna_def_stack_row_marker(ot);
  PropertyRNA *prop = RNA_def_enum(
      ot->srna, "channel", rna_enum_dummy_NULL_items, 0, "Channel", "Channel the image paints");
  /* The assign popup's own items: the channels of the material's set, the same list a drop
   * offers, so both ways in read alike. */
  RNA_def_enum_funcs(prop, stack_channel_image_assign_channel_itemf);
}

void OUTLINER_OT_stack_layer_material_copy(wmOperatorType *ot)
{
  ot->name = "Copy Material";
  ot->idname = "OUTLINER_OT_stack_layer_material_copy";
  ot->description = "Copy the source material of a Material layer, to paste into another slot";
  ot->exec = stack_layer_material_copy_exec;
  ot->poll = stack_layer_material_copy_poll;
  ot->flag = OPTYPE_INTERNAL;

  rna_def_stack_row_ordinal(ot, "Row to copy from; -1 uses the selection, else the active one");
  rna_def_stack_row_marker(ot);
}

void OUTLINER_OT_stack_layer_material_paste(wmOperatorType *ot)
{
  ot->name = "Paste Material";
  ot->idname = "OUTLINER_OT_stack_layer_material_paste";
  ot->description = "Assign the copied material as a Material layer's source";
  ot->exec = stack_layer_material_paste_exec;
  ot->poll = stack_layer_material_paste_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  rna_def_stack_row_ordinal(ot, "Row to paste into; -1 uses the selection, else the active one");
  rna_def_stack_row_marker(ot);
}

/** \} */

}  // namespace blender::ed::outliner