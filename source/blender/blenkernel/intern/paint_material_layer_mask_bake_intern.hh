/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * The dead-branch anchor of a baked layer mask-correction chain: how one is created, found and
 * removed, and how a reader tells a baked coverage input from a live one.
 *
 * A "layer mask correction chain" is the stack of Mix nodes hanging on a layer row's coverage input
 * (`factor_coverage`, else `factor`). Baking it into one Image means the GPU shader only samples
 * the baked result; the live nodes stay in the tree, linked to an anchor node whose own output goes
 * nowhere, so `gpu_node_graph_prune_unused` drops the anchor and, with it, the whole live chain.
 * The anchor's id-properties are what lets a later reader find that chain again.
 */

#include <cstdio>

#include "BLI_uuid.h"

/*
 * Temporary diagnostics for the mask-bake anchor/live-chain paths. Set to 0 to silence. Each line
 * flushes stderr so the last one printed before a crash names the path that faulted.
 */
#ifndef PBR_MASK_BAKE_TRACE
#  define PBR_MASK_BAKE_TRACE 0
#endif

#if PBR_MASK_BAKE_TRACE
#  define MASK_BAKE_TRACE(...) \
    do { \
      std::fprintf(stderr, "[maskbake] " __VA_ARGS__); \
      std::fflush(stderr); \
    } while (0)
#else
#  define MASK_BAKE_TRACE(...) ((void)0)
#endif

