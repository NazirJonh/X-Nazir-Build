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
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_layer_edit.hh"
#include "BKE_paint_material_resolve.hh"
#include "BKE_report.hh"

#include "BLI_map.hh"
#include "BLI_span.hh"
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

/** The map \a channel of the row \a ordinal shows now, or null when it has none or is off. */
static Image *paint_layer_channel_target(Main &bmain,
                                         Material &owner,
                                         const int ordinal,
                                         const int channel)
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

/** The correction \a marker in either section of the stack entry \a entry, or null. */
static const PaintMaterialLayerCorrectionEntry *paint_layer_correction_find(
    const PaintMaterialLayerStackEntry &entry, const bUUID &marker)
{
  for (const PaintMaterialLayerCorrectionEntry &correction : entry.content_corrections) {
    if (correction.marker == marker) {
      return &correction;
    }
  }
  for (const PaintMaterialLayerCorrectionEntry &correction : entry.mask_corrections) {
    if (correction.marker == marker) {
      return &correction;
    }
  }
  return nullptr;
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

int add_from_material(bContext &C,
                      Material &owner,
                      const int anchor_ordinal,
                      Material &source,
                      const PaintMaterialLayerMovePlace place)
{
  using namespace ed::material_bake;
  wmWindowManager *wm = CTX_wm_manager(&C);
  Material *picked = &source;
  Main *bmain = CTX_data_main(&C);

  int image_size = 1024;
  const Scene *scene = CTX_data_scene(&C);
  if (scene != nullptr && scene->toolsettings != nullptr) {
    /* The same size the first brush stroke would have created this material's maps at. */
    image_size = scene->toolsettings->paint_mode.new_channel_image_size;
  }

  /* The channels the layer can carry: every material channel the picked material actually
   * feeds. Constant channels bake as flat fills without a render; only Unavailable ones are
   * skipped. What the owner's stack has wired does not limit this: missing chains are migrated
   * up front, and an empty owner takes a dedicated path. */
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(picked);
  Vector<int> required;
  for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
    if (resolve.channels[channel] != ChannelResolution::Unavailable) {
      required.append(channel);
    }
  }
  if (required.is_empty()) {
    BKE_reportf(CTX_wm_reports(&C),
                RPT_WARNING,
                RPT_("Material \"%s\" feeds none of the paint channels"),
                picked->id.name + 2);
    return -1;
  }

  /* The layer is re-configured by editing its source, which a linked material -- an asset,
   * typically -- does not allow, and the bake link must not point into a library either.
   * (A group standing for a material only references it, so a linked one is fine there.) */
  if (ID_IS_LINKED(&picked->id)) {
    BKE_lib_id_make_local(bmain, &picked->id, 0);
    if (picked->id.newid != nullptr) {
      /* Still used from inside its library, so a local copy was made instead. */
      Material *local = id_cast<Material *>(picked->id.newid);
      picked->id.newid = nullptr;
      picked = local;
    }
    if (ID_IS_LINKED(&picked->id)) {
      BKE_reportf(CTX_wm_reports(&C),
                  RPT_ERROR,
                  RPT_("Material \"%s\" could not be made local"),
                  picked->id.name + 2);
      return -1;
    }
    BKE_reportf(
        CTX_wm_reports(&C), RPT_INFO, RPT_("Material \"%s\" was made local"), picked->id.name + 2);
    WM_event_add_notifier(&C, NC_ID | NA_EDITED, nullptr);
  }

  const uint64_t revision_before = BKE_material_paint_layer_revision_get(owner);

  /* Route E (empty canvas) vs S (existing stack). An ensure refusal for a channel wired to a
   * foreign graph stops the gesture before a single bake image exists. */
  bool use_base = false;
  {
    Vector<PaintMaterialLayerStackEntry> probe;
    const bool probe_ok = BKE_paint_material_layer_stack_from_material(*bmain, owner, probe);
    if (!probe_ok) {
      use_base = true;
    }
    else {
      PaintMaterialLayerEditError ensure_error = PaintMaterialLayerEditError::None;
      const bool ensure_ok = BKE_paint_material_layer_channels_ensure(
          *bmain, owner, required.as_span(), &ensure_error);
      if (!ensure_ok) {
        if (ensure_error == PaintMaterialLayerEditError::NotAStack) {
          use_base = true;
        }
        else {
          BKE_report(CTX_wm_reports(&C),
                     RPT_ERROR,
                     RPT_(BKE_paint_material_layer_edit_error_message(ensure_error)));
          return -1;
        }
      }
    }
  }

  Vector<BakeTargetSpec> targets;
  for (const int channel : required) {
    targets.append({eMaterialPaintChannel(channel)});
  }

  int new_ordinal = -1;
  bool edit_attempted = false;
  bool edit_ok = false;
  /* The add runs between target creation and the render: the job's worker updates node trees of
   * its own, and a refused add frees maps a running job would then look for. */
  auto add_layer = [&](const MaterialBakeToImagesResult &bake) {
    edit_attempted = true;
    /* The baked maps go in as the layer's own maps: no placeholder is created for those channels
     * only to be replaced, and each map's one fresh user becomes the user of the node showing it.
     * The add takes all of them over, so a refused add or a channel the layer does not get leaves
     * nothing behind here to clean up. */
    Vector<PaintMaterialLayerChannelImage> baked_maps;
    for (const int i : bake.created.index_range()) {
      baked_maps.append({int(bake.created_channels[i]), bake.created[i]});
    }

    PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
    bool step_ok = false;
    if (use_base) {
      /* Path E: the empty canvas becomes one normalized row holding the baked maps. */
      step_ok = BKE_paint_material_layer_add_material_base(*bmain,
                                                              owner,
                                                              baked_maps.as_span(),
                                                              PaintMaterialLayerKind::Material,
                                                              &new_ordinal,
                                                              &error);
    }
    else {
      PaintMaterialLayerAddParams params;
      params.kind = PaintMaterialLayerKind::Material;
      /* Below the anchor is the anchor's own position in the add's absolute numbering (the add
       * fills a place, a move names a neighbor); above it keeps the neighbor wording, which also
       * carries an insert into a folder the anchor names. */
      if (place == PaintMaterialLayerMovePlace::Below && anchor_ordinal >= 0) {
        params.ordinal = anchor_ordinal;
      }
      else {
        params.anchor_ordinal = anchor_ordinal;
      }
      params.image_size = image_size;
      params.channel_images = baked_maps;
      step_ok = BKE_paint_material_layer_add(*bmain, owner, params, &new_ordinal, &error);
    }
    if (!step_ok) {
      BKE_report(
          CTX_wm_reports(&C), RPT_ERROR, RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return false;
    }
    edit_ok = true;
    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &owner.id);
    return true;
  };

  MaterialBakeToImagesParams bake_params;
  bake_params.material = picked;
  bake_params.targets = targets;
  bake_params.size = image_size;
  bake_params.blocking = false;
  bake_params.before_render = add_layer;
  material_bake_to_images(*bmain, wm, CTX_wm_window(&C), bake_params);
  if (!edit_attempted) {
    /* An ensure that already widened the stack stays behind as its own visible change; say so
     * plainly instead of leaving a silent preparation. */
    if (BKE_material_paint_layer_revision_get(owner) != revision_before) {
      BKE_reportf(CTX_wm_reports(&C),
                  RPT_ERROR,
                  RPT_("Bake of \"%s\" failed after preparing the layer channels; "
                       "no Material layer was added"),
                  picked->id.name + 2);
    }
    return -1;
  }
  return edit_ok ? new_ordinal : -1;
}

