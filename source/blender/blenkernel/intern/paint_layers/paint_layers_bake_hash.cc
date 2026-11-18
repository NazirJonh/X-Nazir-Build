/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The structural half of a row bake's validity, extracted from `paint_layers_bake.cc`: the hash of
 * everything in the description that changes the pixels of the row's result, the mapping and
 * projection folds and blocks that decide whether a row is baked at all, and the validity and
 * finalize entry points that weigh the hash against the stored bake. The pixel half is the
 * subscription (#BKE_paint_layers_bake_notice_changes, `paint_layers_bake_coverage.cc`).
 *
 * Changing what a row's bake contains -- and therefore what enters #bake_hash_layer -- means
 * revisiting `bake_coverage_pointwise` in `paint_layers_bake_coverage.cc`: the two walk the same
 * row of decisions, and a row this hash learns to read past an edited rectangle must stop culling
 * there.
 */

#include <algorithm>
#include <cstdint>

#include "BLI_index_range.hh"
#include "BLI_string.h"
#include "BLI_uuid.h"

#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_asset.hh"
#include "BKE_paint_layers_generator.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_mesh_maps.hh"
#include "BKE_paint_material_composite.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"

#include "paint_layers_bake_intern.hh"
#include "paint_layers_hash_intern.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Validity
 *
 * The hash is the structural half of validity: everything in the description that changes the
 * pixels of the row's result, and nothing that is only a view of them. Pixel edits of the source
 * maps do not move it; the pixel-edit subscription reports those
 * (#BKE_paint_layers_bake_notice_changes, `paint_layers_bake_coverage.cc`).
 * \{ */

static uint64_t bake_hash_idprops(uint64_t h, const IDProperty *group)
{
  if (group == nullptr || group->type != IDP_GROUP) {
    return h;
  }
  for (const IDProperty *prop = static_cast<const IDProperty *>(group->data.group.first);
       prop != nullptr;
       prop = prop->next)
  {
    /* The smart-asset binding labels are metadata, not pixels: making an instance unique or
     * re-binding it must not invalidate its bakes, so the marks never reach the hash. */
    if (STREQ(prop->name, ASSET_PARAMS_PROP_NAME)) {
      continue;
    }
    for (const char *c = prop->name; *c != '\0'; c++) {
      h = bake_hash_mix(h, uint8_t(*c));
    }
    h = bake_hash_mix(h, prop->type);
    switch (prop->type) {
      case IDP_STRING: {
        const char *text = static_cast<const char *>(prop->data.pointer);
        if (text != nullptr) {
          for (const char *c = text; *c != '\0'; c++) {
            h = bake_hash_mix(h, uint8_t(*c));
          }
        }
        break;
      }
      case IDP_GROUP:
        h = bake_hash_idprops(h, prop);
        break;
      default:
        /* Scalars live in the two int words (a double occupies both). */
        h = bake_hash_mix(h, uint32_t(prop->data.val));
        h = bake_hash_mix(h, uint32_t(prop->data.val2));
        break;
    }
  }
  return h;
}

/** Whether \a layer keeps its effects apart (a Material row, an Anchor Point reader). */
static bool bake_row_keeps_effects_apart(const MaterialPaintLayer &layer)
{
  return paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) ||
         paint_layer_kind_reads_anchor_point(layer);
}

/** Whether \a layer carries an enabled CPU-computed effect (Noise, Blur). */
static bool bake_row_has_cpu_effect(const MaterialPaintLayer &layer)
{
  for (const MaterialPaintLayer &effect : layer.effects) {
    if ((effect.flag & MA_PAINT_LAYER_ENABLED) != 0 && paint_layer_effect_is_cpu_computed(effect)) {
      return true;
    }
  }
  return false;
}

/**
 * Whether the mapping of \a layer is folded into its bake instead of keeping the row live: a UV
 * remap (Mirror included) of a row that carries an enabled CPU-computed effect (Noise, Blur), or the
 * projection of such a row (#paint_layer_row_folds_into_bake). Such a row has no live form, so it
 * shows only once baked and substituted, and the CPU composite the bake renders from applies the
 * mapping itself; the substituted group has no mapping inputs, so it is not applied twice. An
 * identity transform with a clipping wrap folds as well, as it only cuts what a bake never holds.
 *
 * \a cpu_in_subtree is #paint_layer_subtree_has_cpu_effect of the row the bake is made for: a
 * projection nested under such a row (a child, a Text Decal, a mask item inside the picture) is
 * folded with it, so the effect runs over what the surface projects.
 */
bool bake_own_mapping_folds(const Material &ma,
                            const MaterialPaintLayer &layer,
                            const bool cpu_in_subtree)
{
  if (BKE_paint_layers_projection_applies(ma, layer)) {
    /* A Mirror on a projected row is a step of the projected read (the CPU resample and the
     * graph), so it never keeps the row live; #bake_hash_projection_fold hashes its axes and
     * center. A nested row folds along with the bake it is rendered in. */
    return paint_layer_node_folds_into_bake(ma, layer, cpu_in_subtree);
  }
  if (!BKE_paint_layers_mapping_remaps(ma, layer)) {
    return false;
  }
  /* A Mirror is folded only where the effects are cached with the row; a source or an Anchor Point
   * reader keeps it live as it always did. */
  if (BKE_paint_layers_mapping_mirror_get(layer) != 0 &&
      (bake_row_keeps_effects_apart(layer) || !bake_row_has_cpu_effect(layer)))
  {
    return false;
  }
  if (BKE_paint_layers_mapping_is_identity(layer)) {
    return true;
  }
  /* Rows that read a source or an Anchor Point cache their effects apart and are kept live. */
  if (bake_row_keeps_effects_apart(layer)) {
    return false;
  }
  return bake_row_has_cpu_effect(layer);
}

/**
 * Whether the transform effects (Mirror, Transform Mapping) of \a layer are rendered into its bake:
 * the CPU composite resamples through them before the CPU effect runs, and the substituted group
 * builds no transform chains, so they must not keep the row live.
 */
static bool bake_effect_transforms_fold(const MaterialPaintLayer &layer)
{
  return !bake_row_keeps_effects_apart(layer) && bake_row_has_cpu_effect(layer);
}

/**
 * The numbers of a projected row that its bake holds: the frame and the mapping the projection is
 * read through. The mesh and the material slots behind the frame belong to the tile identity.
 */
static uint64_t bake_hash_projection_fold(uint64_t h, const MaterialPaintLayer &layer)
{
  const MaterialPaintLayerProjection &frame = *layer.projection;
  for (const float value : frame.center) {
    h = bake_hash_float(h, value);
  }
  for (const float value : frame.size) {
    h = bake_hash_float(h, value);
  }
  for (const float value : frame.normal) {
    h = bake_hash_float(h, value);
  }
  h = bake_hash_float(h, frame.roll);
  h = bake_hash_mix(h, uint8_t(frame.axis));
  h = bake_hash_mix(h, uint16_t(frame.flag));
  h = bake_hash_mix(h, uint16_t(layer.mapping.space));
  h = bake_hash_mix(h, uint16_t(layer.mapping.flag));
  h = bake_hash_mix(h, uint16_t(layer.mapping.wrap));
  h = bake_hash_mix(h, uint8_t(BKE_paint_layers_mapping_mirror_get(layer)));
  for (const float value : layer.mapping.offset) {
    h = bake_hash_float(h, value);
  }
  for (const float value : layer.mapping.scale) {
    h = bake_hash_float(h, value);
  }
  h = bake_hash_float(h, layer.mapping.rotation);
  h = bake_hash_float(h, layer.mapping.mirror_center[0]);
  h = bake_hash_float(h, layer.mapping.mirror_center[1]);
  return h;
}

static uint64_t bake_hash_layer(uint64_t h,
                                const Material &ma,
                                const MaterialPaintLayer &layer,
                                const bool is_child,
                                const bool parent_cpu_in_subtree = false)
{
  /* Whether a CPU effect runs over this bake's picture: asked once at the row the bake is made for
   * and handed down, so a projection nested at any depth is hashed (it is part of what the cache
   * holds) exactly when #bake_mapping_blocks lets it through. False for every stack that has no such
   * effect, which keeps the hash of every older bake as it was. */
  const bool cpu_in_subtree = is_child ? parent_cpu_in_subtree :
                                         paint_layer_subtree_has_cpu_effect(layer);
  /* A content bake's mask, its opacities and its blend are live over the substituted map, so none
   * of them may move the hash of what was rendered; a child of another row's cache keeps its mask
   * inside, being part of the picture that cache holds (#paint_layer_bake_scope). */
  const PaintLayerBakeScope scope = paint_layer_bake_scope(layer, is_child);
  /* An effect that owns its result bakes that result alone: the effect's opacity (its own and its
   * per-channel one) and its visibility are the graph's opacity input over the map, so none of them
   * may move the hash -- switching the effect off and on again must find the map still current. */
  const MaterialPaintLayer *owned_row = nullptr;
  if (!is_child && layer.role == MA_PAINT_LAYER_ROLE_EFFECT &&
      paint_layer_effect_is_cpu_computed(layer))
  {
    owned_row = BKE_paint_layers_correction_owner(ma, layer);
    if (owned_row != nullptr && !paint_layer_effect_owns_result(ma, *owned_row, layer)) {
      owned_row = nullptr;
    }
  }
  const bool owns_result = owned_row != nullptr;
  /* Bumped when what a bake stores changes meaning, so older caches invalidate once. */
  h = bake_hash_mix(h, uint64_t(0x9e3779b97f4a7c15ull));
  h = bake_hash_mix(h, uint8_t(layer.source));
  h = bake_hash_mix(h, uint16_t(layer.kind_subtype));
  /* Which geometry map a MESH_MAP row reads decides its result; only those rows carry the field.
   * The atlas Image's identity is folded in when the owning material is known, so pointing the slot
   * at another Image invalidates the row's bake while a pixel edit (which does not change the UID)
   * does not. */
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    h = bake_hash_mix(h, uint8_t(layer.mesh_map_type));
    const MaterialMeshMapSlot *slot = BKE_mesh_maps_slot_find(ma, layer.mesh_map_type);
    h = bake_hash_mix(h,
                      (slot != nullptr && slot->image != nullptr) ?
                          uint64_t(slot->image->id.session_uid) :
                          uint64_t(0));
  }
  if (BKE_paint_layers_gradient_supported(layer) && layer.gradient != nullptr) {
    /* A Gradient's pixels are its payload: the line, the shape and the ramp. The channel records'
     * value ramps below carry the scalar channels. The leading version invalidates the caches if
     * the generated formula changes. */
    h = bake_hash_mix(h, BKE_PAINT_LAYERS_GRADIENT_HASH_VERSION);
    h = bake_hash_mix(h, uint8_t(layer.gradient->type));
    h = bake_hash_mix(h, uint8_t(layer.gradient->space));
    h = bake_hash_mix(h, uint8_t(layer.gradient->repeat));
    for (const float value : layer.gradient->start) {
      h = bake_hash_float(h, value);
    }
    for (const float value : layer.gradient->end) {
      h = bake_hash_float(h, value);
    }
    h = bake_hash_float(h, BKE_paint_layers_gradient_midpoint_effective(*layer.gradient));
    h = bake_hash_mix(h, BKE_paint_layers_gradient_ramp_hash(layer.gradient->ramp));
  }
  /* A row's own blend is applied over its cache, so it moves no hash of that cache. A child's is
   * inside the result its parent bakes, so it does. */
  if (is_child) {
    h = bake_hash_mix(h, uint8_t(layer.blend));
  }
  /* The non-visibility flags of the row itself. Visibility is appended only for a child, so the
   * row's own cache does not depend on its own on/off state. The lock is left out with it: it
   * changes no pixel, so putting it in would invalidate a bake for nothing. */
  h = bake_hash_mix(h,
                    uint16_t(layer.flag & ~(int(MA_PAINT_LAYER_ENABLED) |
                                            int(MA_PAINT_LAYER_LOCKED))));
  h = bake_hash_mix(h, uint8_t(layer.role));
  /* Whether the opacity stays live over the cached map rather than being folded into it: a
   * content-only bake holds the content alone, and a mask effect's grey is cached before its own
   * opacity -- the reader lays that live over the map. A child is rendered inside the parent's
   * picture, so the parent's opacity is baked with it. */
  const bool opacity_is_live_over_map = scope == PaintLayerBakeScope::ContentOnly ||
                                        (paint_layer_mask_item_bakes_effect(layer) && !is_child) ||
                                        owns_result;
  if (!opacity_is_live_over_map) {
    h = bake_hash_float(h, layer.opacity);
  }
  /* A mask item's contrast reshapes its coverage inside the parent's bake, like the opacity does. */
  h = bake_hash_float(h, layer.mask_contrast);
  /* Only the fact that the mapping applies: such a row stays live and is never baked
   * (#BKE_paint_layers_bake_blocked), so the slider values must not move the hash. A Material row
   * is rendered and its mapping is applied when its maps are read, so toggling it must not move a
   * parent folder's hash either; the folder is kept live by the same block while the mapping is
   * on. */
  if (!paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    h = bake_hash_mix(h, BKE_paint_layers_mapping_remaps(ma, layer) ? 1 : 0);
    /* A mapping folded into the bake is part of what the cache holds, so its values are hashed:
     * moving the gizmo must invalidate the bake. */
    if (bake_own_mapping_folds(ma, layer, cpu_in_subtree)) {
      h = bake_hash_float(h, layer.mapping.offset[0]);
      h = bake_hash_float(h, layer.mapping.offset[1]);
      h = bake_hash_float(h, layer.mapping.scale[0]);
      h = bake_hash_float(h, layer.mapping.scale[1]);
      h = bake_hash_float(h, layer.mapping.rotation);
      h = bake_hash_mix(h, uint16_t(BKE_paint_layers_mapping_wrap_get(layer)));
      /* Only a mirrored row adds to the hash, so a bake saved before keeps its value. */
      if (const int mirror = BKE_paint_layers_mapping_mirror_get(layer)) {
        h = bake_hash_mix(h, uint8_t(mirror));
        h = bake_hash_float(h, layer.mapping.mirror_center[0]);
        h = bake_hash_float(h, layer.mapping.mirror_center[1]);
      }
    }
    /* A projected row with a CPU effect holds the frame and the mapping in its cache. Reached only
     * while #PAINT_LAYERS_BAKE_PROJECTED_EFFECT is on, so the hash of every other row is as it was.
     * NOTE: the surface identity of the tile (mesh content, UV name, and the material slot
     * signature of #composite_projection_surface_identity) joins the tile identity in
     * #paint_layers_bake_row_identity, not this hash. Every node of the subtree that projects is
     * hashed (a child's, a nested correction's, a mask item's), not only the row's own. */
    if (paint_layer_node_folds_into_bake(ma, layer, cpu_in_subtree)) {
      h = bake_hash_projection_fold(h, layer);
    }
  }
  /* An effect in its Decal frame is rendered into the cache, unlike a UV mapping that keeps the row
   * live, so every number of its step and of its frame is part of what the cache holds. The frame
   * decides the surface patch the step walks; the mesh behind it is the tile identity's. */
  /* A transform item of a mask is rendered into its cache the same way: its grey is the mask moved
   * by the step, in UV, so the step's numbers are what the cache holds. */
  if (paint_layer_kind_transforms_input(layer) &&
      (layer.mapping.space == MA_PAINT_LAYER_MAPPING_SPACE_DECAL ||
       layer.role == MA_PAINT_LAYER_ROLE_MASK_ITEM))
  {
    h = bake_hash_mix(h, uint16_t(layer.mapping.space));
    h = bake_hash_mix(h, uint16_t(layer.mapping.flag));
    h = bake_hash_mix(h, uint16_t(layer.mapping.wrap));
    h = bake_hash_float(h, layer.mapping.offset[0]);
    h = bake_hash_float(h, layer.mapping.offset[1]);
    h = bake_hash_float(h, layer.mapping.scale[0]);
    h = bake_hash_float(h, layer.mapping.scale[1]);
    h = bake_hash_float(h, layer.mapping.rotation);
    h = bake_hash_float(h, layer.mapping.mirror_center[0]);
    h = bake_hash_float(h, layer.mapping.mirror_center[1]);
    h = bake_hash_mix(h, layer.projection != nullptr ? 1 : 0);
    if (layer.projection != nullptr) {
      const MaterialPaintLayerProjection &frame = *layer.projection;
      for (const float value : frame.center) {
        h = bake_hash_float(h, value);
      }
      for (const float value : frame.size) {
        h = bake_hash_float(h, value);
      }
      for (const float value : frame.normal) {
        h = bake_hash_float(h, value);
      }
      h = bake_hash_float(h, frame.roll);
      h = bake_hash_mix(h, uint8_t(frame.axis));
      h = bake_hash_mix(h, uint16_t(frame.flag));
    }
  }
  for (int i = 0; i < layer.channels_num; i++) {
    const MaterialPaintLayerChannel &record = layer.channels[i];
    /* A channel outside the material's set is not part of the result, so its record must not move
     * the hash; re-adding the channel changes the hash and forces the re-bake then. */
    if (paint_layer_channel_filtered(ma, record.channel)) {
      continue;
    }
    h = bake_hash_mix(h, uint8_t(record.channel));
    h = bake_hash_mix(h, uint8_t(record.state));
    h = bake_hash_mix(h, uint8_t(record.flag));
    /* How the map is read is baked into the result: a glossiness or an F0 colour lands in the
     * cache as the channel's value, so the switch has to retire it. */
    h = bake_hash_mix(h, uint8_t(record.map_encoding));
    h = bake_hash_mix(h, record.image != nullptr ? record.image->id.session_uid : 0);
    for (const float value : record.value) {
      h = bake_hash_float(h, value);
    }
    if (record.gradient_ramp != nullptr) {
      h = bake_hash_mix(h, BKE_paint_layers_gradient_ramp_hash(*record.gradient_ramp));
    }
  }
  /* The per (row, channel) opacity overrides are baked into the row's result too, and exist whether
   * or not the pair has a record. The blend follows the same rule as the row's own: a child's is
   * inside the parent's result, the row's own is applied over its cache. */
  for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
    const MaterialPaintLayerChannelSettings &settings = layer.channel_settings[channel];
    if (is_child) {
      h = bake_hash_mix(h, uint8_t(settings.blend));
    }
    if (scope != PaintLayerBakeScope::ContentOnly && !owns_result) {
      h = bake_hash_float(h, settings.opacity);
    }
    /* The Height record's own mode and scale are baked into the row's Height result the same way,
     * and the Compare mask reaches every other channel of the row through the stack that renders
     * one -- the whole row's cache is stale without them. The effective mode includes the row's
     * ordinary blend: with Inherit, that blend decides the Height step itself, inside the cache, so
     * the row's own blend moves the cache even though the colour blend does not. */
    h = bake_hash_mix(h,
                      uint8_t(BKE_paint_layers_channel_height_blend_effective(layer, channel)));
    h = bake_hash_float(h, settings.height_scale_delta);
    h = bake_hash_mix(h, uint8_t(settings.height_flag));
  }
  /* The Color ID colours of a map-coverage item decide its match. */
  for (const MaterialPaintLayerIdColor &color : layer.id_colors) {
    h = bake_hash_mix(h, uint8_t(color.flag));
    for (const float component : color.color) {
      h = bake_hash_float(h, component);
    }
    h = bake_hash_float(h, color.tolerance);
  }
  /* A child's own bake size is not part of its parent's pixels, and allocating or releasing the
   * child's cache would otherwise invalidate the parent for no visible change. */
  if (!is_child) {
    /* A generated row has no size of its own: its pixels are laid on the material's canvas (see
     * #bake_size_for), so that canvas is what the cache was rendered at. Hashing the stored size
     * instead would keep a bake made before a resize, and the preview would resample it. */
    const uint32_t bake_size =
        paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_GENERATES) ?
            uint32_t(std::max(int(ma.paint_layers_image_size), 0)) :
            uint32_t(layer.bake != nullptr ? layer.bake->size : 0);
    h = bake_hash_mix(h, bake_size);
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    /* Inside a baked folder a Material row is read through the maps it shows, so which source and
     * which maps it has move the folder's cache; their pixels are the subscription's. */
    h = bake_hash_mix(h, layer.material != nullptr ? layer.material->id.session_uid : 0);
    h = bake_hash_mix(h, paint_layer_material_live_signature(ma, layer));
    if (layer.bake != nullptr) {
      for (const Image *image : layer.bake->images) {
        h = bake_hash_mix(h, image != nullptr ? image->id.session_uid : 0);
      }
      h = bake_hash_mix(h,
                        layer.bake->coverage != nullptr ? layer.bake->coverage->id.session_uid : 0);
    }
  }
  if (paint_layer_kind_reads_anchor_point(layer)) {
    /* The slice's description, channel by channel: what it is made of, never its pixels (those are
     * the subscription's). A cache for one channel therefore follows an edit above its marker. */
    for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
      h = bake_hash_mix(h, paint_layer_anchor_point_slice_hash(ma, layer, channel));
    }
  }
  if (paint_layer_mask_item_bakes_effect(layer)) {
    /* The grey the item bakes reads the mask built up to it: its description folds in. Zero when
     * the item is in no mask stack, which folds nothing in -- the mix step stays here, beside the
     * other hashes. */
    if (const uint64_t input_hash = bake_mask_effect_input_hash(ma, layer)) {
      h = bake_hash_mix(h, input_hash);
    }
  }
  if (owns_result) {
    /* The effect is computed from the row's content below it: the description of that input folds
     * in, beside the effect's own parameters hashed below. The pixels are the subscription's. */
    h = bake_hash_mix(h, paint_layer_owned_effect_input_hash(ma, *owned_row, layer));
  }
  /* The image of a kind's own slot (a Channel Split) is what its result is made of: another image
   * is another bake. The pixels are the subscription's. */
  paint_layer_effect_hash_mix_slot(h, layer);
  /* A kind's parameters (a filter's, a coverage operator's) change the pixels. A ramp filter keeps
   * its stops outside the properties, in its own payload; a curves filter its Bézier points. */
  h = bake_hash_idprops(h, layer.system_properties);
  if (layer.ramp != nullptr) {
    h = bake_hash_mix(h, paint_layer_ramp_hash(layer.ramp->band));
  }
  if (layer.curves != nullptr) {
    h = bake_hash_mix(h, paint_layer_curves_hash(layer.curves->curve_mapping));
  }
  /* The folded numbers reach every stack hash through the filter parameters, but the bake reads the
   * payloads directly, so a matrix or balance is part of what the baked map was made from. */
  h = bake_hash_mix(h, paint_layer_payload_hash(layer));
  /* Effects first, then mask items, then children, each in its own storage order. */
  const bool transforms_fold = bake_effect_transforms_fold(layer);
  for (const MaterialPaintLayer &effect : layer.effects) {
    h = bake_hash_layer(h, ma, effect, true, cpu_in_subtree);
    /* A transform effect folded into this row's bake moves its pixels (Decal-frame ones are hashed
     * above). Only one that remaps adds to the hash, as the others never blocked a bake. */
    if (transforms_fold && paint_layer_kind_transforms_input(effect) &&
        effect.mapping.space != MA_PAINT_LAYER_MAPPING_SPACE_DECAL &&
        BKE_paint_layers_mapping_remaps(ma, effect))
    {
      h = bake_hash_mix(h, uint16_t(effect.mapping.space));
      h = bake_hash_mix(h, uint16_t(effect.mapping.flag));
      h = bake_hash_mix(h, uint16_t(BKE_paint_layers_mapping_wrap_get(effect)));
      h = bake_hash_mix(h, uint8_t(BKE_paint_layers_mapping_mirror_get(effect)));
      h = bake_hash_float(h, effect.mapping.offset[0]);
      h = bake_hash_float(h, effect.mapping.offset[1]);
      h = bake_hash_float(h, effect.mapping.scale[0]);
      h = bake_hash_float(h, effect.mapping.scale[1]);
      h = bake_hash_float(h, effect.mapping.rotation);
      h = bake_hash_float(h, effect.mapping.mirror_center[0]);
      h = bake_hash_float(h, effect.mapping.mirror_center[1]);
    }
  }
  /* The mask of a content bake is laid live over the map and reaches none of its pixels: a stroke
   * into it must leave the cache current. */
  if (scope != PaintLayerBakeScope::ContentOnly) {
    for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
      h = bake_hash_layer(h, ma, mask_item, true, cpu_in_subtree);
    }
  }
  for (const MaterialPaintLayer &child : layer.children) {
    h = bake_hash_layer(h, ma, child, true, cpu_in_subtree);
  }
  /* The row's own visibility is a live factor and must not move its own bake. A parent's bake
   * renders its children, so a child's visibility does change the parent's result. */
  if (is_child) {
    h = bake_hash_mix(h, (layer.flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1 : 0);
  }
  return h;
}

