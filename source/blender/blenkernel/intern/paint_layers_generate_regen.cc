#include "paint_layers_generate_intern.hh"
/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The paint-layer generator: `Material::paint_layers` (the DNA description) becomes a node tree.
 *
 * `paint_layers_tree_build` is the topology half: interface, per-layer nodes and the links between
 * them, with no #Main involved. `BKE_paint_layers_regenerate` is the #Main-side half: it owns the
 * generated group, the instance node in the material's embedded tree, and the routing into the
 * Principled BSDF.
 *
 * Topology and values are separated: the channel chains and the interface are a function of the
 * description's *structure* alone, while the animatable values (`opacity`, `enabled`, a Fill
 * constant) are inputs of each layer group's own interface. Their current values sit on that
 * group's instance node in its parent tree and are copied there by #BKE_paint_layers_values_sync
 * (called from the material evaluation too).
 */

#include "BKE_paint_layers_debug.hh"
#include "BKE_paint_layers_generate.hh"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_threads.h"
#include "BLI_time.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_global.hh"
#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "IMB_colormanagement.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "paint_layers_intern.hh"

#include "NOD_socket.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "paint_material_composite_internal.hh"


namespace blender {
namespace bke::paint_layers {

#if PAINT_LAYERS_DEBUG_LOG

const char *material_mode_name(const PaintLayerMaterialMode mode)
{
  switch (mode) {
    case PaintLayerMaterialMode::Hybrid:
      return "Hybrid";
    case PaintLayerMaterialMode::SourceGroup:
      return "SourceGroup";
    case PaintLayerMaterialMode::Baked:
      return "Baked";
  }
  return "Baked";
}

const char *source_group_refusal_name(const PaintLayersSourceGroupRefusal refusal)
{
  return BKE_paint_layers_source_group_refusal_name(refusal);
}

/**
 * The previous pass's per-row modes, keyed by material `session_uid`, so the diagnostic below logs
 * a row only when its state changed. A static map rather than material runtime: the state has to
 * survive a runtime free and is a debug aid, and a session holds only a handful of layered
 * materials.
 */
Map<uint32_t, Vector<PaintLayersRegenerateReport::MaterialRowModeReport>> &previous_row_modes()
{
  static Map<uint32_t, Vector<PaintLayersRegenerateReport::MaterialRowModeReport>> map;
  return map;
}

const PaintLayersRegenerateReport::MaterialRowModeReport *find_previous_row_mode(
    const Vector<PaintLayersRegenerateReport::MaterialRowModeReport> &rows, const bUUID &marker)
{
  for (const PaintLayersRegenerateReport::MaterialRowModeReport &row : rows) {
    if (BLI_uuid_equal(row.marker, marker)) {
      return &row;
    }
  }
  return nullptr;
}

/** Log one line per Material row whose mode, wrapper, refusal or deferred state changed. */
void material_row_modes_log(
    const Material &ma, const Vector<PaintLayersRegenerateReport::MaterialRowModeReport> &rows)
{
  Vector<PaintLayersRegenerateReport::MaterialRowModeReport> &previous =
      previous_row_modes().lookup_or_add_default(ma.id.session_uid);
  for (const PaintLayersRegenerateReport::MaterialRowModeReport &row : rows) {
    const PaintLayersRegenerateReport::MaterialRowModeReport *old = find_previous_row_mode(
        previous, row.marker);
    if (old != nullptr && old->mode == row.mode && old->wrapper_built == row.wrapper_built &&
        old->refusal == row.refusal && old->deferred == row.deferred)
    {
      continue;
    }
    char group_depth[16];
    if (row.mode == PaintLayerMaterialMode::SourceGroup) {
      SNPRINTF(group_depth, "%d", row.group_depth);
    }
    else {
      STRNCPY(group_depth, "-");
    }
    printf("paint layers: row '%s' owner='%s' owner_tag=0x%x source='%s' source_uid=%u "
           "deferred=%d mode=%s wrapper=%s refusal=%s group_depth=%s\n",
           row.name,
           ma.id.name + 2,
           static_cast<unsigned int>(ma.id.tag),
           row.source_name,
           row.source_uid,
           row.deferred ? 1 : 0,
           material_mode_name(row.mode),
           row.wrapper_built ? "yes" : "no",
           source_group_refusal_name(row.refusal),
           group_depth);
  }
  previous_row_modes().add_overwrite(ma.id.session_uid, rows);
}

/** What a SourceGroup row actually embedded, kept to log only the rows whose state changed. */
struct SourceGroupEmbedState {
  bUUID marker;
  char name[64];
  char layer_tree[MAX_ID_NAME - 2];
  int wrapper_instances = 0;
  int linked_color_outputs = 0;
  int linked_coverage = 0;
};

Map<uint32_t, Vector<SourceGroupEmbedState>> &previous_source_group_embeds()
{
  static Map<uint32_t, Vector<SourceGroupEmbedState>> map;
  return map;
}

const SourceGroupEmbedState *find_previous_embed(const Vector<SourceGroupEmbedState> &states,
                                                 const bUUID &marker)
{
  for (const SourceGroupEmbedState &state : states) {
    if (BLI_uuid_equal(state.marker, marker)) {
      return &state;
    }
  }
  return nullptr;
}

/**
 * Log what a SourceGroup row actually embedded: its layer group, how many instances of the source
 * wrapper it holds and how many of the wrapper's `COLOR:<CHANNEL>`/`COVERAGE` outputs are linked.
 * A row that is live but shows nothing is the exact failure this diagnostic is meant to catch.
 */
void source_group_instances_log(
    const Material &ma,
    const Map<const MaterialPaintLayer *, bNodeTree *> &layer_trees,
    const Map<const Material *, bNodeTree *> &source_groups,
    const PaintLayersRegenCache *cache)
{
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  Vector<SourceGroupEmbedState> states;
  for (const MaterialPaintLayer *layer : layers) {
    if (layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL ||
        BKE_paint_layers_material_mode(ma, *layer, cache) != PaintLayerMaterialMode::SourceGroup)
    {
      continue;
    }
    SourceGroupEmbedState state;
    state.marker = layer->marker;
    STRNCPY(state.name, layer->name);
    bNodeTree *layer_tree = layer_trees.lookup_default(layer, nullptr);
    bNodeTree *wrapper = (layer->material != nullptr) ?
                             source_groups.lookup_default(layer->material, nullptr) :
                             nullptr;
    STRNCPY(state.layer_tree, (layer_tree != nullptr) ? layer_tree->id.name + 2 : "none");
    if (layer_tree != nullptr && wrapper != nullptr) {
      layer_tree->ensure_topology_cache();
      wrapper->ensure_interface_cache();
      for (bNode &node : layer_tree->nodes) {
        if (!node.is_group() || node.id != &wrapper->id) {
          continue;
        }
        state.wrapper_instances++;
        for (bNodeSocket &out : node.outputs) {
          const bNodeTreeInterfaceSocket *iface = nullptr;
          for (const bNodeTreeInterfaceSocket *candidate : wrapper->interface_outputs()) {
            if (candidate->identifier != nullptr && STREQ(candidate->identifier, out.identifier)) {
              iface = candidate;
              break;
            }
          }
          if (iface == nullptr) {
            continue;
          }
          const char *role = prop_string_get(iface->properties, PAINT_LAYERS_CUSTOM_ROLE_PROP);
          if (role == nullptr || out.directly_linked_links().is_empty()) {
            continue;
          }
          if (STRPREFIX(role, "COLOR:")) {
            state.linked_color_outputs++;
          }
          else if (STREQ(role, "COVERAGE")) {
            state.linked_coverage++;
          }
        }
      }
    }
    states.append(state);
  }
  Vector<SourceGroupEmbedState> &previous =
      previous_source_group_embeds().lookup_or_add_default(ma.id.session_uid);
  for (const SourceGroupEmbedState &state : states) {
    const SourceGroupEmbedState *old = find_previous_embed(previous, state.marker);
    if (old != nullptr && STREQ(old->layer_tree, state.layer_tree) &&
        old->wrapper_instances == state.wrapper_instances &&
        old->linked_color_outputs == state.linked_color_outputs &&
        old->linked_coverage == state.linked_coverage)
    {
      continue;
    }
    printf("paint layers: row '%s' layer_tree='%s' wrapper_instances=%d linked_color_outputs=%d "
           "linked_coverage=%d\n",
           state.name,
           state.layer_tree,
           state.wrapper_instances,
           state.linked_color_outputs,
           state.linked_coverage);
  }
  previous_source_group_embeds().add_overwrite(ma.id.session_uid, states);
}

#endif /* PAINT_LAYERS_DEBUG_LOG */
uint64_t topology_hash_layer(uint64_t hash,
                             const Material &ma,
                             const MaterialPaintLayer &layer,
                             Span<int> wired_channels,
                             const PaintLayersRegenCache *cache);

/** Mix \a value into the FNV-1a style accumulator; order-sensitive, so lists hash in order. */
uint64_t topology_hash_mix(uint64_t hash, const uint64_t value)
{
  hash ^= value;
  hash *= 1099511628211ull;
  return hash;
}

/**
 * The set of source materials the `MATERIAL` rows of \a ma read, as one order-independent hash:
 * the sources' `session_uid`s, sorted and deduplicated so a reorder or a repeated source changes
 * nothing. Zero when no row reads a source. The depsgraph builds a relation per distinct source,
 * so a difference here is exactly a change of the graph's relations.
 */
uint64_t paint_layers_source_materials_hash(const Material &ma)
{
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  Vector<uint32_t> source_uids;
  for (const MaterialPaintLayer *layer : layers) {
    if (layer->source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer->material != nullptr) {
      source_uids.append(layer->material->id.session_uid);
    }
  }
  if (source_uids.is_empty()) {
    return 0;
  }
  std::sort(source_uids.begin(), source_uids.end());
  uint64_t hash = 0;
  bool first = true;
  uint32_t previous = 0;
  for (const uint32_t uid : source_uids) {
    if (!first && uid == previous) {
      continue;
    }
    hash = topology_hash_mix(hash, uid);
    previous = uid;
    first = false;
  }
  return hash;
}

void topology_hash_string(uint64_t &hash, const char *text)
{
  if (text == nullptr) {
    hash = topology_hash_mix(hash, 0);
    return;
  }
  for (const char *c = text; *c != '\0'; c++) {
    hash = topology_hash_mix(hash, uint8_t(*c));
  }
  hash = topology_hash_mix(hash, 1);
}

void topology_hash_uid(uint64_t &hash, const bUUID &uid)
{
  char formatted[UUID_STRING_SIZE];
  BLI_uuid_format(formatted, uid);
  topology_hash_string(hash, formatted);
}

uint64_t topology_hash_map_id(const Image *image)
{
  return (image != nullptr) ? image->id.session_uid : 0;
}

/**
 * The topology of one effect or mask item: everything that decides the nodes it contributes.
 * Opacity and a Fill constant are inputs, so they are not here.
 */
uint64_t topology_hash_correction(uint64_t hash,
                                  const Material &ma,
                                  const MaterialPaintLayer &correction,
                                  const Span<int> wired_channels,
                                  const bool mask_item,
                                  const PaintLayersRegenCache *cache)
{
  topology_hash_uid(hash, correction.marker);
  /* The correction's name reaches the group's mirror input names for its value inputs. */
  topology_hash_string(hash, correction.name);
  hash = topology_hash_mix(hash, uint64_t(uint8_t(correction.role)));
  hash = topology_hash_mix(hash, uint64_t(uint8_t(correction.source)));
  if (correction.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    /* Which geometry map the row reads is topology: another map is another node. */
    hash = topology_hash_mix(hash, uint64_t(uint8_t(correction.mesh_map_type)));
  }
  hash = topology_hash_mix(hash, uint64_t(uint8_t(correction.blend)));
  /* Visibility is a value: a disabled effect or mask item stays in the chain with opacity zero
   * (#BKE_paint_layers_effective_opacity), so it must not move this hash. */
  const bool fill = BKE_paint_layers_source_type(correction) == PaintLayerSourceType::Constant;
  hash = topology_hash_mix(hash, fill ? 1 : 0);
  const bool mask_material_or_group = mask_item &&
                                      ELEM(correction.source,
                                           MA_PAINT_LAYER_SOURCE_MATERIAL,
                                           MA_PAINT_LAYER_SOURCE_NODE_GROUP,
                                           MA_PAINT_LAYER_SOURCE_STACK);
  if (mask_item && !fill && !mask_material_or_group) {
    /* Whether the mask map is read as colour data decides whether the chain builds its Divide: a
     * data texture is left pre-multiplied by the Image Texture node, a non-data one is straightened
     * there. A change of the map's colorspace must therefore rebuild the group. */
    const Image *mask_image = paint_layer_mask_correction_image(
        ma, correction, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    const bool data = mask_image != nullptr &&
                      IMB_colormanagement_space_name_is_data(mask_image->colorspace_settings.name);
    hash = topology_hash_mix(hash, data ? 1 : 0);
  }
  if (mask_material_or_group) {
    /* Which channel the mask reads is topology (another channel is another sub-graph: a
     * different socket, a different reduction -- RGBTOBW, Separate X, or the coverage itself). A
     * Normal channel builds nothing at all (the item is skipped), so nothing past the channel
     * number itself can move this hash. */
    hash = topology_hash_mix(hash, uint64_t(uint8_t(correction.mask_channel)));
    if (correction.mask_channel != PAINT_MATERIAL_CHANNEL_NORMAL) {
      if (correction.source == MA_PAINT_LAYER_SOURCE_STACK) {
        /* A Stack mask item's topology is its children's: each child's own full topology
         * (#topology_hash_layer already loops every wired channel for it), depth-first, exactly
         * like a Layer folder's own children below. A value edit inside the subtree (opacity, a
         * Fill constant) does not move this hash, since #topology_hash_layer excludes those the
         * same way it does for a top-level row. */
        for (const MaterialPaintLayer &child :
             correction.children)
        {
          hash = topology_hash_layer(hash, ma, child, wired_channels, cache);
        }
      }
      else if (correction.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
        /* Mirrors the content-correction hash above and #topology_hash_layer: a live constant or
         * live map is topology, and so is the Baked/Hybrid/SourceGroup mode and, in SourceGroup,
         * the source's own topology. */
        float live_value[4];
        const bool live_constant = BKE_paint_layers_material_live_constant(
            ma, correction, correction.mask_channel, live_value, cache);
        Image *live_map_image_probe = nullptr;
        const ImageUser *live_map_iuser_probe = nullptr;
        const bool live_map_probe = !live_constant &&
                                    BKE_paint_layers_material_live_image(
                                        ma, correction, correction.mask_channel,
                                        &live_map_image_probe, &live_map_iuser_probe, cache);
        hash = topology_hash_mix(hash, live_constant ? 1 : 0);
        hash = topology_hash_mix(hash, live_map_probe ? 1 : 0);
        if (live_map_probe) {
          hash = topology_hash_mix(hash, topology_hash_map_id(live_map_image_probe));
        }
        const PaintLayerMaterialMode material_mode = BKE_paint_layers_material_mode(
            ma, correction, cache);
        hash = topology_hash_mix(hash, uint64_t(material_mode));
        if (material_mode == PaintLayerMaterialMode::SourceGroup &&
            correction.material != nullptr)
        {
          hash = topology_hash_mix(
              hash, BKE_paint_layers_source_material_topology_hash(*correction.material));
        }
        if (!live_constant && !live_map_probe) {
          /* Baked fallback: #paint_layer_channel_image already has a Material branch. */
          const Image *baked_mask_image = paint_layer_channel_image(
              ma, correction, correction.mask_channel);
          hash = topology_hash_mix(hash, topology_hash_map_id(baked_mask_image));
        }
      }
      else {
        /* Node Group is Baked-only, like its content-correction counterpart above. */
        Image *correction_baked = nullptr;
        const bool has_bake =
            BKE_paint_layers_bake_substitute(
                ma, correction, correction.mask_channel, &correction_baked) ||
            BKE_paint_layers_bake_substitute_custom(
                ma, correction, correction.mask_channel, &correction_baked, nullptr);
        hash = topology_hash_mix(hash, has_bake ? 1 : 0);
        hash = topology_hash_mix(hash, has_bake ? topology_hash_map_id(correction_baked) : 0);
      }
    }
  }
  if (!mask_item && correction.source == MA_PAINT_LAYER_SOURCE_STACK) {
    /* A content (Effect) Stack correction's topology is its children's, exactly like the Mask Item
     * branch above and a Layer folder's own children below: each child's full topology, depth-first,
     * value edits inside excluded the same way #topology_hash_layer excludes them everywhere else. */
    for (const MaterialPaintLayer &child :
         correction.children)
    {
      hash = topology_hash_layer(hash, ma, child, wired_channels, cache);
    }
  }
  for (const int channel : wired_channels) {
    hash = topology_hash_mix(hash, uint64_t(channel));
    /* A content correction blends by its channel override; a mask item by the row blend. */
    hash = topology_hash_mix(
        hash, uint64_t(BKE_paint_layers_channel_blend_effective(correction, channel)));
    const Image *image = mask_item ?
                             paint_layer_mask_correction_image(ma, correction, channel) :
                             paint_layer_channel_image(ma, correction, channel);
    hash = topology_hash_mix(hash, topology_hash_map_id(image));
    if (!mask_item && !fill) {
      /* A content effect reads its own map per channel; whether that map is data decides whether
       * the chain builds a Divide (see the mask branch above). */
      const bool data = image != nullptr &&
                        IMB_colormanagement_space_name_is_data(image->colorspace_settings.name);
      hash = topology_hash_mix(hash, data ? 1 : 0);
    }
    if (!mask_item && correction.source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
      /* Mirrors the Layer-row hash (#topology_hash_layer): a live constant or live map is topology
       * (it decides whether the group carries a live-constant input, or shows a texture instead of
       * the baked one), and so is the Baked/Hybrid/SourceGroup mode and, in SourceGroup, the
       * source's own topology. The value behind a live constant is not topology (ТЗ-26): it syncs
       * through the group input in place. */
      float live_value[4];
      const bool live_constant = BKE_paint_layers_material_live_constant(
          ma, correction, channel, live_value, cache);
      Image *live_map_image_probe = nullptr;
      const ImageUser *live_map_iuser_probe = nullptr;
      const bool live_map_probe = !live_constant &&
                                  BKE_paint_layers_material_live_image(
                                      ma, correction, channel, &live_map_image_probe,
                                      &live_map_iuser_probe, cache);
      hash = topology_hash_mix(hash, live_constant ? 1 : 0);
      hash = topology_hash_mix(hash, live_map_probe ? 1 : 0);
      if (live_map_probe) {
        hash = topology_hash_mix(hash, topology_hash_map_id(live_map_image_probe));
      }
      const PaintLayerMaterialMode material_mode = BKE_paint_layers_material_mode(
          ma, correction, cache);
      hash = topology_hash_mix(hash, uint64_t(material_mode));
      if (material_mode == PaintLayerMaterialMode::SourceGroup && correction.material != nullptr) {
        hash = topology_hash_mix(
            hash, BKE_paint_layers_source_material_topology_hash(*correction.material));
      }
    }
    if (!mask_item && correction.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
      /* A Node Group correction is Baked-only (no live path exists for it, see the generator's own
       * comment on this branch): whether it currently has a valid bake, and which image, is what
       * decides whether the group carries the TEX_IMAGE node at all. #paint_layer_channel_image has
       * no Node Group branch (unlike Material), so the per-channel hash above never saw this. */
      Image *correction_baked = nullptr;
      const bool has_bake =
          BKE_paint_layers_bake_substitute(ma, correction, channel, &correction_baked) ||
          BKE_paint_layers_bake_substitute_custom(ma, correction, channel, &correction_baked, nullptr);
      hash = topology_hash_mix(hash, has_bake ? 1 : 0);
      hash = topology_hash_mix(hash, has_bake ? topology_hash_map_id(correction_baked) : 0);
    }
  }
  /* A Fill effect with live records carries one constant input per recorded channel; which channels
   * those are is topology. A no-record Fill (or a mask item) keeps the single socket, so it hashes
   * exactly as before this branch existed. */
  if (fill && !mask_item && correction_has_live_channel_records(ma, correction)) {
    hash = topology_hash_mix(hash, 1);
    for (const int channel : wired_channels) {
      hash = topology_hash_mix(hash, uint64_t(channel));
      hash = topology_hash_mix(
          hash, correction_channel_record_live(ma, correction, channel) ? 1 : 0);
    }
  }
  return hash;
}

#if PAINT_LAYERS_DEBUG_LOG
/** Set while #paint_layers_layer_topology_hash hashes a Material row, so its parts are printed. */
static thread_local bool topology_hash_trace_active = false;
#  define PL_HASH_TRACE(...) \
    do { \
      if (topology_hash_trace_active) { \
        printf(__VA_ARGS__); \
      } \
    } while (false)
#else
#  define PL_HASH_TRACE(...) ((void)0)
#endif

/**
 * The full topology hash of one row, recursing into its effects, mask items and -- for a folder --
 * its children. A folder's own chain is a function of which children take part in which channel and
 * of their grouping, so a child's topology is part of the folder's; rebuilding a folder group does
 * not touch the child groups' own nodes, so the two levels still skip independently.
 */
uint64_t topology_hash_layer(uint64_t hash,
                             const Material &ma,
                             const MaterialPaintLayer &layer,
                             const Span<int> wired_channels,
                             const PaintLayersRegenCache *cache)
{
  const bool is_folder = BKE_paint_layers_is_folder(layer);
  hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.source)));
  if (layer.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    /* Which geometry map the row reads is topology: another map is another node. */
    hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.mesh_map_type)));
  }
  hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.blend)));
  hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.role)));
  /* Visibility is a value: disabling leaves the row in the graph with factor zero, so the flag is
   * not part of topology. A rebuild for another reason may drop the row (see the removed-rows set),
   * and enabling it back force-invalidates the stored root hash and rebuilds it in. */
  hash = topology_hash_mix(hash, is_folder ? 1 : 0);
  hash = topology_hash_mix(hash, row_is_substituted(ma, layer) ? 1 : 0);
  /* The name reaches the mirror input names a preserved group carries, so a rename invalidates. */
  topology_hash_string(hash, layer.name);
  PL_HASH_TRACE("paint layers hash: '%s' head=%llx substituted=%d\n",
                layer.name,
                static_cast<unsigned long long>(hash),
                int(row_is_substituted(ma, layer)));

  for (const int channel : wired_channels) {
    Image *baked = nullptr;
    const bool substituted = row_channel_substituted(ma, layer, channel, &baked);
    /* #layer_row_has_group, not the narrower #leaf_participates: for a Material row the latter
     * only asks whether a bake map exists (#paint_layer_channel_present), so before any bake it
     * disagreed with the live source the build actually shows -- #layer_row_has_group is the one
     * predicate the build and the root hash already share (see its own doc comment), and folding a
     * fresh, still-live hand-over's map into `present` must not flip this bit either. */
    const bool participates = is_folder ? layer_subtree_has_channel(ma, layer, channel, cache) :
                                          layer_row_has_group(ma, layer, channel, cache);
    hash = topology_hash_mix(hash, uint64_t(channel));
    hash = topology_hash_mix(hash, substituted ? 1 : 0);
    hash = topology_hash_mix(hash, participates ? 1 : 0);
    hash = topology_hash_mix(
        hash, uint64_t(BKE_paint_layers_channel_blend_effective(layer, channel)));
    float live_value[4];
    const bool live_constant = BKE_paint_layers_material_live_constant(
        ma, layer, channel, live_value, cache);
    Image *live_map_image_probe = nullptr;
    const ImageUser *live_map_iuser_probe = nullptr;
    const bool live_map_probe = BKE_paint_layers_material_live_image(
        ma, layer, channel, &live_map_image_probe, &live_map_iuser_probe, cache);
    /* The build reads this row's own channel map (#paint_layer_channel_image -- for a Material
     * row that is its bake target, #paint_layer_material_source_map) only once the channel is not
     * shown live from the source: the build's channel loop `continue`s past that map entirely for
     * a live constant or a live map (see the Hybrid branch there). Hashing the map's identity while
     * the channel is still live would rebuild this row's group on every hand-over of a fresh bake
     * target (#BKE_paint_layers_material_bake_apply), even though the graph never references it
     * until the row actually turns Baked. Non-Material rows have no such live path, so their map
     * always counts. */
    const PaintLayerMaterialMode material_mode = (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) ?
                                                     BKE_paint_layers_material_mode(ma, layer, cache) :
                                                     PaintLayerMaterialMode::Baked;
    /* A SourceGroup row goes through the source wrapper instance and never samples the channel map
     * (the build passes a null image when `source_group_instance` is set), so a fresh bake target
     * must not rebuild the group. The mode itself is hashed below, so leaving SourceGroup still
     * changes the hash. A substituted channel keeps its own map hash further down. */
    const bool image_wired = (layer.source != MA_PAINT_LAYER_SOURCE_MATERIAL ||
                              !(live_constant || live_map_probe)) &&
                             material_mode != PaintLayerMaterialMode::SourceGroup;
    hash = topology_hash_mix(
        hash,
        image_wired ? topology_hash_map_id(paint_layer_channel_image(ma, layer, channel)) : 0);
    /* Whether the channel is currently shown live as a constant is topology (it decides whether the
     * row's group carries a live-constant input at all); the constant's own value is not (ТЗ-26): it
     * is a group input, filled by #create_value_inputs and kept current by #values_sync_socket via
     * #BKE_paint_layers_material_live_constant, so a source slider move syncs in place instead of
     * rebuilding this group. */
    hash = topology_hash_mix(hash, live_constant ? 1 : 0);
    hash = topology_hash_mix(hash, live_map_probe ? 1 : 0);
    if (live_map_probe) {
      /* Which map the row shows is topology, like any other map a row reads. */
      hash = topology_hash_mix(hash, topology_hash_map_id(live_map_image_probe));
    }
    hash = topology_hash_mix(hash, uint64_t(material_mode));
    if (material_mode == PaintLayerMaterialMode::SourceGroup && layer.material != nullptr) {
      /* Only the source's topology is part of this group's topology; a value edit is synced into
       * the wrapper in place and must not rebuild the row's group. */
      hash = topology_hash_mix(
          hash, BKE_paint_layers_source_material_topology_hash(*layer.material));
    }
    if (substituted) {
      hash = topology_hash_mix(hash, topology_hash_map_id(baked));
      hash = topology_hash_mix(
          hash,
          (layer.bake != nullptr) ? topology_hash_map_id(layer.bake->coverage) : 0);
    }
    PL_HASH_TRACE(
        "paint layers hash: '%s' ch=%d subst=%d part=%d blend=%d live_const=%d live_map=%d "
        "image_wired=%d map=%s mode=%d run=%llx\n",
        layer.name,
        channel,
        int(substituted),
        int(participates),
        int(BKE_paint_layers_channel_blend_effective(layer, channel)),
        int(live_constant),
        int(live_map_probe),
        int(image_wired),
        paint_layer_channel_image(ma, layer, channel) != nullptr ?
            paint_layer_channel_image(ma, layer, channel)->id.name + 2 :
            "-",
        int(material_mode),
        static_cast<unsigned long long>(hash));
  }
  /* The warm items stand in the chain like real ones, so consuming or replenishing one moves the
   * group's hash and rebuilds it. */
  for (const MaterialPaintLayer *effect : paint_layers_build_effects(ma, layer)) {
    hash = topology_hash_correction(hash, ma, *effect, wired_channels, false, cache);
    PL_HASH_TRACE("paint layers hash: '%s' effect '%s' src=%d run=%llx\n",
                  layer.name,
                  effect->name,
                  int(effect->source),
                  static_cast<unsigned long long>(hash));
  }
  for (const MaterialPaintLayer *mask_item : paint_layers_build_mask_items(ma, layer)) {
    hash = topology_hash_correction(hash, ma, *mask_item, wired_channels, true, cache);
    PL_HASH_TRACE("paint layers hash: '%s' mask item '%s' src=%d flag=%d run=%llx\n",
                  layer.name,
                  mask_item->name,
                  int(mask_item->source),
                  int(mask_item->flag),
                  static_cast<unsigned long long>(hash));
  }
  for (const MaterialPaintLayer &child :
       layer.children)
  {
    hash = topology_hash_layer(hash, ma, child, wired_channels, cache);
  }
  return hash;
}


