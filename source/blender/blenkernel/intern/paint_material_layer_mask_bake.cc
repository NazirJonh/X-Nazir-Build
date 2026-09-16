/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * See `paint_material_layer_mask_bake_intern.hh`.
 */

#include "paint_material_layer_mask_bake_intern.hh"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_image_partial_update.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_layer_model.hh"
#include "BKE_paint_material_mask_bake.hh"

#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_ustring.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "paint_material_composite_internal.hh"
#include "paint_material_layer_edit_intern.hh"
#include "paint_material_layer_idprops.hh"
#include "paint_material_layer_mask_intern.hh"

#include <memory>

namespace blender {

/* The image change log the bake cache subscribes to. Brought in wholesale because the switch over
 * #ePartialUpdateCollectResult reads badly with the full qualification on every label. */
using namespace bke::image::partial_update;

/** Whether \a node carries the anchor id-property. */
static bool anchor_node_is(const bNode &node)
{
  if (node.prop == nullptr) {
    return false;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(node.prop, MASK_BAKE_ANCHOR_PROP, IDP_INT);
  return prop != nullptr && IDP_int_get(prop) != 0;
}

/** The channel stamped on the anchor \a node, or -1 when it carries none. */
static int anchor_channel_get(const bNode &node)
{
  if (node.prop == nullptr) {
    return -1;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(node.prop, MASK_BAKE_CHANNEL_PROP, IDP_INT);
  return (prop != nullptr) ? IDP_int_get(prop) : -1;
}

static void anchor_props_set(bNode &node, const int channel)
{
  IDProperty *props = bke::paint_layer::node_properties_ensure(node);
  IDProperty *flag = IDP_GetPropertyTypeFromGroup(props, MASK_BAKE_ANCHOR_PROP, IDP_INT);
  if (flag != nullptr) {
    IDP_int_set(flag, 1);
  }
  else {
    IDP_AddToGroup(props, IDP_NewInt(1, MASK_BAKE_ANCHOR_PROP));
  }
  IDProperty *channel_prop = IDP_GetPropertyTypeFromGroup(props, MASK_BAKE_CHANNEL_PROP, IDP_INT);
  if (channel_prop != nullptr) {
    IDP_int_set(channel_prop, channel);
  }
  else {
    IDP_AddToGroup(props, IDP_NewInt(channel, MASK_BAKE_CHANNEL_PROP));
  }
}

/** The image node the anchor references B by, or null when its second input is not wired to one. */
static bNode *anchor_image_node_resolve(const bNode &anchor)
{
  const bNodeSocket *bake_input = bke::node_find_socket(anchor, SOCK_IN, "B_Float"_ustr);
  if (bake_input == nullptr) {
    return nullptr;
  }
  const Span<const bNodeLink *> links = bake_input->directly_linked_links();
  if (links.size() != 1 || !links[0]->is_available()) {
    return nullptr;
  }
  bNode *from = links[0]->fromnode;
  if (from->type_legacy != SH_NODE_TEX_IMAGE || from->id == nullptr ||
      GS(from->id->name) != ID_IM)
  {
    return nullptr;
  }
  return from;
}

/**
 * Fill \a r_anchor from anchor \a node, whose B reference is \a image_node (already resolved).
 *
 * B is the anchor's own reference to its baked image, read off its second input. A reference whose
 * image is not a baked mask, or a missing one, leaves both image fields null; the anchor is still
 * filled. The live chain parks on the first input that carries a link and is not the B reference.
 * When the anchor holds no chain -- a bake of a coverage that had none -- fall back to the socket
 * #mask_bake_anchor_create would have used, so a reader still has a socket to walk.
 */
static void anchor_fill(const bNode &node, bNode *image_node, MaskBakeAnchor &r_anchor)
{
  Image *image = nullptr;
  if (image_node != nullptr) {
    Image *candidate = id_cast<Image *>(image_node->id);
    if (candidate->paint_layer_channel == PAINT_LAYER_MAP_MASK_BAKED) {
      image = candidate;
    }
    else {
      image_node = nullptr;
    }
  }
  bNodeSocket *live_input = nullptr;
  for (const bNodeSocket &input : node.inputs) {
    const Span<const bNodeLink *> links = input.directly_linked_links();
    if (links.size() != 1 || !links[0]->is_available()) {
      continue;
    }
    if (image_node != nullptr && links[0]->fromnode == image_node) {
      continue;
    }
    live_input = const_cast<bNodeSocket *>(&input);
    break;
  }
  if (live_input == nullptr) {
    live_input = const_cast<bNodeSocket *>(
        bke::node_find_socket(node, SOCK_IN, "A_Float"_ustr));
  }
  r_anchor.node = const_cast<bNode *>(&node);
  r_anchor.live_input = live_input;
  r_anchor.image = image;
  r_anchor.image_node = image_node;
}

bool mask_bake_anchor_read(const bNodeTree &tree,
                           const bUUID &row_marker,
                           const int channel,
                           MaskBakeAnchor &r_anchor)
{
  tree.ensure_topology_cache();
  for (const bNode &node : tree.nodes) {
    if (!anchor_node_is(node) || anchor_channel_get(node) != channel ||
        !BLI_uuid_equal(bke::paint_layer::marker_get(node), row_marker))
    {
      continue;
    }
    anchor_fill(node, anchor_image_node_resolve(node), r_anchor);
    return true;
  }
  return false;
}

bool mask_bake_anchor_create(Main &bmain,
                             bNodeTree &tree,
                             const bNodeSocket &coverage_socket,
                             const bUUID &row_marker,
                             const int channel,
                             const int width,
                             const int height,
                             MaskBakeAnchor &r_anchor)
{
  MASK_BAKE_TRACE("anchor_create enter tree=%p channel=%d coverage=%p\n",
                  static_cast<void *>(&tree),
                  channel,
                  static_cast<void *>(const_cast<bNodeSocket *>(&coverage_socket)));
  if (mask_bake_anchor_read(tree, row_marker, channel, r_anchor)) {
    if (r_anchor.image_node == nullptr) {
      MASK_BAKE_TRACE("anchor_create reuse: image_node null -> false\n");
      return false;
    }
    MASK_BAKE_TRACE("anchor_create reuse anchor=%p image=%p\n",
                    static_cast<void *>(r_anchor.node),
                    static_cast<void *>(r_anchor.image));
    /* The row's map can be resized under an existing bake (a channel resize scales the maps), and
     * B has to follow it: a size mismatch makes every later eval fail and B would stop updating. */
    if (r_anchor.image != nullptr && width > 0 && height > 0) {
      int current_width = 0;
      int current_height = 0;
      BKE_image_get_size(r_anchor.image, nullptr, &current_width, &current_height);
      if (current_width != width || current_height != height) {
        BKE_image_scale(r_anchor.image, width, height, nullptr);
        BKE_image_partial_update_mark_full_update(r_anchor.image);
        BKE_image_free_gputextures(r_anchor.image);
      }
    }
    /* Idempotent ensure: coverage may have been rewired since, or never wired to B at all. */
    bNodeSocket &coverage = const_cast<bNodeSocket &>(coverage_socket);
    bNodeLink *coverage_link = sole_link_into(coverage);
    if (coverage_link == nullptr || coverage_link->fromnode != r_anchor.image_node) {
      bNode &coverage_owner = const_cast<bNode &>(coverage.owner_node());
      for (bNodeLink *link : Vector<bNodeLink *>(coverage.directly_linked_links())) {
        BKE_ntree_update_tag_link_removed(&tree);
        bke::node_remove_link(&tree, *link);
      }
      bNodeSocket *color = bke::node_find_socket(*r_anchor.image_node, SOCK_OUT, "Color"_ustr);
      if (color == nullptr) {
        return false;
      }
      bke::node_add_link(tree, *r_anchor.image_node, *color, coverage_owner, coverage);
    }
    return true;
  }

  /* The top of the live chain feeding coverage is what the anchor takes over. Resolved before
   * anything is created; the link is only broken once every node exists, so a creation failure
   * leaves coverage as it was. */
  bNodeLink *old_link = sole_link_into(const_cast<bNodeSocket &>(coverage_socket));
  if (old_link == nullptr && socket_has_link(const_cast<bNodeSocket &>(coverage_socket))) {
    /* More than one link: the sole-link shape every mutator here assumes, refused rather than
     * buried under another. */
    return false;
  }
  bNode *old_from = (old_link != nullptr) ? old_link->fromnode : nullptr;
  bNodeSocket *old_socket = (old_link != nullptr) ? old_link->fromsock : nullptr;

  /* The anchor is a Mix node only because a node has to exist to park the live link on; its output
   * is never read, which is what makes the GPU prune it and the chain feeding it. A float Mix
   * takes the coverages it is used for without a conversion node of its own. */
  bNode *anchor = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX);
  if (anchor == nullptr) {
    return false;
  }
  NodeShaderMix *storage = static_cast<NodeShaderMix *>(anchor->storage);
  storage->data_type = SOCK_FLOAT;
  storage->factor_mode = NODE_MIX_MODE_UNIFORM;
  storage->blend_type = MA_RAMP_BLEND;

  bNodeSocket *live_input = bke::node_find_socket(*anchor, SOCK_IN, "A_Float"_ustr);
  bNodeSocket *bake_input = bke::node_find_socket(*anchor, SOCK_IN, "B_Float"_ustr);
  if (live_input == nullptr || bake_input == nullptr) {
    bke::node_remove_node(&bmain, tree, *anchor, true);
    return false;
  }
  bke::paint_layer::marker_set(*anchor, row_marker);
  anchor_props_set(*anchor, channel);

  const float transparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  Image *image = BKE_image_add_generated(&bmain,
                                         width,
                                         height,
                                         "Baked Mask",
                                         32,
                                         false,
                                         IMA_GENTYPE_BLANK,
                                         transparent,
                                         false,
                                         true,
                                         false);
  if (image == nullptr) {
    bke::node_remove_node(&bmain, tree, *anchor, true);
    return false;
  }
  /* The same tags a layer's mask carries, plus the internal bake role that keeps it out of every
   * public map list; the row marker is what a maps-by-tag reader keys it by. */
  image->flag |= IMA_PAINT_CANVAS;
  image->paint_layer_id = row_marker;
  image->paint_layer_channel = PAINT_LAYER_MAP_MASK_BAKED;

  bNode *image_node = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
  bNodeSocket *color = (image_node != nullptr) ?
                           bke::node_find_socket(*image_node, SOCK_OUT, "Color"_ustr) :
                           nullptr;
  if (image_node == nullptr || color == nullptr) {
    if (image_node != nullptr) {
      bke::node_remove_node(&bmain, tree, *image_node, true);
    }
    BKE_id_free(&bmain, image);
    bke::node_remove_node(&bmain, tree, *anchor, true);
    return false;
  }
  image_node->id = &image->id;

  /* Every node exists now: break the old link and move its source onto the anchor, then point
   * coverage at B instead. */
  if (old_link != nullptr) {
    BKE_ntree_update_tag_link_removed(&tree);
    bke::node_remove_link(&tree, *old_link);
    bke::node_add_link(tree, *old_from, *old_socket, *anchor, *live_input);
  }

  tree.ensure_topology_cache();
  bNode &coverage_owner = const_cast<bNode &>(coverage_socket.owner_node());
  MASK_BAKE_TRACE("anchor_create fresh: adding B->coverage and B->anchor links\n");
  bke::node_add_link(
      tree, *image_node, *color, coverage_owner, const_cast<bNodeSocket &>(coverage_socket));
  bke::node_add_link(tree, *image_node, *color, *anchor, *bake_input);
  MASK_BAKE_TRACE("anchor_create fresh done anchor=%p image=%p\n",
                  static_cast<void *>(anchor),
                  static_cast<void *>(image));

  r_anchor.node = anchor;
  r_anchor.live_input = live_input;
  r_anchor.image = image;
  r_anchor.image_node = image_node;
  return true;
}

void mask_bake_anchor_remove(Main &bmain,
                             bNodeTree &tree,
                             const bUUID &row_marker,
                             const int channel)
{
  MaskBakeAnchor anchor;
  if (!mask_bake_anchor_read(tree, row_marker, channel, anchor)) {
    MASK_BAKE_TRACE("anchor_remove: no anchor tree=%p channel=%d\n",
                    static_cast<void *>(&tree),
                    channel);
    return;
  }
  MASK_BAKE_TRACE("anchor_remove enter anchor=%p image_node=%p live_input=%p\n",
                  static_cast<void *>(anchor.node),
                  static_cast<void *>(anchor.image_node),
                  static_cast<void *>(anchor.live_input));
  Image *image = anchor.image;

  if (anchor.live_input != nullptr) {
    for (bNodeLink *link : Vector<bNodeLink *>(anchor.live_input->directly_linked_links())) {
      BKE_ntree_update_tag_link_removed(&tree);
      bke::node_remove_link(&tree, *link);
    }
  }
  /* The links are queried again rather than carried over: the loop above may already have removed
   * this very link when #live_input aliased the B input. The cache is refreshed first, since the
   * removals above changed the tree and `directly_linked_links` reads the cached links. */
  if (anchor.node != nullptr) {
    tree.ensure_topology_cache();
    if (bNodeSocket *bake_input = bke::node_find_socket(*anchor.node, SOCK_IN, "B_Float"_ustr)) {
      for (bNodeLink *link : Vector<bNodeLink *>(bake_input->directly_linked_links())) {
        BKE_ntree_update_tag_link_removed(&tree);
        bke::node_remove_link(&tree, *link);
      }
    }
  }

  if (anchor.image_node != nullptr) {
    MASK_BAKE_TRACE("anchor_remove: removing image_node\n");
    bke::node_remove_node(&bmain, tree, *anchor.image_node, true);
    BKE_ntree_update_tag_node_removed(&tree);
  }
  if (anchor.node != nullptr) {
    MASK_BAKE_TRACE("anchor_remove: removing anchor node\n");
    bke::node_remove_node(&bmain, tree, *anchor.node, true);
    BKE_ntree_update_tag_node_removed(&tree);
  }

  if (image != nullptr && image->id.us == 0 && image->source == IMA_SRC_GENERATED) {
    MASK_BAKE_TRACE("anchor_remove: freeing B image %p\n", static_cast<void *>(image));
    BKE_id_free(&bmain, image);
  }
  MASK_BAKE_TRACE("anchor_remove done\n");
}

void mask_bake_unbake_row(Main &bmain, ChainLayer &layer, const int channel)
{
  if (layer.node == nullptr) {
    return;
  }
  bNodeTree &tree = layer.node->owner_tree();
  const bUUID row_marker = bke::paint_layer::marker_get(*layer.node);
  MASK_BAKE_TRACE("unbake_row enter tree=%p channel=%d node=%p\n",
                  static_cast<void *>(&tree),
                  channel,
                  static_cast<void *>(layer.node));
  MaskBakeAnchor anchor;
  if (!mask_bake_anchor_read(tree, row_marker, channel, anchor)) {
    MASK_BAKE_TRACE("unbake_row: no anchor\n");
    return;
  }

  /* The live chain top is captured before the removal drops the link: a bake of a coverage that
   * had no chain parks nothing, and coverage is then left exactly as the removal found it. */
  bNode *live_node = nullptr;
  bNodeSocket *live_socket = nullptr;
  if (anchor.live_input != nullptr) {
    tree.ensure_topology_cache();
    if (bNodeLink *live_link = sole_link_into(*anchor.live_input)) {
      live_node = live_link->fromnode;
      live_socket = live_link->fromsock;
    }
  }
  MASK_BAKE_TRACE("unbake_row: live_node=%p live_socket=%p\n",
                  static_cast<void *>(live_node),
                  static_cast<void *>(live_socket));
  mask_bake_anchor_remove(bmain, tree, row_marker, channel);
  if (live_node == nullptr || live_socket == nullptr) {
    return;
  }

  /* The anchor took over the row's coverage input; with its B link gone the live chain hangs on
   * nothing, so it is pointed back at the coverage it fed before the bake. */
  tree.ensure_topology_cache();
  CompositeMixNode mix;
  if (!composite_mix_node_read(*layer.node, mix)) {
    MASK_BAKE_TRACE("unbake_row: row mix unreadable\n");
    return;
  }
  bNodeSocket *coverage = const_cast<bNodeSocket *>(
      (mix.factor_coverage != nullptr) ? mix.factor_coverage : mix.factor);
  if (coverage == nullptr) {
    MASK_BAKE_TRACE("unbake_row: coverage null\n");
    return;
  }
  MASK_BAKE_TRACE("unbake_row: relink live -> coverage\n");
  relink_into(tree, *coverage, coverage->owner_node(), *live_node, *live_socket);
  MASK_BAKE_TRACE("unbake_row done\n");
}

void mask_bake_size_get(bNodeTree &tree,
                        const ChainLayer &layer,
                        const bUUID &row_marker,
                        int &r_width,
                        int &r_height)
{
  Image *image = nullptr;
  if (bNode *mask_node = layer_mask_node_find(tree, row_marker)) {
    if (mask_node->id != nullptr && GS(mask_node->id->name) == ID_IM) {
      image = id_cast<Image *>(mask_node->id);
    }
  }
  if (image == nullptr) {
    image = layer.image;
  }
  if (image == nullptr && layer.base_map != nullptr && layer.base_map->id != nullptr &&
      GS(layer.base_map->id->name) == ID_IM)
  {
    image = id_cast<Image *>(layer.base_map->id);
  }
  if (image != nullptr) {
    BKE_image_get_size(image, nullptr, &r_width, &r_height);
    return;
  }
  r_width = 1024;
  r_height = 1024;
}

bool mask_bake_coverage_is_baked(const bNodeSocket &coverage_socket,
                                 Image *&r_image,
                                 bNode *&r_image_node)
{
  const Span<const bNodeLink *> links = coverage_socket.directly_linked_links();
  if (links.size() != 1 || !links[0]->is_available()) {
    return false;
  }
  const bNode *from = links[0]->fromnode;
  if (from->type_legacy != SH_NODE_TEX_IMAGE || from->id == nullptr ||
      GS(from->id->name) != ID_IM)
  {
    return false;
  }
  Image *image = id_cast<Image *>(from->id);
  if (image->paint_layer_channel != PAINT_LAYER_MAP_MASK_BAKED) {
    return false;
  }
  r_image = image;
  r_image_node = const_cast<bNode *>(from);
  return true;
}

const bNodeSocket *mask_bake_live_top_socket(const bNodeSocket &coverage_socket)
{
  MaskBakeAnchor anchor;
  if (!mask_bake_anchor_from_coverage(coverage_socket, anchor)) {
    return &coverage_socket;
  }
  return (anchor.live_input != nullptr) ? anchor.live_input : &coverage_socket;
}

bNodeSocket *paint_layer_live_coverage_socket(ChainLayer &layer)
{
  if (layer.node == nullptr) {
    return nullptr;
  }
  /* #composite_mix_node_read walks the Factor's links, so the node's tree cache has to be current
   * before it runs. */
  layer.node->owner_tree().ensure_topology_cache();
  CompositeMixNode mix;
  if (!composite_mix_node_read(*layer.node, mix)) {
    return nullptr;
  }
  /* The row's coverage input, the same one #layer_mask_base_socket reads: the Multiply's coverage
   * side when the opacity/coverage shape is present, the Factor itself for a legacy row. */
  const bNodeSocket *coverage = (mix.factor_coverage != nullptr) ? mix.factor_coverage : mix.factor;
  if (coverage == nullptr) {
    return nullptr;
  }
  return const_cast<bNodeSocket *>(mask_bake_live_top_socket(*coverage));
}

bool mask_bake_anchor_from_coverage(const bNodeSocket &coverage_socket, MaskBakeAnchor &r_anchor)
{
  /* The anchor is found by the B texture it references, not by the row marker or channel: a reader
   * walking a channel has the coverage socket alone, and the B node it reads names the anchor. */
  const bNodeTree &tree = coverage_socket.owner_tree();
  tree.ensure_topology_cache();
  Image *image = nullptr;
  bNode *image_node = nullptr;
  if (!mask_bake_coverage_is_baked(coverage_socket, image, image_node)) {
    return false;
  }
  for (const bNode &node : tree.nodes) {
    if (!anchor_node_is(node)) {
      continue;
    }
    if (anchor_image_node_resolve(node) != image_node) {
      continue;
    }
    anchor_fill(node, image_node, r_anchor);
    return true;
  }
  return false;
}

/**
 * Acquire \a image's buffer for writing, ensuring it is byte RGBA at the image's own size.
 *
 * B is created byte, so this is normally only a check; the reallocation guards an image whose
 * buffer was replaced under the anchor, mirroring what the render bake does for its own target --
 * the write below needs a host byte buffer, and a missing one is cheaper to recreate than to
 * special-case. The caller owns the returned reference and releases it with #BKE_image_release_ibuf
 * through \a r_lock.
 */
static ImBuf *mask_bake_acquire_byte_rgba(Image *image, void **r_lock)
{
  *r_lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(image, nullptr, r_lock);
  if (ibuf != nullptr && ibuf->byte_buffer.data != nullptr && ELEM(ibuf->channels, 0, 4)) {
    return ibuf;
  }
  if (ibuf != nullptr) {
    BKE_image_release_ibuf(image, ibuf, *r_lock);
    *r_lock = nullptr;
  }

  int width = 0;
  int height = 0;
  BKE_image_get_size(image, nullptr, &width, &height);
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  ImBuf *fresh = IMB_allocImBuf(uint(width), uint(height), ImBufFlags::ByteData);
  if (fresh == nullptr) {
    return nullptr;
  }
  fresh->channels = 4;
  BKE_image_replace_imbuf(image, fresh);
  /* #BKE_image_replace_imbuf references the buffer; this call's own reference is still ours. */
  IMB_freeImBuf(fresh);
  return BKE_image_acquire_ibuf(image, nullptr, r_lock);
}

/* -------------------------------------------------------------------- */
/** \name Bake Cache
 *
 * Keyed by the material's #ID.session_uid, the row's marker and the channel, for the same reason
 * the composite cache is: a freed material hands its address to the next one, so a pointer-keyed
 * cache would serve the old bake for the new material. The marker and channel are what make an
 * entry survive the anchor being rebuilt with a fresh B image across an undo step.
 *
 * Main thread only. Everything that reaches this -- an edit's commit, a redraw's ensure -- runs
 * there, and unlike the render bake there is no worker writing results back.
 * \{ */

/**
 * Owning handle for one partial-update subscription.
 *
 * By value rather than as a raw pointer freed by hand, because an entry is dropped from four
 * places -- eviction, a per-material drop, a stale dependency and the teardown -- and every one of
 * them would otherwise have to remember to free it.
 */
struct MaskBakePartialUpdateUserDeleter {
  void operator()(PartialUpdateUser *user) const
  {
    BKE_image_partial_update_free(user);
  }
};
using MaskBakePartialUpdateUserPtr =
    std::unique_ptr<PartialUpdateUser, MaskBakePartialUpdateUserDeleter>;

/** Hash \a uuid into \a hash, field by field; handles need a stable value, not the struct. */
static uint64_t mask_bake_uuid_hash(uint64_t hash, const bUUID &uuid)
{
  uint64_t node_bytes = 0;
  for (const int i : IndexRange(6)) {
    node_bytes |= uint64_t(static_cast<uint8_t>(uuid.node[i])) << (8 * (5 - i));
  }
  return get_default_hash(hash,
                          uuid.time_low,
                          uint64_t(uuid.time_mid) << 16 | uuid.time_hi_and_version,
                          uint64_t(uuid.clock_seq_hi_and_reserved) << 8 | uuid.clock_seq_low,
                          node_bytes);
}

/** Extend \a hash with everything about one correction that changes the row's mask factor. */
static uint64_t mask_bake_correction_structural_hash(
    uint64_t hash, const PaintMaterialCompositeCorrection &correction)
{
  hash = mask_bake_uuid_hash(hash, correction.marker);
  return get_default_hash(hash,
                          correction.image != nullptr ? correction.image->id.session_uid : 0,
                          int(correction.blend),
                          correction.enabled,
                          correction.row_enabled,
                          correction.opacity);
}

/**
 * Hash of everything about the row's layer that changes the mask factor except the pixels
 * themselves.
 *
 * Image *contents* are deliberately absent: there is no content version to hash, and an edit to
 * them is found instead by polling each source image's partial-update log. Session UIDs stand in
 * for the images, since a freed image's address can come back as a different one.
 */
static uint64_t mask_bake_layer_structural_hash(const PaintMaterialCompositeImageLayer &layer)
{
  /* Split rather than appended: #get_default_hash mixes a fixed number of values at once. */
  uint64_t hash = get_default_hash(
      layer.color_image != nullptr ? layer.color_image->id.session_uid : 0,
      layer.mask_image != nullptr ? layer.mask_image->id.session_uid : 0,
      int(layer.blend),
      layer.enabled,
      layer.mask_from_alpha);
  hash = get_default_hash(hash, layer.opacity, layer.mask_influence, layer.is_bare_base);
  hash = mask_bake_uuid_hash(hash, layer.marker);
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    hash = mask_bake_correction_structural_hash(hash, correction);
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.mask_corrections) {
    hash = mask_bake_correction_structural_hash(hash, correction);
  }
  return hash;
}

/**
 * The images the row's mask factor is read from: its base map, its mask, and the maps of both its
 * correction sections.
 *
 * Deduplicated, since a row that masks itself by its own map names the same image twice and one
 * subscription per image is all a poll can use. This is the same set the composite layer holds, so
 * a cache entry watches exactly what the row math reads.
 */
static Vector<Image *> mask_bake_layer_images(const PaintMaterialCompositeImageLayer &layer)
{
  Vector<Image *> images;
  if (layer.color_image != nullptr) {
    images.append_non_duplicates(layer.color_image);
  }
  if (layer.mask_image != nullptr) {
    images.append_non_duplicates(layer.mask_image);
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.content_corrections) {
    if (correction.image != nullptr) {
      images.append_non_duplicates(correction.image);
    }
  }
  for (const PaintMaterialCompositeCorrection &correction : layer.mask_corrections) {
    if (correction.image != nullptr) {
      images.append_non_duplicates(correction.image);
    }
  }
  return images;
}

struct MaskBakeCacheKey {
  uint32_t material_session_uid = 0;
  bUUID row_marker = {};
  int channel = 0;

