/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The conversion of a material from its own node graph, and the way back: the author's Principled
 * links are recorded before the conversion re-aims them, the release hands the stack's own wiring
 * back, and the revert takes the generated tree apart and leaves the plain material. Shared with
 * the generator through #paint_layers_generate_intern.hh.
 */

#include <algorithm>
#include <string>

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_idprop.hh"
#include "BKE_image.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_edit.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_layers_source_graph.hh"
#include "BKE_paint_material_resolve.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_intern.hh"

namespace blender::bke::paint_layers {

/* -------------------------------------------------------------------- */
/** \name The author's original Principled links
 *
 * A conversion re-aims the links that fed the Principled into the stack, and a bake replaces them
 * with the stack's result. Neither may destroy the graph: the links are recorded on the material's
 * node tree before the conversion, so a later revert can put them back even after save and load.
 * The record is deliberately plain ID properties rather than DNA: it is private bookkeeping, and a
 * field would have to be versioned for a feature only this generator reads. The source node is
 * remembered by `bNode::identifier`, not by name: the user renames nodes freely, and the
 * identifier is what stays put.
 * \{ */

struct PaintLayersOriginalLink {
  int from_node_id;
  std::string from_socket;
  std::string to_socket;
};

inline constexpr const char *ORIGINAL_LINKS_PROP = "pbr_paint_layers_original_links";

static IDProperty *original_links_group_get(const bNodeTree &tree)
{
  const IDProperty *root = IDP_GetProperties(const_cast<ID *>(&tree.id));
  if (root == nullptr) {
    return nullptr;
  }
  return IDP_GetPropertyTypeFromGroup(root, ORIGINAL_LINKS_PROP, IDP_GROUP);
}

static bool original_links_present(const Material &ma)
{
  return ma.nodetree != nullptr && original_links_group_get(*ma.nodetree) != nullptr;
}

static void original_links_clear(bNodeTree &tree)
{
  IDProperty *root = IDP_GetProperties(&tree.id);
  if (root == nullptr) {
    return;
  }
  if (IDProperty *group = original_links_group_get(tree)) {
    IDP_FreeFromGroup(root, group);
  }
}

static void original_links_store(bNodeTree &tree, const Span<PaintLayersOriginalLink> links)
{
  IDProperty *root = IDP_EnsureProperties(&tree.id);
  original_links_clear(tree);
  IDPropertyTemplate template_value = {0};
  IDProperty *group = IDP_New(IDP_GROUP, &template_value, ORIGINAL_LINKS_PROP);
  IDP_AddToGroup(group, IDP_NewInt(int(links.size()), "count"));
  IDP_AddToGroup(root, group);
  for (const int i : links.index_range()) {
    char key[64];
    SNPRINTF(key, "from_node_id_%d", int(i));
    IDP_AddToGroup(group, IDP_NewInt(links[i].from_node_id, key));
    SNPRINTF(key, "from_socket_%d", int(i));
    IDP_AddToGroup(group, IDP_NewString(links[i].from_socket.c_str(), key));
    SNPRINTF(key, "to_socket_%d", int(i));
    IDP_AddToGroup(group, IDP_NewString(links[i].to_socket.c_str(), key));
  }
}

static bool original_links_load(const bNodeTree &tree, Vector<PaintLayersOriginalLink> &r_links)
{
  IDProperty *group = original_links_group_get(tree);
  if (group == nullptr) {
    return false;
  }
  const int count = prop_int_get(group, "count", 0);
  for (const int i : IndexRange(count)) {
    char key[64];
    SNPRINTF(key, "from_node_id_%d", i);
    const int from_node_id = prop_int_get(group, key, 0);
    SNPRINTF(key, "from_socket_%d", i);
    const char *from_socket = prop_string_get(group, key);
    SNPRINTF(key, "to_socket_%d", i);
    const char *to_socket = prop_string_get(group, key);
    if (from_socket == nullptr || to_socket == nullptr) {
      continue;
    }
    r_links.append(PaintLayersOriginalLink{from_node_id, from_socket, to_socket});
  }
  return true;
}

static bNode *node_find_by_id(bNodeTree &tree, const int identifier)
{
  for (bNode &node : tree.nodes) {
    if (node.identifier == identifier) {
      return &node;
    }
  }
  return nullptr;
}

/**
 * Whether \a link is the wiring the stack itself made: its instance's results, or one of the
 * marked Normal Map and Bump helpers the routing placed. Only such a link a release puts back; a
 * feed the user added or re-aimed -- even on an input the stack works with -- is their own edit.
 */
static bool link_is_stack_owned(const bNodeLink &link, const bNode *instance)
{
  if (link.fromnode == nullptr) {
    return false;
  }
  if (instance != nullptr && link.fromnode == instance) {
    return true;
  }
  return node_is_generator_owned(*link.fromnode);
}

/** Whether any link that feeds \a input is the stack's own wiring. */
static bool input_is_stack_owned(bNodeTree &tree, bNodeSocket &input, const bNode *instance)
{
  tree.ensure_topology_cache();
  const Span<bNodeLink *> links = input.directly_linked_links();
  return std::any_of(links.begin(), links.end(), [&](const bNodeLink *link) {
    return link_is_stack_owned(*link, instance);
  });
}

static void source_graph_capture(Material &ma)
{
  if (ma.nodetree == nullptr) {
    return;
  }
  bNodeTree &tree = *ma.nodetree;
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  Vector<const bNode *> group_path;
  const bNode *principled = BKE_paint_material_principled_find(ma, reason, &group_path);
  if (principled == nullptr || !group_path.is_empty()) {
    original_links_clear(tree);
    return;
  }
  tree.ensure_topology_cache();
  Vector<PaintLayersOriginalLink> links;
  for (const bNodeSocket &input : principled->inputs) {
    for (const bNodeLink *link : input.directly_linked_links()) {
      if (link->fromnode == nullptr || link->fromsock == nullptr) {
        continue;
      }
      links.append(PaintLayersOriginalLink{
          link->fromnode->identifier, link->fromsock->identifier, input.identifier});
    }
  }
  original_links_store(tree, links);
}

static void source_graph_release(Material &ma, PaintLayersRevertReport *r_report)
{
  if (ma.nodetree == nullptr) {
    return;
  }
  bNodeTree &tree = *ma.nodetree;
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled_const = BKE_paint_material_principled_find(ma, reason);
  Vector<PaintLayersOriginalLink> original;
  const bool has_record = original_links_load(tree, original);
  if (principled_const != nullptr && has_record) {
    /* The record is the graph as the conversion found it. Only the inputs the stack owns -- the
     * ones its instance and helpers feed -- go back to it; an empty record is meaningful: the
     * graph drove every input by value, so an owned input is simply left unlinked, on the value
     * the conversion never took from it. */
    bNode &principled = const_cast<bNode &>(*principled_const);
    const bNode *instance = instance_find(ma, ma.paint_layers_owner_uid);
    for (bNodeSocket &input : principled.inputs) {
      if (!input_is_stack_owned(tree, input, instance)) {
        continue;
      }
      bke::node_remove_socket_links(tree, input);
      const PaintLayersOriginalLink *saved = nullptr;
      for (const PaintLayersOriginalLink &record : original) {
        if (record.to_socket == input.identifier) {
          saved = &record;
          break;
        }
      }
      if (saved == nullptr) {
        continue;
      }
      bNode *from_node = node_find_by_id(tree, saved->from_node_id);
      bNodeSocket *from_socket = nullptr;
      if (from_node != nullptr) {
        from_socket = bke::node_find_socket(
            *from_node, SOCK_OUT, UString::from_ptr_noinline(saved->from_socket.c_str()));
      }
      if (from_node != nullptr && from_socket != nullptr) {
        bke::node_add_link(tree, *from_node, *from_socket, principled, input);
      }
      else if (r_report != nullptr) {
        /* The recorded source is renamed or gone. The link is skipped, and the socket falls back
         * to its stored default value -- a conversion only moves links, so that is the value the
         * graph had before it. A stack-owned input is a channel's socket by construction -- the
         * generator wires no other -- so the report carries the channel and the caller names it. */
        for (const MaterialPaintChannelInfo &info : BKE_paint_material_channels()) {
          if (info.socket_name != nullptr && STREQ(info.socket_name, input.identifier)) {
            r_report->lost_channels.append(info.channel);
            break;
          }
        }
      }
    }
  }
  original_links_clear(tree);
  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_SOURCE_GRAPH;
  BKE_paint_layers_tag_edited(ma);
}

static bool source_graph_can_release(const Material &ma)
{
  return (ma.paint_layers_flag & MA_PAINT_LAYERS_SOURCE_GRAPH) != 0 || original_links_present(ma);
}

/** \} */

}  // namespace blender::bke::paint_layers

