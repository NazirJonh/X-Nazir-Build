/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Paste Selection into the 3D Viewport canvas. The clipboard holds one fragment set per canvas
 * image (one per PBR map of a Material canvas). The paste looks those images up in the material's
 * description to learn each set's channel, optionally adds a Paint row above the active one with
 * one transparent map per pasted channel (the New Layer setting), and hands the fragments to a
 * floating Transform session, so the user places them with the Transform tool's handles before
 * they are written. The maps are created through the same target-ensure path a first stroke takes,
 * so the row behaves exactly like one that was painted into.
 */

#include <algorithm>
#include <string>

#include "BLI_listbase_iterator.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_report.hh"

#include "ED_undo.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "WM_api.hh"
#include "WM_toolsystem.hh"
#include "WM_types.hh"

#include "paint_image_select_fragment.hh"
#include "paint_image_select_intern.hh"
#include "paint_image_select_paste_layer.hh"
#include "paint_image_select_view3d.hh"

namespace blender::ed::sculpt_paint {

bool image_select_paste_new_layer_enabled(bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  return scene != nullptr && scene->toolsettings != nullptr &&
         scene->toolsettings->imapaint.selection_paste_mode ==
             IMAGE_PAINT_SELECTION_PASTE_NEW_LAYER;
}

/** One map's paste: the #eMaterialPaintChannel the copied map drove (Material canvas), the copied
 * image's session UID (any other canvas) and the clipboard fragments duplicated for writing. */
struct ChannelPaste {
  int channel = -1;
  uint32_t image_uid = 0;
  Vector<SelectionTileFragment> fragments;
};

/** The image of \a bmain whose session UID is \a uid (what the clipboard keys its groups by), or
 * null when it is gone. */
static const Image *paste_layer_image_by_uid(Main &bmain, const uint32_t uid)
{
  for (Image &image : bmain.images) {
    if (uint32_t(image.id.session_uid) == uid) {
      return &image;
    }
  }
  return nullptr;
}

/** Deep copy of a clipboard fragment, marked pasted: the write-back must never lift or restore a
 * source region on the destination map. The geometry and edge policy are set before the preview
 * buffers are built, because the feather blend mask the write reads is derived from both. */
static SelectionTileFragment paste_layer_dup_fragment(const SelectionTileFragment &src)
{
  SelectionTileFragment copy = {};
  copy.geom = src.geom;
  copy.edge_policy = src.edge_policy;
  copy.is_pasted = true;
  copy.pixels.fragment_ibuf = src.pixels.fragment_ibuf ? IMB_dupImBuf(src.pixels.fragment_ibuf) :
                                                         nullptr;
  if (src.pixels.fragment_mask_ibuf != nullptr) {
    copy.pixels.fragment_mask_ibuf = IMB_dupImBuf(src.pixels.fragment_mask_ibuf);
    image_select_fragment_update_preview_buffers(copy);
  }
  return copy;
}

bool image_select_paste_clipboard(bContext *C,
                                  ReportList *reports,
                                  Object &ob,
                                  const ImageSelectClipboard &clipboard)
{
  if (clipboard.groups.is_empty()) {
    return false;
  }
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  if (bmain == nullptr || scene == nullptr || scene->toolsettings == nullptr) {
    return false;
  }
  PaintModeSettings &paint_mode = scene->toolsettings->paint_mode;

  /* Only a Material canvas painting layer content has a stack to paste into; a mask edit and any
   * other canvas paste into the current image(s). */
  Material *ma = BKE_paint_layers_active_material_get(&ob);
  const bool layered = ma != nullptr && paint_mode.canvas_source == PAINT_CANVAS_SOURCE_MATERIAL &&
                       BKE_paint_layers_target_mode(&paint_mode) == PaintLayersTargetMode::Content;
  const bool as_layer = layered && image_select_paste_new_layer_enabled(C);

  /* The channel of each clipboard group comes from the map it was copied from, looked up in the
   * material's description. The maps of the *active* layer are not the key: the copy usually comes
   * from another layer, and after the first paste the active layer is the new one. */
  Vector<ChannelPaste> pastes;
  std::string name_buf;
  for (const ImageSelectClipboardGroup &group : clipboard.groups) {
    ChannelPaste paste;
    paste.image_uid = group.image_uid;
    if (layered) {
      const Image *source = paste_layer_image_by_uid(*bmain, group.image_uid);
      PaintLayersImageUse use;
      if (source == nullptr || !BKE_paint_layers_find_image_use(*ma, *source, use) || use.is_mask) {
        continue;
      }
      paste.channel = use.channel;
      if (name_buf.empty() && use.layer != nullptr) {
        /* Name the row after the layer the copy came from. */
        name_buf = BKE_paint_layers_unique_default_name(*ma, use.layer->name);
      }
    }
    /* One fragment list per channel (per image off a Material): a duplicated clipboard group must
     * not paste the same map twice. */
    if (std::any_of(pastes.begin(), pastes.end(), [&](const ChannelPaste &other) {
          return layered ? other.channel == paste.channel : other.image_uid == paste.image_uid;
        }))
    {
      continue;
    }
    for (const SelectionTileFragment &src : group.fragments) {
      paste.fragments.append(paste_layer_dup_fragment(src));
    }
    if (!paste.fragments.is_empty()) {
      pastes.append(std::move(paste));
    }
  }
  if (pastes.is_empty()) {
    BKE_report(reports, RPT_WARNING, "Clipboard images are not on the current canvas");
    return false;
  }

  /* The pasted fragment is placed first and written on confirm: whatever was selected before no
   * longer applies to it, and a stale selection would keep constraining the canvas. */
  image_select_view3d_deselect(C, ob);

  Vector<ImagePaintSelectionTarget> targets;
  Vector<Vector<SelectionTileFragment>> fragments;
  MaterialPaintLayer *layer = nullptr;
  if (as_layer) {
    PaintLayerAddParams params;
    params.kind = PaintLayerAddKind::Paint;
    params.anchor = BKE_paint_layers_active_layer_get(*ma);
    params.place = PaintLayerPlace::Above;
    /* Beside a folder, not into it: the paste lands on the stack like the Outliner's paste. */
    params.fill_folder = false;
    if (!name_buf.empty()) {
      params.name = name_buf.c_str();
    }
    layer = BKE_paint_layers_add_with_policy(*ma, params);
    if (layer == nullptr) {
      BKE_report(reports, RPT_ERROR, "Could not add the layer to the stack");
      for (ChannelPaste &paste : pastes) {
        selection_tile_fragments_free(paste.fragments);
      }
      return true;
    }
    BKE_paint_layers_active_set(*ma, layer->marker);
    /* The new row is painted on its content, not on whatever mode the previous one was edited in. */
    if (Paint *paint = BKE_paint_get_active_from_context(C)) {
      BKE_paint_material_layer_target_mode_set(
          *bmain, *scene, *paint, paint_mode, PAINT_LAYER_TARGET_CONTENT);
    }
    else {
      paint_mode.layer_target_mode = int8_t(PAINT_LAYER_TARGET_CONTENT);
    }

    /* One map per pasted channel. The maps are created explicitly transparent (a Normal map keeps
     * the flat tangent in its empty texels): without the fill a channel the row has no record for
     * yet would start from the channel's opaque neutral and hide the layers below even outside
     * the fragment. */
    static const float transparent_fill[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (ChannelPaste &paste : pastes) {
      PaintLayersTarget target;
      Image *dst = nullptr;
      if (BKE_paint_layers_target_get(*ma, paste.channel, PaintLayersTargetMode::Content, target)) {
        dst = BKE_paint_layers_target_ensure_writable(
            *bmain, target, paint_mode.new_channel_image_size, transparent_fill);
      }
      if (dst == nullptr) {
        selection_tile_fragments_free(paste.fragments);
        continue;
      }
      ImagePaintSelectionTarget selection_target;
      selection_target.image = dst;
      BKE_imageuser_default(&selection_target.iuser);
      selection_target.channel = eMaterialPaintChannel(paste.channel);
      selection_target.is_color_channel = selection_target.channel ==
                                          PAINT_MATERIAL_CHANNEL_BASE_COLOR;
      targets.append(selection_target);
      fragments.append(std::move(paste.fragments));
    }
    BKE_texpaint_slots_refresh_object(scene, &ob);
    WM_event_add_notifier(C, NC_MATERIAL | ND_SHADING, ma);
    /* The row and its maps are one global undo step; the floating session deliberately owns none
     * of its own, so this is pushed here rather than as an operator flag. */
    ED_undo_push(C, "Paste Selection as Layer");
  }
  else {
    /* Inside the current layer: pair each group with the canvas target that takes the same data. */
    const Vector<ImagePaintSelectionTarget> candidates = image_paint_selection_view3d_targets_get(
        C, ob);
    for (ChannelPaste &paste : pastes) {
      const ImagePaintSelectionTarget *match = nullptr;
      for (const ImagePaintSelectionTarget &candidate : candidates) {
        const bool same = layered ? int(candidate.channel) == paste.channel :
                                    (candidate.image != nullptr &&
                                     uint32_t(candidate.image->id.session_uid) == paste.image_uid);
        if (same) {
          match = &candidate;
          break;
        }
      }
      if (match == nullptr && !layered && candidates.size() == 1 && pastes.size() == 1) {
        /* A single plain canvas takes a clipboard copied from another image (the Image Editor). */
        match = &candidates.first();
      }
      if (match == nullptr) {
        selection_tile_fragments_free(paste.fragments);
        continue;
      }
      targets.append(*match);
      fragments.append(std::move(paste.fragments));
    }
  }

  if (fragments.is_empty()) {
    BKE_report(reports, RPT_WARNING, "Clipboard images are not on the current canvas");
    return layer != nullptr;
  }

  /* Placing the fragment is the Transform tool's job (it has the move / rotate / scale handles):
   * switch to it before the session opens so the user can drag right away. */
  WM_toolsystem_ref_set_by_id(C, "builtin.image_paint_select_transform");
  if (!image_select_transform3d_begin_paste(C, reports, std::move(targets), std::move(fragments)))
  {
    return layer != nullptr;
  }
  WM_event_add_notifier(C, NC_WINDOW, nullptr);
  if (layer != nullptr) {
    BKE_reportf(reports, RPT_INFO, "Pasted the selection as %s", layer->name);
  }
  return true;
}

}  // namespace blender::ed::sculpt_paint