  uint64_t hash() const
  {
    return get_default_hash(
        this->material_session_uid, this->channel, mask_bake_uuid_hash(0, this->row_marker));
  }

  friend bool operator==(const MaskBakeCacheKey &a, const MaskBakeCacheKey &b)
  {
    return a.material_session_uid == b.material_session_uid && a.channel == b.channel &&
           BLI_uuid_equal(a.row_marker, b.row_marker);
  }
};

struct MaskBakeCacheEntry {
  int width = 0;
  int height = 0;
  uint64_t structural_hash = 0;
  /** The whole B buffer has to be recomputed. Set on creation, on a resize, and on invalidation. */
  bool dirty_full = true;
  /** Bounding rectangle, in B texels, of the pixels tagged since the last evaluation. */
  rcti dirty_region = {0, 0, 0, 0};
  /**
   * One partial-update subscription per source image, keyed by #ID.session_uid.
   *
   * Never holds an #Image pointer: the images arrive with every ensure call, so a poll always has
   * a fresh one, and a cache outliving an ID it pointed at would be a crash rather than a stale
   * pixel. Undo can free an image out from under the cache; this is what makes that survivable.
   */
  Map<uint32_t, MaskBakePartialUpdateUserPtr> partial_update_users;
  uint64_t revision = 0;
  /** Monotonic counter used to evict the least recently used entry. */
  int64_t last_use = 0;
};

/** B is one byte RGBA buffer per row, so this is a handful of entries at most; the budget only
 * exists to bound a pathological case, not to be managed. */
static constexpr int64_t MASK_BAKE_CACHE_BUDGET_BYTES = 256 * 1024 * 1024;

struct MaskBakeCache {
  Map<MaskBakeCacheKey, MaskBakeCacheEntry> entries;
  /** Monotonic, and only ever compared: the source of #MaskBakeCacheEntry.last_use. */
  int64_t use_counter = 0;
  /** Never reset: a consumer compares revisions over time, across entries that come and go. */
  uint64_t revision_counter = 0;
};

static MaskBakeCache g_cache;

static int64_t mask_bake_entry_size_in_bytes(const MaskBakeCacheEntry &entry)
{
  return int64_t(entry.width) * entry.height * 4;
}

/** Evict least recently used entries until the cache fits the budget, never the one just made. */
static void mask_bake_cache_enforce_budget(const MaskBakeCacheKey &keep)
{
  int64_t total = 0;
  for (const MaskBakeCacheEntry &entry : g_cache.entries.values()) {
    total += mask_bake_entry_size_in_bytes(entry);
  }
  while (total > MASK_BAKE_CACHE_BUDGET_BYTES) {
    const MaskBakeCacheKey *oldest_key = nullptr;
    int64_t oldest_use = INT64_MAX;
    for (const auto item : g_cache.entries.items()) {
      if (item.key == keep) {
        continue;
      }
      if (item.value.last_use < oldest_use) {
        oldest_use = item.value.last_use;
        oldest_key = &item.key;
      }
    }
    if (oldest_key == nullptr) {
      break;
    }
    const MaskBakeCacheKey key = *oldest_key;
    total -= mask_bake_entry_size_in_bytes(g_cache.entries.lookup(key));
    g_cache.entries.remove(key);
  }
}

/**
 * Subscribe \a entry to every image in \a images, drop the subscriptions it no longer needs, and
 * fold whatever they report into its dirty state.
 *
 * Polled rather than reported: a caller that edits an image's pixels has nothing to tell this
 * cache, and -- the point -- a caller that tags the image ID for unrelated reasons can no longer
 * turn a known rectangle into "the whole canvas". Right after a resize this asks for the full
 * rebuild a resize needs anyway, because the fresh subscriptions land on #FullUpdateNeeded.
 *
 * \a entry's dimensions must be current: a change arrives in the image's texels, which for a
 * validated stack are B's own, and the clamped rectangles are in B texels too.
 */
static void mask_bake_entry_poll_dependencies(MaskBakeCacheEntry &entry, Span<Image *> images)
{
  Set<uint32_t> live;
  for (Image *image : images) {
    if (image == nullptr) {
      continue;
    }
    const uint32_t session_uid = image->id.session_uid;
    if (!live.add(session_uid)) {
      continue;
    }
    PartialUpdateUser *user =
        entry.partial_update_users
            .lookup_or_add_cb(
                session_uid,
                [&]() {
                  return MaskBakePartialUpdateUserPtr(
                      BKE_image_partial_update_create(image));
                })
            .get();

    switch (BKE_image_partial_update_collect_changes(image, user)) {
      case ePartialUpdateCollectResult::FullUpdateNeeded:
        /* A brand new subscription lands here too, which is right: nothing of this image has been
         * baked yet. */
        entry.dirty_full = true;
        break;
      case ePartialUpdateCollectResult::NoChangesDetected:
        break;
      case ePartialUpdateCollectResult::PartialChangesDetected: {
        PartialUpdateRegion change;
        while (BKE_image_partial_update_get_next_change(user, &change) ==
               ePartialUpdateIterResult::ChangeAvailable)
        {
          /* B is one buffer, so a change reported for any tile but the first would land at the
           * wrong place in it. Give up precision rather than write pixels somewhere they do not
           * belong. */
          if (change.tile_number != 1001) {
            entry.dirty_full = true;
            break;
          }
          rcti bounds;
          BLI_rcti_init(&bounds, 0, entry.width, 0, entry.height);
          rcti clipped;
          if (!BLI_rcti_isect(&change.region, &bounds, &clipped)) {
            continue;
          }
          if (BLI_rcti_is_empty(&entry.dirty_region)) {
            entry.dirty_region = clipped;
          }
          else {
            BLI_rcti_union(&entry.dirty_region, &clipped);
          }
        }
        break;
      }
    }
  }

  /* An image the row stopped reading stops being watched, or the entry keeps an allocation and a
   * poll per frame for something it no longer reads. */
  Vector<uint32_t> stale;
  for (const uint32_t session_uid : entry.partial_update_users.keys()) {
    if (!live.contains(session_uid)) {
      stale.append(session_uid);
    }
  }
  for (const uint32_t session_uid : stale) {
    entry.partial_update_users.remove(session_uid);
  }
}

/**
 * Drop the entries of \a material_session_uid that no longer have an anchor.
 *
 * A removed anchor can never be looked up again, so leaving its entry would keep a subscription
 * alive for a row that no longer exists. The live set is what the walk found this call, after every
 * tree has been visited.
 */
static void mask_bake_cache_drop_missing(const uint32_t material_session_uid,
                                         Span<MaskBakeCacheKey> live_keys)
{
  Vector<MaskBakeCacheKey> stale;
  for (const auto item : g_cache.entries.items()) {
    if (item.key.material_session_uid != material_session_uid) {
      continue;
    }
    if (!live_keys.contains(item.key)) {
      stale.append(item.key);
    }
  }
  for (const MaskBakeCacheKey &key : stale) {
    g_cache.entries.remove(key);
  }
}

/** \} */

bool BKE_paint_material_mask_bake_ensure(Main &bmain, Material &ma, const bool force_full)
{
  MASK_BAKE_TRACE("ensure enter ma=%p serial=%u force=%d\n",
                  static_cast<void *>(&ma),
                  ma.id.session_uid,
                  int(force_full));
  if (ma.nodetree == nullptr) {
    return false;
  }

  /* Every tree an anchor can sit in: the material's own, plus the group tree each model row lives
   * in. A folder child's row, and so its anchor, is inside the folder's tree. */
  Vector<bNodeTree *> trees;
  trees.append(ma.nodetree);
  Vector<PaintMaterialLayerStackEntry> entries;
  if (BKE_paint_material_layer_stack_from_material(bmain, ma, entries)) {
    for (const PaintMaterialLayerStackEntry &entry : entries) {
      if (entry.owner_tree != nullptr) {
        trees.append_non_duplicates(const_cast<bNodeTree *>(entry.owner_tree));
      }
      if (entry.group_tree != nullptr) {
        trees.append_non_duplicates(const_cast<bNodeTree *>(entry.group_tree));
      }
    }
  }

  bool any_anchor = false;
  bool any_refreshed = false;
  bool any_failed = false;
  /* Every (row, channel) this material still has an anchor for, so entries for removed anchors can
   * be dropped once the whole walk is done. */
  Vector<MaskBakeCacheKey> live_keys;
  for (bNodeTree *tree : trees) {
    tree->ensure_topology_cache();
    /* Collected before any pixels are touched, so the topology cache stays current while the walk
     * below reads the anchors. */
    Vector<const bNode *> anchors;
    for (const bNode &node : tree->nodes) {
      if (anchor_node_is(node)) {
        anchors.append(&node);
      }
    }

    for (const bNode *anchor_node : anchors) {
      const bUUID row_marker = bke::paint_layer::marker_get(*anchor_node);
      const int channel = anchor_channel_get(*anchor_node);
      MASK_BAKE_TRACE("ensure anchor=%p channel=%d n_trees=%zu\n",
                      static_cast<void *>(const_cast<bNode *>(anchor_node)),
                      channel,
                      size_t(trees.size()));
      if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
        /* A role outside the channels -- the mask itself, most of all -- has no stack to compose
         * the row's coverage from. */
        continue;
      }
      MaskBakeAnchor anchor;
      if (!mask_bake_anchor_read(*tree, row_marker, channel, anchor) ||
          anchor.image == nullptr)
      {
        continue;
      }
      Vector<PaintMaterialCompositeImageLayer> layers;
      if (!BKE_paint_material_composite_stack_from_material(bmain, ma, channel, layers)) {
        continue;
      }
      /* The row's own layer, for its structural hash and its dependency images; the rest of the
       * stack only decides B's dimensions, which the size check below already compares. */
      const PaintMaterialCompositeImageLayer *row_layer = nullptr;
      for (const PaintMaterialCompositeImageLayer &candidate : layers) {
        if (BLI_uuid_equal(candidate.marker, row_marker)) {
          row_layer = &candidate;
          break;
        }
      }
      if (row_layer == nullptr) {
        continue;
      }
      any_anchor = true;

      MaskBakeCacheKey key;
      key.material_session_uid = ma.id.session_uid;
      key.row_marker = row_marker;
      key.channel = channel;
      live_keys.append(key);

      Image *image = anchor.image;
      void *lock = nullptr;
      ImBuf *ibuf = mask_bake_acquire_byte_rgba(image, &lock);
      if (ibuf == nullptr) {
        any_failed = true;
        continue;
      }

      MaskBakeCacheEntry &cache_entry = g_cache.entries.lookup_or_add_default(key);
      cache_entry.last_use = ++g_cache.use_counter;

      /* B's buffer is the entry's buffer: nothing is owned here, only remembered. A size change --
       * a fresh B, a resize -- invalidates whatever the entry knew about the old pixels. */
      const bool size_changed = cache_entry.width != ibuf->x || cache_entry.height != ibuf->y;
      if (size_changed) {
        cache_entry.width = ibuf->x;
        cache_entry.height = ibuf->y;
        cache_entry.dirty_full = true;
      }

      mask_bake_entry_poll_dependencies(cache_entry, mask_bake_layer_images(*row_layer));

      const uint64_t structural_hash = mask_bake_layer_structural_hash(*row_layer);
      const bool rebuild_all = force_full || cache_entry.dirty_full ||
                               cache_entry.structural_hash != structural_hash || size_changed;
      const bool rebuild_region = !rebuild_all &&
                                  !BLI_rcti_is_empty(&cache_entry.dirty_region);

      bool wrote = false;
      if (rebuild_all || rebuild_region) {
        const rcti *region = rebuild_region ? &cache_entry.dirty_region : nullptr;
        if (BKE_paint_material_composite_eval_row_mask(layers, row_marker, ibuf, region)) {
          cache_entry.structural_hash = structural_hash;
          cache_entry.dirty_full = false;
          cache_entry.revision = ++g_cache.revision_counter;
          BLI_rcti_init(&cache_entry.dirty_region, 0, 0, 0, 0);
          wrote = true;
        }
        else {
          /* The row could not be built -- disabled, absent, a buffer missing. Leave the entry
           * dirty so the next call retries rather than serving stale pixels. */
          any_failed = true;
        }
      }

      if (wrote) {
        BKE_image_mark_dirty(image, ibuf);
      }
      BKE_image_release_ibuf(image, ibuf, lock);
      if (wrote) {
        /* B is a paint target of its own; the tags below are what tells the GPU and any poller
         * that its pixels moved, exactly as the uncached bake did. */
        BKE_image_partial_update_mark_full_update(image);
        BKE_image_free_gputextures(image);
        any_refreshed = true;
      }
      mask_bake_cache_enforce_budget(key);
    }
  }