namespace blender {

void BKE_paint_layers_source_graph_capture(Material &ma)
{
  bke::paint_layers::source_graph_capture(ma);
}

void BKE_paint_layers_source_graph_capture_clear(Material &ma)
{
  if (ma.nodetree != nullptr) {
    bke::paint_layers::original_links_clear(*ma.nodetree);
  }
}

bool BKE_paint_layers_source_graph_can_release(const Material &ma)
{
  return bke::paint_layers::source_graph_can_release(ma);
}

void BKE_paint_layers_source_graph_release(Material &ma, PaintLayersRevertReport *r_report)
{
  bke::paint_layers::source_graph_release(ma, r_report);
}

bool BKE_paint_layers_convert_from_graph(Main &bmain,
                                         Material &ma,
                                         const PaintChannelMask channel_mask,
                                         PaintLayersConvertReport *r_report)
{
  PaintLayersConvertReport local_report;
  PaintLayersConvertReport &report = (r_report != nullptr) ? *r_report : local_report;

  if (ma.nodetree == nullptr || channel_mask == 0) {
    return false;
  }
  /* A linked or override material's trees are owned by another file: nothing here may rewrite them.
   */
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id)) {
    return false;
  }

  const PaintChannelMask previous_set = ma.paint_layers_channels;
  /* The set first, so the base row below takes part in exactly the channels the caller named. The
   * flag turns the generated tree's chain bottoms into `Source <channel>` inputs, which the
   * regeneration's wiring step feeds from whatever the graph feeds the Principled with -- a link
   * moves onto the input, a plain value becomes its default. The graph's nodes are never edited. */
  BKE_paint_layers_channel_set_mask_set(ma, channel_mask);
  BKE_paint_layers_source_graph_set(ma, true);
  BKE_paint_layers_source_graph_capture(ma);

  /* A Revert left the stack's rows asleep: the conversion re-layers the material over them instead
   * of adding a second base row, so convert -> revert -> convert always reads as one stack. The
   * records keep their markers and maps, and the regeneration re-adopts the groups they own. */
  MaterialPaintLayer *base = nullptr;
  if (BLI_listbase_is_empty(&ma.paint_layers)) {
    PaintLayerAddParams add_params;
    add_params.name = "Layer";
    base = BKE_paint_layers_add_with_policy(ma, add_params);
    if (base == nullptr) {
      /* The material reads as it was found: the set, the mode and the record are undone. */
      BKE_paint_layers_source_graph_capture_clear(ma);
      ma.paint_layers_channels = previous_set;
      BKE_paint_layers_source_graph_set(ma, false);
      return false;
    }
  }
  else {
    ma.paint_layers_flag |= MA_PAINT_LAYERED;
    /* The rows composite with the first one as the base: that is where the stack starts. */
    base = static_cast<MaterialPaintLayer *>(ma.paint_layers.first);
  }
  report.base_layer_marker = base->marker;

  /* Normal and Height of the set stay on the author's chain when the graph still feeds the
   * Principled Normal: adopting it under the stack would apply its tangent transform a second
   * time. The report carries them; the caller names them in the interface language. */
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(&ma);
  const ChannelResolution normal_resolution = resolve.channels[PAINT_MATERIAL_CHANNEL_NORMAL];
  if (normal_resolution == ChannelResolution::Image ||
      normal_resolution == ChannelResolution::Baked)
  {
    for (const eMaterialPaintChannel channel :
         {PAINT_MATERIAL_CHANNEL_NORMAL, PAINT_MATERIAL_CHANNEL_HEIGHT})
    {
      if ((channel_mask & (uint32_t(1) << int(channel))) != 0) {
        report.kept_channels.append(channel);
      }
    }
  }

  PaintLayersRegenerateReport regen;
  report.regenerate_failed = !BKE_paint_layers_regenerate(bmain, ma, &regen);
  report.adopted_source_links = regen.adopted_source_links;
  return true;
}

