/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Helpers shared by the Paint Layers description (`paint_layers.cc`) and the code that reads it.
 * They live here rather than in the public header because they are tiny, pure and only meant for
 * the Paint Layers implementation and its tests.
 */

#pragma once

#include <cstdio>
#include <memory>

#include "BLI_listbase_iterator.hh"
#include "BLI_math_base.h"

#include "BKE_paint_layers.hh"

#include "paint_layers_kind.hh"

#include "DNA_material_types.h"
#include "DNA_scene_types.h"

/**
 * The `[PBR-Layers …]` trace messages are development diagnostics: useful while a layer edit is
 * traced, pure noise in a normal build. They stay compiled out unless this is set to 1, so a
 * debugger can turn them on without carrying the prints into a release.
 */
#ifndef PBR_LAYERS_DIAGNOSTICS
#  define PBR_LAYERS_DIAGNOSTICS 0
#endif
#if PBR_LAYERS_DIAGNOSTICS
#  define PBR_LAYERS_LOG(...) printf(__VA_ARGS__)
#else
#  define PBR_LAYERS_LOG(...) ((void)0)
#endif

namespace blender {

struct Image;
struct ImageUser;
struct ImBuf;
struct Main;
struct MaterialPaintLayerBake;
struct bNode;
struct bNodeSocket;

/**
 * Whether \a layer is a Material row that shows maps rendered from its source in \a channel: the
 * row has its colour map for the channel, the map is not outside the material's channel set, the
 * mode is not Never and the maps still belong to the row (the structural hash). A map the source
 * has since moved on from still shows until the new one lands. \a r_color receives the colour map.
 * Safe on evaluated copies: it reads the row alone.
 */
bool paint_layer_row_material_map(const Material &ma,
                                  const MaterialPaintLayer &layer,
                                  int channel,
                                  Image **r_color);

/**
 * Whether the maps of the Material row \a layer are the row's to show: it has a bake of the
 * source it reads, at the side it asks for, for the channels the material asks for, and the mode
 * is not Never. The structural half of validity; reads the row alone.
 */
bool paint_layer_row_material_shown(const Material &ma, const MaterialPaintLayer &layer);

/** Where a Material row's colour in one channel comes from. */
enum class PaintLayerMaterialSourceKind : int8_t {
  /** The row lays nothing in the channel (yet). */
  None = 0,
  /** The map rendered from the source (#paint_layer_row_material_map). */
  Baked,
  /** The source's plain value: a live input of the row's group, so editing it never rebuilds. */
  LiveConstant,
  /** The source's own Image Texture, shown without the source's graph around it. */
  LiveImage,
  /**
   * The source's graph, copied into a node group of its own that the row instantiates: shown live
   * in the generated graph, never in the CPU composite, which cannot evaluate a shader.
   */
  SourceGroup,
};

/** Who is asking what a Material row lays: the shader graph or the CPU composite. */
enum class PaintLayerMaterialUse : int8_t {
  /** The CPU composite (the Image Editor, paint slots, a folder's cache): maps and live textures.
   */
  Composite = 0,
  /** The generated graph: also the source's own graph through its group. */
  Graph,
};

struct PaintLayerMaterialSource {
  PaintLayerMaterialSourceKind kind = PaintLayerMaterialSourceKind::None;
  /** #LiveConstant: the value; its alpha is one. */
  float constant[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  /** #LiveImage: the source's image; #Baked: the rendered map. */
  Image *image = nullptr;
  /** #LiveImage: the node's own image user and node, for its sampling settings. */
  const ImageUser *iuser = nullptr;
  const bNode *node = nullptr;
};

/**
 * What the Material row \a row lays in the colour \a channel. Per channel and in this order: the
 * source's plain value, the source's plain Image Texture, the map rendered from the source. The
 * answer for a channel does not depend on the others, so a source with a texture in one channel
 * and a graph in the next shows the texture live and the graph from its render.
 */
PaintLayerMaterialSource paint_layer_material_channel_source(
    const Material &ma,
    const MaterialPaintLayer &row,
    int channel,
    PaintLayerMaterialUse use = PaintLayerMaterialUse::Graph);

/** Where a Material row's coverage (the source's transparency) comes from. */
enum class PaintLayerMaterialCoverageKind : int8_t {
  /** The source has no transparency: the row covers by its opacity alone. */
  Full = 0,
  /** The source's Alpha is a plain value: a live input of the row's group. */
  LiveConstant,
  /** The source's Alpha is the Alpha output of its own Image Texture. */
  LiveAlpha,
  /** The map rendered from the source (a grey: its three channels carry the coverage). */
  Baked,
  /** The source's Alpha graph, from its group (#PaintLayerMaterialSourceKind::SourceGroup). */
  SourceGroup,
  /** The coverage has to be rendered and is not yet: the row takes part in nothing meanwhile. */
  Pending,
};

struct PaintLayerMaterialCoverage {
  PaintLayerMaterialCoverageKind kind = PaintLayerMaterialCoverageKind::Full;
  float constant = 1.0f;
  Image *image = nullptr;
  const ImageUser *iuser = nullptr;
  const bNode *node = nullptr;
};

PaintLayerMaterialCoverage paint_layer_material_coverage_source(
    const Material &ma,
    const MaterialPaintLayer &row,
    PaintLayerMaterialUse use = PaintLayerMaterialUse::Graph);

/**
 * Whether the Material row \a row takes part in the colour \a channel: it has a source in the
 * channel (#paint_layer_material_channel_source) and its coverage is ready. The one test for the
 * generator and the CPU composite.
 */
bool paint_layer_row_reads_source(const Material &ma, const MaterialPaintLayer &row, int channel);

/**
 * A hash of everything a slice is made from: the source row, the corrections or mask items above
 * the marker, values, settings, mappings and the images by session uid. No pixels: an edit to a
 * map does not change it, a change of what the slice is made of does.
 */
uint64_t paint_layer_anchor_point_slice_hash(const Material &ma,
                                             const MaterialPaintLayer &reader,
                                             int channel);

/**
 * Render the slice \a reader names in \a channel into two buffers: the straight colour with its
 * content alpha, and the coverage it lays. A mask point read as content gives a grey with alpha one
 * and full coverage. The buffers are in the source row's pixel space, scene linear.
 *
 * The returned shared pointers own the buffers. A complete render (\a dst_rect is null) is also
 * kept by the slice cache, which hands the same buffers out again for an unchanged description;
 * the cache holds a reference only, so a caller that keeps the result alive across a cache clear
 * (or an eviction) still has valid pixels. A partial render writes the requested rectangle and
 * leaves the rest of the buffer zero; it is never cached, so it lives exactly as long as the
 * caller's references.
 *
 * \param dimensions_only: resolve and size the slice, allocating and rendering nothing; returns
 * true with both buffers null when the slice exists. A caller finding the stack dimensions uses
 * this instead of rendering a 1x1 rectangle, whose nested readers would still allocate their whole
 * buffers.
 */
bool BKE_paint_layers_slice_render(const Material &ma,
                                   const MaterialPaintLayer &reader,
                                   int channel,
                                   std::shared_ptr<ImBuf> *r_color,
                                   std::shared_ptr<ImBuf> *r_coverage,
                                   const int *dst_rect,
                                   bool dimensions_only);

/** Drop every cached slice. Buffers still referenced by a description stay alive. For tests and
 * teardown: a stroke on a source map needs no clear, as the cache key holds the pixel versions of
 * the maps the slice reads (#ImageRuntime::pixel_version). */
void paint_layer_anchor_point_slice_cache_clear();

/** Bytes the cache currently accounts for; for tests of the eviction policy. */
size_t paint_layer_anchor_point_slice_cache_bytes();

/** Number of entries the cache holds; for tests of the eviction policy. */
int paint_layer_anchor_point_slice_cache_entry_count();

/** Override the byte budget. Tests use a small one; the cache never frees a buffer a description
 * still references, it only stops holding it. */
void paint_layer_anchor_point_slice_cache_set_budget(size_t bytes);

/** The byte budget currently in force; for a test guard that restores it. */
size_t paint_layer_anchor_point_slice_cache_budget();

/**
 * The maps the slice \a reader reads was rendered from, for the composite cache to subscribe to:
 * the slice hash holds no pixels, so a stroke on the source reaches the cache through these.
 */
void BKE_paint_layers_anchor_point_slice_images(const Material &ma,
                                                const MaterialPaintLayer &reader,
                                                blender::Vector<Image *> &r_images);

/**
 * Whether the content correction \a effect, a correction that reads a material, lays anything in
 * \a channel: the source supplies the channel, and its transparency is a plain value or none. A
 * source whose transparency is a map (or a graph) is not laid by a correction yet, since the
 * correction has no coverage of its own to carry one; it lays nothing here, on the generator's
 * side and the CPU's alike. A plain value is no tangent normal, so it lays nothing in Normal
 * either.
 */
bool paint_layer_correction_reads_source(const Material &ma,
                                         const MaterialPaintLayer &effect,
                                         int channel);

/**
 * Whether the mask correction \a item, a mask item that reads a material, lays anything: its grey
 * is the mean of the source's Base Color (its plain value, its plain texture or the map rendered
 * from it, exactly as a painted item's map is read), and the source's transparency is a plain
 * value or none. A mask is no channel, so the material's channel set does not decide it. A source
 * whose transparency is a map (or a graph) lays nothing, on the generator's side and the CPU's
 * alike.
 */
bool paint_layer_mask_reads_source(const Material &ma, const MaterialPaintLayer &item);

/**
 * The channel of the source whose grey the mask correction \a item lays: the lowest bit of
 * #MaterialPaintLayer::mask_source_channels, Base Color while none is set or the bit names a
 * channel a grey cannot be taken from.
 */
int paint_layer_mask_source_channel(const MaterialPaintLayer &item);

/**
 * Whether the graph of \a row's source in \a channel can be copied into a group: the Principled
 * BSDF is reached through groups the wrapper can copy (#BKE_paint_layers_source_group_wrappable),
 * and the source supplies the channel as a graph (the Normal as the encoded map the chain reads).
 * The coverage is asked as the alpha channel.
 */
bool paint_layer_material_source_group_channel(const Material &ma,
                                               const MaterialPaintLayer &row,
                                               int channel);

/**
 * The uncached walk behind #BKE_paint_layers_source_reads_uv, which memoizes it with the rest of
 * the source's resolve. Only the levels the source group copies are looked at (the root tree and
 * the groups on the path to the Principled BSDF), since a group beside the path is shared, not
 * mapped.
 */
bool paint_layer_source_reads_uv_compute(const Material &source);

/**
 * Whether \a row has a channel that only a rendered map can show in Auto: it needs a render and
 * the source group does not supply it. Such a row is rendered while it is edited, after the
 * source-edit delay, since nothing live stands in for the map.
 */
bool paint_layer_material_row_needs_map(const Material &ma, const MaterialPaintLayer &row);

/**
 * Compare the shape every Material row of \a ma gives its group with the one it gave when it was
 * last seen, and mark \a ma for regeneration when a source edit changed it. Main thread, at the
 * regeneration point. \return whether anything moved.
 */
bool BKE_paint_layers_material_sources_notice(Material &ma);

/**
 * A hash of the shape the Material row \a row gives its group: which source each channel and the
 * coverage come from, and which image a live one reads. It moves when a source edit changes what
 * the generator builds (a channel turning from a value into a texture), never when only a value
 * moves.
 */
uint64_t paint_layer_material_live_signature(const Material &ma, const MaterialPaintLayer &row);

/**
 * The record of \a layer's channels array that stands for \a channel, or null when it has none.
 *
 * The participation rules built on this are shared by every reader of the description, so a row
 * cannot take part in a channel on one side and not on the other.
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

/** Mutable counterpart of #paint_layer_channel_find. */
inline MaterialPaintLayerChannel *paint_layer_channel_find(MaterialPaintLayer &layer,
                                                           const int channel)
{
  return const_cast<MaterialPaintLayerChannel *>(
      paint_layer_channel_find(const_cast<const MaterialPaintLayer &>(layer), channel));
}

/**
 * Whether \a record is a live channel: not null and #MA_PAINT_LAYER_CHANNEL_ENABLED.
 *
 * #MA_PAINT_LAYER_CHANNEL_DISABLED keeps the record and its map but unlinks the channel, so the row
 * takes part nowhere while the map stays in the file; #MA_PAINT_LAYER_CHANNEL_ABSENT has no map at
 * all. Every reader of the description shares this one predicate, so a row cannot take part in a
 * channel on one side and drop out on the other.
 */
inline bool paint_layer_channel_live(const MaterialPaintLayerChannel *record)
{
  return record != nullptr && record->state == MA_PAINT_LAYER_CHANNEL_ENABLED;
}

/**
 * Whether \a record lays something down: it is live and carries a map, or a flat value with some
 * alpha. A map covers by its own alpha; a flat value only takes part when it has any, so an
 * unpainted row stays out of the generated graph exactly as it stays out of the CPU stack.
 *
 * The one place the rule is written: the generator's participation test and the CPU adapter both
 * call it, so a row cannot take part on one side and drop out on the other.
 */
inline bool paint_layer_channel_contributes(const MaterialPaintLayerChannel *record)
{
  return paint_layer_channel_live(record) && (record->image != nullptr || record->value[3] > 0.0f);
}

/**
 * Whether \a channel is filtered out by \a ma's global paint channel set.
 *
 * A channel absent from #Material::paint_layers_channels takes part nowhere; the record and its map
 * stay on the row, so re-adding the channel brings it back untouched. The participation helpers
 * below and the CPU adapter share this one predicate, so a row cannot take part on one side and
 * drop out on the other.
 */
inline bool paint_layer_channel_filtered(const Material &ma, const int channel)
{
  return !BKE_paint_layers_channel_in_set(ma, eMaterialPaintChannel(channel));
}

/**
 * Whether \a layer takes part in \a channel, from the row rather than the record alone.
 *
 * A Fill lays its constant whatever its channels' alpha, so a live record always takes part; every
 * other row follows #paint_layer_channel_contributes (a map, or a flat value with some alpha). A
 * channel the material's global set leaves out takes part nowhere, whatever the record says. The
 * generator's participation test and the CPU adapter both call this, so a Fill cannot take part on
 * one side and drop out on the other.
 */
inline bool paint_layer_channel_contributes(const Material &ma,
                                            const MaterialPaintLayer &layer,
                                            const MaterialPaintLayerChannel *record)
{
  if (record == nullptr || paint_layer_channel_filtered(ma, record->channel)) {
    return false;
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_FLAT_VALUE)) {
    return paint_layer_channel_live(record);
  }
  return paint_layer_channel_contributes(record);
}

/**
 * Whether the content correction \a effect lays anything in \a channel, in a channel the material's
 * set keeps.
 *
 * A live record with a map lays that map, whatever the correction's source. Without one a Paint
 * correction (the Image source) lays nothing however its value reads, and a Fill correction (the
 * Constant source) lays its flat colour -- except in Normal, where a flat value is not a tangent
 * normal and the Normal Combine would read it as a tilt. The one rule for the generator and the CPU
 * adapter.
 */
inline bool paint_layer_effect_lays(const Material &ma,
                                    const MaterialPaintLayer &effect,
                                    const int channel)
{
  if (paint_layer_channel_filtered(ma, channel)) {
    return false;
  }
  /* A correction that reads an Anchor Point lays the slice it names, exactly as a row does. */
  if (paint_layer_kind_reads_anchor_point(effect)) {
    return paint_layer_row_reads_anchor_point(ma, effect, channel);
  }
  /* A correction that reads a material has no records: what it lays is what its source supplies.
   */
  if (paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_READS_SOURCE)) {
    return paint_layer_correction_reads_source(ma, effect, channel);
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(effect, channel);
  if (!paint_layer_channel_live(record)) {
    return false;
  }
  if (record->image != nullptr) {
    return true;
  }
  return paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_FLAT_VALUE) &&
         channel != PAINT_MATERIAL_CHANNEL_NORMAL;
}

