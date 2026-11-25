/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The consumer end of the pixel-edit subscription, extracted from `paint_layers_bake.cc`: the
 * drain of a material's subscriptions (#BKE_paint_layers_bake_notice_changes) and the coverage
 * culling it applies to the regions the partial-update log reports. The subscription records
 * themselves live in `paint_layers_bake_intern.hh`; the structural validity they defer to is in
 * `paint_layers_bake_hash.cc`.
 *
 * The culling is only as safe as its agreement with #bake_hash_layer: a row that can move its bake
 * past an edited rectangle must never drop a region there. Changing what enters the hash of a row
 * means revisiting `bake_coverage_pointwise`.
 */

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_base.h"
#include "BLI_rect.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_layers_node_group.hh"
#include "BKE_paint_material_composite.hh"

#include "IMB_imbuf_types.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"

#include "paint_layers_bake_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Coverage culling of reported pixel-edit regions
 *
 * A region the partial-update log reports for one of a row's source maps can still lie outside
 * everything the row covers: a stroke that painted on the faces of another material marks the
 * canvas, a stamp whose every texel a mask zeroed marks its rectangle. Inside such a region the
 * row's bake cannot move -- an effect's result is premultiplied by the input's content alpha, a
 * row's own result by its map's alpha -- so the region is dropped before it can mark the row
 * stale and start a whole-canvas re-bake of the effect.
 *
 * The culling is a *secondary* guard at the consumer. The primary one is the precision of the
 * producers themselves -- the stroke-end full-update gate, the curve-patch raster that marks only
 * what it wrote, the node-level test that skips foreign materials -- and a writer that reports no
 * edit costs nothing here. Writers whose reports stay coarse (the shape composite's per-region
 * marks, a clone dab over every tile) are narrowed by the coverage, and nothing else filters for
 * them. Whether a region may be dropped is decided by the row's bake, not by the pixels alone: a
 * pointwise chain (#bake_coverage_pointwise) cannot move the bake past the edited rectangle, so
 * only such a row drops an uncovered region. A neighbourhood kernel reads past it, a folded
 * mapping moves the edit to the mapped rectangle, a projection reads through the surface, and a
 * whole-map kind spans the canvas -- none of them may cull, or an erased stroke under a Blur
 * would leave the covered neighbours of the erase stale in the bake.
 * \{ */

/** The coverage of a subscription's row, resolved once per drain. */
struct BakeCoveragePlane {
  enum class Kind : int8_t {
    /** No plane resolved, or one that covers whole: keep every region. */
    Ignore,
    /** The row covers nothing: every region is outside. */
    Nothing,
    /** The plane image: #grey reads the red channel, the alpha otherwise. */
    Image,
  };

  Kind kind = Kind::Ignore;
  Image *image = nullptr;
  bool grey = false;
};

/** Which content plane gates \a layer's bake, resolved the way the composite reads it. */
static BakeCoveragePlane bake_coverage_plane_resolve(const Material &ma,
                                                     const MaterialPaintLayer &layer)
{
  BakeCoveragePlane plane;
  const MaterialPaintLayer *cov_row = &layer;
  /* An effect's result is premultiplied by the input's content alpha, so the row the effect
   * corrects owns the coverage. A mask item's grey gates the row that carries it; finding that
   * row is not worth the walk yet, so mask items keep every region. */
  if (paint_layer_is_owned_effect(ma, layer)) {
    cov_row = BKE_paint_layers_correction_owner(ma, layer);
    if (cov_row == nullptr) {
      return plane;
    }
  }
  else if (paint_layer_mask_item_bakes_effect(layer)) {
    return plane;
  }
  const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(
      ma, *cov_row, PaintLayerMaterialUse::Composite);
  switch (coverage.kind) {
    case PaintLayerMaterialCoverageKind::LiveAlpha:
      plane.kind = BakeCoveragePlane::Kind::Image;
      plane.image = coverage.image;
      plane.grey = false;
      return plane;
    case PaintLayerMaterialCoverageKind::Baked:
      if (coverage.image != nullptr) {
        plane.kind = BakeCoveragePlane::Kind::Image;
        plane.image = coverage.image;
        plane.grey = true;
        return plane;
      }
      break;
    case PaintLayerMaterialCoverageKind::LiveConstant:
      /* Read through the same #PaintLayerMaterialUse the composite reads, and a changed constant
       * is a structural edit that moves the bake hash, so the answer cannot drift between this
       * drain and the bake it gates. */
      plane.kind = (coverage.constant > 0.0f) ? BakeCoveragePlane::Kind::Ignore :
                                                BakeCoveragePlane::Kind::Nothing;
      return plane;
    default:
      break;
  }
  /* A plain row covers by its content map's alpha: the record whose channel tracks the content
   * alpha and whose map does not read the alpha as data. A row that reads a source material
   * covers only what its coverage source says, which is answered above; nothing else is trusted
   * for it. */
  if (paint_layer_kind_has_flag(*cov_row, PAINT_LAYER_KIND_READS_SOURCE)) {
    return plane;
  }
  for (const int i : IndexRange(cov_row->channels_num)) {
    const MaterialPaintLayerChannel &record = cov_row->channels[i];
    if (record.image == nullptr ||
        !BKE_paint_material_channel_tracks_content_alpha(eMaterialPaintChannel(record.channel)) ||
        paint_layer_map_alpha_is_data(&record))
    {
      continue;
    }
    plane.kind = BakeCoveragePlane::Kind::Image;
    plane.image = record.image;
    plane.grey = false;
    return plane;
  }
  return plane;
}

/** Whether one texel of the plane buffer carries coverage. */
static bool bake_coverage_texel_set(const ImBuf &ibuf, const int64_t texel, const bool grey)
{
  if (ibuf.float_buffer.data != nullptr) {
    const float *pixel = ibuf.float_buffer.data + texel * 4;
    return grey ? (pixel[0] > 0.0f) : (pixel[3] > 0.0f);
  }
  if (ibuf.byte_buffer.data != nullptr) {
    const uchar *pixel = ibuf.byte_buffer.data + texel * 4;
    return grey ? (pixel[0] > 0) : (pixel[3] > 0);
  }
  /* No readable pixels: the region is kept. */
  return true;
}

/* The image buffers one drain's coverage scans read, one per (image, tile): the regions of a drain
 * hit the same tiles again and again, and every acquire takes the image's cache mutex. The cache
 * lives for one #BKE_paint_layers_bake_notice_changes call and is released with it, so nothing has
 * to invalidate it: the drain runs on the main thread between strokes, and the pixels cannot move
 * under it. */
struct BakeCoverageBuffers {
  struct Key {
    Image *image;
    int tile;

    bool operator==(const Key &other) const
    {
      return image == other.image && tile == other.tile;
    }
    uint64_t hash() const
    {
      return get_default_hash(image, tile);
    }
  };
  struct Acquired {
    Image *image = nullptr;
    ImBuf *ibuf = nullptr;
    void *lock = nullptr;
    /* Whether the tile holds any covered texel at all, per coverage kind, computed on the first
     * scan that needs it: a tile with no coverage drops all its regions in one whole-tile scan
     * instead of one scan per region. The scan early-exits on the first covered texel, so a
     * covered tile costs little more than its first region. */
    std::optional<bool> covered_alpha;
    std::optional<bool> covered_grey;
  };

  Map<Key, Acquired> map;

  ~BakeCoverageBuffers()
  {
    for (const Acquired &acquired : map.values()) {
      BKE_image_release_ibuf(acquired.image, acquired.ibuf, acquired.lock);
    }
  }
};

/** Whether any texel of the whole buffer carries coverage, memoized per drain in \a memo. */
static bool bake_tile_any_coverage(const ImBuf &ibuf, const bool grey, std::optional<bool> &memo)
{
  if (memo.has_value()) {
    return *memo;
  }
  bool any = false;
  if (ibuf.x > 0 && ibuf.y > 0) {
    const int64_t texels = int64_t(ibuf.x) * ibuf.y;
    for (int64_t texel = 0; texel < texels && !any; texel++) {
      any = bake_coverage_texel_set(ibuf, texel, grey);
    }
  }
  memo = any;
  return any;
}

/**
 * Whether any texel of the coverage plane lies under \a region, a rectangle of tile \a tile_number
 * of \a changed_image: false lets the caller drop the region, and every failure of the test keeps
 * it.
 *
 * Only a region of the plane's own image is tested: the plane is read through the row's mapping,
 * so a region of a different image has no rectangle in the plane's pixel space to trust without
 * that mapping.
 */
static bool bake_region_has_coverage(Image &changed_image,
                                     const int tile_number,
                                     const rcti &region,
                                     const BakeCoveragePlane &plane,
                                     BakeCoverageBuffers &buffers)
{
  if (plane.kind == BakeCoveragePlane::Kind::Nothing) {
    return false;
  }
  if (plane.kind != BakeCoveragePlane::Kind::Image || plane.image != &changed_image) {
    return true;
  }
  BakeCoverageBuffers::Acquired &acquired = buffers.map.lookup_or_add_default(
      BakeCoverageBuffers::Key{&changed_image, tile_number});
  if (acquired.ibuf == nullptr) {
    ImageUser iuser{};
    /* A non-UDIM image owns one tile, 1001, which is what the partial-update log reports, so the
     * tile number selects nothing else for it. */
    iuser.tile = tile_number;
    acquired.image = &changed_image;
    acquired.ibuf = BKE_image_acquire_ibuf(&changed_image, &iuser, &acquired.lock);
  }
  ImBuf *ibuf = acquired.ibuf;
  if (ibuf == nullptr || ibuf->x <= 0 || ibuf->y <= 0) {
    return true;
  }
  /* A tile with no coverage anywhere drops every region of it in one whole-tile scan. */
  if (!bake_tile_any_coverage(
          *ibuf, plane.grey, plane.grey ? acquired.covered_grey : acquired.covered_alpha))
  {
    return false;
  }
  const int x0 = clamp_i(region.xmin, 0, ibuf->x);
  const int x1 = clamp_i(region.xmax, 0, ibuf->x);
  const int y0 = clamp_i(region.ymin, 0, ibuf->y);
  const int y1 = clamp_i(region.ymax, 0, ibuf->y);
  bool covered = false;
  if (x1 > x0 && y1 > y0) {
    const int width = ibuf->x;
    for (int y = y0; y < y1 && !covered; y++) {
      for (int x = x0; x < x1; x++) {
        if (bake_coverage_texel_set(*ibuf, int64_t(y) * width + x, plane.grey)) {
          covered = true;
          break;
        }
      }
    }
  }
  return covered;
}

/**
 * One CPU-computed effect's reach, with the filter parameters resolved from the row's description.
 * The schema fold they come from lives beside the composite that reads the same numbers; the
 * classification itself is #BKE_paint_layers_effect_reach, the one answer the plan gives.
 */
static EffectReach bake_coverage_effect_reach(const MaterialPaintLayer &effect)
{
  /* A chain lays its definition's members one after another, each over the result of the ones
   * before it, so an edit can move the chain's bake past the edited rectangle only as far as the
   * widest member can move it. A definition that is gone lays nothing, but a bake that was made
   * from one cannot be narrowed: nothing is known about what it held. */
  if (paint_layer_effect_is_chain(effect)) {
    const Material *definition = effect.material;
    if (definition == nullptr) {
      return EffectReach::WholeMap;
    }
    for (const MaterialPaintLayer &member : definition->paint_layers) {
      if (member.role != MA_PAINT_LAYER_ROLE_EFFECT) {
        continue;
      }
      /* A member the chain's contract refuses is skipped by the unfolding, but a hand-made
       * definition may hold one: what it would lay is unknown, so the whole map is kept. */
      if (!paint_layer_effect_is_smart_filter_member(member)) {
        return EffectReach::WholeMap;
      }
      /* A member the contract lets through without a CPU kernel is a pointwise colour
       * correction: that is the shape the portability test admits, and the composite lays it as
       * the plain filter branch. The shared reach API answers WholeMap for such a kind because no
       * GPU pass plan describes it -- the conservative answer a lone row keeps, as it says
       * nothing about the CPU kernel -- while the chain knows the member is laid per texel and
       * reads the contract instead. */
      const EffectReach member_reach = paint_layer_effect_is_cpu_computed(member) ?
                                           bake_coverage_effect_reach(member) :
                                           EffectReach::Pointwise;
      if (member_reach != EffectReach::Pointwise) {
        return EffectReach::WholeMap;
      }
    }
    return EffectReach::Pointwise;
  }
  /* A node-group filter's reach is a property of its graph, not of a filter parameter table: a
   * graph of per-pixel node functions is pointwise, while a Bevel or Ambient Occlusion anywhere in
   * it reads a neighborhood and keeps every region. */
  if (effect.kind_subtype == MA_PAINT_LAYER_FILTER_NODE_GROUP) {
    return BKE_paint_layers_node_group_reaches_whole_map(effect) ? EffectReach::WholeMap :
                                                                   EffectReach::Pointwise;
  }
  float params[PAINT_MATERIAL_COMPOSITE_FILTER_PARAMS_NUM] = {};
  paint_layer_effect_filter_params(effect, params);
  return BKE_paint_layers_effect_reach(eMaterialPaintLayerFilter(effect.kind_subtype), params);
}

/**
 * Whether an edit of the plane moves the row's bake only inside the edited rectangle. Only such a
 * row may drop an uncovered region: a neighbourhood kernel (a Blur) reads past it, a folded
 * mapping (a Mirror) moves the edit to the mapped rectangle, a projection reads through the
 * surface, and a whole-map kind (a Color Match) spans the canvas.
 *
 * The checks below are the same row of decisions `bake_hash_layer` makes about what a row's bake
 * contains, so the two can be read side by side: a change to what enters the hash of a row must be
 * mirrored here. Everything that is not pointwise returns early, so a new kind of row that the hash
 * learns about keeps its regions unless this function is taught otherwise.
 *
 * \a cpu_in_subtree is #paint_layer_subtree_has_cpu_effect of the row the bake is made for, handed
 * down through the children exactly as the hash and the fold logic hand it, so a projection nested
 * under a CPU-effect row is judged folded the same way those decide.
 */
static bool bake_coverage_pointwise(const Material &ma,
                                    const MaterialPaintLayer &row,
                                    const bool cpu_in_subtree)
{
  /* Everything below is "not pointwise" and keeps every region, except the effect row's own reach.
   * A projection first: it reads the map through the surface, and the bake pixels an edit moves
   * live in the frame's space, which the plane's rectangle cannot see. */
  if (BKE_paint_layers_projection_applies(ma, row)) {
    return false;
  }
  /* An effect row bakes its one kernel over its input map (its owner's content, or the rendered
   * slice of a row it corrects); nothing else of the stack renders into it. It must be asked
   * before the source checks below, because a correction that reads a material is such a row. */
  if (row.role == MA_PAINT_LAYER_ROLE_EFFECT) {
    return bake_coverage_effect_reach(row) == EffectReach::Pointwise;
  }
  /* A row that renders another material, or an Anchor Point slice, re-renders the source whole: an
   * edit's rectangle has no place in that render to trust (#bake_row_keeps_effects_apart). */
  if (paint_layer_kind_has_flag(row, PAINT_LAYER_KIND_READS_SOURCE) ||
      paint_layer_kind_reads_anchor_point(row))
  {
    return false;
  }
  /* A mapping folded into the bake resamples the map: the bake rectangle of an edit is the mapped
   * one, not the edited one (the hash's #bake_own_mapping_folds). */
  if (bake_own_mapping_folds(ma, row, cpu_in_subtree)) {
    return false;
  }
  /* The row's own CPU effects and its children render into its bake, in storage order, each
   * reading the ones before it: any one of them that reads past the edited texel widens the reach
   * of the whole bake. A chain is not one CPU kind of its own -- its members are -- but its
   * definition decides how far it reads, so it is asked here beside them. */
  for (const MaterialPaintLayer &effect : row.effects) {
    if ((effect.flag & MA_PAINT_LAYER_ENABLED) == 0) {
      continue;
    }
    if (!paint_layer_effect_is_cpu_computed(effect) && !paint_layer_effect_is_chain(effect)) {
      continue;
    }
    /* An effect that owns its result apart is baked on its own, not inside this row's bake: its
     * own subscription reports its edits. (Such an effect hangs only on a source or Anchor Point
     * row, which returns above, so this stays the guard the hash's effect loop has.) */
    if (paint_layer_effect_owns_result(ma, row, effect)) {
      continue;
    }
    if (bake_coverage_effect_reach(effect) != EffectReach::Pointwise) {
      return false;
    }
  }
  for (const MaterialPaintLayer &child : row.children) {
    if (!bake_coverage_pointwise(ma, child, cpu_in_subtree)) {
      return false;
    }
  }
  return true;
}

/** \} */

/* A region drained from one source's partial-update log, waiting for its coverage test outside the
 * runtime lock: the scan acquires an image buffer, which can load from disk, and the lock guards
 * only the map. */
struct BakePendingRegion {
  Image *image;
  int tile_number;
  rcti region;
};

/* One subscription's drained regions, and what the re-lock needs to apply the kept ones. */
struct BakePendingEntry {
  bUUID marker;
  bool was_changed = false;
  bool changed_by_full_update = false;
  Vector<BakePendingRegion> regions;
  /* Whether any region survived the coverage test, and their union (exclusive, as the log). */
  bool kept = false;
  rcti region = {0, 0, 0, 0};
};

/** Fold \a candidate into \a drained's union of kept regions. */
static void bake_pending_region_keep(BakePendingEntry &drained, const BakePendingRegion &candidate)
{
  if (!drained.kept) {
    drained.region = candidate.region;
    drained.kept = true;
    return;
  }
  BLI_rcti_union(&drained.region, &candidate.region);
}

void BKE_paint_layers_bake_notice_changes(Material &ma)
{
  bool any = false;
  /* A baked row stops being substituted the moment a source map changes, which is a different shape
   * of generated graph. */
  bool newly_changed = false;
  Vector<BakePendingEntry> pending;
  {
    /* Phase 1 -- under the runtime lock: drain every source's log into candidate regions. The lock
     * is read per material eval from the depsgraph task pool, and the coverage scan below acquires
     * image buffers, which can load from disk or read back from the GPU: it must not run here,
     * against the map's own rule of holding the lock around the map and nothing else. */
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
    if (runtime == nullptr) {
      return;
    }
    for (BakeLayerSubscription &entry : runtime->layers) {
      const bool was_changed = entry.changed;
      pending.append_as();
      BakePendingEntry &drained = pending.last();
      drained.marker = entry.marker;
      drained.was_changed = was_changed;
      for (BakeSubscriptionSource &source : entry.sources) {
        const auto result = bke::image::partial_update::BKE_image_partial_update_collect_changes(
            source.image, source.user);
        if (result == bke::image::partial_update::ePartialUpdateCollectResult::FullUpdateNeeded) {
          /* No rectangle to trust: the planner re-bakes the whole row. */
          entry.changed = true;
          entry.has_region = false;
          drained.changed_by_full_update = true;
          any = true;
        }
        else if (result ==
                 bke::image::partial_update::ePartialUpdateCollectResult::PartialChangesDetected)
        {
          bke::image::partial_update::PartialUpdateRegion region;
          while (bke::image::partial_update::BKE_image_partial_update_get_next_change(
                     source.user, &region) ==
                 bke::image::partial_update::ePartialUpdateIterResult::ChangeAvailable)
          {
            drained.regions.append({source.image, region.tile_number, region.region});
          }
        }
      }
      newly_changed |= drained.changed_by_full_update && !was_changed;
    }
  }

  /* Phase 2 -- outside the lock: the coverage test acquires an image buffer, which can load from
   * disk, and no image code takes the bake runtime lock, so the two never nest. The runtime map is
   * not touched here; the kept regions are applied back under the lock below. */
  BakeCoverageBuffers buffers;
  for (BakePendingEntry &drained : pending) {
    if (drained.regions.is_empty()) {
      continue;
    }
    /* The row the subscription was built for, or null when the row is gone; its coverage decides
     * which reported regions can move the bake at all. */
    const MaterialPaintLayer *entry_layer = BKE_paint_layers_find(ma, drained.marker);
    if (entry_layer == nullptr) {
      for (const BakePendingRegion &candidate : drained.regions) {
        bake_pending_region_keep(drained, candidate);
      }
      continue;
    }
    const BakeCoveragePlane plane = bake_coverage_plane_resolve(ma, *entry_layer);
    if (plane.kind == BakeCoveragePlane::Kind::Nothing) {
      /* The row covers nothing anywhere: every region is outside it, whatever its bake reads. */
      continue;
    }
    if (plane.kind != BakeCoveragePlane::Kind::Image) {
      for (const BakePendingRegion &candidate : drained.regions) {
        bake_pending_region_keep(drained, candidate);
      }
      continue;
    }
    /* A pointwise bake cannot move past the edited rectangle, so only it may drop an uncovered
     * region; any neighbourhood, whole-map, folded or projected bake keeps its regions whole. */
    const bool cullable = bake_coverage_pointwise(
        ma, *entry_layer, paint_layer_subtree_has_cpu_effect(*entry_layer));
    for (const BakePendingRegion &candidate : drained.regions) {
      if (cullable &&
          !bake_region_has_coverage(
              *candidate.image, candidate.tile_number, candidate.region, plane, buffers))
      {
        continue;
      }
      bake_pending_region_keep(drained, candidate);
    }
  }

  {
    /* Phase 3 -- under the lock again: apply the kept regions. An entry may have been re-subscribed
     * or removed while the coverage ran; a fresh subscription's users were drained at creation, so
     * the drained regions of a gone entry are stale, not missing, and dropping them is safe. */
    std::lock_guard lock(bake_runtime_mutex());
    BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
    if (runtime == nullptr) {
      return;
    }
    for (BakePendingEntry &drained : pending) {
      if (!drained.kept || drained.changed_by_full_update) {
        /* A full update already reset the rectangle: the planner re-bakes the whole row, and no
         * region of the drain can narrow that. */
        continue;
      }
      BakeLayerSubscription *entry = nullptr;
      for (BakeLayerSubscription &candidate : runtime->layers) {
        if (BLI_uuid_equal(candidate.marker, drained.marker)) {
          entry = &candidate;
          break;
        }
      }
      if (entry == nullptr) {
        continue;
      }
      if (!entry->has_region) {
        entry->xmin = drained.region.xmin;
        entry->xmax = drained.region.xmax;
        entry->ymin = drained.region.ymin;
        entry->ymax = drained.region.ymax;
        entry->has_region = true;
      }
      else {
        /* The subscription keeps four loose bounds; the union is made once, on an #rcti, so it
         * cannot disagree with the drain's own union above. */
        rcti region = {entry->xmin, entry->xmax, entry->ymin, entry->ymax};
        BLI_rcti_union(&region, &drained.region);
        entry->xmin = region.xmin;
        entry->xmax = region.xmax;
        entry->ymin = region.ymin;
        entry->ymax = region.ymax;
      }
      entry->changed = true;
      any = true;
      newly_changed |= !drained.was_changed;
    }
  }
  if (any) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_BAKE_STALE;
  }
  if (newly_changed) {
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }
}

}  // namespace blender
