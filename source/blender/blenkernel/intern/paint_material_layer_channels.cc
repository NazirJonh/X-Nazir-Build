/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Bringing a material's channels to the same row structure
 * (#BKE_paint_material_layer_channels_ensure
 * and the migration it drives), building a stack from scratch
 * (#BKE_paint_material_layer_add_material_base),
 * and the per-channel Absent/Enabled/Disabled state a row's channels carry
 * (#BKE_paint_material_layer_channel_enabled_set and its readers). See
 * #BKE_paint_material_layer_edit.hh; the chain-reading infrastructure these mutations share with
 * `paint_material_layer_edit.cc` and `paint_material_layer_props.cc` lives in
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
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include <utility>

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

void BKE_paint_material_layer_channels_wired(Material &ma, Vector<int> &r_channels)
{
  r_channels.clear();
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  Vector<ChannelChain> chains;
  if (!chains_collect(ma, chains, error)) {
    return;
  }
  for (const ChannelChain &chain : chains) {
    r_channels.append(chain.channel);
  }
}

namespace {

/**
 * Whether the Principled input of \a channel carries a user link right now.
 *
 * A missing socket (older Principled without this input) reads as unlinked: the caller asked
 * for this channel, so a missing socket skips rather than denies (same as the forest reader,
 * which simply never reports such a channel).
 */
bool ensure_principled_input_is_linked(Material &ma, const int channel)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason);
  if (principled == nullptr) {
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  if (info.socket_name == nullptr) {
    return false;
  }
  bNodeSocket *socket = socket_find_by_name(
      const_cast<bNode &>(*principled), SOCK_IN, StringRef(info.socket_name));
  if (socket == nullptr) {
    return false;
  }
  return socket_has_link(*socket);
}

/** The Principled input of \a channel, or null when there is no Principled or no such input. */
bNodeSocket *ensure_principled_input_find(Material &ma, const int channel)
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
  return socket_find_by_name(
      const_cast<bNode &>(*principled), SOCK_IN, StringRef(info.socket_name));
}

/** Size of the map \a ref shows, so a neutral mirror stays a drop-in tile. Falls back silently. */
void ensure_ref_image_size(Image *ref, int &r_size_x, int &r_size_y)
{
  if (ref == nullptr) {
    return;
  }
  ImageUser iuser;
  BKE_imageuser_default(&iuser);
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(ref, &iuser, &lock);
  if (ibuf == nullptr) {
    return;
  }
  if (ibuf->x > 0 && ibuf->y > 0) {
    r_size_x = ibuf->x;
    r_size_y = ibuf->y;
  }
  BKE_image_release_ibuf(ref, ibuf, lock);
}

/**
 * A neutral map for \a channel: transparent, so a row fed by it contributes nothing -- except
 * the bottom of a top-level chain, which is opaque and carries \a opaque_color (the constant
 * the free Principled input used to supply on its own).
 */
Image *ensure_neutral_image_create(Main &bmain,
                                   const int channel,
                                   const int size_x,
                                   const int size_y,
                                   const bool opaque,
                                   const float opaque_color[4])
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char image_name[MAX_ID_NAME - 2];
  SNPRINTF_UTF8(image_name, "%s Neutral", info.ui_name);
  float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    /* Flat tangent space; the alpha still decides coverage, like on every channel. */
    color[0] = 0.5f;
    color[1] = 0.5f;
    color[2] = 1.0f;
  }
  if (opaque) {
    if (opaque_color != nullptr && channel != PAINT_MATERIAL_CHANNEL_NORMAL) {
      color[0] = opaque_color[0];
      color[1] = opaque_color[1];
      color[2] = opaque_color[2];
    }
    color[3] = 1.0f;
  }
  Image *image = BKE_image_add_generated(&bmain,
                                         size_x,
                                         size_y,
                                         image_name,
                                         32,
                                         false,
                                         IMA_GENTYPE_BLANK,
                                         color,
                                         false,
                                         !info.is_color,
                                         false);
  if (image != nullptr) {
    image->flag |= IMA_PAINT_CANVAS;
  }
  return image;
}

/** How #ensure_discard releases one created node. */
enum class EnsureNodeKind : int8_t {
  /** Plain node with no ID users of its own (Mix, Normal Map). */
  Plain = 0,
  /** Group instance this transaction gave a user (`id_us_plus`): give it back. */
  Instance,
  /** Image Texture showing a transaction image: clear the reference before the image goes. */
  Texture,
};

struct EnsureCreated {
  bNodeTree *tree = nullptr;
  bNode *node = nullptr;
  EnsureNodeKind kind = EnsureNodeKind::Plain;
};

/**
 * Undo one channel's node creation. Interface sockets added to group trees stay behind:
 * unlinked they are transparent, which is exactly what an empty Result contributes.
 */
void ensure_discard(Main &bmain, Vector<EnsureCreated> &r_created, Vector<Image *> &r_images)
{
  for (const EnsureCreated &created : r_created) {
    if (created.node == nullptr || created.tree == nullptr) {
      continue;
    }
    if (created.kind == EnsureNodeKind::Instance && created.node->id != nullptr) {
      id_us_min(created.node->id);
      created.node->id = nullptr;
    }
    if (created.kind == EnsureNodeKind::Texture) {
      created.node->id = nullptr;
    }
    bke::node_remove_node(&bmain, *created.tree, *created.node, false);
  }
  r_created.clear();
  for (Image *image : r_images) {
    if (image != nullptr) {
      BKE_id_free(&bmain, image);
    }
  }
  r_images.clear();
}

/** One built row: the nodes a reference row mirrors into. */
struct EnsureBuiltRow {
  bNode *tex = nullptr;
  Image *image = nullptr;
  bNode *mix = nullptr;
  bNode *instance = nullptr;
  bool is_group = false;
};

/** Where a mirrored chain feeds: resolved fresh after the tree update, never cached across one. */
struct EnsureTerminal {
  bNode *node = nullptr;
  eNodeSocketInOut in_out = SOCK_IN;
  const char *socket_name = nullptr;
};

/** The Group Output node of \a group_tree, or null when the folder lost it. */
bNode *ensure_group_output_find(bNodeTree &group_tree)
{
  for (bNode &node : group_tree.nodes) {
    if (node.type_legacy == NODE_GROUP_OUTPUT) {
      return &node;
    }
  }
  return nullptr;
}

/**
 * The `Result <Channel>` input of \a group_tree's output, adding the interface socket first
 * when this channel reaches the folder for the first time.
 */
bool ensure_group_result_socket(Main &bmain,
                                bNodeTree &group_tree,
                                const int channel,
                                PaintMaterialLayerEditError &r_error)
{
  bNode *output = ensure_group_output_find(group_tree);
  if (output == nullptr) {
    r_error = PaintMaterialLayerEditError::ChainNotPlain;
    return false;
  }
  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char result_name[64];
  SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);
  if (socket_find_by_name(*output, SOCK_IN, StringRef(result_name)) != nullptr) {
    return true;
  }
  if (group_tree.tree_interface.add_socket(
          result_name, "", "NodeSocketColor", NODE_INTERFACE_SOCKET_OUTPUT, nullptr) == nullptr)
  {
    r_error = PaintMaterialLayerEditError::CreationFailed;
    return false;
  }
  BKE_ntree_update_after_single_tree_change(bmain, group_tree);
  group_tree.ensure_topology_cache();
  return true;
}

/**
 * Build one chain mirroring \a ref_chain's rows for \a channel, bottom to top.
 *
 * Plain rows get no map (Absent): the hidden base under the chain supplies what the channel
 * showed before (see #chain_base_apply). Group rows get a fresh instance of the same folder
 * blended by a keeper Mix. Every Mix carries its reference row's marker -- minted onto
 * the reference itself when an old stack never got one -- so the row keeps one identity in
 * every channel at once.
 *
 * \a r_built, when given, receives the chain it built: the handles its mirror-corrections pass
 * (#ensure_corrections_mirror) inserts through.
 */
