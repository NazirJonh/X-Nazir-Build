/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Helpers shared by `paint_layers.cc` (the description and its edits) and `paint_layers_bake.cc`
 * (the bake side). They live here rather than in one of the two so neither owns the other, and are
 * inline because both are tiny and pure.
 */

#pragma once

#include <cstdint>
#include <memory>

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_resolve.hh"

#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

namespace blender {

struct ImBuf;
struct Image;
struct ImageUser;
struct Main;
struct Mesh;
struct Material;
struct MaterialPaintLayer;
struct MaterialPaintLayerBake;
struct MaterialPaintLayerChannel;
struct Object;
struct bNodeTree;

/** The IDProperty key a Custom group's interface socket carries its role under. */
constexpr const char *PAINT_LAYERS_CUSTOM_ROLE_PROP = "pbr_custom_role";

/**
 * Whether a #eMaterialMeshMapType is scalar -- its R is the whole value -- rather than an RGB map.
 *
 * A scalar atlas (AO, Curvature, Edge) contributes its R spread across the channel's RGB; an RGB
 * atlas (Normal, the two IDs) contributes its stored RGB as-is. The one classification the generator
 * and the CPU composite share, so a scalar map cannot render grey on one side and coloured on the
 * other.
 */
inline bool paint_layer_mesh_map_is_scalar(const int8_t type)
{
  return ELEM(type, MA_MESH_MAP_AO, MA_MESH_MAP_CURVATURE, MA_MESH_MAP_EDGE);
}

/**
 * The atlas a MESH_MAP \a layer reads, when one is assigned, or null.
 *
 * A MESH_MAP row reads the material's atlas slot for its #MaterialPaintLayer::mesh_map_type; every
 * channel of the row reads the same Image. The one definition the generator and the CPU composite
 * both use, so the two cannot disagree about whether the row contributes at all. Null -- no slot or
 * no image -- means the row contributes nothing on either side (spec M2 item 4).
 *
 * The answer is structural: it says what the description (DNA) names, and nothing about runtime
 * state. Whether the image's buffer happens to be loaded must not decide whether the row builds a
 * TEX_IMAGE node, or what the topology hash and the sampler counter see -- an atlas that is merely
 * not loaded yet (a freshly opened file, say) would otherwise stay invisible until something else
 * loads its buffer, and the next regenerate would flicker the tree. The CPU path therefore never
 * consults this about buffers: it acquires and releases the buffer through
 * #composite_image_acquire, like every painted map, and an atlas whose buffer cannot be acquired
 * fails the stack exactly like a painted map without one. When the slot has no atlas the row
 * contributes nothing on both sides, as before.
 */
inline Image *paint_layer_mesh_map_image(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.source != MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    return nullptr;
  }
  const MaterialMeshMapSlot *slot = BKE_mesh_maps_slot_find(ma, layer.mesh_map_type);
  if (slot == nullptr || slot->image == nullptr) {
    return nullptr;
  }
  return slot->image;
}

/** The list whose direct members include \a target, or null when it is not in \a list. */
inline ListBaseT<MaterialPaintLayer> *paint_layer_owner_list(ListBaseT<MaterialPaintLayer> *list,
                                                             const MaterialPaintLayer *target)
{
  for (MaterialPaintLayer &layer : *list) {
    if (&layer == target) {
      return list;
    }
    if (ListBaseT<MaterialPaintLayer> *found = paint_layer_owner_list(&layer.children, target)) {
      return found;
    }
    if (ListBaseT<MaterialPaintLayer> *found = paint_layer_owner_list(&layer.effects, target)) {
      return found;
    }
    if (ListBaseT<MaterialPaintLayer> *found = paint_layer_owner_list(&layer.mask_stack, target)) {
      return found;
    }
  }
  return nullptr;
}

