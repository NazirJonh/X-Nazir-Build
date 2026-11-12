/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The worker job of a heavy row bake and the row result job.
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

/**
 * A heavy bake that landed while its row was still laid live: the row can be substituted again,
 * but flipping it right there would rebuild the material on top of the stroke that made the bake
 * stale. The flip waits out a short idle instead, so a whole painting session is one rebuild.
 */
struct BakeShapeHold {
  bUUID marker;
  double deadline;
};

/** How long a landed heavy bake waits for the hand to stop before its row turns baked. */
static constexpr double BAKE_SHAPE_HOLD_SECONDS = 3.0;

static Map<uint32_t, Vector<BakeShapeHold>> &bake_shape_holds()
{
  static Map<uint32_t, Vector<BakeShapeHold>> map;
  return map;
}

/* -------------------------------------------------------------------- */
/** \name Worker job
 *
 * A heavy row is rendered off the main thread from a localized copy of the description and written
 * back on the main thread, so the window keeps redrawing while it bakes.
 * \{ */

struct PaintLayersBakeJob {
  Main *bmain = nullptr;
  Material *material = nullptr;
  uint32_t material_session_uid = 0;
  /**
   * The scene and view layer whose objects an Object or Generated row is baked for. Main thread
   * data, re-read at the commit to check the objects are still the ones the worker rendered; null
   * for a material none of whose rows reads object geometry.
   */
  const Scene *scene = nullptr;
  const ViewLayer *view_layer = nullptr;
  /** Localized on the main thread at creation so the worker never reads a material being edited. */
  Material *material_copy = nullptr;

  struct ChannelResult {
    int channel = 0;
    bool is_color = false;
    /**
     * The render stored in its map's own format, not the straight-RGBA render: #color holds a float
     * map's premultiplied texels and #color_bytes a byte map's quantized ones. This is
     * #bake_write_image's transformation, done on the worker right after the render -- at 4K the
     * float4 render is 256 MiB where the byte map's storage is 64, and the job holds every result
     * until the commit.
     */
    bool stored_float = false;
    Vector<float> color;
    Vector<uchar> color_bytes;
  };
  struct ObjectResult {
    /** The object of this tile, or zero for a shared (UV) row's single map. */
    uint32_t object_uid = 0;
    uint32_t mesh_uid = 0;
    uint64_t identity = 0;
    /** UDIM tile number, 0 for a non-tiled map. */
    int tile_number = 0;
    /**
     * The surface of this object, rasterized on the main thread at creation and read-only on the
     * worker; null for a row reading no object geometry.
     */
    std::shared_ptr<const PaintMaterialCompositeSurface> surface;
    Vector<ChannelResult> channels;
    /**
     * The row's common factor (`mask x opacity`), folded from every channel's render as it lands on
     * the worker. The commit writes it as the coverage map, so the per-channel coverage need not be
     * held: at 4K that is 64 MiB a channel the job would otherwise keep until the commit.
     */
    Vector<float> common;
  };
  struct RowResult {
    bUUID marker = {};
    int size = 0;
    /**
     * The bake hash of the row when the job was created. The commit stamps this and not the live
     * one: an edit made while the worker ran would otherwise label a render of the old state as
     * current. With the creation hash the row is simply invalid again and the planner queues it
     * anew.
     */
    uint32_t hash[2] = {};
    /** One entry per object tile; a shared (UV) row has one entry with a null surface. */
    Vector<ObjectResult> objects;
  };
  Vector<RowResult> rows;
  /** The worker stopped early; nothing it produced is written. */
  bool cancelled = false;
};

/**
 * Fold one channel's render into \a common, the channel-independent row factor: `common =
 * mask x opacity`, recovered from the per-channel renders without a new map. The channel rendered
 * `coverage = common x content` with its own `content` in the colour alpha. Where that content is
 * present the quotient is the common factor; the maximum over channels does not depend on channel
 * order, and a texel no channel covers keeps zero.
 *
 * A row baked for its content alone rendered no mask and no opacity, so its quotient is the
 * content's own coverage: what the substitution reads back as the row's mask is then the content,
 * and the row's mask and opacity are laid live over it.
 */
