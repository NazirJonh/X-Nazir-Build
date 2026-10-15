/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Topology hashes of a layered material's generated trees.
 *
 * A row's group and the root are a function of the description's structure alone. Their values
 * (opacity, a Fill constant, the enabled flag) are group inputs and never move these hashes, which
 * is what lets a value edit keep every group and only sync the values
 * (#BKE_paint_layers_values_sync).
 */

#include "BKE_paint_layers_generate.hh"

#include "BLI_hash.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_uuid.h"

#include "BKE_idprop.hh"
#include "BKE_paint_layers.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_uuid_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "paint_layers_intern.hh"

namespace blender::bke::paint_layers {

/* -------------------------------------------------------------------- */
/** \name Hash accumulation
 * \{ */

uint64_t topology_hash_mix(const uint64_t hash, const uint64_t value)
{
  /* Why not #get_default_hash(hash, value): that XORs its arguments, so the fold would be
   * commutative and reordering rows would leave the root hash -- and the generated chain -- as it
   * was. */
  return hash ^ (value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2));
}

void topology_hash_string(uint64_t &hash, const char *text)
{
  hash = topology_hash_mix(hash, (text != nullptr) ? get_default_hash(StringRef(text)) : 0);
}

void topology_hash_uid(uint64_t &hash, const bUUID &uid)
{
  hash = topology_hash_mix(hash, UUID(uid).hash());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Stored hashes
 * \{ */

void tree_hash_set(bNodeTree &tree, const char *low_key, const char *high_key, const uint64_t hash)
{
  prop_int_set(tree.id.properties, low_key, int(uint32_t(hash & 0xFFFFFFFFu)));
  prop_int_set(tree.id.properties, high_key, int(uint32_t(hash >> 32)));
}

bool tree_hash_get(bNodeTree &tree, const char *low_key, const char *high_key, uint64_t &r_hash)
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

void tree_hash_clear(bNodeTree &tree)
{
  IDProperty *props = IDP_GetProperties(&tree.id);
  if (props == nullptr) {
    return;
  }
  for (const char *key : {TREE_TOPOLOGY_LOW_PROP,
                          TREE_TOPOLOGY_HIGH_PROP,
                          TREE_ROOT_TOPOLOGY_LOW_PROP,
                          TREE_ROOT_TOPOLOGY_HIGH_PROP})
  {
    if (IDProperty *prop = IDP_GetPropertyFromGroup(props, key)) {
      IDP_FreeFromGroup(props, prop);
    }
  }
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

/** \} */

/* -------------------------------------------------------------------- */
/** \name Topology of a row and of the root
 * \{ */

uint64_t paint_layers_layer_topology_hash(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          const Span<int> wired_channels,
                                          const PaintLayerFormMap &forms)
{
  uint64_t hash = 0;
  hash = topology_hash_mix(hash, uint64_t(layout::kLayoutVersion));
  /* Source and blend decide the nodes; the enabled flag and every value are inputs, so they are
   * deliberately absent. */
  hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.source)));
  hash = topology_hash_mix(hash, uint64_t(uint8_t(layer.blend)));
  topology_hash_string(hash, layer.name);
  /* A row inside an isolating folder is built in the isolated form, with sockets of its own. */
  hash = topology_hash_mix(hash, forms.lookup_default(&layer, {}).in_isolating_folder ? 1 : 0);
  /* A Pass Through folder builds no group, an isolating one does: the mode is structure, although
   * what decides it (mask, blend, opacity) is not all in the hash below. */
  hash = topology_hash_mix(hash, BKE_paint_layers_folder_is_pass_through(layer) ? 1 : 0);
  for (const int channel : wired_channels) {
    hash = topology_hash_mix(hash, uint64_t(channel));
    hash = topology_hash_mix(hash, row_participates(ma, layer, channel) ? 1 : 0);
    /* A map and a flat value are different nodes (an Image Texture against a value input), and the
     * texture node names its image: assigning, swapping or removing a map is structure, even when
     * the channel took part before and after. */
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(layer, channel);
    if (record != nullptr && record->image != nullptr) {
      topology_hash_string(hash, record->image->id.name);
    }
    else {
      hash = topology_hash_mix(hash, 0);
    }
  }
  /* A content correction is nodes of its own (a map, a Mix and a coverage fold) in each channel it
   * lays a map in, so which ones do, and which map, is structure; its opacity is an input. Its
   * marker names those inputs, so a removed and re-added correction is a different shape. */
  for (const MaterialPaintLayer &effect : layer.effects) {
    topology_hash_uid(hash, effect.marker);
    hash = topology_hash_mix(hash, uint64_t(uint8_t(effect.blend)));
    for (const int channel : wired_channels) {
      if (paint_layer_effect_lays(ma, effect, channel)) {
        topology_hash_string(hash, paint_layer_channel_find(effect, channel)->image->id.name);
      }
      else {
        hash = topology_hash_mix(hash, 0);
      }
      /* A row kept only for its corrections starts from zero coverage instead of its constant. */
      hash = topology_hash_mix(hash, paint_layer_row_empty_base(ma, layer, channel) ? 1 : 0);
    }
  }
  /* A mask is nodes (an Image Texture and its grey chain against a value input), so its presence
   * and whether it has a map are structure; its constant, opacity and enabled flag are inputs. */
  if (const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(layer)) {
    hash = topology_hash_mix(hash, 1);
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(
        *mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
    if (record != nullptr && record->image != nullptr) {
      topology_hash_string(hash, record->image->id.name);
    }
    else {
      hash = topology_hash_mix(hash, 0);
    }
  }
  else {
    hash = topology_hash_mix(hash, 0);
  }
  return hash;
}

uint64_t paint_layers_root_topology_hash(const Material &ma, const Span<int> wired_channels)
{
  uint64_t hash = 0;
  hash = topology_hash_mix(hash, uint64_t(layout::kLayoutVersion));
  for (const int channel : wired_channels) {
    hash = topology_hash_mix(hash, uint64_t(channel));
  }
  /* The rows in order, each with its own topology: adding, removing or reordering a row moves this,
   * a value edit does not. A row that takes part in no wired channel builds no group and is left
   * out, so a change to it cannot make the root rebuild for nothing. */
  const PaintLayerFormMap forms = paint_layers_form_map(ma);
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer *parent) -> bool {
        bool participates = false;
        for (const int channel : wired_channels) {
          if (row_participates(ma, layer, channel)) {
            participates = true;
            break;
          }
        }
        if (!participates) {
          return true;
        }
        /* The parent is part of the shape: moving a row into or out of a folder rewires the chain
         * even when the order of the rows does not change. */
        topology_hash_uid(hash, layer.marker);
        topology_hash_uid(hash, parent != nullptr ? parent->marker : bUUID{});
        hash = topology_hash_mix(hash,
                                 paint_layers_layer_topology_hash(ma, layer, wired_channels, forms));
        return true;
      });
  return hash;
}

/** \} */

}  // namespace blender::bke::paint_layers
