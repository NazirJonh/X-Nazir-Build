/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Internals of the bake (`paint_layers_bake.cc`, `paint_layers_bake_job.cc`,
 * `paint_layers_bake_material_rows.cc`) and of the bake side of a
 * CPU-computed mask effect (`paint_layers_bake_mask_effect.cc`), shared between them. They stay out of
 * `paint_layers_intern.hh` because nothing beside the bake reads them: the planner's branches are
 * their only callers.
 */

#pragma once

#include <memory>
#include <mutex>

#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "BLI_uuid.h"

#include "BKE_paint_layers.hh"

#include "BKE_image_partial_update.hh"

#include "DNA_material_types.h"

#include "IMB_imbuf_types.hh"

#include "paint_layers_intern.hh"

namespace blender {

struct Image;
struct PaintMaterialCompositeSurface;
struct Scene;
struct ViewLayer;
struct Main;
struct MaterialPaintChannelInfo;

/**
 * The map of \a layer in \a channel at \a size, made when it is missing or of another size or
 * format.
 *
 * \param floatbuf: keep the values as float. A map of the wrong format cannot be reused: the pixel
 *                  writers index one buffer or the other, and a byte map read as float (or back)
 *                  would be a different picture.
 */
Image *bake_service_image(Main &bmain,
                          MaterialPaintLayer &layer,
                          int channel,
                          int size,
                          bool is_color,
                          bool floatbuf,
                          int object_num);

/** The map \a channel's bake stores as float rather than bytes; see #bake_service_image. */
bool bake_channel_needs_float(const MaterialPaintLayer &layer,
                              const MaterialPaintChannelInfo &info);

/**
 * Release the maps of \a layer's bake in the channels a render just left out. A channel the row no
 * longer takes part in (switched off, or a record the row no longer lays) is not rendered again, so
 * its map from an earlier bake would otherwise stand in for the row and show what the row stopped
 * laying. The caller tags #MA_PAINT_LAYERS_REGEN, as for #bake_maps_release.
 */
void bake_stale_channel_maps_release(Main &bmain,
                                     MaterialPaintLayer &layer,
                                     const Span<int> written_channels);

/**
 * Whether the row's bake subscription reports pending pixel edits. Unlike
 * #BKE_paint_layers_bake_changed_region, a full-update mark counts too: it sets
 * #BakeLayerSubscription::changed with no rectangle (#BKE_paint_layers_bake_notice_changes).
 */
bool bake_row_subscription_changed(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether a mask effect item waits for the running stroke to end before its re-bake: the same
 * policy a row's content effect runs on, with the grey's map standing where the row's maps do.
 */
bool mask_effect_deferred_by_stroke(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Append the maps \a row itself reads to \a r_images: its channel maps, the maps of its bake and,
 * for a row that reads a material, the textures that material shows live. A reader also brings the
 * maps of the slice it reads (see #bake_collect_slice_images).
 */
void bake_collect_row_maps(const Material &ma,
                           const MaterialPaintLayer &row,
                           blender::Vector<Image *> &r_images,
                           blender::Vector<bUUID> &visited);

/**
 * The structural hash of what the mask effect item \a layer's grey reads: the items before it and
 * what they render from. Zero when the item is in no mask stack yet, so there is no input to hash;
 * the caller folds the value in only then, keeping the mix step where the other hashes live.
 */
uint64_t bake_mask_effect_input_hash(const Material &ma, const MaterialPaintLayer &layer);

/**
 * The side \a row's grey lays on: the mask's canvas, which the base mask's map (or the material
 * canvas) decides. Zero when no map of the owner has dimensions yet.
 */
int bake_mask_effect_size_for(const Material &ma, const MaterialPaintLayer &row);

/**
 * As #bake_write_image, for a source sampled as it is written: \a fill_row is called per row with
 * the row's \a y and the buffer's width, and fills \a width straight RGBA floats into \a r_row. A
 * row scratch is all the source holds -- a map's width wide -- so a source that already exists as
 * a buffer of its own proportions (the mask effect's grey) never needs a second copy of the whole
 * picture to land in the map. The store path -- the buffer choice, the encoding, the premultiply
 * and the changed mark -- is the flat writer's own, so a row source and a flat one land in the map
 * alike.
 */
void bake_write_image_rows(
    Image &image,
    const bool is_color,
    const int *rect,
    const int tile_number,
    const FunctionRef<void(const int y, const int width, float *r_row)> fill_row);

/**
 * Render the CPU-computed mask effect's grey of \a layer (a mask item) into its own bake: the grey
 * the mask effect cache holds, resampled from the row's canvas to the \a size-square map, written
 * whole. True when the map was written.
 */
bool bake_mask_effect_row_write(Main &bmain, Material &ma, MaterialPaintLayer &layer, int size);


/* -------------------------------------------------------------------- */
/** \name Runtime
 *
 * The per-material state `paint_layers_bake.cc` owns (see #bake_runtime).
 * \{ */

struct BakeSubscriptionSource {
  Image *image = nullptr;
  PartialUpdateUser *user = nullptr;
};

struct BakeLayerSubscription {
  bUUID marker = {};
  bool changed = false;
  /** Union of the changed rectangles, valid while #has_region; false means a full re-bake. */
  bool has_region = false;
  int xmin = 0, xmax = 0, ymin = 0, ymax = 0;
  Vector<BakeSubscriptionSource> sources;
};

/** The hashes a Material row's bake was started from and did not land from: not retried as is. */
struct BakeFailure {
  uint32_t hash[2] = {};
  uint32_t source[2] = {};
  /** The render that failed was a preview; the render at the row's own size is another one. */
  bool preview = false;
};

struct BakeMaterialRuntime {
  /**
   * The material that owns this entry. Used so the free path of a same-`session_uid` material (the
   * old ID during a memfile undo) does not delete the entry the re-read ID still uses.
   */
  const Material *owner = nullptr;
  Vector<BakeLayerSubscription> layers;
  /**
   * The bake size of a row whose bake structure the AUTO rules released: the structure is the only
   * copy of the size, but the size is the user's choice rather than part of the cache, so the row's
   * next bake takes it from here instead of the content dimensions.
   */
  Map<UUID, int> dropped_size;
  /**
   * Material rows whose last bake was cancelled or failed, by marker. A render that cannot finish
   * would otherwise be started again by every scene update; the row waits for something to change
   * (the source, the row) or for an explicit retry.
   */
  Map<UUID, BakeFailure> failed;
  /**
   * The shape each Material row last gave its group (#paint_layer_material_live_signature), by
   * marker: what the scene update compares against to know a source edit changed what the
   * generator builds.
   */
  Map<UUID, uint64_t> live_signature;
  /**
   * What the Normal read in the source when the last landed render of each Material row started, by
   * marker: a source edit that leaves it alone is no reason to render when only the Normal is.
   * Runtime only; a row read from a file renders once on its first edit.
   */
  Map<UUID, uint64_t> normal_hash;
  /**
   * Material rows whose last landed render was partial (#bake_row_maps_partial): the maps of the
   * channels the shader shows live were not rendered, so what reads them (an Image Editor) needs a
   * full render before it can trust them.
   */
  Set<UUID> partial_rows;
  /** What a started render recorded about itself, by the full source hash it started from. */
  struct StartedRender {
    uint64_t normal_hash = 0;
    bool partial = false;
  };
  Map<uint64_t, StartedRender> started;
  /**
   * The surface identity each Object/Generated row was rendered from, by row marker
   * (#BKE_paint_layers_gradient_surface_identity). An edit of the geometry or of the UV layer
   * moves it and the row's bake is stale without any description change; a moved object does not,
   * since object space is the object's own.
   */
  Map<UUID, uint64_t> object_identity;
  /**
   * Why an Object/Generated row cannot be baked right now, by row marker
   * (#ePaintLayersBakeObjectIssue). What the Bake panel shows beside a row that stays live.
   */
  Map<UUID, int> object_issue;
};

/**
 * Seconds without an edit of the source before the active row is rendered for a channel that no
 * live form stands in for (the Normal of a Bump). Counted from the last edit, so dragging a value
 * that moves it renders nothing until the hand stops.
 */
constexpr float MAP_ONLY_EDIT_DELAY_SECONDS = 3.0f;

/** Whether \a layer is a row \a ma keeps a bake for (a stack row, a correction or a mask item). */
bool bake_row_owned(Material &ma, const MaterialPaintLayer &layer);

/** The bake runtime of every material, by #ID::session_uid. Hold #bake_runtime_mutex around it. */
Map<uint32_t, BakeMaterialRuntime> &bake_runtime();
/** Guards #bake_runtime; see its definition for the rules. */
std::mutex &bake_runtime_mutex();

/** \} */

/* -------------------------------------------------------------------- */
/** \name Planner helpers shared with the worker job and the pixel writers
 * \{ */

MaterialPaintLayerBakeTile *bake_tile_ensure(MaterialPaintLayerBake &bake,
                                             uint32_t object_uid,
                                             int index);
void bake_tile_object_indices_set(const Material &ma, const PaintLayersBakeTargets &targets);
void bake_tile_object_indices_clear(const Material &ma,
                                    const Scene *scene,
                                    const ViewLayer *view_layer,
                                    const PaintLayersBakeView *view);
void bake_pixel_store_float(const float rgba[4], float r_store[4]);
void bake_pixel_store_byte(const float rgba[4],
                           bool is_color,
                           const ColorSpace *colorspace,
                           uchar r_store[4]);
void bake_write_stored_image(Image &image,
                             bool stored_float,
                             const void *stored,
                             const int *rect,
                             int tile_number = 0);
void bake_image_pixels_changed(Image &image, ImBuf &ibuf);
void bake_object_identity_record(const Material &ma,
                                 const MaterialPaintLayer &layer,
                                 uint64_t identity);
bool bake_row_is_hidden(const Material &ma, const MaterialPaintLayer &layer);
bool bake_row_release_apply(Main &bmain,
                            Material &ma,
                            MaterialPaintLayer &layer,
                            BakeRowRelease release);
int bake_size_for(const Material &ma, const MaterialPaintLayer &row);
int bake_row_object_issue(const Material &ma,
                          const MaterialPaintLayer &layer,
                          const PaintLayersBakeView &view);
void bake_object_identities_notice(Material &ma, const PaintLayersBakeView &view);
bool bake_row_is_candidate(const Material &ma, const MaterialPaintLayer &layer, int object_issue);
void bake_gradient_row_report(const MaterialPaintLayer &layer,
                              int size,
                              bool sync,
                              double seconds,
                              Span<int> channels,
                              double delta_mib,
                              int retained_results,
                              double retained_mib);
bool bake_any_row_needs_surface(const Material &ma);

/**
 * The surface an Object or Generated bake of \a layer renders through, or null when its subtree
 * reads no object geometry at all.
 */
std::shared_ptr<const PaintMaterialCompositeSurface> bake_surface_build(
    const MaterialPaintLayer &layer, const PaintLayersBakeTarget &target, int size);

/** \} */

}  // namespace blender
