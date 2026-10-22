/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * The #Main-side half of the paint-layer generator: it owns the generated group, creates and
 * reuses the per-row groups, wires the group instance into the material, schedules the rebuild
 * and keeps the derived data consistent across a copy and a memfile undo.
 */

#include "BKE_paint_layers_generate.hh"

#include <cstdio>
#include <iterator>
#include <utility>

#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_string_utf8.h"
#include "BLI_ustring.hh"
#include "BLI_threads.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_idprop.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_interface.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_material_composite.hh"
#include "BKE_paint_material_resolve.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "NOD_shader.h"
#include "NOD_socket.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_enums.h"
#include "DNA_object_types.h"
#include "DNA_uuid_types.h"
#include "DNA_windowmanager_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "paint_layers_intern.hh"
#include "paint_layers_kind.hh"

namespace blender {
using namespace bke::paint_layers;

/**
 * The root tree of \a ma: the one it already owns, or a fresh one. A tree in the pointer that is
 * not this material's is left alone (and reported) and replaced by a fresh tree.
 *
 * \return null when the tree could not be created.
 */
static bNodeTree *root_tree_ensure(Main &bmain,
                                   Material &ma,
                                   PaintLayersRegenerateReport &report,
                                   bool &r_created)
{
  r_created = false;
  bNodeTree *tree = ma.paint_layers_tree;
  const bool owned = tree != nullptr &&
                     BLI_uuid_equal(tree_owner_uid_get(*tree), ma.paint_layers_owner_uid);
  if (tree != nullptr && !owned) {
    /* Somebody replaced the pointer with a tree that is not ours. Leave their data alone and make a
     * fresh tree; the foreign one keeps whatever users it has. */
    ma.paint_layers_tree = nullptr;
    tree = nullptr;
    report.replaced_foreign_tree = true;
  }

  char root_name[MAX_ID_NAME - 2];
  /* A leading dot keeps the generated tree out of every ID picker and #Node Groups list (#ID name
   * convention: `interface_template_id` hides `id->name[2] == '.'`). */
  SNPRINTF(root_name, ".PBR Layers (%s)", ma.id.name + 2);
  if (tree == nullptr) {
    tree = bke::node_tree_add_tree(&bmain, root_name, "ShaderNodeTree");
    if (tree == nullptr) {
      return nullptr;
    }
    /* The tree is created with one user, which is this material from here on. */
    ma.paint_layers_tree = tree;
    tree_owner_uid_set(*tree, ma.paint_layers_owner_uid);
    r_created = true;
  }
  else if (!STREQ(tree->id.name + 2, root_name)) {
    /* The material was renamed. The pointer in #paint_layers_tree already found the tree, so this
     * is a rename of a known ID, never a name lookup. */
    BKE_id_rename(bmain, tree->id, root_name);
  }
  return tree;
}

/**
 * Hands each layer a node group of its own during one regeneration, reusing an old one by layer
 * marker so a regeneration does not create a data-block per row each time.
 */
class LayerTreeFactory {
 public:
  LayerTreeFactory(Main &bmain, Material &ma, const bNodeTree &root, const Span<int> wired_channels)
      : bmain_(bmain), ma_(ma), wired_channels_(wired_channels), forms_(paint_layers_form_map(ma))
  {
    /* The layer groups this material already owns, found in #Main rather than through the root's
     * instances: an undo detaches those, and groups only reachable through them would be left
     * behind as orphans for every regeneration after one. */
    for (bNodeTree &candidate : bmain.nodetrees) {
      bUUID marker = BLI_uuid_nil();
      if (&candidate != &root && layer_tree_marker_get(candidate, marker) &&
          BLI_uuid_equal(tree_owner_uid_get(candidate), ma.paint_layers_owner_uid))
      {
        old_.append(&candidate);
      }
      bUUID source_marker = BLI_uuid_nil();
      if (source_group_row_get(candidate, source_marker) &&
          BLI_uuid_equal(tree_owner_uid_get(candidate), ma.paint_layers_owner_uid))
      {
        old_sources_.append(&candidate);
      }
    }
  }

  /**
   * The group holding a copy of the source graph of the Material row \a layer, filled and current:
   * its old one refilled when the source's content moved, or a fresh one. Null when the source
   * cannot be wrapped; the group is then left for #prune_unused.
   */
  bNodeTree *source_group_get(const MaterialPaintLayer &layer)
  {
    if (layer.material == nullptr) {
      return nullptr;
    }
    if (bNodeTree **handed = source_handed_.lookup_ptr(&layer)) {
      return *handed;
    }
    char name[MAX_ID_NAME - 2];
    SNPRINTF(name, ".PL Source %s", layer.name[0] != '\0' ? layer.name : "Layer");
    bNodeTree *group = nullptr;
    for (bNodeTree *candidate : old_sources_) {
      bUUID marker = BLI_uuid_nil();
      if (!sources_used_.contains(candidate) && source_group_row_get(*candidate, marker) &&
          BLI_uuid_equal(marker, layer.marker))
      {
        group = candidate;
        break;
      }
    }
    bool current = false;
    const bool mapped = BKE_paint_layers_mapping_applies(ma_, layer);
    if (group == nullptr) {
      group = bke::node_tree_add_tree(&bmain_, name, "ShaderNodeTree");
      if (group == nullptr) {
        return nullptr;
      }
      /* The only user must be the instance node, which adds its own reference when assigned. */
      id_us_min(&group->id);
      tree_owner_uid_set(*group, ma_.paint_layers_owner_uid);
      source_group_row_set(*group, layer.marker);
      created_ = true;
    }
    else {
      uint64_t stored = 0;
      current = tree_topology_hash_get(*group, stored) &&
                stored == source_group_content_hash(*layer.material, mapped);
      if (!STREQ(group->id.name + 2, name)) {
        BKE_id_rename(bmain_, group->id, name);
      }
    }
    sources_used_.append(group);
    if (!current) {
      tree_hash_clear(*group);
      /* A fill replaces the private copies of the groups the source is reached through, which are
       * ID references that appeared or disappeared. */
      created_ = true;
      if (!source_group_fill(bmain_, *group, *layer.material, mapped)) {
        /* Remembered, so the other channels of the row do not fill it again. */
        source_handed_.add(&layer, nullptr);
        return nullptr;
      }
      pending_hashes_.append({group, source_group_content_hash(*layer.material, mapped)});
      source_group_structure_hash_set(*group,
                                      source_group_structure_hash(*layer.material, mapped));
    }
    source_handed_.add(&layer, group);
    return group;
  }

  /** The source groups handed out so far. */
  Span<bNodeTree *> sources_used() const
  {
    return sources_used_;
  }