/**
 * Whether the correction \a effect is a filter: it changes the colour the row has so far instead of
 * laying a map or a flat colour over it. A filter lays no coverage, so it is deliberately not part
 * of #paint_layer_effect_lays: it never makes a row take part in a channel or keeps an empty row
 * alive.
 */
inline bool paint_layer_effect_is_filter(const MaterialPaintLayer &effect)
{
  return paint_layer_kind_has_flag(effect, PAINT_LAYER_KIND_ACTS_ON_BELOW);
}

/**
 * Whether the filter \a effect (a correction or an adjustment) changes the colour in \a channel.
 * The channels a filter works in are the live records of its row, the same switches a layer has:
 * the record carries no map, only the choice. A tangent normal is not a colour a per-channel
 * function may touch, so Normal is left alone whatever the record says. The one rule for the
 * generator, the CPU adapter and the topology hash.
 */
inline bool paint_layer_effect_filters(const Material &ma,
                                       const MaterialPaintLayer &effect,
                                       const int channel)
{
  return paint_layer_effect_is_filter(effect) && !paint_layer_channel_filtered(ma, channel) &&
         channel != PAINT_MATERIAL_CHANNEL_NORMAL &&
         paint_layer_channel_live(paint_layer_channel_find(effect, channel));
}

/**
 * Whether \a row is an adjustment: a filter that is a row of the stack and works on the stack below
 * it, with the row's own opacity, blend and mask, instead of on a row's colour.
 */