bUUID tree_owner_uid_get(const bNodeTree &tree)
{
  bUUID uid = BLI_uuid_nil();
  uid_prop_get(tree.id.properties, TREE_OWNER_PROP, uid);
  return uid;
}

void tree_owner_uid_set(bNodeTree &tree, const bUUID &uid)
{
  uid_prop_set(tree.id.properties, TREE_OWNER_PROP, uid);
}

void instance_uid_set(bNode &node, const bUUID &uid)
{
  uid_prop_set(node.prop, INSTANCE_OWNER_PROP, uid);
}

/** Write \a hash as two 32-bit words; IDProperty has no 64-bit integer type. */
void tree_hash_set(bNodeTree &tree,
                   const char *low_key,
                   const char *high_key,
                   const uint64_t hash)
{
  prop_int_set(tree.id.properties, low_key, int(uint32_t(hash & 0xFFFFFFFFu)));
  prop_int_set(tree.id.properties, high_key, int(uint32_t(hash >> 32)));
}

/** Read a stored hash; false when the tree has never been stamped under those keys. */
bool tree_hash_get(bNodeTree &tree,
                   const char *low_key,
                   const char *high_key,
                   uint64_t &r_hash)
{
  const IDProperty *props = IDP_GetProperties(&tree.id);
  if (props == nullptr) {
    return false;
  }
  const IDProperty *low = IDP_GetPropertyTypeFromGroup(props, low_key, IDP_INT);
  const IDProperty *high = IDP_GetPropertyTypeFromGroup(props, high_key, IDP_INT);
  if (low == nullptr || high == nullptr) {
    return false;
  }
  r_hash = (uint64_t(uint32_t(IDP_int_get(high))) << 32) | uint64_t(uint32_t(IDP_int_get(low)));
  return true;
}