bool channel_toggle(bContext &C, ReportList &reports, const int channel)
{
  using namespace ed::material_bake;
  Main *bmain = CTX_data_main(&C);
  Scene *scene = CTX_data_scene(&C);
  const std::optional<PaintMaterialActiveLayer> layer = BKE_paint_material_active_layer_get(
      *bmain, scene->toolsettings->paint_mode);
  if (!layer.has_value()) {
    BKE_report(&reports, RPT_ERROR, "No active paint layer");
    return false;
  }
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  MaterialPaintChannelImageBinding &binding =
      scene->toolsettings->paint_mode.channel_image_bindings[channel];

  /* An active correction row toggles the correction's channels, not the ones of the layer it hangs
   * on: the Layer Material tab is showing the correction's own channel set then. */
  if (!BLI_uuid_is_nil(layer->correction)) {
    const PaintMaterialLayerChannelState state =
        BKE_paint_material_layer_correction_channel_state_get(
            *bmain, *layer->owner, layer->correction, channel);
    const bool enable = state != PaintMaterialLayerChannelState::Enabled;
    if (!BKE_paint_material_layer_correction_channel_enabled_set(
            *bmain, *layer->owner, layer->correction, channel, enable, &error))
    {
      BKE_report(&reports, RPT_ERROR, BKE_paint_material_layer_edit_error_message(error));
      return false;
    }
    /* The binding follows the toggle, read fresh like the layer path does: the switch just
     * rewired the correction's nodes, and \a layer's snapshot predates it. */
    Vector<PaintMaterialLayerStackEntry> entries;
    BKE_paint_material_layer_stack_from_material(*bmain, *layer->owner, entries);
    Image *map = nullptr;
    bool map_on = false;
    for (const PaintMaterialLayerStackEntry &entry : entries) {
      if (entry.ordinal != layer->ordinal) {
        continue;
      }
      const PaintMaterialLayerCorrectionEntry *correction = paint_layer_correction_find(
          entry, layer->correction);
      if (correction != nullptr) {
        map = correction->channel_images.lookup_default(channel, nullptr);
        map_on = (map != nullptr) &&
                 (correction->disabled_channels_mask & (uint32_t(1) << channel)) == 0;
      }
      break;
    }
    /* A switched-off channel is not a paint target (spec 6). */
    BKE_paint_material_channel_binding_set(binding, map_on ? map : nullptr);

    WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &layer->owner->id);
    WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
    return true;
  }

  /* The shown state: #BKE_paint_material_layer_channel_state_get and
   * #BKE_paint_material_layer_channel_enabled_set agree on it. */
  const PaintMaterialLayerChannelState state = BKE_paint_material_layer_channel_state_get(
      *bmain, *layer->owner, layer->ordinal, channel);
  const bool enable = state != PaintMaterialLayerChannelState::Enabled;

  if (enable && state == PaintMaterialLayerChannelState::Absent && layer->is_material()) {
    /* A Material layer's new channel is a bake of its source; the map is handed over before the
     * render starts, like every Material layer map (see #before_render). */
    const int size = layer->bake_size > 0 ? layer->bake_size :
                                            scene->toolsettings->paint_mode.new_channel_image_size;
    const BakeTargetSpec target = {eMaterialPaintChannel(channel)};
    MaterialBakeToImagesParams params;
    params.material = layer->source;
    params.targets = Span(&target, 1);
    params.size = size;
    bool handed_over = false;
    auto hand_over = [&](const MaterialBakeToImagesResult &result) {
      if (result.created.is_empty()) {
        return false;
      }
      handed_over = BKE_paint_material_layer_channel_enabled_set(
          *bmain, *layer->owner, layer->ordinal, channel, true, result.created.first(), &error);
      return handed_over;
    };
    params.before_render = hand_over;
    const MaterialBakeToImagesResult result = material_bake_to_images(
        *bmain, CTX_wm_manager(&C), CTX_wm_window(&C), params);
    if (!handed_over) {
      BKE_report(&reports,
                 RPT_ERROR,
                 !result.skipped_unavailable.is_empty() ?
                     RPT_("The layer's material does not feed this channel") :
                     RPT_(BKE_paint_material_layer_edit_error_message(error)));
      return false;
    }
  }
  else if (!BKE_paint_material_layer_channel_enabled_set(
               *bmain, *layer->owner, layer->ordinal, channel, enable, nullptr, &error))
  {
    BKE_report(&reports, RPT_ERROR, BKE_paint_material_layer_edit_error_message(error));
    return false;
  }

  /* A Disabled bake that fell behind -- source edited or the resolution moved -- is rendered
   * again now that it shows. Read fresh rather than through \a layer's snapshot: enabling the
   * channel just swapped the node's map, and the old one -- \a layer's own copy of the pointer --
   * may already be freed. */
  if (enable && state == PaintMaterialLayerChannelState::Disabled && layer->is_material()) {
    if (Image *map = paint_layer_channel_target(*bmain, *layer->owner, layer->ordinal, channel)) {
      const bool resized = layer->bake_size > 0 && paint_layer_map_size(*map) != layer->bake_size;
      if (resized) {
        BKE_image_scale(map, layer->bake_size, layer->bake_size, nullptr);
        ImageMaterialSource link;
        BKE_image_material_source_get(*map, link);
        link.bake_size = layer->bake_size;
        BKE_image_material_source_set(*map, link);
      }
      if (resized || material_bake_source_is_stale(*map)) {
        material_bake_images_rebake(
            *bmain, *layer->source, Span<Image *>(&map, 1), layer->bake_size);
      }
    }
  }
  /* A switched-off channel is not a paint target (spec 6). */
  BKE_paint_material_channel_binding_set(
      binding, paint_layer_channel_target(*bmain, *layer->owner, layer->ordinal, channel));

  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &layer->owner->id);
  WM_event_add_notifier(&C, NC_SCENE | ND_TOOLSETTINGS, nullptr);
  return true;
}

