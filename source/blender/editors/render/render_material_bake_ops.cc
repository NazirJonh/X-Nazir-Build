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

#include <cstddef>

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_report.hh"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "ED_material_bake.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "render_intern.hh" /* own include */

namespace blender {

using ed::material_bake::BakeTargetSpec;
using ed::material_bake::MaterialBakeToImagesParams;
using ed::material_bake::MaterialBakeToImagesResult;

/**
 * Bit index `i` of the flag enum is #eMaterialPaintChannel value `i`.
 *
 * Height, ambient occlusion and Custom are absent on purpose: the resolver has no Principled input
 * to bake them from, so offering them would only produce empty maps.
 */
static const EnumPropertyItem bake_paint_channel_items[] = {
    {PAINT_MATERIAL_CHANNEL_BASE_COLOR, "BASE_COLOR", 0, "Base Color", ""},
    {PAINT_MATERIAL_CHANNEL_METALLIC, "METALLIC", 0, "Metallic", ""},
    {PAINT_MATERIAL_CHANNEL_ROUGHNESS, "ROUGHNESS", 0, "Roughness", ""},
    {PAINT_MATERIAL_CHANNEL_SPECULAR, "SPECULAR", 0, "Specular", ""},
    {PAINT_MATERIAL_CHANNEL_NORMAL, "NORMAL", 0, "Normal", ""},
    {PAINT_MATERIAL_CHANNEL_ALPHA, "ALPHA", 0, "Alpha", ""},
    {PAINT_MATERIAL_CHANNEL_EMISSION, "EMISSION", 0, "Emission", ""},
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
    if (channels_flag & (1 << item->value)) {
      targets.append({eMaterialPaintChannel(item->value)});
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

}  // namespace blender