static bool ensure_mirror_chain(Main &bmain,
                                bNodeTree &tree,
                                const int channel,
                                const ChannelChain &ref_chain,
                                const EnsureTerminal &terminal,
                                Vector<EnsureCreated> &r_created,
                                ChannelChain *r_built,
                                PaintMaterialLayerEditError &r_error)
{
  /* 1. Create every node first: group instances only grow their sockets on a tree update. */
  Vector<EnsureBuiltRow> rows;
  for (const int64_t j : ref_chain.layers.index_range()) {
    /* The caller (and a previous iteration) may have left the tree's topology cache dirty;
     * #composite_source_node_shallow below reads a reference socket's links. */
    tree.ensure_topology_cache();

    const ChainLayer &ref_layer = ref_chain.layers[j];
    if (!ref_layer.is_mix() || ref_layer.node == nullptr || ref_layer.top == nullptr) {
      r_error = PaintMaterialLayerEditError::ChainNotPlain;
      return false;
    }
    bUUID row_id = BKE_paint_material_layer_marker_get(*ref_layer.node);
    if (BLI_uuid_is_nil(row_id)) {
      row_id = BLI_uuid_generate_random();
      BKE_paint_material_layer_marker_set(*ref_layer.node, row_id);
    }

    EnsureBuiltRow row;
    row.is_group = ref_layer.is_group;
    if (!ref_layer.is_group) {
      /* A mirrored row has no map in the new channel: Absent. The base under the chain -- not an
       * opaque neutral image -- supplies what the channel showed before (see
       * #chain_base_apply). */
      row.image = nullptr;
      row.tex = nullptr;
    }
    else {
      const bNode *ref_instance = composite_source_node_shallow(*ref_layer.top);
      if (ref_instance == nullptr || ref_instance->id == nullptr || tree.typeinfo == nullptr ||
          tree.typeinfo->group_idname == nullptr)
      {
        r_error = PaintMaterialLayerEditError::ChainNotPlain;
        return false;
      }
      row.instance = bke::node_add_node(nullptr, tree, tree.typeinfo->group_idname);
      if (row.instance == nullptr) {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      row.instance->id = ref_instance->id;
      id_us_plus(row.instance->id);
      r_created.append({&tree, row.instance, EnsureNodeKind::Instance});
    }
    row.mix = layer_mix_node_create(bmain, tree, channel);
    if (row.mix == nullptr) {
      r_error = PaintMaterialLayerEditError::CreationFailed;
      return false;
    }
    r_created.append({&tree, row.mix, EnsureNodeKind::Plain});
    if (ref_layer.node->label[0] != '\0') {
      STRNCPY_UTF8(row.mix->label, ref_layer.node->label);
    }
    if ((ref_layer.node->flag & NODE_MUTED) != 0) {
      row.mix->flag |= NODE_MUTED;
    }
    BKE_paint_material_layer_marker_set(*row.mix, row_id);
    rows.append(row);
  }

  /* 2. Sockets exist only after the update; resolve everything before touching a link. */
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  tree.ensure_topology_cache();

  bNodeSocket *terminal_socket = socket_find_by_name(
      *terminal.node, terminal.in_out, StringRef(terminal.socket_name));
  if (terminal_socket == nullptr) {
    r_error = PaintMaterialLayerEditError::CreationFailed;
    return false;
  }

  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  char result_name[64];
  SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);

  ChannelChain built;
  built.tree = &tree;
  built.channel = channel;
  built.terminal = terminal_socket;
  built.terminal_node = terminal.node;
  for (EnsureBuiltRow &row : rows) {
    /* The previous iteration wired a link and added a coverage Multiply node, which leaves the
     * topology cache dirty; #composite_mix_node_read walks the Factor's links, so it needs the
     * cache rebuilt from the graph as it stands now. */
    tree.ensure_topology_cache();

    CompositeMixNode mix_prm;
    bNodeSocket *output = mix_output_find(*row.mix);
    const bool mix_read = (output == nullptr) ?
                              false :
                              composite_mix_node_read(*row.mix, mix_prm);
    if (output == nullptr || !mix_read) {
      r_error = PaintMaterialLayerEditError::CreationFailed;
      return false;
    }
    ChainLayer layer;
    layer.node = row.mix;
    layer.bottom = const_cast<bNodeSocket *>(mix_prm.bottom);
    layer.top = const_cast<bNodeSocket *>(mix_prm.top);
    layer.factor = const_cast<bNodeSocket *>(mix_prm.factor);
    layer.output = output;
    layer.image = row.image;
    if (!row.is_group) {
      bNode *multiply = layer_factor_absent_link(tree, *row.mix, *layer.factor, 1.0f);
      if (multiply == nullptr) {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      /* Registered so a later failure in this channel takes it away with the Mix it feeds. */
      r_created.append({&tree, multiply, EnsureNodeKind::Plain});
    }
    else {
      /* The folder gained its Result socket up front; a missing one now means the interface
       * and the instances disagree. */
      bNodeSocket *result = socket_find_by_name(*row.instance, SOCK_OUT, result_name);
      bNodeSocket *alpha = socket_find_by_name(*row.instance, SOCK_OUT, "Alpha");
      if (result == nullptr || alpha == nullptr) {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      bke::node_add_link(tree, *row.instance, *result, *row.mix, *layer.top);
      bNode *multiply = layer_factor_coverage_link(
          tree, *row.mix, *layer.factor, *row.instance, *alpha, 1.0f);
      if (multiply == nullptr) {
        r_error = PaintMaterialLayerEditError::CreationFailed;
        return false;
      }
      r_created.append({&tree, multiply, EnsureNodeKind::Plain});
      layer.is_group = true;
      bke::node_position_relative(*row.instance, *row.mix, result, *layer.top);
    }
    bke::node_position_relative(*row.mix, *terminal.node, layer.output, *terminal_socket);
    built.layers.append(layer);
  }

  /* 3. Wire bottom-up; the terminal takes the top, like every other chain. */
  chain_rebuild_links(built);
  if (r_built != nullptr) {
    *r_built = std::move(built);
  }
  return true;
}

/**
 * The reference rows' corrections, mirrored into \a built's channel (spec 18 §4.5): every wired
 * channel carries the same correction rows, so a migrated channel gets them too, and a channel
 * that drifted -- a correction taken out of it by hand -- is brought back to the reference's
 * sequence. A section whose UUID sequence differs is rebuilt: the rows the channel shows beyond
 * the reference come out (#correction_channel_remove closes their links), then the reference's
 * rows are inserted bottom to top -- an insert appends on top, so the order comes out right. Each
 * insert starts Absent -- the shape #correction_channel_insert builds -- then takes the
 * reference's label, blend, opacity and mute state, which are the row's, not the channel's. A
 * mask section's shared map is wired in from the start, the way
 * #BKE_paint_material_layer_correction_add gives a mask one; no map by that tag leaves the
 * correction Absent here. A section the two chains already agree on is left untouched: the
 * channel's own maps and mute states are its own.
 *
 * The inserts register their nodes with \a r_created, so a later failure of the migration takes
 * them back out; an insert that refuses undoes its own nodes. \a r_changed says whether anything
 * was built or taken out.
 */
static bool ensure_corrections_mirror(Main &bmain,
                                      const ChannelChain &ref_chain,
                                      ChannelChain &built,
                                      Vector<EnsureCreated> &r_created,
                                      bool &r_changed,
                                      PaintMaterialLayerEditError &r_error)
{
  auto rows_have_corrections = [](const ChannelChain &chain) {
    for (const ChainLayer &layer : chain.layers) {
      if (!layer.content_corrections.is_empty() || !layer.mask_corrections.is_empty()) {
        return true;
      }
    }
    return false;
  };
  if (!rows_have_corrections(ref_chain) && !rows_have_corrections(built)) {
    return true;
  }
  bNodeTree &tree = *built.tree;
  for (const int64_t j : ref_chain.layers.index_range()) {
    const ChainLayer &ref_layer = ref_chain.layers[j];
    ChainLayer &layer = built.layers[j];
    bool layer_changed = false;
    for (const int section_i : IndexRange(2)) {
      const auto section = PaintMaterialCorrectionSection(section_i);
      const Vector<ChainCorrection> &ref_rows =
          (section == PaintMaterialCorrectionSection::Content) ? ref_layer.content_corrections :
                                                                 ref_layer.mask_corrections;
      Vector<ChainCorrection> &rows = (section == PaintMaterialCorrectionSection::Content) ?
                                          layer.content_corrections :
                                          layer.mask_corrections;

      bool same = ref_rows.size() == rows.size();
      for (const int64_t i : ref_rows.index_range()) {
        same = same && BLI_uuid_equal(ref_rows[i].marker, rows[i].marker);
      }
      if (same) {
        continue;
      }
      for (const ChainCorrection &nodes : rows) {
        /* The drifted row's own nodes go, links closed behind them (spec 18 §4.1a). */
        tree.ensure_topology_cache();
        correction_channel_remove(bmain, built, layer, nodes);
        r_changed = true;
      }
      rows.clear();

      for (const ChainCorrection &ref : ref_rows) {
        /* The previous insert left the cache stale; this one reads the row's links. */
        tree.ensure_topology_cache();
        ChainCorrection nodes;
        if (!correction_channel_insert(bmain, built, layer, section, ref.marker, nodes)) {
          r_error = PaintMaterialLayerEditError::CreationFailed;
          return false;
        }
        r_changed = true;
        /* Registered so a later failure of this migration takes the row back out. */
        r_created.append({&tree, nodes.mix, EnsureNodeKind::Plain});
        r_created.append({&tree, nodes.factor_multiply, EnsureNodeKind::Plain});
        if (nodes.over_invert != nullptr) {
          r_created.append({&tree, nodes.over_invert, EnsureNodeKind::Plain});
        }
        if (nodes.over_combine != nullptr) {
          r_created.append({&tree, nodes.over_combine, EnsureNodeKind::Plain});
        }
        tree.ensure_topology_cache();
        CompositeMixNode row_mix;
        CompositeMixNode ref_mix;
        if (nodes.mix == nullptr || ref.mix == nullptr ||
            !composite_mix_node_read(*nodes.mix, row_mix) ||
            !composite_mix_node_read(*ref.mix, ref_mix))
        {
          r_error = PaintMaterialLayerEditError::ChainNotPlain;
          return false;
        }
        /* The row is one identity across channels: name, blend, opacity and the on/off state
         * are the reference's. */
        if (ref.mix->label[0] != '\0') {
          STRNCPY_UTF8(nodes.mix->label, ref.mix->label);
        }
        if (nodes.mix->type_legacy == SH_NODE_MIX && ref.mix->type_legacy == SH_NODE_MIX) {
          static_cast<NodeShaderMix *>(nodes.mix->storage)->blend_type =
              static_cast<const NodeShaderMix *>(ref.mix->storage)->blend_type;
        }
        else if (nodes.mix->type_legacy == SH_NODE_MIX &&
                 ref.mix->type_legacy == SH_NODE_MIX_RGB_LEGACY)
        {
          static_cast<NodeShaderMix *>(nodes.mix->storage)->blend_type = int8_t(ref.mix->custom1);
        }
        if (ref_mix.factor_opacity != nullptr && row_mix.factor_opacity != nullptr) {
          static_cast<bNodeSocketValueFloat *>(
              const_cast<bNodeSocket *>(row_mix.factor_opacity)->default_value)
              ->value = static_cast<const bNodeSocketValueFloat *>(
                            ref_mix.factor_opacity->default_value)
                            ->value;
        }
        if (section == PaintMaterialCorrectionSection::Mask) {
          /* A mask has no per-channel choice (spec 18 §4.3): the shared map the reference shows
           * is what this channel's correction shows too. */
          Image *mask = correction_tagged_map_find(bmain, ref.marker, PAINT_LAYER_MAP_MASK);
          if (mask != nullptr && row_mix.top != nullptr && row_mix.factor_coverage != nullptr)
          {
            bNode *tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
            bNodeSocket *tex_color = (tex != nullptr) ?
                                         bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr) :
                                         nullptr;
            bNodeSocket *tex_alpha = (tex != nullptr) ?
                                         bke::node_find_socket(*tex, SOCK_OUT, "Alpha"_ustr) :
                                         nullptr;
            if (tex == nullptr || tex_color == nullptr || tex_alpha == nullptr) {
              if (tex != nullptr) {
                bke::node_remove_node(&bmain, tree, *tex, false);
              }
              r_error = PaintMaterialLayerEditError::CreationFailed;
              return false;
            }
            /* The image is shared with the other channels' nodes: this node is one more user. */
            tex->id = &mask->id;
            id_us_plus(&mask->id);
            /* Registered as an Instance: the discard gives the user back before the node goes. */
            r_created.append({&tree, tex, EnsureNodeKind::Instance});
            /* The map's Color is what the correction blends towards, its Alpha what covers --
             * the same reading the stack model and the compositor give a correction's map. */
            bke::node_add_link(
                tree, *tex, *tex_color, *nodes.mix, *const_cast<bNodeSocket *>(row_mix.top));
            bke::node_add_link(tree,
                               *tex,
                               *tex_alpha,
                               *nodes.factor_multiply,
                               *const_cast<bNodeSocket *>(row_mix.factor_coverage));
            bke::node_position_relative(
                *tex, *nodes.mix, tex_color, *const_cast<bNodeSocket *>(row_mix.top));
            nodes.map = tex;
            nodes.image = mask;
          }
        }
        /* The row switches as one (spec 18 §4.1): the reference's on/off state, applied the way
         * #BKE_paint_material_layer_correction_set_enabled applies it -- Mix and maps muted, the
         * over pair gated rather than muted. */
        correction_row_enabled_apply(tree, nodes, (ref.mix->flag & NODE_MUTED) == 0);
        rows.append(nodes);
      }
      layer_changed = true;
    }
    if (layer_changed) {
      /* The rebuilt mask rows take the form what the row puts into this channel implies. */
      layer_mask_corrections_sync(tree, layer);
    }
  }
  /* The inserts wrote nodes, links and flags after the mirror's own update ran. */
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  return true;
}

/**
 * Migrate one missing channel into \a ref_forest's shape: primary wiring at the Principled
 * input (a fresh Normal Map for Normal), a `Result` interface socket on every folder, and a
 * mirrored chain in every tree. Either the whole channel lands or nothing does.
 */
static bool ensure_migrate_channel(Main &bmain,
                                   Material &ma,
                                   const int channel,
                                   const Vector<ChannelChain> &ref_forest,
                                   PaintMaterialLayerEditError &r_error)
{
  Vector<EnsureCreated> created;
  Vector<Image *> images;
  auto fail = [&](const PaintMaterialLayerEditError error) {
    ensure_discard(bmain, created, images);
    r_error = error;
    return false;
  };

  /* Folders this channel reaches for the first time need their Result socket up front:
   * the outer instances only grow it on a tree update. */
  Vector<bNodeTree *> group_trees;
  Set<bNodeTree *> group_seen;
  for (const ChannelChain &ref_chain : ref_forest) {
    for (const ChainLayer &ref_layer : ref_chain.layers) {
      if (!ref_layer.is_group || ref_layer.top == nullptr) {
        continue;
      }
      const bNode *ref_instance = composite_source_node_shallow(*ref_layer.top);
      bNodeTree *group_tree = (ref_instance == nullptr) ? nullptr :
                                                            layer_group_tree_of(*ref_instance);
      if (group_tree == nullptr) {
        return fail(PaintMaterialLayerEditError::ChainNotPlain);
      }
      if (group_seen.add(group_tree)) {
        group_trees.append(group_tree);
      }
    }
  }

  /* The Normal chain starts one node earlier, at a fresh Normal Map: the Principled Normal
   * input carries an already transformed vector no stack can be recovered from. */
  bNode *normal_map = nullptr;
  if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
    normal_map = bke::node_add_static_node(nullptr, *ma.nodetree, SH_NODE_NORMAL_MAP);
    if (normal_map == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    created.append({ma.nodetree, normal_map, EnsureNodeKind::Plain});
    bNodeSocket *map_color = bke::node_find_socket(*normal_map, SOCK_IN, "Color"_ustr);
    bNodeSocket *map_normal = bke::node_find_socket(*normal_map, SOCK_OUT, "Normal"_ustr);
    bNodeSocket *principled_normal = ensure_principled_input_find(ma, channel);
    if (map_color == nullptr || map_normal == nullptr || principled_normal == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    bNode &principled = const_cast<bNode &>(principled_normal->owner_node());
    bke::node_add_link(*ma.nodetree, *normal_map, *map_normal, principled, *principled_normal);
    bke::node_position_relative(*normal_map, principled, map_normal, *principled_normal);
  }

  for (bNodeTree *group_tree : group_trees) {
    if (!ensure_group_result_socket(bmain, *group_tree, channel, r_error)) {
      ensure_discard(bmain, created, images);
      return false;
    }
  }

  const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
      eMaterialPaintChannel(channel));
  const int top_index = int(ref_forest.size()) - 1;
  for (const int64_t k : ref_forest.index_range()) {
    const ChannelChain &ref_chain = ref_forest[k];
    EnsureTerminal terminal;
    char result_name[64] = "";
    if (k == top_index) {
      if (channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
        terminal.node = normal_map;
        terminal.in_out = SOCK_IN;
        terminal.socket_name = "Color";
      }
      else {
        bNodeSocket *input = ensure_principled_input_find(ma, channel);
        if (input == nullptr) {
          return fail(PaintMaterialLayerEditError::CreationFailed);
        }
        terminal.node = &const_cast<bNode &>(input->owner_node());
        terminal.in_out = SOCK_IN;
        terminal.socket_name = info.socket_name;
      }
    }
    else {
      bNode *output = ensure_group_output_find(*ref_chain.tree);
      if (output == nullptr) {
        return fail(PaintMaterialLayerEditError::ChainNotPlain);
      }
      SNPRINTF_UTF8(result_name, "Result %s", info.ui_name);
      terminal.node = output;
      terminal.in_out = SOCK_IN;
      terminal.socket_name = result_name;
    }
    ChannelChain built;
    if (!ensure_mirror_chain(bmain,
                             *ref_chain.tree,
                             channel,
                             ref_chain,
                             terminal,
                             created,
                             &built,
                             r_error))
    {
      ensure_discard(bmain, created, images);
      return false;
    }
    /* The corrections the reference rows carry are part of the row structure being mirrored
     * (spec 18 §4.1a); a migrated channel without them would disagree with every other one. */
    bool mirror_changed = false;
    if (!ensure_corrections_mirror(bmain, ref_chain, built, created, mirror_changed, r_error)) {
      ensure_discard(bmain, created, images);
      return false;
    }
  }
  return true;
}

}  // namespace