static void bake_common_from_channel(Span<float> color,
                                     Span<float> coverage,
                                     MutableSpan<float> common)
{
  for (const int64_t i : common.index_range()) {
    const float content = color[i * 4 + 3];
    if (content > 0.002f) {
      common[i] = std::max(common[i], clamp_f(coverage[i] / content, 0.0f, 1.0f));
    }
  }
}

PaintLayersBakeJob *BKE_paint_layers_bake_job_create(Main &bmain,
                                                     Material &ma,
                                                     const Scene *scene,
                                                     const ViewLayer *view_layer)
{
  if (!BKE_paint_layers_is_layered(ma)) {
    return nullptr;
  }
  /* The objects the view layer produced are what an Object or Generated row is rendered from, and
   * every row of the material shares them: resolve the list once and notice every edited surface
   * against it. */
  PaintLayersBakeView view;
  if (scene == nullptr || view_layer == nullptr) {
    view.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT;
  }
  else if (bake_any_row_needs_surface(ma)) {
    paint_layers_bake_view_resolve(ma, &bmain, scene, view_layer, view);
  }
  bake_object_identities_notice(ma, view);
  Vector<MaterialPaintLayer *> rows;
  BKE_paint_layers_foreach(ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
    rows.append(&layer);
    for (MaterialPaintLayer &effect : layer.effects) {
      if (paint_layer_kind_reads_anchor_point(effect)) {
        rows.append(&effect);
      }
    }
    for (MaterialPaintLayer &item : layer.mask_stack) {
      if (paint_layer_kind_reads_anchor_point(item)) {
        rows.append(&item);
      }
    }
    return true;
  });

  PaintLayersBakeJob *job = MEM_new<PaintLayersBakeJob>(__func__);
  job->bmain = &bmain;
  job->material = &ma;
  job->material_session_uid = ma.id.session_uid;

  /* Children before their parents: the job renders the queued rows of the copy in this order, and a
   * parent's render composites its children. */
  for (int index = int(rows.size()) - 1; index >= 0; index--) {
    MaterialPaintLayer &layer = *rows[index];
    BakeRowRelease release = BakeRowRelease::Keep;
    if (!bake_row_is_due(ma, layer, view, release)) {
      /* The row can no longer be served by its maps (no UV, too many objects, an animated
       * deformation...): this is the pass that has the view layer to judge it by. */
      bake_row_release_apply(bmain, ma, layer, release);
      continue;
    }
    if (!BKE_paint_layers_bake_is_heavy(ma, layer)) {
      continue;
    }
    const int size = bake_size_for(ma, layer);
    if (size <= 0) {
      continue;
    }

    /* An Object or Generated row is rendered through the surface of each object it serves, and the
     * surfaces are rasterized here: the worker reads the grids and never touches the depsgraph. */
    PaintLayersBakeTargets targets;
    const bool needs_surface = paint_layers_bake_subtree_needs_surface(layer);
    if (needs_surface) {
      paint_layers_bake_targets_resolve(ma, layer, view, size, targets);
      if (targets.issue != PAINT_LAYERS_BAKE_OBJECTS_OK || targets.objects.is_empty()) {
        continue;
      }
    }

    /* The surfaces are built before the bake record: a row whose objects cannot supply them must
     * not be left with an empty, permanently invalid cache. The job needs them all, so they stay
     * here; only the synchronous planner writes and releases one at a time. */
    PaintLayersBakeJob::RowResult row;
    row.marker = layer.marker;
    row.size = size;
    bool surfaces_ok = true;
    if (needs_surface) {
      for (int object_index = 0; object_index < targets.objects.size(); object_index++) {
        const PaintLayersBakeTarget &target = targets.objects[object_index];
        std::shared_ptr<const PaintMaterialCompositeSurface> surface = bake_surface_build(
            layer, target, size);
        if (surface == nullptr) {
          surfaces_ok = false;
          break;
        }
        PaintLayersBakeJob::ObjectResult object;
        object.object_uid = target.object_uid;
        object.mesh_uid = target.mesh_uid;
        object.identity = paint_layers_bake_target_identity(target, size);
        object.tile_number = 1001 + object_index;
        object.surface = std::move(surface);
        row.objects.append(std::move(object));
      }
    }
    else {
      PaintLayersBakeJob::ObjectResult object;
      row.objects.append(std::move(object));
    }
    if (!surfaces_ok) {
      continue;
    }

    /* Every gate has passed and this row is queued right now: only here is it safe to allocate the
     * bake structure. */
    MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(layer);
    bake->size = size;
    if (needs_surface) {
      paint_layers_bake_tiles_clear(*bake);
      bake_tile_object_indices_clear(ma, scene, view_layer, &view);
      /* The shader reads the tile index from each object; write it before the worker renders. */
      bake_tile_object_indices_set(ma, targets);
    }
    BKE_paint_layers_bake_hash(ma, layer, row.hash);
    job->rows.append(std::move(row));
  }

  if (job->rows.is_empty()) {
    MEM_delete(job);
    return nullptr;
  }
  job->scene = scene;
  job->view_layer = view_layer;
  job->material_copy = id_cast<Material *>(BKE_id_copy_ex(
      nullptr, &ma.id, nullptr, LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_NO_ANIMDATA));
  if (job->material_copy == nullptr) {
    MEM_delete(job);
    return nullptr;
  }
  return job;
}