inline bool paint_layer_row_is_adjustment(const MaterialPaintLayer &row)
{
  return paint_layer_effect_is_filter(row) && row.role == MA_PAINT_LAYER_ROLE_LAYER;
}

/** Whether the adjustment \a row changes the stack in \a channel; the rule of a filter. */
inline bool paint_layer_adjustment_acts(const Material &ma,
                                        const MaterialPaintLayer &row,
                                        const int channel)
{
  return paint_layer_row_is_adjustment(row) && paint_layer_effect_filters(ma, row, channel);
}

/** The name a correction goes by in generated sockets when it has none of its own. */
inline const char *paint_layer_effect_label(const MaterialPaintLayer &effect)
{
  if (effect.name[0] != '\0') {
    return effect.name;
  }
  if (effect.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
    return "Material Correction";
  }
  return effect.source == MA_PAINT_LAYER_SOURCE_CONSTANT ? "Correction Fill" : "Correction Paint";
}

/** Whether any content correction of \a layer lays a map in \a channel. */
inline bool paint_layer_effects_lay(const Material &ma,
                                    const MaterialPaintLayer &layer,
                                    const int channel)
{
  for (const MaterialPaintLayer &effect : layer.effects) {
    if (paint_layer_effect_lays(ma, effect, channel)) {
      return true;
    }
  }
  return false;
}

