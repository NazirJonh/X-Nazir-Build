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

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "BLI_listbase_iterator.hh"
#include "BLI_math_vector.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"

#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_gradient.hh"
#include "BKE_paint_material_composite.hh"

#include "DEG_depsgraph.hh"

#include "NOD_socket.hh"

#include "DNA_ID.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"

#include "../paint_material_composite_internal.hh"
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
    /* A group built from its row's bake holds the row's visibility alone; one built live holds the
     * full opacity. If the row's state moved since (a pixel edit made the bake invalid, say) the
     * group has the wrong shape and cannot be fixed by a value, so the caller rebuilds. */
    const bool built_baked = prop_int_get(iface.properties, INPUT_SUBSTITUTED_PROP, 0) != 0;
    if (built_baked != paint_layer_row_substituted(ma, *layer)) {
      return false;
    }
    const float scale = forms.lookup_default(layer, {}).pass_through_scale;
    const float value = built_baked ?
                            ((layer->flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f) * scale :
                            BKE_paint_layers_channel_opacity_effective(*layer, channel) * scale;
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = value;
    if (iface.socket_data != nullptr) {
      static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = value;
    }
    return true;
  }
  if (STREQ(role, ROLE_BLEND_IS_MIX)) {
    if (socket.default_value == nullptr || channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
      return false;
    }
    /* The alpha model follows the channel's mode: Mix lays the row's alpha by its factor, every
     * other mode keeps the alpha below. Both are built; this input picks one. */
    const int ramp = BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(
        BKE_paint_layers_channel_blend_effective(*layer, channel)));
    const float value = (ramp == MA_RAMP_BLEND) ? 1.0f : 0.0f;
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = value;
    if (iface.socket_data != nullptr) {
      static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = value;
    }
    return true;
  }
  if (STREQ(role, ROLE_HEIGHT_SCALE)) {
    if (socket.default_value == nullptr) {
      return false;
    }
    /* The Height record's scale is the one value the height modes read; the channel is Height by
     * the role's own contract. */
    const float value = BKE_paint_layers_channel_height_scale(*layer,
                                                               PAINT_MATERIAL_CHANNEL_HEIGHT);
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
  if (STREQ(role, ROLE_MASK_OPACITY) || STREQ(role, ROLE_MASK_VALUE) ||
      STREQ(role, ROLE_MASK_ENABLED))
  {
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
    else if (STREQ(role, ROLE_MASK_ENABLED)) {
      value = (mask->flag & MA_PAINT_LAYER_ENABLED) != 0 ? 1.0f : 0.0f;
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
  if (STREQ(role, ROLE_GRADIENT_START) || STREQ(role, ROLE_GRADIENT_END)) {
    if (layer->gradient == nullptr || socket.default_value == nullptr) {
      return false;
    }
    const float *source = STREQ(role, ROLE_GRADIENT_START) ? layer->gradient->start :
                                                             layer->gradient->end;
    /* A UV line is 2D: the socket carries Z = 0 so the shader ignores whatever the payload holds. */
    const bool is_uv = layer->gradient->space == MA_PAINT_LAYER_GRADIENT_SPACE_UV;
    const float value[3] = {source[0], source[1], is_uv ? 0.0f : source[2]};
    copy_v3_v3(static_cast<bNodeSocketValueVector *>(socket.default_value)->value, value);
    if (iface.socket_data != nullptr) {
      copy_v3_v3(static_cast<bNodeSocketValueVector *>(iface.socket_data)->value, value);
    }
    return true;
  }
  if (STREQ(role, ROLE_GRADIENT_MIDPOINT) || STREQ(role, ROLE_GRADIENT_TYPE) ||
      STREQ(role, ROLE_GRADIENT_REPEAT))
  {
    if (layer->gradient == nullptr || socket.default_value == nullptr) {
      return false;
    }
    float value = BKE_paint_layers_gradient_midpoint_effective(*layer->gradient);
    if (STREQ(role, ROLE_GRADIENT_TYPE)) {
      value = float(layer->gradient->type);
    }
    else if (STREQ(role, ROLE_GRADIENT_REPEAT)) {
      value = float(layer->gradient->repeat);
    }
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = value;
    if (iface.socket_data != nullptr) {
      static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = value;
    }
    return true;
  }
  if (STREQ(role, ROLE_LIVE_CONSTANT) || STREQ(role, ROLE_LIVE_COVERAGE)) {
    /* A plain value of the row's source. A source that stopped supplying one here is a different
     * shape of group, which only a rebuild can make right. */
    if (socket.default_value == nullptr) {
      return false;
    }
    if (STREQ(role, ROLE_LIVE_COVERAGE)) {
      const PaintLayerMaterialCoverage coverage = paint_layer_material_coverage_source(ma, *layer);
      if (coverage.kind != PaintLayerMaterialCoverageKind::LiveConstant) {
        return false;
      }
      static_cast<bNodeSocketValueFloat *>(socket.default_value)->value = coverage.constant;
      if (iface.socket_data != nullptr) {
        static_cast<bNodeSocketValueFloat *>(iface.socket_data)->value = coverage.constant;
      }
      return true;
    }
    const PaintLayerMaterialSource source = paint_layer_material_channel_source(
        ma, *layer, channel);
    if (source.kind != PaintLayerMaterialSourceKind::LiveConstant) {
      return false;
    }
    copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(socket.default_value)->value, source.constant);
    if (iface.socket_data != nullptr) {
      copy_v4_v4(static_cast<bNodeSocketValueRGBA *>(iface.socket_data)->value, source.constant);
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

namespace {

/** Parse the key of #BLEND_NODE_KEY_PROP into its row marker, channel and mask-item flag. */
bool blend_node_key_parse(const char *key, bUUID &r_marker, int &r_channel, bool &r_mask)
{
  if (key == nullptr || key[0] == '\0' || key[1] != ':') {
    return false;
  }
  if (key[0] == 'M') {
    r_mask = true;
    r_channel = -1;
    return BLI_uuid_parse_string(&r_marker, key + 2);
  }
  if (key[0] != 'B') {
    return false;
  }
  r_mask = false;
  const char *rest = key + 2;
  const char *sep = std::strrchr(rest, ':');
  if (sep == nullptr || sep == rest || sep[1] == '\0') {
    return false;
  }
  const size_t marker_len = size_t(sep - rest);
  if (marker_len >= UUID_STRING_SIZE) {
    return false;
  }
  char marker[UUID_STRING_SIZE];
  std::memcpy(marker, rest, marker_len);
  marker[marker_len] = '\0';
  if (!BLI_uuid_parse_string(&r_marker, marker)) {
    return false;
  }
  r_channel = std::atoi(sep + 1);
  return true;
}

int blend_nodes_count(bNodeTree &group)
{
  int count = 0;
  for (bNode &node : group.nodes) {
    if (prop_string_get(node.system_properties, BLEND_NODE_KEY_PROP) != nullptr) {
      count++;
    }
  }
  return count;
}

}  // namespace

namespace bke::paint_layers {

void paint_layers_blend_nodes_commit(Material &ma)
{
  bNodeTree *root = ma.paint_layers_tree;
  if (root == nullptr) {
    return;
  }
  for (bNode &instance : root->nodes) {
    if (!instance.is_group() || instance.id == nullptr || GS(instance.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(instance.id);
    if (group == nullptr) {
      continue;
    }
    bUUID marker = BLI_uuid_nil();
    if (!layer_tree_marker_get(*group, marker)) {
      continue;
    }
    if (BKE_paint_layers_find(ma, marker) == nullptr) {
      continue;
    }
    /* A kept group keeps its count: recounting it would hide a node the user deleted from it. Only
     * a group this pass cleared (marked -1) or made fresh gets a new one. */
    if (prop_int_get(group->id.properties, BLEND_NODE_COUNT_PROP, -1) >= 0) {
      continue;
    }
    prop_int_set(group->id.properties, BLEND_NODE_COUNT_PROP, blend_nodes_count(*group));
  }
}

bool paint_layers_blend_sync(Material &ma, Main &bmain)
{
  bNodeTree *root = ma.paint_layers_tree;
  if (root == nullptr) {
    return true;
  }
  bool ok = true;
  Vector<bNodeTree *> changed_groups;
  for (bNode &instance : root->nodes) {
    if (!instance.is_group() || instance.id == nullptr || GS(instance.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(instance.id);
    if (group == nullptr) {
      continue;
    }
    bUUID group_marker = BLI_uuid_nil();
    if (!layer_tree_marker_get(*group, group_marker)) {
      continue;
    }
    if (BKE_paint_layers_find(ma, group_marker) == nullptr) {
      continue;
    }
    /* A tree without the count was built before the nodes were keyed (or by a hand edit that
     * removed the property): it cannot be trusted to carry every blend, so the caller rebuilds. */
    const int expected = prop_int_get(group->id.properties, BLEND_NODE_COUNT_PROP, -1);
    if (expected < 0) {
      /* Drop the stored shape too, so the rebuild below cannot keep this group as it is. */
      tree_hash_clear(*group);
      ok = false;
      continue;
    }
    int found = 0;
    bool group_ok = true;
    bool group_changed = false;
    for (bNode &node : group->nodes) {
      const char *key = prop_string_get(node.system_properties, BLEND_NODE_KEY_PROP);
      if (key == nullptr) {
        continue;
      }
      found++;
      bUUID marker = BLI_uuid_nil();
      int channel = -1;
      bool mask = false;
      if (!blend_node_key_parse(key, marker, channel, mask)) {
        group_ok = false;
        continue;
      }
      MaterialPaintLayer *row = BKE_paint_layers_find(ma, marker);
      if (row == nullptr) {
        group_ok = false;
        continue;
      }
      if (!mask && (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM)) {
        group_ok = false;
        continue;
      }
      const int blend = mask ? row->blend :
                               BKE_paint_layers_channel_blend_effective(*row, channel);
      const int ramp = BKE_paint_layers_blend_to_ramp(eMaterialPaintLayerBlend(blend));
      if (node.type_legacy == SH_NODE_MIX && node.storage != nullptr) {
        NodeShaderMix *storage = static_cast<NodeShaderMix *>(node.storage);
        if (storage->blend_type != ramp) {
          storage->blend_type = ramp;
          BKE_ntree_update_tag_node_property(group, &node);
          group_changed = true;
        }
      }
      else if (BKE_paint_material_is_blend_group(node)) {
        /* The dynamic group carries the mode as an integer input: the whole blend is built once
         * inside the group, so writing this value recompiles no shader. A missing or mistyped
         * socket means this is not the group the generator makes, and the tree is rebuilt. */
        bNodeSocket *mode_socket = bke::paint_layers::paint_layers_blend_group_socket(
            node, "Mode", BLEND_GROUP_ID_MODE);
        if (mode_socket == nullptr || mode_socket->type != SOCK_INT ||
            mode_socket->default_value == nullptr || ramp < MA_RAMP_BLEND ||
            ramp > MA_RAMP_EXCLUSION)
        {
          group_ok = false;
          continue;
        }
        auto &mode_value = *static_cast<bNodeSocketValueInt *>(mode_socket->default_value);
        if (mode_value.value != ramp) {
          mode_value.value = ramp;
          BKE_ntree_update_tag_node_property(group, &node);
          group_changed = true;
        }
      }
      else {
        /* A keyed node of any other type is a hand edit or a tree from an older layout: neither
         * can be trusted to carry the description's blend. */
        group_ok = false;
        continue;
      }
    }
    if (!group_ok || found != expected) {
      /* The group carries an incomplete or foreign set of blend nodes: drop its stored shape so
       * the rebuild that follows clears and builds it again, rather than keeping it by its hash. */
      tree_hash_clear(*group);
      ok = false;
    }
    if (group_changed) {
      changed_groups.append(group);
    }
  }
  /* The updates run after the walk: a tree update can touch the instances the loop is reading. */
  for (bNodeTree *group : changed_groups) {
    BKE_ntree_update_after_single_tree_change(bmain, *group);
    DEG_id_tag_update(&group->id, ID_RECALC_SYNC_TO_EVAL);
  }
  if (!changed_groups.is_empty()) {
    BKE_paint_layers_values_tag_trees(ma);
  }
  return ok;
}

}  // namespace bke::paint_layers

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