/** The `pbr_custom_role` string on \a properties, or null when there is none. */
inline const char *custom_role_get(const IDProperty *properties)
{
  if (properties == nullptr) {
    return nullptr;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(
      properties, PAINT_LAYERS_CUSTOM_ROLE_PROP, IDP_STRING);
  return prop != nullptr ? IDP_string_get(prop) : nullptr;
}

/**
 * The record of \a layer's channels array that stands for \a channel, or null when it has none.
 *
 * The participation rules built on this -- present, image -- are shared by the generator, the CPU
 * compositor and the bake, so a row cannot take part in a channel on one side and not the other.
 */
inline const MaterialPaintLayerChannel *paint_layer_channel_find(const MaterialPaintLayer &layer,
                                                                 const int channel)
{
  for (int i = 0; i < layer.channels_num; i++) {
    if (layer.channels[i].channel == channel) {
      return &layer.channels[i];
    }
  }
  return nullptr;
}

/**
 * The map a Material layer shows in \a channel: what its source material was baked into, or null.
 *
 * Those maps are the layer's *content*, like a Paint layer's own maps: its mask, corrections,
 * opacity and blend apply to them live, so editing any of those never re-bakes the source. The
 * maps are used whether or not the source changed since: a re-bake refreshes them in place, and
 * the layer keeps showing the previous result meanwhile rather than dropping out.
 */
inline Image *paint_layer_material_source_map(const MaterialPaintLayer &layer, const int channel)
{
  if (layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL || layer.bake == nullptr || channel < 0 ||
      channel >= int(ARRAY_SIZE(layer.bake->images)))
  {
    return nullptr;
  }
  /* Alpha has no channel slot of its own: #BKE_paint_layers_material_bake_apply and the bake
   * planner (`render_material_bake.cc`) both write and read the source's transparency as the
   * row's #MaterialPaintLayerBake::coverage, not `images[PAINT_MATERIAL_CHANNEL_ALPHA]` -- nothing
   * ever fills that slot. Reading `images[]` here left every Material row seeing Alpha as
   * permanently unmapped, so a row could never settle on Baked. */
  if (channel == PAINT_MATERIAL_CHANNEL_ALPHA) {
    return layer.bake->coverage;
  }
  return layer.bake->images[channel];
}

/**
 * Whether \a record is a live channel: not null and #MA_PAINT_LAYER_CHANNEL_ENABLED.
 *
 * #MA_PAINT_LAYER_CHANNEL_DISABLED keeps the record and its map but unlinks the channel, so the row
 * takes part nowhere while the map stays in the file; #MA_PAINT_LAYER_CHANNEL_ABSENT has no map at
 * all. The participation helpers and the paint target share this one predicate, so a row cannot
 * stay in the viewport or accept a stroke on one side and drop out on the other.
 */
inline bool paint_layer_channel_live(const MaterialPaintLayerChannel *record)
{
  return record != nullptr && record->state == MA_PAINT_LAYER_CHANNEL_ENABLED;
}

/**
 * Whether \a layer's \a channel is filtered out by the material's global channel set.
 *
 * A channel absent from #Material::paint_layers_channels takes part nowhere; the record and its map
 * stay on the row, so re-adding the channel brings it back untouched. A Mask Item is exempt: it is
 * a single scalar over the row, not a PBR channel, and its placeholder channel record must survive
 * the filter for #paint_layer_mask_correction_image to keep reading it.
 */
inline bool paint_layer_channel_filtered(const Material &ma,
                                         const MaterialPaintLayer &layer,
                                         const int channel)
{
  if (BKE_paint_layers_role(layer) == PaintLayerRole::MaskItem) {
    return false;
  }
  return !BKE_paint_layers_channel_in_set(ma, eMaterialPaintChannel(channel));
}

/** #paint_layer_channel_live for a (row, channel) pair, with the global channel set applied. */
inline bool paint_layer_channel_live(const Material &ma,
                                     const MaterialPaintLayer &layer,
                                     const int channel)
{
  return !paint_layer_channel_filtered(ma, layer, channel) &&
         paint_layer_channel_live(paint_layer_channel_find(layer, channel));
}

/** Whether \a layer takes part in \a channel: a live record the material's set also carries. */
inline bool paint_layer_channel_present(const Material &ma,
                                        const MaterialPaintLayer &layer,
                                        const int channel)
{
  if (paint_layer_channel_filtered(ma, layer, channel)) {
    return false;
  }
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    /* A MESH_MAP row paints the atlas in every channel its records name, whatever the map type. */
    return paint_layer_mesh_map_image(ma, layer) != nullptr &&
           paint_layer_channel_live(paint_layer_channel_find(layer, channel));
  }
  if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return paint_layer_material_source_map(layer, channel) != nullptr;
  }
  return paint_layer_channel_live(paint_layer_channel_find(layer, channel));
}