bool BKE_paint_material_layer_channels_ensure(Main &bmain,
                                              Material &ma,
                                              Span<int> channels,
                                              PaintMaterialLayerEditError *r_error)
{
  auto fail = [&](const PaintMaterialLayerEditError error) {
    if (r_error != nullptr) {
      *r_error = error;
    }
    return false;
  };
  /* Channels with no Principled input (Custom, Height, AO) can never be wired: asking for
   * them is meaningless, so they drop out before the forest is even read. */
  Vector<int> work;
  for (const int channel : channels) {
    if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
      return fail(PaintMaterialLayerEditError::IndexOutOfRange);
    }
    if (BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).socket_name == nullptr) {
      continue;
    }
    work.append_non_duplicates(channel);
  }
  if (work.is_empty()) {
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    return true;
  }
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Empty material takes path E (#BKE_paint_material_layer_add_material_base); there is no
   * forest to mirror here. */
  Vector<Vector<ChannelChain>> forest;
  PaintMaterialLayerEditError forest_error = PaintMaterialLayerEditError::None;
  if (!chains_collect_forest(ma, forest, forest_error)) {
    return fail(forest_error);
  }
  /* The stack the other operations already refuse to rewrite is refused here too, before a
   * byte is written. */
  {
    const int64_t top_len = forest.first().last().layers.size();
    for (const Vector<ChannelChain> &per_channel : forest) {
      if (per_channel.last().layers.size() != top_len) {
        return fail(PaintMaterialLayerEditError::ChannelsDisagree);
      }
    }
  }
  {
    Set<const bNodeTree *> scope_seen;
    const auto scope_check = [&](bNodeTree &tree) {
      if (scope_seen.add(&tree)) {
        PaintMaterialLayerEditError scope_error = PaintMaterialLayerEditError::None;
        if (!tree_write_scope_check(bmain, ma, tree, scope_error)) {
          forest_error = scope_error;
          return false;
        }
      }
      return true;
    };
    if (!scope_check(*ma.nodetree)) {
      return fail(forest_error);
    }
    for (const ChannelChain &ref_chain : forest.first()) {
      for (const ChainLayer &ref_layer : ref_chain.layers) {
        if (!ref_layer.is_group || ref_layer.top == nullptr) {
          continue;
        }
        const bNode *ref_instance = composite_source_node_shallow(*ref_layer.top);
        bNodeTree *group_tree = (ref_instance == nullptr) ? nullptr :
                                                          layer_group_tree_of(*ref_instance);
        if (group_tree == nullptr) {
          return fail(PaintMaterialLayerEditError::ChainNotPlain);
        }
        if (!scope_check(*group_tree)) {
          return fail(forest_error);
        }
      }
    }
  }
  auto have_channel = [&](const Vector<Vector<ChannelChain>> &look, const int channel) {
    for (const Vector<ChannelChain> &per_channel : look) {
      if (!per_channel.is_empty() && per_channel.last().channel == channel) {
        return true;
      }
    }
    return false;
  };
  for (const int channel : work) {
    if (have_channel(forest, channel)) {
      continue;
    }
    /* A link that is not a stack is someone's deliberate graph: replacing it would silently
     * change what the material looks like. */
    if (ensure_principled_input_is_linked(ma, channel)) {
      return fail(PaintMaterialLayerEditError::ChannelHasUnsupportedSource);
    }
  }
  /* Bare bottoms cannot lend a marker to their mirror: wrap them first, the way every other
   * operation does once its preflight has passed. */
  if (forest_has_bare_bottom(forest)) {
    if (BKE_paint_material_layer_bottom_normalize(bmain, ma)) {
      BKE_paint_material_layer_markers_ensure(ma);
    }
    if (!chains_collect_forest(ma, forest, forest_error)) {
      return fail(forest_error);
    }
  }
  for (const int channel : work) {
    if (have_channel(forest, channel)) {
      continue;
    }
    if (!ensure_migrate_channel(bmain, ma, channel, forest.first(), forest_error)) {
      /* #ensure_migrate_channel unwinds its own channel, but a channel migrated on an earlier
       * pass of this loop stays in the graph (as does a `Result` socket it added to a folder).
       * The operator that drives this is #OPTYPE_UNDO, so a single undo step still takes the
       * whole partial migration back; a self-contained rollback across channels is a follow-up. */
      return fail(forest_error);
    }
    /* The new channel invalidates the collected topology: read the forest again so the next
     * channel mirrors a graph as it is, with the markers just minted still on it. */
    if (!chains_collect_forest(ma, forest, forest_error)) {
      return fail(forest_error);
    }
  }
  BKE_paint_material_layer_markers_ensure(ma);
  BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

namespace {

/** The nodes making up a chain's top row, resolved while the topology cache is still good. */
struct RealignTopRowNodes {
  bNode *mix = nullptr;
  bNode *map = nullptr;
  /** The Multiply a mask or the map's own alpha reads through, when the Factor has one. */
  bNode *coverage = nullptr;
  /** The nodes of the row's corrections and its mask (spec 18 §4.5): they hang on the row's own
   * inputs, so a trimmed row takes them with it. Empty for a row without corrections, which the
   * removal below leaves exactly as it handled it before. */
  Vector<bNode *> corrections;
};

RealignTopRowNodes realign_top_row_resolve(bNodeTree &tree, const ChainLayer &top)
{
  RealignTopRowNodes row;
  tree.ensure_topology_cache();
  row.mix = top.node;
  CompositeMixNode mix;
  if (row.mix != nullptr && composite_mix_node_read(*row.mix, mix)) {
    row.map = const_cast<bNode *>(composite_mix_map_node(mix));
    if (mix.factor != nullptr) {
      if (bNodeLink *factor_link = sole_link_into(*const_cast<bNodeSocket *>(mix.factor))) {
        if (factor_link->fromnode != row.map) {
          row.coverage = factor_link->fromnode;
        }
      }
    }
  }
  /* What the row owns beyond the three nodes above -- its corrections and its mask -- is read
   * with it by the chain collector, and goes when the row goes. Only rows with corrections take
   * this path: a row without any is removed exactly as it was before they existed. */
  if (!top.content_corrections.is_empty() || !top.mask_corrections.is_empty()) {
    Vector<bNode *> owned;
    layer_owned_nodes_collect(top, owned);
    for (bNode *node : owned) {
      if (node != row.mix && node != row.map && node != row.coverage) {
        row.corrections.append_non_duplicates(node);
      }
    }
  }
  return row;
}

void realign_top_row_remove(Main &bmain, bNodeTree &tree, const RealignTopRowNodes &row)
{
  for (bNode *node : row.corrections) {
    bke::node_remove_node(&bmain, tree, *node, true);
  }
  if (row.coverage != nullptr) {
    bke::node_remove_node(&bmain, tree, *row.coverage, true);
  }
  if (row.map != nullptr) {
    bke::node_remove_node(&bmain, tree, *row.map, true);
  }
  if (row.mix != nullptr) {
    bke::node_remove_node(&bmain, tree, *row.mix, true);
  }
}

/**
 * Remove \a channel's top-level rows until \a keep_num of them are left; \a keep_num == 0 takes
 * the whole chain down, leaving the channel unwired for a fresh migration.
 */
bool realign_channel_trim_to(Main &bmain,
                             Material &ma,
                             const int channel,
                             const int64_t keep_num,
                             PaintMaterialLayerEditError &r_error)
{
  for (int pass = 0; pass < 64; pass++) {
    const bNodeSocket *terminal = paint_material_channel_socket_find(ma, channel);
    if (terminal == nullptr) {
      return true;
    }
    ma.nodetree->ensure_topology_cache();
    ChannelChain chain;
    chain.channel = channel;
    if (!chain_collect(*ma.nodetree, *const_cast<bNodeSocket *>(terminal), chain, r_error)) {
      /* An unlinked terminal reads as ChainNotPlain: the channel is fully unwired, which is
       * where a trim to zero was headed anyway. Anything else is a chain this cannot rewrite. */
      return keep_num == 0;
    }
    if (int64_t(chain.layers.size()) <= keep_num) {
      return true;
    }
    /* The top row goes: whatever read it -- the terminal, since only flat top-level chains are
     * realigned -- reads the row under it instead, then the row's own nodes come off. */
    bNodeTree &tree = *chain.tree;
    if (chain.layers.size() >= 2) {
      ChainLayer &below = chain.layers[chain.layers.size() - 2];
      relink_into(tree, *chain.terminal, *chain.terminal_node, *below.node, *below.output);
    }
    const RealignTopRowNodes row = realign_top_row_resolve(tree, chain.layers.last());
    realign_top_row_remove(bmain, tree, row);
    BKE_ntree_update_after_single_tree_change(bmain, tree);
  }
  r_error = PaintMaterialLayerEditError::ChainNotPlain;
  return false;
}

}  // namespace

bool BKE_paint_material_layer_channels_realign(Main &bmain,
                                               Material &ma,
                                               PaintMaterialLayerEditError *r_error)
{
  auto fail = [&](const PaintMaterialLayerEditError error) {
    if (r_error != nullptr) {
      *r_error = error;
    }
    return false;
  };
  auto succeed = [&]() {
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    return true;
  };

  if (ma.nodetree == nullptr) {
    return fail(PaintMaterialLayerEditError::NotAStack);
  }
  Vector<Vector<ChannelChain>> forest;
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  if (!chains_collect_forest(ma, forest, error)) {
    return fail(error);
  }
  if (forest.is_empty()) {
    return succeed();
  }
  /* Groups hold sub-chains of their own; matching those up is not something this repairs. */
  for (const Vector<ChannelChain> &per_channel : forest) {
    if (per_channel.size() > 1) {
      return fail(PaintMaterialLayerEditError::HasGroups);
    }
  }
  /* The channel the stack UI draws its rows from is the contract; every other wired channel is
   * brought to it -- extra rows come off the top, missing ones are mirrored in, the same
   * migration an unwired channel gets. */
  const int64_t ref_num = forest.first().last().layers.size();
  bool changed = false;
  for (const int64_t ci : forest.index_range().drop_front(1)) {
    const ChannelChain &chain = forest[ci].last();
    const int64_t cur_num = chain.layers.size();
    if (cur_num == ref_num) {
      continue;
    }
    changed = true;
    if (cur_num > ref_num) {
      if (!realign_channel_trim_to(bmain, ma, chain.channel, ref_num, error)) {
        return fail(error);
      }
      continue;
    }
    /* Fewer rows than the reference: the channel's own shape is not worth keeping -- it does not
     * match the stack the UI draws anyway. It comes down and the reference is mirrored into it. */
    if (!realign_channel_trim_to(bmain, ma, chain.channel, 0, error) ||
        !ensure_migrate_channel(bmain, ma, chain.channel, forest.first(), error))
    {
      return fail(error);
    }
  }
  /* The rows are level now; their corrections may not be -- a correction taken out of one channel
   * by hand, say. Every channel carries the reference's correction rows (spec 18 §4.5), the same
   * mirror a migrated channel gets; agreeing sections are left, and their per-channel maps and
   * mute states with them. */
  if (!chains_collect_forest(ma, forest, error)) {
    return fail(error);
  }
  for (const int64_t ci : forest.index_range().drop_front(1)) {
    const ChannelChain &ref_chain = forest.first().last();
    ChannelChain &chain = forest[ci].last();
    if (chain.layers.size() != ref_chain.layers.size()) {
      /* The rows themselves failed to level above; nothing to mirror corrections onto. */
      continue;
    }
    Vector<EnsureCreated> scratch_created;
    if (!ensure_corrections_mirror(bmain, ref_chain, chain, scratch_created, changed, error)) {
      return fail(error);
    }
  }
  if (changed) {
    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
    paint_layer_edit_committed(bmain, ma, true);
  }
  return succeed();
}

/**
 * Path E: the first layer of an empty material, built directly as one normalized Mix row
 * per baked channel -- never as bare images, which carry no marker and would leave the row
 * without one identity across its channels.
 *
 * Takes \a baked_maps over like #BKE_paint_material_layer_add does: shown maps keep their
 * fresh user on the node, and every other map -- all of them on refusal -- is freed.
 */
bool BKE_paint_material_layer_add_material_base(Main &bmain,
                                                 Material &ma,
                                                 Span<PaintMaterialLayerChannelImage> baked_maps,
                                                 const PaintMaterialLayerKind kind,
                                                 int *r_ordinal,
                                                 PaintMaterialLayerEditError *r_error)
{
  Set<Image *> given_used;
  auto given_release = [&]() {
    Set<Image *> released;
    for (const PaintMaterialLayerChannelImage &given : baked_maps) {
      if (given.image != nullptr && !given_used.contains(given.image) &&
          released.add(given.image))
      {
        BKE_id_free(&bmain, given.image);
      }
    }
  };
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    given_used.clear();
    given_release();
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };

  if (baked_maps.is_empty()) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id) || ma.nodetree == nullptr ||
      !ID_IS_EDITABLE(&ma.nodetree->id) || ID_IS_OVERRIDE_LIBRARY(ma.nodetree))
  {
    return fail(PaintMaterialLayerEditError::NotEditable);
  }
  /* Path E is only for an empty canvas: a stack already there takes the ensure+add route. */
  {
    Vector<Vector<ChannelChain>> existing;
    PaintMaterialLayerEditError existing_error = PaintMaterialLayerEditError::None;
    if (chains_collect_forest(ma, existing, existing_error)) {
      return fail(PaintMaterialLayerEditError::ChannelsDisagree);
    }
    if (existing_error != PaintMaterialLayerEditError::NotAStack) {
      return fail(existing_error);
    }
  }
  ChannelUnavailableReason principled_reason = ChannelUnavailableReason::None;
  if (BKE_paint_material_principled_find(ma, principled_reason) == nullptr) {
    return fail(PaintMaterialLayerEditError::NoPrincipled);
  }
  /* Validate everything up front: channel ids, duplicates, and free Principled inputs. A
   * linked input here is someone's graph, not an empty canvas -- same policy as ensure. */
  Set<int> seen_channels;
  for (const PaintMaterialLayerChannelImage &given : baked_maps) {
    if (given.image == nullptr || given.channel < 0 ||
        given.channel >= PAINT_MATERIAL_CHANNEL_NUM ||
        BKE_paint_material_channel_info(eMaterialPaintChannel(given.channel)).socket_name ==
            nullptr)
    {
      return fail(PaintMaterialLayerEditError::IndexOutOfRange);
    }
    if (!seen_channels.add(given.channel)) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    if (ensure_principled_input_is_linked(ma, given.channel)) {
      return fail(PaintMaterialLayerEditError::ChannelHasUnsupportedSource);
    }
  }

  bNodeTree &tree = *ma.nodetree;
  if (tree.typeinfo == nullptr || tree.typeinfo->group_idname == nullptr) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }

  /* One identity for the whole row, restamped onto the baked maps the way layer_add does:
   * the bake's own layer id only ever lived in the bake link. */
  const bUUID layer_id = BLI_uuid_generate_random();

  struct BaseRow {
    int channel = -1;
    bNode *tex = nullptr;
    bNode *mix = nullptr;
  };
  Vector<BaseRow> rows;
  Vector<EnsureCreated> created;
  auto fail_nodes = [&](const PaintMaterialLayerEditError reason) {
    Vector<Image *> owned;
    ensure_discard(bmain, created, owned);
    return fail(reason);
  };

  /* Normal starts one node earlier, at its own Normal Map (same as ensure's primary). */
  bNode *normal_map = nullptr;
  for (const PaintMaterialLayerChannelImage &given : baked_maps) {
    if (given.channel != PAINT_MATERIAL_CHANNEL_NORMAL) {
      continue;
    }
    normal_map = bke::node_add_static_node(nullptr, tree, SH_NODE_NORMAL_MAP);
    if (normal_map == nullptr) {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    created.append({&tree, normal_map, EnsureNodeKind::Plain});
    bNodeSocket *map_color = bke::node_find_socket(*normal_map, SOCK_IN, "Color"_ustr);
    bNodeSocket *map_normal = bke::node_find_socket(*normal_map, SOCK_OUT, "Normal"_ustr);
    bNodeSocket *principled_normal = ensure_principled_input_find(ma, given.channel);
    if (map_color == nullptr || map_normal == nullptr || principled_normal == nullptr) {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    bNode &principled = const_cast<bNode &>(principled_normal->owner_node());
    bke::node_add_link(tree, *normal_map, *map_normal, principled, *principled_normal);
    bke::node_position_relative(*normal_map, principled, map_normal, *principled_normal);
  }

  for (const PaintMaterialLayerChannelImage &given : baked_maps) {
    BaseRow row;
    row.channel = given.channel;
    row.tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
    if (row.tex == nullptr) {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    row.tex->id = &given.image->id;
    given_used.add(given.image);
    created.append({&tree, row.tex, EnsureNodeKind::Texture});
    row.mix = layer_mix_node_create(bmain, tree, given.channel);
    if (row.mix == nullptr) {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    created.append({&tree, row.mix, EnsureNodeKind::Plain});
    BKE_paint_material_layer_marker_set(*row.mix, layer_id);
    if (kind != PaintMaterialLayerKind::Paint) {
      BKE_paint_material_layer_kind_set(*row.mix, kind);
    }
    given.image->paint_layer_id = layer_id;
    /* A baked map goes on to be a layer canvas like any map #layer_image_create makes; the ID
     * browser's paint-canvas view keys off this flag together with #paint_layer_id. */
    given.image->flag |= IMA_PAINT_CANVAS;
    rows.append(row);
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);

  for (BaseRow &row : rows) {
    /* The previous iteration wired links and added a coverage Multiply node; rebuild the
     * topology cache before #composite_mix_node_read walks the Factor's links again. */
    tree.ensure_topology_cache();

    CompositeMixNode mix_prm;
    bNodeSocket *output = mix_output_find(*row.mix);
    bNodeSocket *tex_color = bke::node_find_socket(*row.tex, SOCK_OUT, "Color"_ustr);
    bNodeSocket *tex_alpha = bke::node_find_socket(*row.tex, SOCK_OUT, "Alpha"_ustr);
    if (output == nullptr || tex_color == nullptr || tex_alpha == nullptr ||
        !composite_mix_node_read(*row.mix, mix_prm))
    {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    /* #CompositeMixNode hands out const sockets; links need mutable ones, like the layer
     * structs everywhere else in this file. */
    bNodeSocket *mix_top = const_cast<bNodeSocket *>(mix_prm.top);
    bNodeSocket *mix_factor = const_cast<bNodeSocket *>(mix_prm.factor);
    bNodeSocket *terminal = nullptr;
    bNode *terminal_node = nullptr;
    if (row.channel == PAINT_MATERIAL_CHANNEL_NORMAL) {
      if (normal_map == nullptr) {
        return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
      }
      terminal = bke::node_find_socket(*normal_map, SOCK_IN, "Color"_ustr);
      terminal_node = normal_map;
    }
    else {
      terminal = ensure_principled_input_find(ma, row.channel);
      terminal_node = (terminal == nullptr) ?
                          nullptr :
                          &const_cast<bNode &>(terminal->owner_node());
    }
    if (terminal == nullptr || terminal_node == nullptr) {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    bke::node_add_link(tree, *row.tex, *tex_color, *row.mix, *mix_top);
    if (!layer_factor_coverage_link(
            tree, *row.mix, *mix_factor, *row.tex, *tex_alpha, 1.0f))
    {
      return fail_nodes(PaintMaterialLayerEditError::CreationFailed);
    }
    bke::node_position_relative(*row.tex, *row.mix, tex_color, *mix_top);
    bke::node_position_relative(*row.mix, *terminal_node, output, *terminal);
    relink_into(tree, *terminal, *terminal_node, *row.mix, *output);
  }

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  paint_layer_edit_committed(bmain, ma, true);
  given_release();
  if (r_ordinal != nullptr) {
    *r_ordinal = 0;
  }
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

Image *BKE_paint_material_layer_neutral_image_create(Main &bmain,
                                                     const int channel,
                                                     const int size)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM || size <= 0) {
    return nullptr;
  }
  return ensure_neutral_image_create(bmain, channel, size, size, false, nullptr);
}

bool BKE_paint_material_layer_channel_image_set(Main &bmain,
                                                Material &ma,
                                                const int ordinal,
                                                const int channel,
                                                Image &image,
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

  /* 1. Preflight: the row exists and the material is writable, decided without writing a byte.
   * A bare base is accepted -- see the plan's case for why this operation is the one exception. */
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::ChannelImageSet, plan, error)) {
    /* Channels that have drifted out of step refuse every edit; bringing them back to the row
     * structure the UI draws is what lets the assignment through. */
    if (error != PaintMaterialLayerEditError::ChannelsDisagree ||
        !BKE_paint_material_layer_channels_realign(bmain, ma, &error) ||
        !layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::ChannelImageSet, plan, error))
    {
      return fail(error);
    }
  }
  /* 2. Shape: nothing to convert, and #plan.needs_bottom_normalize is ignored on purpose -- a
   * bare base takes the image as its own map, a Mix layer takes it through its existing top
   * socket. The chain of the one channel asked for is what gets touched. */
  ChannelChain *target_chain = nullptr;
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel == channel) {
      target_chain = chain;
      break;
    }
  }
  if (target_chain == nullptr) {
    /* A material may wire Base Color and leave Roughness constant: a channel with no stack has
     * no map of this layer for the image to land in. */
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }
  /* 3. Mutation: from here, a refusal is impossible. */
  ChainLayer &layer = target_chain->layers[plan.layer_index];
  bNodeTree &tree = *target_chain->tree;

  /* The map node owns exactly one user of the image it shows: gaining one costs the replacement,
   * and what the replacement orphans -- a generated blank nobody else holds -- is freed at once
   * instead of lingering in the file until a purge. */
  auto orphan_check = [&bmain](Image *previous) {
    if (previous != nullptr) {
      id_us_min(&previous->id);
      if (previous->id.us == 0 && previous->source == IMA_SRC_GENERATED) {
        BKE_id_free(&bmain, previous);
      }
    }
  };

  if (!layer.is_mix()) {
    /* The bare base is the channel's own Image Texture, so the assignment is that node's image.
     * It carries no layer marker, and the image it takes loses any stale one with it: an image
     * tagged as some other layer's map must not stay tagged once it becomes the base. */
    if (layer.node->id == nullptr || GS(layer.node->id->name) != ID_IM) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    Image *previous = id_cast<Image *>(layer.node->id);
    layer.node->id = &image.id;
    if (previous != &image) {
      id_us_plus(&image.id);
      orphan_check(previous);
    }
    image.paint_layer_id = bUUID{};
  }
  else {
    tree.ensure_topology_cache();
    CompositeMixNode mix;
    if (!composite_mix_node_read(*layer.node, mix) || mix.top == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    const bUUID layer_marker = BKE_paint_material_layer_marker_get(*layer.node);
    /* The map this layer already shows, when it has one: a single Image Texture feeding the map
     * input and tagged as this layer's. Reusing it keeps the node the user may have moved or
     * renamed; anything else feeding the input is replaced by a fresh map node. */
    bNodeSocket &top = const_cast<bNodeSocket &>(*mix.top);
    bNodeLink *map_link = sole_link_into(top);
    bNode *map_node = (map_link != nullptr) ? map_link->fromnode : nullptr;
    bool reuse = (map_node != nullptr) && map_node->type_legacy == SH_NODE_TEX_IMAGE &&
                 (map_node->id == nullptr ||
                  (GS(map_node->id->name) == ID_IM &&
                   BLI_uuid_equal(id_cast<Image *>(map_node->id)->paint_layer_id, layer_marker)));
    if (reuse) {
      Image *previous = (map_node->id != nullptr && GS(map_node->id->name) == ID_IM) ?
                            id_cast<Image *>(map_node->id) :
                            nullptr;
      map_node->id = &image.id;
      if (previous != &image) {
        /* The node's user moves from the old image to the new one. */
        id_us_plus(&image.id);
        orphan_check(previous);
      }
    }
    else {
      bNode *tex = bke::node_add_static_node(nullptr, tree, SH_NODE_TEX_IMAGE);
      bNodeSocket *color = bke::node_find_socket(*tex, SOCK_OUT, "Color"_ustr);
      if (color == nullptr) {
        bke::node_remove_node(&bmain, tree, *tex, false);
        return fail(PaintMaterialLayerEditError::CreationFailed);
      }
      tex->id = &image.id;
      id_us_plus(&image.id);
      bke::node_position_relative(*tex, *layer.node, nullptr, *const_cast<bNodeSocket *>(mix.top));
      if (map_link != nullptr) {
        BKE_ntree_update_tag_link_removed(&tree);
        bke::node_remove_link(&tree, *map_link);
      }
      bke::node_add_link(tree, *tex, *color, *layer.node, *const_cast<bNodeSocket *>(mix.top));
    }
    image.paint_layer_id = layer_marker;
  }
  image.paint_layer_channel = channel;

  BKE_ntree_update_after_single_tree_change(bmain, tree);
  /* The map may sit in a folder's own node tree, whose evaluated copy is separate from the
   * material's; see #paint_layer_edit_committed for why it has to be refreshed. */
  if (&tree != ma.nodetree) {
    DEG_id_tag_update(&tree.id, ID_RECALC_SYNC_TO_EVAL);
  }
  paint_layer_edit_committed(bmain, ma, true);
  if (r_error != nullptr) {
    *r_error = PaintMaterialLayerEditError::None;
  }
  return true;
}

