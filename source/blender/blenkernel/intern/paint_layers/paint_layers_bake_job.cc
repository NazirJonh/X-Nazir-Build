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
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_layers_target.hh"
#include "BKE_paint_perf_debug.hh"

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
#include "BKE_paint_material_composite.hh"
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
#include "BLI_rect.h"
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

/** Releases a snapshot's mesh; the job is destroyed on the main thread, which that needs. */
struct BakeSnapshotDeleter {
  void operator()(PaintMaterialProjectionSnapshot *snapshot) const
  {
    BKE_paint_material_projection_snapshot_free(snapshot);
  }
};

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
    /**
     * The band of the map this result was computed for, in map pixels, and the map it may land in.
     * Set only when the creation named a changed region, the channel's map already stood before the
     * compute (#bake_effect_channel_compute), and #map_uid still names the map at the commit: then
     * the commit writes #rect and leaves the pixels outside it as the previous result left them.
     * Otherwise #rect is unused and the whole stored buffer is written, as before.
     */
    bool regional = false;
    int rect[4] = {};
    uint32_t map_uid = 0;
    Vector<float> color;
    Vector<uchar> color_bytes;
  };
  struct ObjectResult {
    /** The object of this tile, or zero for a shared (UV) row's single map. */
    uint32_t object_uid = 0;
    uint32_t mesh_uid = 0;
    uint64_t identity = 0;
    /** UDIM tile number the result is written to, 0 for a non-tiled map. */
    int tile_number = 0;
    /**
     * The UDIM tile of the object's UVs this render covers; 0 leaves the tile to the surface. An
     * object whose UVs span several tiles has one entry per tile, all with the same
     * #object_index.
     */
    int uv_tile = 0;
    /** The index of the object in the row's targets, shared by the entries of its tiles. */
    int object_index = 0;
    /**
     * The surface of this object, rasterized on the main thread at creation and read-only on the
     * worker; null for a row reading no object geometry.
     */
    std::shared_ptr<const PaintMaterialCompositeSurface> surface;
    /**
     * What the worker reads of the object's mesh, matrices and slots when the row folds a
     * projection; owned by #PaintLayersBakeJob::snapshots, null for any other row.
     */
    const PaintMaterialProjectionSnapshot *snapshot = nullptr;
    /** The evaluated object #snapshot was taken of: compared by the worker, never dereferenced. */
    const Object *object = nullptr;
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
    /**
     * One entry per render of an object's tile block (one per UV tile with geometry); a shared (UV)
     * row has one entry with a null surface.
     */
    Vector<ObjectResult> objects;
    /** How many objects #objects covers, the tiles of one object counted once. */
    int object_num = 0;
    /** Every image tile the objects' blocks cover; empty selects one tile per object. */
    Vector<int> image_tiles;
  };
  Vector<RowResult> rows;
  /**
   * The snapshots the rows' objects were taken as, one per object. They are kept here and not in
   * #rows so the worker's cancel path, which drops the rows on its own thread, never releases a
   * mesh: #BKE_paint_layers_bake_job_free does, on the main thread.
   */
  Vector<std::unique_ptr<PaintMaterialProjectionSnapshot, BakeSnapshotDeleter>> snapshots;
  /**
   * The result of a CPU-computed effect that owns it (#paint_layer_effect_owns_result), computed on
   * the worker from #material_copy. The channels to compute are decided on the main thread at
   * creation, so the worker reads no state of the live material.
   */
  struct EffectResult {
    bUUID marker = {};
    int size = 0;
    /** The bake hash of the effect when the job was created; the commit drops a result whose live
     * hash moved (see #RowResult::hash). */
    uint32_t hash[2] = {};
    /** One channel the effect filters, with the map that stood when the job was created. */
    struct QueuedChannel {
      int channel = 0;
      /**
       * Whether the channel's map stood before the compute; only then may the commit patch its
       * band instead of writing the whole canvas. #map_uid is that map's identity, re-checked at
       * the commit because #bake_service_image may replace a map of another size or format.
       */
      bool had_map = false;
      uint32_t map_uid = 0;
    };
    /** The channels the effect filters; the worker fills #channels from them. */
    Vector<QueuedChannel> wanted_channels;
    /**
     * The rectangle of the effect's canvas whose inputs changed since its maps were last written,
     * resolved on the main thread at creation (#bake_effect_region_resolve). The effect's bake
     * subscription is per row, so one region serves every channel it filters; the per-channel
     * clip and halo are what #bake_effect_channel_compute derives from it. False means the whole
     * canvas, as before.
     */
    bool has_changed_region = false;
    rcti changed_region = {};
    Vector<ChannelResult> channels;
  };
  Vector<EffectResult> effects;
  /**
   * The GPU results of the queued owned effects, computed by the provider on the main thread at
   * creation -- the worker has no GPU context, so without this a GPU-computed effect's channel
   * would fall back to the CPU composite there. One entry per channel the provider answered; the
   * worker moves its entry out at the start of the effect stage (#bake_job_precomputed_take) and
   * releases the buffer once the map's own format holds the pixels, so each is held from here to
   * its take -- through the whole rows stage above. The ImBuf is the input's own size, a float RGBA
   * of the same order as the 256 MiB a 4K channel map costs (see #ChannelResult), and the converted
   * result that replaces it stays until the commit like every #ChannelResult. A channel with no
   * entry (no provider, not GPU-computed, refused) is computed on the CPU as before.
   */
  Vector<PrecomputedChannelResult> precomputed_effects;
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

