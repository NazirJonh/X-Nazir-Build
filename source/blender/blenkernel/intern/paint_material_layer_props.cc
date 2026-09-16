/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * A paint layer's own properties: its fill colour, its name, its enabled state, its opacity and
 * its per-channel values. Its mask lives in `paint_material_layer_mask.cc`. See
 * #BKE_paint_material_layer_edit.hh; the chain-reading infrastructure these mutations share with
 * `paint_material_layer_edit.cc` and `paint_material_layer_channels.cc` lives in
 * `paint_material_layer_edit_intern.hh`.
 */

#include "paint_material_layer_edit_intern.hh"

#include "MEM_guardedalloc.h"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"
#include "BKE_preview_image.hh"

#include "BLT_translation.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_color.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"
#include "BLI_string_utf8.h"
#include "BLI_vector.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_tree_interface_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"
#include "DNA_uuid_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include <utility>

#include "paint_material_composite_internal.hh"
#include "paint_material_layer_idprops.hh"

namespace blender {

void image_fill_flat(Image &image, const float color[4])
{
  /* A generated map nobody has painted is rebuilt from its tile's colour whenever its buffer is
   * dropped -- a file reload, a memory purge -- so that colour has to move with the pixels, or the
   * refill silently reverts to the colour the layer was created with. A map that has been painted
   * is saved from its buffer instead, and the refill makes it dirty like any other edit. */
  const bool regenerates = image.source == IMA_SRC_GENERATED && !BKE_image_is_dirty(&image);
  if (regenerates) {
    if (ImageTile *tile = BKE_image_get_tile(&image, 0)) {
      copy_v4_v4(tile->gen_color, color);
    }
  }

  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, &iuser, &lock);
  if (ibuf != nullptr) {
    /* The same colour-space handling the generator applies (see `add_ibuf_for_tile`): a byte
     * buffer takes the colour as given, a float buffer of a colour map takes it linearized. */
    if (uint8_t *bytes = ibuf->byte_data_for_write()) {
      BKE_image_buf_fill_color(bytes, nullptr, ibuf->x, ibuf->y, color);
    }
    if (float *floats = ibuf->float_data_for_write()) {
      float float_color[4];
      if (IMB_colormanagement_space_name_is_data(image.colorspace_settings.name)) {
        copy_v4_v4(float_color, color);
      }
      else {
        srgb_to_linearrgb_v4(float_color, color);
      }
      BKE_image_buf_fill_color(nullptr, floats, ibuf->x, ibuf->y, float_color);
    }
    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    if (!regenerates) {
      BKE_image_mark_dirty(&image, ibuf);
    }
    BKE_image_release_ibuf(&image, ibuf, lock);
  }
  /* The Outliner row and every other consumer of the ID's preview icon draw the cached
   * thumbnail: without clearing it here they keep showing the pre-fill colour until something
   * else happens to invalidate it. The next draw re-renders it from the new pixels (as a job),
   * so both picker ticks and the bake refresh the row preview. */
  if (image.preview != nullptr) {
    BKE_previewimg_clear(image.preview);
  }
  BKE_image_partial_update_mark_full_update(&image);
}