  /** The group holding \a layer's nodes: its old one (kept or emptied), or a fresh one. */
  bNodeTree *layer_tree_get(const MaterialPaintLayer &layer)
  {
    /* Handed out once per pass: the build asks per channel, and a second ask must return the same
     * tree rather than create a fresh one. */
    if (bNodeTree **handed = handed_.lookup_ptr(&layer)) {
      return *handed;
    }
    char name[MAX_ID_NAME - 2];
    SNPRINTF(name, ".PL Layer %s", layer.name[0] != '\0' ? layer.name : "Layer");
    const uint64_t topology = paint_layers_layer_topology_hash(ma_, layer, wired_channels_, forms_);
    for (bNodeTree *candidate : old_) {
      if (used_.contains(candidate)) {
        continue;
      }
      bUUID marker = BLI_uuid_nil();
      if (layer_tree_marker_get(*candidate, marker) && BLI_uuid_equal(marker, layer.marker)) {
        uint64_t stored = 0;
        const bool has_stored = tree_topology_hash_get(*candidate, stored);
        PBR_LAYERS_LOG("[PBR-Layers regen]   layer=\"%s\" group topology stored=%llu (has=%d) now=%llu => %s\n",
               layer.name,
               (unsigned long long)stored,
               int(has_stored),
               (unsigned long long)topology,
               (has_stored && stored == topology) ? "group KEPT" : "group CLEARED + REBUILT");
        if (has_stored && stored == topology) {
          /* Unchanged: keep the nodes and the interface; the build only re-links the instance. */
          used_.append(candidate);
          unchanged_.add(&layer);
          handed_.add(&layer, candidate);
          return candidate;
        }
        /* The structure changed: the root was cleared before the build, so nothing instantiates
         * the group any more, and a full clear drops the stale nodes and interface. The stored hash
         * is dropped with them and written back only once the regeneration succeeded, so a build
         * that failed half way is never taken for a finished group. */
        tree_clear(bmain_, *candidate);
        tree_hash_clear(*candidate);
        /* The keyed blend nodes are gone with the nodes; the commit after the build recounts. */
        prop_int_set(candidate->id.properties, BLEND_NODE_COUNT_PROP, -1);
        pending_hashes_.append({candidate, topology});
        if (!STREQ(candidate->id.name + 2, name)) {
          BKE_id_rename(bmain_, candidate->id, name);
        }
        used_.append(candidate);
        handed_.add(&layer, candidate);
        return candidate;
      }
    }
    PBR_LAYERS_LOG("[PBR-Layers regen]   layer=\"%s\" no previous group => NEW group\n", layer.name);
    bNodeTree *fresh = bke::node_tree_add_tree(&bmain_, name, "ShaderNodeTree");
    if (fresh == nullptr) {
      return nullptr;
    }
    /* The only user must be the instance node, which adds its own reference when assigned. */
    id_us_min(&fresh->id);
    tree_owner_uid_set(*fresh, ma_.paint_layers_owner_uid);
    uid_prop_set(fresh->id.properties, TREE_LAYER_PROP, layer.marker);
    pending_hashes_.append({fresh, topology});
    used_.append(fresh);
    handed_.add(&layer, fresh);
    created_ = true;
    return fresh;
  }

  /** Whether \a layer's group was kept unchanged in this pass. */
  bool is_unchanged(const MaterialPaintLayer &layer) const
  {
    return unchanged_.contains(&layer);
  }

  /** The groups handed out so far. */
  Span<bNodeTree *> used() const
  {
    return used_;
  }

  /** Delete the groups nobody uses any more. */
  void prune_unused()
  {
    for (bNodeTree *layer_tree : old_) {
      if (used_.contains(layer_tree)) {
        continue;
      }
      /* The layer is gone and its group has no users; a tree the user linked keeps its users. */
      if (ID_REAL_USERS(&layer_tree->id) <= 0) {
        this->tree_delete(layer_tree);
      }
    }
    for (bNodeTree *layer_tree : used_) {
      /* The build asked for a group but did not instantiate it, so nothing owns it. */
      if (ID_REAL_USERS(&layer_tree->id) <= 0) {
        this->tree_delete(layer_tree);
      }
    }
    /* Last: deleting a layer group frees the instance that held its source group. */
    for (bNodeTree *source_tree : old_sources_) {
      if (!sources_used_.contains(source_tree) && ID_REAL_USERS(&source_tree->id) <= 0) {
        this->tree_delete(source_tree);
      }
    }
    for (bNodeTree *source_tree : sources_used_) {
      /* A source group that was filled but is not instantiated, or whose fill failed. */
      if (ID_REAL_USERS(&source_tree->id) <= 0) {
        this->tree_delete(source_tree);
      }
    }
    /* The private copies of the groups a deleted source group instanced. */
    source_group_paths_prune(bmain_, ma_.paint_layers_owner_uid);
  }

  /** Record the topology of every group this pass built; call once the whole build succeeded. */
  void topology_hashes_commit()
  {
    for (const auto &[tree, topology] : pending_hashes_) {
      tree_topology_hash_set(*tree, topology);
    }
    pending_hashes_.clear();
  }

  bool groups_created() const
  {
    return created_;
  }
  bool groups_deleted() const
  {
    return deleted_;
  }

 private:
  /** Delete \a tree, and forget the hash a build meant to store on it. */
  void tree_delete(bNodeTree *tree)
  {
    pending_hashes_.remove_if(
        [&](const std::pair<bNodeTree *, uint64_t> &item) { return item.first == tree; });
    /* Its nodes go first so the groups they instance lose that user before they are pruned. */
    tree_clear_nodes(bmain_, *tree);
    BKE_id_delete(&bmain_, tree);
    deleted_ = true;
  }

