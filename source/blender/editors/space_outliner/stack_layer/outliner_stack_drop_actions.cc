/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spoutliner
 *
 * Drop action registry for Stack Layers: built-in actions for images and materials, and a fallback
 * that hands a file to the registered `bpy.types.FileHandler` import operators, so extensions can
 * add drop types without editing the dispatch.
 */

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

#include "AS_asset_representation.hh"

#include "DNA_asset_types.h"
#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_space_types.h"
#include "DNA_uuid_types.h"

#include "BLT_translation.hh"

#include "BLI_assert.h"
#include "BLI_fileops.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "BKE_context.hh"
#include "BKE_file_handler.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
/* #ID_IS_EDITABLE needs the #LibraryRuntime definition and #LIBRARY_ASSET_EDITABLE. */
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_import.hh"
#include "BKE_report.hh"

#include "ED_asset_image_utils.hh"
#include "ED_asset_import.hh"
#include "ED_undo.hh"

#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "fmt/format.h"

#include "../outliner_intern.hh"
#include "outliner_stack_layers_intern.hh"
#include "outliner_stack_source.hh"
#include "outliner_stack_source_paint_layers_intern.hh"

namespace blender::ed::outliner {

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

/** The owner of a layered paint stack is always a material. */
static Material &layers_owner(ID &owner)
{
  BLI_assert(GS(owner.name) == ID_MA);
  return id_cast<Material &>(owner);
}

/** Whether the stack of \a material may be written to at all. */
static bool material_editable(const Material &material)
{
  return ID_IS_EDITABLE(&material.id) && !ID_IS_OVERRIDE_LIBRARY(&material.id);
}

/**
 * The material a drop would write to, or null when it may not: not a material, or linked or
 * overridden (then \a r_hint says so). The one place the const owner of a gate is cast mutable,
 * since the lookups below take a mutable material; a gate only reads through it.
 */
static Material *drop_material(const ID &owner, const char **r_hint)
{
  if (GS(owner.name) != ID_MA) {
    return nullptr;
  }
  Material &material = id_cast<Material &>(const_cast<ID &>(owner));
  if (!material_editable(material)) {
    *r_hint = TIP_("The layered material is linked or overridden");
    return nullptr;
  }
  return &material;
}

/** The material a drop payload names in \a bmain, or null when it is not in this file. */
static Material *drop_payload_material(Main *bmain, const StackDropPayload &payload)
{
  if (bmain == nullptr || payload.id_uid == 0) {
    return nullptr;
  }
  return id_cast<Material *>(BKE_libblock_find_session_uid(bmain, ID_MA, payload.id_uid));
}

/** The row or attached item a drop target names, or null when it names none. */
static MaterialPaintLayer *target_row(Material &material, const StackDropTarget &target)
{
  if (BLI_uuid_is_nil(target.anchor.row_id)) {
    return nullptr;
  }
  return BKE_paint_layers_find(material, target.anchor.row_id);
}

/** Whether \a row is one of a row's content corrections (a filter correction included). */
static bool is_content_correction(const MaterialPaintLayer *row)
{
  return row != nullptr && row->role == MA_PAINT_LAYER_ROLE_EFFECT;
}

/** Whether \a row is a correction or a filter over its row's base mask. */
static bool is_mask_item(const MaterialPaintLayer *row)
{
  return row != nullptr && row->role == MA_PAINT_LAYER_ROLE_MASK_ITEM &&
         (row->flag & MA_PAINT_LAYER_MASK_BASE) == 0;
}

/** The name a row made from \a image carries: the image's own, without a file extension. */
static void image_row_name(const Image &image, char r_name[MAX_NAME])
{
  /* The array arrives as a pointer here, so the size has to be named rather than deduced by
   * #STRNCPY. */
  BLI_strncpy(r_name, image.id.name + 2, MAX_NAME);
  BLI_path_extension_strip(r_name);
}

/** Push a new row's ordinal to the Outliner and make it what the user works on next. */
static int row_created_finish(bContext &C,
                              SpaceOutliner &space_outliner,
                              Material &material,
                              MaterialPaintLayer &row)
{
  const int ordinal = paint_layers_ordinal_of(material, &row);
  stack_row_edit_finished(C, space_outliner, material.id, ordinal, true);
  outliner_stack_row_activate(&C, space_outliner, ordinal);
  /* A mask correction is listed under its row's mask section: bring that section up so the row the
   * drop just made is not there and unseen. */
  stack_row_reveal_attached(space_outliner, ordinal);
  /* The helper above only tells the Outliner; the Properties editor and the previews show the
   * material. */
  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
  return ordinal;
}

/**
 * Whether \a payload names a file with an image extension, as opposed to an ID. Judged by the name
 * alone: the poll asks this on every mouse move, and touching the disk there would stall the drag.
 * Resolving the drop checks that the file exists when it loads it.
 */
static bool is_image_file(const StackDropPayload &payload)
{
  return !payload.filepath.empty() &&
         BLI_path_extension_check_array(payload.filepath.c_str(), imb_ext_image);
}

/** The local image an image asset stands for, marked as an asset the way a dropped one is. */
static ID *image_from_asset(Main &bmain, const asset_system::AssetRepresentation &asset)
{
  Image *image = asset::resolve_image_from_asset(bmain, asset);
  if (image != nullptr && image->id.asset_data == nullptr) {
    asset::image_mark_as_asset(image);
  }
  return (image != nullptr) ? &image->id : nullptr;
}

/**
 * Import a material asset with "Append & Reuse": the layer reads it, so it has to be local, and
 * one shared copy is better than a new one per drop.
 */
static ID *material_from_asset(bContext &C, const asset_system::AssetRepresentation &asset)
{
  return asset::asset_local_id_ensure_imported(
      *CTX_data_main(&C), asset, 0, ASSET_IMPORT_APPEND_REUSE, std::nullopt, CTX_wm_reports(&C));
}

/**
 * Open the channel popup that puts \a payload's image into \a anchor's row, or into a new Fill
 * layer or correction beside it when #StackMovePlace::Into was not asked for. The popup's own
 * operator is the undo step; the drop has nothing of its own to push.
 */
static bool image_channel_assign_invoke(bContext &C,
                                        const StackDropPayload &payload,
                                        const MaterialPaintLayer *anchor,
                                        const StackDropTarget &target,
                                        const wmEvent *event)
{
  PointerRNA props = WM_operator_properties_create(
      "OUTLINER_OT_stack_layer_channel_image_assign");
  RNA_int_set(&props, "image_uid", int(payload.id_uid));
  if (anchor != nullptr) {
    char marker_str[UUID_STRING_SIZE];
    BLI_uuid_format(marker_str, anchor->marker);
    RNA_string_set(&props, (target.place == StackMovePlace::Into) ? "layer" : "anchor", marker_str);
  }
  RNA_boolean_set(&props, "below", target.place == StackMovePlace::Below);
  const wmOperatorStatus status = WM_operator_name_call(
      &C,
      "OUTLINER_OT_stack_layer_channel_image_assign",
      wm::OpCallContext::InvokeDefault,
      &props,
      event);
  WM_operator_properties_free(&props);
  return (status & (OPERATOR_FINISHED | OPERATOR_INTERFACE)) != 0;
}

/** Open the channel popup for an image landing beside \a target's row or correction. */
static bool image_drop_channel_popup(bContext &C,
                                     ID &owner,
                                     const StackDropPayload &payload,
                                     const StackDropTarget &target,
                                     const wmEvent *event)
{
  Material &material = layers_owner(owner);
  const MaterialPaintLayer *anchor = target_row(material, target);
  return image_channel_assign_invoke(C, payload, anchor, target, event);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Built-in Action: Image → Correction Fill
 *
 * An image dropped beside one of a row's content corrections becomes a Fill correction of that
 * row, placed where the drop line was -- between the two corrections it was aimed between. This
 * is the same "correction is a setting of its row" reading the Add menu has, with the drop line
 * naming the place among them, and the image becoming the map of the channel the popup asks for.
 * \{ */

class ImageToContentCorrectionAction final : public StackDropAction {
 public:
  int priority() const override
  {
    return 90;
  }

  bool can_handle(const StackDropPayload &payload) const override
  {
    return payload.id_type == ID_IM;
  }

  bool can_accept(const StackReadContext & /*ctx*/,
                  const ID &owner,
                  const StackDropPayload & /*payload*/,
                  const StackDropTarget &target,
                  const char **r_disabled_hint) const override
  {
    Material *material = drop_material(owner, r_disabled_hint);
    if (material == nullptr || target.place == StackMovePlace::Into) {
      return false;
    }
    return is_content_correction(target_row(*material, target));
  }

  bool execute(bContext &C,
               const StackFocus & /*focus*/,
               ID &owner,
               const StackDropPayload &payload,
               const StackDropTarget &target,
               const wmEvent *event,
               int * /*r_affected_ordinal*/) const override
  {
    if (id_cast<Image *>(
            BKE_libblock_find_session_uid(CTX_data_main(&C), ID_IM, payload.id_uid)) == nullptr)
    {
      return false;
    }
    return image_drop_channel_popup(C, owner, payload, target, event);
  }

  std::string tooltip(const StackDropPayload & /*payload*/,
                      StringRef item_name,
                      StringRef row_name,
                      const StackDropTarget &target) const override
  {
    if (row_name.is_empty()) {
      return fmt::format(fmt::runtime(TIP_("Add {} as a Correction Fill on top")), item_name);
    }
    return fmt::format(fmt::runtime(target.place == StackMovePlace::Below ?
                                        TIP_("Add {} as a Correction Fill below {}") :
                                        TIP_("Add {} as a Correction Fill above {}")),
                       item_name,
                       row_name);
  }

  void id_types(Vector<short> &r_types) const override
  {
    r_types.append(ID_IM);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Built-in Action: Image → Mask Fill Correction
 *
 * An image dropped beside one of a row's mask corrections becomes a Mask Fill correction of that
 * row, placed where the drop line was. A mask item has one Base Color record and no channel
 * choice, so the drop goes straight in: the map's grey and alpha are the coverage the correction
 * lays, the same pair the generator reads for a painted mask correction. Unlike a map on the
 * content side, this needs no popup, since the mask has no channels to pick from.
 * \{ */

class ImageToMaskFillCorrectionAction final : public StackDropAction {
 public:
  int priority() const override
  {
    return 91;
  }

  bool can_handle(const StackDropPayload &payload) const override
  {
    return payload.id_type == ID_IM;
  }

  bool can_accept(const StackReadContext & /*ctx*/,
                  const ID &owner,
                  const StackDropPayload & /*payload*/,
                  const StackDropTarget &target,
                  const char **r_disabled_hint) const override
  {
    Material *material = drop_material(owner, r_disabled_hint);
    if (material == nullptr || target.place == StackMovePlace::Into) {
      return false;
    }
    return is_mask_item(target_row(*material, target));
  }

  bool execute(bContext &C,
               const StackFocus & /*focus*/,
               ID &owner,
               const StackDropPayload &payload,
               const StackDropTarget &target,
               const wmEvent * /*event*/,
               int *r_affected_ordinal) const override
  {
    Image *image = id_cast<Image *>(
        BKE_libblock_find_session_uid(CTX_data_main(&C), ID_IM, payload.id_uid));
    if (image == nullptr) {
      return false;
    }
    Material &material = layers_owner(owner);
    MaterialPaintLayer *anchor = target_row(material, target);
    MaterialPaintLayer *mask_owner = (anchor != nullptr) ?
                                         BKE_paint_layers_mask_item_owner(material, *anchor) :
                                         nullptr;
    if (mask_owner == nullptr) {
      return false;
    }
    SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
    if (space_outliner == nullptr) {
      return false;
    }

    char name[MAX_NAME];
    image_row_name(*image, name);
    MaterialPaintLayer *item = BKE_paint_layers_mask_item_add(
        material, mask_owner, MA_PAINT_LAYER_SOURCE_CONSTANT, name);
    if (item == nullptr) {
      return false;
    }
    /* The correction's own place among the mask items, and the image as the coverage its Base
     * Color record reads instead of the flat value. */
    const PaintLayerPlace place = (target.place == StackMovePlace::Below) ? PaintLayerPlace::Below :
                                                                            PaintLayerPlace::Above;
    if (!BKE_paint_layers_edit_move(material, *item, anchor, place) ||
        !BKE_paint_layers_channel_set_image(
            material, item, PAINT_MATERIAL_CHANNEL_BASE_COLOR, image))
    {
      BKE_paint_layers_mask_item_remove(material, item);
      return false;
    }
    const int ordinal = row_created_finish(C, *space_outliner, material, *item);
    if (r_affected_ordinal != nullptr) {
      *r_affected_ordinal = ordinal;
    }
    /* #OUTLINER_OT_stack_layer_id_drop carries no #OPTYPE_UNDO; this action edits directly, so it
     * owns the step the popup hands to its own operator on the content side. */
    ED_undo_push(&C, "Add Mask Fill Correction");
    return true;
  }

  std::string tooltip(const StackDropPayload & /*payload*/,
                      StringRef item_name,
                      StringRef row_name,
                      const StackDropTarget &target) const override
  {
    if (row_name.is_empty()) {
      return fmt::format(fmt::runtime(TIP_("Add {} as a Mask Fill Correction on top")), item_name);
    }
    return fmt::format(fmt::runtime(target.place == StackMovePlace::Below ?
                                        TIP_("Add {} as a Mask Fill Correction below {}") :
                                        TIP_("Add {} as a Mask Fill Correction above {}")),
                       item_name,
                       row_name);
  }

  void id_types(Vector<short> &r_types) const override
  {
    r_types.append(ID_IM);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Built-in Action: Image → Fill Layer
 *
 * An image dropped beside a row (or naming no row at all) becomes a new Fill layer showing that
 * image. The image may come from the file, from an asset library, or from a bare image file
 * dropped from outside Blender (which #StackDropHandler::drop_resolve loads into an ID_IM first).
 * \{ */

class ImageToFillLayerAction final : public StackDropAction {
 public:
  int priority() const override
  {
    return 100;
  }

  bool can_handle(const StackDropPayload &payload) const override
  {
    /* An ID_IM, or an image file that resolving has not turned into one yet. */
    return payload.id_type == ID_IM || (payload.id_type == 0 && is_image_file(payload));
  }

  bool can_accept(const StackReadContext & /*ctx*/,
                  const ID &owner,
                  const StackDropPayload & /*payload*/,
                  const StackDropTarget &target,
                  const char **r_disabled_hint) const override
  {
    Material *material = drop_material(owner, r_disabled_hint);
    if (material == nullptr) {
      return false;
    }
    /* Beside a stack row only: a correction and a mask item have their own actions, and taking
     * the drop here would try to link a stack row next to something that is not one. */
    if (const MaterialPaintLayer *anchor = target_row(*material, target)) {
      if (anchor->role != MA_PAINT_LAYER_ROLE_LAYER) {
        return false;
      }
    }
    /* Onto a row an image is that row's map, which #ImageToChannelAction takes; declining without
     * a word lets it answer instead. */
    if (target.place == StackMovePlace::Into && !BLI_uuid_is_nil(target.anchor.row_id)) {
      return false;
    }
    return true;
  }

  bool execute(bContext &C,
               const StackFocus & /*focus*/,
               ID &owner,
               const StackDropPayload &payload,
               const StackDropTarget &target,
               const wmEvent *event,
               int * /*r_affected_ordinal*/) const override
  {
    if (id_cast<Image *>(
            BKE_libblock_find_session_uid(CTX_data_main(&C), ID_IM, payload.id_uid)) == nullptr)
    {
      return false;
    }
    return image_drop_channel_popup(C, owner, payload, target, event);
  }

  std::string tooltip(const StackDropPayload & /*payload*/,
                      StringRef item_name,
                      StringRef row_name,
                      const StackDropTarget &target) const override
  {
    if (row_name.is_empty()) {
      return fmt::format(fmt::runtime(TIP_("Add {} as a new Fill layer on top")), item_name);
    }
    if (target.place == StackMovePlace::Into) {
      /* Only reached when the row's maps refuse the image: the drop will not land, but the same
       * "onto this row" wording says what was aimed at. */
      return fmt::format(fmt::runtime(TIP_("Assign {} to {}")), item_name, row_name);
    }
    return fmt::format(fmt::runtime(target.place == StackMovePlace::Below ?
                                        TIP_("Add {} as a new Fill layer below {}") :
                                        TIP_("Add {} as a new Fill layer above {}")),
                       item_name,
                       row_name);
  }

  void id_types(Vector<short> &r_types) const override
  {
    r_types.append(ID_IM);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Built-in Action: Image → Channel Map (Into)
 *
 * An image dropped onto the middle of a Fill row (#StackMovePlace::Into) is assigned to that
 * row's channel map, chosen through a popup.
 * \{ */

class ImageToChannelAction final : public StackDropAction {
 public:
  int priority() const override
  {
    return 110;
  }

  bool can_handle(const StackDropPayload &payload) const override
  {
    return payload.id_type == ID_IM;
  }

  bool can_accept(const StackReadContext & /*ctx*/,
                  const ID &owner,
                  const StackDropPayload & /*payload*/,
                  const StackDropTarget &target,
                  const char **r_disabled_hint) const override
  {
    Material *material = drop_material(owner, r_disabled_hint);
    if (material == nullptr || target.place != StackMovePlace::Into) {
      return false;
    }
    const MaterialPaintLayer *row = target_row(*material, target);
    /* Only a stack row has a map to assign: a correction or a mask item takes the image beside it
     * as another correction, never as its own map. */
    if (row == nullptr || row->role != MA_PAINT_LAYER_ROLE_LAYER) {
      return false;
    }
    /* A Paint row owns the maps its brush writes into, so a drop must not swap them; a folder
     * composites its children and a Material layer shows its source. The rule is the BKE one the
     * popup applies, so the drag and the assignment cannot disagree. */
    switch (BKE_paint_layers_row_image_map_issue(*row)) {
      case PaintLayerImageMapIssue::None:
        return true;
      case PaintLayerImageMapIssue::PaintRowOwnsMaps:
        *r_disabled_hint = TIP_(
            "The maps of a Paint layer cannot be replaced; drop the image between rows to add a "
            "Fill layer");
        return false;
      case PaintLayerImageMapIssue::TakesNoImage:
        *r_disabled_hint = TIP_(
            "Only a Fill row takes an image; drop it between rows to add a Fill layer");
        return false;
    }
    return false;
  }

  bool execute(bContext &C,
               const StackFocus & /*focus*/,
               ID &owner,
               const StackDropPayload &payload,
               const StackDropTarget &target,
               const wmEvent *event,
               int * /*r_affected_ordinal*/) const override
  {
    Material &material = layers_owner(owner);
    const MaterialPaintLayer *anchor = BKE_paint_layers_find(material, target.anchor.row_id);
    if (anchor == nullptr) {
      return false;
    }
    return image_channel_assign_invoke(C, payload, anchor, target, event);
  }

  std::string tooltip(const StackDropPayload & /*payload*/,
                      StringRef item_name,
                      StringRef row_name,
                      const StackDropTarget & /*target*/) const override
  {
    return fmt::format(fmt::runtime(TIP_("Assign {} to {}")), item_name, row_name);
  }

  bool supports_into(const StackDropPayload &payload) const override
  {
    return payload.id_type == ID_IM;
  }

  void id_types(Vector<short> &r_types) const override
  {
    r_types.append(ID_IM);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Built-in Action: Smart Material → Rows
 *
 * A Material tagged as a smart paint asset is not a layer that shows another material: it is a
 * saved part of a stack, and a drop inserts its rows (or applies its mask) at the landing place.
 * The action runs before #MaterialLayerAction, which still takes every ordinary material.
 * \{ */

/** The stack's place for a drop target: the seam's two enums are separate types. */
static PaintLayerPlace paint_layer_place_from_drop(const StackMovePlace place)
{
  switch (place) {
    case StackMovePlace::Below:
      return PaintLayerPlace::Below;
    case StackMovePlace::Into:
      return PaintLayerPlace::Into;
    case StackMovePlace::Above:
      break;
  }
  return PaintLayerPlace::Above;
}

/**
 * Inserts a Smart asset's rows, before #MaterialLayerAction. Its priority 100 sorts it ahead of
 * the Material layer action's 120, and #StackDropHandler::add_action registers it first as well, so
 * the ordering survives the sort. A Smart asset must be taken here; otherwise the ordinary
 * Material layer action would treat the saved stack as a material to show. When this action
 * refuses, the Material action refuses the same payload too, rather than quietly falling back.
 */
class SmartMaterialAssetAction final : public StackDropAction {
 public:
  int priority() const override
  {
    return 100;
  }

  bool can_handle(const StackDropPayload &payload) const override
  {
    return payload.id_type == ID_MA;
  }

  bool can_accept(const StackReadContext &ctx,
                  const ID &owner,
                  const StackDropPayload &payload,
                  const StackDropTarget &target,
                  const char **r_disabled_hint) const override
  {
    Material *material = drop_material(owner, r_disabled_hint);
    if (material == nullptr) {
      return false;
    }
    const Material *source = drop_payload_material(ctx.bmain, payload);
    if (source == nullptr || BKE_paint_layers_asset_type_get(*source) == PaintLayerAssetType::None)
    {
      /* Not a Smart asset: the ordinary Material layer action below takes it. Leave the hint
       * empty so an accepted drop has nothing to explain. */
      return false;
    }
    MaterialPaintLayer *anchor = target_row(*material, target);
    return BKE_paint_layers_asset_can_insert(*material,
                                             *source,
                                             anchor,
                                             paint_layer_place_from_drop(target.place),
                                             r_disabled_hint);
  }

  bool execute(bContext &C,
               const StackFocus & /*focus*/,
               ID &owner,
               const StackDropPayload &payload,
               const StackDropTarget &target,
               const wmEvent *event,
               int *r_affected_ordinal) const override
  {
    Material *source = drop_payload_material(CTX_data_main(&C), payload);
    if (source == nullptr) {
      return false;
    }
    const PaintLayerAssetType type = BKE_paint_layers_asset_type_get(*source);

    Material &material = layers_owner(owner);
    MaterialPaintLayer *anchor = target_row(material, target);

    if (type == PaintLayerAssetType::Mask) {
      if (anchor == nullptr) {
        return false;
      }
      /* The same mask can replace the target's or only add its corrections: the popup asks, and
       * its own operator carries the undo step. */
      PointerRNA props = WM_operator_properties_create(
          "OUTLINER_OT_stack_layer_smart_mask_apply");
      RNA_int_set(&props, "asset_uid", int(payload.id_uid));
      RNA_int_set(&props, "owner_uid", int(owner.session_uid));
      char marker_str[UUID_STRING_SIZE];
      BLI_uuid_format(marker_str, anchor->marker);
      RNA_string_set(&props, "layer", marker_str);
      const wmOperatorStatus status = WM_operator_name_call(
          &C,
          "OUTLINER_OT_stack_layer_smart_mask_apply",
          wm::OpCallContext::InvokeDefault,
          &props,
          event);
      WM_operator_properties_free(&props);
      return (status & (OPERATOR_FINISHED | OPERATOR_INTERFACE)) != 0;
    }

    PaintLayerImportResult result;
    if (!BKE_paint_layers_asset_insert(*CTX_data_main(&C),
                                       material,
                                       *source,
                                       anchor,
                                       paint_layer_place_from_drop(target.place),
                                       &result))
    {
      BKE_report(CTX_wm_reports(&C), RPT_ERROR, "Could not insert the Smart asset");
      return false;
    }
    if (result.dropped_object_masks) {
      BKE_report(CTX_wm_reports(&C), RPT_WARNING, "Object masks were not imported");
    }
    if (result.dropped_curve_patches > 0) {
      BKE_report(CTX_wm_reports(&C), RPT_WARNING, "Curve Patch effects were not imported");
    }

    const int ordinal = (result.first != nullptr) ? paint_layers_ordinal_of(material, result.first) :
                                                    -1;
    if (r_affected_ordinal != nullptr) {
      *r_affected_ordinal = ordinal;
    }
    /* The drop operator carries no #OPTYPE_UNDO: this action owns both the invalidation the row
     * helpers would have done and its undo step. */
    if (SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C)) {
      stack_row_edit_finished(C, *space_outliner, material.id, ordinal, true);
    }
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &material.id);
    ED_undo_push(&C, "Insert Smart Material");
    return true;
  }

  std::string tooltip(const StackDropPayload & /*payload*/,
                      StringRef item_name,
                      StringRef /*row_name*/,
                      const StackDropTarget & /*target*/) const override
  {
    return fmt::format(fmt::runtime(TIP_("Insert smart asset {}")), item_name);
  }

  void id_types(Vector<short> &r_types) const override
  {
    r_types.append(ID_MA);
  }

};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Built-in Action: Material → Material Layer
 *
 * A material dropped onto the stack becomes a Material layer that shows what it shows. The
 * material may come from the file or from an asset library (imported with "Append & Reuse").
 * A material is always a row of its own, so a drop onto the middle of a row reads as one beside
 * it.
 * \{ */

class MaterialLayerAction final : public StackDropAction {
 public:
  int priority() const override
  {
    return 120;
  }

  bool can_handle(const StackDropPayload &payload) const override
  {
    return payload.id_type == ID_MA;
  }

  bool can_accept(const StackReadContext &ctx,
                  const ID &owner,
                  const StackDropPayload &payload,
                  const StackDropTarget & /*target*/,
                  const char **r_disabled_hint) const override
  {
    const Material *material = drop_material(owner, r_disabled_hint);
    if (material == nullptr) {
      return false;
    }
    /* The same refusals the Add reports: the drop states them up front, as a hint on the gesture
     * the user is making right now. */
    if (const Material *candidate = drop_payload_material(ctx.bmain, payload)) {
      /* A smart asset is a saved stack, not a material to read: it is inserted as rows by
       * #SmartMaterialAssetAction, and when that refuses, the drop is refused rather than
       * quietly becoming a Material layer. */
      if (BKE_paint_layers_asset_type_get(*candidate) != PaintLayerAssetType::None) {
        *r_disabled_hint = TIP_("That material is a Smart asset; drop it on a layer to insert it");
        return false;
      }
      switch (BKE_paint_layers_material_source_issue(*material, *candidate)) {
        case PaintLayerMaterialSourceIssue::None:
          break;
        case PaintLayerMaterialSourceIssue::Self:
          *r_disabled_hint = TIP_("A layered material cannot read itself");
          return false;
        case PaintLayerMaterialSourceIssue::Cycle:
          *r_disabled_hint = TIP_("That material already reads this one; it would form a cycle");
          return false;
      }
    }
    return true;
  }

  bool execute(bContext &C,
               const StackFocus & /*focus*/,
               ID &owner,
               const StackDropPayload &payload,
               const StackDropTarget &target,
               const wmEvent * /*event*/,
               int *r_affected_ordinal) const override
  {
    Material *source = drop_payload_material(CTX_data_main(&C), payload);
    if (source == nullptr) {
      return false;
    }

    Material &material = layers_owner(owner);
    const MaterialPaintLayer *anchor = BLI_uuid_is_nil(target.anchor.row_id) ?
                                           nullptr :
                                           BKE_paint_layers_find(material, target.anchor.row_id);

    StackAddArgs args;
    args.source = &source->id;
    args.place = target.place;
    /* The line was drawn beside the row the drop was aimed at: an anchor folder does not swallow
     * it. */
    args.fill_folder = false;

    SpaceOutliner *space_outliner = CTX_wm_space_outliner(&C);
    if (space_outliner == nullptr) {
      return false;
    }

    const StackEditor *editor = stack_source_for_space(*space_outliner)->editor();
    if (editor == nullptr) {
      return false;
    }
    Vector<StackAddKindInfo> kinds;
    editor->add_kinds(kinds);
    const auto *material_kind = std::find_if(
        kinds.begin(), kinds.end(), [](const StackAddKindInfo &kind) {
          return kind.identifier == "MATERIAL";
        });
    if (material_kind == kinds.end()) {
      return false;
    }
    /* The same wrapper the Add uses: it invalidates the cached rows, moves the selection to the
     * new row and notifies the Outliner. */
    const int ordinal = outliner_stack_row_add(
        &C,
        *space_outliner,
        int(material_kind - kinds.begin()),
        anchor != nullptr ? paint_layers_ordinal_of(material, anchor) : -1,
        args);
    if (ordinal < 0) {
      return false;
    }
    if (r_affected_ordinal != nullptr) {
      *r_affected_ordinal = ordinal;
    }
    /* #OUTLINER_OT_stack_layer_id_drop carries no #OPTYPE_UNDO: a drop that only hands off to a
     * popup has nothing of its own to push, so the drop that edits directly pushes its own. */
    ED_undo_push(&C, "Add Material Layer");
    return true;
  }

  std::string tooltip(const StackDropPayload & /*payload*/,
                      StringRef item_name,
                      StringRef row_name,
                      const StackDropTarget &target) const override
  {
    if (row_name.is_empty()) {
      return fmt::format(fmt::runtime(TIP_("Add {} as a Material layer on top")), item_name);
    }
    return fmt::format(fmt::runtime(target.place == StackMovePlace::Below ?
                                        TIP_("Add {} as a Material layer below {}") :
                                        TIP_("Add {} as a Material layer above {}")),
                       item_name,
                       row_name);
  }

  void id_types(Vector<short> &r_types) const override
  {
    r_types.append(ID_MA);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Built-in Action: FileHandler Bridge
 *
 * An external file the built-in actions do not take is offered to the registered
 * `bpy.types.FileHandler` import operators. This is the extension point for formats the stack has
 * no vocabulary for: the handler imports the file and, when its operator declares the optional
 * target properties, learns where the drop landed.
 * \{ */

/** Whether a registered FileHandler claims \a path. */
static bool file_handler_claims_path(const std::string &path)
{
  for (const std::unique_ptr<bke::FileHandlerType> &file_handler : bke::file_handlers()) {
    if (file_handler->import_operator[0] == '\0') {
      continue;
    }
    std::string paths[1] = {path};
    if (!file_handler->filter_supported_paths(Span<std::string>(paths, 1)).is_empty()) {
      return true;
    }
  }
  return false;
}

/**
 * Fill \a props for an FileHandler import operator.
 *
 * `filepath` is what the FileHandler protocol already asks for. The drop target is passed through
 * the optional `stack_anchor`, `stack_place` and `stack_owner_material` operator properties: an
 * extension that wants to place what it imports declares them, and other import operators simply
 * never see them.
 *
 * \return false when the operator cannot take the file, having no `filepath` at all.
 */
static bool file_handler_props_write(PointerRNA &props,
                                     const std::string &filepath,
                                     const StackDropTarget &target,
                                     const Material &material)
{
  PropertyRNA *filepath_prop = RNA_struct_find_property_check(props, "filepath", PROP_STRING);
  if (filepath_prop == nullptr) {
    return false;
  }
  RNA_property_string_set(&props, filepath_prop, filepath.c_str());

  if (PropertyRNA *anchor_prop = RNA_struct_find_property_check(props,
                                                               "stack_anchor",
                                                               PROP_STRING))
  {
    char anchor_str[UUID_STRING_SIZE];
    BLI_uuid_format(anchor_str, target.anchor.row_id);
    RNA_property_string_set(&props, anchor_prop, anchor_str);
  }
  if (PropertyRNA *place_prop = RNA_struct_find_property_check(props, "stack_place", PROP_INT)) {
    RNA_property_int_set(&props, place_prop, int(target.place));
  }
  if (PropertyRNA *owner_prop = RNA_struct_find_property_check(props,
                                                               "stack_owner_material",
                                                               PROP_STRING))
  {
    RNA_property_string_set(&props, owner_prop, material.id.name + 2);
  }
  /* The name alone cannot tell a linked material from a local one of the same name; the session
   * UID can, for as long as the file stays loaded. A uint32 read through an int, like the image
   * UID of the channel popup. */
  if (PropertyRNA *uid_prop = RNA_struct_find_property_check(props, "stack_owner_uid", PROP_INT)) {
    RNA_property_int_set(&props, uid_prop, int(material.id.session_uid));
  }
  return true;
}

/**
 * The operator a FileHandler import receives is called with `filepath` and, for each of these it
 * declares, the drop target: `stack_anchor` (string, marker of the row aimed at, empty for none),
 * `stack_place` (int, #StackMovePlace), `stack_owner_material` (string, the material's name, for
 * display) and `stack_owner_uid` (int, its session UID reinterpreted as a signed int; resolve it
 * with `BKE_libblock_find_session_uid`, it is valid until the file is reloaded). Prefer the UID to
 * find the owner: names repeat across libraries.
 */
class FileHandlerDropAction final : public StackDropAction {
 public:
  int priority() const override
  {
    return 900;
  }

  bool can_handle(const StackDropPayload &payload) const override
  {
    /* Only a file nothing resolved into a data-block: an image the built-in actions refused here
     * must stay refused rather than reach a generic image importer, which would skip the reason
     * the stack gave for refusing it. */
    return payload.id_type == 0 && !payload.filepath.empty() && !is_image_file(payload) &&
           file_handler_claims_path(payload.filepath);
  }

  bool can_accept(const StackReadContext & /*ctx*/,
                  const ID &owner,
                  const StackDropPayload &payload,
                  const StackDropTarget & /*target*/,
                  const char ** /*r_disabled_hint*/) const override
  {
    /* The file is the handler's to judge; the stack only has to be a paint stack. */
    return GS(owner.name) == ID_MA && !payload.filepath.empty();
  }

  bool execute(bContext &C,
               const StackFocus & /*focus*/,
               ID &owner,
               const StackDropPayload &payload,
               const StackDropTarget &target,
               const wmEvent * /*event*/,
               int * /*r_affected_ordinal*/) const override
  {
    std::string paths[1] = {payload.filepath};
    const Vector<bke::FileHandlerType *> file_handlers = bke::file_handlers_poll_file_drop(
        &C, Span<std::string>(paths, 1));
    for (bke::FileHandlerType *file_handler : file_handlers) {
      if (file_handler->import_operator[0] == '\0') {
        continue;
      }
      wmOperatorType *ot = WM_operatortype_find(file_handler->import_operator, false);
      if (ot == nullptr) {
        continue;
      }
      PointerRNA props = WM_operator_properties_create_ptr(ot);
      const bool written = file_handler_props_write(
          props, payload.filepath, target, layers_owner(owner));
      wmOperatorStatus status = OPERATOR_CANCELLED;
      if (written) {
        status = WM_operator_name_call_ptr(
            &C, ot, wm::OpCallContext::InvokeDefault, &props, nullptr);
      }
      WM_operator_properties_free(&props);
      if (written && (status & OPERATOR_FINISHED)) {
        return true;
      }
    }
    return false;
  }

  std::string tooltip(const StackDropPayload & /*payload*/,
                      StringRef item_name,
                      StringRef /*row_name*/,
                      const StackDropTarget & /*target*/) const override
  {
    return fmt::format(fmt::runtime(TIP_("Import {}")), item_name);
  }
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name StackDropHandler Implementation (Facade)
 * \{ */

void StackDropHandler::add_action(std::unique_ptr<StackDropAction> action)
{
  actions_.append(std::move(action));
  std::sort(actions_.begin(), actions_.end(), [](const auto &a, const auto &b) {
    return a->priority() < b->priority();
  });
}

const StackDropAction *StackDropHandler::find_action_handling(
    const StackDropPayload &payload) const
{
  for (const std::unique_ptr<StackDropAction> &action : actions_) {
    if (action->can_handle(payload)) {
      return action.get();
    }
  }
  return nullptr;
}

const StackDropAction *StackDropHandler::find_action(const StackReadContext &ctx,
                                                     const ID *owner,
                                                     const StackDropPayload &payload,
                                                     const StackDropTarget &target,
                                                     const char **r_disabled_hint) const
{
  for (const std::unique_ptr<StackDropAction> &action : actions_) {
    if (!action->can_handle(payload)) {
      continue;
    }
    if (owner == nullptr || action->can_accept(ctx, *owner, payload, target, r_disabled_hint)) {
      return action.get();
    }
  }
  return nullptr;
}

bool StackDropHandler::can_accept(const StackReadContext &ctx,
                                  const ID &owner,
                                  const StackDropPayload &payload,
                                  const StackDropTarget &target,
                                  const char **r_disabled_hint) const
{
  return find_action(ctx, &owner, payload, target, r_disabled_hint) != nullptr;
}

bool StackDropHandler::execute(bContext &C,
                               const StackFocus &focus,
                               ID &owner,
                               const StackDropPayload &payload,
                               const StackDropTarget &target,
                               const wmEvent *event,
                               int *r_affected_ordinal) const
{
  const StackReadContext ctx = outliner_stack_read_context(C);
  const char *hint = nullptr;
  const StackDropAction *action = find_action(ctx, &owner, payload, target, &hint);
  if (action == nullptr) {
    return false;
  }
  return action->execute(C, focus, owner, payload, target, event, r_affected_ordinal);
}

void StackDropHandler::drop_id_types(Vector<short> &r_types) const
{
  for (const std::unique_ptr<StackDropAction> &action : actions_) {
    Vector<short> types;
    action->id_types(types);
    for (const short type : types) {
      if (!r_types.contains(type)) {
        r_types.append(type);
      }
    }
  }
}

bool StackDropHandler::drop_external_poll(const wmDrag &drag) const
{
  if (drag.type != WM_DRAG_PATH) {
    return false;
  }
  const char *path = WM_drag_get_single_path(&drag);
  if (path == nullptr) {
    return false;
  }
  StackDropPayload payload;
  payload.filepath = path;
  return find_action_handling(payload) != nullptr;
}

bool StackDropHandler::drop_supports_into(short id_type) const
{
  if (id_type == 0) {
    /* An external file always becomes a row of its own, never a map of the row it was dropped
     * on: the user did not open that row for it. */
    return false;
  }
  StackDropPayload payload;
  payload.id_type = id_type;
  for (const std::unique_ptr<StackDropAction> &action : actions_) {
    if (action->can_handle(payload) && action->supports_into(payload)) {
      return true;
    }
  }
  return false;
}

ID *StackDropHandler::drop_resolve(bContext &C, wmDrag &drag, StackDropPayload &payload) const
{
  Main &bmain = *CTX_data_main(&C);

  if (drag.type == WM_DRAG_PATH) {
    const char *path = WM_drag_get_single_path(&drag);
    if (path == nullptr) {
      return nullptr;
    }
    payload.filepath = path;
    /* A bare image file is loaded into an image, so the rest of the drop reads it as an ID_IM.
     * An image edited in Blender is taken once more rather than shared: the row the drop makes
     * paints into it, and a stroke on one user must not change what another one shows. */
    if (is_image_file(payload) && BLI_exists(path)) {
      Image *image = ed::asset::image_load_for_import(bmain, path, nullptr, true);
      if (image == nullptr) {
        return nullptr;
      }
      /* The layer that takes the image is its user. */
      id_us_min(&image->id);
      payload.id_uid = image->id.session_uid;
      payload.id_type = ID_IM;
      return &image->id;
    }
    /* Any other file is the FileHandler bridge's to take, by path. */
    return nullptr;
  }

  if (drag.type == WM_DRAG_ASSET_LIST) {
    const ListBaseT<wmDragAssetListItem> *asset_drags = WM_drag_asset_list_get(&drag);
    if (asset_drags == nullptr) {
      return nullptr;
    }
    for (const wmDragAssetListItem &item : *asset_drags) {
      const ID_Type item_idtype = item.is_external ?
                                      item.asset_data.external_info->asset->get_id_type() :
                                      (item.asset_data.local_id ? GS(item.asset_data.local_id->name) :
                                                                  ID_Type(0));
      if (item_idtype == ID_IM) {
        if (!item.is_external) {
          return item.asset_data.local_id;
        }
        return image_from_asset(bmain, *item.asset_data.external_info->asset);
      }
      if (item_idtype == ID_MA) {
        if (!item.is_external) {
          return item.asset_data.local_id;
        }
        return material_from_asset(C, *item.asset_data.external_info->asset);
      }
    }
    return nullptr;
  }

  /* A plain local data-block or a single asset drag. */
  if (ID *local = WM_drag_get_local_ID_or_import_from_asset(&C, &drag, ID_IM)) {
    return local;
  }
  if (drag.type == WM_DRAG_ASSET) {
    if (wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_IM)) {
      return image_from_asset(bmain, *asset_drag->asset);
    }
  }
  if (ID *local = WM_drag_get_local_ID(&drag, ID_MA)) {
    return local;
  }
  if (drag.type == WM_DRAG_ASSET) {
    if (wmDragAsset *asset_drag = WM_drag_get_asset_data(&drag, ID_MA)) {
      return material_from_asset(C, *asset_drag->asset);
    }
  }
  return nullptr;
}

std::string StackDropHandler::drop_tooltip(const StackReadContext &ctx,
                                           const ID *owner,
                                           const StackDropPayload &payload,
                                           StringRef item_name,
                                           StringRef row_name,
                                           const StackDropTarget &target) const
{
  const char *hint = nullptr;
  const StackDropAction *action = find_action(ctx, owner, payload, target, &hint);
  if (action == nullptr) {
    /* Nothing would take it here. A refusal with no wording of its own shows no tooltip at all:
     * saying where it would land is worse than saying nothing. */
    return {};
  }
  return action->tooltip(payload, item_name, row_name, target);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Factory for the Paint Layers Drop Handler
 * \{ */

std::unique_ptr<StackDropHandler> paint_layers_drop_handler_create()
{
  auto handler = std::make_unique<StackDropHandler>();

  handler->add_action(std::make_unique<ImageToContentCorrectionAction>());
  handler->add_action(std::make_unique<ImageToMaskFillCorrectionAction>());
  handler->add_action(std::make_unique<ImageToFillLayerAction>());
  handler->add_action(std::make_unique<ImageToChannelAction>());
  handler->add_action(std::make_unique<SmartMaterialAssetAction>());
  handler->add_action(std::make_unique<MaterialLayerAction>());
  /* Last: only a file no built-in action takes reaches the extensions. */
  handler->add_action(std::make_unique<FileHandlerDropAction>());

  return handler;
}

/** \} */

}  // namespace blender::ed::outliner