bool BKE_paint_material_layer_fill_color_apply(Main &bmain,
                                               Material &ma,
                                               const int ordinal,
                                               const float color[4],
                                               PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists, the material is writable, and every channel's node for the row
   * carries the Fill kind -- all decided without writing a byte. Re-filling a painted layer would
   * silently destroy work, so the kind check runs across all channels before the first pixel. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::FillColorSet, plan, error)) {
    return fail(error);
  }
  for (const ChannelChain *chain : plan.chains) {
    const ChainLayer &layer = chain->layers[plan.layer_index];
    if (BKE_paint_material_layer_kind_get(*layer.node) != PaintMaterialLayerKind::Fill) {
      return fail(PaintMaterialLayerEditError::IndexOutOfRange);
    }
  }
  /* 2. Shape: nothing to convert. A bare base takes the fill as its own map, exactly the way
   * #layer_image_create made it; a Mix layer takes it through the map its top socket shows. */
  /* 3. Mutation: re-fill every wired channel's map, then record the colour on the marker. */
  for (ChannelChain *chain : plan.chains) {
    ChainLayer &layer = chain->layers[plan.layer_index];
    /* The colour is recorded in every channel, like the kind: a reader may look at any of them. */
    BKE_paint_material_layer_fill_color_set(*layer.node, color);
    if (layer.image == nullptr) {
      /* A channel the layer never got a map for behaves as unwired for this layer. */
      continue;
    }
    float map_color[4];
    fill_map_color_for(chain->channel, color, map_color);
    image_fill_flat(*layer.image, map_color);
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_fill_color_preview(Main &bmain,
                                                 Material &ma,
                                                 const int ordinal,
                                                 const float color[4],
                                                 PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight, exactly like #BKE_paint_material_layer_fill_color_apply: the row exists, the
   * material is writable, and every channel's node for the row carries the Fill kind -- all
   * decided without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::FillColorSet, plan, error)) {
    return fail(error);
  }
  for (const ChannelChain *chain : plan.chains) {
    const ChainLayer &layer = chain->layers[plan.layer_index];
    if (BKE_paint_material_layer_kind_get(*layer.node) != PaintMaterialLayerKind::Fill) {
      return fail(PaintMaterialLayerEditError::IndexOutOfRange);
    }
  }
  /* 2. Shape: nothing to convert, like the apply. */
  /* 3. Mutation: re-fill every wired channel's map, deliberately recording nothing on the
   * layer's marker -- the marker is what the layer *stands for*, and only the dialog's exec
   * (the bake) moves it. The revision bump and cache invalidation below are still needed so
   * the stack reader and the compositor see the new pixels. */
  for (ChannelChain *chain : plan.chains) {
    ChainLayer &layer = chain->layers[plan.layer_index];
    if (layer.image == nullptr) {
      /* A channel the layer never got a map for behaves as unwired for this layer. */
      continue;
    }
    float map_color[4];
    fill_map_color_for(chain->channel, color, map_color);
    image_fill_flat(*layer.image, map_color);
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

/**
 * The write half of the per-channel value API: re-fill the map of \a channel on the row at \a
 * ordinal with the flat colour \a color stands for there, and -- when \a record_marker -- record
 * the raw \a color on the row's node for #BKE_paint_material_layer_channel_value_get to read.
 *
 * Shared by the apply and the preview so the two cannot drift; what the preview leaves out is the
 * recording, nothing else.
 */
static bool layer_channel_value_write(Main &bmain,
                                      Material &ma,
                                      const int ordinal,
                                      const int channel,
                                      const float color[4],
                                      const bool record_marker,
                                      PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists and the material is writable, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::FillColorSet, plan, error)) {
    return fail(error);
  }
  /* 2. The channel addressed: a row the channel does not carry -- no chain of its own, or no map
   * in it -- behaves as out of range. */
  ChannelChain *target = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == channel) {
      target = chain;
    }
  }
  if (target == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  ChainLayer &layer = target->layers[plan.layer_index];
  if (layer.image == nullptr || layer.node == nullptr) {
    /* A channel the layer never got a map for behaves as unwired for this layer. */
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  /* 3. Mutation: re-fill this channel's own map, then record the value on the row's node. */
  float map_color[4];
  fill_map_color_for(channel, color, map_color);
  image_fill_flat(*layer.image, map_color);
  if (record_marker) {
    bke::paint_layer::channel_value_set(*layer.node, channel, color);
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_channel_value_get(Main &bmain,
                                                Material &ma,
                                                const int ordinal,
                                                const int channel,
                                                float r_color[4])
{
  /* The same plan the writes build: a row this module cannot resolve has nothing to read. */
  LayerEditPlan plan;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::FillColorSet, plan, error)) {
    return false;
  }
  for (const ChannelChain *chain : plan.chains) {
    if (chain->channel != channel) {
      continue;
    }
    const ChainLayer &layer = chain->layers[plan.layer_index];
    if (layer.node == nullptr) {
      return false;
    }
    /* The marker, not the pixels: a painted-over map cannot be asked what it was filled with. */
    return bke::paint_layer::channel_value_get(*layer.node, channel, r_color);
  }
  return false;
}

bool BKE_paint_material_layer_channel_image_assigned_get(Main &bmain,
                                                        Material &ma,
                                                        const int ordinal,
                                                        const int channel)
{
  /* The same plan the writes build: a row this module cannot resolve has nothing to read. */
  LayerEditPlan plan;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::FillColorSet, plan, error)) {
    return false;
  }
  for (const ChannelChain *chain : plan.chains) {
    if (chain->channel != channel) {
      continue;
    }
    const ChainLayer &layer = chain->layers[plan.layer_index];
    if (layer.node == nullptr) {
      return false;
    }
    /* The record, not the graph: an assigned image carries the row's tag like a generated map. */
    return bke::paint_layer::channel_image_assigned_get(*layer.node, channel);
  }
  return false;
}