  Main &bmain_;
  Material &ma_;
  Span<int> wired_channels_;
  /** The place-dependent form of every row, walked once for the whole regeneration. */
  PaintLayerFormMap forms_;
  /** The groups this material owned before the regeneration. */
  Vector<bNodeTree *> old_;
  /** The groups handed out in this regeneration. */
  Vector<bNodeTree *> used_;
  /** The source groups this material owned before the regeneration, and those handed out since. */
  Vector<bNodeTree *> old_sources_;
  Vector<bNodeTree *> sources_used_;
  Map<const MaterialPaintLayer *, bNodeTree *> source_handed_;
  /** The group handed to each row this pass, so a second ask returns the same tree. */
  Map<const MaterialPaintLayer *, bNodeTree *> handed_;
  /** The rows whose group was kept unchanged (its topology hash matched). */
  Set<const MaterialPaintLayer *> unchanged_;
  /** Groups built or rebuilt this pass with the hash to store on them after a successful build. */
  Vector<std::pair<bNodeTree *, uint64_t>> pending_hashes_;
  bool created_ = false;
  bool deleted_ = false;
};

/** Update the layer groups first, then the root that instances them. */
static void generated_trees_update(Main &bmain, const Span<bNodeTree *> layer_trees, bNodeTree &root)
{
  for (bNodeTree *layer_tree : layer_trees) {
    BKE_ntree_update_tag_all(layer_tree);
    BKE_ntree_update_after_single_tree_change(bmain, *layer_tree);
    DEG_id_tag_update(&layer_tree->id, ID_RECALC_SYNC_TO_EVAL);
  }
  BKE_ntree_update_tag_all(&root);
  BKE_ntree_update_after_single_tree_change(bmain, root);
}

/**
 * The node in \a ma's embedded tree that instances \a tree: found by its marker and re-pointed when
 * it names a different tree, created when missing.
 *
 * \return null when the instance node cannot be created.
 */
static bNode *instance_ensure(Material &ma, bNodeTree &tree)
{
  bNode *instance = instance_find(ma, ma.paint_layers_owner_uid);
  if (instance == nullptr) {
    if (ma.nodetree->typeinfo == nullptr || ma.nodetree->typeinfo->group_idname == nullptr) {
      return nullptr;
    }
    instance = bke::node_add_node(nullptr, *ma.nodetree, ma.nodetree->typeinfo->group_idname);
    if (instance == nullptr) {
      return nullptr;
    }
    instance->location[0] = -200.0f;
    instance->location[1] = 0.0f;
  }
  if (instance->id != &tree.id) {
    if (instance->id != nullptr) {
      id_us_min(instance->id);
    }
    instance->id = &tree.id;
    id_us_plus(&tree.id);
    instance_uid_set(*instance, ma.paint_layers_owner_uid);
    /* A group assigned after the node was created only grows its sockets once the updater is told
     * the node's group changed; a plain tree update does not notice. */
    BKE_ntree_update_tag_node_property(ma.nodetree, instance);
  }
  return instance;
}

/**
 * Refill, in place, the source group of each Material row whose source's content moved since the
 * group was filled. A kept root and a kept layer group do not rebuild, so this is what brings a
 * source edit that changed no structure (a node's setting, a link) into the group. The group's
 * outputs stay as they were: a change in which channels it feeds is a structural change that the
 * row's live signature (#paint_layer_material_live_signature) turns into a rebuild.
 */
static void source_groups_refresh(Main &bmain, Material &ma)
{
  BKE_paint_layers_foreach_source_row(
      static_cast<const Material &>(ma), [&](const MaterialPaintLayer &layer) -> bool {
        if (layer.material == nullptr) {
          return true;
        }
        const bool mapped = BKE_paint_layers_mapping_applies(ma, layer);
        for (bNodeTree &group : bmain.nodetrees) {
          bUUID marker = BLI_uuid_nil();
          if (!source_group_row_get(group, marker) || !BLI_uuid_equal(marker, layer.marker) ||
              !BLI_uuid_equal(tree_owner_uid_get(group), ma.paint_layers_owner_uid))
          {
            continue;
          }
          const uint64_t content = source_group_content_hash(*layer.material, mapped);
          uint64_t stored = 0;
          if (tree_topology_hash_get(group, stored) && stored == content) {
            break;
          }
          /* Only socket values moved (a slider dragged): the group takes them in place, with no
           * rebuild of its nodes and no change of relations. */
          const uint64_t structure = source_group_structure_hash(*layer.material, mapped);
          uint64_t stored_structure = 0;
          if (source_group_structure_hash_get(group, stored_structure) &&
              stored_structure == structure &&
              source_group_values_sync(bmain, group, *layer.material))
          {
            tree_topology_hash_set(group, content);
            DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
            break;
          }
          tree_hash_clear(group);
          if (source_group_fill(bmain, group, *layer.material, mapped)) {
            tree_topology_hash_set(group, content);
            source_group_structure_hash_set(group, structure);
            /* The private copies of the groups on the source's path are new IDs. */
            DEG_relations_tag_update(&bmain);
          }
          else {
            /* Nothing to show any more: the rebuild decides what the row lays instead. */
            ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
          }
          BKE_ntree_update_tag_all(&group);
          BKE_ntree_update_after_single_tree_change(bmain, group);
          DEG_id_tag_update(&group.id, ID_RECALC_SYNC_TO_EVAL);
          DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
          break;
        }
        return true;
      });
}

/**
 * The embedded tree and the generated group were rewritten in place. A shading tag alone
 * re-evaluates the existing evaluated copies without re-copying them, so the viewport would keep
 * the group node's old sockets and never show the new rows; a final render, which builds its own
 * copies, would. Only an ID reference that appeared or disappeared is a relation change.
 */
static void regenerate_tags(Main &bmain,
                            Material &ma,
                            bNodeTree &tree,
                            const bool relations_changed)
{
  DEG_id_tag_update(&tree.id, ID_RECALC_SYNC_TO_EVAL);
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  if (relations_changed) {
    DEG_relations_tag_update(&bmain);
  }
}

/* -------------------------------------------------------------------- */
/** \name Viewport paint pass preview
 *
 * The preview keeps a Mix Shader between the material's Principled and its Material Output, with an
 * Emission fed by one channel's generated result. The mix factor is the only thing that moves when
 * a pass is selected: 0 shows the material, 1 shows the pass. The nodes are marked so a user's own
 * Mix or Emission is never mistaken for them.
 * \{ */

/** A marked viewer node of \a ma's embedded tree, or null. */
static bNode *viewer_node_find(Material &ma, const char *prop_key, const int type_legacy)
{
  if (ma.nodetree == nullptr) {
    return nullptr;
  }
  for (bNode &node : ma.nodetree->nodes) {
    if (node.type_legacy != type_legacy) {
      continue;
    }
    if (prop_int_get(node.prop, prop_key, 0) != 0) {
      return &node;
    }
  }
  return nullptr;
}

/**
 * The node feeding \a socket, and how many links feed it. More than one is ambiguous and treated as
 * a hand-built graph by the caller.
 */
static bNode *viewer_socket_source(const bNodeTree &tree,
                                   const bNodeSocket &socket,
                                   int &r_links_num)
{
  bNode *from = nullptr;
  r_links_num = 0;
  for (const bNodeLink &link : tree.links) {
    if (link.tosock != &socket) {
      continue;
    }
    r_links_num++;
    from = link.fromnode;
  }
  return from;
}

static bool viewer_socket_linked_to(const bNodeTree &tree,
                                    const bNodeSocket &to,
                                    const bNodeSocket &from)
{
  for (const bNodeLink &link : tree.links) {
    if (link.tosock == &to && link.fromsock == &from) {
      return true;
    }
  }
  return false;
}

static void viewer_mix_factor_set(bNodeTree &tree, bNode &mix, const float factor)
{
  bNodeSocket *fac = socket_in(mix, "Fac");
  if (fac == nullptr) {
    return;
  }
  bke::node_remove_socket_links(tree, *fac);
  if (bNodeSocketValueFloat *value = static_cast<bNodeSocketValueFloat *>(fac->default_value)) {
    value->value = factor;
  }
}

static void viewer_depsgraph_tag(Main *bmain, Material &ma)
{
  if (ma.nodetree == nullptr) {
    return;
  }
  BKE_ntree_update_tag_all(ma.nodetree);
  if (bmain != nullptr) {
    BKE_ntree_update_after_single_tree_change(*bmain, *ma.nodetree);
  }
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
}

bool BKE_paint_layers_is_view_pass_mix(const bNode &node)
{
  return node.type_legacy == SH_NODE_MIX_SHADER &&
         prop_int_get(node.prop, VIEW_PASS_MIX_PROP, 0) != 0;
}

/** The active row of \a ma, or null. */
static MaterialPaintLayer *view_pass_active_layer(const Material &ma)
{
  Material &mutable_ma = const_cast<Material &>(ma);
  MaterialPaintLayer *active = BKE_paint_layers_active_layer_get(mutable_ma);
  /* A content correction answers as itself, but the mask it is laid under is its row's. A mask
   * item is already answered as that row. */
  if (active != nullptr && active->role == MA_PAINT_LAYER_ROLE_EFFECT) {
    if (MaterialPaintLayer *owner = BKE_paint_layers_correction_owner(mutable_ma, *active)) {
      return owner;
    }
  }
  return active;
}

/** The flat color the viewer shows for a channel of the set that has no layer stack to show. */
static void view_pass_empty_color(const int pass, float r_color[4])
{
  /* The neutral value of the channel: no occlusion, a flat tangent normal, full coverage. */
  float value[3] = {0.0f, 0.0f, 0.0f};
  switch (pass) {
    case PAINT_MATERIAL_CHANNEL_AO:
    case PAINT_MATERIAL_CHANNEL_ALPHA:
      value[0] = value[1] = value[2] = 1.0f;
      break;
    case PAINT_MATERIAL_CHANNEL_NORMAL:
      value[0] = value[1] = 0.5f;
      value[2] = 1.0f;
      break;
    default:
      break;
  }
  copy_v3_v3(r_color, value);
  r_color[3] = 1.0f;
}

/** The interface output of \a tree named \a name, or null. */
static bNodeTreeInterfaceSocket *tree_output_interface_find(bNodeTree &tree, const char *name)
{
  tree.ensure_interface_cache();
  for (bNodeTreeInterfaceSocket *iface : tree.interface_outputs()) {
    if (iface->name != nullptr && STREQ(iface->name, name) && iface->identifier != nullptr) {
      return iface;
    }
  }
  return nullptr;
}

/** The Group Output input socket of \a tree fed by the interface output \a name. */
static bNodeSocket *tree_group_output_socket_find(bNodeTree &tree, const char *name)
{
  bNode *output = tree.group_output_node();
  bNodeTreeInterfaceSocket *iface = tree_output_interface_find(tree, name);
  if (output == nullptr || iface == nullptr) {
    return nullptr;
  }
  return bke::node_find_socket(*output, SOCK_IN, UString::from_ptr_noinline(iface->identifier));
}

/** The instance in \a ma's generated root of the layer group \a marker, or null. */
static bNode *view_pass_layer_instance_find(const Material &ma, const bUUID &marker)
{
  if (ma.paint_layers_tree == nullptr || BLI_uuid_is_nil(marker)) {
    return nullptr;
  }
  for (bNode &node : ma.paint_layers_tree->nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr) {
      continue;
    }
    bUUID group_marker = BLI_uuid_nil();
    if (!layer_tree_marker_get(*group, group_marker) ||
        !BLI_uuid_equal(group_marker, marker) ||
        !BLI_uuid_equal(tree_owner_uid_get(*group), ma.paint_layers_owner_uid))
    {
      continue;
    }
    return &node;
  }
  return nullptr;
}

/* Combined first, then every channel the generated tree can expose, then the active layer's mask.
 * Height is here although the Image Editor's display list leaves it out: the viewport shows the
 * channel's generated result directly, and the mask is a display of its own.
 *
 * Append-only: #Material::paint_layers_view_pass stores an index into this array. */
