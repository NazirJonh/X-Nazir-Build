/* SPDX-FileCopyrightText: 2009 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup RNA
 */

#include <cstdlib>

#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "RNA_define.hh"

#include "rna_internal.hh" /* own include */

namespace blender {

/**
 * Bakeable Principled channels.
 *
 * Height, ambient occlusion and Custom are absent on purpose: the resolver has no Principled input
 * to bake them from, so offering them would only produce empty maps. The companion array in
 * `rna_image.cc` carries a leading None for reading a link back, which a flag enum must not have.
 */
static const EnumPropertyItem rna_enum_bake_paint_channel_items[] = {
    {PAINT_MATERIAL_CHANNEL_BASE_COLOR, "BASE_COLOR", 0, "Base Color", ""},
    {PAINT_MATERIAL_CHANNEL_METALLIC, "METALLIC", 0, "Metallic", ""},
    {PAINT_MATERIAL_CHANNEL_ROUGHNESS, "ROUGHNESS", 0, "Roughness", ""},
    {PAINT_MATERIAL_CHANNEL_SPECULAR, "SPECULAR", 0, "Specular", ""},
    {PAINT_MATERIAL_CHANNEL_NORMAL, "NORMAL", 0, "Normal", ""},
    {PAINT_MATERIAL_CHANNEL_ALPHA, "ALPHA", 0, "Alpha", ""},
    {PAINT_MATERIAL_CHANNEL_EMISSION, "EMISSION", 0, "Emission", ""},
    {0, nullptr, 0, nullptr, nullptr},
};

}  // namespace blender

#ifdef RNA_RUNTIME

#  include "BLI_string.h"
#  include "BLI_vector.hh"

#  include "BKE_context.hh"

#  include "DNA_material_types.h"

#  include "ED_material_bake.hh"

namespace blender {

static void rna_Material_bake_paint_channels(Material *material,
                                             bContext *C,
                                             int channels_flag,
                                             int size,
                                             const char *layer_id,
                                             bool blocking,
                                             char *result)
{
  using namespace ed::material_bake;
  result[0] = '\0';

  Vector<BakeTargetSpec> targets;
  for (const EnumPropertyItem *item = rna_enum_bake_paint_channel_items;
       item->identifier != nullptr;
       item++)
  {
    if (channels_flag & (1 << item->value)) {
      targets.append({eMaterialPaintChannel(item->value)});
    }
  }
  if (targets.is_empty()) {
    return;
  }

  MaterialBakeToImagesParams params;
  params.material = material;
  params.targets = targets;
  params.size = size;
  params.blocking = blocking;
  STRNCPY(params.layer_id, layer_id);

  const MaterialBakeToImagesResult res = material_bake_to_images(
      *CTX_data_main(C), CTX_wm_manager(C), CTX_wm_window(C), params);
  if (res.ok) {
    BLI_strncpy(result, res.layer_id, UUID_STRING_SIZE);
  }
}

}  // namespace blender

#else

namespace blender {

void RNA_api_material(StructRNA *srna)
{
  FunctionRNA *func;
  PropertyRNA *parm;

  func = RNA_def_function(srna, "bake_paint_channels", "rna_Material_bake_paint_channels");
  RNA_def_function_ui_description(
      func,
      "Bake Principled BSDF channels into one image data-block each; returns the paint layer ID");
  RNA_def_function_flag(func, FUNC_USE_CONTEXT);
  parm = RNA_def_enum_flag(
      func, "channels", rna_enum_bake_paint_channel_items, 0, "Channels", "Channels to bake");
  RNA_def_parameter_flags(parm, PropertyFlag(0), PARM_REQUIRED);
  RNA_def_int(func, "size", 2048, 16, 16384, "Size", "Square map side", 16, 16384);
  RNA_def_string(func,
                 "layer_id",
                 nullptr,
                 UUID_STRING_SIZE,
                 "Layer ID",
                 "Paint layer ID to stamp on every created map; empty generates one");
  RNA_def_boolean(func, "blocking", false, "Blocking", "Run synchronously to completion");
  parm = RNA_def_string(func,
                        "result",
                        nullptr,
                        UUID_STRING_SIZE,
                        "Result",
                        "The paint layer ID, or empty when nothing could be baked");
  RNA_def_parameter_flags(parm, PROP_THICK_WRAP, ParameterFlag(0)); /* String return value. */
  RNA_def_function_output(func, parm);
}

}  // namespace blender

#endif
