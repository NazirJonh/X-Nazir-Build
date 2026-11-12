/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The Material rows of the bake: the rows that show maps rendered from a source material, the
 * questions the scene update asks about them (stale, due, forced, preview) and the landing of a
 * render's pixels. Split out of `paint_layers_bake.cc`; the shared internals are in
 * `paint_layers_bake_intern.hh`.
 */

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_target.hh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <optional>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BKE_anim_data.hh"
#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_key.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_modifier.hh"
#include "BKE_node_runtime.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers_gradient_surface.hh"
#include "BKE_paint_material_composite_surface.hh"
#include "BKE_paint_material_resolve.hh"

#include "BKE_scene.hh"

#include "BLI_array.hh"
#include "BLI_hash.hh"
#include "BLI_function_ref.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_math_geom.h"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_time.h"
#include "BLI_uuid.h"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "paint_layers_bake_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Material rows
 *
 * A Material row shows maps rendered from its source material (`material_bake_layered_rows_ensure`
 * on the editor side). They are the row's source the way a painted map is a painted row's: the
 * row's opacity, blend, mask and corrections stay live over them, which is why such a row is not
 * the cache of a subtree (#paint_layer_row_substituted) and its hash is only structural.
 *
 * Two questions, two readers. Whether the maps are the row's to show is the structural hash, a few
 * integers, answerable from an evaluated copy. Whether they still describe the source is the
 * source's content hash and the pixel subscription, which only the main thread may read; while
 * they do not, the old maps keep showing until the new ones land.
 * \{ */

bool paint_layer_row_material_shown(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr || layer.material == nullptr ||
      layer.bake->mode == MA_PAINT_LAYER_BAKE_NEVER ||
      !ELEM(layer.role,
            MA_PAINT_LAYER_ROLE_LAYER,
            MA_PAINT_LAYER_ROLE_EFFECT,
            MA_PAINT_LAYER_ROLE_MASK_ITEM))
  {
    return false;
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  return (hash[0] != 0 || hash[1] != 0) &&
         (hash[0] == layer.bake->hash[0] && hash[1] == layer.bake->hash[1]);
}

bool paint_layer_row_material_map(const Material &ma,
                                  const MaterialPaintLayer &layer,
                                  const int channel,
                                  Image **r_color)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      !paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) || layer.bake == nullptr ||
      layer.bake->images[channel] == nullptr || paint_layer_channel_filtered(ma, channel) ||
      !paint_layer_row_material_shown(ma, layer))
  {
    return false;
  }
  if (r_color != nullptr) {
    *r_color = layer.bake->images[channel];
  }
  return true;
}

static void bake_source_hash_split(const uint64_t hash, uint32_t r_words[2])
{
  r_words[0] = uint32_t(hash & 0xFFFFFFFFu);
  r_words[1] = uint32_t(hash >> 32);
}

/**
 * Whether the render of \a layer is partial: Auto, no Image Editor reading the maps, and the source
 * supplies everything but the Normal live. Then only the Normal is rendered, only what the Normal
 * reads can make the map stale, and an edit of a colour or a roughness renders nothing. The maps
 * of the channels shown live are left as they are until something reads them.
 */
static bool bake_row_maps_partial(const Material &ma, const MaterialPaintLayer &layer)
{
  if (BKE_paint_layers_bake_mode_get(layer) == MA_PAINT_LAYER_BAKE_ALWAYS ||
      BKE_paint_layers_preview_wanted(ma))
  {
    return false;
  }
  bool normal_needs_map = false;
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (!BKE_paint_layers_material_channel_needs_render(ma, layer, channel) ||
        paint_layer_material_source_group_channel(ma, layer, channel))
    {
      continue;
    }
    if (channel != PAINT_MATERIAL_CHANNEL_NORMAL) {
      return false;
    }
    normal_needs_map = true;
  }
  return normal_needs_map;
}

bool BKE_paint_layers_material_channel_render_wanted(const Material &ma,
                                                     const MaterialPaintLayer &layer,
                                                     const int channel)
{
  if (!BKE_paint_layers_material_channel_needs_render(ma, layer, channel)) {
    return false;
  }
  return !bake_row_maps_partial(ma, layer) ||
         !paint_layer_material_source_group_channel(ma, layer, channel);
}

