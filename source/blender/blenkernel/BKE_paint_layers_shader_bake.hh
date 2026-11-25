/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The shader-bake: a node tree rendered over the map as pixels, the CPU-composite answer for the
 * node-group layer and generator kinds. (A node-group filter is the effect cache's kernel: it is
 * computed through the same backend, handing its own content as the request's input, and keeps its
 * answer in the effect cache.) A shader node graph is a per-pixel function; compiled to a material
 * and evaluated over the UV it is exactly a map, and the composite, the bake and the Image Editor's
 * CPU preview read that map like any other.
 *
 * The GPU context and the material compiler live with the editors and the renderers, so this
 * module holds only the pure half -- the cache, the keys, the staleness -- and asks a registered
 * backend for the pixels, the way the GPU effect cache holds the keys and hands the textures to
 * hooks. A build that registers no backend answers null, and the rows of the node-group kinds
 * take no part (the consumers skip a null map as they skip a map not yet baked).
 *
 * The keys are the definition's identity folded with the row's values: the interface hash (what
 * the tree exposes), the packed parameters (what the row stores) and the input's own hash, so an
 * edit of the tree, of the row's parameters or of what feeds the tree invalidates exactly the
 * entries that read it. A stale entry is still served -- the hash is the invalidation's question,
 * not the reader's, the way the source graph's cache and the bake maps work.
 */

#include <cstdint>

#include "BLI_span.hh"

#include "BKE_paint_layers.hh"

#include "DNA_image_types.h"

