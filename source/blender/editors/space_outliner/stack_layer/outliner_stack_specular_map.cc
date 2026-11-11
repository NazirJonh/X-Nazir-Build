/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * The specular-map verbs of the paint-layers stack: converting a row's specular map into the grey
 * maps the scalar channels read, and setting what a row's map stores
 * (#eMaterialPaintLayerMapEncoding). Both read the row the stack shows -- the active one, or the
 * ordinal an operator names -- and change it through the `BKE_paint_layers_*` API by marker, the
 * same way every other verb of the source does; they are a file of their own because they are the
 * Specular/Roughness map's own subject, not a row's.
 */

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"

#include "BKE_context.hh"
#include "BKE_paint_layers.hh"
#include "BKE_report.hh"

#include "BLI_utildefines.h"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_types.hh"

#include "UI_resources.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "../outliner_intern.hh"
#include "outliner_stack_layers_intern.hh"
#include "outliner_stack_source_paint_layers_intern.hh"

namespace blender::ed::outliner {

/* -------------------------------------------------------------------- */
/** \name The row the map verbs address
 * \{ */

/**
 * The row whose map the specular verbs read: the active one, or the ordinal an operator names --
 * \a op of an operator that carries the ordinal property, null for the active row. \a r_material
 * receives the owner when it is a material; null otherwise. The row alone is null when the
 * ordinal names nothing.
 */
static MaterialPaintLayer *stack_active_row_resolve(bContext &C,
                                                    wmOperator *op,
                                                    Material **r_material,
                                                    int *r_ordinal = nullptr)
{
  *r_material = nullptr;
  if (r_ordinal != nullptr) {
    *r_ordinal = -1;
  }
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
  if (space_outliner == nullptr || space_outliner->runtime == nullptr) {
    return nullptr;
  }
  ID *owner = outliner_stack_owner_get(outliner_stack_read_context(C), *space_outliner);
  if (owner == nullptr || GS(owner->name) != ID_MA) {
    return nullptr;
  }
  Material &material = layers_owner(*owner);
  if (r_material != nullptr) {
    *r_material = &material;
  }
  const int ordinal = (op != nullptr) ? stack_operator_ordinal_get(C, *space_outliner, *op) :
                                        outliner_stack_active_ordinal_get(
                                            outliner_stack_read_context(C), *space_outliner);
  MaterialPaintLayer *row = (ordinal >= 0) ?
                                paint_description_row_for_ordinal(material, ordinal) : nullptr;
  if (r_ordinal != nullptr) {
    *r_ordinal = ordinal;
  }
  return row;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Convert a Row's Specular Map
 *
 * The Specular channel feeds the Principled BSDF's "Specular IOR Level", a scalar, so a
 * specular-glossiness map cannot be slotted as it is. The converter turns the map the row's
 * Specular record carries into the grey maps the scalar channels read: the level for Specular,
 * and -- when the map packs glossiness in its alpha -- the roughness for Roughness.
 * \{ */

static const EnumPropertyItem stack_specular_convert_kind_items[] = {
    {int(PaintSpecularMapKind::SpecGlossSet),
     "SPEC_GLOSS_SET",
     0,
     "Use as Spec-Gloss Set (no bake)",
     "The map keeps its pixels: Specular reads its colour as the F0 and Roughness reads "
     "1 - glossiness from the same image's alpha. Nothing is baked, and turning the encodings "
     "back undoes it"},
    {int(PaintSpecularMapKind::ColorGlossiness),
     "COLOR_GLOSSINESS",
     0,
     "Specular-Glossiness",
     "RGB is the specular colour, alpha is glossiness: makes the level map for Specular and the "
     "roughness map for Roughness"},
    {int(PaintSpecularMapKind::Color),
     "COLOR",
     0,
     "Specular Colour",
     "RGB is the specular colour: makes the level map for Specular only"},
    {int(PaintSpecularMapKind::Level),
     "LEVEL",
     0,
     "Level",
     "The map already holds the specular level: flatten it to grey"},
    {0, nullptr, 0, nullptr, nullptr},
};

static bool stack_specular_convert_poll(bContext *C)
{
  if (!stack_mode_poll(C)) {
    return false;
  }
  Material *material = nullptr;
  MaterialPaintLayer *row = stack_active_row_resolve(*C, nullptr, &material);
  if (row == nullptr ||
      BKE_paint_layers_row_image_map_issue(*row) != PaintLayerImageMapIssue::None)
  {
    return false;
  }
  const MaterialPaintLayerChannel *record = BKE_paint_layers_row_channel_find(
      *row, PAINT_MATERIAL_CHANNEL_SPECULAR);
  return record != nullptr && record->image != nullptr;
}

static wmOperatorStatus stack_specular_convert_exec(bContext *C, wmOperator *op)
{
  Material *material = nullptr;
  int ordinal = -1;
  MaterialPaintLayer *row = stack_active_row_resolve(*C, op, &material, &ordinal);
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (material == nullptr || space_outliner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (!stack_clipboard_material_editable_check(*material, op->reports)) {
    return OPERATOR_CANCELLED;
  }
  if (row == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "The row to convert is gone");
    return OPERATOR_CANCELLED;
  }
  if (BKE_paint_layers_row_image_map_issue(*row) != PaintLayerImageMapIssue::None) {
    BKE_report(op->reports, RPT_ERROR, "Only a Fill row takes a converted map");
    return OPERATOR_CANCELLED;
  }
  const MaterialPaintLayerChannel *spec_record = BKE_paint_layers_row_channel_find(
      *row, PAINT_MATERIAL_CHANNEL_SPECULAR);
  if (spec_record == nullptr || spec_record->image == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "The row has no specular map to convert");
    return OPERATOR_CANCELLED;
  }

  const PaintSpecularMapKind kind = PaintSpecularMapKind(RNA_enum_get(op->ptr, "kind"));
  const bool make_roughness = RNA_boolean_get(op->ptr, "make_roughness");

  Image *level_image = nullptr;
  Image *rough_image = nullptr;
  const char *reason = nullptr;
  Main *bmain = CTX_data_main(C);
  if (!BKE_paint_layers_specular_map_convert(*bmain,
                                             *material,
                                             *row,
                                             *spec_record->image,
                                             kind,
                                             make_roughness,
                                             &level_image,
                                             &rough_image,
                                             &reason))
  {
    /* The converter names every refusal of its own; the row's roughness map it will not replace
     * among them, so the operator needs no check of its own. */
    BKE_report(op->reports,
               RPT_ERROR,
               (reason != nullptr) ? reason : "The specular map could not be converted");
    return OPERATOR_CANCELLED;
  }

  /* A channel the material's set leaves out takes part nowhere: a map it just got would sit on the
   * row unread. The conversion is the user's ask for one, so the channels join the set --
   * explicitly, as the set only ever changes through its own setter. */
  const auto ensure_channel = [&](const eMaterialPaintChannel channel) {
    if (!BKE_paint_layers_channel_in_set(*material, channel)) {
      BKE_paint_layers_channel_set_enable(*material, channel, true);
    }
  };
  ensure_channel(PAINT_MATERIAL_CHANNEL_SPECULAR);
  /* The roughness half was used, by a map made for it or by the same image read differently. */
  const bool used_roughness = (rough_image != nullptr) ||
                              (kind == PaintSpecularMapKind::SpecGlossSet && make_roughness);
  if (used_roughness) {
    ensure_channel(PAINT_MATERIAL_CHANNEL_ROUGHNESS);
  }
  /* The maps of the row changed, so its rows and previews rebuild; the Properties editor and the
   * previews show the material. */
  stack_row_edit_finished(*C, *space_outliner, material->id, ordinal, false);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material->id);
  if (kind == PaintSpecularMapKind::SpecGlossSet) {
    BKE_report(op->reports,
               RPT_INFO,
               (make_roughness) ?
                   "The specular reads the colour and the roughness reads the glossiness from the "
                   "same image; nothing was baked" :
                   "The specular reads the colour as the F0; nothing was baked");
  }
  else {
    BKE_report(op->reports,
               RPT_INFO,
               (rough_image != nullptr) ? "Made the specular level map and the roughness map" :
                                          "Made the specular level map");
  }
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_specular_convert(wmOperatorType *ot)
{
  ot->name = "Convert Specular Map";
  ot->description =
      "Convert the row's specular map into the grey maps the Specular and Roughness channels "
      "read: the specular colour as an IOR level, a packed glossiness as 1 - glossiness";
  ot->idname = "OUTLINER_OT_stack_layer_specular_convert";

  ot->exec = stack_specular_convert_exec;
  ot->poll = stack_specular_convert_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_enum(ot->srna,
               "kind",
               stack_specular_convert_kind_items,
               int(PaintSpecularMapKind::ColorGlossiness),
               "Kind",
               "What the specular map carries");
  RNA_def_boolean(ot->srna,
                  "make_roughness",
                  true,
                  "Make Roughness",
                  "Also make the roughness map out of the glossiness, when the kind carries it");

  rna_def_stack_row_ordinal(ot, "Row to convert; -1 uses the active one");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set What a Row's Map Stores
 *
 * The map encoding of a Roughness or a Specular record: what its map stores, which the stack reads
 * as the channel's value. A glossiness reads as `1 - value`, the spec-gloss packing as `1 - alpha`
 * with the alpha as data, a specular colour as the F0 level. The one validation point is the BKE
 * setter, which refuses a pair the channel does not take.
 * \{ */

/* The whole set the records may name, unfiltered: which pair a channel takes is the BKE setter's
 * (#BKE_paint_layers_channel_map_encoding_allowed) to refuse, not this list's to guess, so the
 * operator accepts every item and the refusal comes back as the report. */
static const EnumPropertyItem stack_map_encoding_items[] = {
    {int(MA_PAINT_LAYER_MAP_ENCODING_CHANNEL),
     "CHANNEL",
     0,
     "Channel",
     "The map stores the channel's own value"},
    {int(MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS),
     "GLOSSINESS",
     ICON_ARROW_LEFTRIGHT,
     "Glossiness",
     "The map is glossiness: the channel reads 1 - value"},
    {int(MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS_ALPHA),
     "GLOSSINESS_ALPHA",
     ICON_ARROW_LEFTRIGHT,
     "Glossiness (Alpha)",
     "The map carries glossiness in its alpha (the spec-gloss packing): the channel reads "
     "1 - alpha, and the alpha is data, not coverage"},
    {int(MA_PAINT_LAYER_MAP_ENCODING_F0),
     "F0",
     ICON_SHADERFX,
     "Specular Colour (F0)",
     "The map is the specular colour: the channel reads level = luminance / 0.08"},
    {int(MA_PAINT_LAYER_MAP_ENCODING_F0_GLOSSINESS_ALPHA),
     "F0_GLOSSINESS_ALPHA",
     ICON_SHADERFX,
     "Specular + Glossiness (Alpha)",
     "The map is the specular colour with glossiness in its alpha; the alpha is data, not "
     "coverage"},
    {0, nullptr, 0, nullptr, nullptr},
};

/** The record of the active row in \a channel, when it carries a map. */
static const MaterialPaintLayerChannel *stack_map_encoding_record(
    bContext *C, const eMaterialPaintChannel channel)
{
  if (!stack_mode_poll(C)) {
    return nullptr;
  }
  Material *material = nullptr;
  MaterialPaintLayer *row = stack_active_row_resolve(*C, nullptr, &material);
  if (row == nullptr) {
    return nullptr;
  }
  /* The encoding is read where a stack row's own map enters the stack; a correction's map is read
   * by the correction chain in the channel's scale, and a painted row's map is painted in it, so
   * the setter refuses both and the menu must not offer them. */
  if (BKE_paint_layers_owner_list(*material, *row) == nullptr ||
      BKE_paint_layers_row_image_map_issue(*row) != PaintLayerImageMapIssue::None)
  {
    return nullptr;
  }
  const MaterialPaintLayerChannel *record = BKE_paint_layers_row_channel_find(*row, channel);
  return (record != nullptr && record->image != nullptr) ? record : nullptr;
}

static bool stack_map_encoding_poll(bContext *C)
{
  return stack_map_encoding_record(C, PAINT_MATERIAL_CHANNEL_ROUGHNESS) != nullptr ||
         stack_map_encoding_record(C, PAINT_MATERIAL_CHANNEL_SPECULAR) != nullptr;
}

static wmOperatorStatus stack_map_encoding_exec(bContext *C, wmOperator *op)
{
  Material *material = nullptr;
  int ordinal = -1;
  MaterialPaintLayer *row = stack_active_row_resolve(*C, nullptr, &material, &ordinal);
  SpaceOutliner *space_outliner = CTX_wm_space_outliner(C);
  if (material == nullptr || space_outliner == nullptr) {
    return OPERATOR_CANCELLED;
  }
  if (!stack_clipboard_material_editable_check(*material, op->reports)) {
    return OPERATOR_CANCELLED;
  }
  if (row == nullptr) {
    BKE_report(op->reports, RPT_WARNING, "The row to set is gone");
    return OPERATOR_CANCELLED;
  }
  const eMaterialPaintChannel channel = eMaterialPaintChannel(RNA_enum_get(op->ptr, "channel"));
  const MaterialPaintLayerChannel *record = BKE_paint_layers_row_channel_find(*row, channel);
  if (record == nullptr || record->image == nullptr) {
    BKE_report(op->reports, RPT_ERROR, "The row has no map in the channel");
    return OPERATOR_CANCELLED;
  }

  eMaterialPaintLayerMapEncoding encoding = eMaterialPaintLayerMapEncoding(
      RNA_enum_get(op->ptr, "encoding"));
  if (RNA_boolean_get(op->ptr, "toggle")) {
    /* The quick toggle: a glossiness map back to the channel's own read, and the other way
     * round. */
    encoding = (record->map_encoding == int8_t(MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS)) ?
                   MA_PAINT_LAYER_MAP_ENCODING_CHANNEL :
                   MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS;
  }
  if (!BKE_paint_layers_channel_set_map_encoding(*material, row, channel, encoding)) {
    BKE_report(op->reports, RPT_ERROR, "The channel does not read its map that way");
    return OPERATOR_CANCELLED;
  }

  stack_row_edit_finished(*C, *space_outliner, material->id, ordinal, false);
  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &material->id);
  return OPERATOR_FINISHED;
}

void OUTLINER_OT_stack_layer_channel_map_encoding(wmOperatorType *ot)
{
  ot->name = "Set Map Encoding";
  ot->description =
      "Set what the row's map in the channel stores: a glossiness reads as 1 - value, a "
      "specular colour as the F0 level; the map itself keeps its pixels";
  ot->idname = "OUTLINER_OT_stack_layer_channel_map_encoding";

  ot->exec = stack_map_encoding_exec;
  ot->poll = stack_map_encoding_poll;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  static const EnumPropertyItem channel_items[] = {
      {int(PAINT_MATERIAL_CHANNEL_ROUGHNESS), "ROUGHNESS", 0, "Roughness", ""},
      {int(PAINT_MATERIAL_CHANNEL_SPECULAR), "SPECULAR", 0, "Specular", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };
  RNA_def_enum(ot->srna,
               "channel",
               channel_items,
               int(PAINT_MATERIAL_CHANNEL_ROUGHNESS),
               "Channel",
               "The channel whose map is read differently");
  RNA_def_enum(ot->srna,
               "encoding",
               stack_map_encoding_items,
               int(MA_PAINT_LAYER_MAP_ENCODING_GLOSSINESS),
               "Encoding",
               "What the map stores");
  RNA_def_boolean(ot->srna,
                  "toggle",
                  false,
                  "Toggle Glossiness",
                  "Switch between reading the map as glossiness and as the channel's own value");
}

/** \} */

}  // namespace blender::ed::outliner