bool BKE_paint_layers_material_row_stale(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr || layer.material == nullptr) {
    return true;
  }
  const uint64_t full = BKE_paint_layers_source_material_tree_hash_cached(*layer.material);
  uint32_t current[2];
  bake_source_hash_split(full, current);
  if (current[0] == 0 && current[1] == 0) {
    return true;
  }
  /* The stamp always holds the full hash, whatever state the editor is in, so closing an Image
   * Editor does not make a row look edited. A full hash that moved is still no reason to render
   * when the maps are only of the Normal and what the Normal reads is what the last render saw. */
  const bool source_moved = current[0] != layer.bake->source_hash[0] ||
                            current[1] != layer.bake->source_hash[1];
  const bool partial = bake_row_maps_partial(ma, layer);
  const bool normal_only = source_moved && partial;
  const uint64_t normal = normal_only ?
                              BKE_paint_layers_source_normal_hash_cached(*layer.material) :
                              0;
  std::lock_guard lock(bake_runtime_mutex());
  const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime != nullptr && !partial && runtime->partial_rows.contains(UUID(layer.marker))) {
    /* The last render left the maps of the live channels as they were, and something reads them now
     * (an Image Editor opened, the Always mode). */
    return true;
  }
  if (source_moved) {
    const uint64_t *rendered = (normal_only && runtime != nullptr) ?
                                   runtime->normal_hash.lookup_ptr(UUID(layer.marker)) :
                                   nullptr;
    if (rendered == nullptr || *rendered != normal) {
      return true;
    }
  }
  if (runtime != nullptr) {
    for (const BakeLayerSubscription &entry : runtime->layers) {
      if (BLI_uuid_equal(entry.marker, layer.marker) && entry.changed) {
        return true;
      }
    }
  }
  return false;
}

void BKE_paint_layers_material_bake_stamp_get(const Material &ma,
                                              const MaterialPaintLayer &layer,
                                              uint32_t r_hash[2],
                                              uint32_t r_source_hash[2])
{
  BKE_paint_layers_bake_hash(ma, layer, r_hash);
  const uint64_t full = layer.material != nullptr ?
                            BKE_paint_layers_source_material_tree_hash_cached(*layer.material) :
                            0;
  bake_source_hash_split(full, r_source_hash);
  if (full == 0) {
    return;
  }
  /* What the Normal reads at the start of the render, kept against the full hash it goes with:
   * the render that lands from this stamp makes it the one its maps reflect. */
  const uint64_t normal = BKE_paint_layers_source_normal_hash_cached(*layer.material);
  std::lock_guard lock(bake_runtime_mutex());
  BakeMaterialRuntime &runtime = bake_runtime().lookup_or_add(ma.id.session_uid,
                                                              BakeMaterialRuntime{});
  runtime.owner = &ma;
  if (runtime.started.size() > 16) {
    runtime.started.clear();
  }
  runtime.started.add_overwrite(
      full, BakeMaterialRuntime::StartedRender{normal, bake_row_maps_partial(ma, layer)});
}