static constexpr int view_passes[] = {
    PAINT_LAYER_PASS_COMBINED,
    PAINT_MATERIAL_CHANNEL_BASE_COLOR,
    PAINT_MATERIAL_CHANNEL_METALLIC,
    PAINT_MATERIAL_CHANNEL_ROUGHNESS,
    PAINT_MATERIAL_CHANNEL_SPECULAR,
    PAINT_MATERIAL_CHANNEL_NORMAL,
    PAINT_MATERIAL_CHANNEL_HEIGHT,
    PAINT_MATERIAL_CHANNEL_AO,
    PAINT_MATERIAL_CHANNEL_ALPHA,
    PAINT_MATERIAL_CHANNEL_EMISSION,
    PAINT_LAYER_MAP_MASK,
};
/* Guards the stored indices: Combined has to stay zero, which is what files saved before the
 * preview read as, and reordering an entry needs a versioning step (see the list's declaration). */
static_assert(view_passes[0] == PAINT_LAYER_PASS_COMBINED);
static_assert(std::size(view_passes) == 11);
static_assert(view_passes[1] == PAINT_MATERIAL_CHANNEL_BASE_COLOR &&
              view_passes[6] == PAINT_MATERIAL_CHANNEL_HEIGHT &&
              view_passes[std::size(view_passes) - 1] == PAINT_LAYER_MAP_MASK);

Span<int> BKE_paint_layers_view_passes()
{
  return Span<int>(view_passes, int(std::size(view_passes)));
}

int BKE_paint_layers_view_pass_get(const Material &ma)
{
  if (!BKE_paint_layers_is_layered(ma)) {
    return PAINT_LAYER_PASS_COMBINED;
  }
  const Span<int> passes = BKE_paint_layers_view_passes();
  if (passes.is_empty()) {
    return PAINT_LAYER_PASS_COMBINED;
  }
  int index = ma.paint_layers_view_pass;
  if (index < 0) {
    index = 0;
  }
  else if (index >= int(passes.size())) {
    index = int(passes.size()) - 1;
  }
  return passes[index];
}

bool BKE_paint_layers_view_pass_available(const Material &ma, const int pass)
{
  if (!BKE_paint_layers_is_layered(ma)) {
    return false;
  }
  if (pass == PAINT_LAYER_PASS_COMBINED) {
    return true;
  }
  if (pass == PAINT_LAYER_MAP_MASK) {
    MaterialPaintLayer *active = view_pass_active_layer(ma);
    if (active == nullptr || BKE_paint_layers_mask_base(*active) == nullptr) {
      return false;
    }
    /* Before the generated tree carries the mask output, offer the pass so choosing it can upgrade
     * the tree once. Afterwards, require the active row's group to expose it. */
    if (ma.paint_layers_tree == nullptr ||
        tree_output_interface_find(*ma.paint_layers_tree, "Result Mask") == nullptr)
    {
      return true;
    }
    bNode *instance = view_pass_layer_instance_find(ma, active->marker);
    if (instance == nullptr || instance->id == nullptr || GS(instance->id->name) != ID_NT) {
      return false;
    }
    bNodeTree *group = id_cast<bNodeTree *>(instance->id);
    return group != nullptr && instance_output_find(*group, *instance, "Mask") != nullptr;
  }
  /* Every role past the channel range has no generated result of its own. */
  if (pass < 0 || pass >= PAINT_MATERIAL_CHANNEL_NUM) {
    return false;
  }
  if (!BKE_paint_layers_channel_in_set(ma, eMaterialPaintChannel(pass))) {
    return false;
  }
  /* Every channel of the set is offered, as the Image Editor does: one without a layer stack has no
   * generated result, and the viewer shows its neutral flat value instead (see the sync). This
   * also keeps the check cheap, as it runs for each item of the header menu on every redraw. */
  return true;
}

bool BKE_paint_layers_view_pass_set(Material &ma, const int pass)
{
  if (pass != PAINT_LAYER_PASS_COMBINED && !BKE_paint_layers_view_pass_available(ma, pass)) {
    return false;
  }
  const Span<int> passes = BKE_paint_layers_view_passes();
  int index = 0;
  if (pass != PAINT_LAYER_PASS_COMBINED) {
    for (int i = 1; i < int(passes.size()); i++) {
      if (passes[i] == pass) {
        index = i;
        break;
      }
    }
    if (index == 0) {
      /* A channel the display list does not offer (Height, Custom) is no preview. */
      return false;
    }
  }
  ma.paint_layers_view_pass = int16_t(index);
  return true;
}

/**
 * Point the root's `Result Mask` output at the active row's group instance. Returns false when the
 * active row has no mask output; a root saved before the output existed is scheduled for the one
 * rebuild that adds it.
 */
static bool view_pass_mask_root_link(Main *bmain, Material &ma)
{
  if (ma.paint_layers_tree == nullptr) {
    return false;
  }
  bNodeTree &root = *ma.paint_layers_tree;
  if (tree_output_interface_find(root, "Result Mask") == nullptr) {
    /* Only a tree with channel outputs can gain the mask output; one with nothing wired is left
     * alone, so the pass cannot schedule itself for rebuild forever. */
    if (!root.interface_outputs().is_empty() &&
        (ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN) == 0)
    {
      ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
      DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
    }
    return false;
  }

  MaterialPaintLayer *active = view_pass_active_layer(ma);
  if (active == nullptr || BKE_paint_layers_mask_base(*active) == nullptr) {
    return false;
  }
  bNode *instance = view_pass_layer_instance_find(ma, active->marker);
  if (instance == nullptr || instance->id == nullptr || GS(instance->id->name) != ID_NT) {
    return false;
  }
  bNodeTree *group = id_cast<bNodeTree *>(instance->id);
  bNodeSocket *mask_out = (group != nullptr) ? instance_output_find(*group, *instance, "Mask") :
                                               nullptr;
  bNode *root_output = root.group_output_node();
  bNodeSocket *root_out = tree_group_output_socket_find(root, "Result Mask");
  if (mask_out == nullptr || root_output == nullptr || root_out == nullptr) {
    return false;
  }
  if (viewer_socket_linked_to(root, *root_out, *mask_out)) {
    return true;
  }
  bke::node_remove_socket_links(root, *root_out);
  bke::node_add_link(root, *instance, *mask_out, *root_output, *root_out);
  BKE_ntree_update_tag_all(&root);
  if (bmain != nullptr) {
    BKE_ntree_update_after_single_tree_change(*bmain, root);
  }
  DEG_id_tag_update(&root.id, ID_RECALC_SYNC_TO_EVAL);
  return true;
}