void BKE_paint_layers_bake_job_compute(PaintLayersBakeJob &job,
                                       const FunctionRef<void(float progress)> report_progress,
                                       const FunctionRef<bool()> is_cancelled)
{
  if (job.material_copy == nullptr) {
    return;
  }
  Material &ma = *job.material_copy;
  /* Every object tile is weighted by the same fixed channel count, whether or not each channel
   * actually renders, so progress only ever needs the two totals below. */
  int64_t total_pairs = 0;
  for (const PaintLayersBakeJob::RowResult &row : job.rows) {
    total_pairs += int64_t(row.objects.size()) * BKE_paint_material_channels().size();
  }
  int64_t done_pairs = 0;
  const auto report = [&]() {
    if (report_progress) {
      report_progress(total_pairs > 0 ? float(done_pairs) / float(total_pairs) : 1.0f);
    }
  };
  for (PaintLayersBakeJob::RowResult &row : job.rows) {
    MaterialPaintLayer *layer = BKE_paint_layers_find(ma, row.marker);
    if (layer == nullptr) {
      done_pairs += int64_t(row.objects.size()) * BKE_paint_material_channels().size();
      report();
      continue;
    }
    const bool gradient_row = paint_layer_kind_has_flag(*layer, PAINT_LAYER_KIND_GENERATES);
    const double gradient_start = gradient_row ? BLI_time_now_seconds() : 0.0;
    const size_t row_memory_start = gradient_row ? MEM_get_memory_in_use() : 0;
    Vector<int> gradient_channels;
    for (PaintLayersBakeJob::ObjectResult &object : row.objects) {
      for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
        /* The render of one channel is not interruptible; checking between channels keeps the
         * cancel button responsive to within one channel's render. */
        if (is_cancelled && is_cancelled()) {
          /* A cancelled job writes nothing, and what it holds is hundreds of MiB at 4K: the results
           * and the surfaces they were rendered from go now, not at the free. The commit reads no
           * rows once this mark is set. */
          job.cancelled = true;
          job.rows = Vector<PaintLayersBakeJob::RowResult>();
          return;
        }
        const int channel = int(info.channel);
        Vector<float> color(int64_t(row.size) * row.size * 4);
        Vector<float> coverage(int64_t(row.size) * row.size);
        const bool rendered = BKE_paint_layers_bake_render_node(
            ma, *layer, channel, row.size, color.data(), coverage.data(), nullptr, object.surface);
        done_pairs++;
        report();
        if (!rendered) {
          continue;
        }
        const int64_t pixel_num = int64_t(row.size) * row.size;
        /* The common factor folds in from the straight render, before the pixels take the map's
         * own format: a byte one quantizes the alpha the quotient divides by. The per-channel
         * coverage is not held: the fold-in is the only thing the commit needs it for. */
        if (object.common.is_empty()) {
          object.common = Vector<float>(pixel_num, 0.0f);
        }
        bake_common_from_channel(color, coverage, object.common);
        PaintLayersBakeJob::ChannelResult result;
        result.channel = channel;
        result.is_color = info.is_color;
        result.stored_float = bake_channel_needs_float(*layer, info);
        if (result.stored_float) {
          result.color = Vector<float>(pixel_num * 4);
          for (const int64_t i : IndexRange(pixel_num)) {
            bake_pixel_store_float(&color[i * 4], &result.color[i * 4]);
          }
        }
        else {
          /* The colorspace a byte map's buffer carries, from the same roles the commit's
           * #bake_service_image gives one (see #add_ibuf_for_tile): a colour channel is encoded
           * into the default byte colorspace, a data channel is quantized as it came. Only a
           * colour channel is encoded here too. */
          const ColorSpace *byte_colorspace =
              info.is_color ?
                  IMB_colormanagement_space_get_named(IMB_colormanagement_role_colorspace_name_get(
                      COLOR_ROLE_DEFAULT_BYTE)) :
                  nullptr;
          result.color_bytes = Vector<uchar>(pixel_num * 4);
          for (const int64_t i : IndexRange(pixel_num)) {
            bake_pixel_store_byte(
                &color[i * 4], info.is_color, byte_colorspace, &result.color_bytes[i * 4]);
          }
        }
        object.channels.append(std::move(result));
        if (!gradient_channels.contains(channel)) {
          gradient_channels.append(channel);
        }
      }
    }
    if (gradient_row) {
      const double delta_mib = double(int64_t(MEM_get_memory_in_use()) -
                                      int64_t(row_memory_start)) /
                               (1024.0 * 1024.0);
      int retained_results = 0;
      for (const PaintLayersBakeJob::RowResult &held : job.rows) {
        for (const PaintLayersBakeJob::ObjectResult &held_object : held.objects) {
          retained_results += held_object.channels.size();
        }
      }
      const double retained_mib = double(BKE_paint_layers_bake_job_retained_bytes(job)) /
                                  (1024.0 * 1024.0);
      bake_gradient_row_report(*layer,
                               row.size,
                               false,
                               BLI_time_now_seconds() - gradient_start,
                               gradient_channels,
                               delta_mib,
                               retained_results,
                               retained_mib);
    }
  }
}