namespace {

/** The Image Texture feeding \a layer's map input, when that is what feeds it. */
bNode *layer_map_node(const CompositeMixNode &mix)
{
  return const_cast<bNode *>(composite_mix_map_node(mix));
}

/**
 * State of one row in one chain as the graph has it (I2), through the rule every reader shares
 * (#composite_mix_channel_state_get). False for a folder or a row that is not the per-channel
 * shape: it is none of the three states and is left alone.
 */
bool layer_channel_state_read(const ChainLayer &layer,
                              CompositeMixNode &r_mix,
                              PaintMaterialLayerChannelState &r_state)
{
  if (!layer.is_mix() || layer.is_group || !composite_mix_node_read(*layer.node, r_mix)) {
    return false;
  }
  return composite_mix_channel_state_get(r_mix, r_state);
}

/** The mask Image Texture of the layer carrying \a marker, in \a tree, or null. */
bNode *layer_mask_node_find(bNodeTree &tree, const bUUID &marker)
{
  for (bNode &node : tree.nodes) {
    if (node.type_legacy != SH_NODE_TEX_IMAGE || node.id == nullptr || GS(node.id->name) != ID_IM)
    {
      continue;
    }
    const Image &image = *id_cast<const Image *>(node.id);
    if (image.paint_layer_channel == PAINT_LAYER_MAP_MASK &&
        BLI_uuid_equal(image.paint_layer_id, marker))
    {
      return &node;
    }
  }
  return nullptr;
}

/** Coverage off in one channel (I1): no link, and an explicit zero rather than Math's 0.5. */
void layer_coverage_clear(bNodeTree &tree, const CompositeMixNode &mix)
{
  bNodeSocket &coverage = const_cast<bNodeSocket &>(*mix.factor_coverage);
  for (bNodeLink *link : Vector<bNodeLink *>(coverage.directly_linked_links())) {
    BKE_ntree_update_tag_link_removed(&tree);
    bke::node_remove_link(&tree, *link);
  }
  coverage.default_value_typed<bNodeSocketValueFloat>()->value = 0.0f;
  BKE_ntree_update_tag_socket_property(&tree, &coverage);
}

/** Coverage back on: the layer's mask when it has one, otherwise its own map's alpha. */
void layer_coverage_restore(bNodeTree &tree,
                            const ChainLayer &layer,
                            const CompositeMixNode &mix,
                            bNode &map)
{
  bNodeSocket &coverage = const_cast<bNodeSocket &>(*mix.factor_coverage);
  bNode &multiply = const_cast<bNode &>(coverage.owner_node());
  bNode *mask = layer_mask_node_find(tree, BKE_paint_material_layer_marker_get(*layer.node));
  bNode &source = (mask != nullptr) ? *mask : map;
  bNodeSocket *output = bke::node_find_socket(
      source, SOCK_OUT, (mask != nullptr) ? "Color"_ustr : "Alpha"_ustr);
  if (output == nullptr) {
    /* Both are Image Texture nodes, which always have these outputs. */
    BLI_assert_unreachable();
    return;
  }
  relink_into(tree, coverage, multiply, source, *output);
}

/**
 * Coverage back on for a row whose base map just came on, when nothing covers it: the mask image
 * when the row has one, otherwise what the row's content corrections accumulate over the map, and
 * the map's own alpha for a row without any. A row with mask corrections is left alone -- their
 * chain owns the coverage input, and #layer_mask_corrections_sync gives it its base.
 */
void layer_coverage_reconnect(bNodeTree &tree,
                              const ChainLayer &layer,
                              const CompositeMixNode &mix,
                              bNode &map)
{
  if (!layer.mask_corrections.is_empty()) {
    return;
  }
  tree.ensure_topology_cache();
  bNodeSocket &coverage = const_cast<bNodeSocket &>(*mix.factor_coverage);
  if (socket_has_link(coverage)) {
    return;
  }
  const bool has_mask = layer_mask_node_find(tree, BKE_paint_material_layer_marker_get(*layer.node)) !=
                        nullptr;
  if (!has_mask && !layer.content_corrections.is_empty() &&
      layer.content_corrections.last().over_combine != nullptr)
  {
    bNode &over = *layer.content_corrections.last().over_combine;
    if (bNodeSocket *over_out = static_cast<bNodeSocket *>(over.outputs.first)) {
      relink_into(tree, coverage, coverage.owner_node(), over, *over_out);
    }
    return;
  }
  layer_coverage_restore(tree, layer, mix, map);
}

}  // namespace