bool BKE_paint_layers_view_pass_sync(Main *bmain, Material &ma)
{
  if (ma.nodetree == nullptr || !BKE_paint_layers_is_layered(ma)) {
    return false;
  }
  /* A linked or override material's trees are owned by another file: never rewrite them for a
   * view-only setting. */
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id)) {
    return false;
  }

  int pass = BKE_paint_layers_view_pass_get(ma);
  if (pass != PAINT_LAYER_PASS_COMBINED && !BKE_paint_layers_view_pass_available(ma, pass)) {
    /* The channel left the material's set or lost its stack: the selector must not rest on a
     * pass that can no longer be shown. */
    ma.paint_layers_view_pass = 0;
    pass = PAINT_LAYER_PASS_COMBINED;
  }

  bNode *mix = viewer_node_find(ma, VIEW_PASS_MIX_PROP, SH_NODE_MIX_SHADER);
  if (pass == PAINT_LAYER_PASS_COMBINED) {
    if (mix != nullptr) {
      viewer_mix_factor_set(*ma.nodetree, *mix, 0.0f);
      viewer_depsgraph_tag(bmain, ma);
    }
    return true;
  }

  bNodeTree &tree = *ma.nodetree;
  /* Any failure leaves the material on its combined shading: a half-installed viewer must never
   * keep showing a pass whose channel moved. */
  const auto fail = [&]() -> bool {
    if (mix != nullptr) {
      viewer_mix_factor_set(tree, *mix, 0.0f);
      viewer_depsgraph_tag(bmain, ma);
    }
    return false;
  };

  bNode *output = ntreeShaderOutputNode(&tree, SHD_OUTPUT_ALL);
  if (output == nullptr) {
    return fail();
  }
  bNodeSocket *surface = socket_in(*output, "Surface");
  if (surface == nullptr) {
    return fail();
  }

  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  const bNode *principled_const = BKE_paint_material_principled_find(ma, reason);
  if (principled_const == nullptr) {
    return fail();
  }
  /* The Mix has to reach the Principled with a link in this tree. A Principled the resolver found
   * inside a group is wired through that group and cannot be linked here directly. */
  bool principled_in_tree = false;
  for (bNode &node : tree.nodes) {
    if (&node == principled_const) {
      principled_in_tree = true;
      break;
    }
  }
  if (!principled_in_tree) {
    return fail();
  }
  bNode &principled = const_cast<bNode &>(*principled_const);
  bNodeSocket *bsdf = socket_out(principled, "BSDF");
  if (bsdf == nullptr) {
    return fail();
  }

  int surface_links_num = 0;
  bNode *surface_from = viewer_socket_source(tree, *surface, surface_links_num);
  if (mix == nullptr) {
    /* Only a material whose Surface the generator routes from a Principled gets a viewer: on a
     * hand-built shader the Mix would change what the user sees without their asking. */
    if (surface_links_num != 1 || surface_from != &principled) {
      return fail();
    }
    mix = bke::node_add_static_node(nullptr, tree, SH_NODE_MIX_SHADER);
    if (mix == nullptr) {
      return fail();
    }
    prop_int_set(mix->prop, VIEW_PASS_MIX_PROP, 1);
    STRNCPY_UTF8(mix->label, "Paint Pass");
    mix->location[0] = output->location[0] - 200.0f;
    mix->location[1] = output->location[1];
  }
  else if (!(surface_links_num == 1 && surface_from == mix)) {
    /* The Mix is ours, but the user may have detached it. Reattach only where the Surface is free
     * or still fed by the Principled; anything else is their graph. */
    if (surface_links_num != 0 && !(surface_links_num == 1 && surface_from == &principled)) {
      return fail();
    }
    bke::node_remove_socket_links(tree, *surface);
  }

  bNodeSocket *mix_principled = static_cast<bNodeSocket *>(BLI_findlink(&mix->inputs, 1));
  bNodeSocket *mix_viewer = static_cast<bNodeSocket *>(BLI_findlink(&mix->inputs, 2));
  bNodeSocket *mix_out = socket_out(*mix, "Shader");
  if (mix_principled == nullptr || mix_viewer == nullptr || mix_out == nullptr) {
    return fail();
  }
  if (!viewer_socket_linked_to(tree, *surface, *mix_out)) {
    bke::node_remove_socket_links(tree, *surface);
    bke::node_add_link(tree, *mix, *mix_out, *output, *surface);
  }
  if (!viewer_socket_linked_to(tree, *mix_principled, *bsdf)) {
    bke::node_remove_socket_links(tree, *mix_principled);
    bke::node_add_link(tree, principled, *bsdf, *mix, *mix_principled);
  }

  bNode *emission = viewer_node_find(ma, VIEW_PASS_EMISSION_PROP, SH_NODE_EMISSION);
  if (emission == nullptr) {
    emission = bke::node_add_static_node(nullptr, tree, SH_NODE_EMISSION);
    if (emission == nullptr) {
      return fail();
    }
    prop_int_set(emission->prop, VIEW_PASS_EMISSION_PROP, 1);
    STRNCPY_UTF8(emission->label, "Paint Pass Viewer");
    emission->location[0] = mix->location[0] - 200.0f;
    emission->location[1] = mix->location[1] - 160.0f;
  }
  bNodeSocket *emission_out = socket_out(*emission, "Emission");
  bNodeSocket *emission_color = socket_in(*emission, "Color");
  bNodeSocket *emission_strength = socket_in(*emission, "Strength");
  if (emission_out == nullptr || emission_color == nullptr) {
    return fail();
  }
  if (emission_strength != nullptr) {
    if (bNodeSocketValueFloat *value = static_cast<bNodeSocketValueFloat *>(
            emission_strength->default_value))
    {
      value->value = 1.0f;
    }
  }
  if (!viewer_socket_linked_to(tree, *mix_viewer, *emission_out)) {
    bke::node_remove_socket_links(tree, *mix_viewer);
    bke::node_add_link(tree, *emission, *emission_out, *mix, *mix_viewer);
  }

  bNode *instance = instance_find(ma, ma.paint_layers_owner_uid);
  if (instance == nullptr || ma.paint_layers_tree == nullptr) {
    return fail();
  }
  bNodeSocket *result = nullptr;
  if (pass == PAINT_LAYER_MAP_MASK) {
    /* The root's mask output has to follow the active row before the material can show it. */
    if (!view_pass_mask_root_link(bmain, ma)) {
      return fail();
    }
    result = instance_output_find(*ma.paint_layers_tree, *instance, "Result Mask");
  }
  else {
    const MaterialPaintChannelInfo &info = BKE_paint_material_channel_info(
        eMaterialPaintChannel(pass));
    char result_name[64];
    SNPRINTF(result_name, "Result %s", info.ui_name);
    result = instance_output_find(*ma.paint_layers_tree, *instance, result_name);
  }
  if (result == nullptr && pass != PAINT_LAYER_MAP_MASK) {
    /* A channel of the set that no row takes part in has no generated result. It is still offered,
     * as the Image Editor offers it, and shows the channel's neutral value. */
    bke::node_remove_socket_links(tree, *emission_color);
    if (bNodeSocketValueRGBA *value = static_cast<bNodeSocketValueRGBA *>(
            emission_color->default_value))
    {
      view_pass_empty_color(pass, value->value);
    }
  }
  else if (result == nullptr) {
    return fail();
  }
  else if (!viewer_socket_linked_to(tree, *emission_color, *result)) {
    bke::node_remove_socket_links(tree, *emission_color);
    bke::node_add_link(tree, *instance, *result, *emission, *emission_color);
  }

  viewer_mix_factor_set(tree, *mix, 1.0f);
  viewer_depsgraph_tag(bmain, ma);
  return true;
}

/** \} */

