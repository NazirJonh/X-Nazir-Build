/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * See #ED_paint_material_layer.hh.
 */

#include "ED_paint_material_layer.hh"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_report.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"

#include "ED_material_bake.hh"

#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include <optional>

namespace blender::ed::sculpt_paint::material_layer {

/** Whether \a marker names \a layer or anything nested under it (children, effects, mask items). */
static bool paint_layer_subtree_has_marker(const MaterialPaintLayer &layer, const bUUID &marker)
{
  if (BLI_uuid_equal(layer.marker, marker)) {
    return true;
  }
  for (const MaterialPaintLayer &child :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.children))
  {
    if (paint_layer_subtree_has_marker(child, marker)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &effect :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.effects))
  {
    if (paint_layer_subtree_has_marker(effect, marker)) {
      return true;
    }
  }
  for (const MaterialPaintLayer &mask_item :
       *reinterpret_cast<const ListBaseT<MaterialPaintLayer> *>(&layer.mask_stack))
  {
    if (paint_layer_subtree_has_marker(mask_item, marker)) {
      return true;
    }
  }
  return false;
}

bool mask_target_is_removed(const Material &ma, const bUUID &removed_marker, const int8_t target_mode)
{
  if (target_mode != PAINT_LAYER_TARGET_MASK || BLI_uuid_is_nil(ma.active_layer_marker)) {
    return false;
  }
  const MaterialPaintLayer *removed = BKE_paint_layers_find(
      const_cast<Material &>(ma), removed_marker);
  return removed != nullptr && paint_layer_subtree_has_marker(*removed, ma.active_layer_marker);
}

MaterialPaintLayer *add_material_layer_from_material(bContext &C,
                                                     Material &owner,
                                                     Material &source,
                                                     MaterialPaintLayer *anchor,
                                                     const PaintLayerPlace place)
{
  using namespace ed::material_bake;
  Main *bmain = CTX_data_main(&C);
  if (bmain == nullptr) {
    return nullptr;
  }
  /* The layer re-bakes from its source, so a linked source has to be local first; the same the
   * old material drop did. */
  Material *picked = &source;
  if (ID_IS_LINKED(&picked->id)) {
    BKE_lib_id_make_local(bmain, &picked->id, 0);
    if (picked->id.newid != nullptr) {
      Material *local = id_cast<Material *>(picked->id.newid);
      picked->id.newid = nullptr;
      picked = local;
    }
    if (ID_IS_LINKED(&picked->id)) {
      BKE_reportf(
          CTX_wm_reports(&C), RPT_ERROR, "Material \"%s\" could not be made local", picked->id.name + 2);
      return nullptr;
    }
  }

  int size = 1024;
  Scene *scene = CTX_data_scene(&C);
  if (scene != nullptr && scene->toolsettings != nullptr) {
    size = scene->toolsettings->paint_mode.new_channel_image_size;
  }
  if (size <= 0) {
    size = 1024;
  }

  MaterialPaintLayer *layer = BKE_paint_layers_add(
      owner, MA_PAINT_LAYER_SOURCE_MATERIAL, picked->id.name + 2, anchor, place);
  if (layer == nullptr) {
    return nullptr;
  }
  layer->material = picked;
  id_us_plus(&picked->id);

  Vector<BakeTargetSpec> targets;
  for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
    targets.append({channel});
  }

  wmWindowManager *wm = CTX_wm_manager(&C);
  MaterialBakeToImagesParams params;
  params.material = picked;
  params.targets = targets;
  params.size = size;
  /* The rows are taken over on the main thread before the render, so the heavy work can run in a
   * wmJob. Only without a window manager (file read, background) does it block. */
  params.blocking = (wm == nullptr);
  bool taken_over = false;
  /* Named: #FunctionRef does not own the callable, so a temporary lambda would dangle. */
  const auto hand_over = [&](const MaterialBakeToImagesResult &result) -> bool {
    Vector<int> channels;
    Vector<Image *> images;
    for (const int i : result.created.index_range()) {
      channels.append(int(result.created_channels[i]));
      images.append(result.created[i]);
    }
    BKE_paint_layers_material_bake_apply(
        *bmain, owner, *layer, size, channels.as_span(), images.as_span());
    taken_over = true;
    return true;
  };
  params.before_render = hand_over;
  material_bake_to_images(*bmain, wm, CTX_wm_window(&C), params);
  if (!taken_over) {
    /* The source fed no channel at all: the empty row is not worth keeping. */
    BKE_paint_layers_remove(owner, layer);
    BKE_reportf(CTX_wm_reports(&C),
                RPT_WARNING,
                "Material \"%s\" feeds none of the paint channels",
                picked->id.name + 2);
    return nullptr;
  }

  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &owner.id);
  return layer;
}

void mask_edit_end_if_target_removed(bContext &C, Material &ma, const bUUID &removed_marker)
{
  Scene *scene = CTX_data_scene(&C);
  if (scene == nullptr || scene->toolsettings == nullptr) {
    return;
  }
  PaintModeSettings &mode_settings = scene->toolsettings->paint_mode;
  if (!mask_target_is_removed(ma, removed_marker, mode_settings.layer_target_mode)) {
    return;
  }
  Paint *paint = BKE_paint_get_active_from_context(&C);
  Main *bmain = CTX_data_main(&C);
  if (paint == nullptr || bmain == nullptr) {
    return;
  }
  /* The mask brush goes back to the content brush; the mode is the target, not any binding. */
  BKE_paint_material_layer_target_mode_set(
      *bmain, *scene, *paint, mode_settings, PAINT_LAYER_TARGET_CONTENT);
}

}  // namespace blender::ed::sculpt_paint::material_layer