void tree_topology_hash_set(bNodeTree &tree, const uint64_t hash)
{
  tree_hash_set(tree, TREE_TOPOLOGY_LOW_PROP, TREE_TOPOLOGY_HIGH_PROP, hash);
}

bool tree_topology_hash_get(bNodeTree &tree, uint64_t &r_hash)
{
  return tree_hash_get(tree, TREE_TOPOLOGY_LOW_PROP, TREE_TOPOLOGY_HIGH_PROP, r_hash);
}

void tree_root_hash_set(bNodeTree &tree, const uint64_t hash)
{
  tree_hash_set(tree, TREE_ROOT_TOPOLOGY_LOW_PROP, TREE_ROOT_TOPOLOGY_HIGH_PROP, hash);
}

bool tree_root_hash_get(bNodeTree &tree, uint64_t &r_hash)
{
  return tree_hash_get(tree, TREE_ROOT_TOPOLOGY_LOW_PROP, TREE_ROOT_TOPOLOGY_HIGH_PROP, r_hash);
}

bNode *instance_find(const Material &ma, const bUUID &owner_uid)
{
  if (ma.nodetree == nullptr) {
    return nullptr;
  }
  for (bNode &node : ma.nodetree->nodes) {
    bUUID node_uid = BLI_uuid_nil();
    if (uid_prop_get(node.prop, INSTANCE_OWNER_PROP, node_uid) &&
        BLI_uuid_equal(node_uid, owner_uid))
    {
      return &node;
    }
  }
  return nullptr;
}

/** Remove every node of \a tree, leaving its interface untouched. */
void tree_clear_nodes(Main &bmain, bNodeTree &tree)
{
  Vector<bNode *> nodes;
  for (bNode &node : tree.nodes) {
    nodes.append(&node);
  }
  for (bNode *node : nodes) {
    bke::node_remove_node(&bmain, tree, *node, true);
  }
}

void tree_clear(Main &bmain, bNodeTree &tree)
{
  tree_clear_nodes(bmain, tree);
  tree.tree_interface.clear_items();
}

/** Delete the source-group wrappers of \a owner whose source no row reads any more. */
void source_groups_prune(Main &bmain, const Material &owner);

/** A layer group's own marker (the layer it stands for), or false when the tree is not one. */
bool layer_tree_marker_get(const bNodeTree &tree, bUUID &r_marker)
{
  return uid_prop_get(tree.id.properties, TREE_LAYER_PROP, r_marker);
}

/** Collect the layer groups a tree holds, descending into nested ones; folders will nest. */
void layer_trees_collect(bNodeTree &tree,
                         const bUUID &owner_uid,
                         Vector<bNodeTree *> &r_trees)
{
  for (bNode &node : tree.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr || !BLI_uuid_equal(tree_owner_uid_get(*group), owner_uid)) {
      continue;
    }
    bUUID marker = BLI_uuid_nil();
    if (!layer_tree_marker_get(*group, marker) || r_trees.contains(group)) {
      continue;
    }
    r_trees.append(group);
    layer_trees_collect(*group, owner_uid, r_trees);
  }
}

/** Copy the layer groups of a ripped tree, re-pointing the nodes at the copies. */
void layer_trees_copy(Main *bmain,
                      bNodeTree &tree,
                      const bUUID &src_owner,
                      const bUUID &dst_owner)
{
  for (bNode &node : tree.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr || !BLI_uuid_equal(tree_owner_uid_get(*group), src_owner)) {
      continue;
    }
    bUUID marker = BLI_uuid_nil();
    if (!layer_tree_marker_get(*group, marker)) {
      continue;
    }
    bNodeTree *group_copy = id_cast<bNodeTree *>(BKE_id_copy(bmain, &group->id));
    if (group_copy == nullptr) {
      continue;
    }
    /* #BKE_id_copy hands back a reference of its own; the node below is the user. */
    id_us_min(&group_copy->id);
    id_us_min(node.id);
    node.id = &group_copy->id;
    id_us_plus(&group_copy->id);
    tree_owner_uid_set(*group_copy, dst_owner);
    layer_trees_copy(bmain, *group_copy, src_owner, dst_owner);
  }
}

/** The Material Output node of \a ma's embedded tree, or null. */
bNode *material_output_find(Material &ma)
{
  if (ma.nodetree == nullptr) {
    return nullptr;
  }
  for (bNode &node : ma.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_OUTPUT_MATERIAL) {
      return &node;
    }
  }
  return nullptr;
}

/** The Principled input a channel routes into, or null when there is none. */
bNodeSocket *principled_channel_socket(Material &ma, const int channel)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason);
  if (principled == nullptr) {
    return nullptr;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.socket_name == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(
      const_cast<bNode &>(*principled), SOCK_IN, UString::from_ptr_noinline(info.socket_name));
}

/** A Normal Map node in \a ma, created when missing. */
bNode *normal_map_ensure(Material &ma)
{
  for (bNode &node : ma.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_NORMAL_MAP) {
      return &node;
    }
  }
  bNode *node = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_NORMAL_MAP);
  if (node != nullptr) {
    node->location[0] = 400.0f;
    node->location[1] = -400.0f;
  }
  return node;
}

/** A Bump node in \a ma, created when missing. */
bNode *bump_ensure(Material &ma)
{
  for (bNode &node : ma.nodetree->nodes) {
    if (node.type_legacy == SH_NODE_BUMP) {
      return &node;
    }
  }
  bNode *node = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_BUMP);
  if (node != nullptr) {
    node->location[0] = 400.0f;
    node->location[1] = -560.0f;
  }
  return node;
}

/** Ensure a Principled exists and drives the Surface input, per C-5. */
void principled_ensure(Material &ma, PaintLayersRegenerateReport &r_report)
{
  if (ma.nodetree == nullptr) {
    return;
  }
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  if (BKE_paint_material_principled_find(ma, reason) != nullptr) {
    return;
  }
  bNode *output = material_output_find(ma);
  if (output == nullptr) {
    output = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_OUTPUT_MATERIAL);
    if (output == nullptr) {
      r_report.no_principled = true;
      return;
    }
    output->location[0] = 300.0f;
  }
  bNodeSocket *surface = socket_in(*output, "Surface");
  if (surface == nullptr) {
    r_report.no_principled = true;
    return;
  }
  /* A Surface taken by something other than a Principled is the user's graph: never replaced. */
  if (!surface->directly_linked_links().is_empty()) {
    r_report.no_principled = true;
    return;
  }
  bNode *principled = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_BSDF_PRINCIPLED);
  if (principled == nullptr) {
    r_report.no_principled = true;
    return;
  }
  principled->location[0] = 0.0f;
  principled->location[1] = 0.0f;
  bke::node_add_link(
      *ma.nodetree, *principled, *socket_out(*principled, "BSDF"), *output, *surface);
  r_report.created_principled = true;
}

/**
 * Link \a from_socket into \a input, replacing whatever was there.
 *
 * An input the generator owns is restored in Locked mode (default); in Unlocked mode a foreign
 * link is left alone and reported. A link already exactly of ours is a no-op.
 */
void input_link_restore(Material &ma,
                        bNodeSocket &input,
                        bNode &from_node,
                        bNodeSocket &from_socket,
                        PaintLayersRegenerateReport &r_report)
{
  const bool locked = (ma.paint_layers_flag & MA_PAINT_LAYERS_LOCKED) != 0;
  const Span<bNodeLink *> links = input.directly_linked_links();
  if (!links.is_empty()) {
    if (links[0]->fromnode == &from_node && links[0]->fromsock == &from_socket) {
      return;
    }
    if (!locked) {
      r_report.skipped_foreign_inputs = true;
      return;
    }
    r_report.restored_inputs = true;
  }
  bke::node_remove_socket_links(*ma.nodetree, input);
  bke::node_add_link(*ma.nodetree, from_node, from_socket, input.owner_node(), input);
}

/** The instance output socket behind the group output named \a result_name, or null. */
bNodeSocket *instance_output_find(bNodeTree &tree, bNode &instance, const char *result_name)
{
  tree.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *iface : tree.interface_outputs()) {
    if (iface->name != nullptr && STREQ(iface->name, result_name) && iface->identifier != nullptr) {
      return bke::node_find_socket(
          instance, SOCK_OUT, UString::from_ptr_noinline(iface->identifier));
    }
  }
  return nullptr;
}

/** Route every generated output into the material, channel by channel. */
void wire_instance_to_material(Material &ma,
                               bNodeTree &tree,
                               bNode &instance,
                               PaintLayersRegenerateReport &r_report)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled_const = BKE_paint_material_principled_find(ma, reason);
  if (principled_const == nullptr) {
    r_report.no_principled = true;
    return;
  }
  bNode &principled = const_cast<bNode &>(*principled_const);

  for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
    if (info.channel == PAINT_MATERIAL_CHANNEL_CUSTOM ||
        info.channel == PAINT_MATERIAL_CHANNEL_AO)
    {
      continue;
    }
    char result_name[64];
    SNPRINTF(result_name, "Result %s", info.ui_name);
    bNodeSocket *instance_out = instance_output_find(tree, instance, result_name);
    if (instance_out == nullptr) {
      continue;
    }

    if (info.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      bNode *normal_map = normal_map_ensure(ma);
      if (normal_map == nullptr) {
        continue;
      }
      /* The node was just created on a tree whose topology cache predates it.
       * #input_link_restore reads a socket's owner through that cache, so refresh it before the
       * new node's sockets are used. */
      ma.nodetree->ensure_topology_cache();
      bNodeSocket *map_color = socket_in(*normal_map, "Color");
      bNodeSocket *map_normal = socket_out(*normal_map, "Normal");
      bNodeSocket *principled_normal = socket_in(principled, "Normal");
      if (map_color == nullptr || map_normal == nullptr || principled_normal == nullptr) {
        continue;
      }
      input_link_restore(ma, *map_color, instance, *instance_out, r_report);
      input_link_restore(ma, *principled_normal, *normal_map, *map_normal, r_report);
      continue;
    }
    if (info.channel == PAINT_MATERIAL_CHANNEL_HEIGHT) {
      bNode *bump = bump_ensure(ma);
      if (bump == nullptr) {
        continue;
      }
      /* See the Normal branch: the freshly created node is not in the topology cache yet. */
      ma.nodetree->ensure_topology_cache();
      bNodeSocket *bump_height = socket_in(*bump, "Height");
      bNodeSocket *bump_normal_out = socket_out(*bump, "Normal");
      bNodeSocket *principled_normal = socket_in(principled, "Normal");
      if (bump_height == nullptr || bump_normal_out == nullptr || principled_normal == nullptr) {
        continue;
      }
      input_link_restore(ma, *bump_height, instance, *instance_out, r_report);
      /* Height rides over the Normal stack: the Bump's Normal input takes whatever currently feeds
       * the Principled Normal, be that the Normal Map or a hand-built graph. */
      bNodeSocket *bump_normal_in = socket_in(*bump, "Normal");
      if (bump_normal_in != nullptr) {
        const Span<bNodeLink *> normal_links = principled_normal->directly_linked_links();
        if (normal_links.size() == 1) {
          input_link_restore(ma,
                             *bump_normal_in,
                             *normal_links[0]->fromnode,
                             *normal_links[0]->fromsock,
                             r_report);
        }
      }
      input_link_restore(ma, *principled_normal, *bump, *bump_normal_out, r_report);
      continue;
    }

    bNodeSocket *target = principled_channel_socket(ma, info.channel);
    if (target == nullptr) {
      continue;
    }
    input_link_restore(ma, *target, instance, *instance_out, r_report);
  }
}