/**
 * The scene a job was started for, or null when it (or its view layer) left \a bmain while the
 * worker ran: the pointers the job kept are then not to be touched.
 */
static const Scene *bake_job_scene_get(Main &bmain,
                                       const Scene *scene,
                                       const ViewLayer *view_layer)
{
  if (scene == nullptr || view_layer == nullptr) {
    return nullptr;
  }
  for (Scene &candidate : bmain.scenes) {
    if (&candidate != scene) {
      continue;
    }
    for (ViewLayer &layer : candidate.view_layers) {
      if (&layer == view_layer) {
        return &candidate;
      }
    }
    break;
  }
  return nullptr;
}

bool BKE_paint_layers_bake_job_commit(PaintLayersBakeJob &job)
{
  if (job.bmain == nullptr) {
    return false;
  }
  Main &bmain = *job.bmain;
  Material *ma = nullptr;
  for (Material &candidate : bmain.materials) {
    if (candidate.id.session_uid == job.material_session_uid && &candidate == job.material) {
      ma = &candidate;
      break;
    }
  }
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    return false;
  }
  if (job.cancelled) {
    /* Without this mark the rows left stale would start the same job again on the next update. An
     * edit clears it (see #BKE_paint_layers_tag_edited). */
    ma->paint_layers_flag |= MA_PAINT_LAYERS_BAKE_CANCELLED;
    return false;
  }
  bool changed = false;
  const Scene *scene = bake_job_scene_get(bmain, job.scene, job.view_layer);
  PaintLayersBakeView view;
  if (scene == nullptr || job.view_layer == nullptr) {
    view.issue = PAINT_LAYERS_BAKE_OBJECTS_NO_CONTEXT;
  }
  else if (bake_any_row_needs_surface(*ma)) {
    paint_layers_bake_view_resolve(*ma, &bmain, scene, job.view_layer, view);
  }
  /* A regeneration this commit does not need must not be introduced by the subscriptions below;
   * one an earlier edit asked for is kept. */
  const bool had_regen = (ma->paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0;
  bool need_regen = false;
  for (PaintLayersBakeJob::RowResult &row : job.rows) {
    MaterialPaintLayer *layer = BKE_paint_layers_find(*ma, row.marker);
    /* The row was removed, its bake dropped or switched off while the worker ran: nothing to
     * write. */
    if (layer == nullptr || layer->bake == nullptr || layer->bake->mode == MA_PAINT_LAYER_BAKE_NEVER)
    {
      continue;
    }
    uint32_t current_hash[2];
    BKE_paint_layers_bake_hash(*ma, *layer, current_hash);
    if (current_hash[0] != row.hash[0] || current_hash[1] != row.hash[1]) {
      /* The row changed while the worker ran: the pixels describe the old description, which the
       * stored hash would label as current. Dropping them leaves the row due for a new render. */
      continue;
    }
    const bool tiled_row = !row.objects.is_empty() && row.objects[0].surface != nullptr;
    PaintLayersBakeTargets targets;
    if (tiled_row) {
      /* The objects or their geometry may have moved while the worker ran: the pixels then describe
       * surfaces the view layer no longer has, so they are dropped and the row re-bakes. */
      paint_layers_bake_targets_resolve(*ma, *layer, view, row.size, targets);
      if (targets.issue != PAINT_LAYERS_BAKE_OBJECTS_OK ||
          targets.objects.size() != row.objects.size())
      {
        continue;
      }
      bool matches = true;
      for (int index = 0; index < row.objects.size(); index++) {
        const PaintLayersBakeJob::ObjectResult &object = row.objects[index];
        const PaintLayersBakeTarget &target = targets.objects[index];
        if (target.object_uid != object.object_uid || target.mesh_uid != object.mesh_uid ||
            paint_layers_bake_target_identity(target, row.size) != object.identity)
        {
          matches = false;
          break;
        }
      }
      if (!matches) {
        continue;
      }
      paint_layers_bake_tiles_clear(*layer->bake);
      bake_tile_object_indices_clear(*ma, scene, job.view_layer, &view);
      bake_tile_object_indices_set(*ma, targets);
    }
    const int object_num = tiled_row ? int(row.objects.size()) : 0;
    /* Whether the row was laid live when the bake landed. That is the only case where the newly
     * valid bake changes the generated shape; a partial re-bake of a row that already read its
     * bake does not. */
    const bool was_substituted = paint_layer_row_substituted(*ma, *layer);
    bool row_written = false;
    bool all_objects = true;
    Vector<int> written_channels;
    for (int object_index = 0; object_index < row.objects.size(); object_index++) {
      PaintLayersBakeJob::ObjectResult &object = row.objects[object_index];
      bool object_written = false;
      for (PaintLayersBakeJob::ChannelResult &channel : object.channels) {
        const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
            eMaterialPaintChannel(channel.channel));
        Image *color_image = bake_service_image(bmain,
                                               *layer,
                                               channel.channel,
                                               row.size,
                                               channel.is_color,
                                               bake_channel_needs_float(*layer, info),
                                               object_num);
        if (color_image == nullptr) {
          continue;
        }
        /* The stored buffer is the map's own format, so this write converts nothing: it puts down
         * the render the worker held, not its float4. The buffer is freed here and not at the job's
         * teardown, so the held memory falls as the maps are written. */
        bake_write_stored_image(
            *color_image,
            channel.stored_float,
            channel.stored_float ? static_cast<const void *>(channel.color.data()) :
                                   static_cast<const void *>(channel.color_bytes.data()),
            nullptr,
            object.tile_number);
        channel.color = Vector<float>();
        channel.color_bytes = Vector<uchar>();
        if (!written_channels.contains(channel.channel)) {
          written_channels.append(channel.channel);
        }
        object_written = true;
      }
      if (!object_written) {
        all_objects = false;
        continue;
      }
      row_written = true;
      {
        /* The common factor was folded on the worker as each channel rendered; the per-channel
         * coverages it came from are gone, which is the point. */
        if (Image *coverage_image = bake_service_image(
                bmain, *layer, -1, row.size, false, false, object_num))
        {
          bake_write_coverage_image(
              *coverage_image, object.common.data(), nullptr, object.tile_number);
        }
        object.common = Vector<float>();
      }
      if (tiled_row) {
        MaterialPaintLayerBakeTile *tile = bake_tile_ensure(
            *layer->bake, object.object_uid, object_index);
        tile->hash[0] = uint32_t(object.identity & 0xFFFFFFFFu);
        tile->hash[1] = uint32_t(object.identity >> 32);
      }
    }
    if (!row_written || !all_objects) {
      continue;
    }
    if (tiled_row) {
      layer->bake->object_count = int(row.objects.size());
    }
    bake_stale_channel_maps_release(bmain, *layer, written_channels);
    layer->bake->hash[0] = row.hash[0];
    layer->bake->hash[1] = row.hash[1];
    if (tiled_row) {
      /* The maps are the surfaces': the next plan compares those identities to notice an edit of
       * the geometry or of the UV layer. */
      bake_object_identity_record(*ma, *layer, row.objects[0].identity);
    }
    BKE_paint_layers_bake_subscribe(*ma, *layer);
    const bool now_substituted = paint_layer_row_substituted(*ma, *layer);
    if (!was_substituted && now_substituted) {
      /* The row was live while it was painted and can be baked again: turn it baked after the
       * hand stops instead of on top of the stroke. */
      BKE_paint_layers_bake_hold_note(*ma, layer->marker);
    }
    else if (!now_substituted) {
      /* The bake is still not usable: the row stays live, which needs no new shape. */
      need_regen = true;
    }
    changed = true;
  }
  if (changed) {
    /* A row that was live stays live and a partially re-baked one keeps its baked shape: neither
     * needs a rebuild. #BKE_paint_layers_bake_subscribe marks one unconditionally, so the flag is
     * restored to what the edit actually needs. */
    if (!need_regen && !had_regen) {
      ma->paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;
    }
    else {
      ma->paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    }
    /* New bake maps were written into the layers outside any operator. Tagging the material marks
     * it changed for the next undo push; otherwise a memfile undo reuses it as it is and frees the
     * maps, leaving the layers' bake slots dangling. */
    DEG_id_tag_update(&ma->id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  }
  /* The queue is drained once no baked row is invalid any more, as the synchronous planner decides
   * it. */
  bool pending = false;
  BKE_paint_layers_foreach(
      *ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        pending = layer.bake != nullptr && layer.bake->mode != MA_PAINT_LAYER_BAKE_NEVER &&
                  bake_row_is_candidate(*ma, layer, bake_row_object_issue(*ma, layer, view)) &&
                  !BKE_paint_layers_bake_is_valid(*ma, layer) && !bake_row_is_hidden(*ma, layer);
        return !pending;
      });
  if (!pending) {
    ma->paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_STALE;
  }
  return changed;
}