bool BKE_paint_layers_material_row_render_forced_wanted(const Material &ma,
                                                        const MaterialPaintLayer &row)
{
  if (!paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE) ||
      !ELEM(row.role,
            MA_PAINT_LAYER_ROLE_LAYER,
            MA_PAINT_LAYER_ROLE_EFFECT,
            MA_PAINT_LAYER_ROLE_MASK_ITEM) ||
      row.material == nullptr || row.material->nodetree == nullptr ||
      BKE_paint_layers_bake_mode_get(row) == MA_PAINT_LAYER_BAKE_NEVER ||
      !BKE_paint_layers_material_row_needs_render(ma, row))
  {
    return false;
  }
  if (!paint_layer_row_material_shown(ma, row) || BKE_paint_layers_material_row_stale(ma, row)) {
    return true;
  }
  /* A partial render (Auto, nothing reading the maps) is fresh and still holds the Normal alone. */
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (!BKE_paint_layers_material_channel_needs_render(ma, row, channel)) {
      continue;
    }
    const Image *map = (channel == PAINT_MATERIAL_CHANNEL_ALPHA) ? row.bake->coverage :
                                                                   row.bake->images[channel];
    if (map == nullptr) {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_material_row_due(const Material &ma, const MaterialPaintLayer &layer)
{
  if (!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) ||
      !ELEM(layer.role,
            MA_PAINT_LAYER_ROLE_LAYER,
            MA_PAINT_LAYER_ROLE_EFFECT,
            MA_PAINT_LAYER_ROLE_MASK_ITEM) ||
      layer.material == nullptr || layer.material->nodetree == nullptr ||
      BKE_paint_layers_bake_mode_get(layer) == MA_PAINT_LAYER_BAKE_NEVER)
  {
    return false;
  }
  /* A hidden row shows nothing, so its maps are rendered when it is shown. */
  if (bake_row_is_hidden(ma, layer)) {
    return false;
  }
  /* The active row is the one being edited. In Auto its graph is shown live in the shader, but the
   * Image Editor's composite reads only the rendered maps, so while one shows this material the row
   * is rendered as a preview; with none open there is nothing to keep current and the render waits
   * for the active marker to leave the row. Always has no live form (its maps are what the row
   * shows), so it renders on every edit. */
  const bool editing = BKE_paint_layers_bake_row_is_deferred(ma, layer);
  const bool always = BKE_paint_layers_bake_mode_get(layer) == MA_PAINT_LAYER_BAKE_ALWAYS;
  if (editing && !always && !BKE_paint_layers_preview_wanted(ma) &&
      !paint_layer_material_row_needs_map(ma, layer))
  {
    return false;
  }
  /* Everything the source supplies is a value or a plain texture: there is nothing to render. */
  if (!BKE_paint_layers_material_row_needs_render(ma, layer)) {
    return false;
  }
  /* Maps of preview quality are current for an edited row as long as they are of the side the
   * material asks for now, and due to be replaced once it is left or that side changed. Maps of the
   * row's own size are never replaced by a preview before an edit asks for one. */
  const int preview_side = (layer.bake != nullptr) ? int(layer.bake->preview_side16) * 16 : 0;
  const int wanted_side = (editing && !always) ?
                              BKE_paint_layers_preview_size_get(
                                  ma, BKE_paint_layers_material_row_size(ma, layer)) :
                              0;
  const bool preview_grade_left = preview_side != 0 && preview_side != wanted_side;
  if (paint_layer_row_material_shown(ma, layer) && !BKE_paint_layers_material_row_stale(ma, layer) &&
      !preview_grade_left)
  {
    return false;
  }
  uint32_t hash[2];
  uint32_t source_hash[2];
  BKE_paint_layers_material_bake_stamp_get(ma, layer, hash, source_hash);
  std::lock_guard lock(bake_runtime_mutex());
  const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime != nullptr) {
    if (const BakeFailure *failure = runtime->failed.lookup_ptr(UUID(layer.marker))) {
      if (failure->hash[0] == hash[0] && failure->hash[1] == hash[1] &&
          failure->source[0] == source_hash[0] && failure->source[1] == source_hash[1] &&
          failure->preview == (editing && !always))
      {
        return false;
      }
    }
  }
  return true;
}

bool BKE_paint_layers_material_row_preview_active(const Material &ma,
                                                  const MaterialPaintLayer &layer)
{
  return BKE_paint_layers_bake_row_is_deferred(ma, layer) &&
         BKE_paint_layers_bake_mode_get(layer) != MA_PAINT_LAYER_BAKE_ALWAYS &&
         BKE_paint_layers_preview_wanted(ma);
}

int BKE_paint_layers_material_row_size(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake != nullptr && layer.bake->size > 0) {
    return layer.bake->size;
  }
  return ma.paint_layers_image_size > 0 ? int(ma.paint_layers_image_size) : 4096;
}

bool BKE_paint_layers_material_row_is_preview(const MaterialPaintLayer &layer)
{
  return layer.bake != nullptr && layer.bake->preview_side16 != 0;
}

int BKE_paint_layers_preview_size_get(const Material &ma, const int final_size)
{
  /* The side a preview is asked for, before it is capped. */
  int side = 0;
  switch (ma.paint_layers_preview_size) {
    case MA_PAINT_PREVIEW_SIZE_256:
      side = 256;
      break;
    case MA_PAINT_PREVIEW_SIZE_512:
      side = 512;
      break;
    case MA_PAINT_PREVIEW_SIZE_1024:
      side = 1024;
      break;
    case MA_PAINT_PREVIEW_SIZE_2048:
      side = 2048;
      break;
    case MA_PAINT_PREVIEW_SIZE_4096:
      side = 4096;
      break;
    default:
      side = std::max(final_size / 4, 256);
      break;
  }
  /* Never more than the row will be rendered at, nor than the size the material's layers are
   * created with: a larger preview would cost more than the render it stands in for. */
  if (ma.paint_layers_image_size > 0) {
    side = std::min(side, int(ma.paint_layers_image_size));
  }
  /* A multiple of 16, which is what #MaterialPaintLayerBake::preview_side16 can record. */
  side = std::clamp(side, 16, std::max(final_size, 16));
  return std::max(side - side % 16, 16);
}

