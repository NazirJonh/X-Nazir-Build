/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * A paint layer's own properties: its mask, its fill colour, its name, its enabled state and
 * its opacity. See #BKE_paint_material_layer_edit.hh; the chain-reading infrastructure these
 * mutations share with `paint_material_layer_edit.cc` and `paint_material_layer_channels.cc`
 * lives in `paint_material_layer_edit_intern.hh`.
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

bool BKE_paint_material_layer_mask_add(Main &bmain,
                                       Material &ma,
                                       const int ordinal,
                                       const float initial_color[4],
                                       const int image_size,
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
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::MaskAdd, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * mask is known to be added; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::MaskAdd, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  const int layer_index = plan.layer_index;
  /* The mask node goes in the tree the layer lives in, which is the group's for a nested row. */
  bNodeTree &tree = *plan.chains.first()->tree;

  /* One mask for the layer, shared by every channel: a layer is one thing, and a mask that
   * differed per channel would be several. */
  char mask_name[MAX_ID_NAME - 2];
  const char *layer_label = plan.chains.first()->layers[layer_index].node->label;
  SNPRINTF_UTF8(mask_name, "%s Mask", (layer_label[0] != 0) ? layer_label : "Layer");
  Image *mask = BKE_image_add_generated(&bmain,
                                        image_size,
                                        image_size,
                                        mask_name,
                                        32,
                                        false,
                                        IMA_GENTYPE_BLANK,
                                        initial_color,
                                        false,
                                        true,
                                        false);
  if (mask == nullptr) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  mask->flag |= IMA_PAINT_CANVAS;
  mask->paint_layer_id = BKE_paint_material_layer_marker_get(
      *plan.chains.first()->layers[layer_index].node);
  mask->paint_layer_channel = PAINT_LAYER_MAP_MASK;

  bNode *tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  bNodeSocket *color = bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr);
  if (color == nullptr) {
    bke::node_remove_node(&bmain, tree, *tex, false);
    BKE_id_free(&bmain, mask);
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  tex->id = &mask->id;
  tree.ensure_topology_cache();

  for (ChannelChain *chain : plan.chains) {
    ChainLayer &layer = chain->layers[layer_index];
    /* A previous channel's relink, below, invalidates the topology cache
     * #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
    chain->tree->ensure_topology_cache();
    CompositeMixNode mix;
    if (!composite_mix_node_read(*layer.node, mix) || mix.factor == nullptr) {
      continue;
    }
    if (!layer.mask_corrections.is_empty()) {
      /* The mask becomes the base of the mask-correction chain (spec 18 §4.3): it feeds what the
       * lowest mask correction blends over, exactly where the coverage the row had without the
       * mask used to sit, so the mask defines the coverage the corrections then shape. A channel
       * whose chain base is unlinked keeps the explicit-zero form (I1) -- the mask is picked up
       * when the channel gets coverage. */
      const ChainCorrection &lowest = layer.mask_corrections.first();
      CompositeMixNode lowest_mix;
      if (lowest.mix == nullptr || !composite_mix_node_read(*lowest.mix, lowest_mix) ||
          lowest_mix.bottom == nullptr)
      {
        continue;
      }
      bNodeSocket &bottom = *const_cast<bNodeSocket *>(lowest_mix.bottom);
      if (!socket_has_link(bottom)) {
        continue;
      }
      relink_into(*chain->tree, bottom, *lowest.mix, *tex, *color);
      continue;
    }
    if (mix.factor_opacity != nullptr) {
      if (composite_mix_coverage_off(mix)) {
        /* Absent or Disabled here: the mask must not become this channel's coverage (I1). It is
         * picked up when the channel is switched on. */
        continue;
      }
      /* Coverage and the layer's own opacity already coexist: swap only what feeds the coverage
       * side, so the opacity the user may already have set survives adding a mask. */
      bNodeSocket &coverage = const_cast<bNodeSocket &>(*mix.factor_coverage);
      bNode &multiply = const_cast<bNode &>(coverage.owner_node());
      relink_into(*chain->tree, coverage, multiply, *tex, *color);
    }
    else {
      /* Either a bare constant -- carried over as the new Multiply's opacity -- or an old-style
       * mask with nothing to carry over; either way the layer keeps an editable opacity from here
       * on, wrapped fresh around the new mask. */
      float initial_opacity = 1.0f;
      if (BKE_paint_material_source_socket(*mix.factor) == nullptr) {
        initial_opacity =
            static_cast<const bNodeSocketValueFloat *>(mix.factor->default_value)->value;
      }
      layer_factor_coverage_link(*chain->tree,
                                 *layer.node,
                                 *const_cast<bNodeSocket *>(mix.factor),
                                 *tex,
                                 *color,
                                 initial_opacity);
    }
  }
  ChainLayer &masked = plan.chains.first()->layers[layer_index];
  bke::node_position_relative(*tex, *masked.node, color, *masked.top);

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_mask_remove(Main &bmain,
                                          Material &ma,
                                          const int ordinal,
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
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::MaskRemove, plan, error)) {
    return fail(error);
  }
  /* 2. Shape: a bottom that is still a bare image is wrapped in a Mix node first, now that the
   * mask is known to be removed; the plan is read again afterwards. */
  if (plan.needs_bottom_normalize) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
      if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::MaskRemove, plan, error)) {
        BLI_assert_unreachable();
        return fail(error);
      }
    }
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  const int layer_index = plan.layer_index;
  bNodeTree &tree = *plan.chains.first()->tree;

  Vector<std::pair<bNodeTree *, bNode *>> mask_nodes;
  /* The masks whose nodes go below, for the orphan check at the end: a generated blank nothing
   * reads anymore must not keep answering as the row's mask to the model and the paint canvas. */
  Vector<Image *> mask_images;
  for (ChannelChain *chain_ptr : plan.chains) {
    ChannelChain &chain = *chain_ptr;
    ChainLayer &layer = chain.layers[layer_index];
    /* A previous channel's relink, below, invalidates the topology cache
     * #composite_mix_node_read now reads (to detect the opacity/coverage Multiply). */
    chain.tree->ensure_topology_cache();
    CompositeMixNode mix;
    if (!composite_mix_node_read(*layer.node, mix) || mix.factor == nullptr) {
      continue;
    }
    if (!layer.mask_corrections.is_empty()) {
      /* The mask corrections stay; what goes is the mask image at the chain's base, and what
       * comes back is the coverage the row had without the mask -- the content corrections'
       * accumulated coverage, or the row's own map's alpha. */
      const ChainCorrection &lowest = layer.mask_corrections.first();
      CompositeMixNode lowest_mix;
      if (lowest.mix == nullptr || !composite_mix_node_read(*lowest.mix, lowest_mix) ||
          lowest_mix.bottom == nullptr)
      {
        continue;
      }
      bNodeSocket &bottom = *const_cast<bNodeSocket *>(lowest_mix.bottom);
      bNodeLink *mask_link = sole_link_into(bottom);
      if (mask_link == nullptr) {
        /* Nothing feeds the chain's base here -- the Absent form: no mask in this channel. */
        continue;
      }
      bNode *mask_node = mask_link->fromnode;
      if (mask_node->type_legacy != SH_NODE_TEX_IMAGE || mask_node->id == nullptr ||
          GS(mask_node->id->name) != ID_IM)
      {
        continue;
      }
      Image &mask_image = *id_cast<Image *>(mask_node->id);
      /* The row's own mask is the tagged one; the map's alpha (or a correction's over output) at
         the base is the no-mask shape, not a mask to remove. */
      if (mask_image.paint_layer_channel != PAINT_LAYER_MAP_MASK ||
          !BLI_uuid_equal(mask_image.paint_layer_id,
                          BKE_paint_material_layer_marker_get(*layer.node)))
      {
        continue;
      }
      bNode *base_node = nullptr;
      bNodeSocket *base_socket = nullptr;
      if (!layer.content_corrections.is_empty()) {
        const ChainCorrection &top = layer.content_corrections.last();
        if (top.over_combine != nullptr) {
          base_node = top.over_combine;
          base_socket = static_cast<bNodeSocket *>(base_node->outputs.first);
        }
      }
      if (base_node == nullptr && layer.base_map != nullptr) {
        base_node = layer.base_map;
        base_socket = bke::node_find_socket(*base_node, SOCK_OUT, "Alpha"_ustr);
      }
      /* Counted before the relink: the Color output's remaining links say whether the node is
         the last reader's, the same rule the plain path below follows. */
      if (mask_link->fromsock->directly_linked_links().size() == 1) {
        mask_nodes.append_non_duplicates({chain.tree, mask_node});
        mask_images.append_non_duplicates(&mask_image);
      }
      if (base_node != nullptr && base_socket != nullptr) {
        relink_into(*chain.tree, bottom, *lowest.mix, *base_node, *base_socket);
      }
      else {
        for (bNodeLink *link : Vector<bNodeLink *>(bottom.directly_linked_links())) {
          BKE_ntree_update_tag_link_removed(chain.tree);
          bke::node_remove_link(chain.tree, *link);
        }
      }
      continue;
    }
    /* Coverage and opacity coexist: what is being removed lives on the Multiply's coverage
     * input, and its opacity constant is left exactly as it was. An old-style layer with nothing
     * to separate an opacity from is masked straight on #factor instead, same as always. */
    bNodeSocket *coverage_socket = (mix.factor_opacity != nullptr) ?
                                       const_cast<bNodeSocket *>(mix.factor_coverage) :
                                       const_cast<bNodeSocket *>(mix.factor);
    bNode &coverage_owner = const_cast<bNode &>(coverage_socket->owner_node());
    bNodeLink *mask_link = sole_link_into(*coverage_socket);
    if (mask_link == nullptr) {
      continue;
    }
    /* Coverage goes back to the layer's own map, which is where it comes from for a layer that
     * never had a mask. Resolved before deciding what "the mask" even is: a layer whose coverage
     * already comes straight from its own map -- the default shape now, not just the masked one --
     * has no mask to remove, and mistaking its own Image Texture node for one would delete it. */
    bNodeLink *map_link = (layer.top == nullptr) ? nullptr : sole_link_into(*layer.top);
    if (map_link != nullptr && mask_link->fromnode == map_link->fromnode) {
      continue;
    }
    if (mask_link->fromsock->directly_linked_links().size() == 1) {
      mask_nodes.append_non_duplicates({chain.tree, mask_link->fromnode});
    }
    bNodeSocket *alpha = (map_link == nullptr) ?
                             nullptr :
                             bke::node_find_socket(*map_link->fromnode, SOCK_OUT, "Alpha"_ustr);
    if (alpha == nullptr) {
      /* Nothing to restore coverage from; leaving it unlinked is still a layer without a mask. */
      for (bNodeLink *link : Vector<bNodeLink *>(coverage_socket->directly_linked_links())) {
        BKE_ntree_update_tag_link_removed(chain.tree);
        bke::node_remove_link(chain.tree, *link);
      }
      continue;
    }
    /* The map's node is taken from the link rather than from the socket: relinking the previous
     * channel already invalidated the topology cache #owner_node asserts on. */
    if (mix.factor_opacity != nullptr) {
      relink_into(*chain.tree, *coverage_socket, coverage_owner, *map_link->fromnode, *alpha);
    }
    else {
      /* No opacity to preserve here -- wrap the restored coverage in a fresh Multiply so the
       * layer keeps an editable one going forward. */
      layer_factor_coverage_link(
          *chain.tree, *layer.node, *coverage_socket, *map_link->fromnode, *alpha, 1.0f);
    }
  }

  Set<bNodeTree *> touched_trees;
  for (const std::pair<bNodeTree *, bNode *> &entry : mask_nodes) {
    bke::node_remove_node(&bmain, *entry.first, *entry.second, true);
    BKE_ntree_update_tag_node_removed(entry.first);
    touched_trees.add(entry.first);
  }
  for (Image *image : mask_images) {
    /* The node removal gave the mask's user back; a generated blank nothing reads anymore is
     * freed the way every other exit in this module disposes of its own orphans -- left tagged
     * and user-less it would still reach the stack model's maps-by-tag pass. */
    if (image->id.us == 0 && image->source == IMA_SRC_GENERATED) {
      BKE_id_free(&bmain, image);
    }
  }
  mask_images.clear();
  for (bNodeTree *touched : touched_trees) {
    if (touched != ma.nodetree) {
      BKE_ntree_update_after_single_tree_change(bmain, *touched);
    }
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

bool BKE_paint_material_layer_stack_contains_mask(Main &bmain, Material &ma, const Image &image)
{
  Vector<PaintMaterialLayerStackEntry> entries;
  if (!BKE_paint_material_layer_stack_from_material(bmain, ma, entries)) {
    return false;
  }
  for (const PaintMaterialLayerStackEntry &entry : entries) {
    if (entry.channel_images.lookup_default(PAINT_LAYER_MAP_MASK, nullptr) == &image) {
      return true;
    }
  }
  return false;
}

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
