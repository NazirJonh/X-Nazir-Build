/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * Python-facing operators for baking a material's Principled BSDF channels into #Image
 * data-blocks. The work itself lives in #blender::ed::material_bake::material_bake_to_images; this
 * file only turns operator properties into its parameters.
 */

#include <algorithm>
#include <cstddef>

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_layer_edit.hh"
#include "BKE_report.hh"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_bits.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "ED_material_bake.hh"

#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_enum_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "render_intern.hh" /* own include */

namespace blender {

using ed::material_bake::BakeTargetSpec;
using ed::material_bake::MaterialBakeToImagesParams;
using ed::material_bake::MaterialBakeToImagesResult;

/**
 * Each value is the bit for its #eMaterialPaintChannel index.
 *
 * Height, ambient occlusion and Custom are absent on purpose: the resolver has no Principled input
 * to bake them from, so offering them would only produce empty maps.
 */
static const EnumPropertyItem bake_paint_channel_items[] = {
    {1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR, "BASE_COLOR", 0, "Base Color", ""},
    {1 << PAINT_MATERIAL_CHANNEL_METALLIC, "METALLIC", 0, "Metallic", ""},
    {1 << PAINT_MATERIAL_CHANNEL_ROUGHNESS, "ROUGHNESS", 0, "Roughness", ""},
    {1 << PAINT_MATERIAL_CHANNEL_SPECULAR, "SPECULAR", 0, "Specular", ""},
    {1 << PAINT_MATERIAL_CHANNEL_NORMAL, "NORMAL", 0, "Normal", ""},
    {1 << PAINT_MATERIAL_CHANNEL_ALPHA, "ALPHA", 0, "Alpha", ""},
    {1 << PAINT_MATERIAL_CHANNEL_EMISSION, "EMISSION", 0, "Emission", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

/* -------------------------------------------------------------------- */
/** \name Bake From Material
 * \{ */

static wmOperatorStatus material_bake_from_material_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  char material_name[MAX_ID_NAME - 2];
  RNA_string_get(op->ptr, "material", material_name);
  Material *material = static_cast<Material *>(
      BLI_findstring(&bmain->materials, material_name, offsetof(ID, name) + 2));
  if (material == nullptr) {
    BKE_reportf(op->reports, RPT_ERROR, "Material '%s' not found", material_name);
    return OPERATOR_CANCELLED;
  }

  const int channels_flag = RNA_enum_get(op->ptr, "channels");
  Vector<BakeTargetSpec> targets;
  for (const EnumPropertyItem *item = bake_paint_channel_items;
       item->identifier != nullptr;
       item++)
  {
    if (channels_flag & item->value) {
      targets.append({eMaterialPaintChannel(bitscan_forward_uint(item->value))});
    }
  }
  if (targets.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No channels selected");
    return OPERATOR_CANCELLED;
  }

  MaterialBakeToImagesParams params;
  params.material = material;
  params.targets = targets;
  params.size = RNA_int_get(op->ptr, "size");
  params.blocking = RNA_boolean_get(op->ptr, "blocking");
  RNA_string_get(op->ptr, "layer_id", params.layer_id);

  const MaterialBakeToImagesResult result = ed::material_bake::material_bake_to_images(
      *bmain, CTX_wm_manager(C), CTX_wm_window(C), params);
  if (!result.ok) {
    BKE_report(op->reports,
               RPT_ERROR,
               "Material has no Principled BSDF, or none of the requested channels is available");
    return OPERATOR_CANCELLED;
  }
  if (!result.skipped_unavailable.is_empty()) {
    BKE_reportf(op->reports,
                RPT_INFO,
                "%d channel(s) skipped as unavailable",
                int(result.skipped_unavailable.size()));
  }
  return OPERATOR_FINISHED;
}

void IMAGE_OT_bake_from_material(wmOperatorType *ot)
{
  ot->name = "Bake From Material";
  ot->description = "Bake a material's Principled BSDF channels into one image data-block each";
  ot->idname = "IMAGE_OT_bake_from_material";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = material_bake_from_material_exec;

  RNA_def_string(
      ot->srna, "material", nullptr, MAX_ID_NAME - 2, "Material", "Source material name");
  RNA_def_enum_flag(ot->srna,
                    "channels",
                    bake_paint_channel_items,
                    (1 << PAINT_MATERIAL_CHANNEL_BASE_COLOR) |
                        (1 << PAINT_MATERIAL_CHANNEL_METALLIC) |
                        (1 << PAINT_MATERIAL_CHANNEL_ROUGHNESS) |
                        (1 << PAINT_MATERIAL_CHANNEL_NORMAL),
                    "Channels",
                    "Which Principled channels to bake");
  RNA_def_int(ot->srna, "size", 2048, 16, 16384, "Size", "Square map side", 16, 16384);
  RNA_def_string(ot->srna,
                 "layer_id",
                 nullptr,
                 UUID_STRING_SIZE,
                 "Layer ID",
                 "Paint layer ID to stamp on every created map; empty generates one");
  RNA_def_boolean(ot->srna,
                  "blocking",
                  false,
                  "Blocking",
                  "Run synchronously to completion instead of in a background job");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Re-bake Stale Material Sources
 * \{ */

static wmOperatorStatus material_rebake_stale_exec(bContext *C, wmOperator *op)
{
  using namespace ed::material_bake;
  Main *bmain = CTX_data_main(C);
  char layer_id[UUID_STRING_SIZE];
  RNA_string_get(op->ptr, "layer_id", layer_id);
  const bool blocking = RNA_boolean_get(op->ptr, "blocking");
  bUUID want_uuid = {};
  const bool filter_by_layer = layer_id[0] != 0 && BLI_uuid_parse_string(&want_uuid, layer_id);

  /* Grouped by source material: one #material_bake_to_images call renders the whole material once
   * for every one of its stale channels, rather than once per channel. */
  struct MaterialRebake {
    Vector<BakeTargetSpec> targets;
    int size = 0;
    char layer_id[UUID_STRING_SIZE] = "";
  };
  Map<Material *, MaterialRebake> by_material;
  for (Image &image : bmain->images) {
    if (filter_by_layer && !BLI_uuid_equal(image.paint_layer_id, want_uuid)) {
      continue;
    }
    if (!material_bake_source_is_stale(image)) {
      continue;
    }
    ImageMaterialSource source;
    if (!BKE_image_material_source_get(image, source)) {
      continue;
    }
    MaterialRebake &rebake = by_material.lookup_or_add_default(source.material);
    rebake.targets.append({eMaterialPaintChannel(source.channel)});
    /* Every target of one material was baked as a set, so any of them answers for the group. */
    rebake.size = source.bake_size;
    BLI_uuid_format(rebake.layer_id, image.paint_layer_id);
  }

  for (auto item : by_material.items()) {
    MaterialBakeToImagesParams params;
    params.material = item.key;
    params.targets = item.value.targets;
    params.size = item.value.size;
    params.blocking = blocking;
    /* The targets already exist and carry the layer; re-fill them instead of making new ones. */
    params.reuse_existing = true;
    STRNCPY(params.layer_id, item.value.layer_id);
    material_bake_to_images(*bmain, CTX_wm_manager(C), CTX_wm_window(C), params);
  }
  if (by_material.is_empty()) {
    BKE_report(op->reports, RPT_INFO, "No stale material-baked images");
  }
  return OPERATOR_FINISHED;
}

void IMAGE_OT_rebake_stale_material_sources(wmOperatorType *ot)
{
  ot->name = "Re-bake Stale Material Sources";
  ot->description = "Re-bake every image whose source material changed since it was baked";
  ot->idname = "IMAGE_OT_rebake_stale_material_sources";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = material_rebake_stale_exec;

  RNA_def_string(ot->srna,
                 "layer_id",
                 nullptr,
                 UUID_STRING_SIZE,
                 "Layer ID",
                 "Only re-bake maps of this paint layer; empty considers every layer");
  RNA_def_boolean(ot->srna,
                  "blocking",
                  false,
                  "Blocking",
                  "Run synchronously to completion instead of in a background job");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Material Paint Layer Settings
 *
 * The Layer Material tab's controls. The active layer is the one the scene's channel bindings
 * point at (#BKE_paint_material_active_layer_owner_get). Its channel states are read from the
 * stack; a Material layer's source and resolution are read back from its maps' bake links.
 * \{ */

struct ActiveMaterialLayer {
  Material *owner = nullptr;
  int ordinal = -1;
  PaintMaterialLayerKind kind = PaintMaterialLayerKind::Paint;
  /** The layer's map per channel, Disabled ones included, for every channel wired as a stack. */
  Map<int, Image *> maps;
  /** The material the layer's maps are baked from, when any of them carries a bake link. */
  Material *source = nullptr;
  /** The largest size the layer's baked maps were baked at. */
  int bake_size = 0;

  /**
   * Whether the row is re-baked from a source material. The kind decides it, not the bake link
   * alone: a Paint layer whose map happens to carry one is still painted by hand.
   */
  bool is_material() const
  {
    return this->kind == PaintMaterialLayerKind::Material && this->source != nullptr;
  }
};

/** The stack row the scene's channel bindings currently point at, whatever kind it is. */
static bool active_material_layer_find(Main &bmain,
                                       const Scene &scene,
                                       ActiveMaterialLayer &r_layer)
{
  if (scene.toolsettings == nullptr) {
    return false;
  }
  int ordinal = -1;
  Material *owner = BKE_paint_material_active_layer_owner_get(
      bmain, scene.toolsettings->paint_mode, &ordinal);
  if (owner == nullptr) {
    return false;
  }
  Vector<PaintMaterialLayerStackEntry> entries;
  if (!BKE_paint_material_layer_stack_from_material(bmain, *owner, entries)) {
    return false;
  }
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    if (entry.ordinal != ordinal) {
      continue;
    }
    r_layer.owner = owner;
    r_layer.ordinal = entry.ordinal;
    r_layer.kind = PaintMaterialLayerKind(entry.kind);
    for (const auto item : entry.channel_images.items()) {
      if (item.key < 0 || item.key >= PAINT_MATERIAL_CHANNEL_NUM || item.value == nullptr) {
        continue;
      }
      r_layer.maps.add(item.key, item.value);
      ImageMaterialSource source;
      if (BKE_image_material_source_get(*item.value, source)) {
        r_layer.source = source.material;
        r_layer.bake_size = std::max(r_layer.bake_size, source.bake_size);
      }
    }
    return true;
  }
  return false;
}

/** Point \a binding at \a image, moving the user the binding holds along with it. */
static void paint_layer_binding_set(MaterialPaintChannelImageBinding &binding, Image *image)
{
  if (binding.image == image) {
    return;
  }
  if (binding.image != nullptr) {
    id_us_min(&binding.image->id);
  }
  binding.image = image;
  if (image != nullptr) {
    id_us_plus(&image->id);
  }
  BKE_imageuser_default(&binding.iuser);
}

/** The pixel width \a image currently has, or zero when it has no buffer. */
static int paint_layer_map_size(Image &image)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  const int size = ibuf != nullptr ? ibuf->x : 0;
  BKE_image_release_ibuf(&image, ibuf, lock);
  return size;
}

static bool paint_layer_settings_poll(bContext *C)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ActiveMaterialLayer layer;
  if (scene == nullptr || !active_material_layer_find(*bmain, *scene, layer)) {
    CTX_wm_operator_poll_msg_set(C, "No active paint layer");
    return false;
  }
  if (!ID_IS_EDITABLE(&layer.owner->id) ||
      (layer.source != nullptr && !ID_IS_EDITABLE(&layer.source->id)))
  {
    CTX_wm_operator_poll_msg_set(C, "The layer's material is not editable");
    return false;
  }
  return true;
}

static bool paint_layer_material_settings_poll(bContext *C)
{
  if (!paint_layer_settings_poll(C)) {
    return false;
  }
  Main *bmain = CTX_data_main(C);
  ActiveMaterialLayer layer;
  active_material_layer_find(*bmain, *CTX_data_scene(C), layer);
  if (!layer.is_material()) {
    CTX_wm_operator_poll_msg_set(C, "The active layer is not a Material layer");
    return false;
  }
  return true;
}

/** The map \a channel of the row \a ordinal shows now, or null when it has none or is off. */
static Image *paint_layer_channel_target(Main &bmain, Material &owner, const int ordinal, const int channel)
{
  if (BKE_paint_material_layer_channel_state_get(bmain, owner, ordinal, channel) !=
      PaintMaterialLayerChannelState::Enabled)
  {
    return nullptr;
  }
  Vector<PaintMaterialLayerStackEntry> entries;
  BKE_paint_material_layer_stack_from_material(bmain, owner, entries);
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    if (entry.ordinal == ordinal) {
      return entry.channel_images.lookup_default(channel, nullptr);
    }
  }
  return nullptr;
}

static wmOperatorStatus paint_layer_channel_toggle_exec(bContext *C, wmOperator *op)
{
  using namespace ed::material_bake;
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  const int channel = RNA_enum_get(op->ptr, "channel");
  ActiveMaterialLayer layer;
  if (!active_material_layer_find(*bmain, *scene, layer)) {
    BKE_report(op->reports, RPT_ERROR, "No active paint layer");
    return OPERATOR_CANCELLED;
  }
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  MaterialPaintChannelImageBinding &binding =
      scene->toolsettings->paint_mode.channel_image_bindings[channel];

  /* The shown state -- a file from before per-channel states left some channels linked like an
   * enabled one but showing a neutral stand-in; #BKE_paint_material_layer_channel_state_get and
   * #BKE_paint_material_layer_channel_enabled_set agree that such a channel reads as Disabled and
   * restore its parked map on the way back to Enabled. */
  const PaintMaterialLayerChannelState state = BKE_paint_material_layer_channel_state_get(
      *bmain, *layer.owner, layer.ordinal, channel);
  const bool enable = state != PaintMaterialLayerChannelState::Enabled;

  if (enable && state == PaintMaterialLayerChannelState::Absent && layer.is_material()) {
    /* A Material layer's new channel is a bake of its source; the map is handed over before
     * the render starts, like every Material layer map (see #before_render). */
    const int size = layer.bake_size > 0 ? layer.bake_size :
                                           scene->toolsettings->paint_mode.new_channel_image_size;
    const BakeTargetSpec target = {eMaterialPaintChannel(channel)};
    MaterialBakeToImagesParams params;
    params.material = layer.source;
    params.targets = Span(&target, 1);
    params.size = size;
    bool handed_over = false;
    auto hand_over = [&](const MaterialBakeToImagesResult &result) {
      if (result.created.is_empty()) {
        return false;
      }
      handed_over = BKE_paint_material_layer_channel_enabled_set(
          *bmain, *layer.owner, layer.ordinal, channel, true, result.created.first(), &error);
      return handed_over;
    };
    params.before_render = hand_over;
    const MaterialBakeToImagesResult result = material_bake_to_images(
        *bmain, CTX_wm_manager(C), CTX_wm_window(C), params);
    if (!handed_over) {
      BKE_report(op->reports,
                 RPT_ERROR,
                 !result.skipped_unavailable.is_empty() ?
                     RPT_("The layer's material does not feed this channel") :
                     RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return OPERATOR_CANCELLED;
    }
  }
  else if (!BKE_paint_material_layer_channel_enabled_set(
               *bmain, *layer.owner, layer.ordinal, channel, enable, nullptr, &error))
  {
    BKE_report(op->reports, RPT_ERROR, BKE_paint_material_layer_edit_error_message(error));
    return OPERATOR_CANCELLED;
  }

  /* A Disabled bake that fell behind -- source edited, the resolution moved, or (a legacy file)
   * the parked map was never re-baked while it sat aside -- is rendered again now that it shows.
   * Read fresh rather than through \a layer's snapshot: a legacy restore just swapped the node's
   * map, and the old one -- \a layer's own copy of the pointer -- may already be freed. */
  if (enable && state == PaintMaterialLayerChannelState::Disabled && layer.is_material()) {
    if (Image *map = paint_layer_channel_target(*bmain, *layer.owner, layer.ordinal, channel)) {
      const bool resized = layer.bake_size > 0 && paint_layer_map_size(*map) != layer.bake_size;
      if (resized) {
        BKE_image_scale(map, layer.bake_size, layer.bake_size, nullptr);
        ImageMaterialSource link;
        BKE_image_material_source_get(*map, link);
        link.bake_size = layer.bake_size;
        BKE_image_material_source_set(*map, link);
      }
      if (resized || material_bake_source_is_stale(*map)) {
        material_bake_images_rebake(*bmain, *layer.source, Span<Image *>(&map, 1), layer.bake_size);
      }
    }
  }
  /* A switched-off channel is not a paint target (spec 6). */
  paint_layer_binding_set(
      binding, paint_layer_channel_target(*bmain, *layer.owner, layer.ordinal, channel));

  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &layer.owner->id);
  WM_event_add_notifier(C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_channel_toggle(wmOperatorType *ot)
{
  ot->name = "Toggle Paint Layer Channel";
  ot->description =
      "Switch this channel of the active paint layer on or off. Off keeps the map and leaves the "
      "channel to the layers below; on restores it, or gives the layer the channel when it has "
      "none";
  ot->idname = "MATERIAL_OT_paint_layer_channel_toggle";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_channel_toggle_exec;
  ot->poll = paint_layer_settings_poll;

  PropertyRNA *prop = RNA_def_enum(ot->srna,
                                   "channel",
                                   rna_enum_material_paint_channel_items,
                                   PAINT_MATERIAL_CHANNEL_BASE_COLOR,
                                   "Channel",
                                   "Channel to switch on or off");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);
}

static wmOperatorStatus paint_layer_rebake_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ActiveMaterialLayer layer;
  if (!active_material_layer_find(*bmain, *scene, layer)) {
    BKE_report(op->reports, RPT_ERROR, "No active Material paint layer");
    return OPERATOR_CANCELLED;
  }
  Vector<Image *> baked_maps;
  for (Image *image : layer.maps.values()) {
    ImageMaterialSource link;
    if (BKE_image_material_source_get(*image, link)) {
      baked_maps.append(image);
    }
  }
  if (baked_maps.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  /* Rendered whether or not the maps look current: this is for when they do not look right
   * although nothing the staleness check sees has changed -- an image the material samples was
   * edited outside of it, say, or a bake was cancelled. */
  ed::material_bake::material_bake_images_rebake(*bmain, *layer.source, baked_maps, 0);
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_rebake(wmOperatorType *ot)
{
  ot->name = "Re-bake Paint Layer";
  ot->description = "Bake every map of the active Material paint layer from its source material again";
  ot->idname = "MATERIAL_OT_paint_layer_rebake";
  /* No undo step: the maps' pixels are not part of undo, and nothing else changes. */
  ot->flag = OPTYPE_REGISTER;
  ot->exec = paint_layer_rebake_exec;
  ot->poll = paint_layer_material_settings_poll;
}

static const EnumPropertyItem paint_layer_bake_size_items[] = {
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_256, "SIZE_256", 0, "256", "256 x 256"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_512, "SIZE_512", 0, "512", "512 x 512"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_1K, "SIZE_1K", 0, "1K", "1024 x 1024"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_2K, "SIZE_2K", 0, "2K", "2048 x 2048"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_4K, "SIZE_4K", 0, "4K", "4096 x 4096"},
    {PAINT_NEW_CHANNEL_IMAGE_SIZE_8K, "SIZE_8K", 0, "8K", "8192 x 8192"},
    {0, nullptr, 0, nullptr, nullptr},
};

static wmOperatorStatus paint_layer_bake_size_set_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  const int size = RNA_enum_get(op->ptr, "size");
  ActiveMaterialLayer layer;
  if (!active_material_layer_find(*bmain, *scene, layer)) {
    BKE_report(op->reports, RPT_ERROR, "No active Material paint layer");
    return OPERATOR_CANCELLED;
  }

  Vector<Image *> baked_maps;
  for (Image *image : layer.maps.values()) {
    ImageMaterialSource link;
    if (!BKE_image_material_source_get(*image, link)) {
      continue;
    }
    /* Resized now, so the layer shows the new resolution while the bake is on its way. */
    BKE_image_scale(image, size, size, nullptr);
    link.bake_size = size;
    BKE_image_material_source_set(*image, link);
    BKE_image_partial_update_mark_full_update(image);
    WM_event_add_notifier(C, NC_IMAGE | NA_EDITED, image);
    baked_maps.append(image);
  }
  if (baked_maps.is_empty()) {
    return OPERATOR_CANCELLED;
  }
  ed::material_bake::material_bake_images_rebake(*bmain, *layer.source, baked_maps, size);

  WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, &layer.owner->id);
  return OPERATOR_FINISHED;
}

void MATERIAL_OT_paint_layer_bake_size_set(wmOperatorType *ot)
{
  ot->name = "Set Paint Layer Resolution";
  ot->description = "Resize the maps of the active Material paint layer and bake them again";
  ot->idname = "MATERIAL_OT_paint_layer_bake_size_set";
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot->exec = paint_layer_bake_size_set_exec;
  ot->poll = paint_layer_material_settings_poll;

  ot->prop = RNA_def_enum(ot->srna,
                          "size",
                          paint_layer_bake_size_items,
                          PAINT_NEW_CHANNEL_IMAGE_SIZE_2K,
                          "Resolution",
                          "Square side of the layer's maps");
}

/** \} */

}  // namespace blender
