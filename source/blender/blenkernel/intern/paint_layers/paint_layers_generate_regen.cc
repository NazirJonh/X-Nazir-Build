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

#include <utility>

#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_span.hh"
#include "BLI_string.h"
#include "BLI_threads.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_idprop.hh"
#include "BKE_lib_id.hh"
#include "BKE_lib_query.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_material_composite.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "NOD_socket.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_uuid_types.h"

#include "paint_layers_generate_intern.hh"
#include "paint_layers_generate_layout.hh"
#include "paint_layers_intern.hh"

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
      : bmain_(bmain), ma_(ma), wired_channels_(wired_channels)
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
    }
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
    const uint64_t topology = paint_layers_layer_topology_hash(ma_, layer, wired_channels_);
    for (bNodeTree *candidate : old_) {
      if (used_.contains(candidate)) {
        continue;
      }
      bUUID marker = BLI_uuid_nil();
      if (layer_tree_marker_get(*candidate, marker) && BLI_uuid_equal(marker, layer.marker)) {
        uint64_t stored = 0;
        if (tree_topology_hash_get(*candidate, stored) && stored == topology) {
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
        pending_hashes_.append({candidate, topology});
        if (!STREQ(candidate->id.name + 2, name)) {
          BKE_id_rename(bmain_, candidate->id, name);
        }
        used_.append(candidate);
        handed_.add(&layer, candidate);
        return candidate;
      }
    }
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
        BKE_id_delete(&bmain_, layer_tree);
        deleted_ = true;
      }
    }
    for (bNodeTree *layer_tree : used_) {
      /* The build asked for a group but did not instantiate it, so nothing owns it. */
      if (ID_REAL_USERS(&layer_tree->id) <= 0) {
        BKE_id_delete(&bmain_, layer_tree);
        deleted_ = true;
      }
    }
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
  Main &bmain_;
  Material &ma_;
  Span<int> wired_channels_;
  /** The groups this material owned before the regeneration. */
  Vector<bNodeTree *> old_;
  /** The groups handed out in this regeneration. */
  Vector<bNodeTree *> used_;
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
  if (BLI_uuid_is_nil(ma.paint_layers_owner_uid)) {
    ma.paint_layers_owner_uid = BLI_uuid_generate_random();
  }

  bool created_tree = false;
  bNodeTree *tree = root_tree_ensure(bmain, ma, report, created_tree);
  if (tree == nullptr) {
    report_out();
    return false;
  }

  PaintLayersBuildContext ctx;
  Vector<int> wired_channels = paint_layers_wired_channels(ma);
  /* The Normal chain needs the shared combine group; create it only when a Normal row exists. */
  for (const MaterialPaintLayer &layer : ma.paint_layers) {
    if (leaf_participates(ma, layer, PAINT_MATERIAL_CHANNEL_NORMAL)) {
      ctx.normal_combine_group = BKE_paint_material_normal_combine_group_ensure(bmain);
      break;
    }
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
  if (keep_root) {
    BKE_paint_layers_values_sync(ma);
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
    ctx.layer_tree_get = layer_tree_get;
    ctx.layer_unchanged = layer_unchanged;

    tree_clear(bmain, *tree);
    /* The root's own hash goes the same way: a pass that fails before the instance is wired must
     * not leave a hash that makes the next one keep a half-built root. */
    tree_hash_clear(*tree);
    BKE_paint_layers_tree_build(ma, *tree, ctx);

    generated_trees_update(bmain, factory.used(), *tree);
    factory.prune_unused();

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

    relations_changed = factory.groups_created() || factory.groups_deleted() || created_tree ||
                        report.replaced_foreign_tree;
    regenerate_tags(bmain, ma, *tree, relations_changed);
  }

  ma.paint_layers_flag &= ~MA_PAINT_LAYERS_REGEN;

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
    if ((ma.id.tag & no_regen_tags) != 0 || !BKE_paint_layers_is_layered(ma)) {
      continue;
    }
    const bool needs_regen = (ma.paint_layers_flag & MA_PAINT_LAYERS_REGEN) != 0 ||
                             ma.paint_layers_tree == nullptr;
    if (needs_regen) {
      BKE_paint_layers_regenerate(bmain, ma);
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

  /* The uid keys the generated tree, so sharing it would make two materials' generated groups
   * indistinguishable. */
  if (!BLI_uuid_is_nil(ma_src.paint_layers_owner_uid)) {
    ma_dst.paint_layers_owner_uid = BLI_uuid_generate_random();
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

/* -------------------------------------------------------------------- */
/** \name Undo
 * \{ */

/** The layered materials of \a bmain that own a generated tree (have an owner uid). */
static Vector<Material *> layered_materials_with_owner(Main &bmain)
{
  Vector<Material *> materials;
  for (Material &ma : bmain.materials) {
    if (BKE_paint_layers_is_layered(ma) && !BLI_uuid_is_nil(ma.paint_layers_owner_uid)) {
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
}

/** \} */

}  // namespace blender