bool BKE_paint_material_layer_channel_value_preview(Main &bmain,
                                                    Material &ma,
                                                    const int ordinal,
                                                    const int channel,
                                                    const float color[4],
                                                    PaintMaterialLayerEditError *r_error)
{
  return layer_channel_value_write(bmain, ma, ordinal, channel, color, false, r_error);
}

bool BKE_paint_material_layer_channel_value_apply(Main &bmain,
                                                  Material &ma,
                                                  const int ordinal,
                                                  const int channel,
                                                  const float color[4],
                                                  PaintMaterialLayerEditError *r_error)
{
  return layer_channel_value_write(bmain, ma, ordinal, channel, color, true, r_error);
}

bool BKE_paint_material_layer_channel_unlink(Main &bmain,
                                             Material &ma,
                                             const int ordinal,
                                             const int channel,
                                             PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* 1. Preflight: the row exists and the material is writable, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::FillColorSet, plan, error)) {
    return fail(error);
  }
  /* 2. The channel addressed: a row the channel does not carry -- no chain of its own, or no map
   * in it -- behaves as out of range. */
  ChannelChain *target = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == channel) {
      target = chain;
    }
  }
  if (target == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  ChainLayer &layer = target->layers[plan.layer_index];
  if (layer.image == nullptr || layer.node == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* 3. The value the map gets back: the last one recorded on the marker, or the neutral one a map
   * of the channel starts at. The neutral value is recorded like an applied one, so the picker
   * reads back what the map holds. */
  float value[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  bke::paint_layer::channel_value_get(*layer.node, channel, value);
  float map_color[4];
  fill_map_color_for(channel, value, map_color);

  /* 4. Mutation: a fresh map of the row's own takes the channel over, created the way the
   * channel-enable path creates one. Whatever the channel showed is never written to: an image
   * assigned through #BKE_paint_material_layer_channel_image_set carries the row's tag like a
   * generated one, so owning cannot be read back off the graph, and refilling it in place might
   * destroy a dropped image's pixels. */
  PaintMaterialLayerAddParams params;
  params.kind = BKE_paint_material_layer_kind_get(*layer.node);
  BKE_paint_material_layer_fill_color_get(*layer.node, params.fill_color);
  int size_x = 1024;
  int size_y = 1024;
  if (!BKE_paint_material_layer_map_size_get(bmain, ma, ordinal, size_x, size_y) || size_x <= 0 ||
      size_y <= 0)
  {
    size_x = 1024;
    size_y = 1024;
  }
  params.image_size = size_x;
  Image *fresh = layer_image_create(bmain, channel, params);
  if (fresh == nullptr) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  if (!BKE_paint_material_layer_channel_image_set(bmain, ma, ordinal, channel, *fresh, &error)) {
    /* Never wired: the fresh map is still only the creation's user, this call's to free. */
    BKE_id_free(&bmain, fresh);
    return fail(error);
  }
  /* The map node's user is the one the image-set took; the one the creation gave is the extra. */
  id_us_min(&fresh->id);

  /* 5. The wiring re-shaped the channel's chain, so the plan is read again before the pixels and
   * the record are written -- the pattern every edit that relinks first follows. */
  plan = LayerEditPlan();
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::FillColorSet, plan, error)) {
    BLI_assert_unreachable();
    return fail(error);
  }
  target = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == channel) {
      target = chain;
    }
  }
  if (target == nullptr) {
    BLI_assert_unreachable();
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  image_fill_flat(*fresh, map_color);
  bke::paint_layer::channel_value_set(*target->layers[plan.layer_index].node, channel, value);
  bke::paint_layer::channel_image_assigned_set(
      *target->layers[plan.layer_index].node, channel, false);

  /* The relation edit was #BKE_paint_material_layer_channel_image_set's to commit; the refill and
   * the record are values inside one tree. */
  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_kind_set(Main &bmain,
                                       Material &ma,
                                       const int ordinal,
                                       const PaintMaterialLayerKind kind,
                                       PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists and the material is writable, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::KindSet, plan, error)) {
    return fail(error);
  }
  /* 2. Mutation: the kind marker is written on the layer's node in every channel at once, so the
   * row reads as one kind no matter which channel a reader looks at. */
  for (ChannelChain *chain : plan.chains) {
    BKE_paint_material_layer_kind_set(*chain->layers[plan.layer_index].node, kind);
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_rename(Main &bmain,
                                     Material &ma,
                                     const int ordinal,
                                     const char *name,
                                     PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  if (name == nullptr) {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* 1. Preflight: the row exists and a bare base is refused, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::Rename, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * rename is known to happen; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::Rename, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. The name a user sees is the label of the
   * layer's Mix nodes, set in every channel at once. */
  for (ChannelChain *chain : plan.chains) {
    STRNCPY_UTF8(chain->layers[plan.layer_index].node->label, name);
  }

  paint_layer_edit_committed(bmain, ma, false);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_set_enabled(Main &bmain,
                                          Material &ma,
                                          const int ordinal,
                                          const bool enable,
                                          PaintMaterialLayerEditError *r_error)
{
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  /* 1. Preflight: the row exists and a bare base is refused, without writing a byte. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::SetEnabled, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * toggle is known to happen; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::SetEnabled, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. Muting the layer's Mix nodes is what the UI
   * model reads back as "disabled", in every channel at once. */
  for (ChannelChain *chain : plan.chains) {
    bNode &node = *chain->layers[plan.layer_index].node;
    SET_FLAG_FROM_TEST(node.flag, !enable, NODE_MUTED);
    BKE_ntree_update_tag_node_mute(chain->tree, &node);
  }
  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  /* The Shading component is NO_COW_TAG_ON_UPDATE: ID_RECALC_SHADING alone never re-copies the
   * evaluated material, so the mute flag GPU compilation reads would stay on whatever it was at
   * the last relation sync. #SYNC_TO_EVAL is what actually refreshes it, the same fix already
   * applied where a node's ID field changes (see #paint_layer_edit_committed). */
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

void BKE_paint_material_layer_opacity_changed(Main &bmain, bNodeTree &tree, bNodeSocket &socket)
{
  BKE_ntree_update_tag_socket_property(&tree, &socket);
  BKE_ntree_update_after_single_tree_change(bmain, tree);

  /* A correction's opacity is meant to read as one shared strength across every channel, unlike a
   * layer's own opacity -- one node per channel, genuinely independent (plan 19 review). The
   * Outliner only ever writes the socket of the channel #stack_layer_channel currently shows, so
   * without this the other channels' copies of the same correction would silently keep the old
   * value. #socket sits on the correction's coverage-multiply node; its single output feeds the
   * correction's own Mix, which is where the marker naming the row lives. */
  bUUID correction_marker = {};
  bool is_correction = false;
  {
    tree.ensure_topology_cache();
    bNodeSocket *result = static_cast<bNodeSocket *>(socket.owner_node().outputs.first);
    if (result != nullptr && result->directly_linked_links().size() == 1) {
      bNode *host = result->directly_linked_links()[0]->tonode;
      if (host != nullptr && bke::paint_layer::node_is_correction(*host)) {
        correction_marker = BKE_paint_material_layer_marker_get(*host);
        is_correction = true;
      }
    }
  }
  const float opacity_value = socket.default_value_typed<bNodeSocketValueFloat>()->value;

  /* The socket may live in a folder's own tree, whose evaluated copy is separate; every material
   * reaching it is refreshed too, since the Shading component alone never re-copies the evaluated
   * material (see #paint_layer_edit_committed). */
  DEG_id_tag_update(&tree.id, ID_RECALC_SYNC_TO_EVAL);
  for (Material &ma : bmain.materials) {
    if (ma.nodetree == nullptr) {
      continue;
    }
    if (ma.nodetree == &tree || bke::node_tree_contains_tree(*ma.nodetree, tree)) {
      if (is_correction) {
        BKE_paint_material_layer_correction_opacity_set(
            bmain, ma, correction_marker, opacity_value, nullptr);
      }
      paint_layer_edit_committed(bmain, ma, false);
      DEG_id_tag_update(&ma.id, ID_RECALC_SYNC_TO_EVAL);
    }
  }
}

}  // namespace blender