bool BKE_paint_layers_regenerate(Main &bmain,
                                 Material &ma,
                                 PaintLayersRegenerateReport *r_report)
{
  PaintLayersRegenerateReport report;
  const auto report_out = [&]() {
    if (r_report != nullptr) {
      *r_report = report;
    }
  };

  if (!BKE_paint_layers_is_layered(ma) || ma.nodetree == nullptr) {
    report_out();
    return false;
  }
  /* A linked or override material is not editable here: its trees are owned by another file. */
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id)) {
    report_out();
    return false;
  }
  if (BLI_uuid_is_nil(ma.paint_layers_owner_uid)) {
    ma.paint_layers_owner_uid = BLI_uuid_generate_random();
  }

  bool created_tree = false;
  bNodeTree *tree = root_tree_ensure(bmain, ma, report, created_tree);
  if (tree == nullptr) {
    report_out();
    return false;
  }

  /* Structural edits outside the blend setters (a folder gaining a mask, a row removed) can make a
   * tier channel stop building a blend node. Prune before the hash and the build so such a record
   * cannot pull the channel back into the dynamic tier later. */
  paint_layers_blend_tier_prune(ma);

  PaintLayersBuildContext ctx;
  Vector<int> wired_channels = paint_layers_wired_channels(ma);
  /* The Normal chain needs the shared combine group; create it only when a Normal row exists,
   * whatever folder it sits in. */
  bool normal_row_exists = false;
  BKE_paint_layers_foreach(
      ma, [&](const MaterialPaintLayer &layer, const MaterialPaintLayer * /*parent*/) -> bool {
        normal_row_exists = leaf_participates(ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL);
        return !normal_row_exists;
      });
  if (normal_row_exists) {
    ctx.normal_combine_group = BKE_paint_material_normal_combine_group_ensure(bmain);
  }
  /* Same pattern for the runtime blend group: only a tier channel instantiates it, so the group is
   * only created when at least one row or correction is being scrubbed. */
  if (paint_layers_blend_any_dynamic(ma)) {
    ctx.blend_group = BKE_paint_material_blend_group_ensure(bmain);
  }

  /* A value edit changes no structure, so the root's stored hash still matches its description:
   * every group is kept and only the values are written. Nothing is rebuilt, and the groups' nodes
   * (and anything a user put in them) survive. A missing or mismatched stored hash -- a file written
   * before this feature, or a structural edit -- falls through to a full rebuild. */
  const uint64_t root_hash = paint_layers_root_topology_hash(ma, wired_channels);
  uint64_t stored_root = 0;
  /* The kept root also has to still be instantiated in the material: a user can delete the group
   * node, and a full regeneration is what puts it back (#instance_ensure). */
  const bNode *kept_instance = instance_find(ma, ma.paint_layers_owner_uid);
  const bool keep_root = !created_tree && tree_root_hash_get(*tree, stored_root) &&
                         stored_root == root_hash && kept_instance != nullptr &&
                         kept_instance->id == &tree->id;

  bool relations_changed = report.replaced_foreign_tree;
  /* A failed sync means a value input has no socket on its instance (a hand-edited tree) or a
   * keyed blend node no longer matches the description: the kept root cannot be trusted, so the
   * full rebuild below takes over. Nodes are synced before values, so a tree update cannot reset a
   * value written in the same pass. */
  const bool blend_sync_ok = keep_root && paint_layers_blend_sync(ma, bmain);
  const bool values_sync_ok = blend_sync_ok && BKE_paint_layers_values_sync(ma);
  const bool kept = values_sync_ok;
  PBR_LAYERS_LOG(
      "[PBR-Layers regen] material=\"%s\" created_tree=%d root_hash=%llu stored_root=%llu "
      "instance_ok=%d keep_root=%d blend_sync=%d values_sync=%d => %s\n",
      ma.id.name + 2,
      int(created_tree),
      (unsigned long long)root_hash,
      (unsigned long long)stored_root,
      int(kept_instance != nullptr && kept_instance->id == &tree->id),
      int(keep_root),
      int(blend_sync_ok),
      int(values_sync_ok),
      kept ? "KEPT (in-place)" : "FULL REBUILD");
  if (kept) {
    source_groups_refresh(bmain, ma);
    regenerate_tags(bmain, ma, *tree, false);
  }
  else {
    /* The factory outlives the build, which only holds a non-owning reference to it. */
    LayerTreeFactory factory(bmain, ma, *tree, wired_channels);
    const auto layer_tree_get = [&](const MaterialPaintLayer &layer) -> bNodeTree * {
      return factory.layer_tree_get(layer);
    };
    /* Named like #layer_tree_get: a #FunctionRef keeps a pointer to its callable, so a lambda
     * assigned straight to it would be gone by the time the build calls it. */
    const auto layer_unchanged = [&](const MaterialPaintLayer &layer) -> bool {
      return factory.is_unchanged(layer);
    };
    const auto source_group_get = [&](const MaterialPaintLayer &layer) -> bNodeTree * {
      return factory.source_group_get(layer);
    };
    ctx.layer_tree_get = layer_tree_get;
    ctx.layer_unchanged = layer_unchanged;
    ctx.source_group_get = source_group_get;

    tree_clear(bmain, *tree);
    /* The root's own hash goes the same way: a pass that fails before the instance is wired must
     * not leave a hash that makes the next one keep a half-built root. */
    tree_hash_clear(*tree);
    BKE_paint_layers_tree_build(ma, *tree, ctx);

    /* Innermost first: a source group sits in a layer group, which sits in the root. */
    Vector<bNodeTree *> generated(factory.sources_used());
    generated.extend(factory.used());
    generated_trees_update(bmain, generated, *tree);
    factory.prune_unused();
    /* The groups are built and instantiated: record how many keyed blend nodes each holds, so a
     * later blend edit can tell a whole tree from a hand-edited one. */
    paint_layers_blend_nodes_commit(ma);

    bNode *instance = instance_ensure(ma, *tree);
    if (instance == nullptr) {
      report_out();
      return false;
    }
    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);
    /* The root was rebuilt with a new interface: the instance has to follow it before its outputs
     * are looked up for the routing below. */
    nodes::update_node_declaration_and_sockets(*ma.nodetree, *instance);

    principled_ensure(ma, report);
    wire_instance_to_material(ma, *tree, *instance, report);

    BKE_ntree_update_after_single_tree_change(bmain, *ma.nodetree);

    /* A group that was kept unchanged had its value inputs written by an earlier pass; this makes
     * the instances the root just re-created carry the description's current values. */
    BKE_paint_layers_values_sync(ma);

    /* Everything was built and wired: only now do the stored hashes say "this is what exists". */
    factory.topology_hashes_commit();
    tree_root_hash_set(*tree, root_hash);
    /* A layer group kept unchanged still holds the source group of the old content. */
    source_groups_refresh(bmain, ma);

    relations_changed = factory.groups_created() || factory.groups_deleted() || created_tree ||
                        report.replaced_foreign_tree;
    PBR_LAYERS_LOG("[PBR-Layers regen] rebuild done material=\"%s\" groups_created=%d groups_deleted=%d\n",
           ma.id.name + 2,
           int(factory.groups_created()),
           int(factory.groups_deleted()));
    regenerate_tags(bmain, ma, *tree, relations_changed);
  }

  /* The tier itself lives only in memory, so a file written while scrubbing can hold dynamic group
   * instances no loader could interpret. This bit is the persisted dirty marker: a loader that sees
   * it asks for a static rebuild. Written on both paths, since a kept tree can already carry dynamic
   * instances from an earlier pass in this session. */
  prop_int_set(tree->id.properties,
               TREE_ROOT_BLEND_DYNAMIC_PROP,
               paint_layers_blend_any_dynamic(ma) ? 1 : 0);

  ma.paint_layers_flag &= ~(MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC);

  /* A rebuild re-creates the instance's sockets, so the viewport pass viewer has to find the
   * channel result again. Idempotent and cheap when the combined shading is shown. */
  BKE_paint_layers_view_pass_sync(&bmain, ma);

  report.relations_changed = relations_changed;
  report_out();
  return true;
}

void BKE_paint_layers_regenerate_tagged(Main &bmain)
{
  /* Regeneration creates IDs and writes node trees, so it may only run on the main thread.
   * #BKE_scene_graph_update_tagged and #BKE_scene_graph_update_for_newframe_ex are also reached from
   * render and preview worker threads, so the guard belongs here rather than at each call site. */
  if (!BLI_thread_is_main()) {
    return;
  }
  /* Only a material that owns its description and its tree may be regenerated. A localized or
   * evaluated copy shares the pointer with the original (see #BKE_paint_layers_generate_copy_data),
   * and rebuilding it from a copy would rewrite the original's tree. */
  const int no_regen_tags = ID_TAG_LOCALIZED | ID_TAG_COPIED_ON_EVAL | ID_TAG_NO_MAIN;
  for (Material &ma : bmain.materials) {
    if ((ma.id.tag & no_regen_tags) != 0 || !BKE_paint_layers_is_layered(ma) ||
        !ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id))
    {
      continue;
    }
    /* Drain the bake subscriptions: a pixel edit to a source map shows up here and marks the
     * material for the planner, the same point the tree is brought current. Then the planner
     * re-bakes the light rows whose stored hash no longer matches, on the main thread; a heavy row
     * is left to the editor's worker job. A bake that was written, or a row that stopped being
     * substituted, tags the rebuild below. */
    BKE_paint_layers_bake_notice_changes(ma);
    /* A source edit that changed what a Material row's group is made of (a value that became a
     * texture) marks the rebuild too; one that only moved a value is the evaluation's sync. */
    BKE_paint_layers_material_sources_notice(ma);
    BKE_paint_layers_bake_plan_run(bmain, ma);
    const bool needs_regen =
        (ma.paint_layers_flag & (MA_PAINT_LAYERS_REGEN | MA_PAINT_LAYERS_BLEND_SYNC)) != 0 ||
        ma.paint_layers_tree == nullptr;
    if (needs_regen) {
      PBR_LAYERS_LOG(
          "[PBR-Layers regen] tagged material=\"%s\" flag_regen=%d flag_blend_sync=%d "
          "bake_stale=%d no_tree=%d\n",
          ma.id.name + 2,
          int((ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0),
          int((ma.paint_layers_flag & MA_PAINT_LAYERS_BLEND_SYNC) != 0),
          int((ma.paint_layers_flag & MA_PAINT_LAYERS_BAKE_STALE) != 0),
          int(ma.paint_layers_tree == nullptr));
      BKE_paint_layers_regenerate(bmain, ma);
    }
    else {
      /* A source edit that moved no structure: the groups copying its graph follow it in place. */
      source_groups_refresh(bmain, ma);
    }
  }
}