namespace blender {

struct ChainLayer;
struct Image;
struct Main;
struct Material;
struct bNode;
struct bNodeSocket;
struct bNodeTree;

/** Boolean id-property stamped on an anchor node. */
inline constexpr const char *MASK_BAKE_ANCHOR_PROP = "pbr_mask_bake";
/** Int id-property stamped on an anchor node: the channel the anchor belongs to. */
inline constexpr const char *MASK_BAKE_CHANNEL_PROP = "pbr_mask_bake_channel";

/** The nodes and image one baked-mask anchor owns. */
struct MaskBakeAnchor {
  bNode *node = nullptr;             /* The anchor Mix node. */
  bNodeSocket *live_input = nullptr; /* Its input socket holding the live chain top. */
  Image *image = nullptr;            /* The baked mask B. */
  bNode *image_node = nullptr;       /* The Image Texture reading B. */
};

/**
 * Read the anchor of (\a row_marker, \a channel) in \a tree, or false.
 *
 * The tree's topology cache is made current here. Lenient about a half-built anchor: whatever is
 * resolvable is returned, and a matching node is enough for true. False only when no node carries
 * the anchor properties for this row and channel.
 */
bool mask_bake_anchor_read(const bNodeTree &tree,
                           const bUUID &row_marker,
                           int channel,
                           MaskBakeAnchor &r_anchor);

/**
 * Create (or return the existing) anchor, B image and B texture for (\a row_marker, \a channel),
 * wiring them into \a coverage_socket.
 *
 * The top of the live chain currently feeding \a coverage_socket is moved onto the anchor's input,
 * and the anchor's output is left unconnected: the anchor and everything feeding only it are the
 * dead branch the GPU prunes. B's Color output then feeds \a coverage_socket, so the shader samples
 * the baked result alone. B carries the internal baked-mask role, so the stack model and the paint
 * canvas never mistake it for a correction's map. The B texture is also linked into the anchor's
 * second input purely as the anchor's reference to its image; #mask_bake_anchor_read finds B
 * through it.
 */
bool mask_bake_anchor_create(Main &bmain,
                             bNodeTree &tree,
                             const bNodeSocket &coverage_socket,
                             const bUUID &row_marker,
                             int channel,
                             int width,
                             int height,
                             MaskBakeAnchor &r_anchor);

/**
 * The size a fresh B for \a layer's row \a row_marker should get, in \a tree: its own mask image
 * when it has one, else its own map image, else the conventional 1024. A mask correction shapes the
 * mask, so the bake has to fit whatever the mask paints at; the map is the next best authority, and
 * a row with neither has no pixels to size against.
 *
 * #mask_bake_anchor_create compares this against an existing B on every sync, so a resize of the
 * row's map is followed rather than leaving B at a size the evaluator refuses.
 */
void mask_bake_size_get(bNodeTree &tree,
                        const ChainLayer &layer,
                        const bUUID &row_marker,
                        int &r_width,
                        int &r_height);

/**
 * Remove the anchor, B texture and B image of (\a row_marker, \a channel). B is freed only when it
 * is a generated image nothing else uses; an image still referenced stays.
 *
 * The live chain is deliberately left in the tree, but unlinked: the caller must re-link coverage
 * to it (or rebuild the chain) after this, since nothing else points coverage at anything now.
 */
void mask_bake_anchor_remove(Main &bmain, bNodeTree &tree, const bUUID &row_marker, int channel);

/**
 * Whether \a coverage_socket is overridden by a baked-mask texture; if so return B and its node.
 *
 * The socket's tree topology cache must be current. Lenient: multiple links, a non-image source or
 * a foreign image all read as not baked.
 */
bool mask_bake_coverage_is_baked(const bNodeSocket &coverage_socket,
                                 Image *&r_image,
                                 bNode *&r_image_node);

/**
 * Find the anchor whose B reference is the image texture \a coverage_socket reads, or false.
 *
 * The readers that walk a channel only have the coverage socket, not the row marker and channel
 * #mask_bake_anchor_read keys on; the anchor's own B input names the image coverage now reads, and
 * that is enough to find it. False when coverage is not baked or no anchor references its image.
 *
 * \a r_anchor carries whatever is resolvable, like #mask_bake_anchor_read. Makes the socket's tree
 * topology cache current, then scans the tree for the anchor (O(nodes)).
 */
bool mask_bake_anchor_from_coverage(const bNodeSocket &coverage_socket, MaskBakeAnchor &r_anchor);

/**
 * Undo #mask_bake_anchor_create for \a layer in \a channel: remove the anchor and point the row's
 * coverage back at the live chain the bake parked on it. No-op when the row is not baked.
 *
 * The mutators that carry a row's nodes elsewhere need the canonical (unbaked) graph: an anchor
 * and its B are not part of #layer_owned_nodes_collect, so a baked row would be copied without its
 * coverage. After the operation a later sync bakes the destination row again.
 */
void mask_bake_unbake_row(Main &bmain, ChainLayer &layer, int channel);

/**
 * The live top socket a reader should walk: #MaskBakeAnchor::live_input when coverage is baked and
 * an anchor resolves, else \a coverage_socket itself. Lenient: returns \a coverage_socket when no
 * valid anchor exists.
 *
 * Makes the socket's tree topology cache current, then scans the tree for the anchor (O(nodes)).
 */
const bNodeSocket *mask_bake_live_top_socket(const bNodeSocket &coverage_socket);

/**
 * The socket a mutator should treat as the row's coverage top: the anchor's live input when the
 * row is baked, else the layer Mix's factor_coverage/factor. Null when the row has no coverage.
 */
bNodeSocket *paint_layer_live_coverage_socket(ChainLayer &layer);

/**
 * Recompute every baked mask B of \a ma whose row has an anchor, so the shader samples current
 * pixels.
 *
 * For each anchor found in the material's own tree and the group trees its stack reaches, the
 * row's channel stack is collected, B's buffer is acquired and
 * #BKE_paint_material_composite_eval_row_mask writes the row's mask factor into it. B is marked
 * dirty and its GPU textures are freed so the next draw re-uploads.
 *
 * The work is skipped when the session cache (see `BKE_paint_material_mask_bake.hh`) finds the
 * row's graph, size and source pixels unchanged since the last call; only the dirty rows are
 * recomputed, and a pixel edit refreshes just the rectangle it was reported in.
 *
 * \param force_full: rebuild every B, dirty or not.
 * \return true when at least one B was refreshed or there was nothing dirty; false when the
 *         material has no node tree or anchors existed that had to be written but none could be.
 */
bool BKE_paint_material_mask_bake_ensure(Main &bmain, Material &ma, bool force_full);

}  // namespace blender