/**
 * Re-declare every generated group instance reachable from \a tree so its sockets match its
 * group's current interface. A group that gained inputs (a new effect or mask) while the tree that
 * instantiates it was kept would otherwise be stale, and the node tree update's enum/interface pass
 * assumes the instance's inputs line up with the group's interface.
 */
void refresh_generated_instances(bNodeTree &tree, const bUUID &owner_uid, Set<bNodeTree *> &visited)
{
  if (!visited.add(&tree)) {
    return;
  }
  for (bNode &node : tree.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr || !BLI_uuid_equal(tree_owner_uid_get(*group), owner_uid)) {
      continue;
    }
    nodes::update_node_declaration_and_sockets(tree, node);
    refresh_generated_instances(*group, owner_uid, visited);
  }
}

}  // namespace bke::paint_layers
using namespace bke::paint_layers;

bool BKE_paint_layers_row_removed_clear(Material &ma, const bUUID &marker)
{
  bke::MaterialPaintLayersRuntime *runtime = bke::paint_layers_runtime_mutable(ma);
  if (runtime == nullptr) {
    return false;
  }
  for (int i = 0; i < runtime->removed_rows.size(); i++) {
    if (BLI_uuid_equal(runtime->removed_rows[i], marker)) {
      runtime->removed_rows.remove(i);
      return true;
    }
  }
  return false;
}

void BKE_paint_layers_root_hash_invalidate(Material &ma)
{
  if (ma.paint_layers_tree != nullptr) {
    tree_root_hash_set(*ma.paint_layers_tree, 0);
  }
}

uint64_t paint_layers_layer_topology_hash(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          const Span<int> wired_channels,
                                          const PaintLayersRegenCache *cache)
{
  uint64_t hash = 1469598103934665603ull;
  /* The UV layer a group's Image Texture nodes read is topology: changing it must rebuild them. */
  topology_hash_string(hash, BKE_paint_layers_uv_map_name(ma));
#if PAINT_LAYERS_DEBUG_LOG
  /* Only Material rows are traced: the extra regeneration after adding one is what is being chased. */
  const bool trace = layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL;
  const bool trace_before = topology_hash_trace_active;
  topology_hash_trace_active = trace;
  const uint64_t result = topology_hash_layer(hash, ma, layer, wired_channels, cache);
  PL_HASH_TRACE("paint layers hash: '%s' FINAL=%llx wired=%d\n",
                layer.name,
                static_cast<unsigned long long>(result),
                int(wired_channels.size()));
  topology_hash_trace_active = trace_before;
  return result;
#else
  return topology_hash_layer(hash, ma, layer, wired_channels, cache);
#endif
}

/**
 * The generated group tree of \a marker reachable from \a scope, or null. A nested child of a folder
 * that was reused untouched was never re-ensured this pass, so it is absent from the factory's
 * `layer_trees` map; the value-input hash finds it here instead, and so does not depend on whether
 * the folder happened to rebuild.
 */
static bNodeTree *generated_layer_tree_find(const bNodeTree &scope, const bUUID &marker)
{
  for (const bNode &node : scope.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr) {
      continue;
    }
    bUUID group_marker = BLI_uuid_nil();
    if (!uid_prop_get(group->id.properties, TREE_LAYER_PROP, group_marker)) {
      continue;
    }
    if (BLI_uuid_equal(group_marker, marker)) {
      return group;
    }
    if (bNodeTree *found = generated_layer_tree_find(*group, marker)) {
      return found;
    }
  }
  return nullptr;
}

/**
 * The hash of everything the root tree's own nodes, links and interface depend on: the wired
 * channels, the top-level rows and their per-channel participation, the interface signature of every
 * top-level layer group's *contract* (its Below inputs and its outputs), and the *source* value
 * inputs of every layer group (the root carries a mirror per value, so adding or removing one changes
 * the root; the mirror sockets themselves are not hashed). A rebuilt group's internals are otherwise
 * absent, so a map or correction edit inside one layer leaves the root hash unchanged and the root is
 * kept; a value edit changes a socket's default value, never this set.
 */