float BKE_paint_layers_source_edit_delay(const Material &consumer, const Material &source)
{
  const bool delayed_preview = (consumer.paint_layers_preview_flag & MA_PAINT_PREVIEW_DELAYED) != 0;
  /* Only an edit made through the active row waits: the row being edited is the one a preview is
   * rendered for; any other consumer renders after the editor's short pause. A row with a channel
   * that only a render can show (a Bump's Normal) waits whatever the Sync setting says: dragging
   * a value that moves it would otherwise render at every pause of the hand. */
  bool active_reads_source = false;
  bool preview_wait = false;
  /* The Material corrections count as well: one that is being edited waits like a row does. */
  BKE_paint_layers_foreach_source_row(
      consumer, [&](const MaterialPaintLayer &row) {
        if (row.material != &source || !BKE_paint_layers_bake_row_is_deferred(consumer, row)) {
          return true;
        }
        if (delayed_preview && BKE_paint_layers_material_row_preview_active(consumer, row)) {
          preview_wait = true;
        }
        if (preview_wait || paint_layer_material_row_needs_map(consumer, row)) {
          active_reads_source = true;
        }
        return !active_reads_source;
      });
  if (!active_reads_source) {
    return 0.0f;
  }
  if (!preview_wait) {
    /* Only a map no live form stands in for is waited on: short, since nothing shows its edit until
     * it lands. */
    return MAP_ONLY_EDIT_DELAY_SECONDS;
  }
  switch (consumer.paint_layers_preview_delay) {
    case MA_PAINT_PREVIEW_DELAY_10S:
      return 10.0f;
    case MA_PAINT_PREVIEW_DELAY_15S:
      return 15.0f;
    case MA_PAINT_PREVIEW_DELAY_25S:
      return 25.0f;
    default:
      return 5.0f;
  }
}

bool BKE_paint_layers_preview_wanted(const Material &ma)
{
  return (ma.paint_layers_flag & MA_PAINT_LAYERS_PREVIEW_WANTED) != 0;
}

void BKE_paint_layers_preview_wanted_set(Material &ma, const bool wanted)
{
  if (wanted) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_PREVIEW_WANTED;
  }
  else {
    ma.paint_layers_flag &= ~MA_PAINT_LAYERS_PREVIEW_WANTED;
  }
}

void BKE_paint_layers_material_row_log(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       const char *event)
{
  /* Why a render of a Material row starts: which of the three answers made it due. */
  uint32_t hash[2];
  uint32_t source_hash[2];
  BKE_paint_layers_material_bake_stamp_get(ma, layer, hash, source_hash);
  bool changed = false;
  {
    std::lock_guard lock(bake_runtime_mutex());
    if (const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid)) {
      for (const BakeLayerSubscription &entry : runtime->layers) {
        if (BLI_uuid_equal(entry.marker, layer.marker)) {
          changed = changed || entry.changed;
        }
      }
    }
  }
  const MaterialPaintLayerBake *bake = layer.bake;
  printf(
      "[paint-layers material row] %s: row '%s' shown=%d needs_render=%d struct_hash=%08x%08x "
      "stored_struct=%08x%08x source_hash=%08x%08x stored_source=%08x%08x pixels_changed=%d\n",
      event,
      layer.name,
      int(paint_layer_row_material_shown(ma, layer)),
      int(BKE_paint_layers_material_row_needs_render(ma, layer)),
      hash[1],
      hash[0],
      bake != nullptr ? bake->hash[1] : 0u,
      bake != nullptr ? bake->hash[0] : 0u,
      source_hash[1],
      source_hash[0],
      bake != nullptr ? bake->source_hash[1] : 0u,
      bake != nullptr ? bake->source_hash[0] : 0u,
      int(changed));
  if (layer.material == nullptr || layer.material->nodetree == nullptr) {
    printf("[paint-layers material row]   source has no node tree\n");
    return;
  }
  const std::shared_ptr<const MaterialSourceResolve> resolve = BKE_paint_layers_source_resolve(
      *layer.material);
  printf("[paint-layers material row]   mode=%d group_wrappable=%d\n",
         int(BKE_paint_layers_bake_mode_get(layer)),
         int(BKE_paint_layers_source_group_wrappable(*layer.material)));
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    const PaintLayerMaterialSource source = paint_layer_material_channel_source(
        ma, layer, channel);
    printf(
        "[paint-layers material row]   channel %d: resolved=%d group_ok=%d shown_kind=%d "
        "needs_render=%d\n",
        channel,
        int(resolve->channels[channel]),
        int(paint_layer_material_source_group_channel(ma, layer, channel)),
        int(source.kind),
        int(BKE_paint_layers_material_channel_needs_render(ma, layer, channel)));
  }
}