/**
 * The structural hash of a Material row: which material it reads, the side its maps are rendered at
 * and which channels the material asks for. Nothing of the source's content, which only the main
 * thread may read (see #BKE_paint_layers_material_row_stale).
 */
static uint64_t bake_hash_material_row(const Material &ma, const MaterialPaintLayer &layer)
{
  uint64_t h = 1469598103934665603ull;
  h = bake_hash_mix(h, uint64_t(0x4d4154u));
  h = bake_hash_mix(h, layer.material != nullptr ? layer.material->id.session_uid : 0);
  h = bake_hash_mix(h, layer.bake != nullptr ? uint32_t(layer.bake->size) : 0);
  h = bake_hash_mix(h, BKE_paint_layers_channel_set_mask_get(ma));
  return h;
}

void BKE_paint_layers_bake_hash(const Material &ma,
                                const MaterialPaintLayer &layer,
                                uint32_t r_hash[2])
{
  /* A Material row's maps belong to its source and its size; its opacity, blend, mask and
   * corrections stay live over them and are not part of what was rendered. */
  const uint64_t hash = paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE) ?
                            bake_hash_material_row(ma, layer) :
                            bake_hash_layer(1469598103934665603ull, ma, layer, false);
  r_hash[0] = uint32_t(hash & 0xFFFFFFFFu);
  r_hash[1] = uint32_t(hash >> 32);
}