/**
 * Whether a copy of a material becomes a second *owner* of the generated tree. A copy that lives
 * outside Main, takes no user references, or is the depsgraph's evaluated copy is only a view of
 * its source: it keeps the owner uid and shares the tree pointer, and the generic copy pass does
 * the user counting. Everything else is a real copy and gets a tree and a uid of its own.
 */
static bool copy_owns_generated_tree(const Main *bmain, const int copy_flag)
{
  constexpr int view_flags = LIB_ID_CREATE_NO_MAIN | LIB_ID_CREATE_NO_USER_REFCOUNT |
                             LIB_ID_COPY_SET_COPIED_ON_WRITE;
  return bmain != nullptr && (copy_flag & view_flags) == 0;
}

/**
 * Give \a rows (and everything they hold) their own copy of every editable map, so painting on a
 * duplicated material never reaches the original. \a clones keeps one copy per source image: two
 * rows that shared a map still share it, inside the duplicate.
 */
static void maps_copy(Main &bmain, ListBaseT<MaterialPaintLayer> &rows, Map<Image *, Image *> &clones)
{
  for (MaterialPaintLayer &row : rows) {
    /* A baked row of the copy starts with no cache of its own: sharing the original's maps would
     * let a re-bake of the copy change what the original shows. The planner bakes it again. */
    if (row.bake != nullptr) {
      for (Image *&baked : row.bake->images) {
        baked = nullptr;
      }
      row.bake->coverage = nullptr;
      row.bake->hash[0] = 0;
      row.bake->hash[1] = 0;
    }
    paint_layer_dependencies(row, [&](PaintLayerDependency &dep) {
      if (dep.kind != PaintLayerDependencyKind::ImageMap || *dep.slot == nullptr ||
          !ID_IS_EDITABLE(*dep.slot))
      {
        return;
      }
      Image *&image = reinterpret_cast<Image *&>(*dep.slot);
      Image *clone = clones.lookup_default(image, nullptr);
      if (clone == nullptr) {
        clone = id_cast<Image *>(BKE_id_copy(&bmain, &image->id));
        if (clone == nullptr) {
          return;
        }
        /* Like the tree copy below: #BKE_id_copy hands back a reference of its own, and the generic
         * copy pass adds the single reference the channel owns. */
        id_us_min(&clone->id);
        clones.add(image, clone);
      }
      image = clone;
    });
    maps_copy(bmain, row.mask_stack, clones);
    maps_copy(bmain, row.effects, clones);
    maps_copy(bmain, row.children, clones);
  }
}

void BKE_paint_layers_generate_copy_data(Main *bmain,
                                         Material &ma_dst,
                                         const Material &ma_src,
                                         const int copy_flag)
{
  if (!copy_owns_generated_tree(bmain, copy_flag)) {
    /* The owner uid was copied with the struct. */
    ma_dst.paint_layers_tree = ma_src.paint_layers_tree;
    return;
  }

  /* A copy has no debounce timer and no job of its own: a mark copied with the struct would stay.
   */
  ma_dst.paint_layers_flag &= ~MA_PAINT_LAYERS_BAKE_SCHEDULED;

  /* The uid keys the generated tree, so sharing it would make two materials' generated groups
   * indistinguishable. */
  if (!BLI_uuid_is_nil(ma_src.paint_layers_owner_uid)) {
    ma_dst.paint_layers_owner_uid = BLI_uuid_generate_random();
  }

  /* The maps are pixels the material paints into, so a duplicate that kept the source's images
   * would change the original on its first stroke. The copied generated groups still name the
   * source's images; the regeneration this tags rewires them to the copies. */
  if (bmain != nullptr && ma_dst.paint_layers.first != nullptr) {
    Map<Image *, Image *> clones;
    maps_copy(*bmain, ma_dst.paint_layers, clones);
    ma_dst.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
  }

  if (ma_src.paint_layers_tree == nullptr) {
    ma_dst.paint_layers_tree = nullptr;
    return;
  }

  bNodeTree *tree_copy = id_cast<bNodeTree *>(BKE_id_copy(bmain, &ma_src.paint_layers_tree->id));
  if (tree_copy == nullptr) {
    ma_dst.paint_layers_tree = nullptr;
    return;
  }
  /* #BKE_id_copy hands back a reference of its own; the generic copy pass below adds the single
   * reference the material owns, so drop the returned one to end at exactly that. */
  id_us_min(&tree_copy->id);
  ma_dst.paint_layers_tree = tree_copy;
  tree_owner_uid_set(*tree_copy, ma_dst.paint_layers_owner_uid);
  /* Descend into the layer groups while the source owner still names them; a copy shares no tree
   * with the original, and the layer markers stay the same. */
  layer_trees_copy(
      bmain, *tree_copy, ma_src.paint_layers_owner_uid, ma_dst.paint_layers_owner_uid);

  /* The copy's embedded tree carries an instance node still pointing at the source tree; re-point
   * it before the generic copy pass walks the destination, so the copy owns only its own tree. */
  if (ma_dst.nodetree != nullptr) {
    for (bNode &node : ma_dst.nodetree->nodes) {
      bUUID node_uid = BLI_uuid_nil();
      if (!uid_prop_get(node.prop, INSTANCE_OWNER_PROP, node_uid) ||
          !BLI_uuid_equal(node_uid, ma_src.paint_layers_owner_uid))
      {
        continue;
      }
      if (node.id != nullptr) {
        id_us_min(node.id);
      }
      node.id = &tree_copy->id;
      id_us_plus(&tree_copy->id);
      instance_uid_set(node, ma_dst.paint_layers_owner_uid);
    }
  }
}

static void layer_trees_reassign_owner(bNodeTree &tree,
                                       const bUUID &old_owner,
                                       const bUUID &new_owner)
{
  for (bNode &node : tree.nodes) {
    if (!node.is_group() || node.id == nullptr || GS(node.id->name) != ID_NT) {
      continue;
    }
    bNodeTree *group = id_cast<bNodeTree *>(node.id);
    if (group == nullptr || !BLI_uuid_equal(tree_owner_uid_get(*group), old_owner)) {
      continue;
    }
    tree_owner_uid_set(*group, new_owner);
    layer_trees_reassign_owner(*group, old_owner, new_owner);
  }
}

void BKE_paint_layers_owner_uid_changed(Main & /*bmain*/, Material &ma, const bUUID &old_uid)
{
  if (ma.paint_layers_tree != nullptr &&
      BLI_uuid_equal(tree_owner_uid_get(*ma.paint_layers_tree), old_uid))
  {
    tree_owner_uid_set(*ma.paint_layers_tree, ma.paint_layers_owner_uid);
    layer_trees_reassign_owner(*ma.paint_layers_tree, old_uid, ma.paint_layers_owner_uid);
  }
  if (ma.nodetree != nullptr) {
    for (bNode &node : ma.nodetree->nodes) {
      bUUID node_uid = BLI_uuid_nil();
      if (uid_prop_get(node.prop, INSTANCE_OWNER_PROP, node_uid) &&
          BLI_uuid_equal(node_uid, old_uid))
      {
        instance_uid_set(node, ma.paint_layers_owner_uid);
      }
    }
  }
  BKE_paint_layers_tag_edited(ma);
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
}

/* -------------------------------------------------------------------- */
/** \name Undo
 * \{ */

/**
 * The editable layered materials of \a bmain that own a generated tree (have an owner uid). A
 * linked or override material is skipped: undo must never rewrite the trees of another file.
 */