bool BKE_paint_layers_convert_from_bake(Main &bmain,
                                        Material &ma,
                                        Span<eMaterialPaintChannel> baked_channels,
                                        Span<Image *> baked_images,
                                        const int16_t image_size,
                                        PaintLayersConvertBakeReport *r_report)
{
  PaintLayersConvertBakeReport local_report;
  PaintLayersConvertBakeReport &report = (r_report != nullptr) ? *r_report : local_report;

  if (ma.nodetree == nullptr || baked_channels.is_empty() ||
      baked_channels.size() != baked_images.size())
  {
    report.failure = PaintLayersConvertBakeReport::Failure::NotEditable;
    for (Image *image : baked_images) {
      BKE_id_delete(&bmain, image);
    }
    return false;
  }
  /* A linked or override material's trees are owned by another file: nothing here may rewrite them.
   */
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id)) {
    report.failure = PaintLayersConvertBakeReport::Failure::NotEditable;
    for (Image *image : baked_images) {
      BKE_id_delete(&bmain, image);
    }
    return false;
  }

  PaintChannelMask baked_mask = 0;
  for (const eMaterialPaintChannel channel : baked_channels) {
    baked_mask |= uint32_t(1) << int(channel);
  }

  /* Everything below runs under one rollback: the state the conversion touches is put back, the
   * maps the bake made go (they had no owner yet), and only the row this pass added is removed. */
  const PaintChannelMask previous_set = ma.paint_layers_channels;
  const int16_t previous_size = ma.paint_layers_image_size;
  MaterialPaintLayer *added_fill = nullptr;
  auto roll_back = [&]() {
    if (added_fill != nullptr) {
      /* A reused Fill is the user's stack; only the row this pass added may go. The add gave every
       * channel of the bake a live record, so the link cannot fail for one in practice, and maps
       * already replaced on a reused row stay in the file through the fake user set on them. */
      BKE_paint_layers_remove(ma, added_fill);
    }
    for (Image *image : baked_images) {
      BKE_id_delete(&bmain, image);
    }
    BKE_paint_layers_source_graph_capture_clear(ma);
    ma.paint_layers_channels = previous_set;
    ma.paint_layers_image_size = previous_size;
  };

  /* The links that feed the Principled now are what a later Revert hands back. The bake itself
   * rendered from a copy and touched nothing, so recording here is recording the graph as the
   * conversion found it. */
  BKE_paint_layers_source_graph_capture(ma);
  if (!BKE_paint_layers_channel_set_mask_set(ma, baked_mask)) {
    report.failure = PaintLayersConvertBakeReport::Failure::ChannelSet;
    roll_back();
    return false;
  }

  /* The material's own size decides the maps the rows painted above the bake create later: a stack
   * whose rows disagree on size cannot be composited, so the Fill layer's maps and every map
   * painted over them have to be the size this bake renders at. */
  ma.paint_layers_image_size = image_size;

  /* The maps become the Fill layer's own: a bake link back to this material would ask a re-bake
   * from the generated stack the moment its tree changed, which would flatten the layers painted
   * above back into the bottom maps. */
  for (Image *image : baked_images) {
    BKE_image_material_source_clear(*image);
  }

  /* The sleeping row a previous bake filled comes back with the maps: the bake puts them where the
   * old bake sat instead of stacking a second Fill over the same data. Only the row the conversion
   * itself created answers (#BKE_paint_layers_conversion_fill_find), never a Fill the user made. */
  MaterialPaintLayer *fill = BKE_paint_layers_conversion_fill_find(ma);
  if (fill == nullptr) {
    PaintLayerAddParams add_params;
    add_params.kind = PaintLayerAddKind::Fill;
    add_params.empty = true;
    add_params.channels = baked_mask;
    add_params.name = ma.id.name + 2;
    fill = BKE_paint_layers_add_with_policy(ma, add_params);
    if (fill == nullptr) {
      report.failure = PaintLayersConvertBakeReport::Failure::FillAdd;
      roll_back();
      return false;
    }
    /* The marker a later bake reuses: the row it fills again is the one this conversion made. */
    fill->flag |= MA_PAINT_LAYER_CONVERSION_FILL;
    added_fill = fill;
  }
  else {
    /* The sleeping rows come back with the maps: the row exists, so the layered flag has to be
     * raised by hand, and the scheduled regeneration needs the edit tag the Add would have sent.
     * The records follow the new mask: the bake's channels switch on, the ones it left out switch
     * off with their maps kept for the next bake that asks for them. */
    BKE_paint_layers_conversion_fill_channels_align(ma, *fill, baked_mask);
    ma.paint_layers_flag |= MA_PAINT_LAYERED;
    BKE_paint_layers_tag_edited(ma);
  }

  for (const int i : baked_images.index_range()) {
    /* The align left every channel of the bake with a live record; the add is a no-op for one. */
    MaterialPaintLayerChannel *record = BKE_paint_layers_channel_add(
        ma, fill, baked_channels[i]);
    /* The map this one replaces would drop to zero users and be purged on the next save, which
     * would lose the earlier bake; a fake user keeps it in the file. */
    if (record != nullptr && record->image != nullptr && record->image != baked_images[i]) {
      id_fake_user_set(&record->image->id);
    }
    if (!BKE_paint_layers_channel_set_image(ma, fill, baked_channels[i], baked_images[i])) {
      report.failure = PaintLayersConvertBakeReport::Failure::MapLink;
      roll_back();
      return false;
    }
  }

  report.fill_marker = fill->marker;
  return true;
}