bool BKE_paint_layers_material_sources_notice(Material &ma)
{
  Vector<std::pair<bUUID, uint64_t>> signatures;
  BKE_paint_layers_foreach_source_row(
      static_cast<const Material &>(ma), [&](const MaterialPaintLayer &row) {
        signatures.append({row.marker, paint_layer_material_live_signature(ma, row)});
        return true;
      });
  if (signatures.is_empty()) {
    return false;
  }
  bool changed = false;
  {
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime &runtime = bake_runtime().lookup_or_add(ma.id.session_uid,
                                                                BakeMaterialRuntime{});
    runtime.owner = &ma;
    for (const auto &[marker, signature] : signatures) {
      uint64_t *previous = runtime.live_signature.lookup_ptr(UUID(marker));
      if (previous == nullptr) {
        /* A row seen for the first time was built with the state it has: nothing moved. */
        runtime.live_signature.add(UUID(marker), signature);
      }
      else if (*previous != signature) {
        *previous = signature;
        changed = true;
      }
    }
  }
  if (changed) {
    /* The shape of a row's group follows its source: a value that became a texture is other nodes.
     */
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
  return changed;
}

void BKE_paint_layers_material_bake_retry(Material &ma, MaterialPaintLayer &layer)
{
  {
    std::lock_guard lock(bake_runtime_mutex());
    if (BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid)) {
      runtime->failed.remove(UUID(layer.marker));
    }
  }
  if (layer.bake != nullptr) {
    layer.bake->source_hash[0] = 0;
    layer.bake->source_hash[1] = 0;
  }
}

void BKE_paint_layers_source_material_consumers(Main &bmain,
                                                const Material &source,
                                                Vector<Material *> &r_consumers)
{
  const int copy_tags = ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN;
  for (Material &ma : bmain.materials) {
    if ((ma.id.tag & copy_tags) != 0 || !BKE_paint_layers_is_layered(ma)) {
      continue;
    }
    bool reads = false;
    BKE_paint_layers_foreach_source_row(static_cast<const Material &>(ma),
                                        [&](const MaterialPaintLayer &row) {
                                          reads = row.material == &source;
                                          return !reads;
                                        });
    if (reads) {
      r_consumers.append(&ma);
    }
  }
}

bool BKE_paint_layers_bake_outstanding(const Material &ma)
{
  if (!BKE_paint_layers_is_layered(ma)) {
    return false;
  }
  if (BKE_paint_layers_bake_heavy_pending(ma)) {
    return true;
  }
  bool due = false;
  BKE_paint_layers_foreach_source_row(ma, [&](const MaterialPaintLayer &row) {
    due = BKE_paint_layers_material_row_due(ma, row);
    return !due;
  });
  return due;
}

bool BKE_paint_layers_is_stale(const Material &ma)
{
  if (!BKE_paint_layers_is_layered(ma)) {
    return false;
  }
  /* The editor's mark covers whatever it has queued or running that has not resolved into a flag
   * or a pending render yet. */
  return BKE_paint_layers_bake_stale_get(ma) || BKE_paint_layers_bake_scheduled_get(ma) ||
         BKE_paint_layers_bake_outstanding(ma);
}

PaintLayersBakeGateAction BKE_paint_layers_bake_gate_decide(const bool stale,
                                                            const bool is_invoke,
                                                            const bool headless)
{
  /* Headless first: nothing there ever makes a stale result fresh by itself. */
  if (headless || !stale) {
    return PaintLayersBakeGateAction::Proceed;
  }
  return is_invoke ? PaintLayersBakeGateAction::Wait : PaintLayersBakeGateAction::Refuse;
}