void channel_map_mute_set(bNodeTree &tree, bNode &map, const bool enable)
{
  /* The muted map only spares the sampler; the coverage input's explicit zero -- the caller's
   * clear -- is what makes the channel contribute nothing (I1). */
  SET_FLAG_FROM_TEST(map.flag, !enable, NODE_MUTED);
  BKE_ntree_update_tag_node_mute(&tree, &map);
}

Image *correction_tagged_map_find(Main &bmain, const bUUID &marker, const int channel)
{
  for (Image &image : bmain.images) {
    if (image.paint_layer_channel != channel) {
      continue;
    }
    if (BLI_uuid_equal(image.paint_layer_id, marker)) {
      return &image;
    }
  }
  return nullptr;
}

void BKE_paint_material_layer_channel_states_get(
    Main &bmain,
    Material &ma,
    const int ordinal,
    MutableSpan<PaintMaterialLayerChannelState> r_states)
{
  BLI_assert(r_states.size() >= PAINT_MATERIAL_CHANNEL_NUM);
  r_states.fill(PaintMaterialLayerChannelState::Absent);
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::SetEnabled, plan, error)) {
    return;
  }
  for (ChannelChain *chain : plan.chains) {
    if (chain->channel < 0 || chain->channel >= r_states.size()) {
      continue;
    }
    CompositeMixNode mix;
    PaintMaterialLayerChannelState state = PaintMaterialLayerChannelState::Absent;
    chain->tree->ensure_topology_cache();
    if (layer_channel_state_read(chain->layers[plan.layer_index], mix, state)) {
      r_states[chain->channel] = state;
    }
  }
}