int64_t BKE_paint_layers_bake_job_retained_bytes(const PaintLayersBakeJob &job)
{
  int64_t bytes = 0;
  for (const PaintLayersBakeJob::RowResult &row : job.rows) {
    for (const PaintLayersBakeJob::ObjectResult &object : row.objects) {
      for (const PaintLayersBakeJob::ChannelResult &channel : object.channels) {
        bytes += channel.color.size() * int64_t(sizeof(float)) +
                 channel.color_bytes.size() * int64_t(sizeof(uchar));
      }
      bytes += object.common.size() * int64_t(sizeof(float));
    }
  }
  return bytes;
}

void BKE_paint_layers_bake_hold_note(Material &ma, const bUUID &marker)
{
  const double deadline = BLI_time_now_seconds() + BAKE_SHAPE_HOLD_SECONDS;
  Vector<BakeShapeHold> &holds = bake_shape_holds().lookup_or_add_default(ma.id.session_uid);
  for (BakeShapeHold &hold : holds) {
    if (BLI_uuid_equal(hold.marker, marker)) {
      hold.deadline = deadline;
      return;
    }
  }
  holds.append({marker, deadline});
}

double BKE_paint_layers_bake_hold_next_deadline()
{
  double next = -1.0;
  for (const auto item : bake_shape_holds().items()) {
    for (const BakeShapeHold &hold : item.value) {
      if (next < 0.0 || hold.deadline < next) {
        next = hold.deadline;
      }
    }
  }
  return next;
}

