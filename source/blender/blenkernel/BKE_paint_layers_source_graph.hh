/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * What "source" means here, and what does not:
 *
 * - The **source graph** is the material's own node graph, kept by a conversion
 *   (#MA_PAINT_LAYERS_SOURCE_GRAPH): the stack composites over the graph's channels, which reach it
 *   through the generated tree's `Source <channel>` interface inputs -- one per wired channel, the
 *   links that fed the Principled re-aimed onto them. The graph's nodes are never edited by the
 *   generator.
 * - The **source-graph cache** is the CPU side of that: a map per fed channel plus a claim hash
 *   (#Material::paint_layers_source_hash), baked from the graph and read by the composite, the
 *   channel view and the exports.
 * - A **source Material row** is a different kind of source: a row whose images are rendered from
 *   another material entirely. Its content hashes and resolve (#BKE_paint_layers_source_material_tree_hash,
 *   #BKE_paint_layers_source_resolve) stay in #BKE_paint_layers.hh -- the declarations here are the
 *   conversion's, not the row's.
 *
 * The mode itself (#BKE_paint_layers_source_graph_get / #BKE_paint_layers_source_graph_set) also
 * stays there, beside the stack flags it belongs to.
 */

#include <cstdint>

#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_paint_layers.hh"

#include "DNA_uuid_types.h"

namespace blender {

struct Image;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct bNodeSocket;
enum eMaterialPaintChannel : int8_t;

/* -------------------------------------------------------------------- */
/** \name The source-graph cache
 * \{ */

/** The side at or above which the source-graph cache is capped, whatever the stack's map size. */
constexpr int PAINT_LAYERS_SOURCE_CACHE_MAX_SIZE = 2048;

/**
 * The CPU-composite cache image of \a ma's own graph for \a channel, or null.
 *
 * Only a source-graph material (#MA_PAINT_LAYERS_SOURCE_GRAPH) carries one, and only for the
 * channels its graph feeds -- never Normal or Height, whose author chains stay on the Principled,
 * and never a channel without a Principled socket. The image is the one CPU readers of the chain's
 * bottom go through: fresh or stale alike, since stale pixels are the better answer than a chain
 * that lost its graph (see #BKE_paint_layers_source_channel_graph_hash for the validity key). Null
 * means "no cache": the caller starts at the channel's neutral constant, exactly as a plain stack
 * does. The image is read-only -- it is never painted into and never shown as a stack row.
 */
Image *BKE_paint_layers_source_cache_image_get(const Material &ma, int channel);

/**
 * Re-derive the state of \a ma's source-graph cache from its stored hash: the graph's live channel
 * hash (#BKE_paint_layers_source_channel_graph_hash) no longer matching marks the cache stale
 * (#MA_PAINT_LAYERS_SOURCE_STALE). Returns whether \a ma was marked. The mark is runtime-only and
 * cleared on a file read, so a load re-derives it through
 * #BKE_paint_layers_source_cache_validate_all; the editor's material update asks here too.
 *
 * \param check_unpacked: the load-time check: a cache image is generated with no packed file by
 *                        design (the cache is never packed), so its pixels did not survive the
 *                        load -- the material's reference is released (#id_us_min, the image goes
 *                        to the next orphan purge), the stored hash drops to zero and the cache is
 *                        marked stale for a re-bake. A runtime call leaves this off: a freshly
 *                        baked image is unpacked too, and its pixels are in memory.
 */
bool BKE_paint_layers_source_cache_validate(Material &ma, bool check_unpacked = false);

/** #BKE_paint_layers_source_cache_validate for every material of \a bmain, the load-time way. */
void BKE_paint_layers_source_cache_validate_all(Main &bmain);

/** How a landed cache bake left the material's cache (#BKE_paint_layers_source_cache_land_apply). */
enum class PaintLayersSourceCacheLandState : int8_t {
  /** Every requested channel landed; the claim now names the baked graph and reads fresh. */
  Fresh = 0,
  /** Every requested channel landed, but the graph moved while the bake ran: the claim is stamped
   * and the validation reads stale for the newer graph. */
  Stale = 1,
  /** At least one requested channel produced no buffer: the claim is not stamped, so the cache
   * stays stale until a bake lands all of it. */
  Partial = 2,
};

/** One channel a landed cache bake carries: the channel and the image that now holds its map. */
struct PaintLayersSourceCacheLandedChannel {
  eMaterialPaintChannel channel;
  Image *image;
};

/**
 * Apply a landed source-graph cache bake to \a ma: every landed channel's map becomes the
 * material's claim (the map it replaces loses its user), and -- only when \a landed carries every
 * channel of \a requested -- the claim becomes \a hash, the one the bake started from, with the
 * validation re-deriving the state from it. A partial landing never stamps: the claim would name a
 * render the graph's channels do not all have, and the validation, which reads the whole graph,
 * would clear the stale mark over channels whose pixels never arrived.
 *
 * \return the state the landing left the cache in.
 */
PaintLayersSourceCacheLandState BKE_paint_layers_source_cache_land_apply(
    Material &ma,
    uint64_t hash,
    Span<eMaterialPaintChannel> requested,
    Span<PaintLayersSourceCacheLandedChannel> landed);

/**
 * Whether \a ma's source-graph cache is due for a bake: the cache is stale, the graph feeds at
 * least one linked channel, and a bake has not already failed to land at the graph as it is now
 * (#BKE_paint_layers_source_cache_failure_remember). The auto path (the debounce tick, the scene
 * sweep, a landing that re-arms) asks with \a manual false; the manual
 * #MATERIAL_OT_source_cache_update asks with \a manual true, so the user asking again is what
 * gets past the failure.
 */
bool BKE_paint_layers_source_cache_bake_due(const Material &ma, bool manual);

/**
 * Remember that a bake of \a ma's cache failed to land at the graph hash \a hash, so the auto path
 * does not retry the same graph on its own (#BKE_paint_layers_source_cache_bake_due). Runtime
 * only, main thread only; a graph that moves on makes the memory moot by no longer matching.
 */
void BKE_paint_layers_source_cache_failure_remember(const Material &ma, uint64_t hash);

/**
 * Drop the failure memory of \a ma, called when the material is freed: a leaked entry would both
 * hold its hash forever and answer a session uid a later material arrived with. Runtime only,
 * main thread only.
 */
void BKE_paint_layers_source_cache_failure_forget(const Material &ma);

/** Drop every remembered failure and release the map buffer. Main thread only. */
void BKE_paint_layers_source_cache_failure_free_all();

/** Whether the graph as it is now is the one a cache bake already failed to land at. */
bool BKE_paint_layers_source_cache_failure_blocks(const Material &ma);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Capture, convert, release and revert of a material's own graph
 * \{ */
/**
 * Remember the links that currently feed \a ma's Principled inputs, so a later
 * #BKE_paint_layers_source_graph_release can put them back. Called before a conversion takes the
 * links over; the record lives in the material's node tree as ID properties, so a revert survives
 * save and load. A material whose Principled is reachable only through a node group stores an empty
 * record: the conversion refuses such graphs rather than editing a group definition the instance
 * does not own.
 */
void BKE_paint_layers_source_graph_capture(Material &ma);

/** Forget the record #BKE_paint_layers_source_graph_capture made, if any. */
void BKE_paint_layers_source_graph_capture_clear(Material &ma);

/**
 * Whether \a ma has a graph a revert can hand back: a source-graph material, or a converted one
 * whose original links were recorded by #BKE_paint_layers_source_graph_capture`.
 */
bool BKE_paint_layers_source_graph_can_release(const Material &ma);

/** Non-fatal notes a revert to a plain material produced. */
struct PaintLayersRevertReport {
  /**
   * A recorded Principled link whose source node or socket no longer exists (renamed or deleted
   * while the stack owned the input): the input fell back to its stored default value, which is
   * what the graph left on it -- a conversion only ever moves links, never socket values. Each
   * entry is the channel whose input the record could not restore; naming it in the interface
   * language is the caller's, so the kernel keeps the translation out of the data.
   */
  Vector<eMaterialPaintChannel> lost_channels;
};

/**
 * Put the material's own graph back: the inputs the stack owned -- its instance's results and its
 * minted Normal Map and Bump helpers -- get the recorded links back, every other Principled
 * connection (including one the user added after the conversion) is left as it stands,
 * #MA_PAINT_LAYERS_SOURCE_GRAPH is cleared and the material is tagged for regeneration. The
 * generated nodes are left in place; the caller that removes the stack also removes the instance.
 * Nothing of the author's graph is deleted.
 *
 * A recorded link whose source node or socket is gone restores the input's stored default value and
 * reports the channel in \a r_report.
 */
void BKE_paint_layers_source_graph_release(Material &ma, PaintLayersRevertReport *r_report = nullptr);

/** Non-fatal notes a conversion from a graph produced. */
struct PaintLayersConvertReport {
  /**
   * The marker of the row the stack starts from: the base row the conversion added, or the first
   * of the rows a re-conversion layers over. The caller makes it active and ensures its maps.
   */
  bUUID base_layer_marker;
  /**
   * Channels of the set the graph still feeds directly: the author's Normal chain (and the Height
   * riding it) stays on the Principled, and the stack's row for the channel has no route into the
   * material until the user removes that chain.
   */
  Vector<eMaterialPaintChannel> kept_channels;
  /** Whether the graph's channel links were re-aimed onto the stack's `Source <channel>` inputs. */
  bool adopted_source_links = false;
  /** Whether the generated tree could not be built in this pass; the next tagged update builds it. */
  bool regenerate_failed = false;
};

/**
 * Turn \a ma into a layered material that composites over its own node graph, the whole convert an
 * operator needs: the channel set is written, #MA_PAINT_LAYERS_SOURCE_GRAPH is set and the links
 * that feed the Principled are recorded (#BKE_paint_layers_source_graph_capture), the base row is
 * added -- or, when rows already sleep on the material from an earlier conversion, the first one is
 * re-layered instead -- and the stack is regenerated. The graph's nodes are never edited.
 *
 * \return false when \a ma cannot be edited here, the set is empty, or the base row could not be
 * added; the set, the mode and the record are rolled back then, so a refused convert leaves the
 * material exactly as it was found.
 */
bool BKE_paint_layers_convert_from_graph(Main &bmain,
                                         Material &ma,
                                         PaintChannelMask channel_mask,
                                         PaintLayersConvertReport *r_report = nullptr);

/** Non-fatal notes a conversion from a bake produced. */
struct PaintLayersConvertBakeReport {
  /** What rolled the conversion back; #Failure::None on success. */
  enum class Failure : int8_t {
    None = 0,
    /** The material cannot be edited here (linked, override or without a node tree). */
    NotEditable,
    /** The baked channels could not form a channel set (#BKE_paint_layers_channel_set_mask_set). */
    ChannelSet,
    /** The Fill row could not be added to the stack. */
    FillAdd,
    /** A baked map could not be linked into the Fill row. */
    MapLink,
  };
  /** The marker of the Fill row the maps went into, added or reused; the caller makes it active. */
  bUUID fill_marker;
  /** Why the conversion rolled back, when it returns false. */
  Failure failure = Failure::None;
};

/**
 * Flatten the maps a bake just rendered into the stack, the whole fill an operator needs: the
 * channel set is written, the material's own map size is set, the bake's link back to the material
 * is cleared off every map, and the maps go into a Fill row -- a new one when the stack has none
 * of the conversion's (marked #MA_PAINT_LAYER_CONVERSION_FILL, named after the material), or the
 * row a previous conversion's bake filled, re-aligned to the channels of this bake. The bake's
 * Principled links were recorded (#BKE_paint_layers_source_graph_capture) on the way in, so a
 * later Revert can hand the graph back.
 *
 * The baked maps and every other state the conversion touches are the function's to clean up: a
 * false return leaves the material exactly as it was found, with \a r_report->failure naming why.
 * Rendering the maps stays with the caller (it needs the editors' bake); this only receives what
 * it rendered.
 *
 * \param baked_channels: the channel of each map, parallel to \a baked_images.
 * \param baked_images: the maps the bake created, owned by nobody yet.
 * \param image_size: the size the maps were rendered at, the stack's own size from now on.
 * \return false when \a ma cannot be edited here, the bake produced no channel set, the Fill row
 * could not be added, or a map could not be linked; nothing of the conversion remains then.
 */
bool BKE_paint_layers_convert_from_bake(Main &bmain,
                                        Material &ma,
                                        Span<eMaterialPaintChannel> baked_channels,
                                        Span<Image *> baked_images,
                                        int16_t image_size,
                                        PaintLayersConvertBakeReport *r_report = nullptr);

/**
 * Hand a converted material its plain node graph back, the whole revert an operator needs: the
 * recorded links are restored (#BKE_paint_layers_source_graph_release), the nodes the generator
 * minted in the material's own tree are removed (its Normal Map and Bump helpers, the paint pass
 * viewer's Mix and Emission), Material Output's Surface is routed from the Principled again, the
 * generated instance is removed and the generated root tree (#Material::paint_layers_tree) is
 * deleted with it. A Bump or Normal Map the stack only marked -- a duplicate the user made of a
 * helper, or one the stack adopted from their graph -- is the user's and stays. The layer and
 * source groups the tree instantiated are left in the main: the regeneration finds them by marker
 * and owner uid and re-uses them, so nothing the user authored inside them is lost.
 *
 * The layer records and their maps stay in the material: they sleep until the stack is re-layered
 * or re-converted, and #MA_PAINT_LAYERED and #MA_PAINT_LAYERS_SOURCE_GRAPH are cleared so the
 * material reads as plain. The material's node tree is tagged; the caller sends the notifiers.
 *
 * \return false when \a ma is not a layered material the file allows editing, or has no embedded
 * node tree; nothing is touched then.
 */
bool BKE_paint_layers_revert_to_plain(Main &bmain,
                                      Material &ma,
                                      PaintLayersRevertReport *r_report = nullptr);

/**
 * The content hash of the graph \a ma's own stack reads: the walk starts at every **linked**
 * `Source <channel>` input of the generated instance and follows what feeds it upstream, through
 * groups, muted links and generator-placed route helpers (which contribute nothing -- their
 * recreation on a regeneration must not move the hash). An unlinked Source input reads the
 * channel's neutral constant and is not the graph's content: it contributes nothing, and a graph
 * that feeds no adopted channel hashes to zero, the "no cache belongs to this material" value.
 * Nothing else of the tree takes part, so editing the stack itself leaves the hash alone; feeding
 * a channel, changing a value along its chain or swapping the image a node samples moves it.
 *
 * Data-block references are hashed by name and owning library, never by `session_uid`: this hash is
 * stored in the material (#Material::paint_layers_source_hash) and compared after a file load, and
 * a session uid is reborn every session. Main thread only.
 */
uint64_t BKE_paint_layers_source_channel_graph_hash(const Material &ma);

/** The channel-graph hash above, remembered against the node tree update signal. Main thread only. */
uint64_t BKE_paint_layers_source_channel_graph_hash_cached(const Material &ma);

/** One channel the material's own graph feeds: its linked `Source <channel>` instance input. */
struct PaintLayersSourceChannelFeed {
  eMaterialPaintChannel channel;
  /** The linked input socket of the generated instance, in the material's own node tree. */
  const bNodeSocket *socket;
};

/**
 * The channels \a ma's own graph feeds the stack by link, in the canonical channel order: one feed
 * per `Source <channel>` input the generated instance carries **and** has a link on. A channel the
 * graph feeds by value alone reads its neutral constant and has nothing to bake; Normal, Height,
 * Custom and AO are never fed by construction. The sockets belong to \a ma's tree and are only
 * good while it is not edited -- a localized copy is resolved on the copy, the way the bake does.
 *
 * \return the number of feeds written to \a r_feeds (sized to hold a feed per material channel).
 */
int BKE_paint_layers_source_graph_feeds_find(const Material &ma,
                                             MutableSpan<PaintLayersSourceChannelFeed> r_feeds);

/**
 * Whether the graph the stack reads may depend on the object or the camera: the walk upstream of
 * the Source inputs met a Geometry, Object Info, Texture Coordinate, Camera Info or Light Path
 * node. A UV-quad bake cannot reproduce such a channel faithfully (the render reads the real
 * surface), so callers warn rather than promise the pixels. Remembered with the channel-graph
 * hash. Main thread only.
 */
bool BKE_paint_layers_source_graph_object_dependent(const Material &ma);

/** Whether \a ma's source-graph cache is behind the graph (#MA_PAINT_LAYERS_SOURCE_STALE). */
bool BKE_paint_layers_source_cache_stale_get(const Material &ma);

/**
 * The bottom row of \a ma that a material conversion's bake filled and left sleeping: the row a
 * later bake of the same conversion puts its maps back into, instead of stacking a second Fill
 * over the same data. Only a row marked #MA_PAINT_LAYER_CONVERSION_FILL answers -- a Fill the user
 * made by hand never does, whatever it holds -- and a stack whose bottom row is anything else has
 * no fill to reuse.
 *
 * \return the row, or null when there is none to reuse.
 */
MaterialPaintLayer *BKE_paint_layers_conversion_fill_find(Material &ma);

/**
 * Make the conversion fill \a fill of \a ma paint exactly the channels of \a baked_mask: every
 * channel of the mask gets a record (enabled, without a map -- the bake links its own) and every
 * record of a channel outside the mask is switched off. Nothing is removed: a switched-off record
 * keeps its map and its value in the row, so the pixels a user painted stay in the file however
 * the masks move, and re-aligning with the old mask turns the channels back on untouched.
 */
void BKE_paint_layers_conversion_fill_channels_align(Material &ma,
                                                     MaterialPaintLayer &fill,
                                                     PaintChannelMask baked_mask);

/** \} */

}  // namespace blender