PaintMaterialLayerChannelState BKE_paint_material_layer_channel_state_get(Main &bmain,
                                                                          Material &ma,
                                                                          const int ordinal,
                                                                          const int channel)
{
  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM) {
    return PaintMaterialLayerChannelState::Absent;
  }
  PaintMaterialLayerChannelState states[PAINT_MATERIAL_CHANNEL_NUM];
  BKE_paint_material_layer_channel_states_get(
      bmain, ma, ordinal, MutableSpan(states, PAINT_MATERIAL_CHANNEL_NUM));
  return states[channel];
}

bool BKE_paint_material_layer_channel_enabled_set(Main &bmain,
                                                  Material &ma,
                                                  const int ordinal,
                                                  const int channel,
                                                  const bool enable,
                                                  Image *new_map,
                                                  PaintMaterialLayerEditError *r_error)
{
  Image *owned_map = new_map;
  auto release_map = [&]() {
    if (owned_map != nullptr) {
      BKE_id_free(&bmain, owned_map);
      owned_map = nullptr;
    }
  };
  auto fail = [&](const PaintMaterialLayerEditError reason) {
    release_map();
    if (r_error != nullptr) {
      *r_error = reason;
    }
    return false;
  };
  auto succeed = [&]() {
    if (r_error != nullptr) {
      *r_error = PaintMaterialLayerEditError::None;
    }
    return true;
  };

  if (channel < 0 || channel >= PAINT_MATERIAL_CHANNEL_NUM ||
      BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).socket_name == nullptr)
  {
    return fail(PaintMaterialLayerEditError::IndexOutOfRange);
  }

  /* 1. Preflight, without writing a byte. */
  PaintMaterialLayerEditError error = PaintMaterialLayerEditError::None;
  LayerEditPlan plan;
  if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::SetEnabled, plan, error)) {
    return fail(error);
  }
  ChainLayer &any_row = plan.chains.first()->layers[plan.layer_index];
  if (any_row.is_group) {
    return fail(PaintMaterialLayerEditError::ChannelNotToggleable);
  }
  ChannelChain *chain = nullptr;
  for (ChannelChain *candidate : plan.chains) {
    if (candidate->channel == channel) {
      chain = candidate;
    }
  }
  PaintMaterialLayerChannelState state = PaintMaterialLayerChannelState::Absent;
  CompositeMixNode mix;
  if (chain != nullptr) {
    chain->tree->ensure_topology_cache();
    if (!layer_channel_state_read(chain->layers[plan.layer_index], mix, state)) {
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }
  }
  const bool is_on = state == PaintMaterialLayerChannelState::Enabled;
  if (is_on == enable) {
    release_map();
    return succeed();
  }
  const PaintMaterialLayerKind kind = BKE_paint_material_layer_kind_get(*any_row.node);
  if (!enable) {
    /* The active layer is found through the channels it paints (spec 18 I2'): its base's enabled
     * ones, and the ones its corrections keep painting while the base is Absent or Disabled
     * there. The toggle only touches the base, so what is left when it is off is exactly what
     * the corrections still bring -- a row whose correction paints the channel being switched
     * off keeps painting it, and stays findable either way. */
    bool paints_somewhere = false;
    for (ChannelChain *other : plan.chains) {
      other->tree->ensure_topology_cache();
      CompositeMixNode other_mix;
      PaintMaterialLayerChannelState other_state = PaintMaterialLayerChannelState::Absent;
      const bool base_on = layer_channel_state_read(other->layers[plan.layer_index], other_mix,
                                                    other_state) &&
                           other_state == PaintMaterialLayerChannelState::Enabled &&
                           other->channel != channel;
      if (base_on || row_channel_painted_by_corrections(other->layers[plan.layer_index])) {
        paints_somewhere = true;
        break;
      }
    }
    if (!paints_somewhere) {
      return fail(PaintMaterialLayerEditError::LastEnabledChannel);
    }
  }

  bNodeTree &row_tree = *plan.chains.first()->tree;

  /* 2. Enabled -> Disabled, Disabled -> Enabled: links and a mute flag, nothing to create. */
  if (chain != nullptr && state != PaintMaterialLayerChannelState::Absent) {
    release_map();
    bNodeTree &tree = *chain->tree;
    /* The row's own map sits directly on its map input -- or below the content corrections that
     * hang there (spec 18 I1'); it is the node a switched-off channel mutes. */
    bNode *row_map = layer_map_node(mix);
    if (row_map == nullptr) {
      row_map = chain->layers[plan.layer_index].base_map;
    }
    if (row_map == nullptr) {
      return fail(PaintMaterialLayerEditError::ChainNotPlain);
    }
    bNode &map = *row_map;
    if (enable) {
      channel_map_mute_set(tree, map, true);
      /* The coverage input keeps whatever covered the row before -- a content correction's
       * accumulated coverage when corrections hang on it. Only a row whose coverage was taken
       * away reads its mask, or its own map's alpha, back. */
      layer_coverage_reconnect(tree, chain->layers[plan.layer_index], mix, map);
    }
    else {
      /* The row's corrections keep painting this channel when its base goes off (spec 18 I2'):
       * the coverage they accumulate stays on the row's Multiply, and the muted map reads as an
       * absent base under them. With nothing of the row's own left painting, the coverage goes
       * back to the explicit-zero form (I1) -- on the coverage input itself, unless mask
       * corrections own it: their chain stays linked, and the sync below zeroes it instead. */
      const ChainLayer &layer = chain->layers[plan.layer_index];
      if (!row_channel_painted_by_corrections(layer) && layer.mask_corrections.is_empty()) {
        layer_coverage_clear(tree, mix);
      }
      channel_map_mute_set(tree, map, false);
    }
    /* The mask corrections follow what the row now puts into the channel (spec 18 §4.3). */
    layer_mask_corrections_sync(tree, chain->layers[plan.layer_index]);
    BKE_ntree_update_after_single_tree_change(bmain, tree);
    if (&tree != ma.nodetree) {
      DEG_id_tag_update(&tree.id, ID_RECALC_SYNC_TO_EVAL);
    }
    paint_layer_edit_committed(bmain, ma, true);
    return succeed();
  }

  /* 3. Absent -> Enabled. 3.1: the map and its node before anything else, unlinked. */
  const bUUID marker = BKE_paint_material_layer_marker_get(*any_row.node);
  if (owned_map == nullptr) {
    PaintMaterialLayerAddParams params;
    params.kind = kind;
    BKE_paint_material_layer_fill_color_get(*any_row.node, params.fill_color);
    int size_x = 1024, size_y = 1024;
    for (ChannelChain *other : plan.chains) {
      if (other->layers[plan.layer_index].image != nullptr) {
        ensure_ref_image_size(other->layers[plan.layer_index].image, size_x, size_y);
        break;
      }
    }
    params.image_size = size_x;
    owned_map = layer_image_create(bmain, channel, params);
    if (owned_map == nullptr) {
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
  }
  owned_map->flag |= IMA_PAINT_CANVAS;
  owned_map->paint_layer_id = marker;
  owned_map->paint_layer_channel = channel;
  bNode *map = bke::node_add_static_node(nullptr, row_tree, SH_NODE_TEX_IMAGE);
  if (map == nullptr) {
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  /* Resolved now, while a missing socket can still refuse without leaving anything behind. */
  bNodeSocket *map_color = bke::node_find_socket(*map, SOCK_OUT, "Color"_ustr);
  if (map_color == nullptr) {
    bke::node_remove_node(&bmain, row_tree, *map, false);
    return fail(PaintMaterialLayerEditError::CreationFailed);
  }
  map->id = &owned_map->id;

  /* 3.2: the channel's chain, when the stack has none yet. */
  if (chain == nullptr) {
    const int one[1] = {channel};
    if (!BKE_paint_material_layer_channels_ensure(bmain, ma, Span<int>(one, 1), &error)) {
      map->id = nullptr;
      bke::node_remove_node(&bmain, row_tree, *map, false);
      return fail(error);
    }
    /* 3.3: read again; the chain was built by the reader's own contract, so this cannot refuse. */
    plan = LayerEditPlan();
    if (!layer_edit_plan_build(bmain, ma, ordinal, LayerEditOp::SetEnabled, plan, error)) {
      BLI_assert_unreachable();
      return fail(error);
    }
    for (ChannelChain *candidate : plan.chains) {
      if (candidate->channel == channel) {
        chain = candidate;
      }
    }
    if (chain == nullptr) {
      BLI_assert_unreachable();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
    chain->tree->ensure_topology_cache();
    if (!layer_channel_state_read(chain->layers[plan.layer_index], mix, state)) {
      BLI_assert_unreachable();
      return fail(PaintMaterialLayerEditError::CreationFailed);
    }
  }

  /* 3.4: links only; nothing below can refuse. The map now belongs to the node. */
  owned_map = nullptr;
  bNodeTree &tree = *chain->tree;
  ChainLayer &layer = chain->layers[plan.layer_index];
  if (layer.content_corrections.is_empty()) {
    bke::node_add_link(tree, *map, *map_color, *layer.node, const_cast<bNodeSocket &>(*mix.top));
    bke::node_position_relative(*map, *layer.node, nullptr, const_cast<bNodeSocket &>(*mix.top));
  }
  else {
    /* Under content corrections the map input is theirs: the new map is the base of their stack,
     * feeding the lowest correction's base and, by its alpha, the lowest over pair's `a_below`. A
     * second link into the row's map input would be a shape nothing reads. */
    const ChainCorrection &lowest = layer.content_corrections.first();
    tree.ensure_topology_cache();
    CompositeMixNode lowest_mix;
    bNodeSocket *map_alpha = bke::node_find_socket(*map, SOCK_OUT, "Alpha"_ustr);
    if (lowest.mix != nullptr && map_alpha != nullptr &&
        composite_mix_node_read(*lowest.mix, lowest_mix) && lowest_mix.bottom != nullptr)
    {
      bNodeSocket &base = const_cast<bNodeSocket &>(*lowest_mix.bottom);
      relink_into(tree, base, *lowest.mix, *map, *map_color);
      bke::node_position_relative(*map, *lowest.mix, map_color, base);
      for (const std::pair<bNode *, int> &input : {std::pair<bNode *, int>{lowest.over_invert, 1},
                                                   std::pair<bNode *, int>{lowest.over_combine, 2}})
      {
        if (input.first == nullptr) {
          continue;
        }
        if (bNodeSocket *socket = static_cast<bNodeSocket *>(
                BLI_findlink(&input.first->inputs, input.second)))
        {
          relink_into(tree, *socket, *input.first, *map, *map_alpha);
        }
      }
    }
  }
  layer.base_map = map;
  layer_coverage_reconnect(tree, layer, mix, *map);
  layer_mask_corrections_sync(tree, layer);

  /* 3.5 */
  chain_rebuild_links(*chain);
  BKE_ntree_update_after_single_tree_change(bmain, tree);
  if (&tree != ma.nodetree) {
    DEG_id_tag_update(&tree.id, ID_RECALC_SYNC_TO_EVAL);
  }
  paint_layer_edit_committed(bmain, ma, true);
  return succeed();
}

}  // namespace blender