/**
 * Compute the GPU results of the queued owned effects here, on the main thread: the worker has no
 * GPU context, and its own compute would leave every channel #BKE_paint_layers_effect_is_gpu_computed
 * answers true for to the CPU composite. The provider is asked on the original material -- this
 * runs before the description is localized, on the main thread, and the live description is exactly
 * what the queued hashes above were stamped from. The planner leaves a heavy owned effect to this
 * job in a windowed session; a background session's inline path computes the same channel on this
 * same (main) thread instead, never across the worker, so nothing races this for the results.
 * A channel the provider refuses is left out of the store and stays on the worker's CPU path as
 * before.
 *
 * The region the job's creation resolved for the effect (#bake_effect_region_resolve) rides along
 * to the provider, which may narrow its pass dispatches to it. The result is still a full-canvas
 * read-back: the provider's texture is valid as a whole and this codebase has no sub-region
 * texture read (CombinedGpuFrame::read_back runs GPU_texture_read whole), so the dispatch narrows
 * but the read-back does not. The commit lays only the band #bake_effect_channel_compute derives
 * from that region (#BKE_paint_layers_effect_valid_region), which is what lets the narrower
 * dispatch pay.
 */
static void bake_job_owned_effects_gpu_precompute(Material &ma, PaintLayersBakeJob &job)
{
  if (job.effects.is_empty()) {
    return;
  }
  const auto provider = BKE_paint_layers_owned_effect_gpu_provider_get();
  if (provider == nullptr) {
    return;
  }
  /* The whole precompute runs on the main thread before the worker starts, so it is a UI freeze no
   * other timer names: the per-channel `effect.own_bake.gpu` scopes below it carry the Hit/Render
   * split, this scope the total the bake's queue costs the wait for. */
  bke::paint_perf::ScopedTimer perf_precompute("effect.own_bake.precompute");
  int effect_num = 0;
  int channel_num = 0;
  int64_t readback_bytes = 0;
  for (const PaintLayersBakeJob::EffectResult &queued : job.effects) {
    const MaterialPaintLayer *effect = BKE_paint_layers_find(ma, queued.marker);
    if (effect == nullptr || !BKE_paint_layers_effect_is_gpu_computed(*effect)) {
      continue;
    }
    const MaterialPaintLayer *row = BKE_paint_layers_correction_owner(ma, *effect);
    if (row == nullptr) {
      continue;
    }
    effect_num++;
    for (const PaintLayersBakeJob::EffectResult::QueuedChannel &wanted : queued.wanted_channels) {
      const int channel = wanted.channel;
      channel_num++;
      /* The region the effect's subscription named at creation, or null for the whole canvas: see
       * the note above -- the read-back stays whole, the map write below is what the region pays
       * for. */
      const rcti *changed_region = queued.has_changed_region ? &queued.changed_region : nullptr;
      std::shared_ptr<ImBuf> result = provider(
          ma, *row, *effect, channel, PAINT_TILE_NUMBER_BASE, changed_region);
      if (result == nullptr) {
        /* No GPU answer for this channel: the worker computes it on the CPU as before. */
        continue;
      }
      /* A full canvas of floats per channel is the readback the freeze above waits on. */
      readback_bytes += int64_t(result->x) * result->y * 4 * int64_t(sizeof(float));
      PrecomputedChannelResult entry;
      entry.effect_marker = queued.marker;
      entry.channel = channel;
      entry.result = std::move(result);
      job.precomputed_effects.append(std::move(entry));
    }
  }
  perf_precompute.note("effects=%d channels=%d readback_bytes=%lld",
                       effect_num,
                       channel_num,
                       readback_bytes);
}