  mask_bake_cache_drop_missing(ma.id.session_uid, live_keys);

  /* Nothing anchored is nothing to do, and a call that found everything clean wrote nothing and
   * is still a success; only an anchor that was dirty and could not be written is a failure. */
  return !any_anchor || any_refreshed || !any_failed;
}

void BKE_paint_material_mask_bake_cache_invalidate(const Material *ma)
{
  if (ma == nullptr) {
    for (MaskBakeCacheEntry &entry : g_cache.entries.values()) {
      entry.dirty_full = true;
    }
    return;
  }
  const uint32_t session_uid = ma->id.session_uid;
  for (auto item : g_cache.entries.items()) {
    if (item.key.material_session_uid == session_uid) {
      item.value.dirty_full = true;
    }
  }
}

void BKE_paint_material_mask_bake_cache_free_material(const Material &ma)
{
  if (g_cache.entries.is_empty()) {
    /* This runs from #ID free, so it is on the path of every material in every file ever loaded,
     * almost none of which ever had a baked mask. */
    return;
  }
  const uint32_t session_uid = ma.id.session_uid;
  Vector<MaskBakeCacheKey> dead_keys;
  for (auto item : g_cache.entries.items()) {
    if (item.key.material_session_uid == session_uid) {
      dead_keys.append(item.key);
    }
  }
  /* Collected first: removing from the map while iterating it would invalidate the iteration. */
  for (const MaskBakeCacheKey &key : dead_keys) {
    g_cache.entries.remove(key);
  }
}

void BKE_paint_material_mask_bake_cache_free_all()
{
  /* Clearing is the whole teardown: every entry owns its subscriptions outright. */
  g_cache.entries.clear();
}

bool BKE_paint_material_mask_bake_cache_contains(const Material &ma,
                                                 const bUUID &row_marker,
                                                 const int channel)
{
  MaskBakeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.row_marker = row_marker;
  key.channel = channel;
  return g_cache.entries.contains(key);
}

uint64_t BKE_paint_material_mask_bake_cache_revision(const Material &ma,
                                                     const bUUID &row_marker,
                                                     const int channel)
{
  MaskBakeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.row_marker = row_marker;
  key.channel = channel;
  const MaskBakeCacheEntry *entry = g_cache.entries.lookup_ptr(key);
  return (entry != nullptr) ? entry->revision : 0;
}

}  // namespace blender