uint64_t paint_layers_root_topology_hash(
    const Material &ma,
    const Span<int> wired_channels,
    const Map<const MaterialPaintLayer *, bNodeTree *> &layer_trees,
    const PaintLayersRegenCache *cache)
{
  uint64_t hash = 1469598103934665603ull;
  /* The UV layer every generated Image Texture reads is topology: the root's nodes gain a UV Map
   * node when it is set, so the name is part of what the root is built from. */
  topology_hash_string(hash, BKE_paint_layers_uv_map_name(ma));
  for (const int channel : wired_channels) {
    hash = topology_hash_mix(hash, uint64_t(channel));
  }
  /* Top-level rows in order, and which of them take part in each channel. A Pass Through folder
   * contributes nothing of its own, so its children are hashed exactly as if the folder were absent;
   * that is what lets moving a row into one leave this hash -- and the kept root -- unchanged. */
  auto hash_row_list = [&](auto &&self, const ListBaseT<MaterialPaintLayer> &list) -> void {
    for (const MaterialPaintLayer &layer :
         list)
    {
      if (BKE_paint_layers_folder_is_pass_through(ma, layer)) {
        if (!row_is_removed(ma, layer)) {
          self(self, layer.children);
        }
        continue;
      }
      topology_hash_uid(hash, layer.marker);
      for (const int channel : wired_channels) {
        const bool has_group = layer_row_has_group(ma, layer, channel, cache);
        hash = topology_hash_mix(hash, has_group ? 1 : 0);
      }
    }
  };
  hash_row_list(hash_row_list, ma.paint_layers);
  /* Every layer group's interface: names and types in creation order. It decides the root's
   * instance sockets and the links between them. */
  /* Only the groups the root instantiates count: the top-level rows, seen through Pass Through
   * folders exactly as in the walk above. The children of an isolating folder live inside that
   * folder's group, whose own interface is hashed here and whose topology hash already covers its
   * children. They are also present in \a layer_trees only when the folder's group was rebuilt in
   * this pass -- an untouched folder is reused without asking for its children -- so hashing them
   * made the value depend on whether the folder happened to rebuild, and the next pass rebuilt the
   * root again. */
  Vector<const MaterialPaintLayer *> layers;
  auto collect_root_layers = [&](auto &&self, const ListBaseT<MaterialPaintLayer> &list) -> void {
    for (const MaterialPaintLayer &layer :
         list)
    {
      if (BKE_paint_layers_folder_is_pass_through(ma, layer)) {
        if (!row_is_removed(ma, layer)) {
          self(self, layer.children);
        }
        continue;
      }
      layers.append(&layer);
    }
  };
  collect_root_layers(collect_root_layers, ma.paint_layers);
  for (const MaterialPaintLayer *layer : layers) {
    bNodeTree *const *tree_ptr = layer_trees.lookup_ptr(layer);
    if (tree_ptr == nullptr || *tree_ptr == nullptr) {
      continue;
    }
    hash = topology_hash_mix(hash, 1);
    (*tree_ptr)->tree_interface.foreach_item([&](const bNodeTreeInterfaceItem &item) {
      if (item.item_type != NodeTreeInterfaceItemType::Socket) {
        return true;
      }
      const auto &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
      /* A value input is owned by the group; it never reaches the root's nodes or links. */
      if ((socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0 &&
          prop_string_get(socket.properties, INPUT_ROLE_PROP) != nullptr)
      {
        return true;
      }
      hash = topology_hash_mix(
          hash, (socket.flag & NODE_INTERFACE_SOCKET_INPUT) != 0 ? 1 : 2);
      topology_hash_string(hash, socket.name);
      topology_hash_string(hash, socket.socket_type);
      return true;
    });
  }

  /* Every value input a generated group owns is topology now: the root carries one mirror per value
   * in the whole stack, so adding or removing a value input changes the root's interface and links
   * and must rebuild it. Only the *source* value inputs count -- a mirror (#INPUT_MIRROR_PROP) is a
   * relay derived from them, so hashing it as well would make the signature depend on how deeply a
   * value happens to be nested. A value edit changes a socket's default value, not this set, so it
   * still never rebuilds the root. */
  Vector<const MaterialPaintLayer *> value_layers;
  BKE_paint_layers_flatten(ma, value_layers);
  for (const MaterialPaintLayer *layer : value_layers) {
    bNodeTree *group_tree = nullptr;
    if (bNodeTree *const *tree_ptr = layer_trees.lookup_ptr(layer)) {
      group_tree = *tree_ptr;
    }
    if (group_tree == nullptr && ma.paint_layers_tree != nullptr) {
      group_tree = generated_layer_tree_find(*ma.paint_layers_tree, layer->marker);
    }
    if (group_tree == nullptr) {
      continue;
    }
    bool has_value = false;
    group_tree->tree_interface.foreach_item([&](const bNodeTreeInterfaceItem &item) {
      if (item.item_type != NodeTreeInterfaceItemType::Socket) {
        return true;
      }
      const auto &socket = reinterpret_cast<const bNodeTreeInterfaceSocket &>(item);
      if ((socket.flag & NODE_INTERFACE_SOCKET_INPUT) == 0) {
        return true;
      }
      const char *role = prop_string_get(socket.properties, INPUT_ROLE_PROP);
      if (role == nullptr || prop_int_get(socket.properties, INPUT_MIRROR_PROP, 0) != 0) {
        return true;
      }
      if (!has_value) {
        hash = topology_hash_mix(hash, 4);
        has_value = true;
      }
      topology_hash_string(hash, socket.name);
      topology_hash_string(hash, socket.socket_type);
      topology_hash_string(hash, role);
      hash = topology_hash_mix(
          hash, uint64_t(prop_int_get(socket.properties, INPUT_CHANNEL_PROP, -1)));
      bUUID marker = BLI_uuid_nil();
      if (uid_prop_get(socket.properties, INPUT_MARKER_PROP, marker)) {
        /* A value in a warm slot is identified by the slot: the real item that takes a spare's place
         * keeps the spare's sockets and must not move the root's hash. */
        topology_hash_uid(hash, value_slot_or_marker(socket.properties, marker));
      }
      return true;
    });
  }
  return hash;
}

namespace {

/**
 * Release the manual user references the build made from the nodes of the throwaway \a scratch tree.
 *
 * The scratch is created with #LIB_ID_CREATE_NO_MAIN | #LIB_ID_CREATE_NO_USER_REFCOUNT, so its #ID
 * carries #ID_TAG_NO_USER_REFCOUNT and `BKE_id_free` skips its usual relink pass
 * (`lib_id_delete.cc`: the `BKE_libblock_relink_ex` call is inside the `LIB_ID_FREE_NO_USER_REFCOUNT`
 * guard). That relink is the only thing that would have `id_us_min`'d the IDs a tree's nodes point
 * at, so without this the build's every `id_us_plus` -- layer groups, source wrappers, images, the
 * Normal Combine group -- would leak one user per regeneration and the data-blocks would never be
 * freed. Every manual reference the build takes in this file is a `bNode::id` (see the `id_us_plus`
 * calls in #paint_layers_tree_build and its helpers), so mirroring them per node is exact.
 */
void scratch_user_refs_release(bNodeTree &scratch)
{
  for (bNode &node : scratch.nodes) {
    if (node.id != nullptr) {
      id_us_min(node.id);
      node.id = nullptr;
    }
  }
}

}  // namespace

/**
 * A shared 1x1 image the warm items are bound to; a fake user keeps it out of orphan purges.
 *
 * It is made like the maps a stroke creates (#paint_layers_map_create: 24 bit, straight alpha, and
 * #IMA_GPU_LINEAR_PREMUL), because the colorspace, alpha mode and flags of a texture are part of the
 * code its Image Texture node generates. \a is_color picks the sRGB twin used for color channels;
 * masks and every other channel read Non-Color data.
 */
static Image *warm_image_ensure(Main &bmain, const char *name, const bool is_color)
{
  /* A linked library may carry its own copy; the warm items must bind to this file's, so the lookup
   * stays local (the unset optional would return the first match of either). */
  if (ID *found = BKE_libblock_find_name(&bmain, ID_IM, name, nullptr)) {
    return id_cast<Image *>(found);
  }
  const float white[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  Image *image = BKE_image_add_generated(
      &bmain, 1, 1, name, 24, false, IMA_GENTYPE_BLANK, white, false, !is_color, false);
  if (image != nullptr) {
    image->alpha_mode = IMA_ALPHA_STRAIGHT;
    image->flag |= IMA_GPU_LINEAR_PREMUL;
    id_fake_user_set(&image->id);
  }
  return image;
}

void BKE_paint_layers_generate_runtime_free(Material &ma)
{
  bke::paint_layers_runtime_free(ma);
#if PAINT_LAYERS_DEBUG_LOG
  /* The diagnostic maps only exist with the log on; in a quiet build there is nothing to drop. */
  previous_row_modes().remove(ma.id.session_uid);
  previous_source_group_embeds().remove(ma.id.session_uid);
#endif
}

bool BKE_paint_layers_regenerate(Main &bmain,
                                 Material &ma,
                                 PaintLayersRegenerateReport *r_report)
{
  PaintLayersRegenerateReport report;
#if PAINT_LAYERS_DEBUG_LOG
  const double regen_start = BLI_time_now_seconds();
#endif
  if (!paint_layers_is_layered(ma) || ma.nodetree == nullptr) {
    if (r_report != nullptr) {
      *r_report = report;
    }
    return false;
  }
  if (BLI_uuid_is_nil(ma.paint_layers_owner_uid)) {
    ma.paint_layers_owner_uid = BLI_uuid_generate_random();
  }

  bNodeTree *tree = ma.paint_layers_tree;
  const bool owned = tree != nullptr &&
                     BLI_uuid_equal(tree_owner_uid_get(*tree), ma.paint_layers_owner_uid);
  if (tree != nullptr && !owned) {
    /* Somebody replaced the pointer with a tree that is not ours. Leave their data alone and make a
     * fresh tree; the foreign one keeps whatever users it has. */
    ma.paint_layers_tree = nullptr;
    tree = nullptr;
    report.replaced_foreign_tree = true;
  }
  /* Collect the layer groups before any clear removes their instances; afterwards they are lost. */
  Vector<bNodeTree *> old_layer_trees;
  if (tree != nullptr) {
    layer_trees_collect(*tree, ma.paint_layers_owner_uid, old_layer_trees);
  }
  bool created_tree = false;
  if (tree == nullptr) {
    char name[MAX_ID_NAME - 2];
    /* A leading dot keeps the generated tree out of every ID picker and #Node Groups list (#ID
     * name convention: templates/interface_template_id.cc hides `id->name[2] == '.'`). */
    SNPRINTF(name, ".PBR Layers (%s)", ma.id.name + 2);
    tree = bke::node_tree_add_tree(&bmain, name, "ShaderNodeTree");
    if (tree == nullptr) {
      return false;
    }
    /* The tree is created with one user, which is this material from here on. */
    ma.paint_layers_tree = tree;
    tree_owner_uid_set(*tree, ma.paint_layers_owner_uid);
    created_tree = true;
  }
  {
    /* Files saved before the dot scheme carry the bare root name; rename on the next regeneration,
     * with no versioning pass. The pointer in #paint_layers_tree already found the tree, so this is
     * a rename of a known ID, never a name lookup. */
    char root_name[MAX_ID_NAME - 2];
    SNPRINTF(root_name, ".PBR Layers (%s)", ma.id.name + 2);
    if (!STREQ(tree->id.name + 2, root_name)) {
      BKE_id_rename(bmain, tree->id, root_name);
    }
  }
  /* An existing tree is not cleared yet: whether the root can be kept is decided after the layer
   * groups are (re)built, because that decision reads their final interfaces. */

  /* The Normal chain needs the shared combine group; create it only when a Normal row exists. */
  PaintLayersBuildContext ctx;
  /* What this call learns once (a source's resolve, a row's mode, the Pass Through scales) and hands
   * to every reader below. It lives and dies with this call; nothing keeps it between two. */
  PaintLayersRegenCache regen_cache;
  ctx.regen_cache = &regen_cache;
  /* The shared image the warm items point at is created here, on the main thread: the pure build
   * never touches #Main. The reconcile runs before anything reads a hash, so the graph and its hashes
   * agree about which slots stand ready. It also runs before the forced set below is dropped, on
   * purpose: the plan reads each Material row's mode, and a row the sampler budget pinned onto its
   * bake must keep its slot while the pin is visible (#paint_layers_warm_plan), or the slot would
   * flip off here and back on once the pin lifts, rebuilding the root each time. */
  const bool warm_needed = paint_layers_warm_needed(ma, &regen_cache);
  paint_layers_warm_reconcile(
      ma,
      warm_needed ? warm_image_ensure(bmain, ".PL Warm", false) : nullptr,
      &regen_cache,
      warm_needed ? warm_image_ensure(bmain, ".PL Warm Color", true) : nullptr);
  {
    Vector<const MaterialPaintLayer *> layers;
    BKE_paint_layers_flatten(ma, layers);
    for (const MaterialPaintLayer *layer : layers) {
      if (paint_layer_channel_present(ma, *layer, PAINT_MATERIAL_CHANNEL_NORMAL)) {
        ctx.normal_combine_group = BKE_paint_material_normal_combine_group_ensure(bmain);
        break;
      }
    }
  }
  /* Capture the previous pass's forced set, then drop it before anything reads a mode so the
   * fallback re-derives it from scratch and lifts as soon as the budget allows. A row that was pinned
   * and whose bake has since gone invalid is carried forward below: it stays on its stale maps until
   * a fresh bake lands, instead of reviving live and forcing a rebuild loop. */
  const Vector<bUUID> previously_forced = forced_bake_markers(ma);
  forced_bake_clear(ma);
  budget_cleanup_owner_set(ma, false);

  /* Wrapper groups for Material rows that show their whole source graph. The factory needs #Main
   * and runs on the main thread, so it is prepared here, like the Normal combine group, and handed
   * to the pure build. Each row's mode and, for SourceGroup, whether the wrapper was built are
   * recorded for the report; a refused source keeps its row on the baked maps. */
  Map<const Material *, bNodeTree *> source_groups;
  Map<const Material *, PaintLayersSourceGroupRefusal> source_group_refusals;
  bool source_groups_changed = false;
  /* Re-runnable: the sampler fallback changes some rows' modes, so the report is rebuilt once the
   * final forced set is known. Wrappers are cached, so a second call does not rebuild them. */
  auto populate_material_rows = [&]() {
    report.material_rows.clear();
    Vector<const MaterialPaintLayer *> layers;
    /* An Effect correction with source Material needs its own wrapper ensured here too (this is
     * what fills #ctx.source_group_get, read by #resolve_row_material_source), so this walks every
     * role, not just Layer rows. The report itself stays Layer-only below: it is a per-row status
     * display, and mixing a correction into it would change what its existing readers see. */
    BKE_paint_layers_flatten_all(ma, layers);
    for (const MaterialPaintLayer *layer : layers) {
      if (layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL) {
        continue;
      }
      const bool is_layer_row = BKE_paint_layers_role(*layer) == PaintLayerRole::Layer;
      const PaintLayerMaterialMode mode = BKE_paint_layers_material_mode(ma, *layer, &regen_cache);
      PaintLayersSourceGroupRefusal refusal = PaintLayersSourceGroupRefusal::None;
      bool wrapper_built = false;
      int group_depth = 0;
      if (mode == PaintLayerMaterialMode::SourceGroup && layer->material != nullptr) {
        ChannelUnavailableReason path_reason = ChannelUnavailableReason::None;
        Vector<const bNode *> path;
        if (BKE_paint_material_principled_find(*layer->material, path_reason, &path) != nullptr) {
          group_depth = int(path.size());
        }
        bNodeTree *wrapper = nullptr;
        if (bNodeTree *const *found = source_groups.lookup_ptr(layer->material)) {
          wrapper = *found;
          refusal = source_group_refusals.lookup_default(layer->material,
                                                         PaintLayersSourceGroupRefusal::None);
        }
        else {
          bool wrapper_changed = false;
          wrapper = BKE_paint_layers_source_group_ensure(
              bmain, ma, *layer->material, refusal, &wrapper_changed);
          source_groups_changed |= wrapper_changed;
          source_groups.add(layer->material, wrapper);
          source_group_refusals.add(layer->material, refusal);
          if (wrapper == nullptr &&
              report.source_group_refusal == PaintLayersSourceGroupRefusal::None)
          {
            report.source_group_refusal = refusal;
          }
        }
        wrapper_built = wrapper != nullptr;
      }
      if (is_layer_row) {
        PaintLayersRegenerateReport::MaterialRowModeReport row;
        row.marker = layer->marker;
        STRNCPY(row.name, layer->name);
        row.mode = mode;
        row.refusal = refusal;
        row.wrapper_built = wrapper_built;
        row.group_depth = group_depth;
        row.source_name[0] = '\0';
        row.source_uid = 0;
        if (layer->material != nullptr) {
          STRNCPY(row.source_name, layer->material->id.name + 2);
          row.source_uid = layer->material->id.session_uid;
        }
        row.deferred = BKE_paint_layers_bake_row_is_deferred(ma, *layer);
        if (BKE_paint_layers_material_forced_bake(ma, *layer)) {
          row.refusal = PaintLayersSourceGroupRefusal::TooManyTextures;
        }
        report.material_rows.append(row);
      }
      /* The row-level write the Main-free status reads. A source wrapper that failed to build is
       * otherwise known only inside this report, which the tagged entry point discards. Read for
       * any role, so an Effect correction's own SourceGroup failure is tracked too. */
      BKE_paint_layers_source_group_build_failed_set(
          ma, *layer, refusal == PaintLayersSourceGroupRefusal::BuildFailed);
    }
  };
  populate_material_rows();
  /* Named: #FunctionRef does not own the callable, so a temporary lambda would dangle. */
  const auto source_group_lookup = [&source_groups](const Material &source) -> bNodeTree * {
    return source_groups.lookup_default(&source, nullptr);
  };
  ctx.source_group_get = source_group_lookup;

  /* The sampler budget is runtime state; zero disables the check. The decision is made from an
   * estimate over the description -- never by building a preview -- so it does not mutate the layer
   * groups. It must run before `paint_layers_wired_channels`: the fallback moves some rows' modes,
   * and the wired set, every layer group's topology hash and the root hash are computed from the
   * final modes, or a second pass would see a different graph and loop. */
  const int budget = sampler_runtime().budget;
  int sampler_estimate_value = 0;
  int fallback_rows = 0;
  int removed_hidden = 0;

  /* A non-mutating estimate of the samplers the built graph would use: user nodes outside the stack
   * plus the rows' own sources, maps, corrections and baked maps. Shared images dedup by
   * `(Image, sampler state)` exactly as the counter and EEVEE do, and the parity rules -- which rows
   * a mode substitutes, which children an isolating folder expands -- come from the same predicates
   * the build uses, so the estimate tracks the graph rather than a copy of the rules. */
  auto sampler_estimate = [&]() -> int {
    SamplerCounter counter;
    if (ma.nodetree != nullptr) {
      counter.visit_tree_skipping(*ma.nodetree, ma.paint_layers_tree);
    }

    /* A correction's maps are built at most once per row, so adding each one once is enough. A
     * Constant (Fill) correction builds a group input, not a map: a stale image on it is ignored by
     * the build and must not be counted. Only channels the description wires are built at all, so a
     * channel outside the set (or one a disabled record dropped) contributes no sampler. */
    const Vector<int> wired_probe = paint_layers_wired_channels(ma, &regen_cache);
    Set<int> wired;
    for (const int channel : wired_probe) {
      wired.add(channel);
    }

    /* The state a Hybrid live map is shown with: the generator copies the source node's storage, so
     * the key has to come from that node, not from the default. Declared first so the correction
     * helpers below can use it. */
    std::function<uint32_t(const MaterialPaintLayer &, int)> live_image_state =
        [&](const MaterialPaintLayer &layer, const int channel) {
          const MaterialSourceResolve &resolve = regen_cache.resolve(layer.material);
          if (const bNode *source_node = resolve.images[channel].node) {
            if (const NodeTexImage *storage = static_cast<const NodeTexImage *>(
                    source_node->storage))
            {
              return image_sampler_state_key(*storage);
            }
          }
          return default_image_sampler_state();
        };

    /* Forward declaration: #add_stack_children recurses back into #add_corrections for a child's
     * own effects/masks, and #add_corrections calls #add_stack_children for a Stack item's subtree. */
    std::function<void(const MaterialPaintLayer &)> add_corrections;

    /* A Stack correction's (or mask item's) content is its children, composited in isolation
     * exactly like a Layer folder's; their maps are samplers of their own. Mirrors #build_list over
     * the subtree, where a Pass Through folder is inlined and a removed row drops out.
     *
     * \a fixed_channel is the channel the subtree builds in: a Stack Effect correction builds in
     * the owner's current channel, so it is walked once per wired channel (-1 here); a Stack mask
     * item builds in a fixed channel (its `mask_channel`, or Base Color on the Alpha convention),
     * regardless of the wired set, so the subtree reads exactly that channel. */
    std::function<void(const ListBaseT<MaterialPaintLayer> &, int)> add_stack_children;
    add_stack_children = [&](const ListBaseT<MaterialPaintLayer> &list, const int fixed_channel) {
      auto walk_channel = [&](const MaterialPaintLayer &child, const int channel) {
        if (fixed_channel < 0 && !wired.contains(channel)) {
          return;
        }
        Image *baked = nullptr;
        if (row_channel_substituted(ma, child, channel, &baked) && baked != nullptr) {
          counter.add_image(*baked);
        }
        else if (Image *image = paint_layer_channel_image(ma, child, channel)) {
          counter.add_image(*image);
        }
      };
      for (const MaterialPaintLayer &child : list) {
        if (row_is_removed(ma, child)) {
          continue;
        }
        if (BKE_paint_layers_folder_is_pass_through(ma, child)) {
          add_stack_children(child.children, fixed_channel);
          continue;
        }
        if (fixed_channel >= 0) {
          walk_channel(child, fixed_channel);
        }
        else {
          for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
            walk_channel(child, channel);
          }
        }
        add_corrections(child);
        /* A folder inside the subtree composites its own children in isolation, exactly like a
         * top-level folder (#build_list recurses into it). */
        if (BKE_paint_layers_is_folder(child) &&
            !BKE_paint_layers_folder_is_pass_through(ma, child))
        {
          add_stack_children(child.children, fixed_channel);
        }
      }
    };

    /* The coverage a Material or Node Group correction/mask reads for its own factor, mirroring
     * #resolve_correction_coverage: a live constant reads nothing, a live map reads that image, a
     * SourceGroup reads the wrapper, and Baked reads the row's own coverage bake. */
    auto add_material_or_group_coverage = [&](const MaterialPaintLayer &row) {
      if (row.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
        if (row.bake != nullptr && row.bake->coverage != nullptr) {
          counter.add_image(*row.bake->coverage);
        }
        return;
      }
      float live_value[4];
      Image *live_image = nullptr;
      const ImageUser *live_iuser = nullptr;
      if (BKE_paint_layers_material_live_constant(
              ma, row, PAINT_MATERIAL_CHANNEL_ALPHA, live_value, &regen_cache))
      {
        return;
      }
      if (BKE_paint_layers_material_live_image(
              ma, row, PAINT_MATERIAL_CHANNEL_ALPHA, &live_image, &live_iuser, &regen_cache))
      {
        counter.add_image(*live_image, live_image_state(row, PAINT_MATERIAL_CHANNEL_ALPHA));
        return;
      }
      if (row.material != nullptr && row.material != nullptr &&
          BKE_paint_layers_material_mode(ma, row, &regen_cache) ==
              PaintLayerMaterialMode::SourceGroup)
      {
        if (bNodeTree *wrapper = source_group_lookup(*row.material)) {
          counter.add_tree(*wrapper);
        }
        return;
      }
      if (row.bake != nullptr && row.bake->coverage != nullptr) {
        counter.add_image(*row.bake->coverage);
      }
    };

    /* The content map a Material/Node Group correction or mask reads in \a channel, mirroring the
     * build's Baked/Hybrid/SourceGroup resolution. */
    auto add_material_or_group_content = [&](const MaterialPaintLayer &row, const int channel) {
      if (row.source == MA_PAINT_LAYER_SOURCE_NODE_GROUP) {
        Image *baked = nullptr;
        if (BKE_paint_layers_bake_substitute(ma, row, channel, &baked) ||
            BKE_paint_layers_bake_substitute_custom(ma, row, channel, &baked, nullptr))
        {
          if (baked != nullptr) {
            counter.add_image(*baked);
          }
        }
        return;
      }
      float live_value[4];
      Image *live_image = nullptr;
      const ImageUser *live_iuser = nullptr;
      if (BKE_paint_layers_material_live_constant(ma, row, channel, live_value, &regen_cache)) {
        return;
      }
      if (BKE_paint_layers_material_live_image(
              ma, row, channel, &live_image, &live_iuser, &regen_cache))
      {
        counter.add_image(*live_image, live_image_state(row, channel));
        return;
      }
      if (row.material != nullptr &&
          BKE_paint_layers_material_mode(ma, row, &regen_cache) ==
              PaintLayerMaterialMode::SourceGroup)
      {
        if (bNodeTree *wrapper = source_group_lookup(*row.material)) {
          counter.add_tree(*wrapper);
        }
        return;
      }
      if (Image *image = paint_layer_channel_image(ma, row, channel)) {
        counter.add_image(*image);
      }
    };

    add_corrections = [&](const MaterialPaintLayer &layer) {
          for (const MaterialPaintLayer *effect : paint_layers_build_effects(ma, layer)) {
            if (BKE_paint_layers_source_type(*effect) == PaintLayerSourceType::Constant) {
              continue;
            }
            if (ELEM(effect->source,
                     MA_PAINT_LAYER_SOURCE_MATERIAL,
                     MA_PAINT_LAYER_SOURCE_NODE_GROUP))
            {
              /* Content per wired channel; the Normal channel is skipped for a content correction
               * only when it is a Fill, which this branch already excluded. */
              for (const int channel : wired) {
                add_material_or_group_content(*effect, channel);
              }
              /* A Material content correction reads its own factor coverage
               * (#resolve_correction_coverage); a Node Group content correction has no separate
               * coverage read (the build reads only its bake map). */
              if (!wired.is_empty() && effect->source == MA_PAINT_LAYER_SOURCE_MATERIAL) {
                add_material_or_group_coverage(*effect);
              }
            }
            else {
              for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
                if (!wired.contains(channel)) {
                  continue;
                }
                if (Image *image = paint_layer_channel_image(ma, *effect, channel)) {
                  counter.add_image(*image);
                }
              }
            }
            /* A Stack correction's content is its children's; a content correction builds in the
             * owner's channel, so the subtree is walked once per wired channel. */
            if (effect->source == MA_PAINT_LAYER_SOURCE_STACK) {
              add_stack_children(effect->children, -1);
            }
          }
          for (const MaterialPaintLayer *mask_item : paint_layers_build_mask_items(ma, layer)) {
            if (BKE_paint_layers_source_type(*mask_item) == PaintLayerSourceType::Constant) {
              continue;
            }
            if (mask_item->source == MA_PAINT_LAYER_SOURCE_STACK) {
              /* A Stack mask item builds its subtree in a fixed channel: the Alpha convention
               * reads Base Color, every other `mask_channel` reads itself. */
              const int mask_build_channel = (mask_item->mask_channel ==
                                              PAINT_MATERIAL_CHANNEL_ALPHA) ?
                                                 int(PAINT_MATERIAL_CHANNEL_BASE_COLOR) :
                                                 int(mask_item->mask_channel);
              add_stack_children(mask_item->children, mask_build_channel);
              continue;
            }
            if (ELEM(mask_item->source,
                     MA_PAINT_LAYER_SOURCE_MATERIAL,
                     MA_PAINT_LAYER_SOURCE_NODE_GROUP))
            {
              /* A normal mask channel builds nothing (the item is skipped). */
              if (mask_item->mask_channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
                continue;
              }
              /* Alpha reads only its own coverage as the grey; every other channel reads the
               * `mask_channel` content map plus the item's own coverage for its factor. */
              add_material_or_group_coverage(*mask_item);
              if (mask_item->mask_channel != PAINT_MATERIAL_CHANNEL_ALPHA) {
                add_material_or_group_content(*mask_item, mask_item->mask_channel);
              }
              continue;
            }
            /* A plain Paint/MESH_MAP mask reads one Base Color map over the row. */
            if (Image *image = paint_layer_mask_correction_image(ma, *mask_item, 0)) {
              counter.add_image(*image);
            }
          }
        };

    /* Whether the wrapper exposes a COVERAGE output: when it does, the build reads the row's factor
     * from the wrapper and never builds the baked coverage map. */
    auto wrapper_has_coverage = [&](const Material *source) -> bool {
      bNodeTree *wrapper = (source != nullptr) ? source_group_lookup(*source) : nullptr;
      if (wrapper == nullptr) {
        return false;
      }
      wrapper->ensure_interface_cache();
      for (bNodeTreeInterfaceSocket *iface : wrapper->interface_outputs()) {
        const char *role = custom_role_get(iface->properties);
        if (role != nullptr && STREQ(role, "COVERAGE")) {
          return true;
        }
      }
      return false;
    };

    /* Walk the stack so a dropped Pass Through folder takes its whole inlined subtree with it; a
     * flat list cannot tell that an enabled child sits under a removed folder. Returns whether
     * anything under \a list contributes a sampler: the build drops a row with no channel, no live
     * value and no wrapper, and then builds neither its coverage nor its corrections, so the
     * estimate must not count them either. */
    std::function<bool(const ListBaseT<MaterialPaintLayer> &)> walk = [&](const ListBaseT<MaterialPaintLayer> &list) -> bool {
      bool any = false;
      for (const MaterialPaintLayer &layer :
           list)
      {
        if (row_is_removed(ma, layer)) {
          continue;
        }
        if (BKE_paint_layers_folder_is_pass_through(ma, layer)) {
          /* Inlined: the folder's own corrections are not built, only its children's. */
          any |= walk(layer.children);
          continue;
        }
        const bool is_folder = BKE_paint_layers_is_folder(layer);
        if (row_is_substituted(ma, layer)) {
          /* The generator replaces the whole row with its bake (`row_is_substituted`), so the build
           * expands no children and builds no corrections. This is the same predicate the build's
           * value-input path uses. */
          counter.add_baked_maps(layer);
          any = true;
          continue;
        }
        const PaintLayerMaterialMode mode = (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL) ?
                                                BKE_paint_layers_material_mode(
                                                    ma, layer, &regen_cache) :
                                                PaintLayerMaterialMode::Baked;
        bool participates = false;
        bool any_substituted = false;
        for (int channel = 0; channel < PAINT_MATERIAL_CHANNEL_NUM; channel++) {
          Image *baked = nullptr;
          if (row_channel_substituted(ma, layer, channel, &baked) && baked != nullptr) {
            /* A per-channel cache (a Custom row's stale bake) replaces just this channel. */
            counter.add_image(*baked);
            any_substituted = true;
            participates = true;
            continue;
          }
          if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer.material != nullptr) {
            if (mode == PaintLayerMaterialMode::SourceGroup) {
              /* One wrapper instance serves every channel; the visitor dedups its tree. */
              if (bNodeTree *wrapper = source_group_lookup(*layer.material)) {
                counter.add_tree(*wrapper);
                participates = true;
              }
              /* No wrapper (refused): the build falls back to the row's own maps. */
              else if (Image *image = paint_layer_channel_image(ma, layer, channel)) {
                counter.add_image(*image);
                participates = true;
              }
              continue;
            }
            /* Hybrid shows a live constant (no sampler), a live map, or the row's baked map. */
            float live_value[4];
            Image *live_image = nullptr;
            const ImageUser *live_iuser = nullptr;
            if (BKE_paint_layers_material_live_constant(
                    ma, layer, channel, live_value, &regen_cache))
            {
              participates = true;
              continue;
            }
            if (BKE_paint_layers_material_live_image(
                    ma, layer, channel, &live_image, &live_iuser, &regen_cache))
            {
              counter.add_image(*live_image, live_image_state(layer, channel));
              participates = true;
              continue;
            }
          }
          if (Image *image = paint_layer_channel_image(ma, layer, channel)) {
            counter.add_image(*image);
            participates = true;
          }
          else if (leaf_participates(ma, layer, channel)) {
            /* A row a constant Fill qualifies (`leaf_participates`) builds its group and, with it,
             * its warm Mask/Effect chains: those shared maps are samplers the description alone
             * cannot see (a lone Fill row contributes none of its own), so mark the row as built so
             * #add_corrections counts them. */
            participates = true;
          }
        }
        if (is_folder) {
          participates |= walk(layer.children);
        }
        if (participates) {
          if (any_substituted && layer.bake != nullptr && layer.bake->coverage != nullptr) {
            counter.add_image(*layer.bake->coverage);
          }
          if (layer.source == MA_PAINT_LAYER_SOURCE_MATERIAL && layer.bake != nullptr &&
              layer.bake->coverage != nullptr &&
              !(mode == PaintLayerMaterialMode::SourceGroup &&
                wrapper_has_coverage(layer.material)))
          {
            /* The row's factor falls back to the baked coverage when the source's alpha is not
             * shown live and the wrapper has no COVERAGE output: a live constant needs no sampler, a
             * live image is counted in its own channel, and a wrapper coverage is in the wrapper. */
            float live_alpha_value[4];
            Image *live_alpha_image = nullptr;
            const ImageUser *live_alpha_iuser = nullptr;
            const bool live_alpha =
                BKE_paint_layers_material_live_constant(
                    ma, layer, PAINT_MATERIAL_CHANNEL_ALPHA, live_alpha_value, &regen_cache) ||
                BKE_paint_layers_material_live_image(ma,
                                                     layer,
                                                     PAINT_MATERIAL_CHANNEL_ALPHA,
                                                     &live_alpha_image,
                                                     &live_alpha_iuser,
                                                     &regen_cache);
            if (!live_alpha) {
              counter.add_image(*layer.bake->coverage);
            }
          }
          add_corrections(layer);
        }
        any |= participates;
      }
      return any;
    };
    walk(ma.paint_layers);
    return counter.total();
  };

  sampler_estimate_value = sampler_estimate();
  if (budget > 0 && sampler_estimate_value > budget) {
    /* 1. Drop disabled rows. A hidden Pass Through folder only leaves under pressure: without it
     *    its children stay in the graph with factor zero, which is the value-edit behavior. */
    bool any_disabled = false;
    {
      Vector<const MaterialPaintLayer *> all_layers;
      BKE_paint_layers_flatten(ma, all_layers);
      for (const MaterialPaintLayer *layer : all_layers) {
        if ((layer->flag & MA_PAINT_LAYER_ENABLED) == 0) {
          any_disabled = true;
          break;
        }
      }
    }
    if (any_disabled) {
      budget_cleanup_owner_set(ma, true);
      removed_rows_reconcile(ma);
      sampler_estimate_value = sampler_estimate();
    }
    /* 2. Keep a row pinned last pass on its stale maps while its bake is rebuilt. It must not revive
     *    live: that would rebuild the root on every source edit and re-enter the bake on the next
     *    pass -- the bake -> hash -> regeneration -> bake loop this state exists to break. */
    for (const bUUID &marker : previously_forced) {
      MaterialPaintLayer *layer = BKE_paint_layers_find(ma, marker);
      if (layer == nullptr || layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL ||
          layer->bake == nullptr || BKE_paint_layers_bake_is_valid(ma, *layer) ||
          forced_bake_contains(ma, marker))
      {
        continue;
      }
      forced_bake_add(ma, marker);
      sampler_estimate_value = sampler_estimate();
    }
    /* 3. Pin live SourceGroup rows onto their maps, largest sampler saving first. Only a pin that
     *    lowers the count is taken: pinning a row whose baked maps cost more than its live graph
     *    would raise the count instead. Ties break on the marker so the choice is deterministic. */
    if (sampler_estimate_value > budget) {
      struct FallbackCandidate {
        int gain = 0;
        bUUID marker = {};
      };
      Vector<FallbackCandidate> candidates;
      Vector<const MaterialPaintLayer *> all_layers;
      BKE_paint_layers_flatten(ma, all_layers);
      for (const MaterialPaintLayer *layer : all_layers) {
        if (layer->source != MA_PAINT_LAYER_SOURCE_MATERIAL || layer->material == nullptr ||
            layer->bake == nullptr || !BKE_paint_layers_material_bake_ready(ma, *layer) ||
            BKE_paint_layers_material_mode(ma, *layer, &regen_cache) !=
                PaintLayerMaterialMode::SourceGroup ||
            source_group_lookup(*layer->material) == nullptr ||
            forced_bake_contains(ma, layer->marker))
        {
          continue;
        }
        /* Probe the row: measure the count with it pinned, then undo the pin. */
        const int before = sampler_estimate_value;
        forced_bake_add(ma, layer->marker);
        const int after = sampler_estimate();
        forced_bake_remove(ma, layer->marker);
        if (before > after) {
          candidates.append({before - after, layer->marker});
        }
      }
      std::sort(candidates.begin(),
                candidates.end(),
                [](const FallbackCandidate &a, const FallbackCandidate &b) {
                  if (a.gain != b.gain) {
                    return a.gain > b.gain;
                  }
                  char a_text[UUID_STRING_SIZE];
                  char b_text[UUID_STRING_SIZE];
                  BLI_uuid_format(a_text, a.marker);
                  BLI_uuid_format(b_text, b.marker);
                  return strcmp(a_text, b_text) < 0;
                });
      for (const FallbackCandidate &candidate : candidates) {
        if (sampler_estimate_value <= budget) {
          break;
        }
        forced_bake_add(ma, candidate.marker);
        fallback_rows++;
        /* The baked maps add samplers back, so re-estimate after every pin. */
        sampler_estimate_value = sampler_estimate();
      }
    }
    report.sampler_budget_exceeded = sampler_estimate_value > budget;
    Vector<const MaterialPaintLayer *> all_layers;
    BKE_paint_layers_flatten(ma, all_layers);
    for (const MaterialPaintLayer *layer : all_layers) {
      if ((layer->flag & MA_PAINT_LAYER_ENABLED) == 0 && row_is_removed(ma, *layer)) {
        removed_hidden++;
      }
    }
  }
  /* From here the forced set is final, so a row's mode can be remembered: every reader below asks it
   * many times per row (the wired set, each layer's hash, the build, the report). Before this point
   * the fallback was still moving modes, and a remembered one would have outlived the move. */
  regen_cache.modes_frozen = true;
  /* The modes may have moved (forced bake, hidden cleanup): refresh the report before building. */
  populate_material_rows();