PaintLayersBakeJob *BKE_paint_layers_bake_job_create(Main &bmain,
                                                     Material &ma,
                                                     const Scene *scene,
                                                     const ViewLayer *view_layer,
                                                     const bool notice_surface_edits)
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
  if (notice_surface_edits) {
    bake_object_identities_notice(ma, view);
  }
  Vector<MaterialPaintLayer *> rows;
  BKE_paint_layers_foreach(ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
    rows.append(&layer);
    for (MaterialPaintLayer &effect : layer.effects) {
      if (paint_layer_kind_reads_anchor_point(effect)) {
        rows.append(&effect);
      }
    }
    for (MaterialPaintLayer &item : layer.mask_stack) {
      /* A mask effect whose grey reads the surface is a row of this pass too, though never heavy:
       * it is the synchronous planner's, which needs the view this pass has (see below). */
      if (paint_layer_kind_reads_anchor_point(item) ||
          (paint_layer_mask_item_bakes_effect(item) &&
           paint_layers_bake_node_reads_surface(ma, item)))
      {
        rows.append(&item);
      }
    }
    return true;
  });

  bool surface_sync_due = false;
  /* The snapshots of the objects projected rows are baked for, taken at most once each. */
  Map<const Object *, const PaintMaterialProjectionSnapshot *> snapshot_by_object;
  Vector<const Object *> controllers;
  bool controllers_ready = false;
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
      /* A due row that is light but reads the surface (a transform effect in its Decal frame) is
       * the synchronous planner's, which needs this view: the regeneration's own plan has no scene,
       * so without the run below nothing would ever bake it. */
      surface_sync_due = surface_sync_due || paint_layers_bake_node_reads_surface(ma, layer);
      continue;
    }
    const bool folds_projection = bake_subtree_folds_projection(ma, layer);
    if (folds_projection && (G.background || bake_drag_active(ma))) {
      /* A handle drag re-renders the row at every step at the coarse drag size, which the
       * synchronous planner owns; the full bake follows the drag as a job. */
      surface_sync_due = true;
      continue;
    }
    const int size = bake_size_for(ma, layer);
    if (size <= 0) {
      continue;
    }

    /* An Object or Generated row is rendered through the surface of each object it serves, and the
     * surfaces are rasterized here: the worker reads the grids and never touches the depsgraph. */
    PaintLayersBakeTargets targets;
    const bool needs_surface = paint_layers_bake_node_reads_surface(ma, layer);
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
      row.object_num = int(targets.objects.size());
      row.image_tiles = bake_targets_image_tiles(targets.objects);
      for (int object_index = 0; object_index < targets.objects.size() && surfaces_ok;
           object_index++)
      {
        const PaintLayersBakeTarget &target = targets.objects[object_index];
        const PaintMaterialProjectionSnapshot *snapshot = nullptr;
        if (folds_projection) {
          /* The worker projects through the object's mesh and matrices, which only this thread may
           * read: it gets a copy of them, one per object however many rows fold and however many
           * UV tiles the object renders. */
          snapshot = snapshot_by_object.lookup_default(target.object, nullptr);
          if (snapshot == nullptr) {
            if (!controllers_ready) {
              bake_projection_controllers_collect(ma, controllers);
              controllers_ready = true;
            }
            PaintMaterialProjectionSnapshot *created = BKE_paint_material_projection_snapshot_create(
                *target.object, ma, controllers, target.uv_tiles);
            if (created == nullptr) {
              /* No mesh, UV, triangle or slot the projection could read: the synchronous bake
               * would produce nothing either, so the row waits for a usable surface. */
              surfaces_ok = false;
              break;
            }
            job->snapshots.append(
                std::unique_ptr<PaintMaterialProjectionSnapshot, BakeSnapshotDeleter>(created));
            snapshot = created;
            snapshot_by_object.add(target.object, snapshot);
          }
        }
        /* One render per UV tile with geometry, each with the surface of its own tile. */
        for (const PaintLayersBakeTilePair &pair : bake_target_tile_pairs(target, object_index)) {
          std::shared_ptr<const PaintMaterialCompositeSurface> surface = bake_surface_build(
              layer, target, size, pair.uv_tile);
          if (surface == nullptr) {
            surfaces_ok = false;
            break;
          }
          PaintLayersBakeJob::ObjectResult object;
          if (folds_projection) {
            object.snapshot = snapshot;
            object.object = target.object;
          }
          object.object_uid = target.object_uid;
          object.mesh_uid = target.mesh_uid;
          object.identity = paint_layers_bake_row_identity(ma, layer, target, size);
          object.tile_number = pair.image_tile;
          object.uv_tile = pair.uv_tile;
          object.object_index = object_index;
          object.surface = std::move(surface);
          row.objects.append(std::move(object));
        }
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

  /* The effects that own their result are queued here, on the main thread: what the worker needs
   * from the live material (the due check, the canvas size, the channels the effect filters, the
   * hash it stamps) is decided now, and the worker only computes from the localized copy. The lower
   * effect comes first, so the one above it finds its input in the effect cache. */
  BKE_paint_layers_foreach(ma, [&](MaterialPaintLayer &layer, MaterialPaintLayer * /*parent*/) -> bool {
    for (MaterialPaintLayer &effect : layer.effects) {
      if (!paint_layer_effect_is_cpu_computed(effect)) {
        continue;
      }
      BakeRowRelease release = BakeRowRelease::Keep;
      if (!bake_row_is_due(ma, effect, view, release)) {
        bake_row_release_apply(bmain, ma, effect, release);
        continue;
      }
      if (!paint_layer_is_owned_effect(ma, effect) || !BKE_paint_layers_bake_is_heavy(ma, effect)) {
        continue;
      }
      const int size = bake_size_for(ma, effect);
      if (size <= 0) {
        continue;
      }
      PaintLayersBakeJob::EffectResult queued;
      queued.marker = effect.marker;
      queued.size = size;
      for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
        if (paint_layer_effect_filters(ma, effect, int(info.channel))) {
          PaintLayersBakeJob::EffectResult::QueuedChannel wanted;
          wanted.channel = int(info.channel);
          queued.wanted_channels.append(wanted);
        }
      }
      if (queued.wanted_channels.is_empty()) {
        continue;
      }
      /* Every gate has passed: only here is the bake structure allocated, so an effect nothing
       * computes never carries a permanently invalid one. */
      MaterialPaintLayerBake *bake = BKE_paint_layers_bake_struct_ensure(effect);
      bake->size = size;
      /* The map each channel stood with before this bake, read here on the main thread: the
       * worker may only patch a map that already held the previous result. A slot can keep a pointer
       * an undo or a reset freed (#bake_service_image_is_live compares the address without touching
       * it), and reading its id would be a use after free; a dead slot counts as no map, and the
       * commit makes a new one whose id cannot match. */
      for (PaintLayersBakeJob::EffectResult::QueuedChannel &wanted : queued.wanted_channels) {
        const Image *slot = bake->images[wanted.channel];
        const Image *map = (slot != nullptr && bake_service_image_is_live(bmain, slot)) ? slot :
                                                                                          nullptr;
        wanted.had_map = map != nullptr;
        wanted.map_uid = map != nullptr ? map->id.session_uid : 0;
      }
      BKE_paint_layers_bake_hash(ma, effect, queued.hash);
      /* The subscription's rectangle is the same one the inline writer would lay: one region per
       * effect row serves every channel it filters. Resolved here, on the main thread, before the
       * worker copies the material; the hash comparison inside the resolve is what makes a pixel
       * edit narrow the job while a parameter change does not. */
      rcti changed_region;
      if (const rcti *region = bake_effect_region_resolve(ma, effect, changed_region)) {
        queued.has_changed_region = true;
        queued.changed_region = *region;
      }
      job->effects.append(std::move(queued));
    }
    return true;
  });

  if (surface_sync_due && scene != nullptr && view_layer != nullptr) {
    /* On the main thread, which is where the evaluated mesh behind the Decal map may be read. It
     * tags the regeneration itself when a row landed. */
    BKE_paint_layers_bake_plan_run(bmain, ma, nullptr, scene, view_layer, notice_surface_edits);
  }
  if (job->rows.is_empty() && job->effects.is_empty()) {
    MEM_delete(job);
    return nullptr;
  }
  /* The worker has no GPU context, so the GPU-computed effects' channels are produced here, on the
   * main thread, while the description is still the original's; the localized copy below never
   * touches them. */
  bake_job_owned_effects_gpu_precompute(ma, *job);
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