namespace blender {

struct bNodeTree;
struct ColorBand;
struct IDProperty;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct PaintLayerTileContext;

/* -------------------------------------------------------------------- */
/** \name The backend hooks
 * \{ */

/**
 * One render the backend is asked for. The tree is the definition; the parameters are the row's
 * stored schema values, keyed by the dynamic schema's names (the group instance binds them to its
 * inputs); the input, when given, is the content the filter reads, already at the canvas size,
 * straight RGBA -- the backend binds it as the `Input` texture. The answers are the RGBA float
 * maps at \a width x \a height, straight (unassociated) alpha, one row of the map per scanline:
 * one per outputs entry, each naming the paint channel it fills (or -1 for the one-answer
 * contract, where every channel reads the same map).
 *
 * The one exception to the straight-alpha answer is \a result_has_alpha: the caller's kind lays a
 * layer with an alpha of its own (#PAINT_LAYER_KIND_RESULT_HAS_ALPHA, the Drop Shadow's contract),
 * so the backend wires the tree's `Alpha` output and hands the answer back premultiplied -- the
 * colour times the alpha it covers by, in the fourth channel. A source or a generator ask carries
 * the flag clear and the answer stays exactly as it always was.
 */
struct PaintLayerShaderBakeRequest {
  const bNodeTree *tree = nullptr;
  /** The row's `system_properties` group: one property per dynamic-schema parameter, named after
   * the socket identifier, or null when the kind carries none. The backend binds each to the
   * group instance input with the same name. */
  const IDProperty *parameters = nullptr;
  /** The content the filter reads as `Input`, as RGBA floats, or null. */
  const float *input_pixels = nullptr;
  int width = 0;
  int height = 0;
  /** The UDIM tile the map covers, for a backend that reads the tile. */
  int tile_number = 0;
  /**
   * The ask is a filter whose kind's result is a layer with an alpha of its own: wire the tree's
   * `Alpha` output and return the answer premultiplied by it. The kernel sets it from the kind
   * flag, and only for a content ask -- a mask item's grey carries no alpha, so its answer is
   * never premultiplied.
   */
  bool result_has_alpha = false;
  /**
   * The ask is a node-group generator's pattern emission (#MA_PAINT_LAYER_SOURCE_GENERATOR with the
   * #MA_PAINT_LAYER_GENERATOR_NODE_GROUP subtype): the backend builds the placement chain the
   * graph builds -- the space's own texture-coordinate output, turned, scaled and moved by the
   * placement parameters of #parameters, shifted by the seed -- feeds it to the tree's
   * `Coordinate` input, and lays each output's ramp tail over the tree's `Fac`. The compute sets
   * it; every other ask keeps it clear and binds the tree the way it always has.
   */
  bool generator = false;
};

/**
 * One channel answer a backend fills: the paint channel, and the RGBA float map at the request's
 * size. A channel the backend cannot answer (nothing on the Principled to feed) is filled
 * transparent, so a reader never lays another channel's pixels by mistake.
 *
 * On a generator ask the ramp context rides beside the pixels: the band the channel's tail reads
 * (the row's own band for a colour channel, the record's value ramp for a scalar one) and the two
 * ends a null band falls back to, both in the channel's stored scale -- the same answer the CPU
 * composite's generator payload carries. A null #ramp is the linear pass: the raw `Fac` taken
 * from 0 at #scalar_start to 1 at #scalar_end, laid as a grey -- the fallback
 * #BKE_paint_material_generator_sample_coordinate evaluates when a channel has no band. Every ask
 * but a generator one carries a null ramp and the identity ends: their answers are raw colours,
 * and the backend wires the tree's own output straight through.
 */
struct PaintLayerShaderBakeOutput {
  int channel = -1;
  float *pixels = nullptr;
  /** The channel's band, owned by the row for the length of the render, or null for the linear
   * pass. Meaningless when the ask is not a generator one; the backend ignores it there. */
  const ColorBand *ramp = nullptr;
  /** The channel's encoded ends, the null-#ramp fallback's range. Stored scale, like the ramp's
   * own grey stops: the describe encodes the record's ends once, and no reader re-encodes. */
  float scalar_start = 0.0f;
  float scalar_end = 1.0f;
};

/**
 * The backend: render \a request into \a outputs. False when it cannot (no GPU context, a tree
 * it refuses); the cache keeps the entry then and the ask is retried next time. One call answers
 * every output the caller names -- a source tree with per-channel outputs is one pass for all of
 * them, the way the bake renders every requested channel together.
 */
using PaintLayerShaderBakeRender = bool (*)(const PaintLayerShaderBakeRequest &request,
                                            MutableSpan<PaintLayerShaderBakeOutput> outputs);

/** The hooks the editors register once, before the first ask. */
struct PaintLayerShaderBakeHooks {
  PaintLayerShaderBakeRender render = nullptr;
  /**
   * The main the cache creates its generated images in. The cache owns session-local images --
   * datablocks -- and only the editors know the main at the moment an ask lands. Null main means
   * no backend: the cache answers nothing.
   */
  Main *main = nullptr;
};

/** Install (or clear, with a default) the backend. Not thread-safe against an ask; call at startup. */
void BKE_paint_layers_shader_bake_hooks_set(const PaintLayerShaderBakeHooks &hooks);

/** The installed hooks, or null before the first registration. The kernels and the effect cache
 * read them without owning the install. */
const PaintLayerShaderBakeHooks *BKE_paint_layers_shader_bake_hooks_get();

/**
 * The main the hooks registered, for the callers that must create or free the cache's images
 * themselves. Null until a backend registers.
 */
Main *BKE_paint_layers_shader_bake_main();

/** \} */

/* -------------------------------------------------------------------- */
/** \name The cache
 * \{ */

/**
 * The cached answer for one row's channel on one tile, or null when no backend is registered or
 * the kind reads nothing a bake could answer. The image is generated and float-buffered; it may
 * be empty (never rendered yet) or stale (the definition or the values moved since the last
 * render) -- the reader lays what is there and the due-checks bring the render.
 *
 * The image is owned by the cache for the session: it is not saved with the file, so a reload
 * re-bakes. The reader must not free it; #BKE_paint_layers_shader_bake_free_material and
 * #BKE_paint_layers_shader_bake_free_all do.
 */
Image *BKE_paint_layers_shader_bake_image_get(Main &bmain,
                                             const Material &ma,
                                             const MaterialPaintLayer &row,
                                             int channel,
                                             const PaintLayerTileContext &tile,
                                             int width,
                                             int height);

/**
 * Whether \a row's cache on \a tile is missing or stale -- a render is due. A row whose
 * definition is gone answers false: there is nothing to render.
 */
bool BKE_paint_layers_shader_bake_due(const Material &ma,
                                      const MaterialPaintLayer &row,
                                      const PaintLayerTileContext &tile,
                                      int width,
                                      int height);

/**
 * Render what is due on \a tile, through the hooks, and land it in the cache. The caller owns the
 * scheduling (the editors debounce); the composite never calls this, it only reads. False when no
 * backend is registered or the render failed; the stale entry stands either way.
 */
bool BKE_paint_layers_shader_bake_compute(Main &bmain,
                                          const Material &ma,
                                          const MaterialPaintLayer &row,
                                          const PaintLayerTileContext &tile,
                                          int width,
                                          int height);

/** Drop every cache entry the rows of \a ma carry. Called with the other material frees. */
void BKE_paint_layers_shader_bake_free_material(uint32_t material_session_uid);

/** Drop every cache entry. Called when the file closes. */
void BKE_paint_layers_shader_bake_free_all();

/** \} */

}  // namespace blender
