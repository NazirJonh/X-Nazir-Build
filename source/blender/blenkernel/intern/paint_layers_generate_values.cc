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
using namespace bke::paint_layers;
namespace {

/** Write one value socket of \a instance from its row, or return false for an unknown role. */
bool values_sync_socket(Material &ma,
                        bNode &instance,
                        const bNodeTreeInterfaceSocket &iface,
                        bNodeSocket &socket,
                        const PaintLayersRegenCache *cache)
{
  const char *role = prop_string_get(iface.properties, INPUT_ROLE_PROP);
  if (role == nullptr) {
    return false;
  }
  bUUID marker = BLI_uuid_nil();
  if (!uid_prop_get(iface.properties, INPUT_MARKER_PROP, marker)) {
    return false;
  }
  const MaterialPaintLayer *layer = BKE_paint_layers_find(ma, marker);
  if (layer == nullptr || socket.default_value == nullptr) {
    return false;
  }
  if (STREQ(role, ROLE_OPACITY)) {
    const int channel = prop_int_get(iface.properties, INPUT_CHANNEL_PROP, -1);
    if (channel < 0) {
      return false;
    }
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value =
        BKE_paint_layers_channel_opacity_effective(*layer, channel) *
        pass_through_scale_of(ma, *layer, cache);
    return true;
  }
  if (STREQ(role, ROLE_FILL)) {
    const int channel = prop_int_get(iface.properties, INPUT_CHANNEL_PROP, -1);
    if (channel < 0) {
      return false;
    }
    float color[4];
    paint_layer_channel_constant(*layer, channel, color);
    if (bNodeSocketValueRGBA *value = static_cast<bNodeSocketValueRGBA *>(socket.default_value)) {
      copy_v4_v4(value->value, color);
    }
    return true;
  }
  if (STREQ(role, ROLE_CORRECTION_OPACITY)) {
    const int channel = prop_int_get(iface.properties, INPUT_CHANNEL_PROP, -1);
    if (channel < 0) {
      return false;
    }
    /* A mask correction carries the row opacity on every channel socket. */
    const bool mask_correction = BKE_paint_layers_role(*layer) == PaintLayerRole::MaskItem;
    static_cast<bNodeSocketValueFloat *>(socket.default_value)->value =
        mask_correction ? BKE_paint_layers_effective_opacity(*layer) :
                          BKE_paint_layers_channel_opacity_effective(*layer, channel);
    return true;
  }
  if (STREQ(role, ROLE_CORRECTION_FILL)) {
    float color[4];
    BKE_paint_layers_correction_constant(*layer, PAINT_MATERIAL_CHANNEL_BASE_COLOR, color);
    if (bNodeSocketValueRGBA *value = static_cast<bNodeSocketValueRGBA *>(socket.default_value)) {
      copy_v4_v4(value->value, color);
    }
    return true;
  }
  if (STREQ(role, ROLE_LIVE_CONSTANT)) {
    const int channel = prop_int_get(iface.properties, INPUT_CHANNEL_PROP, -1);
    if (channel < 0) {
      return false;
    }
    /* #layer.material is walked by #material_paint_layer_foreach_id, so on the evaluated copy of
     * `ma` this call (from #BKE_material_eval) already reads the evaluated source, exactly like the
     * generator reads the original source when it rebuilds from the original `ma`. Either way the
     * helper decides whether the channel is still live; the socket keeps its last value otherwise,
     * since a channel that stopped being live rebuilds the row and drops this input entirely. */
    float value[4];
    if (!BKE_paint_layers_material_live_constant(ma, *layer, channel, value, cache)) {
      return false;
    }
    if (bNodeSocketValueRGBA *socket_value = static_cast<bNodeSocketValueRGBA *>(
            socket.default_value))
    {
      copy_v4_v4(socket_value->value, value);
    }
    return true;
  }
  return false;
}

/**
 * Copy the description's values into the root instance's input sockets. The root carries a mirror
 * of every value in the stack (see the mirror pass at the end of #paint_layers_tree_build) and links
 * each one down to the group that owns it, so writing the root alone reaches the whole stack. That
 * is the only instance in a tree the material owns: a nested layer group lives in an #ID of its own,
 * and writing into it during evaluation would touch another ID's evaluated copy.
 */
void values_sync_instance(Material &ma, bNode &root_instance, const PaintLayersRegenCache *cache)
{
  bNodeTree *root_tree = id_cast<bNodeTree *>(root_instance.id);
  if (root_tree == nullptr) {
    return;
  }
  root_tree->ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *iface : root_tree->interface_inputs()) {
    if (iface->identifier == nullptr) {
      continue;
    }
    bNodeSocket *socket = bke::node_find_socket(
        root_instance, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
    if (socket == nullptr) {
      continue;
    }
    values_sync_socket(ma, root_instance, *iface, *socket, cache);
  }
}

}  // namespace
void BKE_paint_layers_values_sync(Material &ma)
{
  /* The sync runs on every value edit and asks the Pass Through scale once per opacity socket; the
   * cache turns those walks of the whole stack into one. It is built from `ma` itself, which on the
   * evaluated copy is the description being synced. */
  const PaintLayersRegenCache cache;
  values_sync_with_cache(ma, &cache);
}

namespace bke::paint_layers {

void values_sync_with_cache(Material &ma, const PaintLayersRegenCache *cache)
{
  /* Custom parameters live on the description, not in the generated tree (a Custom layer has no
   * instance there); give missing ones their socket defaults before syncing the rest. */
  BKE_paint_layers_custom_properties_sync(ma);

  bNode *instance = instance_find(ma, ma.paint_layers_owner_uid);
  if (instance == nullptr || ma.nodetree == nullptr) {
    return;
  }
  /* Only the root instance is written. Its mirrors link down through the folder group interfaces to
   * every layer group, so a nested tree -- another #ID -- is never touched, and no DEG tag is needed
   * during evaluation: the evaluated copy is edited in place and #GPU_material_free runs after. */
  values_sync_instance(ma, *instance, cache);
}

}  // namespace bke::paint_layers

}  // namespace blender