/**
 * Move the precomputed GPU result of \a effect_marker's \a channel out of the job's store, or null
 * when its creation got none and the CPU compute is to run. The entry is left empty rather than
 * removed: each channel is taken once, and the walk over the store is cheaper than a rebuild per
 * take. The take is what releases the buffer on the worker -- it goes the moment the map's own
 * format below holds the pixels.
 */
static std::shared_ptr<ImBuf> bake_job_precomputed_take(PaintLayersBakeJob &job,
                                                        const bUUID &effect_marker,
                                                        const int channel)
{
  for (PrecomputedChannelResult &entry : job.precomputed_effects) {
    if (entry.channel == channel && BLI_uuid_equal(entry.effect_marker, effect_marker)) {
      /* A taken entry stays as an emptied slot rather than being removed: the take is once per
       * (marker, channel), and the channel at -1 keeps a repeated take from matching the slot
       * again -- the contract answers null, not a hit on an already-moved result. */
      entry.channel = -1;
      return std::move(entry.result);
    }
  }
  return nullptr;
}

/**
 * Compute \a effect's result in \a channel from the localized material \a ma and store it in the
 * map's own format at the \a size-square the map will have (the format and the resampling are what
 * #bake_effect_row_write gives the main-thread path). \a precomputed is the result the job's
 * creation computed on the main thread (the worker has no GPU context); moved in, and null when
 * there was none. False when the channel has no result.
 *
 * \a changed_region is the rectangle the creation resolved for the effect, or null for the whole
 * canvas. It narrows the band #r_result will name for the commit
 * (#BKE_paint_layers_effect_valid_region, the same clip and halo the main-thread writer uses), and
 * the CPU compute where that compute hits its cache -- the worker's localized copy usually misses
 * (its own session uid), so on this thread the region mostly pays for the map write rather than for
 * less compute. It applies only when the channel's map already stood before the compute
 * (\a had_map, with \a map_uid the identity the commit re-checks): a fresh or replaced map holds no
 * previous result outside the band, so it is written whole. The precomputed buffer is always the
 * effect's full canvas -- the provider narrows its dispatches but the GPU read-back is whole (see
 * #bake_job_owned_effects_gpu_precompute).
 */