bool BKE_paint_layers_bake_hold_due(Main &bmain, const double now)
{
  bool tagged = false;
  Vector<uint32_t> empty;
  for (const auto item : bake_shape_holds().items()) {
    Material *ma = nullptr;
    for (Material &candidate : bmain.materials) {
      if (candidate.id.session_uid == item.key) {
        ma = &candidate;
        break;
      }
    }
    if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
      empty.append(item.key);
      continue;
    }
    bool tag_material = false;
    for (int64_t i = item.value.size() - 1; i >= 0; i--) {
      const BakeShapeHold &hold = item.value[i];
      if (hold.deadline > now) {
        continue;
      }
      const MaterialPaintLayer *layer = BKE_paint_layers_find(*ma, hold.marker);
      /* A row that lost its bake or whose bake is no longer valid stays live; there is no baked
       * shape to turn back to. */
      if (layer != nullptr && paint_layer_row_substituted(*ma, *layer)) {
        tag_material = true;
      }
      item.value.remove(i);
    }
    if (item.value.is_empty()) {
      empty.append(item.key);
    }
    if (tag_material) {
      BKE_paint_layers_tag_edited(*ma);
      tagged = true;
    }
  }
  for (const uint32_t uid : empty) {
    bake_shape_holds().remove(uid);
  }
  return tagged;
}