/**
 * Whether a leaf row takes part in \a channel: through its own record, or only through the
 * corrections laid over it (an unpainted Paint row kept alive for them, #paint_layer_row_empty_base).
 * The generator's participation test and the CPU adapter both call this one predicate.
 */
inline bool paint_layer_row_participates(const Material &ma,
                                         const MaterialPaintLayer &layer,
                                         const int channel)
{
  return paint_layer_channel_contributes(ma, layer, paint_layer_channel_find(layer, channel)) ||
         paint_layer_effects_lay(ma, layer, channel) ||
         paint_layer_adjustment_acts(ma, layer, channel) ||
         paint_layer_row_reads_source(ma, layer, channel) ||
         paint_layer_row_reads_anchor_point(ma, layer, channel);
}

/**
 * Whether a row is only there for its corrections in \a channel: it carries no map and no flat
 * value with alpha (or its record is switched off), so on its own it would cover nothing. Its
 * coverage then starts at zero and the corrections bring it in, instead of the row filling the
 * channel with its empty (black) constant.
 */
inline bool paint_layer_row_empty_base(const Material &ma,
                                       const MaterialPaintLayer &layer,
                                       const int channel)
{
  return !paint_layer_channel_contributes(ma, layer, paint_layer_channel_find(layer, channel)) &&
         !paint_layer_row_reads_source(ma, layer, channel) &&
         !paint_layer_row_reads_anchor_point(ma, layer, channel) &&
         paint_layer_effects_lay(ma, layer, channel);
}