#if PAINT_LAYERS_DEBUG_LOG
  material_row_modes_log(ma, report.material_rows);
#endif

  /* The factory hands each layer a tree of its own, reusing an old one by layer marker. It outlives
   * the build, which only holds a non-owning reference to it. A reused tree whose stored topology
   * hash matches the description is handed back untouched and reported as unchanged, so the build
   * only restores the parent-side links and never re-creates its nodes. Wired channels and the layer
   * tree hashes are read here, after the fallback fixed every row's mode. */
  const Vector<int> wired_channels = paint_layers_wired_channels(ma, &regen_cache);
  Set<const MaterialPaintLayer *> unchanged_layers;
  Vector<bNodeTree *> used_layer_trees;
  Map<const MaterialPaintLayer *, bNodeTree *> layer_trees;
  bool groups_created = false;
  bool groups_deleted = false;
  int layer_groups_rebuilt = 0;
  auto layer_tree_get = [&](const MaterialPaintLayer &layer) -> bNodeTree * {
    char name[MAX_ID_NAME - 2];
    if (BKE_paint_layers_is_folder(layer)) {
      SNPRINTF(name, ".PL Folder %s", layer.name[0] != '\0' ? layer.name : "Folder");
    }
    else {
      SNPRINTF(name, ".PL Layer %s", layer.name[0] != '\0' ? layer.name : "Layer");
    }
    const uint64_t topology = paint_layers_layer_topology_hash(
        ma, layer, wired_channels, &regen_cache);
    for (bNodeTree *candidate : old_layer_trees) {
      if (used_layer_trees.contains(candidate)) {
        continue;
      }
      bUUID marker = BLI_uuid_nil();
      if (layer_tree_marker_get(*candidate, marker) && BLI_uuid_equal(marker, layer.marker)) {
        uint64_t stored = 0;
        if (tree_topology_hash_get(*candidate, stored) && stored == topology) {
          used_layer_trees.append(candidate);
          unchanged_layers.add(&layer);
          layer_trees.add(&layer, candidate);
          return candidate;
        }
        /* Clear the nodes but keep the interface: a rebuilt group reuses its sockets by name, so
         * their identifiers -- and the parent's links into them -- survive. Unused sockets are
         * pruned at the end of the build. */
        PL_DEBUG_PRINTF(
            "paint layers regen diff: layer '%s' source=%d role=%d old=%llx new=%llx warm(base=%d "
            "mask=%d effect=%d) mode=%d deferred=%d wired=%d\n",
            layer.name,
            int(layer.source),
            int(layer.role),
            static_cast<unsigned long long>(stored),
            static_cast<unsigned long long>(topology),
            int(paint_layers_warm_item(ma, layer, WarmKind::MaskBase) != nullptr),
            int(paint_layers_warm_item(ma, layer, WarmKind::Mask) != nullptr),
            int(paint_layers_warm_item(ma, layer, WarmKind::Effect) != nullptr),
            int(BKE_paint_layers_material_mode(ma, layer, &regen_cache)),
            int(BKE_paint_layers_bake_row_is_deferred(ma, layer)),
            int(wired_channels.size()));
        tree_clear_nodes(bmain, *candidate);
        layer_groups_rebuilt++;
        if (!STREQ(candidate->id.name + 2, name)) {
          BKE_id_rename(bmain, candidate->id, name);
        }
        tree_topology_hash_set(*candidate, topology);
        used_layer_trees.append(candidate);
        layer_trees.add(&layer, candidate);
        return candidate;
      }
    }
    bNodeTree *fresh = bke::node_tree_add_tree(&bmain, name, "ShaderNodeTree");
    if (fresh == nullptr) {
      return nullptr;
    }
    /* The only user must be the instance node, which adds its own reference when assigned. */
    id_us_min(&fresh->id);
    tree_owner_uid_set(*fresh, ma.paint_layers_owner_uid);
    uid_prop_set(fresh->id.properties, TREE_LAYER_PROP, layer.marker);
    tree_topology_hash_set(*fresh, topology);
    used_layer_trees.append(fresh);
    layer_trees.add(&layer, fresh);
    groups_created = true;
    layer_groups_rebuilt++;
    return fresh;
  };
  auto layer_tree_unchanged = [&](const MaterialPaintLayer &layer) {
    return unchanged_layers.contains(&layer);
  };
  ctx.layer_tree_get = layer_tree_get;
  ctx.layer_tree_unchanged = layer_tree_unchanged;

  /* Build once with the final modes and cleanup. This rebuilds the groups whose hash moved (in
   * place) and gives their final interfaces without touching the real root. The scratch is a
   * throwaway build target, never a data-block the file should see: create it outside #Main
   * (`bmain == nullptr` selects #LIB_ID_CREATE_NO_MAIN | #LIB_ID_CREATE_NO_USER_REFCOUNT) so it is
   * never linked into a library or saved, and free it with a null #Main to match. */
  bNodeTree *scratch = bke::node_tree_add_tree(nullptr, "PBR Layers Scratch", "ShaderNodeTree");
  bNodeTree &build_target = (scratch != nullptr) ? *scratch : *tree;
  paint_layers_tree_build(ma, build_target, ctx);
  /* The build may have grown a group's interface (a new effect or mask). Every instance of that
   * group, in the real root included, must have matching sockets before any tree update: an update
   * of a layer tree also visits the trees that use it, and the node tree update's interface pass
   * assumes an instance's inputs line up with its group's interface. */
  {
    Set<bNodeTree *> refreshed;
    refresh_generated_instances(*tree, ma.paint_layers_owner_uid, refreshed);
  }
  /* A pass that rebuilt, created or dropped nothing leaves every group exactly as it was. Tagging
   * the trees updated anyway makes the shading system treat the material as edited and compile it
   * again for a change that is not there (a click that only re-tagged a regeneration was enough). */
  const bool groups_touched = groups_created || groups_deleted || layer_groups_rebuilt > 0 ||
                              source_groups_changed;
  for (bNodeTree *layer_tree : used_layer_trees) {
    Set<bNodeTree *> refreshed;
    refresh_generated_instances(*layer_tree, ma.paint_layers_owner_uid, refreshed);
    if (groups_touched) {
      BKE_ntree_update_tag_all(layer_tree);
      BKE_ntree_update_after_single_tree_change(bmain, *layer_tree);
      DEG_id_tag_update(&layer_tree->id, ID_RECALC_SYNC_TO_EVAL);
    }
  }
  const uint64_t root_hash = paint_layers_root_topology_hash(
      ma, wired_channels, layer_trees, &regen_cache);
  uint64_t stored_root = 0;
  const bool have_stored_root = !created_tree && tree_root_hash_get(*tree, stored_root);
  /* Undo safety net: a row recorded as removed but enabled again (memfile undo preserves the row
   * and its session state but not our runtime set) must not stay missing. Forget the marker and
   * force the rebuild that puts it back. An extra rebuild is fine; a missing row never is. */
  bool undo_forces_rebuild = false;
  {
    Vector<const MaterialPaintLayer *> all_layers;
    BKE_paint_layers_flatten(ma, all_layers);
    for (const MaterialPaintLayer *layer : all_layers) {
      if ((layer->flag & MA_PAINT_LAYER_ENABLED) != 0 &&
          removed_rows_contains(ma, layer->marker))
      {
        BKE_paint_layers_row_removed_clear(ma, layer->marker);
        undo_forces_rebuild = true;
      }
    }
  }
  const bool keep_root = !undo_forces_rebuild && !created_tree && have_stored_root &&
                         stored_root == root_hash;
  report.root_rebuilt = !keep_root;
  report.layer_groups_rebuilt = layer_groups_rebuilt;
