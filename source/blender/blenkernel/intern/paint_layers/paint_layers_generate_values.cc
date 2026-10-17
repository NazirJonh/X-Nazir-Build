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
#include "paint_layers_kind.hh"

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
    const float value = BKE_paint_layers_channel_opacity_effective(*layer, channel) *
                        forms.lookup_default(layer, {}).pass_through_scale;
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = value;
    if (iface.socket_data != nullptr) {
      static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = value;
    }
    return true;
  }
  if (STREQ(role, ROLE_PARAM)) {
    /* The position names the parameter in the row's kind schema; a row whose kind lost it (a
     * hand-edited tree) cannot be trusted, so the caller rebuilds. */
    const PaintLayerKindInfo *kind = paint_layer_kind_of(*layer);
    const int index = prop_int_get(iface.properties, INPUT_PARAM_PROP, -1);
    if (kind == nullptr || index < 0 || index >= kind->params_num || socket.default_value == nullptr)
    {
      return false;
    }
    const float value = paint_layer_kind_param_get(*layer, index);
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = value;
    if (iface.socket_data != nullptr) {
      static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = value;
    }
    return true;
  }
  if (STREQ(role, ROLE_MASK_OPACITY) || STREQ(role, ROLE_MASK_VALUE)) {
    /* The marker names the owner row, and the item is whatever base mask it carries now; a mask
     * correction's inputs name the correction itself. */
    const MaterialPaintLayer *mask = (layer->role == MA_PAINT_LAYER_ROLE_MASK_ITEM) ?
                                         layer :
                                         BKE_paint_layers_mask_base(*layer);
    if (mask == nullptr || socket.default_value == nullptr) {
      return false;
    }
    float value = 0.0f;
    if (STREQ(role, ROLE_MASK_OPACITY)) {
      value = BKE_paint_layers_effective_opacity(*mask);
    }
    else if (paint_layer_kind_is_map_coverage(*mask)) {
      /* Gradient carries the item's standard value; White/Black read one. */
      value = BKE_paint_layers_mask_item_output_value(*mask);
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
  if (STREQ(role, ROLE_MAPPING_OFFSET) || STREQ(role, ROLE_MAPPING_SCALE) ||
      STREQ(role, ROLE_MAPPING_ROTATION))
  {
    /* The Mapping's three Vector inputs; the marker names the row that owns the mapping. The scale
     * goes through the helper the CPU shares, so an old file's zero axis reads as one on both. */
    if (socket.default_value == nullptr) {
      return false;
    }
    float value[3] = {0.0f, 0.0f, 0.0f};
    if (STREQ(role, ROLE_MAPPING_OFFSET)) {
      value[0] = layer->mapping.offset[0];
      value[1] = layer->mapping.offset[1];
    }
    else if (STREQ(role, ROLE_MAPPING_SCALE)) {
      value[0] = BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[0]);
      value[1] = BKE_paint_layers_mapping_scale_normalize(layer->mapping.scale[1]);
      value[2] = 1.0f;
    }
    else {
      value[2] = layer->mapping.rotation;
    }
    copy_v3_v3(static_cast<bNodeSocketValueVector *>(socket.default_value)->value, value);
    if (iface.socket_data != nullptr) {
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(iface.socket_data)->value, value);
    }
    return true;
  }
  if (STREQ(role, ROLE_FILL_ALPHA)) {
    const MaterialPaintLayerChannel *record = paint_layer_channel_find(*layer,
                                                                      eMaterialPaintChannel(channel));
    if (record == nullptr || socket.default_value == nullptr) {
      return false;
    }
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = record->value[3];
    if (iface.socket_data != nullptr) {
      static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = record->value[3];
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
  if (STREQ(role, ROLE_ID_COLOR_ENABLED) || STREQ(role, ROLE_ID_COLOR) ||
      STREQ(role, ROLE_ID_COLOR_TOLERANCE))
  {
    /* One colour of a Color ID item's list, named by its position; a list that shrank under the
     * tree is a hand edit, so the caller rebuilds rather than keeping a stale colour. */
    const int index = prop_int_get(iface.properties, INPUT_ID_COLOR_PROP, -1);
    if (index < 0 || socket.default_value == nullptr) {
      return false;
    }
    const MaterialPaintLayerIdColor *color = nullptr;
    int position = 0;
    for (const MaterialPaintLayerIdColor &entry : layer->id_colors) {
      if (position == index) {
        color = &entry;
        break;
      }
      position++;
    }
    if (color == nullptr) {
      return false;
    }
    if (STREQ(role, ROLE_ID_COLOR_ENABLED)) {
      const float value = (color->flag & MA_PAINT_LAYER_ID_COLOR_ENABLED) != 0 ? 1.0f : 0.0f;
      static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = value;
      if (iface.socket_data != nullptr) {
        static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = value;
      }
    }
    else if (STREQ(role, ROLE_ID_COLOR_TOLERANCE)) {
      static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = color->tolerance;
      if (iface.socket_data != nullptr) {
        static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = color->tolerance;
      }
    }
    else {
      float value[4] = {color->color[0], color->color[1], color->color[2], 1.0f};
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket.default_value)->value, value);
      if (iface.socket_data != nullptr) {
        copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(iface.socket_data)->value, value);
      }
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