void BKE_paint_layers_material_bake_apply(Main & /*bmain*/,
                                          Material &ma,
                                          MaterialPaintLayer &layer,
                                          const int size,
                                          const Span<int> channels,
                                          const Span<Image *> images)
{
  if (!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) || size <= 0 ||
      channels.size() != images.size() || !bake_row_owned(ma, layer))
  {
    return;
  }
  MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
  bake->size = size;
  Image *alpha = nullptr;
  Vector<bool> kept(PAINT_MATERIAL_CHANNEL_NUM, false);
  for (const int64_t i : channels.index_range()) {
    if (channels[i] == PAINT_MATERIAL_CHANNEL_ALPHA) {
      /* The source's transparency is the row's coverage, not a channel of its own. */
      alpha = images[i];
      continue;
    }
    if (channels[i] >= 0 && channels[i] < PAINT_MATERIAL_CHANNEL_NUM) {
      BKE_paint_layers_bake_set_map(ma, layer, channels[i], images[i]);
      kept[channels[i]] = true;
    }
  }
  /* Every channel the render did not hand a map for is one the source no longer feeds. */
  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (!kept[channel] && bake->images[channel] != nullptr) {
      BKE_paint_layers_bake_set_map(ma, layer, channel, nullptr);
    }
  }
  /* A coverage map only exists while the source's transparency is rendered: when it is a value or
   * a plain texture, or the source has none, it is shown as it is and the old map goes. */
  BKE_paint_layers_bake_set_map(ma, layer, -1, alpha);
  /* The structural hash is stamped now so the generator wires the new maps in while they render;
   * the source hash only when they land. The subscription starts here too, so a pixel edit of the
   * source made during the render is seen after it. */
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  bake->hash[0] = hash[0];
  bake->hash[1] = hash[1];
  BKE_paint_layers_bake_subscribe(ma, layer);
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
}

bool BKE_paint_layers_material_bake_land(Material &ma,
                                         const bUUID &marker,
                                         const uint32_t hash[2],
                                         const uint32_t source_hash[2],
                                         const bool landed,
                                         const int preview_side)
{
  MaterialPaintLayer *layer = BKE_paint_layers_find(ma, marker);
  if (layer == nullptr || layer->bake == nullptr ||
      !paint_layer_kind_has_flag(*layer, PAINT_LAYER_KIND_READS_SOURCE))
  {
    return false;
  }
  if (!landed) {
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime &runtime = bake_runtime().lookup_or_add(ma.id.session_uid,
                                                                BakeMaterialRuntime{});
    runtime.owner = &ma;
    BakeFailure failure;
    failure.hash[0] = hash[0];
    failure.hash[1] = hash[1];
    failure.source[0] = source_hash[0];
    failure.source[1] = source_hash[1];
    failure.preview = preview_side != 0;
    runtime.failed.add_overwrite(UUID(marker), failure);
    return false;
  }
  uint32_t now[2];
  BKE_paint_layers_bake_hash(ma, *layer, now);
  if (now[0] != hash[0] || now[1] != hash[1]) {
    /* The row was pointed elsewhere or resized meanwhile: these pixels are not its own. */
    return false;
  }
  /* What the pixels reflect is what the render started from. A source that moved on since is
   * stale against it, and the next scene update renders it again. */
  layer->bake->source_hash[0] = source_hash[0];
  layer->bake->source_hash[1] = source_hash[1];
  layer->bake->preview_side16 = int16_t(std::clamp(preview_side / 16, 0, 512));
  {
    std::lock_guard lock(bake_runtime_mutex());
    if (BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid)) {
      runtime->failed.remove(UUID(marker));
      const uint64_t full = (uint64_t(source_hash[1]) << 32) | uint64_t(source_hash[0]);
      if (const BakeMaterialRuntime::StartedRender *started = runtime->started.lookup_ptr(full)) {
        runtime->normal_hash.add_overwrite(UUID(marker), started->normal_hash);
        if (started->partial) {
          runtime->partial_rows.add(UUID(marker));
        }
        else {
          runtime->partial_rows.remove(UUID(marker));
        }
      }
      else {
        runtime->normal_hash.remove(UUID(marker));
        runtime->partial_rows.remove(UUID(marker));
      }
    }
  }
  ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  return true;
}

/** \} */

}  // namespace blender