/**
 * The image \a layer shows in \a channel, or null when its source is a constant.
 *
 * A MESH_MAP row's map is the material's shared atlas for its type, the same Image in every
 * channel; it is never the channel record's own `image`, which stays unused. One resolver for the
 * generator and the CPU composite, so the two cannot disagree about whether a row contributes.
 */
inline Image *paint_layer_channel_image(const Material &ma,
                                        const MaterialPaintLayer &layer,
                                        const int channel)
{
  if (paint_layer_channel_filtered(ma, layer, channel)) {
    return nullptr;
  }
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    return paint_layer_channel_present(ma, layer, channel) ?
               paint_layer_mesh_map_image(ma, layer) :
               nullptr;
  }
  if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return paint_layer_material_source_map(layer, channel);
  }
  const MaterialPaintLayerChannel *entry = paint_layer_channel_find(layer, channel);
  if (!paint_layer_channel_live(entry)) {
    return nullptr;
  }
  return entry->image;
}

/**
 * Whether a Fill row reads a map the user assigned in \a channel instead of its
 * constant. A Fill is never painted, but its channels may hold their own image, which is then a
 * texture source for that channel only; the other channels keep the constant. The generator, the
 * CPU composite and the topology hash all ask this, so they cannot disagree about it.
 *
 * Covers both a stack Layer Fill and a content Fill correction (Effect): the one rule
 * #BKE_paint_layers_fill_reads_map.
 */
inline bool paint_layer_fill_reads_map(const Material &ma,
                                       const MaterialPaintLayer &row,
                                       const int channel)
{
  return BKE_paint_layers_fill_reads_map(ma, row, channel);
}

/**
 * Whether a content Fill correction lays nothing in \a channel because that channel is switched off.
 *
 * A Fill that carries channel records contributes only through the live ones, so a disabled or
 * missing record must never fall back to the row's flat colour (it would fill the channel with the
 * constant's default black). Only a Fill with no records at all keeps the legacy "constant in every
 * channel" meaning.
 */
inline bool paint_layer_fill_effect_channel_off(const Material &ma,
                                                const MaterialPaintLayer &correction,
                                                const int channel)
{
  return correction.role == MA_PAINT_LAYER_ROLE_EFFECT &&
         correction.source == MA_PAINT_LAYER_SOURCE_CONSTANT && correction.channels_num > 0 &&
         !paint_layer_channel_live(ma, correction, channel);
}

/**
 * The map a mask item carries in its channel record, or null when the item is a constant.
 *
 * A mask is scalar and one for every channel, so its map and constant live in a single channel
 * record; Base Color is only a placeholder index. A Mask-mode stroke on the item writes there. A
 * MESH_MAP mask item reads the atlas through the same record, so a stroke can still give it a map.
 */
