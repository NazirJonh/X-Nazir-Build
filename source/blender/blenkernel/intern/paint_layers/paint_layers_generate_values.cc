/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The values half of the paint-layer generator.
 *
 * The generated topology (the channel chains, the row groups, the interface) is a function of the
 * description's *structure* alone; a row's animatable values -- its opacity and a Fill row's
 * constant -- are inputs of that row's group. This file copies the current values from the
 * description into those inputs, without touching a single node, so a slider move never rebuilds a
 * group. #BKE_paint_layers_values_sync is called on every value edit and from the material's
 * evaluation, where an animated value has to reach the evaluated copy.
 */

#include "BKE_paint_layers_generate.hh"

#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"

#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_paint_layers.hh"

#include "DEG_depsgraph.hh"

#include "NOD_socket.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_intern.hh"

namespace blender {
using namespace bke::paint_layers;

namespace {

/**
 * Write one value socket of \a instance from its row. \return false when the socket names a value
 * the description no longer carries (or vice versa), which the caller treats as "the tree was
 * hand-edited": it rebuilds rather than silently keeping a stale value. A socket with no role is a
 * contract socket (Below/Result), not a value, and is left alone.
 */
bool values_sync_socket(Material &ma,
                        const PaintLayerFormMap &forms,
                        const bNodeTreeInterfaceSocket &iface,
                        bNodeSocket &socket)
{
  const char *role = prop_string_get(iface.properties, INPUT_ROLE_PROP);
  if (role == nullptr) {
    return true;
  }
  bUUID marker = BLI_uuid_nil();
  if (!uid_prop_get(iface.properties, INPUT_MARKER_PROP, marker)) {
    return false;
  }
  const MaterialPaintLayer *layer = BKE_paint_layers_find(ma, marker);
  if (layer == nullptr) {
    return false;
  }
  const int channel = prop_int_get(iface.properties, INPUT_CHANNEL_PROP, -1);

  if (STREQ(role, ROLE_OPACITY)) {
    if (socket.default_value == nullptr) {
      return false;
    }
    const float value = BKE_paint_layers_effective_opacity(*layer) *
                        forms.lookup_default(layer, {}).pass_through_scale;
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = value;
    if (iface.socket_data != nullptr) {
      static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = value;
    }
    return true;
  }
  if (STREQ(role, ROLE_MASK_OPACITY) || STREQ(role, ROLE_MASK_VALUE)) {
    /* The marker names the owner row; the item is whatever base mask it carries now. */
    const MaterialPaintLayer *mask = BKE_paint_layers_mask_base(*layer);
    if (mask == nullptr || socket.default_value == nullptr) {
      return false;
    }
    float value = 0.0f;
    if (STREQ(role, ROLE_MASK_OPACITY)) {
      value = BKE_paint_layers_effective_opacity(*mask);
    }
    else {
      const MaterialPaintLayerChannel *record = paint_layer_channel_find(
          *mask, PAINT_MATERIAL_CHANNEL_BASE_COLOR);
      if (record == nullptr) {
        return false;
      }
      value = paint_layer_mask_constant(*record);
    }
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = value;
    if (iface.socket_data != nullptr) {
      static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = value;
    }
    return true;
  }
  if (STREQ(role, ROLE_FILL)) {
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(*layer,
                                                                      eMaterialPaintChannel(channel));
    if (record == nullptr || socket.default_value == nullptr) {
      return false;
    }
    copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket.default_value)->value, record->value);
    if (iface.socket_data != nullptr) {
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(iface.socket_data)->value, record->value);
    }
    return true;
  }
  return false;
}

}  // namespace

bool BKE_paint_layers_values_sync(Material &ma)
{
  bNodeTree *root = ma.paint_layers_tree;
  if (root == nullptr) {
    return true;
  }
  /* The value inputs live on the row groups' own interfaces and on their instances in this root.
   * The shader reads the instance sockets, so those are what must agree with the description. */
  bool ok = true;
  const PaintLayerFormMap forms = paint_layers_form_map(ma);
  for (bNode &node : root->nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group_tree = id_cast<bNodeTree *>(node.id);
    if (group_tree == nullptr) {
      continue;
    }
    bUUID marker = BLI_uuid_nil();
    if (!layer_tree_marker_get(*group_tree, marker)) {
      continue;
    }
    if (BKE_paint_layers_find(ma, marker) == nullptr) {
      continue;
    }
    group_tree->ensure_interface_cache();
    for (bNodeTreeInterfaceSocket *iface : group_tree->interface_inputs()) {
      if (iface->identifier == nullptr) {
        continue;
      }
      bNodeSocket *socket = bke::node_find_socket(
          node, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
      if (socket == nullptr) {
        /* A value input the description owns with no socket on the instance is a hand-edited tree;
         * report it so the caller can fall back to a full regeneration. */
        if (prop_string_get(iface->properties, INPUT_ROLE_PROP) != nullptr) {
          ok = false;
        }
        continue;
      }
      ok = values_sync_socket(ma, forms, *iface, *socket) && ok;
    }
  }
  return ok;
}

void BKE_paint_layers_values_tag_trees(Material &ma)
{
  bNodeTree *root = ma.paint_layers_tree;
  if (root == nullptr) {
    return;
  }
  /* Every row's group is a data-block of its own, and so is the root holding their instances: the
   * evaluated copy the shader reads is only refreshed when that tree is tagged, tagging the
   * material alone leaves it on the old value. Kept apart from #BKE_paint_layers_values_sync,
   * which the evaluation itself calls on an evaluated copy and must therefore tag nothing. */
  for (bNode &node : root->nodes) {
    if (node.is_group() && node.id != nullptr && GS(node.id->name) == ID_NT) {
      DEG_id_tag_update(node.id, ID_RECALC_SYNC_TO_EVAL);
    }
  }
  DEG_id_tag_update(&root->id, ID_RECALC_SYNC_TO_EVAL);
}

}  // namespace blender