void BKE_paint_layers_bake_hold_clear()
{
  bake_shape_holds().clear();
}

void BKE_paint_layers_bake_job_free(PaintLayersBakeJob &job)
{
  if (job.material_copy != nullptr) {
    BKE_id_free(nullptr, &job.material_copy->id);
    job.material_copy = nullptr;
  }
  MEM_delete(&job);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Row Result Job
 * \{ */

struct PaintLayersRowResultJob {
  Main *bmain = nullptr;
  Material *material = nullptr;
  uint32_t material_session_uid = 0;
  /** Localized on the main thread at creation so the worker never reads a material being edited.
   */
  Material *material_copy = nullptr;
  bUUID source_marker = {};
  bUUID target_marker = {};
  int size = 0;
  struct ChannelResult {
    int channel = 0;
    bool is_color = false;
    Vector<float> color;
  };
  Vector<ChannelResult> channels;
};

PaintLayersRowResultJob *BKE_paint_layers_row_result_job_create(Main &bmain,
                                                                Material &ma,
                                                                const bUUID &source_marker,
                                                                const bUUID &target_marker,
                                                                const int size)
{
  if (!BKE_paint_layers_is_layered(ma) || size <= 0 ||
      BKE_paint_layers_find(ma, source_marker) == nullptr ||
      BKE_paint_layers_find(ma, target_marker) == nullptr)
  {
    return nullptr;
  }
  PaintLayersRowResultJob *job = MEM_new<PaintLayersRowResultJob>(__func__);
  job->bmain = &bmain;
  job->material = &ma;
  job->material_session_uid = ma.id.session_uid;
  job->source_marker = source_marker;
  job->target_marker = target_marker;
  job->size = size;
  job->material_copy = id_cast<Material *>(
      BKE_id_copy_ex(nullptr,
                     &ma.id,
                     nullptr,
                     LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_NO_ANIMDATA));
  if (job->material_copy == nullptr) {
    MEM_delete(job);
    return nullptr;
  }
  return job;
}

void BKE_paint_layers_row_result_job_compute(PaintLayersRowResultJob &job)
{
  if (job.material_copy == nullptr) {
    return;
  }
  Material &ma = *job.material_copy;
  const MaterialPaintLayer *source = BKE_paint_layers_find(ma, job.source_marker);
  if (source == nullptr) {
    return;
  }
  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    const int channel = int(info.channel);
    Vector<float> color(int64_t(job.size) * job.size * 4);
    Vector<float> coverage(int64_t(job.size) * job.size);
    if (!BKE_paint_layers_bake_render_node(
            ma, *source, channel, job.size, color.data(), coverage.data()))
    {
      continue;
    }
    PaintLayersRowResultJob::ChannelResult result;
    result.channel = channel;
    result.is_color = info.is_color;
    result.color = std::move(color);
    job.channels.append(std::move(result));
  }
}

bool BKE_paint_layers_row_result_job_commit(PaintLayersRowResultJob &job)
{
  if (job.bmain == nullptr || job.channels.is_empty()) {
    return false;
  }
  Main &bmain = *job.bmain;
  Material *ma = nullptr;
  for (Material &candidate : bmain.materials) {
    if (candidate.id.session_uid == job.material_session_uid && &candidate == job.material) {
      ma = &candidate;
      break;
    }
  }
  if (ma == nullptr || !BKE_paint_layers_is_layered(*ma)) {
    return false;
  }
  MaterialPaintLayer *target = BKE_paint_layers_find(*ma, job.target_marker);
  if (target == nullptr) {
    return false;
  }
  bool any = false;
  for (PaintLayersRowResultJob::ChannelResult &result : job.channels) {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(result.channel));
    char name[192];
    SNPRINTF(name, "%s %s", target->name[0] != '\0' ? target->name : "Layer", info.ui_name);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Image *image = BKE_image_add_generated(&bmain,
                                           job.size,
                                           job.size,
                                           name,
                                           32,
                                           false,
                                           IMA_GENTYPE_BLANK,
                                           black,
                                           false,
                                           /*is_data=*/!info.is_color,
                                           false);
    if (image == nullptr) {
      continue;
    }
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    /* A row result is a bake the stack owns, so the browsers hide it like every other map. */
    BKE_paint_layers_image_mark_internal(*image);
    /* The render is the CPU one the bake planner uses, so the colour's alpha already is the source
     * row's content alpha. Written through unchanged: the target map then means the same thing any
     * other paint-layer map does. */
    bake_write_image(*image, result.color.data(), info.is_color, nullptr);
    if (BKE_paint_layers_channel_add(*ma, target, eMaterialPaintChannel(result.channel)) ==
            nullptr ||
        !BKE_paint_layers_channel_set_image(
            *ma, target, eMaterialPaintChannel(result.channel), image))
    {
      BKE_id_free(&bmain, &image->id);
      continue;
    }
    any = true;
  }
  if (any) {
    /* Remembered so the target can be refreshed from the same row without choosing it again. */
    target->row_result_source = job.source_marker;
  }
  return any;
}

void BKE_paint_layers_row_result_job_free(PaintLayersRowResultJob &job)
{
  if (job.material_copy != nullptr) {
    BKE_id_free(nullptr, &job.material_copy->id);
    job.material_copy = nullptr;
  }
  MEM_delete(&job);
}

/** \} */


}  // namespace blender