inline Image *paint_layer_mask_correction_image(const Material &ma,
                                                const MaterialPaintLayer &correction,
                                                const int /*channel*/)
{
  if (correction.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    return paint_layer_mesh_map_image(ma, correction);
  }
  return paint_layer_channel_image(ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

/**
 * The flat colour \a layer contributes to \a channel when it has no map: a Layer-role row's channel
 * record value (Base Color included), or a mask/Fill-effect correction's DNA `fill_color`. One
 * definition for the generator and the CPU, so a constant cannot mean two things.
 *
 * INVARIANT: this is the only reader of a Layer-role row's channel constant. Direct reads of
 * #MaterialPaintLayer::fill_color are valid only for rows without channel records (masks and
 * Fill-effect corrections), which read through #BKE_paint_layers_correction_constant.
 */
void paint_layer_channel_constant(const MaterialPaintLayer &layer, int channel, float r_color[4]);

/**
 * Whether an enabled content correction of \a layer lays a map or a flat colour in \a channel.
 *
 * A Paint row nobody has painted yet holds no map and a fully transparent constant, so it would
 * normally drop out of the stack and take its corrections with it. This is what keeps such a row
 * alive as an empty (alpha 0) constant instead, without allocating a map for it: nothing is
 * stored until the user paints. One answer for the generator and the CPU composite.
 */
bool paint_layer_effects_lay_content(const Material &ma, const MaterialPaintLayer &layer, int channel);

/* -------------------------------------------------------------------- */
/** \name Implementation-only declarations
 *
 * Moved out of `BKE_paint_layers.hh`: these are consumed only by the Paint Layers
 * description/composite/bake implementation and its tests. Names, signatures and documentation
 * are unchanged, so the public header stays the module's contract.
 * \{ */

/**
 * Where a row reads its values from, the description-level view of a row's source.
 *
 * Stored directly on the row as #MaterialPaintLayer::source, so callers ask what a row *is*
 * rather than reconstruct it from other fields.
 */
enum class PaintLayerSourceType : int8_t {
  /** A painted map; a Paint row with no map yet still reads as Image (its flat value is the
   * starting point of its first stroke, not a constant source). */
  Image = 0,
  /** A flat constant: a Fill row, or a Fill effect. */
  Constant,
  /** Another material's channels, baked. */
  Material,
  /** A user's node group. */
  NodeGroup,
  /** A nested stack: a folder. */
  Stack,
  /**
   * A geometry map of the owning object, read from the material's shared UV atlas; the row stores
   * the abstract map type in #MaterialPaintLayer::mesh_map_type.
   */
  MeshMap,
};

/**
 * Static description of one #eMaterialPaintLayerSource: the per-source switches that used to be
 * `ELEM(kind, ...)` checks scattered across the generator, the CPU compositor and the bake.
 *
 * One table instead of many, so adding a source is one edit here plus its branches, and the
 * Outliner's add-kinds list, the generator and the bake all read the same answer. A row's source
 * alone decides these switches -- including for a correction, whose source is always Image or
 * Constant -- so a caller for whom the structural role also matters (a Fill-effect correction is
 * not a stack Layer) checks #BKE_paint_layers_role separately; see `paint_layers.cc` for where
 * that distinction is load-bearing.
 */
struct PaintLayerKindInfo {
  int source;
  /** Stable identifier shared with the Outliner's add-kinds and the Python API. */
  const char *identifier;
  /** Untranslated UI name. */
  const char *ui_name;
  /** The row holds its stack in #MaterialPaintLayer::children and takes part through it alone. */
  bool is_folder;
  /** The row paints with #MaterialPaintLayer::fill_color rather than a map or value. */
  bool uses_fill_color;
  /**
   * The CPU compositor cannot evaluate the row: an editor bake fills its maps and the
   * generator/CPU substitute them. MATERIAL bakes its source through the material bake; NODE_GROUP
   * is rendered through EEVEE/AOV.
   */
  bool needs_external_bake;
};

/**
 * What one #BKE_paint_layers_regenerate call learns once and reuses: it lives on that call's stack
 * and is handed down, never kept between calls, so nothing outside a regeneration can read a stale
 * answer. Every reader takes it as an optional pointer; without one the answer is recomputed, which
 * is what bake, the CPU composite, RNA and the editors do.
 */
struct PaintLayersRegenCache {
  /**
   * Whether #modes may be filled. The sampler-budget fallback moves rows' modes (a forced bake), so
   * a mode cached before it is done would outlive the change; the regeneration raises this once the
   * final forced set is known.
   */
  bool modes_frozen = false;
  /** Rows' modes, filled lazily and only while #modes_frozen. */
  mutable Map<const MaterialPaintLayer *, PaintLayerMaterialMode> modes;
  /** #BKE_paint_layers_material_bake_ready per row: hashing the source tree is not free. */
  mutable Map<const MaterialPaintLayer *, bool> bake_ready;
  /** The Pass Through visibility multiplier of every row, from one walk of the stack. */
  mutable Map<const MaterialPaintLayer *, float> pass_through_scales;
  mutable bool pass_through_scales_valid = false;

  /**
   * The resolve of \a source, computed on first ask. A source's node tree is not written while a
   * stack is regenerated (only the owner's is), so the answer holds for the whole call. The entry is
   * heap-allocated, so the returned reference stays valid when later ones are added.
   */
  const MaterialSourceResolve &resolve(const Material *source) const;

  /**
   * #resolve through \a cache when there is one, else a fresh resolve held in \a r_local. Either way
   * the caller reads the returned reference and never copies the resolve.
   */
  static const MaterialSourceResolve &resolve_get(const Material *source,
                                                  const PaintLayersRegenCache *cache,
                                                  MaterialSourceResolve &r_local);

  /**
   * Drop what depends on the owner's own node tree, for a caller about to rewrite it: a row that
   * reads its owner as its source would otherwise keep the answer from before the rewrite.
   */
  void invalidate_for_owner(const Material &owner);

 private:
  mutable Map<const Material *, std::unique_ptr<MaterialSourceResolve>> resolves_;
};

/**
 * The UV layer name a Paint Layers stack samples, stored on the material itself. Never null; an
 * empty string means no name is set and callers fall back to the old behavior.
 */
const char *BKE_paint_layers_uv_map_name(const Material &ma);

/**
 * Whether \a folder can be inlined into its parent's chain instead of isolating its children.
 *
 * A Pass Through folder changes nothing its children do, so the generator expands it in place and
 * the CPU compositor flattens it: the same rows, the same order, no wrapper. The mode is a pure
 * function of the description -- no mask item, no content correction, Mix blend and opacity one on
 * every channel (per-channel overrides included) and no valid bake standing in -- so the generator
 * and the compositor can never disagree about it. Visibility is deliberately not part of the test:
 * a hidden Pass Through folder scales its children's factor to zero as a value edit.
 */
bool BKE_paint_layers_folder_is_pass_through(const Material &ma,
                                             const MaterialPaintLayer &folder);

/**
 * The #PaintLayerSourceType \a layer reads from.
 *
 * Reads #MaterialPaintLayer::source directly; a correction's source is always Image or Constant.
 */
PaintLayerSourceType BKE_paint_layers_source_type(const MaterialPaintLayer &layer);

/** The descriptor for \a source; an unknown source reads back as Image. */
const PaintLayerKindInfo &BKE_paint_layers_kind_info(int source);

/**
 * Convert a Fill row to Paint, carrying its constant into every channel record first, so a stroke
 * can later give one channel a map while the others stay flat: source -> Image, every existing
 * record's value set to the row's fill colour.
 *
 * \param r_fill: receives the row's fill colour (scene linear), valid after the call.
 * \return true when the row was a Fill and was converted; false when it was already Paint.
 */
bool BKE_paint_layers_fill_to_paint(Material &ma, MaterialPaintLayer &layer, float r_fill[4]);

/** Whether \a marker names \a layer or anything nested under it (children, effects,
 * mask stack). */
bool BKE_paint_layers_subtree_contains(const MaterialPaintLayer &layer, const bUUID &marker);

/**
 * The factor \a layer blends by before any per-pixel coverage: its own opacity, zero when it is
 * disabled, and -- when its mask is a constant rather than a map -- the mask's value folded in.
 *
 * The generator, the value sync and the CPU composite all read this one helper, so a constant mask
 * and a switched-off row cannot mean different things on the two sides. A mask that carries a map
 * is not folded here: its per-pixel alpha multiplies the factor at render and composite time.
 */
float BKE_paint_layers_effective_opacity(const MaterialPaintLayer &layer);

/**
 * The blend \a layer's \a channel blends by: the channel record's override, or the row's
 * #MaterialPaintLayer::blend when the channel has no record or records `-1` (inherit).
 *
 * The generator, the CPU compositor and the UI all read this one helper, so a per-channel blend
 * cannot mean two different things.
 */
int BKE_paint_layers_channel_blend_effective(const MaterialPaintLayer &layer, int channel);

/**
 * Whether \a layer's Normal channel replaces what lies below it rather than combining with it.
 *
 * Unlike every other channel the Normal one ignores the row's own blend: it is either the Normal
 * Combine group or, when its channel override is #MA_PAINT_LAYER_BLEND_NORMAL_REPLACE, a plain
 * Mix. The generator and the CPU composite read this one helper.
 */
bool BKE_paint_layers_normal_replace(const MaterialPaintLayer &layer);

/**
 * The factor \a layer's \a channel blends by: the row's effective opacity
 * (#BKE_paint_layers_effective_opacity, which folds enabled and a constant mask) times the
 * channel record's #MaterialPaintLayerChannel::opacity, or the row's effective opacity alone when
 * the channel has no record.
 */
float BKE_paint_layers_channel_opacity_effective(const MaterialPaintLayer &layer, int channel);

/**
 * The flat colour \a correction contributes to \a channel when its effect is Fill: the correction's
 * own fill colour, or -- for a Paint correction with no map in this channel -- the channel's flat
 * value. The generator and the CPU composite read this one helper, so a constant correction cannot
 * mean two different colours.
 */
void BKE_paint_layers_correction_constant(const MaterialPaintLayer &correction,
                                          eMaterialPaintChannel channel,
                                          float r_color[4]);

/**
 * Decode one straight RGBA sample of \a image out of its stored colorspace and into scene linear.
 *
 * This is the per-element form of the conversion the Image Texture node performs, kept for the
 * cross-test interpreter and for unit-testing the round trip; compositing decodes whole buffers at
 * once (see the CPU composite), never pixel by pixel. A data (Non-Color) or already scene-linear
 * colorspace is the identity.
 */
void BKE_paint_layers_sample_to_linear(const Image &image, float rgba[4]);

/** The inverse of #BKE_paint_layers_sample_to_linear: encode a scene-linear RGBA sample. */
void BKE_paint_layers_sample_from_linear(const Image &image, float rgba[4]);

/**
 * Decode a description constant -- a Fill colour or a channel's flat value -- into scene linear.
 *
 * The description stores these as RNA `PROP_COLOR` values, which are already scene linear, so this
 * is the identity today and the one place that would change if a gamma-stored constant appeared.
 * The generator, the value sync and the CPU composite all read it, so the two sides cannot disagree
 * about what a constant means.
 */
void BKE_paint_layers_constant_to_linear(eMaterialPaintChannel channel,
                                         const float rgba[4],
                                         float r_linear[4]);

/**
 * The `MA_RAMP_*` code a description blend stands for -- the one table the generator's Mix nodes
 * and the CPU's `ramp_blend` both read, so a new blend cannot mean two different things.
 *
 * `MA_PAINT_LAYER_BLEND_NORMAL_COMBINE` is not a Mix mode and has no ramp code; it answers
 * `MA_RAMP_BLEND`, and the Normal channel never reaches this function with it.
 */
int BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend blend);

/** The identifier a Custom role uses for \a channel, e.g. `"BASE_COLOR"`, or null. */
const char *BKE_paint_layers_custom_channel_identifier(int channel);

/**
 * Give every role-less input of a Custom layer's group a stored value in
 * `MaterialPaintLayer::properties`, keyed by the socket identifier, so the UI has a value to edit
 * and the bake has something to instantiate with. Existing values are left alone.
 */
void BKE_paint_layers_custom_properties_sync(Material &ma);

/**
 * The value a new channel record of \a layer starts from: the Principled input's own default
 * (Metallic 0, Roughness 0.5, Specular IOR Level 0.5, ...), or the table fallback for a material
 * with no Principled. Scalar channels come back as a grey RGBA with alpha one, since the generated
 * chain and the CPU read the channel's mean.
 *
 * A Paint record starts transparent instead (see #BKE_paint_layers_channel_add): only a Fill shows
 * these defaults, so a fresh Paint covers nothing.
 */
void BKE_paint_layers_channel_default_value(const Material &ma, int channel, float r_value[4]);

/** The #eMaterialMeshMapType of a #MA_PAINT_LAYER_SOURCE_MESH_MAP row, or -1 for any other row. */
int BKE_paint_layers_mesh_map_type_get(const MaterialPaintLayer &layer);

/**
 * Debug check that the split lists agree with their rows' roles: every row of
 * #MaterialPaintLayer::effects has #PaintLayerRole::Effect and every row of
 * #MaterialPaintLayer::mask_stack has #PaintLayerRole::MaskItem. The body is `BLI_assert`
 * only, so it is a no-op in a release build; the generator calls it once per build to catch a
 * storage bug early.
 */
void BKE_paint_layers_assert_consistent(const Material &ma);

/**
 * Start (or restart) the partial-update subscription over the maps \a layer's bake depends on.
 * Called when a bake is written, so pixel edits to those maps are noticed by
 * #BKE_paint_layers_bake_notice_changes.
 */
void BKE_paint_layers_bake_subscribe(Material &ma, MaterialPaintLayer &layer);

/** Drain \a ma's subscriptions; a pixel change marks the material stale for the planner. */
void BKE_paint_layers_bake_notice_changes(Material &ma);

/**
 * The union of the pixel rectangles \a layer's source maps changed in since its last bake, as
 * \a r_region `{xmin, xmax, ymin, ymax}`, or false when nothing is pending.
 *
 * The planner uses it to re-bake only the changed tile-sized rectangle; the first bake and the
 * first check after a load are full (the subscription reports a full update).
 */
bool BKE_paint_layers_bake_changed_region(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          int r_region[4]);

/**
 * Bring every baked row of \a ma current, the synchronous half of the K-1 planner.
 *
 * A row whose stored hash no longer matches is re-rendered through
 * #BKE_paint_layers_bake_render_node into its service maps, stamped and subscribed. Callers run
 * this on the main thread after draining the subscription (#BKE_paint_layers_bake_notice_changes);
 * the heavy map work is meant to move to a wmJob later. \a r_changed reports whether any row was
 * re-baked, so the caller knows to rebuild the tree.
 */
bool BKE_paint_layers_bake_plan_run(Main &bmain, Material &ma, bool *r_changed = nullptr);

/**
 * The generated-node weight above which an AUTO row is baked instead of evaluated live: the number
 * of nodes the row's subtree would add to the tree. One place to tune -- the planner compares a
 * row's weight against it, so a "heavy enough to be worth caching" row is one number, not a rule.
 *
 * 36 keeps the widest default row light: `paint_layer_subtree_weight` gives a leaf row
 * `4 + channels * 6`, so the five build-default channels (Base Color, Metallic, Roughness, Normal,
 * AO) weigh 34, two under the threshold. The historical value 24 predated that five-channel
 * default and classified every fresh row as heavy; a heavy row is queued to a wmJob instead of
 * staying live, which a script with no job can never settle.
 */
constexpr int PAINT_LAYERS_AUTO_BAKE_NODES = 36;

/** What a child whose valid bake stands in costs its parent's weight: one map, like one channel. */
constexpr int PAINT_LAYERS_BAKED_CHILD_WEIGHT = 6;

/**
 * The live constant a Material row's \a channel takes from its source right now.
 *
 * The row shows its source instead of its baked map when its bake is deferred
 * (#BKE_paint_layers_bake_row_is_deferred) -- so the user sees a slider move without a bake --
 * or when this channel has no baked map yet. The second case keeps the row's channel set from
 * depending on focus: without it, a freshly added Material row would drop out of the viewport
 * the moment the user moved off it, and the root topology would change on every focus move. A
 * channel with no map is better shown as the source's constant than not shown at all.
 *
 * Only channels the resolver calls #ChannelResolution::Constant are answered here; an Image or
 * Baked channel returns false and stays on its baked map. A row that is not deferred and already
 * has a map for the channel wins with the map.
 *
 * The generator and the CPU compositor both ask this, so the two cannot disagree about
 * what the row is showing.
 *
 * \param r_value: the source's socket default -- x for a scalar channel, xyz for a colour
 *                 one, matching #MaterialSourceResolve.constants.
 */
bool BKE_paint_layers_material_live_constant(const Material &ma,
                                             const MaterialPaintLayer &layer,
                                             int channel,
                                             float r_value[4],
                                             const PaintLayersRegenCache *cache = nullptr);

/**
 * The live map an active Material row's \a channel takes from its source right now.
 *
 * Mirrors #BKE_paint_layers_material_live_constant for #ChannelResolution::Image, under
 * the same rule: the row is deferred, or this channel has no baked map yet.
 *
 * Only a trivially mapped texture qualifies -- flat projection and nothing linked to the
 * node's Vector input. The CPU compositor samples an #ImBuf straight in UV space and
 * reproduces neither a mapping chain nor a projection, so anything else would make the two
 * sides disagree and the picture jump when the row is left.
 */
bool BKE_paint_layers_material_live_image(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          int channel,
                                          Image **r_image,
                                          const ImageUser **r_iuser,
                                          const PaintLayersRegenCache *cache = nullptr);

/**
 * Whether a sampler-budget fallback pinned \a layer onto its baked maps for this session. Runtime
 * state only: it is recomputed from the budget on every regeneration, lifted as soon as the budget
 * allows, and never saved. #BKE_paint_layers_material_mode reports #PaintLayerMaterialMode::Baked
 * while this holds, which is what makes the topology hash see the mode change and rebuild once.
 */
bool BKE_paint_layers_material_forced_bake(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether the bake of a Material row can be shown: its stored hash matches the source and none of
 * its maps is being rendered right now. The hash alone is not enough, because the hand-over stamps
 * it before the worker has written any pixel. A row that is not ready stays live, so a stale, blank
 * or half-written map is never shown; it moves to its maps once, when the last map lands.
 */
bool BKE_paint_layers_material_bake_ready(const Material &ma, const MaterialPaintLayer &layer);

/**
 * The single substitution decision the generator and the CPU compositor share: whether \a layer's
 * baked map for \a channel may stand in for its live subtree right now, and the map.
 *
 * \return true and sets \a r_image when #BKE_paint_layers_bake_is_valid holds and a baked map exists
 * for the channel. Never reads a baked map any other way, so the two sides cannot disagree.
 */
bool BKE_paint_layers_bake_substitute(const Material &ma,
                                      const MaterialPaintLayer &layer,
                                      int channel,
                                      Image **r_image);

/**
 * The C-7 fallback for a Custom layer when its bake is not current.
 *
 * A Custom group has no CPU expression at all, so a render without a GPU context -- or one that
 * started before the first bake landed -- has nothing live to fall back on. When maps exist from an
 * earlier bake they are still shown, flagged stale through \a r_stale, rather than dropping the row
 * to black; when no maps exist the row is skipped (the result below passes through). Only NodeGroup
 * is covered: every other source has a live subtree the strict #BKE_paint_layers_bake_substitute
 * leaves in place.
 *
 * \return true and sets \a r_image when a color map and a coverage map exist for \a channel.
 */
bool BKE_paint_layers_bake_substitute_custom(const Material &ma,
                                             const MaterialPaintLayer &layer,
                                             int channel,
                                             Image **r_image,
                                             bool *r_stale);

/**
 * Composite \a layer's subtree for \a channel the way a bake of it would: the isolated-group model
 * (`P/a`, the same the CPU already computes for a folder), producing the node's straight scene-
 * linear colour and its grey coverage.
 *
 * \a r_color_rgba is `size * size * 4` floats, \a r_coverage_gray `size * size`. The node's own maps
 * are composited at their resolution and nearest-sampled to \a size. The bake planner and the
 * generator's substitution read this one function's result, so a baked map means one thing.
 *
 * \param dst_rect: when given, only this `{x0, y0, x1, y1}` rectangle of the `size`-square result
 *                  is computed and written; the caller passes it for a partial re-bake so the
 *                  compute follows the changed tile instead of the whole map. The rest of the
 *                  outputs is left untouched. The rectangle is clamped to `size`.
 *
 * \return false when \a layer is not in \a ma, does not take part in \a channel, or the composite
 * fails.
 */
bool BKE_paint_layers_bake_render_node(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       int channel,
                                       int size,
                                       float *r_color_rgba,
                                       float *r_coverage_gray,
                                       const int *dst_rect = nullptr);

/** The pixel dimensions of \a layer's content in \a channel, the size a bake defaults to. */
bool BKE_paint_layers_row_dimensions(const Material &ma,
                                     const MaterialPaintLayer &layer,
                                     int channel,
                                     int &r_width,
                                     int &r_height);

/**
 * Render \a row's result in \a channel -- the same content a bake of it stores, through
 * #BKE_paint_layers_bake_render_node -- into \a dst, which must already be \a size square and carry
 * the channel's colorspace. Used by the "Use Row Result" operator to copy one row's content into
 * another row's channel map.
 */
bool BKE_paint_layers_bake_row_to_image(const Material &ma,
                                        const MaterialPaintLayer &row,
                                        int channel,
                                        int size,
                                        Image &dst);

/**
 * Whether a value edit or a pixel change left some row of \a ma waiting to be re-baked.
 *
 * A scheduler signal only ("something in this material changed"), never the per-row answer: which
 * row is stale is #BKE_paint_layers_bake_is_valid's question.
 */
bool BKE_paint_layers_bake_stale_get(const Material &ma);
/** Clear \a ma's pending-bake mark; the planner calls this once the queue is drained. */
void BKE_paint_layers_bake_stale_clear(Material &ma);

/**
 * Allocate \a layer's bake cache if it has none and return it, without marking anything: the caller
 * owns the edit (mode, size, a baked map) and tags the description itself.
 */
MaterialPaintLayerBake *BKE_paint_layers_bake_struct_ensure(MaterialPaintLayer &layer);

/**
 * Point \a layer's baked map for \a channel at \a image (negative \a channel is its coverage map),
 * maintaining the image's user count. The caller -- a material bake handing over its result, or the
 * synchronous planner -- owns \a image. Marks the tree stale; the bake hash is only settled by
 * #BKE_paint_layers_bake_finalize once every map of the row is in place.
 */
bool BKE_paint_layers_bake_set_map(Material &ma,
                                   MaterialPaintLayer &layer,
                                   int channel,
                                   Image *image);

/** \} */

}  // namespace blender