#if PAINT_LAYERS_DEBUG_LOG
  if (!keep_root && have_stored_root) {
    printf("paint layers regen diff: root old=%llx new=%llx undo=%d\n",
           static_cast<unsigned long long>(stored_root),
           static_cast<unsigned long long>(root_hash),
           int(undo_forces_rebuild));
  }
#endif
  if (keep_root) {
    /* The root's nodes, links and interface already match: discard the scratch and leave it. */
    if (scratch != nullptr) {
      scratch_user_refs_release(*scratch);
      BKE_id_free(nullptr, scratch);
    }
    BLI_assert_msg(root_hash == stored_root, "a kept root must match its own stored hash");
    if (groups_touched) {
      BKE_ntree_update_tag_all(tree);
      BKE_ntree_update_after_single_tree_change(bmain, *tree);
    }
  }
  else {
    /* Full root rebuild: the groups are current now, so the build only lays out the root skeleton
     * and reuses every group untouched. The rebuild is the only chance to drop rows the user has
     * disabled: record them, so the build leaves them out, and store the hash of what was built. */
    if (scratch != nullptr) {
      scratch_user_refs_release(*scratch);
      BKE_id_free(nullptr, scratch);
    }
    for (bNodeTree *layer_tree : used_layer_trees) {
      if (!old_layer_trees.contains(layer_tree)) {
        old_layer_trees.append(layer_tree);
      }
    }
    used_layer_trees.clear();
    unchanged_layers.clear();
    layer_trees.clear();
    removed_rows_reconcile(ma);
    tree_clear(bmain, *tree);
    paint_layers_tree_build(ma, *tree, ctx);
    for (bNodeTree *layer_tree : used_layer_trees) {
      BKE_ntree_update_tag_all(layer_tree);
      BKE_ntree_update_after_single_tree_change(bmain, *layer_tree);
      DEG_id_tag_update(&layer_tree->id, ID_RECALC_SYNC_TO_EVAL);
    }
    /* #removed_rows_reconcile above may have just recorded a row that the pre-reconcile
     * #root_hash still counted (a row disabled during this very pass): storing the old hash would
     * leave it describing a graph the build did not make, so the next unchanged pass would see a
     * mismatch and rebuild the root a second time. Recompute it now that the build -- and every
     * group interface it touched -- is final. */
    tree_root_hash_set(*tree,
                       paint_layers_root_topology_hash(ma, wired_channels, layer_trees, &regen_cache));
    BKE_ntree_update_tag_all(tree);
    BKE_ntree_update_after_single_tree_change(bmain, *tree);
  }
