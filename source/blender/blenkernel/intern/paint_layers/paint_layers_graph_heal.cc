/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 *
 * Self-heal for a layered material's generated graph, see
 * #BKE_paint_layers_graph_check_and_heal. The detection is the fatal half of the former read-only
 * graph inspection: only the defects that on their own mean the graph must be rebuilt are looked
 * for, and nothing is reported -- a broken graph is repaired instead of described.
 */

#include "BKE_paint_layers_graph_heal.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_listbase_iterator.hh"
#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_threads.h"
#include "BLI_ustring.hh"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_node.hh"
#include "BKE_node_runtime.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_generate.hh"

#include "DNA_ID.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"

#include "DEG_depsgraph.hh"

#include "paint_layers_generate_intern.hh"

namespace blender {

/* The row map helpers are declared in #paint_layers_generate_intern.hh and shared with the paint
 * target; this is their one definition. */
namespace bke::paint_layers {

Vector<const Image *> row_map_images(const MaterialPaintLayer &row)
{
  Vector<const Image *> images;
  const auto collect = [&](const MaterialPaintLayer &item) {
    for (const int i : IndexRange(item.channels_num)) {
      if (item.channels[i].image != nullptr) {
        images.append(item.channels[i].image);
      }
    }
    if (item.bake != nullptr) {
      for (const Image *image : item.bake->images) {
        if (image != nullptr) {
          images.append(image);
        }
      }
      if (item.bake->coverage != nullptr) {
        images.append(item.bake->coverage);
      }
    }
  };
  collect(row);
  for (const MaterialPaintLayer &effect : row.effects) {
    collect(effect);
  }
  for (const MaterialPaintLayer &item : row.mask_stack) {
    collect(item);
  }
  return images;
}

bool row_has_maps(const Material &ma, const MaterialPaintLayer &row)
{
  if (!row_map_images(row).is_empty()) {
    return true;
  }
  if (row.source == MA_PAINT_LAYER_SOURCE_MESH_MAP) {
    for (const MaterialMeshMapSlot &slot : ma.mesh_map_slots) {
      if (slot.image != nullptr) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace bke::paint_layers

namespace {

using bke::paint_layers::layer_tree_marker_get;
using bke::paint_layers::row_has_maps;
using bke::paint_layers::source_group_row_get;
using bke::paint_layers::tree_hash_clear;
using bke::paint_layers::tree_owner_uid_get;

/* The state of one detection pass over a material's generated graph. */
struct GraphHeal {
  const Material &ma;

  /* Every image / node tree ID currently in the database; the dangling-pointer checks compare
   * against these instead of walking the lists per pointer. */
  Set<const void *> image_ids;
  Set<const void *> tree_ids;
  /* The trees already visited, so a shared group is walked once and a cycle stops. */
  Set<const void *> visited_trees;

  /* Keyed by the blender::UUID wrapper, not the raw bUUID: only the wrapper carries the `hash()`
   * that blender::Map needs. */
  Map<UUID, const bNodeTree *> layer_tree_by_marker;

  /* The stack row each generated layer group belongs to, and whether the stack still holds at
   * least one map at all. An image defect is a rebuild trigger only where the builder itself
   * places image nodes that always carry a map -- the root and the row groups whose records hold
   * maps. A source group copies the user's own graph, where an imageless Image Texture can be
   * intentional, so those never trigger. */
  Map<const bNodeTree *, const MaterialPaintLayer *> layer_tree_rows;
  bool any_row_maps = false;

  /* Set when a defect that demands a rebuild was found. */
  bool fatal = false;

  GraphHeal(Main &bmain, const Material &ma) : ma(ma)
  {
    for (const Image &image : bmain.images) {
      image_ids.add(&image);
    }
    for (const bNodeTree &tree : bmain.nodetrees) {
      tree_ids.add(&tree);
      if (!BLI_uuid_equal(tree_owner_uid_get(tree), ma.paint_layers_owner_uid)) {
        continue;
      }
      bUUID marker = BLI_uuid_nil();
      /* A stale duplicate could carry a marker twice; the first tree wins, the same choice the
       * build makes. */
      if (layer_tree_marker_get(tree, marker) && !layer_tree_by_marker.contains(marker)) {
        layer_tree_by_marker.add(marker, &tree);
      }
    }
  }
};

/* Walk \a tree and everything nested in it, marking #GraphHeal::fatal when a defect that demands a
 * rebuild is found. Only reads.
 *
 * \param image_scope: whether an image defect found in this tree or in a nested non-source tree
 * below it counts as a rebuild trigger, see #GraphHeal::layer_tree_rows. */
static void heal_tree_walk(GraphHeal &heal, const bNodeTree &tree, const bool image_scope)
{
  if (heal.visited_trees.contains(&tree)) {
    return;
  }
  heal.visited_trees.add(&tree);

  if (!heal.tree_ids.contains(&tree)) {
    /* Reached through a group node although the database does not own it: a use-after-free in the
     * making, and the strongest sign of the undo bug this check hunts. The owner's stack tree is
     * entered through the same path, so a stack tree outside #Main lands here too. */
    heal.fatal = true;
  }

  /* Refine the image-defect scope for this tree. The root and a row group whose stack records
   * still hold maps are places where the builder only ever creates image nodes that carry a map,
   * so a lost one there is a defect to rebuild over. A source group copies the user's own graph,
   * where an imageless Image Texture can be intentional; every other nested tree inherits the
   * scope it was reached through. */
  bool scope = image_scope;
  if (const MaterialPaintLayer *const *row = heal.layer_tree_rows.lookup_ptr(&tree)) {
    scope = row_has_maps(heal.ma, **row);
  }
  else if (&tree == heal.ma.paint_layers_tree) {
    scope = heal.any_row_maps;
  }
  else {
    bUUID source_marker = BLI_uuid_nil();
    if (source_group_row_get(tree, source_marker)) {
      scope = false;
    }
  }

  for (const bNode &node : tree.nodes) {
    if (node.is_group()) {
      const ID *group_id = node.id;
      if (group_id == nullptr) {
        heal.fatal = true;
        continue;
      }
      if (GS(group_id->name) != ID_NT) {
        /* Not a defect this check heals over (the builder never writes one): the walk cannot
         * descend into a non-tree id, so it stops here. */
        continue;
      }
      const bNodeTree *group = id_cast<const bNodeTree *>(group_id);
      if (!heal.tree_ids.contains(group)) {
        heal.fatal = true;
        continue;
      }
      /* A node whose socket counts no longer match its group's interface is stale: its extra
       * sockets read their fallback values and its missing ones drop their links, which shows as
       * black or as lost inputs. */
      group->ensure_interface_cache();
      const int node_inputs = BLI_listbase_count(&node.inputs);
      const int node_outputs = BLI_listbase_count(&node.outputs);
      const int iface_inputs = group->interface_inputs().size();
      const int iface_outputs = group->interface_outputs().size();
      if (node_inputs != iface_inputs || node_outputs != iface_outputs) {
        heal.fatal = true;
      }
      heal_tree_walk(heal, *group, scope);
      continue;
    }
    if (node.is_type("ShaderNodeTexImage"_ustr) || node.is_type("ShaderNodeTexEnvironment"_ustr))
    {
      if (!scope) {
        continue;
      }
      const ID *image_id = node.id;
      if (image_id == nullptr) {
        heal.fatal = true;
        continue;
      }
      if (GS(image_id->name) != ID_IM) {
        /* Not a defect this check heals over (the builder never writes one). */
        continue;
      }
      if (!heal.image_ids.contains(image_id)) {
        heal.fatal = true;
      }
    }
  }
}

}  // namespace

bool BKE_paint_layers_graph_check_and_heal(Main &bmain, Material &ma)
{
  if (!BKE_paint_layers_is_layered(ma) || ma.paint_layers.first == nullptr) {
    return false;
  }

  const bNodeTree *root = ma.paint_layers_tree;
  if (root == nullptr) {
    /* No generated tree to check: a pending regeneration builds it and the stack description
     * itself is whole, so there is nothing this check can heal here. */
    return false;
  }

  /* Refuse before any walk: the checks below read node trees and the rebuild writes them, so they
   * run on the main thread only, and a linked or override material's trees are owned by another
   * file and are never rebuilt here. Every hook that reaches this function passes both guards;
   * refusing first keeps a layered material the caller cannot heal from paying for the walk. */
  if (!BLI_thread_is_main()) {
    return false;
  }
  if (!ID_IS_EDITABLE(&ma.id) || ID_IS_OVERRIDE_LIBRARY(&ma.id)) {
    return false;
  }

  /* Map every generated layer group to its stack row before the walk, so the image defects found
   * on the way know whether they demand a rebuild. */
  GraphHeal heal(bmain, ma);
  BKE_paint_layers_foreach(ma, [&](const MaterialPaintLayer &row,
                                   const MaterialPaintLayer * /*parent*/) -> bool {
    const bNodeTree *const *found = heal.layer_tree_by_marker.lookup_ptr(row.marker);
    if (found != nullptr) {
      heal.layer_tree_rows.add(*found, &row);
    }
    heal.any_row_maps = heal.any_row_maps || row_has_maps(ma, row);
    return true;
  });
  heal_tree_walk(heal, *root, true);
  if (!heal.fatal) {
    /* The graph is whole: touch nothing. */
    return false;
  }

  /* Drop the stored topology hashes of every tree the material owns first. The regeneration is
   * hash-guarded: a root or row group re-read from the memfile snapshot carries the hash of the
   * state that was written, and that state can be exactly the broken one, so the guarded pass
   * would keep it. With the hashes gone the regeneration rebuilds every group from the
   * description, the same unconditional rebuild an edit of the stack schedules. */
  for (bNodeTree &tree : bmain.nodetrees) {
    if (BLI_uuid_is_nil(ma.paint_layers_owner_uid) ||
        !BLI_uuid_equal(tree_owner_uid_get(tree), ma.paint_layers_owner_uid))
    {
      continue;
    }
    tree_hash_clear(tree);
  }

  /* The same regen request the stack edits make; #BKE_paint_layers_regenerate clears the edited
   * flag itself and pushes no undo step, and nothing in it re-enters the undo code. */
  BKE_paint_layers_tag_edited(ma);
  BKE_paint_layers_regenerate(bmain, ma);

  /* The rebuild rewrote the material's shading graph: tag it, so the healed graph shows without
   * waiting for the next tagged update. Raising a notifier is the caller's business. */
  DEG_id_tag_update(&ma.id, ID_RECALC_SHADING);
  return true;
}

}  // namespace blender