static Vector<Material *> layered_materials_with_owner(Main &bmain)
{
  Vector<Material *> materials;
  for (Material &ma : bmain.materials) {
    if (BKE_paint_layers_is_layered(ma) && !BLI_uuid_is_nil(ma.paint_layers_owner_uid) &&
        ID_IS_EDITABLE(&ma.id) && !ID_IS_OVERRIDE_LIBRARY(&ma.id))
    {
      materials.append(&ma);
    }
  }
  return materials;
}

/**
 * The generated trees each of \a materials owns, found in one pass over \a bmain's trees rather
 * than one pass per material: every undo step goes through here for each layered material.
 */
static Vector<Vector<bNodeTree *>> owned_trees_by_material(Main &bmain,
                                                           const Span<Material *> materials)
{
  Vector<Vector<bNodeTree *>> owned(materials.size());
  for (bNodeTree &tree : bmain.nodetrees) {
    const bUUID owner = tree_owner_uid_get(tree);
    if (BLI_uuid_is_nil(owner)) {
      continue;
    }
    for (const int i : materials.index_range()) {
      if (BLI_uuid_equal(owner, materials[i]->paint_layers_owner_uid)) {
        owned[i].append(&tree);
        break;
      }
    }
  }
  return owned;
}

void BKE_paint_layers_undo_prepare(Main &bmain)
{
  const Vector<Material *> materials = layered_materials_with_owner(bmain);
  if (materials.is_empty()) {
    return;
  }
  const Vector<Vector<bNodeTree *>> owned_by_material = owned_trees_by_material(bmain, materials);

  for (const int i : materials.index_range()) {
    Material &ma = *materials[i];
    /* Not gated on the undo tags: the decode itself walks every ID pointer of the data-blocks it
     * keeps (the user-count recompute) before any hook can run after it, and the tags do not say
     * which maps and groups the target state lacks. The generated data is derived, so it is always
     * dropped and rebuilt by the next regeneration. */
    const Vector<bNodeTree *> &owned = owned_by_material[i];
    /* Compared by address: `node.id` is any ID type, so it is never cast to a tree here. */
    Set<const ID *> owned_ids;
    for (const bNodeTree *tree : owned) {
      owned_ids.add(&tree->id);
    }

    /* The material's own tree holds the instances of the generated groups. */
    if (ma.nodetree != nullptr) {
      for (bNode &node : ma.nodetree->nodes) {
        if (node.id != nullptr && owned_ids.contains(node.id)) {
          node.id = nullptr;
        }
      }
    }

    for (bNodeTree *tree : owned) {
      /* Pointers are valid here: the previous decode's #BKE_paint_layers_undo_validate cleared the
       * dangling ones, so reading the type is safe. Only maps and groups are generated data. */
      for (bNode &node : tree->nodes) {
        if (node.id != nullptr && ELEM(GS(node.id->name), ID_IM, ID_NT)) {
          node.id = nullptr;
        }
        for (bNodeSocket &sock : node.inputs) {
          if (sock.type == SOCK_IMAGE) {
            sock.default_value_typed<bNodeSocketValueImage>()->value = nullptr;
          }
        }
        for (bNodeSocket &sock : node.outputs) {
          if (sock.type == SOCK_IMAGE) {
            sock.default_value_typed<bNodeSocketValueImage>()->value = nullptr;
          }
        }
      }
      /* The maps and groups are gone, so a stored hash no longer describes what the tree holds. A
       * tree the decode keeps as it is would otherwise be taken for unchanged (a kept root, a reused
       * group) and never get them back; a re-read tree carries its own hash from the snapshot. */
      tree_hash_clear(*tree);
      /* The evaluated copy still names the old data-blocks until it is copied again. */
      DEG_id_tag_update(&tree->id, ID_RECALC_SYNC_TO_EVAL);
    }
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  }
}

/** Null an ID pointer that is not in \a live. \return True when it was dangling. */
template<typename T> static bool undo_dangling_id_clear(T *&id, const Set<const ID *> &live)
{
  if (id == nullptr || live.contains(reinterpret_cast<const ID *>(id))) {
    return false;
  }
  id = nullptr;
  return true;
}

/** Null the ID pointers of \a tree that the decode freed. \return True when any was dangling. */
static bool undo_tree_dangling_clear(bNodeTree &tree, const Set<const ID *> &live)
{
  bool cleared = false;
  const auto clear_socket = [&](bNodeSocket &sock) {
    switch (sock.type) {
      case SOCK_OBJECT:
        cleared |= undo_dangling_id_clear(
            sock.default_value_typed<bNodeSocketValueObject>()->value, live);
        break;
      case SOCK_IMAGE:
        cleared |= undo_dangling_id_clear(
            sock.default_value_typed<bNodeSocketValueImage>()->value, live);
        break;
      case SOCK_COLLECTION:
        cleared |= undo_dangling_id_clear(
            sock.default_value_typed<bNodeSocketValueCollection>()->value, live);
        break;
      case SOCK_TEXTURE:
        cleared |= undo_dangling_id_clear(
            sock.default_value_typed<bNodeSocketValueTexture>()->value, live);
        break;
      case SOCK_MATERIAL:
        cleared |= undo_dangling_id_clear(
            sock.default_value_typed<bNodeSocketValueMaterial>()->value, live);
        break;
      default:
        break;
    }
  };
  for (bNode &node : tree.nodes) {
    cleared |= undo_dangling_id_clear(node.id, live);
    for (bNodeSocket &sock : node.inputs) {
      clear_socket(sock);
    }
    for (bNodeSocket &sock : node.outputs) {
      clear_socket(sock);
    }
  }
  return cleared;
}

void BKE_paint_layers_undo_validate(Main &bmain)
{
  /* Most files have no layered material: skip building the set of every ID of Main for them. */
  const Vector<Material *> materials = layered_materials_with_owner(bmain);
  if (materials.is_empty()) {
    return;
  }
  const Vector<Vector<bNodeTree *>> owned_by_material = owned_trees_by_material(bmain, materials);

  /* Compared by address only, a freed data-block is never dereferenced. A pointer that is in the
   * set is left alone, so this cannot damage a tree the decode re-read or kept intact. */
  Set<const ID *> live;
  ID *id_iter;
  FOREACH_MAIN_ID_BEGIN (&bmain, id_iter) {
    live.add(id_iter);
  }
  FOREACH_MAIN_ID_END;

  for (const int i : materials.index_range()) {
    Material &ma = *materials[i];
    undo_dangling_id_clear(ma.paint_layers_tree, live);

    /* A bake cache whose maps the decode freed is not a cache any more: clearing the hash makes the
     * planner render the row again. A Material row whose source the decode freed reads nothing. */
    BKE_paint_layers_foreach(
        ma, [&](MaterialPaintLayer &row, MaterialPaintLayer * /*parent*/) -> bool {
          undo_dangling_id_clear(row.material, live);
          if (row.bake == nullptr) {
            return true;
          }
          bool dangling = undo_dangling_id_clear(row.bake->coverage, live);
          for (Image *&image : row.bake->images) {
            dangling |= undo_dangling_id_clear(image, live);
          }
          if (dangling) {
            row.bake->hash[0] = 0;
            row.bake->hash[1] = 0;
          }
          return true;
        });

    /* The generated trees a material owns, which a decode may have kept as they were while the
     * maps and groups they name were freed. The material's own tree holds the group instances. */
    Vector<bNodeTree *> trees = owned_by_material[i];
    if (ma.nodetree != nullptr) {
      trees.append(ma.nodetree);
    }
    for (bNodeTree *tree : trees) {
      undo_tree_dangling_clear(*tree, live);
    }
    /* Always regenerated: a material the decode re-read takes its flag from the snapshot, which
     * drops the one #BKE_paint_layers_undo_prepare set, and a tree it kept has had its maps and
     * groups nulled there, which no dangling check can tell from a deliberately empty pointer. */
    ma.paint_layers_flag |= MA_PAINT_LAYERS_REGEN;
    DEG_id_tag_update(&ma.id, ID_RECALC_SHADING | ID_RECALC_SYNC_TO_EVAL);
  }
  /* The pixel-edit subscriptions name images by address: drop the ones the decode freed. */
  BKE_paint_layers_bake_subscriptions_validate(bmain);
}

/** \} */

}  // namespace blender