#if PAINT_LAYERS_DEBUG_LOG
  printf("paint layers regen: root=%s groups_created=%d groups_deleted=%d groups_rebuilt=%d "
         "source_groups_changed=%d total=%.2fms\n",
         keep_root ? "kept" : "rebuilt",
         int(groups_created),
         int(groups_deleted),
         layer_groups_rebuilt,
         int(source_groups_changed),
         (BLI_time_now_seconds() - regen_start) * 1000.0);
  std::function<void(const ListBaseT<MaterialPaintLayer> &, const char *)> log_layers =
      [&](const ListBaseT<MaterialPaintLayer> &list, const char *path) {
        for (const MaterialPaintLayer &layer :
             list)
        {
          char full[192];
          SNPRINTF(full, "%s%s", path, layer.name);
          const char *state = "none";
          if (layer_trees.contains(&layer)) {
            state = unchanged_layers.contains(&layer) ? "kept" : "rebuilt";
          }
          printf("paint layers regen: %s '%s'=%s\n",
                 BKE_paint_layers_is_folder(layer) ? "folder" : "layer",
                 full,
                 state);
          if (BKE_paint_layers_is_folder(layer)) {
            char child_path[192];
            SNPRINTF(child_path, "%s/", full);
            log_layers(layer.children, child_path);
          }
        }
      };
  log_layers(ma.paint_layers, "");
#endif

  for (bNodeTree *layer_tree : old_layer_trees) {
    if (used_layer_trees.contains(layer_tree)) {
      continue;
    }
    /* The layer is gone and its group has no users; a tree the user linked keeps its users. */
    if (ID_REAL_USERS(&layer_tree->id) <= 0) {
      BKE_id_delete(&bmain, layer_tree);
      groups_deleted = true;
    }
  }
  for (bNodeTree *layer_tree : used_layer_trees) {
    /* The build asked for a group but did not instantiate it, so nothing owns it. */
    if (ID_REAL_USERS(&layer_tree->id) <= 0) {
      BKE_id_delete(&bmain, layer_tree);
      groups_deleted = true;
    }
  }
  /* Source-group wrappers are pruned by the same rule: no row of this owner reads their source any
   * more, so nothing keeps them. */
  source_groups_prune(bmain, ma);
#if PAINT_LAYERS_DEBUG_LOG
  source_group_instances_log(ma, layer_trees, source_groups, &regen_cache);
#endif

  /* The instance: found by marker, re-pointed when it names a different tree. */
  bNode *instance = instance_find(ma, ma.paint_layers_owner_uid);
  if (instance == nullptr) {
    if (ma.nodetree->typeinfo == nullptr || ma.nodetree->typeinfo->group_idname == nullptr) {
      return false;
    }
    instance = bke::node_add_node(nullptr, *ma.nodetree, ma.nodetree->typeinfo->group_idname);
    if (instance == nullptr) {
      return false;
    }
    instance->location[0] = -200.0f;
    instance->location[1] = 0.0f;
    instance->id = &tree->id;
    id_us_plus(&tree->id);
    instance_uid_set(*instance, ma.paint_layers_owner_uid);
    /* A group assigned after the node was created only grows its sockets once the updater is told
     * the node's group changed; a plain tree update does not notice. */
    BKE_ntree_update_tag_node_property(ma.nodetree, instance);
  }
  else if (instance->id != &tree->id) {
    if (instance->id != nullptr) {
      id_us_min(instance->id);
    }
    instance->id = &tree->id;
    id_us_plus(&tree->id);
    instance_uid_set(*instance, ma.paint_layers_owner_uid);
    BKE_ntree_update_tag_node_property(ma.nodetree, instance);
  }
  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);

  /* The owner's own node tree is rewritten from here. Only a row that reads its owner as its source
   * could have cached an answer about it, but dropping it costs one resolve and is always right. */
  regen_cache.invalidate_for_owner(ma);
  principled_ensure(ma, report);
  wire_instance_to_material(ma, *tree, *instance, report);
  /* The root's interface carries a mirror for every value (A1); re-declare the embedded tree's
   * instance so the value sync below finds the mirror input sockets. A group node assigned its tree
   * only grows its sockets once the updater is told, and the plain update above does not always do
   * it for inputs. */
  nodes::update_node_declaration_and_sockets(*ma.nodetree, *instance);
  values_sync_with_cache(ma, &regen_cache);

  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);

  /* One calibration line per structural rebuild: it tells the user what to set the reserved sampler
   * budget from. `over` is the count above the budget, or zero when the check is off. The count is
   * taken only now, after the stack instance is wired to the Principled: only then is the generated
   * graph reachable from a Material Output, so the count sees the user's own nodes, the stack, every
   * layer group and the wrappers -- exactly what EEVEE will allocate. Comparing it with the
   * description estimate is the calibration check for the counter and the estimate. */
  /* Re-derive the estimate now that the build reconciled the removed rows: the fallback may have
   * dropped disabled rows, and only the post-build removed set knows them. Reported unconditionally
   * so a caller (and the tests) can compare it with the finished count. */
  sampler_estimate_value = sampler_estimate();
  report.sampler_estimate = sampler_estimate_value;
#if PAINT_LAYERS_DEBUG_LOG
  if (!keep_root) {
    const int max_textures = sampler_runtime().max_textures;
    const int sampler_count = BKE_paint_layers_sampler_count(ma);
    const int estimate = sampler_estimate_value;
    const int over = (budget > 0 && sampler_count > budget) ? sampler_count - budget : 0;
    printf("paint layers samplers: material='%s' count=%d estimate=%d budget=%d max=%d over=%d "
           "fallback_rows=%d removed_hidden=%d%s.\n",
           ma.id.name + 2,
           sampler_count,
           estimate,
           budget,
           max_textures,
           over,
           fallback_rows,
           removed_hidden,
           (sampler_count != estimate) ? " MISMATCH" : "");
  }
#endif

  /* Debug-only: a kept root's interface must still be exactly what a full build would produce, so
   * its value inputs and every group's interface signature hash the same as before. values_sync
   * only writes socket values, never topology. */
  if (keep_root) {
    const uint64_t recheck = paint_layers_root_topology_hash(
        ma, wired_channels, layer_trees, &regen_cache);
    BLI_assert_msg(recheck == stored_root, "a kept root's interface drifted");
    UNUSED_VARS(recheck);
  }

  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

  /* A Material row's source material is in the depsgraph only because this layered material's
   * `build_material` puts it there, so a source that appeared, disappeared or was swapped is a
   * change of the graph's relations. The generated tree itself may be untouched (the root is kept),
   * so this pass is the only one that notices; the stored set hash forces the rebuild when it
   * moved. */
  const uint64_t source_materials = paint_layers_source_materials_hash(ma);
  uint64_t stored_source_materials = 0;
  const bool have_stored_source_materials = tree_hash_get(*tree,
                                                          TREE_SOURCE_MATERIALS_LOW_PROP,
                                                          TREE_SOURCE_MATERIALS_HIGH_PROP,
                                                          stored_source_materials);
  const bool sources_changed = !have_stored_source_materials ||
                               stored_source_materials != source_materials;
  tree_hash_set(
      *tree, TREE_SOURCE_MATERIALS_LOW_PROP, TREE_SOURCE_MATERIALS_HIGH_PROP, source_materials);

  /* A wrapper and its nested path copies are IDs of their own. Without a relations tag the
   * evaluated layer group would keep referencing the old ID (the source is no longer in the graph
   * under that pointer), and without a SYNC_TO_EVAL tag its evaluated copy would keep the old
   * body; either way the viewport would not show the rebuilt wrapper. */
  if (source_groups_changed) {
    for (const auto item : source_groups.items()) {
      if (item.value != nullptr) {
        DEG_id_tag_update(&item.value->id, ID_RECALC_SYNC_TO_EVAL);
      }
      const int source_uid = int(item.key->id.session_uid);
      for (bNodeTree &wrapper_copy : bmain.nodetrees) {
        if (&wrapper_copy == item.value ||
            !BLI_uuid_equal(tree_owner_uid_get(wrapper_copy), ma.paint_layers_owner_uid) ||
            prop_int_get(wrapper_copy.id.properties, TREE_SOURCE_PROP, 0) != source_uid)
        {
          continue;
        }
        DEG_id_tag_update(&wrapper_copy.id, ID_RECALC_SYNC_TO_EVAL);
      }
    }
  }

  /* The embedded tree and the generated group were rewritten in place. A shading tag alone
   * re-evaluates the existing evaluated copies without re-copying them, so the viewport would keep
   * the group node's old sockets and never show the new rows; a final render, which builds its own
   * copies, would. Only an ID reference that appeared or disappeared is a relation change: a kept
   * root re-uses every group, so its relations are untouched and re-tagging them would force a
   * needless relations rebuild. */
  DEG_id_tag_update(&tree->id, ID_RECALC_SYNC_TO_EVAL);
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  if (groups_created || groups_deleted || created_tree || report.replaced_foreign_tree ||
      sources_changed || source_groups_changed)
  {
    DEG_relations_tag_update(&bmain);
    report.relations_changed = true;
  }
  if (r_report != nullptr) {
    *r_report = report;
  }
  return true;
}

void BKE_paint_layers_regenerate_tagged(Main &bmain, const PaintModeSettings *paint_mode)
{
  /* Rule K-1: regeneration creates IDs and writes node trees, so it may only run on the main
   * thread. #BKE_scene_graph_update_tagged and #BKE_scene_graph_update_for_newframe_ex are also
   * reached from render and preview worker threads, so the guard belongs here rather than at each
   * call site. */
  if (!BLI_thread_is_main()) {
    return;
  }
  /* Only a material that owns its description and its tree may be regenerated. A localized or
   * evaluated copy shares the pointer with the original (see #BKE_paint_layers_generate_copy_data),
   * and rebuilding it from a copy would rewrite the original's tree. */
  const int no_regen_tags = ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN;
  for (Material &ma : bmain.materials) {
    if ((ma.id.tag & no_regen_tags) != 0) {
      continue;
    }
    if (!paint_layers_is_layered(ma)) {
      continue;
    }
    /* A file written before the field existed leaves it zero, and the derived set would then be
     * recomputed on every per-channel filter call. Freeze it here, on the original and the main
     * thread, before the generator, composite and bake ever run their loops. Quiet: this is not a
     * user edit, so no tag and no extra regeneration is forced. */
    BKE_paint_layers_channels_materialize(ma);
    /* Drain the bake subscriptions: a pixel edit to a source map shows up here and marks the
     * material for the planner, the same point the tree and slots are brought current. Then the
     * planner re-bakes the rows whose stored hash no longer matches, on the main thread. */
    BKE_paint_layers_bake_notice_changes(ma);
    BKE_paint_layers_bake_plan_run(bmain, ma);
    const bool needs_regen = (ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0 ||
                             ma.paint_layers_tree == nullptr;
    const bool needs_slots = (ma.paint_layers_flag & MA_PAINT_LAYERS_SLOTS_STALE) != 0;
    if (!needs_regen && !needs_slots) {
      continue;
    }
    if (needs_regen) {
      BKE_paint_layers_regenerate(bmain, ma);
    }
    if (needs_slots) {
      if (paint_mode != nullptr) {
        BKE_paint_layers_texpaint_slots_refresh(&ma, paint_mode);
      }
      else {
        /* No settings to rebuild from: drop the cache rather than leave dangling #Image*. */
        MEM_SAFE_DELETE(ma.texpaintslot);
        ma.tot_slots = 0;
        ma.paint_layers_flag &= ~MA_PAINT_LAYERS_SLOTS_STALE;
      }
    }
  }
}

void BKE_paint_layers_generate_copy_data(Main *bmain,
                                         Material &ma_dst,
                                         const Material &ma_src,
                                         const int copy_flag)
{
  if (ma_src.paint_layers_tree == nullptr ||
      (copy_flag & LIB_ID_CREATE_NO_USER_REFCOUNT) != 0)
  {
    ma_dst.paint_layers_tree = ma_src.paint_layers_tree;
    return;
  }
  if ((copy_flag & LIB_ID_COPY_SET_COPIED_ON_WRITE) != 0) {
    /* The depsgraph copies both IDs and relinks the reference; sharing the pointer here is what
     * makes the evaluated material use the evaluated tree. */
    ma_dst.paint_layers_tree = ma_src.paint_layers_tree;
    return;
  }

  bNodeTree *tree_copy = id_cast<bNodeTree *>(BKE_id_copy(bmain, &ma_src.paint_layers_tree->id));
  if (tree_copy == nullptr) {
    ma_dst.paint_layers_tree = nullptr;
    return;
  }
  /* #BKE_id_copy hands back a reference of its own; the generic copy pass below adds the single
   * reference the material owns, so drop the returned one to end at exactly that. */
  id_us_min(&tree_copy->id);
  ma_dst.paint_layers_tree = tree_copy;
  ma_dst.paint_layers_owner_uid = BLI_uuid_generate_random();
  tree_owner_uid_set(*tree_copy, ma_dst.paint_layers_owner_uid);
  /* Descend into the layer groups while the source owner still names them; a copy shares no tree
   * with the original, and the layer markers stay the same. */
  layer_trees_copy(
      bmain, *tree_copy, ma_src.paint_layers_owner_uid, ma_dst.paint_layers_owner_uid);

  /* The copy's embedded tree carries an instance node still pointing at the source tree; re-point
   * it before the generic copy pass walks the destination, so the copy owns only its own tree. */
  if (ma_dst.nodetree != nullptr) {
    for (bNode &node : ma_dst.nodetree->nodes) {
      bUUID node_uid = BLI_uuid_nil();
      if (!uid_prop_get(node.prop, INSTANCE_OWNER_PROP, node_uid) ||
          !BLI_uuid_equal(node_uid, ma_src.paint_layers_owner_uid))
      {
        continue;
      }
      if (node.id != nullptr) {
        id_us_min(node.id);
      }
      node.id = &tree_copy->id;
      id_us_plus(&tree_copy->id);
      instance_uid_set(node, ma_dst.paint_layers_owner_uid);
    }
  }
}

}  // namespace blender