bool rebake(bContext &C, ReportList &reports)
{
  Main *bmain = CTX_data_main(&C);
  Scene *scene = CTX_data_scene(&C);
  const std::optional<PaintMaterialActiveLayer> layer = BKE_paint_material_active_layer_get(
      *bmain, scene->toolsettings->paint_mode);
  if (!layer.has_value()) {
    BKE_report(&reports, RPT_ERROR, "No active Material paint layer");
    return false;
  }
  Vector<Image *> baked_maps;
  for (Image *image : layer->maps.values()) {
    ImageMaterialSource link;
    if (BKE_image_material_source_get(*image, link)) {
      baked_maps.append(image);
    }
  }
  if (baked_maps.is_empty()) {
    return false;
  }
  /* Rendered whether or not the maps look current: this is for when they do not look right
   * although nothing the staleness check sees has changed -- an image the material samples was
   * edited outside of it, say, or a bake was cancelled. */
  ed::material_bake::material_bake_images_rebake(*bmain, *layer->source, baked_maps, 0);
  return true;
}

bool resize(bContext &C, ReportList &reports, const int size)
{
  Main *bmain = CTX_data_main(&C);
  Scene *scene = CTX_data_scene(&C);
  const std::optional<PaintMaterialActiveLayer> layer = BKE_paint_material_active_layer_get(
      *bmain, scene->toolsettings->paint_mode);
  if (!layer.has_value()) {
    BKE_report(&reports, RPT_ERROR, "No active Material paint layer");
    return false;
  }

  Vector<Image *> baked_maps;
  for (Image *image : layer->maps.values()) {
    ImageMaterialSource link;
    if (!BKE_image_material_source_get(*image, link)) {
      continue;
    }
    /* Resized now, so the layer shows the new resolution while the bake is on its way. */
    BKE_image_scale(image, size, size, nullptr);
    link.bake_size = size;
    BKE_image_material_source_set(*image, link);
    BKE_image_partial_update_mark_full_update(image);
    WM_event_add_notifier(&C, NC_IMAGE | NA_EDITED, image);
    baked_maps.append(image);
  }
  if (baked_maps.is_empty()) {
    return false;
  }
  /* The corrections of the layer scale with it (spec 18 D8): left at the old resolution, their
   * maps would disagree with the layer's own the moment it moved. Only once the resize is known
   * to go through -- a refused one ends the operator cancelled, with no undo step for a scale. */
  BKE_paint_material_layer_corrections_scale(*bmain, *layer->owner, layer->ordinal, size, size);
  ed::material_bake::material_bake_images_rebake(*bmain, *layer->source, baked_maps, size);

  WM_event_add_notifier(&C, NC_MATERIAL | ND_SHADING, &layer->owner->id);
  return true;
}

}  // namespace blender::ed::sculpt_paint::material_layer