bool BKE_paint_layers_bake_is_valid(const Material &ma, const MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr) {
    return false;
  }
  if (bake_drag_reduced_owes(ma, layer)) {
    /* Baked below its size for a drag that is over: the full bake is owed. */
    return false;
  }
  if (paint_layer_kind_has_flag(layer, PAINT_LAYER_KIND_READS_SOURCE)) {
    /* Reads the source's node trees: main thread only. The generator and the CPU composite ask
     * #paint_layer_row_material_map instead, which reads the row alone. */
    /* A row with nothing to render is as current as it can be. */
    return !BKE_paint_layers_material_row_needs_render(ma, layer) ||
           (paint_layer_row_material_shown(ma, layer) &&
            !BKE_paint_layers_material_row_stale(ma, layer));
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  if (hash[0] != layer.bake->hash[0] || hash[1] != layer.bake->hash[1] ||
      (hash[0] == 0 && hash[1] == 0))
  {
    return false;
  }
  if (!bake_node_is_shared(ma, layer) && !bake_tiles_ready(ma, layer)) {
    return false;
  }
  /* The structural hash cannot see pixel edits; the runtime subscription, drained by
   * #BKE_paint_layers_bake_notice_changes, reports those. A layer with no subscription is trusted
   * on its hash alone. */
  std::lock_guard lock(bake_runtime_mutex());
  const BakeMaterialRuntime *runtime = bake_runtime().lookup_ptr(ma.id.session_uid);
  if (runtime != nullptr) {
    for (const BakeLayerSubscription &entry : runtime->layers) {
      if (BLI_uuid_equal(entry.marker, layer.marker) && entry.changed) {
        return false;
      }
    }
  }
  return true;
}