/**
 * Whether the mask item \a item lays coverage over its row's factor.
 *
 * The base mask always does: its Base Color record is a coverage of its own, a constant until a
 * stroke gives it a map. A mask correction over it lays its live record's map, or its flat grey when
 * it is a Fill; a Paint mask correction with no map yet lays nothing, so a fresh one changes no
 * pixel. The generator, the CPU adapter and the topology hash all ask this one rule.
 */
inline bool paint_layer_mask_item_lays(const MaterialPaintLayer &item)
{
  if ((item.flag & MA_PAINT_LAYER_MASK_BASE) != 0) {
    return true;
  }
  /* A map-coverage operator takes part whenever its element is enabled, even with no map: the
   * missing map samples as `match = 0` rather than dropping the operator out (that would be a
   * pass-through, the opposite of the operator's restriction). See #paint_layer_map_coverage_has_map. */
  if (paint_layer_kind_is_map_coverage(item)) {
    return true;
  }
  /* A filter item changes the coverage built so far: it has no map or grey of its own and so no
   * record, and what it changes is the opacity's business. */
  if (paint_layer_effect_is_filter(item)) {
    return true;
  }
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  return paint_layer_channel_live(record) &&
         (record->image != nullptr || paint_layer_kind_has_flag(item, PAINT_LAYER_KIND_FLAT_VALUE));
}