static bool bake_effect_channel_compute(const Material &ma,
                                        const MaterialPaintLayer &effect,
                                        const int size,
                                        const MaterialPaintChannelInfo &info,
                                        const rcti *changed_region,
                                        const bool had_map,
                                        const uint32_t map_uid,
                                        std::shared_ptr<ImBuf> precomputed,
                                        PaintLayersBakeJob::ChannelResult &r_result)
{
  const int channel = int(info.channel);
  const MaterialPaintLayer *row = BKE_paint_layers_correction_owner(ma, effect);
  /* A region into a map that does not stand would be lost: the compute would answer for the band
   * only while the fresh map needs the whole canvas. The same rule the inline writer applies. */
  const rcti *compute_region = (changed_region != nullptr && had_map) ? changed_region : nullptr;
  /* The creation precomputed this channel's GPU result on the main thread, which is where the GPU
   * context is: consume it instead of computing here, before the CPU compute below runs. Moved out
   * of the job's store, so the buffer is released when this returns; a null leaves the CPU path as
   * it was -- this thread is not the main one, so the compute's own gate turns it to the CPU
   * composite for a channel the provider never answered. */
  std::shared_ptr<ImBuf> result = std::move(precomputed);
  const bool cpu_compute = result == nullptr;
  rcti compute_valid;
  BLI_rcti_init(&compute_valid, 0, 0, 0, 0);
  if (cpu_compute) {
    if (row == nullptr) {
      return false;
    }
    result = paint_layer_owned_effect_result_compute(ma,
                                                     *row,
                                                     effect,
                                                     channel,
                                                     PAINT_TILE_NUMBER_BASE,
                                                     compute_region,
                                                     &compute_valid);
  }
  if (result == nullptr || result->float_buffer.data == nullptr || result->x <= 0 ||
      result->y <= 0)
  {
    return false;
  }
  const int width = result->x;
  const int height = result->y;
  /* The band of the result the compute answered for; the whole canvas when no region was offered
   * or it cannot be narrowed. The CPU path's own rectangle may reach past it -- the cache names
   * the changed maps it patched, which can cover a band the subscription's chunks do not -- so the
   * two are united, while a whole-canvas answer leaves the narrower band to write. */
  rcti valid_region;
  BLI_rcti_init(&valid_region, 0, width, 0, height);
  if (compute_region != nullptr && row != nullptr) {
    BKE_paint_layers_effect_valid_region(ma,
                                         *row,
                                         effect,
                                         channel,
                                         PAINT_TILE_NUMBER_BASE,
                                         compute_region,
                                         width,
                                         height,
                                         valid_region);
    const bool compute_whole = compute_valid.xmin == 0 && compute_valid.ymin == 0 &&
                               compute_valid.xmax >= width && compute_valid.ymax >= height;
    if (cpu_compute && !BLI_rcti_is_empty(&compute_valid) && !compute_whole) {
      BLI_rcti_union(&valid_region, &compute_valid);
    }
  }
  const float *src = result->float_buffer.data;
  const int64_t pixel_num = int64_t(size) * size;
  r_result.channel = channel;
  r_result.is_color = info.is_color;
  r_result.stored_float = bake_channel_needs_float(effect, info);
  const ColorSpace *byte_colorspace = nullptr;
  if (r_result.stored_float) {
    r_result.color = Vector<float>(pixel_num * 4);
  }
  else {
    r_result.color_bytes = Vector<uchar>(pixel_num * 4);
    if (info.is_color) {
      byte_colorspace = IMB_colormanagement_space_get_named(
          IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DEFAULT_BYTE));
    }
  }
  /* The nearest sample of the result, each axis independently. The effect cache's buffer is
   * premultiplied by the input's content alpha and the map holds the straight colour, so the alpha
   * is not part of the result, unless the kind's result is a layer that covers by it (see
   * #bake_effect_row_write). */
  const bool keeps_alpha = paint_layer_kind_result_has_alpha(effect);
  for (int y = 0; y < size; y++) {
    const int src_y = min_ii(int(int64_t(y) * height / size), height - 1);
    const float *src_row = src + int64_t(src_y) * width * 4;
    for (int x = 0; x < size; x++) {
      const int src_x = min_ii(int(int64_t(x) * width / size), width - 1);
      const float *texel = src_row + int64_t(src_x) * 4;
      const float inv = texel[3] > 0.0f ? 1.0f / texel[3] : 1.0f;
      const float straight[4] = {
          texel[0] * inv, texel[1] * inv, texel[2] * inv, keeps_alpha ? texel[3] : 1.0f};
      const int64_t i = int64_t(y) * size + x;
      if (r_result.stored_float) {
        bake_pixel_store_float(straight, &r_result.color[i * 4]);
      }
      else {
        bake_pixel_store_byte(straight, info.is_color, byte_colorspace, &r_result.color_bytes[i * 4]);
      }
    }
  }
  /* The map rectangle the valid band lands in. A map texel x samples result texel
   * min(x * width / size, width - 1), the loop above, so the result columns [xmin, xmax) are read
   * by the map columns [xmin * size / width, xmax * size / width) rounded up -- the same numbers
   * the inline writer derives (#bake_effect_row_write). */
  if (compute_region != nullptr && !BLI_rcti_is_empty(&valid_region)) {
    r_result.regional = true;
    r_result.map_uid = map_uid;
    r_result.rect[0] = clamp_i(
        int((int64_t(valid_region.xmin) * size + width - 1) / width), 0, size);
    r_result.rect[1] = clamp_i(
        int((int64_t(valid_region.ymin) * size + height - 1) / height), 0, size);
    r_result.rect[2] = clamp_i(
        int((int64_t(valid_region.xmax) * size + width - 1) / width), 0, size);
    r_result.rect[3] = clamp_i(
        int((int64_t(valid_region.ymax) * size + height - 1) / height), 0, size);
  }
  return true;
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
  for (const PaintLayersBakeJob::EffectResult &effect : job.effects) {
    total_pairs += effect.wanted_channels.size();
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
           * and the surfaces they were rendered from go now, not at the free. The precomputed GPU
           * results none of the effect stage below would consume go with them. The commit reads no
           * rows once this mark is set. */
          job.cancelled = true;
          job.rows = Vector<PaintLayersBakeJob::RowResult>();
          job.effects = Vector<PaintLayersBakeJob::EffectResult>();
          job.precomputed_effects.clear();
          return;
        }
        const int channel = int(info.channel);
        Vector<float> color(int64_t(row.size) * row.size * 4);
        Vector<float> coverage(int64_t(row.size) * row.size);
        const bool rendered = BKE_paint_layers_bake_render_node(
            ma,
            *layer,
            channel,
            row.size,
            color.data(),
            coverage.data(),
            nullptr,
            object.surface,
            /* The snapshot, not the object, is what a projected row reads on this thread. */
            PaintLayerTileContext{
                .painted_object = PaintLayerSurfaceRef{object.object, object.snapshot},
                .tile_number = object.uv_tile});
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
      /* The render of this tile is done: a block of many tiles holds one surface per tile, which at
       * 4K is the largest thing the job keeps, and nothing reads the surface after its tile. */
      object.surface.reset();
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
  for (PaintLayersBakeJob::EffectResult &effect_result : job.effects) {
    const MaterialPaintLayer *effect = BKE_paint_layers_find(ma, effect_result.marker);
    if (effect == nullptr) {
      done_pairs += effect_result.wanted_channels.size();
      report();
      continue;
    }
    for (const PaintLayersBakeJob::EffectResult::QueuedChannel &wanted :
         effect_result.wanted_channels)
    {
      const int channel = wanted.channel;
      if (is_cancelled && is_cancelled()) {
        job.cancelled = true;
        job.rows = Vector<PaintLayersBakeJob::RowResult>();
        job.effects = Vector<PaintLayersBakeJob::EffectResult>();
        /* Whatever precomputed results the channels before this one left unconsumed go too. */
        job.precomputed_effects.clear();
        BKE_paint_layers_effect_cache_free_material(ma);
        return;
      }
      PaintLayersBakeJob::ChannelResult result;
      if (bake_effect_channel_compute(
              ma,
              *effect,
              effect_result.size,
              BKE_paint_material_channel_info(eMaterialPaintChannel(channel)),
              effect_result.has_changed_region ? &effect_result.changed_region : nullptr,
              wanted.had_map,
              wanted.map_uid,
              bake_job_precomputed_take(job, effect_result.marker, channel),
              result))
      {
        effect_result.channels.append(std::move(result));
      }
      done_pairs++;
      report();
    }
  }
  /* The effect cache entries a lower effect left for the one above it are worth nothing once the
   * results are held: they belong to the localized copy and would sit in the budget until it ends. */
  if (!job.effects.is_empty()) {
    BKE_paint_layers_effect_cache_free_material(ma);
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
    /* Not read off the surfaces: the worker releases each one as soon as its tile is rendered. */
    const bool tiled_row = row.object_num > 0;
    if (tiled_row && row.objects[0].snapshot != nullptr &&
        BKE_paint_layers_bake_stroke_active_get())
    {
      /* A stroke moves the mesh the snapshot was taken of (a sculpt stroke at every dab), and the
       * identity below can lag a cached content hash: nothing from a job that overlaps a stroke is
       * written. The row stays invalid, so the job the stroke's end starts renders it anew. */
      continue;
    }
    PaintLayersBakeTargets targets;
    if (tiled_row) {
      /* The objects or their geometry may have moved while the worker ran: the pixels then describe
       * surfaces the view layer no longer has, so they are dropped and the row re-bakes. */
      paint_layers_bake_targets_resolve(*ma, *layer, view, row.size, targets);
      if (targets.issue != PAINT_LAYERS_BAKE_OBJECTS_OK ||
          int(targets.objects.size()) != row.object_num)
      {
        continue;
      }
      bool matches = true;
      for (const PaintLayersBakeJob::ObjectResult &object : row.objects) {
        const PaintLayersBakeTarget &target = targets.objects[object.object_index];
        /* The tiles are compared along with the identity: the same objects with their UVs moved to
         * other tiles (or another block layout) would land the pixels in the wrong tiles. The
         * identity folds the block of a multi-tile object; the tile list covers the single-tile
         * scheme, where it is the tile of the UVs. */
        const Vector<PaintLayersBakeTilePair> pairs = bake_target_tile_pairs(
            target, object.object_index);
        const bool tile_found = std::any_of(
            pairs.begin(), pairs.end(), [&](const PaintLayersBakeTilePair &pair) {
              return pair.uv_tile == object.uv_tile && pair.image_tile == object.tile_number;
            });
        if (target.object_uid != object.object_uid || target.mesh_uid != object.mesh_uid ||
            !tile_found ||
            paint_layers_bake_row_identity(*ma, *layer, target, row.size) != object.identity)
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
    const int object_num = tiled_row ? row.object_num : 0;
    const Span<int> image_tiles = row.image_tiles;
    /* The renders each object wrote; an object is recorded once all of its tiles landed. */
    Array<int> object_pairs_written(std::max(object_num, 1), 0);
    Array<int> object_pairs_total(std::max(object_num, 1), 0);
    for (const PaintLayersBakeJob::ObjectResult &object : row.objects) {
      object_pairs_total[tiled_row ? object.object_index : 0]++;
    }
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
                                               object_num,
                                               image_tiles);
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
                bmain, *layer, -1, row.size, false, false, object_num, image_tiles))
        {
          bake_write_coverage_image(
              *coverage_image, object.common.data(), nullptr, object.tile_number);
        }
        object.common = Vector<float>();
      }
      object_pairs_written[tiled_row ? object.object_index : 0]++;
    }
    if (!row_written || !all_objects) {
      continue;
    }
    if (tiled_row) {
      for (const int object_index : targets.objects.index_range()) {
        if (object_pairs_written[object_index] != object_pairs_total[object_index]) {
          continue;
        }
        const PaintLayersBakeTarget &target = targets.objects[object_index];
        const PaintLayersBakeJob::ObjectResult *first = nullptr;
        for (const PaintLayersBakeJob::ObjectResult &object : row.objects) {
          if (object.object_index == object_index) {
            first = &object;
            break;
          }
        }
        if (first == nullptr) {
          continue;
        }
        MaterialPaintLayerBakeTile *tile = bake_tile_ensure(
            *layer->bake, target.object_uid, target.block_origin[0], target.block_origin[1]);
        tile->hash[0] = uint32_t(first->identity & 0xFFFFFFFFu);
        tile->hash[1] = uint32_t(first->identity >> 32);
      }
      layer->bake->object_count = row.object_num;
    }
    bake_stale_channel_maps_release(bmain, *layer, written_channels);
    layer->bake->hash[0] = row.hash[0];
    layer->bake->hash[1] = row.hash[1];
    /* The job renders at the full size and is never queued during a drag, so a reduced bake the drag
     * left behind is now replaced; without this the row would owe its full bake forever
     * (#bake_drag_reduced_owes) and be queued again after every commit. */
    bake_drag_reduced_forget(*ma, *layer);
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
  for (PaintLayersBakeJob::EffectResult &effect_result : job.effects) {
    MaterialPaintLayer *effect = BKE_paint_layers_find(*ma, effect_result.marker);
    /* The effect was removed, switched to Never or stopped owning its result while the worker
     * ran: nothing to write. */
    if (effect == nullptr || effect->bake == nullptr || !paint_layer_is_owned_effect(*ma, *effect) ||
        BKE_paint_layers_bake_mode_get(*effect) == MA_PAINT_LAYER_BAKE_NEVER)
    {
      continue;
    }
    uint32_t current_hash[2];
    BKE_paint_layers_bake_hash(*ma, *effect, current_hash);
    if (current_hash[0] != effect_result.hash[0] || current_hash[1] != effect_result.hash[1]) {
      /* A parameter, the row's mapping or the source moved while the worker ran: the pixels are
       * the old description's and are dropped. The effect is invalid, so the next plan queues it
       * again. */
      continue;
    }
    Vector<int> written_channels;
    for (PaintLayersBakeJob::ChannelResult &channel : effect_result.channels) {
      const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
          eMaterialPaintChannel(channel.channel));
      effect->bake->size = effect_result.size;
      Image *color_image = bake_service_image(bmain,
                                             *effect,
                                             channel.channel,
                                             effect_result.size,
                                             channel.is_color,
                                             bake_channel_needs_float(*effect, info),
                                             0);
      if (color_image == nullptr) {
        continue;
      }
      /* The band the worker's compute answered for, when the creation had a region for the effect
       * and the map that stood then is still the one #bake_service_image just returned. A map it
       * replaced is fresh and blank outside the band, so the whole stored buffer is laid into it;
       * the stored buffer is always the full canvas -- the GPU read-back is not narrowed (see
       * #bake_job_owned_effects_gpu_precompute). */
      int rect[4] = {channel.rect[0], channel.rect[1], channel.rect[2], channel.rect[3]};
      const bool regional = channel.regional && color_image->id.session_uid == channel.map_uid;
      bake_write_stored_image(
          *color_image,
          channel.stored_float,
          channel.stored_float ? static_cast<const void *>(channel.color.data()) :
                                 static_cast<const void *>(channel.color_bytes.data()),
          regional ? rect : nullptr,
          0);
      channel.color = Vector<float>();
      channel.color_bytes = Vector<uchar>();
      written_channels.append(channel.channel);
    }
    if (written_channels.is_empty()) {
      continue;
    }
    bake_stale_channel_maps_release(bmain, *effect, written_channels);
    BKE_paint_layers_bake_finalize(*ma, *effect);
    /* The map holds the result now: a copy the main thread's effect cache kept is for nothing. */
    for (const int channel : written_channels) {
      paint_layer_owned_effect_cache_drop(*ma, *effect, channel, PAINT_TILE_NUMBER_BASE);
    }
    /* The graph lays the map from now on, which is a new shape; unlike a row's bake there is no live
     * form of the effect that was being painted, so no hold waits for the hand to stop. */
    need_regen = true;
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
        for (const MaterialPaintLayer &effect : layer.effects) {
          /* An effect that owns its result and has none yet (or waits for a stroke to end) keeps the
           * signal up like a row does. */
          pending = pending || bake_owned_effect_pending(*ma, effect);
        }
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
  for (const PaintLayersBakeJob::EffectResult &effect : job.effects) {
    for (const PaintLayersBakeJob::ChannelResult &channel : effect.channels) {
      bytes += channel.color.size() * int64_t(sizeof(float)) +
               channel.color_bytes.size() * int64_t(sizeof(uchar));
    }
  }
  /* The precomputed GPU results the creation left, still waiting for the effect stage to take
   * them; the float RGBA of each is at the effect input's own size. */
  for (const PrecomputedChannelResult &precomputed : job.precomputed_effects) {
    if (precomputed.result != nullptr && precomputed.result->float_buffer.data != nullptr) {
      bytes += int64_t(precomputed.result->x) * precomputed.result->y * 4 * int64_t(sizeof(float));
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
  /* The snapshots' meshes go on this (the main) thread, after the worker has finished. */
  job.rows.clear();
  job.snapshots.clear();
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
  /**
   * The UDIM tiles the render covers, the source stack's own list: a canvas tiled beyond its base
   * gets one render per tile, and the maps the target takes hold each tile's pixels.
   */
  Vector<int> tiles;
  struct ChannelResult {
    int channel = 0;
    bool is_color = false;
    /**
     * The straight RGBA of one tile of #tiles, in the same order; empty where the tile failed.
     * Held until the commit, so the job's peak is #channels x #tiles x size² x 16 bytes -- a 4K
     * colour tile is 268 MiB -- which the commit pays down by freeing each tile as it writes it.
     */
    Vector<Vector<float>> color;
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

bool BKE_paint_layers_row_result_job_channel_set(PaintLayersRowResultJob &job,
                                                 const int channel,
                                                 const float *rgba)
{
  if (job.material_copy == nullptr) {
    return false;
  }
  /* The GPU render covers the base tile only; a stack tiled beyond it is rendered tile by tile on
   * the CPU. */
  Vector<int> tiles;
  BKE_paint_layers_stack_tiles_collect(*job.material_copy, tiles);
  if (BKE_paint_layers_stack_is_tiled(tiles)) {
    return false;
  }
  PaintLayersRowResultJob::ChannelResult result;
  result.channel = channel;
  result.is_color = BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).is_color;
  result.color.append(Vector<float>(Span<float>(rgba, int64_t(job.size) * job.size * 4)));
  job.channels.append(std::move(result));
  return true;
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
  /* The stack's own tiles, so a canvas tiled beyond its base is rendered tile by tile: each render
   * reads the row's maps at that tile and describes that tile's UV square. */
  BKE_paint_layers_stack_tiles_collect(ma, job.tiles);
  /* The slot of each channel's result in #job.channels, by the channel enum rather than a walk. */
  std::array<int, PAINT_MATERIAL_CHANNEL_NUM> channel_slots;
  channel_slots.fill(-1);
  /* The channels handed in already (the GPU render) keep their result; the tiles skip them. */
  std::array<bool, PAINT_MATERIAL_CHANNEL_NUM> handed_in;
  handed_in.fill(false);
  for (const int slot : job.channels.index_range()) {
    channel_slots[job.channels[slot].channel] = int(slot);
    handed_in[job.channels[slot].channel] = true;
  }
  for (const int tile_index : job.tiles.index_range()) {
    const int tile_number = job.tiles[tile_index];
    for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
      const int channel = int(info.channel);
      if (handed_in[channel]) {
        continue;
      }
      Vector<float> color(int64_t(job.size) * job.size * 4);
      Vector<float> coverage(int64_t(job.size) * job.size);
      /* The whole row: a source with a CPU effect would otherwise come back without its mask and
       * opacity, which only a live substitution lays over such a map. */
      if (!BKE_paint_layers_bake_render_node(ma,
                                            *source,
                                            channel,
                                            job.size,
                                            color.data(),
                                            coverage.data(),
                                            nullptr,
                                            nullptr,
                                            PaintLayerTileContext{.tile_number = tile_number},
                                            true))
      {
        /* The tile carries nothing: the map is left without it, and the composite reads its
         * constant there, the way a map the stack never painted reads. */
        continue;
      }
      /* The channel is entered on its first rendered tile only, so a channel no tile rendered is
       * skipped, as it was before the render went per tile. The slot index spares the walk over
       * the results the channels before it already appended. */
      PaintLayersRowResultJob::ChannelResult *result = nullptr;
      if (channel_slots[channel] >= 0) {
        result = &job.channels[channel_slots[channel]];
      }
      if (result == nullptr) {
        PaintLayersRowResultJob::ChannelResult fresh;
        fresh.channel = channel;
        fresh.is_color = info.is_color;
        fresh.color.resize(job.tiles.size());
        job.channels.append(std::move(fresh));
        channel_slots[channel] = int(job.channels.size()) - 1;
        result = &job.channels.last();
      }
      result->color[tile_index] = std::move(color);
    }
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
    const bool rendered = std::any_of(result.color.begin(),
                                      result.color.end(),
                                      [](const Vector<float> &tile_color) {
                                        return !tile_color.is_empty();
                                      });
    if (!rendered) {
      continue;
    }
    char name[192];
    SNPRINTF(name, "%s %s", target->name[0] != '\0' ? target->name : "Layer", info.ui_name);
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    /* A map that is not the base tile alone has to be tiled, or the other tiles have nowhere to go;
     * the single-tile result stays the plain generated map it always was. */
    const bool tiled = BKE_paint_layers_stack_is_tiled(job.tiles);
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
                                           tiled);
    if (image == nullptr) {
      continue;
    }
    if (tiled) {
      /* The tiles the render covered are added before the writes, each generated like the base
       * one at the map's own side, so every tile the stack holds carries the result. */
      for (const int tile_number : job.tiles) {
        if (tile_number == PAINT_TILE_NUMBER_BASE) {
          continue;
        }
        BKE_paint_layers_map_coverage_tile_ensure(*image, tile_number, job.size, job.size);
      }
    }
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    /* A row result is a bake the stack owns, so the browsers hide it like every other map. */
    BKE_paint_layers_image_mark_internal(*image);
    /* The render is the CPU one the bake planner uses, so the colour's alpha already is the source
     * row's content alpha. Written through unchanged: the target map then means the same thing any
     * other paint-layer map does. */
    for (const int tile_index : job.tiles.index_range()) {
      Vector<float> &tile_color = result.color[tile_index];
      if (tile_color.is_empty()) {
        continue;
      }
      bake_write_image(*image, tile_color.data(), info.is_color, nullptr, job.tiles[tile_index]);
      /* Written: the tile's RGBA is hundreds of MiB at 4K, so it goes now rather than at the free,
       * and the commit's own peak stays one tile per channel. */
      tile_color.clear_and_shrink();
    }
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