void BKE_paint_layers_bake_finalize(Material &ma, MaterialPaintLayer &layer)
{
  if (layer.bake == nullptr || !bake_row_owned(ma, layer)) {
    return;
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(ma, layer, hash);
  layer.bake->hash[0] = hash[0];
  layer.bake->hash[1] = hash[1];
  BKE_paint_layers_bake_subscribe(ma, layer);
}

/**
 * Whether \a layer or anything under it applies a UV mapping or a projection: a mapped row stays live.
 *
 * \a scope is what the bake the question is asked for holds (#PaintLayerBakeScope): the own mask
 * of a content bake is laid live over the map, where the graph reads a mapped mask as well as the
 * CPU composite does, so a mapping there is no reason to keep the row out of the bake. Everything
 * rendered into the cache keeps blocking, and so does the row's own mapping.
 */
static bool bake_mapping_blocks(const Material &ma,
                                const MaterialPaintLayer &layer,
                                const PaintLayerBakeScope scope,
                                const bool cpu_in_subtree)
{
  /* A plain UV mapping of a CPU-computed row (Noise, Blur) is folded into its bake rather than
   * keeping it live: such a row shows only when baked and substituted
   * (#bake_own_mapping_folds). A projection folds when the row it is rendered in has a CPU effect
   * in its subtree (#paint_layer_node_folds_into_bake): the effect then runs over what the surface
   * projects. Without one the stack keeps its live graph, so the Auto bake never starts for a
   * projected stack that had no effect to bake. */
  if ((BKE_paint_layers_mapping_remaps(ma, layer) &&
       !bake_own_mapping_folds(ma, layer, cpu_in_subtree)) ||
      (BKE_paint_layers_projection_applies(ma, layer) &&
       !paint_layer_node_folds_into_bake(ma, layer, cpu_in_subtree)))
  {
    return true;
  }
  const bool transforms_fold = bake_effect_transforms_fold(layer);
  for (const MaterialPaintLayer &effect : layer.effects) {
    /* A Mirror or Transform Mapping effect of a row that bakes a CPU effect is rendered into the
     * bake, like the row's own mapping. */
    if (transforms_fold && paint_layer_kind_transforms_input(effect)) {
      continue;
    }
    /* An effect in its Decal frame is what the bake renders (CPU effect-space resampling over the
     * object's surface), so it must not keep the row live; the graph leaves it out of the row anyway
     * (#BKE_paint_layers_active_uv_transform_effects), so the baked map is not transformed twice. */
    if (BKE_paint_layers_effect_decal_applies(ma, layer, effect)) {
      continue;
    }
    if (bake_mapping_blocks(ma, effect, PaintLayerBakeScope::Child, cpu_in_subtree)) {
      return true;
    }
  }
  if (scope != PaintLayerBakeScope::ContentOnly) {
    for (const MaterialPaintLayer &mask_item : layer.mask_stack) {
      if (bake_mapping_blocks(ma, mask_item, PaintLayerBakeScope::Child, cpu_in_subtree)) {
        return true;
      }
    }
  }
  for (const MaterialPaintLayer &child : layer.children) {
    if (bake_mapping_blocks(ma, child, PaintLayerBakeScope::Child, cpu_in_subtree)) {
      return true;
    }
  }
  return false;
}

/** Whether any descendant of \a layer has an object mask: the choice is applied in the shader, after every shared cache. */
static bool bake_object_mask_blocks(const MaterialPaintLayer &layer)
{
  for (const MaterialPaintLayer &child : layer.children) {
    if ((child.object_mask_flag & MA_PAINT_LAYER_OBJECT_MASK_ACTIVE) != 0 ||
        bake_object_mask_blocks(child))
    {
      return true;
    }
  }
  return false;
}

bool BKE_paint_layers_bake_blocked(const Material &ma, const MaterialPaintLayer &layer)
{
  /* A row holding a read Anchor Point (or a folder above one) is never substituted: the cache
   * would swallow the point. A reader whose slice contains an object mask stays live when its mode
   * allows it; the effective-mode helper decides which. */
  return bake_mapping_blocks(ma,
                             layer,
                             paint_layer_bake_scope(layer, false),
                             paint_layer_subtree_has_cpu_effect(layer)) ||
         bake_object_mask_blocks(layer) || paint_layer_anchor_point_blocks_bake(ma, layer);
}

/** \} */

}  // namespace blender