/**
 * As above, asked of a material: an item that reads a material has no record, so what it lays is
 * decided from its source (#paint_layer_mask_reads_source). The generator, the CPU adapter and the
 * topology hash ask this one.
 */
inline bool paint_layer_mask_item_lays(const Material &ma, const MaterialPaintLayer &item)
{
  if (paint_layer_kind_reads_anchor_point(item)) {
    return paint_layer_row_reads_anchor_point(
        ma, item, paint_layer_anchor_point_mask_read_channel(ma, item));
  }
  if (paint_layer_kind_has_flag(item, PAINT_LAYER_KIND_READS_SOURCE)) {
    return paint_layer_mask_reads_source(ma, item);
  }
  return paint_layer_mask_item_lays(item);
}

/** The Base Color record a map-coverage operator \a item samples, or null when it has none. */
inline const MaterialPaintLayerChannel *paint_layer_map_coverage_record(
    const MaterialPaintLayer &item)
{
  return paint_layer_channel_find(item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
}

/**
 * Whether the map-coverage operator \a item has a map to sample.
 *
 * A record the user switched off (#MA_PAINT_LAYER_CHANNEL_DISABLED) counts as no map, exactly like
 * an absent one: the kind has no user channel switch, so `DISABLED` is the only way a saved file or
 * an edit expresses "the map is gone", and both must read as `match = 0` rather than as an error.
 * The operator takes part either way (see #paint_layer_mask_item_lays); this only decides whether it
 * matches anything.
 */
inline bool paint_layer_map_coverage_has_map(const MaterialPaintLayer &item)
{
  const MaterialPaintLayerChannel *record = paint_layer_map_coverage_record(item);
  return paint_layer_channel_live(record) && record->image != nullptr;
}

/** How many Color ID colours \a item carries; zero for every other kind. */
inline int paint_layer_id_color_count(const MaterialPaintLayer &item)
{
  return item.id_colors.count();
}

/**
 * The constant coverage a mask item's Base Color \a record carries: the mean of its RGB, clamped.
 * The CPU composite and the generator's value input read the one number, so the two cannot
 * disagree about what a constant mask hides.
 */
inline float paint_layer_mask_constant(const MaterialPaintLayerChannel &record)
{
  const float mean = (record.value[0] + record.value[1] + record.value[2]) / 3.0f;
  return clamp_f(mean, 0.0f, 1.0f);
}

/**
 * Whether the mask item \a item reads its coverage as `1 - C`, its map's grey or its flat grey
 * alike, the base mask included. The generator, the CPU adapter and the topology hash all ask this
 * one rule.
 */
inline bool paint_layer_mask_item_inverted(const MaterialPaintLayer &item)
{
  const MaterialPaintLayerChannel *record = paint_layer_channel_find(
      item, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
  return record != nullptr && (record->flag & MA_PAINT_LAYER_CHANNEL_INVERT) != 0;
}

/**
 * Whether the map-coverage item \a item outputs its match scaled by its constant value instead of
 * the plain White/Black result. The value itself is the record's grey, the standard gradient value.
 */
inline bool paint_layer_mask_item_gradient(const MaterialPaintLayer &item)
{
  return (item.flag & MA_PAINT_LAYER_MASK_ITEM_GRADIENT) != 0;
}

/** Whether the Color ID item compares each colour with its own tolerance. */
inline bool paint_layer_mask_item_color_id_advanced(const MaterialPaintLayer &item)
{
  return (item.flag & MA_PAINT_LAYER_MASK_ITEM_COLOR_ID_ADVANCED) != 0;
}

/** Whether the output map carries its coverage in the alpha channel. */
inline bool paint_layer_mask_item_alpha_background(const MaterialPaintLayer &item)
{
  return (item.flag & MA_PAINT_LAYER_MASK_ITEM_ALPHA_BACKGROUND) != 0;
}

/**
 * The `MA_RAMP_*` blend code a description blend stands for.
 *
 * The one table the generator's Mix nodes and the CPU's `ramp_blend` both read, so a blend cannot
 * mean two different things. #MA_PAINT_LAYER_BLEND_NORMAL_COMBINE is not a Mix mode and has no ramp
 * code; it answers #MA_RAMP_BLEND, and the Normal channel never reaches this with it.
 */
int BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend blend);

namespace bke::paint_layers {

/**
 * How much of a row's blend mode is graph shape rather than a node property: the Normal channel's
 * Replace (a different node) and, for a folder, whether it still inlines. Everything else is the
 * `blend_type` of an existing Mix node, kept current by the generator's blend sync.
 */
uint64_t paint_layer_blend_shape(const MaterialPaintLayer &layer);

/**
 * Whether \a layer's \a channel would build a blend node at all in the generator: the Normal
 * channel combines normals instead, an inlined folder's own blend has nowhere to live, and a
 * channel the row does not take part in builds nothing. Runtime blend-tier records are only kept
 * for eligible channels, so the topology hash and the timer never react to a blend that cannot
 * change a node.
 */
bool paint_layer_blend_dynamic_eligible(const Material &ma,
                                        const MaterialPaintLayer &layer,
                                        int channel);

/**
 * A socket of a shared blend group instance by its fixed interface identifier, or by its public
 * name when the interface was authored differently.
 *
 * #bke::node_find_socket matches identifiers, not display names, and the group's identifiers
 * (`Socket_N`) are not its names (`Factor`, `A`, `B`, `Mode`, `Result`). The generator and the
 * blend sync both need the same rule, so it lives here once.
 */
bNodeSocket *paint_layers_blend_group_socket(bNode &instance,
                                             const char *name,
                                             const char *identifier);

/** The bit mask of \a layer's eligible channels currently in the runtime blend tier. */
uint64_t paint_layer_blend_dynamic_mask(const Material &ma, const MaterialPaintLayer &layer);

/** Whether any row, folder or correction of \a ma is currently in the runtime blend tier. */
bool paint_layers_blend_any_dynamic(const Material &ma);

/**
 * Runtime blend-tier state, keyed by the material's session uid and the row's marker. A record
 * means "this channel is being scrubbed": its blend is built with the shared dynamic group, and
 * the record is dropped once its deadline passes. Nothing here is saved to a file.
 */
bool paint_layers_blend_tier_is_dynamic(const Material &ma, const bUUID marker, int channel);
void paint_layers_blend_tier_note(const Material &ma, const bUUID marker, int channel);
void paint_layers_blend_tier_forget(const Material &ma, const bUUID marker, int channel);
void paint_layers_blend_tier_forget_row(const Material &ma, const bUUID marker);
/**
 * Drop records of \a ma whose row or channel no longer builds a blend node. Structural edits other
 * than the blend setters (a folder gaining a mask, a row deleted) can make a channel ineligible; the
 * regeneration point calls this so a stale record cannot pull the channel back into the tier.
 */
void paint_layers_blend_tier_prune(Material &ma);

}  // namespace bke::paint_layers

/* -------------------------------------------------------------------- */
/** \name Row bake
 *
 * What the bake (`paint_layers_bake.cc`) shares with the code that reads the description. A baked
 * row is one decision, #paint_layer_row_substituted, which the generator and the CPU composite both
 * ask, so a row cannot be a cache on one side and live on the other.
 * \{ */

/** The cap on the nodes an AUTO row may cost live; a row that would add more is baked. */
constexpr int PAINT_LAYERS_AUTO_BAKE_NODES = 36;

/**
 * Whether the Anchor Point Reader \a reader takes its slice from the bake in its mode right now:
 * Never stays live, Always bakes, and Auto bakes only over the live limits or with an object mask
 * in the slice (which Auto keeps live, so the mask follows each object).
 */
bool paint_layer_anchor_point_reader_baked(const Material &ma,
                                           const MaterialPaintLayer &reader);

/** What a child whose valid bake stands in costs its parent's weight: one map, like one channel. */
constexpr int PAINT_LAYERS_BAKED_CHILD_WEIGHT = 6;

/**
 * Whether the bake of \a layer stands in for the row right now: it is a Layer row (never a filter
 * or a Pass Through folder), its mode is not Never, nothing inside it keeps it live (a mapping, an
 * object mask) and the stored bake is valid (#BKE_paint_layers_bake_is_valid). The one test for the
 * generator, the CPU composite and the topology hash.
 */
bool paint_layer_row_substituted(const Material &ma, const MaterialPaintLayer &layer);

/**
 * Whether \a layer is substituted and its bake holds a map for \a channel: the row then takes part
 * in the channel through that map and nothing else. \a r_color receives the colour map.
 */
bool paint_layer_row_baked_channel(const Material &ma,
                                   const MaterialPaintLayer &layer,
                                   int channel,
                                   Image **r_color);

/**
 * Start (or restart) the partial-update subscription over the maps \a layer's bake depends on.
 * Called when a bake is written, so pixel edits to those maps are noticed by
 * #BKE_paint_layers_bake_notice_changes.
 */
void BKE_paint_layers_bake_subscribe(Material &ma, MaterialPaintLayer &layer);

/**
 * Drain \a ma's subscriptions: a pixel edit to a source map marks its row's bake changed, the
 * material stale and its generated graph for a rebuild (the row is no longer substituted).
 */
void BKE_paint_layers_bake_notice_changes(Material &ma);

/**
 * The union of the pixel rectangles \a layer's source maps changed in since its last bake, as
 * (xmin, xmax, ymin, ymax); false when nothing is known (the first bake, a full-update answer).
 * The planner uses it to re-bake only the changed tile-sized rectangle.
 */
bool BKE_paint_layers_bake_changed_region(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          int r_region[4]);

/**
 * Bring every light baked row of \a ma current, the synchronous half of the planner: a row that is
 * due and not heavy is rendered through #BKE_paint_layers_bake_render_node into its service maps,
 * stamped and subscribed. Callers run this on the main thread after draining the subscription
 * (#BKE_paint_layers_bake_notice_changes). A heavy row is left to the worker job
 * (#BKE_paint_layers_bake_job_create).
 *
 * \return true when some row was baked or released, so the caller knows the tree needs a rebuild.
 */
bool BKE_paint_layers_bake_plan_run(Main &bmain, Material &ma, bool *r_changed = nullptr);

/**
 * Allocate \a layer's bake cache if it has none and return it, without marking anything: the
 * caller owns the edit and tags the description itself.
 */
MaterialPaintLayerBake *BKE_paint_layers_bake_struct_ensure(MaterialPaintLayer &layer);

/**
 * Point \a layer's baked map for \a channel at \a image (negative \a channel is its coverage
 * map), maintaining the image's user count.
 */
bool BKE_paint_layers_bake_set_map(Material &ma,
                                   MaterialPaintLayer &layer,
                                   int channel,
                                   Image *image);

/** \} */

/** Drop the bake of every reader that no longer resolves. Called after an edit that can change a
 * reference: a point or source row was deleted, or a reader was re-targeted. */
void paint_layer_anchor_point_release_broken_bakes(Material &ma);

}  // namespace blender