bool BKE_paint_layers_revert_to_plain(Main &bmain,
                                      Material &ma,
                                      PaintLayersRevertReport *r_report)
{
  PaintLayersRevertReport local_report;
  PaintLayersRevertReport &report = (r_report != nullptr) ? *r_report : local_report;

  if (!BKE_paint_layers_is_layered(ma) || ma.nodetree == nullptr) {
    return false;
  }
  /* A linked or override material's trees are owned by another file: nothing here may rewrite them.
   */
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id)) {
    return false;
  }

  bNodeTree &tree = *ma.nodetree;

  /* The recorded links come back first, while every node they may still be read from exists. */
  bke::paint_layers::source_graph_release(ma, &report);

  /* The Principled is looked up before the helpers go: the finder walks back from the output's
   * Surface, which the paint pass viewer's Mix still feeds here and which is empty once it is
   * removed, so a lookup after the removal would find nothing to route the Surface back to. */
  ChannelUnavailableReason principled_reason = ChannelUnavailableReason::None;
  bNode *principled_found = const_cast<bNode *>(
      BKE_paint_material_principled_find(ma, principled_reason));

  /* The nodes the generator minted go the way of the stack: the Normal Map and Bump the routing
   * placed, and the paint pass viewer's Mix and Emission. A node that only carries the ownership
   * marker -- a duplicate the user made of a helper, or one the stack adopted from their graph --
   * is not in the minted record, is theirs, and stays. */
  Vector<bNode *> owned_nodes;
  for (bNode &node : tree.nodes) {
    if ((bke::paint_layers::node_is_generator_owned(node) &&
         bke::paint_layers::response_node_is_created(tree, node)) ||
        bke::paint_layers::prop_int_get(node.prop, bke::paint_layers::VIEW_PASS_MIX_PROP, 0) != 0 ||
        bke::paint_layers::prop_int_get(node.prop, bke::paint_layers::VIEW_PASS_EMISSION_PROP, 0) !=
            0)
    {
      owned_nodes.append(&node);
    }
  }
  for (bNode *node : owned_nodes) {
    bke::node_remove_node(&bmain, tree, *node, true);
  }
  /* The record named exactly the helpers removed above; a stale entry could only ever name a node
   * the user made later, so it goes now that its nodes are gone. */
  bke::paint_layers::response_nodes_created_clear(tree);

  /* Material Output's Surface goes back to the Principled, where the conversion found it. Only a
   * Surface the removals above freed is routed: one fed another way is the user's graph. */
  bNode *output = bke::paint_layers::material_output_find(ma);
  if (output != nullptr) {
    bNodeSocket *surface = bke::paint_layers::socket_in(*output, "Surface");
    if (surface != nullptr) {
      /* The removals above dirtied the cache the link read goes through. */
      tree.ensure_topology_cache();
      if (surface->directly_linked_links().is_empty()) {
        bNode *principled = principled_found;
        bool principled_in_tree = false;
        for (bNode &node : tree.nodes) {
          if (&node == principled) {
            principled_in_tree = true;
            break;
          }
        }
        bNodeSocket *bsdf = (principled_in_tree) ?
                                bke::paint_layers::socket_out(*principled, "BSDF") :
                                nullptr;
        if (bsdf != nullptr) {
          bke::node_add_link(tree, *principled, *bsdf, *output, *surface);
        }
      }
    }
  }

  if (bNode *instance = bke::paint_layers::instance_find(ma, ma.paint_layers_owner_uid)) {
    bke::node_remove_node(&bmain, tree, *instance, true);
  }

  /* The generated root tree goes with the instance: it is rebuilt from the layer records whenever
   * the material is layered again, and a stale tree whose stored root hash survived would let the
   * next regeneration keep a root it never wired. The layer and source groups it instantiated
   * (".PL ...") are left in the main as a cache: the regeneration looks them up by marker and
   * owner uid, keeps the ones whose stored hash still matches, clears and rebuilds the ones it
   * does not, and creates the missing ones again from the records. Nothing the user authored is
   * expected inside them -- an edit there is wiped by the next rebuild -- so an orphan purge
   * claiming them before the material is layered again loses nothing but the rebuild. */
  if (ma.paint_layers_tree != nullptr &&
      BLI_uuid_equal(bke::paint_layers::tree_owner_uid_get(*ma.paint_layers_tree),
                     ma.paint_layers_owner_uid))
  {
    bNodeTree *root = ma.paint_layers_tree;
    ma.paint_layers_tree = nullptr;
    BKE_id_delete(&bmain, &root->id);
  }

  /* The source-graph cache describes a render of the graph the stack read; with the stack gone it
   * is a stale claim on the graph and dead weight in the file. The maps go the way of the
   * generated tree above, and undo brings every part of this back. */
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (Image *image = ma.paint_layers_source_images[i]) {
      BKE_id_delete(&bmain, &image->id);
    }
    ma.paint_layers_source_images[i] = nullptr;
  }
  ma.paint_layers_source_hash = 0;

  /* The layer records and their maps stay in the material: reverting is about the graph and the
   * generated tree, not about deleting what the user painted. They sleep until the material is
   * re-layered, and undo brings every part of this back. The source-cache stale mark goes too: the
   * cache and its claim were removed above, and a re-layered material starts from no claim. */
  ma.paint_layers_flag &= ~(MA_PAINT_LAYERED | MA_PAINT_LAYERS_SOURCE_GRAPH |
                            MA_PAINT_LAYERS_SOURCE_STALE);

  BKE_ntree_update_tag_all(&tree);
  return true;
}

}  // namespace blender
