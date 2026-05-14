/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spnode
 */

#include <algorithm>

#include "MEM_guardedalloc.h"

#include "DNA_array_utils.hh"
#include "DNA_ID.h"
#include "DNA_node_types.h"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "BLI_easing.h"
#include "BLI_listbase.h"
#include "BLI_math_geom.h"
#include "BLI_rect.h"
#include "BLI_stack.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "BKE_context.hh"
#include "BKE_anim_data.hh"
#include "BKE_fcurve.hh"
#include "BKE_fcurve_driver.h"
#include "BKE_idprop.hh"
#include "BKE_main_invariants.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_screen.hh"

#include "ED_node.hh" /* own include */
#include "ED_keyframing.hh"
#include "ED_render.hh"
#include "ED_screen.hh"
#include "ED_space_api.hh"
#include "ED_viewer_path.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"
#include "RNA_prototypes.hh"
#include "RNA_path.hh"
#include "RNA_types.hh"


#include "WM_api.hh"
#include "WM_types.hh"

#include "UI_interface_icons.hh"
#include "UI_resources.hh"
#include "UI_view2d.hh"
#include "UI_interface.hh"
#include "UI_interface_c.hh"

/* For accessing Button::rnaprop and Button::rnapoin. */
#include "../interface/interface_intern.hh"

#include "NOD_geo_viewer.hh"
#include "NOD_node_declaration.hh"
#include "NOD_socket.hh"
#include "NOD_socket_items.hh"

#include "node_intern.hh" /* own include */

namespace blender {

struct NodeInsertOfsData {
  bNodeTree *ntree;
  bNode *insert;      /* Inserted node. */
  bNode *prev, *next; /* Previous/next node in the chain. */

  wmTimer *anim_timer;

  float offset_x; /* Offset to apply to node chain. */
};

namespace ed::space_node {

static void clear_picking_highlight(ListBaseT<bNodeLink> *links)
{
  for (bNodeLink &link : *links) {
    link.flag &= ~NODE_LINK_TEMP_HIGHLIGHT;
  }
}

/* -------------------------------------------------------------------- */
/** \name Add Node
 * \{ */

static bNodeLink create_drag_link(bNode &node, bNodeSocket &socket)
{
  bNodeLink oplink{};
  if (socket.in_out == SOCK_OUT) {
    oplink.fromnode = &node;
    oplink.fromsock = &socket;
  }
  else {
    oplink.tonode = &node;
    oplink.tosock = &socket;
  }
  oplink.flag |= NODE_LINK_VALID;
  return oplink;
}

static void pick_link(bNodeLinkDrag &nldrag,
                      SpaceNode &snode,
                      bNode *node,
                      bNodeLink &link_to_pick)
{
  clear_picking_highlight(&snode.edittree->links);

  bNodeLink link = create_drag_link(*link_to_pick.fromnode, *link_to_pick.fromsock);

  bke::node_link_set_mute(*snode.edittree, link, (link_to_pick.flag & NODE_LINK_MUTED));

  /* So we can restore order on cancel. */
  if (link_to_pick.tosock->is_multi_input()) {
    link.multi_input_sort_id = link_to_pick.multi_input_sort_id;
  }

  nldrag.links.append(link);
  bke::node_remove_link(snode.edittree, link_to_pick);
  snode.edittree->ensure_topology_cache();
  BLI_assert(nldrag.last_node_hovered_while_dragging_a_link != nullptr);
  update_multi_input_indices_for_removed_links(*nldrag.last_node_hovered_while_dragging_a_link);

  /* Send changed event to original link->tonode. */
  if (node) {
    BKE_ntree_update_tag_node_property(snode.edittree, node);
  }
}

static void pick_input_link_by_link_intersect(const bContext &C,
                                              wmOperator &op,
                                              bNodeLinkDrag &nldrag,
                                              const float2 &cursor)
{
  SpaceNode *snode = CTX_wm_space_node(&C);
  ARegion *region = CTX_wm_region(&C);
  bNodeTree &node_tree = *snode->edittree;

  float2 drag_start;
  RNA_float_get_array(op.ptr, "drag_start", drag_start);
  bNodeSocket *socket = node_find_indicated_socket(*snode, *region, drag_start, SOCK_IN);
  bNode &node = socket->owner_node();

  /* Distance to test overlapping of cursor on link. */
  const float cursor_link_touch_distance = 12.5f * UI_SCALE_FAC;

  bNodeLink *link_to_pick = nullptr;
  clear_picking_highlight(&node_tree.links);
  for (bNodeLink *link : socket->directly_linked_links()) {
    /* Test if the cursor is near a link. */
    std::array<float2, NODE_LINK_RESOL + 1> coords;
    node_link_bezier_points_evaluated(*link, coords);

    for (const int i : IndexRange(coords.size() - 1)) {
      const float distance = dist_squared_to_line_segment_v2(cursor, coords[i], coords[i + 1]);
      if (distance < cursor_link_touch_distance) {
        link_to_pick = link;
        nldrag.last_picked_multi_input_socket_link = link_to_pick;
      }
    }
  }

  /* If no linked was picked in this call, try using the one picked in the previous call.
   * Not essential for the basic behavior, but can make interaction feel a bit better if
   * the mouse moves to the right and loses the "selection." */
  if (!link_to_pick) {
    bNodeLink *last_picked_link = nldrag.last_picked_multi_input_socket_link;
    if (last_picked_link) {
      link_to_pick = last_picked_link;
    }
  }

  if (link_to_pick) {
    /* Highlight is set here and cleared in the next iteration or if the operation finishes. */
    link_to_pick->flag |= NODE_LINK_TEMP_HIGHLIGHT;
    ED_area_tag_redraw(CTX_wm_area(&C));

    if (!node_find_indicated_socket(*snode, *region, cursor, SOCK_IN)) {
      pick_link(nldrag, *snode, &node, *link_to_pick);
    }
  }
}

static bool socket_is_available(const bNodeTree *ntree, bNodeSocket *sock, const bool allow_used)
{
  ntree->ensure_topology_cache();
  if (!sock->is_visible()) {
    return false;
  }

  if (!allow_used && (sock->flag & SOCK_IS_LINKED)) {
    /* Multi input sockets are available (even if used). */
    if (!(sock->flag & SOCK_MULTI_INPUT)) {
      return false;
    }
  }

  return true;
}

static bNodeSocket *best_socket_output(bNodeTree *ntree,
                                       bNode *node,
                                       bNodeSocket *sock_target,
                                       const bool allow_multiple)
{
  /* First look for selected output. */
  for (bNodeSocket &sock : node->outputs) {
    if (!socket_is_available(ntree, &sock, allow_multiple)) {
      continue;
    }

    if (sock.flag & SELECT) {
      return &sock;
    }
  }

  /* Try to find a socket with a matching name. */
  for (bNodeSocket &sock : node->outputs) {
    if (!socket_is_available(ntree, &sock, allow_multiple)) {
      continue;
    }

    /* Check for same types. */
    if (sock.type == sock_target->type) {
      if (STREQ(sock.name, sock_target->name)) {
        return &sock;
      }
    }
  }

  /* Otherwise settle for the first available socket of the right type. */
  for (bNodeSocket &sock : node->outputs) {
    if (!socket_is_available(ntree, &sock, allow_multiple)) {
      continue;
    }

    /* Check for same types. */
    if (sock.type == sock_target->type) {
      return &sock;
    }
  }

  /* If the target is an extend socket, then connect the first available socket that is not
   * already linked to the target node. */
  ntree->ensure_topology_cache();
  if (STREQ(sock_target->idname, "NodeSocketVirtual")) {
    for (bNodeSocket &output : node->outputs) {
      if (!output.is_icon_visible()) {
        continue;
      }

      /* Find out if the socket is already linked to the target node. */
      const Span<bNodeSocket *> directly_linked_sockets = output.directly_linked_sockets();
      bool is_output_linked_to_target_node = false;
      for (bNodeSocket *socket : directly_linked_sockets) {
        if (&socket->owner_node() == &sock_target->owner_node()) {
          is_output_linked_to_target_node = true;
          break;
        }
      }

      /* Already linked, ignore it. */
      if (is_output_linked_to_target_node) {
        continue;
      }

      return &output;
    }
  }

  /* Always allow linking to an reroute node. The socket type of the reroute sockets might change
   * after the link has been created. */
  if (node->is_reroute()) {
    return static_cast<bNodeSocket *>(node->outputs.first);
  }

  return nullptr;
}

/* Returns the list of available inputs sorted by their order of importance, where the order of
 * importance is assumed to be the numerical value of the socket type, such that a higher value
 * corresponds to a higher importance. If only_unlinked is true, only input sockets that are
 * unlinked will be considered. */
static Vector<bNodeSocket *> get_available_sorted_inputs(const bNodeTree *ntree,
                                                         const bNode *node,
                                                         const bool only_unlinked)
{
  Vector<bNodeSocket *> inputs;
  for (bNodeSocket &input : node->inputs) {
    if (socket_is_available(ntree, &input, !only_unlinked)) {
      inputs.append(&input);
    }
  }

  std::ranges::sort(inputs,
                    [](const bNodeSocket *a, const bNodeSocket *b) { return a->type > b->type; });

  return inputs;
}

static bool snode_autoconnect_input(bContext &C,
                                    SpaceNode &snode,
                                    bNode *node_fr,
                                    bNodeSocket *sock_fr,
                                    bNode *node_to,
                                    bNodeSocket *sock_to,
                                    int replace)
{
  bNodeTree *ntree = snode.edittree;

  if (replace) {
    bke::node_remove_socket_links(*ntree, *sock_to);
  }

  bNodeLink &link = bke::node_add_link(*ntree, *node_fr, *sock_fr, *node_to, *sock_to);

  if (link.fromnode->typeinfo->insert_link) {
    bke::NodeInsertLinkParams params{*ntree, *link.fromnode, link, &C};
    if (!link.fromnode->typeinfo->insert_link(params)) {
      bke::node_remove_link(ntree, link);
      return false;
    }
  }
  if (link.tonode->typeinfo->insert_link) {
    bke::NodeInsertLinkParams params{*ntree, *link.tonode, link, &C};
    if (!link.tonode->typeinfo->insert_link(params)) {
      bke::node_remove_link(ntree, link);
      return false;
    }
  }

  return true;
}

struct LinkAndPosition {
  bNodeLink *link;
  float2 multi_socket_position;
};

static void sort_multi_input_socket_links_with_drag(bNodeSocket &socket,
                                                    bNodeLink &drag_link,
                                                    const float2 &cursor)
{
  const float2 &socket_location = socket.runtime->location;

  Vector<LinkAndPosition, 8> links;
  for (bNodeLink *link : socket.directly_linked_links()) {
    const float2 location = node_link_calculate_multi_input_position(
        socket_location, link->multi_input_sort_id, link->tosock->runtime->total_inputs);
    links.append({link, location});
  };

  links.append({&drag_link, cursor});

  std::ranges::sort(links, [](const LinkAndPosition a, const LinkAndPosition b) {
    return a.multi_socket_position.y < b.multi_socket_position.y;
  });

  for (const int i : links.index_range()) {
    links[i].link->multi_input_sort_id = i;
  }
}

void update_multi_input_indices_for_removed_links(bNode &node)
{
  for (bNodeSocket *socket : node.input_sockets()) {
    if (!socket->is_multi_input()) {
      continue;
    }
    Vector<bNodeLink *, 8> links = socket->directly_linked_links();
    std::ranges::sort(links, [](const bNodeLink *a, const bNodeLink *b) {
      return a->multi_input_sort_id < b->multi_input_sort_id;
    });

    for (const int i : links.index_range()) {
      links[i]->multi_input_sort_id = i;
    }
  }
}

static void snode_autoconnect(bContext &C,
                              SpaceNode &snode,
                              const bool allow_multiple,
                              const bool replace)
{
  bNodeTree *ntree = snode.edittree;
  Vector<bNode *> sorted_nodes = get_selected_nodes(*ntree).extract_vector();

  /* Sort nodes left to right. */
  std::ranges::sort(sorted_nodes, [](const bNode *a, const bNode *b) {
    return a->location[0] < b->location[0];
  });

  // int numlinks = 0; /* UNUSED */
  for (const int i : sorted_nodes.as_mutable_span().drop_back(1).index_range()) {
    bool has_selected_inputs = false;

    bNode *node_fr = sorted_nodes[i];
    bNode *node_to = sorted_nodes[i + 1];
    /* Corner case: input/output node aligned the wrong way around (#47729). */
    if (BLI_listbase_is_empty(&node_to->inputs) || BLI_listbase_is_empty(&node_fr->outputs)) {
      std::swap(node_fr, node_to);
    }

    /* If there are selected sockets, connect those. */
    for (bNodeSocket &sock_to : node_to->inputs) {
      if (sock_to.flag & SELECT) {
        has_selected_inputs = true;

        if (!socket_is_available(ntree, &sock_to, replace)) {
          continue;
        }

        /* Check for an appropriate output socket to connect from. */
        bNodeSocket *sock_fr = best_socket_output(ntree, node_fr, &sock_to, allow_multiple);
        if (!sock_fr) {
          continue;
        }

        if (snode_autoconnect_input(C, snode, node_fr, sock_fr, node_to, &sock_to, replace)) {
          // numlinks++;
        }
      }
    }

    if (!has_selected_inputs) {
      Vector<bNodeSocket *> inputs = get_available_sorted_inputs(ntree, node_to, !replace);
      for (bNodeSocket *input : inputs) {
        /* Check for an appropriate output socket to connect from. */
        bNodeSocket *sock_fr = best_socket_output(ntree, node_fr, input, allow_multiple);
        if (!sock_fr) {
          continue;
        }

        if (snode_autoconnect_input(C, snode, node_fr, sock_fr, node_to, input, replace)) {
          // numlinks++;
          break;
        }
      }
    }
  }
}

/** \} */

namespace viewer_linking {

/* -------------------------------------------------------------------- */
/** \name Link Viewer Operator
 * \{ */

/* Depending on the node tree type, different socket types are supported by viewer nodes. */
static bool socket_can_be_viewed(const bNodeSocket &socket)
{
  if (!socket.is_icon_visible()) {
    return false;
  }
  if (STREQ(socket.idname, "NodeSocketVirtual")) {
    return false;
  }
  return true;
}

static void ensure_geometry_nodes_viewer_starts_with_geometry_socket(bNodeTree &tree,
                                                                     bNode &viewer_node)
{
  const auto &storage = *static_cast<const NodeGeometryViewer *>(viewer_node.storage);
  if (storage.items_num >= 1) {
    const NodeGeometryViewerItem &first_item = storage.items[0];
    if (first_item.socket_type == SOCK_GEOMETRY) {
      return;
    }
  }
  std::optional<int> existing_geometry_index;
  for (const int i : IndexRange(storage.items_num)) {
    const NodeGeometryViewerItem &item = storage.items[i];
    if (item.socket_type == SOCK_GEOMETRY) {
      existing_geometry_index = i;
      break;
    }
  }
  if (existing_geometry_index) {
    BLI_assert(*existing_geometry_index >= 1);
    dna::array::move_index(storage.items, storage.items_num, *existing_geometry_index, 0);
    return;
  }
  nodes::socket_items::add_item_with_socket_type_and_name<nodes::GeoViewerItemsAccessor>(
      tree, viewer_node, SOCK_GEOMETRY, "Geometry");
  dna::array::move_index(storage.items, storage.items_num, storage.items_num - 1, 0);
}

static int ensure_geometry_nodes_viewer_has_non_geometry_socket(
    bNodeTree &ntree, bNode &viewer_node, const eNodeSocketDatatype socket_type)
{
  const auto &storage = *static_cast<const NodeGeometryViewer *>(viewer_node.storage);
  if (storage.items_num == 0) {
    nodes::socket_items::add_item_with_socket_type_and_name<nodes::GeoViewerItemsAccessor>(
        ntree, viewer_node, socket_type, IFACE_("Value"));
    return 0;
  }
  if (storage.items_num == 1 && storage.items[0].socket_type != SOCK_GEOMETRY) {
    storage.items[0].socket_type = socket_type;
    return 0;
  }
  if (storage.items_num == 1 && storage.items[0].socket_type == SOCK_GEOMETRY) {
    nodes::socket_items::add_item_with_socket_type_and_name<nodes::GeoViewerItemsAccessor>(
        ntree, viewer_node, socket_type, IFACE_("Value"));
    return 1;
  }
  if (storage.items_num == 2 && storage.items[0].socket_type == SOCK_GEOMETRY &&
      storage.items[1].socket_type != SOCK_GEOMETRY)
  {
    storage.items[1].socket_type = socket_type;
    return 1;
  }
  std::optional<int> existing_geometry_index;
  for (const int i : IndexRange(storage.items_num)) {
    if (storage.items[i].socket_type == SOCK_GEOMETRY) {
      existing_geometry_index = i;
      break;
    }
  }
  if (existing_geometry_index) {
    dna::array::move_index(storage.items, storage.items_num, *existing_geometry_index, 0);
    storage.items[1].socket_type = socket_type;
    MEM_SAFE_DELETE(storage.items[1].name);
    storage.items[1].name = BLI_strdup(IFACE_("Value"));
    return 1;
  }
  storage.items[0].socket_type = socket_type;
  MEM_SAFE_DELETE(storage.items[0].name);
  storage.items[0].name = BLI_strdup(IFACE_("Value"));
  return 0;
}

static std::string get_viewer_source_name(const bNodeSocket &socket)
{
  const bNode &node = socket.owner_node();
  if (node.is_reroute()) {
    const bNodeSocket &reroute_input = node.input_socket(0);
    if (!reroute_input.is_logically_linked()) {
      return IFACE_(socket.typeinfo->label);
    }
    return reroute_input.logically_linked_sockets()[0]->name;
  }
  return blender::bke::node_socket_label(socket);
}
/**
 * Find the socket to link to in a viewer node.
 */
static bNodeSocket *node_link_viewer_get_socket(bNodeTree &ntree,
                                                bNode &viewer_node,
                                                bNodeSocket &src_socket)
{
  if (viewer_node.type_legacy != GEO_NODE_VIEWER) {
    /* In viewer nodes in the compositor, only the first input should be linked to. */
    return static_cast<bNodeSocket *>(viewer_node.inputs.first);
  }
  if (!nodes::GeoViewerItemsAccessor::supports_socket_type(src_socket.typeinfo->type, ntree.type))
  {
    return nullptr;
  }

  /* For the geometry nodes viewer, find the socket with the correct type. */
  const std::string name = get_viewer_source_name(src_socket);

  int item_index;
  if (src_socket.type == SOCK_GEOMETRY) {
    ensure_geometry_nodes_viewer_starts_with_geometry_socket(ntree, viewer_node);
    item_index = 0;
  }
  else {
    item_index = ensure_geometry_nodes_viewer_has_non_geometry_socket(
        ntree, viewer_node, src_socket.typeinfo->type);
  }

  auto &storage = *static_cast<NodeGeometryViewer *>(viewer_node.storage);
  NodeGeometryViewerItem &item = storage.items[item_index];
  nodes::socket_items::set_item_name_and_make_unique<nodes::GeoViewerItemsAccessor>(
      viewer_node, item, name.c_str());
  item.flag |= NODE_GEO_VIEWER_ITEM_FLAG_AUTO_REMOVE;
  nodes::update_node_declaration_and_sockets(ntree, viewer_node);
  return static_cast<bNodeSocket *>(BLI_findlink(&viewer_node.inputs, item_index));
}

static bool is_viewer_node(const bNode &node)
{
  return ELEM(node.type_legacy, CMP_NODE_VIEWER, GEO_NODE_VIEWER);
}

static bool is_viewer_socket_in_viewer(const bNodeSocket &socket)
{
  const bNode &node = socket.owner_node();
  BLI_assert(is_viewer_node(node));
  if (node.typeinfo->type_legacy == GEO_NODE_VIEWER) {
    return true;
  }
  return socket.index() == 0;
}

static bool is_viewer_socket(const bNodeSocket &socket)
{
  if (is_viewer_node(socket.owner_node())) {
    return is_viewer_socket_in_viewer(socket);
  }
  return false;
}

static int get_default_viewer_type(const bContext *C)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  return ED_node_is_compositor(snode) ? CMP_NODE_VIEWER : GEO_NODE_VIEWER;
}

static void remove_links_to_unavailable_viewer_sockets(bNodeTree &btree, bNode &viewer_node)
{
  for (bNodeLink &link : btree.links.items_mutable()) {
    if (link.tonode == &viewer_node) {
      if (link.tosock->flag & SOCK_UNAVAIL) {
        bke::node_remove_link(&btree, link);
      }
    }
  }
}

static bNodeSocket *determine_socket_to_view(bNode &node_to_view)
{
  int last_linked_data_socket_index = -1;
  bool has_linked_geometry_socket = false;
  for (bNodeSocket *socket : node_to_view.output_sockets()) {
    if (!socket_can_be_viewed(*socket)) {
      continue;
    }
    for (bNodeLink *link : socket->directly_linked_links()) {
      bNodeSocket &target_socket = *link->tosock;
      bNode &target_node = *link->tonode;
      if (is_viewer_socket(target_socket)) {
        if (link->is_muted() || !(target_node.flag & NODE_DO_OUTPUT)) {
          /* This socket is linked to a deactivated viewer, the viewer should be activated. */
          return socket;
        }
        if (socket->type == SOCK_GEOMETRY) {
          has_linked_geometry_socket = true;
        }
        else {
          last_linked_data_socket_index = socket->index();
        }
      }
    }
  }

  if (last_linked_data_socket_index == -1 && !has_linked_geometry_socket) {
    /* Return the first socket that can be viewed. */
    for (bNodeSocket *socket : node_to_view.output_sockets()) {
      if (socket_can_be_viewed(*socket)) {
        return socket;
      }
    }
    return nullptr;
  }

  bNodeSocket *already_viewed_socket = nullptr;

  /* Pick the next socket to be linked to the viewer. */
  const int tot_outputs = node_to_view.output_sockets().size();
  for (const int offset : IndexRange(1, tot_outputs)) {
    const int index = (last_linked_data_socket_index + offset) % tot_outputs;
    bNodeSocket &output_socket = node_to_view.output_socket(index);
    if (!socket_can_be_viewed(output_socket)) {
      continue;
    }
    if (has_linked_geometry_socket && output_socket.type == SOCK_GEOMETRY) {
      /* Skip geometry sockets when cycling if one is already viewed. */
      already_viewed_socket = &output_socket;
      continue;
    }

    bool is_currently_viewed = false;
    for (const bNodeLink *link : output_socket.directly_linked_links()) {
      bNodeSocket &target_socket = *link->tosock;
      bNode &target_node = *link->tonode;
      if (!is_viewer_socket(target_socket)) {
        continue;
      }
      if (link->is_muted()) {
        continue;
      }
      if (!(target_node.flag & NODE_DO_OUTPUT)) {
        continue;
      }
      is_currently_viewed = true;
      break;
    }
    if (is_currently_viewed) {
      already_viewed_socket = &output_socket;
      continue;
    }
    return &output_socket;
  }
  return already_viewed_socket;
}

static void finalize_viewer_link(const bContext &C,
                                 SpaceNode &snode,
                                 bNode &viewer_node,
                                 bNodeLink &viewer_link)
{
  Main *bmain = CTX_data_main(&C);
  remove_links_to_unavailable_viewer_sockets(*snode.edittree, viewer_node);
  viewer_link.flag &= ~NODE_LINK_MUTED;
  viewer_node.flag &= ~NODE_MUTED;
  viewer_node.flag |= NODE_DO_OUTPUT;

  if (snode.edittree->type == NTREE_GEOMETRY) {

    std::optional<int> item_identifier;
    const NodeGeometryViewerItem *item =
        nodes::socket_items::find_item_by_identifier<nodes::GeoViewerItemsAccessor>(
            viewer_node, viewer_link.tosock->identifier);
    BLI_assert(item);
    if (item) {
      item_identifier = item->identifier;
    }

    viewer_path::activate_geometry_node(*bmain, snode, viewer_node, item_identifier);
  }
  else if (snode.edittree->type == NTREE_COMPOSIT) {
    for (bNode *node : snode.nodetree->all_nodes()) {
      if (node->is_type("CompositorNodeViewer"_ustr) && node != &viewer_node) {
        node->flag &= ~NODE_DO_OUTPUT;
      }
    }
  }
  BKE_ntree_update_tag_active_output_changed(snode.edittree);
  BKE_main_ensure_invariants(*bmain, snode.edittree->id);
}

static const bNode *find_overlapping_node(const bNodeTree &tree,
                                          const rctf &rect,
                                          const Span<const bNode *> ignored_nodes)
{
  for (const bNode *node : tree.all_nodes()) {
    if (node->is_frame()) {
      continue;
    }
    if (ignored_nodes.contains(node)) {
      continue;
    }
    if (BLI_rctf_isect(&rect, &node->runtime->draw_bounds, nullptr)) {
      return node;
    }
  }
  return nullptr;
}

/**
 * Builds a list of possible locations for the viewer node that follows some search pattern where
 * positions closer to the initial position come first.
 */
static Vector<float2> get_viewer_node_position_candidates(const float2 initial,
                                                          const float step_distance,
                                                          const float max_distance)
{
  /* Prefer moving viewer a bit further horizontally than vertically. */
  const float y_scale = 0.5f;

  Vector<float2> candidates;
  candidates.append(initial);
  for (float distance = step_distance; distance <= max_distance; distance += step_distance) {
    const float arc_length = distance * M_PI;
    const int checks = std::max<int>(2, ceilf(arc_length / step_distance));
    for (const int i : IndexRange(checks)) {
      const float angle = i / float(checks - 1) * M_PI;
      const float candidate_x = initial.x + distance * std::sin(angle);
      const float candidate_y = initial.y + distance * std::cos(angle) * y_scale;
      candidates.append({candidate_x, candidate_y});
    }
  }
  return candidates;
}

/**
 * Positions the viewer node so that it is slightly to the right and top of the node to view. The
 * algorithm tries to avoid moving the viewer to a place where it would overlap with other nodes.
 * For that it iterates over many possible locations with increasing distance to the node to view.
 */
static void position_viewer_node(const bContext &C,
                                 bNodeTree &tree,
                                 bNode &viewer_node,
                                 const bNode &node_to_view)
{
  ScrArea &area = *CTX_wm_area(&C);
  ARegion &region = *CTX_wm_region(&C);
  ARegion &sidebar = *BKE_area_find_region_type(&area, RGN_TYPE_UI);

  tree.ensure_topology_cache();

  const View2D &v2d = region.v2d;
  rctf region_rect;
  region_rect.xmin = 0;
  region_rect.xmax = region.winx;
  region_rect.ymin = 0;
  region_rect.ymax = region.winy;
  if (U.uiflag2 & USER_REGION_OVERLAP) {
    region_rect.xmax -= sidebar.winx;
  }

  rctf region_bounds;
  ui::view2d_region_to_view_rctf(&v2d, &region_rect, &region_bounds);

  viewer_node.ui_order = tree.all_nodes().size();
  tree_draw_order_update(tree);

  const bool is_new_viewer_node = BLI_rctf_size_x(&viewer_node.runtime->draw_bounds) == 0;
  if (!is_new_viewer_node &&
      BLI_rctf_inside_rctf(&region_bounds, &viewer_node.runtime->draw_bounds) &&
      viewer_node.runtime->draw_bounds.xmin > node_to_view.runtime->draw_bounds.xmax)
  {
    /* Stay at the old viewer position when the viewer node is still in view and on the right side
     * of the node-to-view. */
    return;
  }

  const float default_padding_x = U.node_margin;
  const float default_padding_y = 10;
  const float viewer_width = is_new_viewer_node ?
                                 viewer_node.width * UI_SCALE_FAC :
                                 BLI_rctf_size_x(&viewer_node.runtime->draw_bounds);
  const float viewer_height = is_new_viewer_node ?
                                  100 * UI_SCALE_FAC :
                                  BLI_rctf_size_y(&viewer_node.runtime->draw_bounds);

  const float2 main_candidate{node_to_view.runtime->draw_bounds.xmax + default_padding_x,
                              node_to_view.runtime->draw_bounds.ymax + viewer_height +
                                  default_padding_y};

  std::optional<float2> new_viewer_position;

  const Vector<float2> position_candidates = get_viewer_node_position_candidates(
      main_candidate, 50 * UI_SCALE_FAC, 800 * UI_SCALE_FAC);
  for (const float2 &candidate_pos : position_candidates) {
    rctf candidate;
    candidate.xmin = candidate_pos.x;
    candidate.xmax = candidate_pos.x + viewer_width;
    candidate.ymin = candidate_pos.y - viewer_height;
    candidate.ymax = candidate_pos.y;

    if (!BLI_rctf_inside_rctf(&region_bounds, &candidate)) {
      /* Avoid moving viewer outside of visible region. */
      continue;
    }

    rctf padded_candidate = candidate;
    BLI_rctf_pad(&padded_candidate, default_padding_x - 1, default_padding_y - 1);

    const bNode *overlapping_node = find_overlapping_node(
        tree, padded_candidate, {&viewer_node, &node_to_view});
    if (!overlapping_node) {
      new_viewer_position = candidate_pos;
      break;
    }
  }

  if (!new_viewer_position) {
    new_viewer_position = main_candidate;
  }

  viewer_node.location[0] = new_viewer_position->x / UI_SCALE_FAC;
  viewer_node.location[1] = new_viewer_position->y / UI_SCALE_FAC;
  viewer_node.parent = nullptr;
}

static int view_socket(const bContext &C,
                       SpaceNode &snode,
                       bNodeTree &btree,
                       bNode &bnode_to_view,
                       bNodeSocket &bsocket_to_view)
{
  bNode *viewer_node = nullptr;
  /* Try to find a viewer that is already active. */
  for (bNode *node : btree.all_nodes()) {
    if (is_viewer_node(*node)) {
      if (node->flag & NODE_DO_OUTPUT && node->custom1 == NODE_VIEWER_SHORTCUT_NONE) {
        viewer_node = node;
        break;
      }
    }
  }

  /* Try to reactivate existing viewer connection. */
  for (bNodeLink *link : bsocket_to_view.directly_linked_links()) {
    bNodeSocket &target_socket = *link->tosock;
    bNode &target_node = *link->tonode;
    if (is_viewer_socket(target_socket) && ELEM(viewer_node, nullptr, &target_node)) {
      finalize_viewer_link(C, snode, target_node, *link);
      position_viewer_node(C, btree, target_node, bnode_to_view);
      return OPERATOR_FINISHED;
    }
  }

  if (viewer_node == nullptr) {
    for (bNode *node : btree.all_nodes()) {
      if (is_viewer_node(*node) && node->custom1 == NODE_VIEWER_SHORTCUT_NONE) {
        viewer_node = node;
        break;
      }
    }
  }
  if (viewer_node == nullptr) {
    const float2 socket_location = bsocket_to_view.runtime->location;
    const int viewer_type = get_default_viewer_type(&C);
    const float2 location{socket_location.x / UI_SCALE_FAC + 100,
                          socket_location.y / UI_SCALE_FAC};
    viewer_node = add_static_node(C, viewer_type, location);
  }

  bNodeSocket *viewer_bsocket = node_link_viewer_get_socket(btree, *viewer_node, bsocket_to_view);
  if (viewer_bsocket == nullptr) {
    return OPERATOR_CANCELLED;
  }
  viewer_bsocket->flag &= ~SOCK_HIDDEN;

  bNodeLink *viewer_link = nullptr;
  for (bNodeLink &link : btree.links.items_mutable()) {
    if (link.tosock == viewer_bsocket) {
      viewer_link = &link;
      break;
    }
  }
  if (viewer_link == nullptr) {
    viewer_link = &bke::node_add_link(
        btree, bnode_to_view, bsocket_to_view, *viewer_node, *viewer_bsocket);
  }
  else {
    viewer_link->fromnode = &bnode_to_view;
    viewer_link->fromsock = &bsocket_to_view;
    BKE_ntree_update_tag_link_changed(&btree);
  }
  finalize_viewer_link(C, snode, *viewer_node, *viewer_link);
  position_viewer_node(C, btree, *viewer_node, bnode_to_view);
  return OPERATOR_CANCELLED;
}

static int node_link_viewer(const bContext &C, bNode &bnode_to_view, bNodeSocket *bsocket_to_view)
{
  SpaceNode &snode = *CTX_wm_space_node(&C);
  bNodeTree *btree = snode.edittree;
  btree->ensure_topology_cache();

  if (bsocket_to_view == nullptr) {
    bsocket_to_view = determine_socket_to_view(bnode_to_view);
  }

  if (bsocket_to_view == nullptr) {
    return OPERATOR_CANCELLED;
  }

  return view_socket(C, snode, *btree, bnode_to_view, *bsocket_to_view);
}

/** \} */

}  // namespace viewer_linking

/* -------------------------------------------------------------------- */
/** \name Link to Viewer Node Operator
 * \{ */

static wmOperatorStatus node_active_link_viewer_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNode *node = bke::node_get_active(*snode.edittree);

  if (!node) {
    return OPERATOR_CANCELLED;
  }

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  bNodeSocket *socket_to_view = nullptr;
  for (bNodeSocket &socket : node->outputs) {
    if (socket.flag & SELECT) {
      socket_to_view = &socket;
      break;
    }
  }

  if (viewer_linking::node_link_viewer(*C, *node, socket_to_view) == OPERATOR_CANCELLED) {
    return OPERATOR_CANCELLED;
  }

  BKE_main_ensure_invariants(*CTX_data_main(C), snode.edittree->id);

  return OPERATOR_FINISHED;
}

static bool node_active_link_viewer_poll(bContext *C)
{
  if (!ED_operator_node_editable(C)) {
    return false;
  }
  SpaceNode *snode = CTX_wm_space_node(C);
  if (ED_node_is_compositor(snode)) {
    return true;
  }
  if (ED_node_is_geometry(snode)) {
    if (snode->node_tree_sub_type == SNODE_GEOMETRY_TOOL) {
      /* The viewer node is not supported in the "Tool" context. */
      return false;
    }
    return true;
  }
  return false;
}

void NODE_OT_link_viewer(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Link to Viewer Node";
  ot->description = "Link to viewer node";
  ot->idname = "NODE_OT_link_viewer";

  /* API callbacks. */
  ot->exec = node_active_link_viewer_exec;
  ot->poll = node_active_link_viewer_poll;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Add Link Operator
 * \{ */

/**
 * Check if any of the dragged links are connected to a socket on the side that they are dragged
 * from.
 */
static bool dragged_links_are_detached(const bNodeLinkDrag &nldrag)
{
  if (nldrag.in_out == SOCK_OUT) {
    for (const bNodeLink &link : nldrag.links) {
      if (link.tonode && link.tosock) {
        return false;
      }
    }
  }
  else {
    for (const bNodeLink &link : nldrag.links) {
      if (link.fromnode && link.fromsock) {
        return false;
      }
    }
  }
  return true;
}

static bool should_create_drag_link_search_menu(const bNodeTree &node_tree,
                                                const bNodeLinkDrag &nldrag)
{
  /* Custom node trees aren't supported yet. */
  if (node_tree.type == NTREE_CUSTOM) {
    return false;
  }
  /* Only create the search menu when the drag has not already connected the links to a socket. */
  if (!dragged_links_are_detached(nldrag)) {
    return false;
  }
  if (nldrag.swap_links) {
    return false;
  }
  /* Don't create the search menu if the drag is disconnecting a link from an input node. */
  if (nldrag.start_socket->in_out == SOCK_IN && nldrag.start_link_count > 0) {
    return false;
  }
  /* Don't allow a drag from the "new socket" (group input node or simulation nodes currently).
   * Handling these properly in node callbacks increases the complexity too much for now. */
  if (nldrag.start_socket->type == SOCK_CUSTOM) {
    return false;
  }
  if (nldrag.start_socket->type == SOCK_TEXTURE) {
    /* This socket types is not used anymore, but can currently still exists in files. */
    return false;
  }
  return true;
}

static bool need_drag_link_tooltip(const bNodeTree &node_tree, const bNodeLinkDrag &nldrag)
{
  return nldrag.swap_links || should_create_drag_link_search_menu(node_tree, nldrag);
}

static void draw_draglink_tooltip(const bContext * /*C*/, ARegion * /*region*/, void *arg)
{
  bNodeLinkDrag *nldrag = static_cast<bNodeLinkDrag *>(arg);

  uchar text_col[4];
  ui::theme::get_color_4ubv(TH_TEXT, text_col);

  const int padding = 4 * UI_SCALE_FAC;
  const float x = nldrag->in_out == SOCK_IN ? nldrag->cursor[0] - 3.3f * padding :
                                              nldrag->cursor[0];
  const float y = nldrag->cursor[1] - 2.0f * UI_SCALE_FAC;

  const bool new_link = nldrag->in_out == nldrag->start_socket->in_out;
  const bool swap_links = nldrag->swap_links;

  const int icon = !swap_links ? ICON_ADD : (new_link ? ICON_ANIM : ICON_UV_SYNC_SELECT);

  ui::icon_draw_ex(
      x, y, icon, UI_INV_SCALE_FAC, 1.0f, 0.0f, text_col, false, UI_NO_ICON_OVERLAY_TEXT);
}

static void draw_draglink_tooltip_activate(const ARegion &region, bNodeLinkDrag &nldrag)
{
  if (nldrag.draw_handle == nullptr) {
    nldrag.draw_handle = ED_region_draw_cb_activate(
        region.runtime->type, draw_draglink_tooltip, &nldrag, REGION_DRAW_POST_PIXEL);
  }
}

static void draw_draglink_tooltip_deactivate(const ARegion &region, bNodeLinkDrag &nldrag)
{
  if (nldrag.draw_handle) {
    ED_region_draw_cb_exit(region.runtime->type, nldrag.draw_handle);
    nldrag.draw_handle = nullptr;
  }
}

static int node_socket_count_links(const bNodeTree &ntree, const bNodeSocket &socket)
{
  int count = 0;
  for (bNodeLink &link : ntree.links) {
    if (ELEM(&socket, link.fromsock, link.tosock)) {
      count++;
    }
  }
  return count;
}

static bNodeSocket *node_find_linkable_socket(const bNodeTree &ntree,
                                              const bNode *node,
                                              bNodeSocket *socket_to_match)
{
  bNodeSocket *first_socket = socket_to_match->in_out == SOCK_IN ?
                                  static_cast<bNodeSocket *>(node->inputs.first) :
                                  static_cast<bNodeSocket *>(node->outputs.first);

  bNodeSocket *socket = socket_to_match->next ? socket_to_match->next : first_socket;
  while (socket != socket_to_match) {
    if (socket->is_visible()) {
      const bool sockets_are_compatible = socket->typeinfo == socket_to_match->typeinfo;
      if (sockets_are_compatible) {
        const int link_count = node_socket_count_links(ntree, *socket);
        const bool socket_has_capacity = link_count < bke::node_socket_link_limit(*socket);
        if (socket_has_capacity) {
          /* Found a valid free socket we can swap to. */
          return socket;
        }
      }
    }
    /* Wrap around the list end. */
    socket = socket->next ? socket->next : first_socket;
  }

  return nullptr;
}

static void displace_links(bNodeTree *ntree, const bNode *node, bNodeLink *inserted_link)
{
  bNodeSocket *linked_socket = node == inserted_link->tonode ? inserted_link->tosock :
                                                               inserted_link->fromsock;
  bNodeSocket *replacement_socket = node_find_linkable_socket(*ntree, node, linked_socket);

  if (linked_socket->is_input()) {
    BLI_assert(!linked_socket->is_multi_input());
    ntree->ensure_topology_cache();

    if (linked_socket->directly_linked_links().is_empty()) {
      return;
    }
    bNodeLink *displaced_link = linked_socket->directly_linked_links().first();

    if (!replacement_socket) {
      bke::node_remove_link(ntree, *displaced_link);
      return;
    }

    displaced_link->tosock = replacement_socket;

    if (replacement_socket->is_multi_input()) {
      /* Check for duplicate links when linking to multi input sockets. */
      for (bNodeLink *existing_link : replacement_socket->runtime->directly_linked_links) {
        if (existing_link->fromsock == displaced_link->fromsock) {
          bke::node_remove_link(ntree, *displaced_link);
          return;
        }
      }
      const int multi_input_sort_id = node_socket_count_links(*ntree, *replacement_socket) - 1;
      displaced_link->multi_input_sort_id = multi_input_sort_id;
    }

    BKE_ntree_update_tag_link_changed(ntree);
    return;
  }

  for (bNodeLink &link : ntree->links.items_mutable()) {
    if (link.fromsock == linked_socket) {
      if (replacement_socket) {
        link.fromsock = replacement_socket;
        BKE_ntree_update_tag_link_changed(ntree);
      }
      else {
        bke::node_remove_link(ntree, link);
        BKE_ntree_update_tag_link_removed(ntree);
      }
    }
  }
}

static void node_displace_existing_links(bNodeLinkDrag &nldrag, bNodeTree &ntree)
{
  bNodeLink &link = nldrag.links.first();
  if (!link.fromsock || !link.tosock) {
    return;
  }
  if (nldrag.start_socket->is_input()) {
    displace_links(&ntree, link.fromnode, &link);
  }
  else {
    displace_links(&ntree, link.tonode, &link);
  }
}

static void node_swap_links(bNodeLinkDrag &nldrag, bNodeTree &ntree)
{
  bNodeSocket &linked_socket = *nldrag.hovered_socket;
  bNodeSocket *start_socket = nldrag.start_socket;
  bNode *start_node = nldrag.start_node;

  if (linked_socket.is_input()) {
    for (bNodeLink &link : ntree.links) {
      if (link.tosock != &linked_socket) {
        continue;
      }
      if (link.fromnode == start_node) {
        /* Don't link a node to itself. */
        bke::node_remove_link(&ntree, link);
        continue;
      }

      link.tosock = start_socket;
      link.tonode = start_node;
    }
  }
  else {
    for (bNodeLink &link : ntree.links) {
      if (link.fromsock != &linked_socket) {
        continue;
      }
      if (link.tonode == start_node) {
        /* Don't link a node to itself. */
        bke::node_remove_link(&ntree, link);
        continue;
      }
      link.fromsock = start_socket;
      link.fromnode = start_node;
    }
  }

  BKE_ntree_update_tag_link_changed(&ntree);
}

static void node_remove_existing_links_if_needed(bNodeLinkDrag &nldrag, bNodeTree &ntree)
{
  bNodeSocket &linked_socket = *nldrag.hovered_socket;

  int link_count = node_socket_count_links(ntree, linked_socket);
  const int link_limit = bke::node_socket_link_limit(linked_socket);
  Set<bNodeLink *> links_to_remove;

  ntree.ensure_topology_cache();

  /* Remove duplicate links first. */
  for (const bNodeLink dragged_link : nldrag.links) {
    if (linked_socket.is_input()) {
      for (bNodeLink *link : linked_socket.runtime->directly_linked_links) {
        const bool duplicate_link = link->fromsock == dragged_link.fromsock;
        if (duplicate_link) {
          links_to_remove.add(link);
          link_count--;
        }
      }
    }
    else {
      for (bNodeLink *link : linked_socket.runtime->directly_linked_links) {
        const bool duplicate_link = link->tosock == dragged_link.tosock;
        if (duplicate_link) {
          links_to_remove.add(link);
          link_count--;
        }
      }
    }
  }

  for (bNodeLink *link : linked_socket.runtime->directly_linked_links) {
    const bool link_limit_exceeded = !(link_count < link_limit);
    if (link_limit_exceeded) {
      if (links_to_remove.add(link)) {
        link_count--;
      }
    }
  }

  for (bNodeLink *link : links_to_remove) {
    bke::node_remove_link(&ntree, *link);
  }
}

static void add_dragged_links_to_tree(bContext &C, bNodeLinkDrag &nldrag)
{
  Main *bmain = CTX_data_main(&C);
  ARegion &region = *CTX_wm_region(&C);
  SpaceNode &snode = *CTX_wm_space_node(&C);
  bNodeTree &ntree = *snode.edittree;

  /* Handle node links already occupying the socket. */
  if (const bNodeSocket *linked_socket = nldrag.hovered_socket) {
    /* Swapping existing links out of multi input sockets is not supported. */
    const bool connecting_to_multi_input = linked_socket->is_multi_input() ||
                                           nldrag.start_socket->is_multi_input();
    if (nldrag.swap_links && !connecting_to_multi_input) {
      const bool is_new_link = nldrag.in_out == nldrag.start_socket->in_out;
      if (is_new_link) {
        node_displace_existing_links(nldrag, ntree);
      }
      else {
        node_swap_links(nldrag, ntree);
      }
    }
    else {
      node_remove_existing_links_if_needed(nldrag, ntree);
    }
  }

  for (const bNodeLink &link : nldrag.links) {
    if (!link.tosock || !link.fromsock) {
      continue;
    }

    /* Before actually adding the link let nodes perform special link insertion handling. */

    bNodeLink *new_link = MEM_new<bNodeLink>(__func__, link);
    if (link.fromnode->typeinfo->insert_link) {
      bke::NodeInsertLinkParams params{ntree, *link.fromnode, *new_link, &C};
      if (!link.fromnode->typeinfo->insert_link(params)) {
        MEM_delete(new_link);
        continue;
      }
    }
    if (link.tonode->typeinfo->insert_link) {
      bke::NodeInsertLinkParams params{ntree, *link.tonode, *new_link, &C};
      if (!link.tonode->typeinfo->insert_link(params)) {
        MEM_delete(new_link);
        continue;
      }
    }

    BLI_addtail(&ntree.links, new_link);
    BKE_ntree_update_tag_link_added(&ntree, new_link);
  }

  BKE_main_ensure_invariants(*bmain, ntree.id);

  /* Ensure drag-link tool-tip is disabled. */
  draw_draglink_tooltip_deactivate(region, nldrag);

  ED_workspace_status_text(&C, nullptr);
  ED_region_tag_redraw(&region);
  clear_picking_highlight(&snode.edittree->links);

  snode.runtime->linkdrag.reset();
}

static void node_link_cancel(bContext *C, wmOperator *op)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  bNodeTree &ntree = *snode->edittree;
  bNodeLinkDrag *nldrag = static_cast<bNodeLinkDrag *>(op->customdata);

  /* Restore pre-existing links. */
  for (bNodeLink &link : nldrag->links) {
    if (nldrag->start_socket->is_output() && nldrag->in_out == SOCK_OUT) {
      /* Dragged from output socket (to create a new link), nothing to restore. */
      continue;
    }

    bNode *fromnode = nullptr, *tonode = nullptr;
    bNodeSocket *fromsock = nullptr, *tosock = nullptr;

    if (nldrag->start_socket->is_output() && nldrag->in_out == SOCK_IN) {
      /* Existing, disconnected (from output socket) link, fixed on an input socket. */
      fromnode = nldrag->start_node;
      fromsock = nldrag->start_socket;
      tonode = link.tonode;
      tosock = link.tosock;
    }
    else if (nldrag->start_socket->is_input() && nldrag->in_out == SOCK_OUT) {
      /* Existing, disconnected (from input socket) link, fixed on an output socket. */
      fromnode = link.fromnode;
      fromsock = link.fromsock;
      tonode = nldrag->start_node;
      tosock = nldrag->start_socket;
    }

    if (ELEM(nullptr, fromnode, fromsock, tonode, tosock)) {
      continue;
    }

    bNodeLink &link_restored = bke::node_add_link(ntree, *fromnode, *fromsock, *tonode, *tosock);

    bke::node_link_set_mute(ntree, link_restored, (link.flag & NODE_LINK_MUTED));

    if ((tosock->flag & SOCK_MULTI_INPUT)) {
      ntree.ensure_topology_cache();
      link_restored.multi_input_sort_id = link.multi_input_sort_id;
      /* When drag-disconnected from input socket, order from other links will
       * have changed, so update sort IDs to prevent invalid cases later on. */
      if (nldrag->start_socket->is_input()) {
        for (bNodeLink *other_link : tosock->directly_linked_links()) {
          if (other_link == &link_restored) {
            continue;
          }
          if (other_link->multi_input_sort_id >= link_restored.multi_input_sort_id) {
            other_link->multi_input_sort_id += 1;
          }
        }
      }
    }

    if (link_restored.fromnode->typeinfo->insert_link) {
      bke::NodeInsertLinkParams params{ntree, *link_restored.fromnode, link_restored, C};
      if (!link_restored.fromnode->typeinfo->insert_link(params)) {
        bke::node_remove_link(&ntree, link_restored);
        continue;
      }
    }
    if (link_restored.tonode->typeinfo->insert_link) {
      bke::NodeInsertLinkParams params{ntree, *link_restored.tonode, link_restored, C};
      if (!link_restored.tonode->typeinfo->insert_link(params)) {
        bke::node_remove_link(&ntree, link_restored);
        continue;
      }
    }
  }

  draw_draglink_tooltip_deactivate(*CTX_wm_region(C), *nldrag);
  view2d_edge_pan_cancel(C, &nldrag->pan_data);
  snode->runtime->linkdrag.reset();
  clear_picking_highlight(&snode->edittree->links);
  BKE_ntree_update_tag_link_changed(snode->edittree);
  BKE_main_ensure_invariants(*CTX_data_main(C), snode->edittree->id);
}

static void node_link_find_socket(bContext &C, wmOperator &op, const float2 &cursor)
{
  SpaceNode &snode = *CTX_wm_space_node(&C);
  ARegion &region = *CTX_wm_region(&C);
  bNodeLinkDrag &nldrag = *static_cast<bNodeLinkDrag *>(op.customdata);

  if (nldrag.in_out == SOCK_OUT) {
    if (bNodeSocket *tsock = node_find_indicated_socket(snode, region, cursor, SOCK_IN)) {
      nldrag.hovered_socket = tsock;
      bNode &tnode = tsock->owner_node();
      for (bNodeLink &link : nldrag.links) {
        /* Skip if socket is on the same node as the fromsock. */
        if (link.fromnode == &tnode) {
          continue;
        }

        /* Skip if tsock is already linked with this output. */
        bNodeLink *existing_link_connected_to_fromsock = nullptr;
        for (bNodeLink &existing_link : snode.edittree->links) {
          if (existing_link.fromsock == link.fromsock && existing_link.tosock == tsock) {
            existing_link_connected_to_fromsock = &existing_link;
            break;
          }
        }

        /* Attach links to the socket. */
        link.tonode = &tnode;
        link.tosock = tsock;
        nldrag.last_node_hovered_while_dragging_a_link = &tnode;
        if (existing_link_connected_to_fromsock) {
          link.multi_input_sort_id = existing_link_connected_to_fromsock->multi_input_sort_id;
          continue;
        }
        if (tsock && tsock->is_multi_input()) {
          sort_multi_input_socket_links_with_drag(*tsock, link, cursor);
        }
      }
    }
    else {
      nldrag.hovered_socket = nullptr;
      for (bNodeLink &link : nldrag.links) {
        link.tonode = nullptr;
        link.tosock = nullptr;
      }
      if (nldrag.last_node_hovered_while_dragging_a_link) {
        update_multi_input_indices_for_removed_links(
            *nldrag.last_node_hovered_while_dragging_a_link);
      }
    }
  }
  else {
    if (bNodeSocket *tsock = node_find_indicated_socket(snode, region, cursor, SOCK_OUT)) {
      nldrag.hovered_socket = tsock;
      bNode &node = tsock->owner_node();
      for (bNodeLink &link : nldrag.links) {
        /* Skip if this is already the target socket. */
        if (link.fromsock == tsock) {
          continue;
        }
        /* Skip if socket is on the same node as the `fromsock`. */
        if (link.tonode == &node) {
          continue;
        }

        /* Attach links to the socket. */
        link.fromnode = &node;
        link.fromsock = tsock;
      }
    }
    else {
      nldrag.hovered_socket = nullptr;
      for (bNodeLink &link : nldrag.links) {
        link.fromnode = nullptr;
        link.fromsock = nullptr;
      }
    }
  }
}

enum class NodeLinkAction : int {
  Begin = 0,
  Cancel = 1,
  Swap = 2,
  Confirm = 3,
};

wmKeyMap *node_link_modal_keymap(wmKeyConfig *keyconf)
{
  static const EnumPropertyItem modal_items[] = {
      {int(NodeLinkAction::Begin), "BEGIN", 0, "Drag Node-link", ""},
      {int(NodeLinkAction::Confirm), "CONFIRM", 0, "Confirm Link", ""},
      {int(NodeLinkAction::Cancel), "CANCEL", 0, "Cancel", ""},
      {int(NodeLinkAction::Swap), "SWAP", 0, "Swap Links", ""},
      {0, nullptr, 0, nullptr, nullptr},
  };

  wmKeyMap *keymap = WM_modalkeymap_find(keyconf, "Node Link Modal Map");

  /* This function is called for each space-type, only needs to add map once. */
  if (keymap && keymap->modal_items) {
    return nullptr;
  }

  keymap = WM_modalkeymap_ensure(keyconf, "Node Link Modal Map", modal_items);

  WM_modalkeymap_assign(keymap, "NODE_OT_link");

  return keymap;
}

static wmOperatorStatus node_link_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  bNodeLinkDrag &nldrag = *static_cast<bNodeLinkDrag *>(op->customdata);
  SpaceNode &snode = *CTX_wm_space_node(C);
  ARegion *region = CTX_wm_region(C);

  view2d_edge_pan_apply_event(C, &nldrag.pan_data, event);

  float2 cursor;
  ui::view2d_region_to_view(&region->v2d, event->mval[0], event->mval[1], &cursor.x, &cursor.y);
  nldrag.cursor[0] = event->mval[0];
  nldrag.cursor[1] = event->mval[1];

  if (event->type == EVT_MODAL_MAP) {
    switch (event->val) {
      case int(NodeLinkAction::Begin): {
        return OPERATOR_RUNNING_MODAL;
      }
      case int(NodeLinkAction::Confirm): {
        /* Add a search menu for compatible sockets if the drag released on empty space. */
        if (should_create_drag_link_search_menu(*snode.edittree, nldrag)) {
          bNodeLink &link = nldrag.links.first();
          if (nldrag.in_out == SOCK_OUT) {
            invoke_node_link_drag_add_menu(*C, *link.fromnode, *link.fromsock, cursor);
          }
          else {
            invoke_node_link_drag_add_menu(*C, *link.tonode, *link.tosock, cursor);
          }
        }
        add_dragged_links_to_tree(*C, nldrag);
        return OPERATOR_FINISHED;
      }
      case int(NodeLinkAction::Cancel): {
        node_link_cancel(C, op);
        return OPERATOR_CANCELLED;
      }
      case int(NodeLinkAction::Swap):
        if (event->prev_val == KM_PRESS) {
          nldrag.swap_links = true;
        }
        else if (event->prev_val == KM_RELEASE) {
          nldrag.swap_links = false;
        }
        return OPERATOR_RUNNING_MODAL;
    }
  }
  else if (event->type == MOUSEMOVE) {
    if (nldrag.start_socket->is_multi_input() && nldrag.links.is_empty()) {
      pick_input_link_by_link_intersect(*C, *op, nldrag, cursor);
    }
    else {
      node_link_find_socket(*C, *op, cursor);
      ED_region_tag_redraw(region);
    }

    if (need_drag_link_tooltip(*snode.edittree, nldrag)) {
      draw_draglink_tooltip_activate(*region, nldrag);
    }
    else {
      draw_draglink_tooltip_deactivate(*region, nldrag);
    }
  }

  return OPERATOR_RUNNING_MODAL;
}

static void remove_unavailable_links(bNodeTree &tree, bNodeSocket &socket)
{
  tree.ensure_topology_cache();
  Vector<bNodeLink *> links = socket.directly_linked_links();
  for (bNodeLink *link : links) {
    if (!link->is_available()) {
      bke::node_remove_link(&tree, *link);
    }
  }
}

static std::unique_ptr<bNodeLinkDrag> node_link_init(ARegion &region,
                                                     SpaceNode &snode,
                                                     const float2 cursor,
                                                     const bool detach)
{
  if (bNodeSocket *sock = node_find_indicated_socket(snode, region, cursor, SOCK_OUT)) {
    remove_unavailable_links(*snode.edittree, *sock);
    snode.edittree->ensure_topology_cache();
    bNode &node = sock->owner_node();

    std::unique_ptr<bNodeLinkDrag> nldrag = std::make_unique<bNodeLinkDrag>();
    nldrag->start_node = &node;
    nldrag->start_socket = sock;
    nldrag->start_link_count = bke::node_count_socket_links(*snode.edittree, *sock);
    int link_limit = bke::node_socket_link_limit(*sock);
    if (nldrag->start_link_count > 0 && (nldrag->start_link_count >= link_limit || detach)) {
      /* Dragged links are fixed on input side. */
      nldrag->in_out = SOCK_IN;
      /* Detach current links and store them in the operator data. */
      for (bNodeLink &link : snode.edittree->links.items_mutable()) {
        if (link.fromsock == sock) {
          bNodeLink oplink = link;
          oplink.next = oplink.prev = nullptr;
          oplink.flag |= NODE_LINK_VALID;

          nldrag->links.append(oplink);
          bke::node_remove_link(snode.edittree, link);
        }
      }
    }
    else {
      /* Dragged links are fixed on output side. */
      nldrag->in_out = SOCK_OUT;
      nldrag->links.append(create_drag_link(node, *sock));
    }
    return nldrag;
  }

  if (bNodeSocket *sock = node_find_indicated_socket(snode, region, cursor, SOCK_IN)) {
    remove_unavailable_links(*snode.edittree, *sock);
    snode.edittree->ensure_topology_cache();
    bNode &node = sock->owner_node();
    std::unique_ptr<bNodeLinkDrag> nldrag = std::make_unique<bNodeLinkDrag>();
    nldrag->last_node_hovered_while_dragging_a_link = &node;
    nldrag->start_node = &node;
    nldrag->start_socket = sock;

    nldrag->start_link_count = bke::node_count_socket_links(*snode.edittree, *sock);
    if (nldrag->start_link_count > 0) {
      /* Dragged links are fixed on output side. */
      nldrag->in_out = SOCK_OUT;
      /* Detach current links and store them in the operator data. */
      bNodeLink *link_to_pick;
      for (bNodeLink &link : snode.edittree->links.items_mutable()) {
        if (link.tosock == sock) {
          link_to_pick = &link;
        }
      }

      if (link_to_pick != nullptr && !nldrag->start_socket->is_multi_input()) {
        bNodeLink oplink = *link_to_pick;
        oplink.next = oplink.prev = nullptr;
        oplink.flag |= NODE_LINK_VALID;

        nldrag->links.append(oplink);
        bke::node_remove_link(snode.edittree, *link_to_pick);

        /* Send changed event to original link->tonode. */
        BKE_ntree_update_tag_node_property(snode.edittree, &node);
      }
    }
    else {
      /* Dragged links are fixed on input side. */
      nldrag->in_out = SOCK_IN;
      nldrag->links.append(create_drag_link(node, *sock));
    }
    return nldrag;
  }

  return {};
}

static wmOperatorStatus node_link_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  Main &bmain = *CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  ARegion &region = *CTX_wm_region(C);

  bool detach = RNA_boolean_get(op->ptr, "detach");

  int2 mval;
  WM_event_drag_start_mval(event, &region, mval);

  float2 cursor;
  ui::view2d_region_to_view(&region.v2d, mval[0], mval[1], &cursor[0], &cursor[1]);
  RNA_float_set_array(op->ptr, "drag_start", cursor);

  ED_preview_kill_jobs(CTX_wm_manager(C), &bmain);

  std::unique_ptr<bNodeLinkDrag> nldrag = node_link_init(region, snode, cursor, detach);
  if (!nldrag) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  view2d_edge_pan_operator_init(C, &nldrag->pan_data, op);

  /* Add icons at the cursor when the link is dragged in empty space. */
  if (need_drag_link_tooltip(*snode.edittree, *nldrag)) {
    draw_draglink_tooltip_activate(*CTX_wm_region(C), *nldrag);
  }
  snode.runtime->linkdrag = std::move(nldrag);
  op->customdata = snode.runtime->linkdrag.get();

  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

void NODE_OT_link(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Link Nodes";
  ot->idname = "NODE_OT_link";
  ot->description = "Use the mouse to create a link between two nodes";

  /* API callbacks. */
  ot->invoke = node_link_invoke;
  ot->modal = node_link_modal;
  ot->poll = ED_operator_node_editable;
  ot->cancel = node_link_cancel;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  RNA_def_boolean(ot->srna, "detach", false, "Detach", "Detach and redirect existing links");
  RNA_def_float_array(ot->srna,
                      "drag_start",
                      2,
                      nullptr,
                      -UI_PRECISION_FLOAT_MAX,
                      UI_PRECISION_FLOAT_MAX,
                      "Drag Start",
                      "The position of the mouse cursor at the start of the operation",
                      -UI_PRECISION_FLOAT_MAX,
                      UI_PRECISION_FLOAT_MAX);

  ui::view2d_edge_pan_operator_properties_ex(ot,
                                             NODE_EDGE_PAN_INSIDE_PAD,
                                             NODE_EDGE_PAN_OUTSIDE_PAD,
                                             NODE_EDGE_PAN_SPEED_RAMP,
                                             NODE_EDGE_PAN_MAX_SPEED,
                                             NODE_EDGE_PAN_DELAY,
                                             NODE_EDGE_PAN_ZOOM_INFLUENCE);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Make Link Operator
 * \{ */

/* Makes a link between selected output and input sockets. */
static wmOperatorStatus node_make_link_exec(bContext *C, wmOperator *op)
{
  Main &bmain = *CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &node_tree = *snode.edittree;
  const bool replace = RNA_boolean_get(op->ptr, "replace");

  ED_preview_kill_jobs(CTX_wm_manager(C), &bmain);

  snode_autoconnect(*C, snode, true, replace);

  /* Deselect sockets after linking. */
  node_deselect_all_input_sockets(node_tree, false);
  node_deselect_all_output_sockets(node_tree, false);

  BKE_main_ensure_invariants(bmain, node_tree.id);

  return OPERATOR_FINISHED;
}

void NODE_OT_link_make(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Make Links";
  ot->description = "Make a link between selected output and input sockets";
  ot->idname = "NODE_OT_link_make";

  /* callbacks */
  ot->exec = node_make_link_exec;
  /* XXX we need a special poll which checks that there are selected input/output sockets. */
  ot->poll = ED_operator_node_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_boolean(
      ot->srna, "replace", false, "Replace", "Replace socket connections with the new links");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Cut Link Operator
 * \{ */

static wmOperatorStatus cut_links_exec(bContext *C, wmOperator *op)
{
  Main &bmain = *CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  const ARegion &region = *CTX_wm_region(C);

  Vector<float2> path;
  RNA_BEGIN (op->ptr, itemptr, "path") {
    float2 loc_region;
    RNA_float_get_array(&itemptr, "loc", loc_region);
    float2 loc_view;
    ui::view2d_region_to_view(&region.v2d, loc_region.x, loc_region.y, &loc_view.x, &loc_view.y);
    path.append(loc_view);
    if (path.size() >= 256) {
      break;
    }
  }
  RNA_END;

  if (path.is_empty()) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  bool found = false;

  ED_preview_kill_jobs(CTX_wm_manager(C), &bmain);

  bNodeTree &node_tree = *snode.edittree;
  node_tree.ensure_topology_cache();

  Set<bNodeLink *> links_to_remove;
  for (bNodeLink &link : node_tree.links) {
    if (node_link_is_hidden_or_dimmed(region.v2d, link)) {
      continue;
    }

    if (link_path_intersection(link, path)) {

      if (!found) {
        /* TODO(sergey): Why did we kill jobs twice? */
        ED_preview_kill_jobs(CTX_wm_manager(C), &bmain);
        found = true;
      }
      links_to_remove.add(&link);
    }
  }

  Set<bNode *> affected_nodes;
  for (bNodeLink *link : links_to_remove) {
    bNode *to_node = link->tonode;
    bke::node_remove_link(snode.edittree, *link);
    affected_nodes.add(to_node);
  }

  node_tree.ensure_topology_cache();
  for (bNode *node : affected_nodes) {
    update_multi_input_indices_for_removed_links(*node);
  }

  BKE_main_ensure_invariants(*CTX_data_main(C), snode.edittree->id);
  if (found) {
    return OPERATOR_FINISHED;
  }

  return OPERATOR_CANCELLED;
}

void NODE_OT_links_cut(wmOperatorType *ot)
{
  ot->name = "Cut Links";
  ot->idname = "NODE_OT_links_cut";
  ot->description = "Use the mouse to cut (remove) some links";

  ot->invoke = WM_gesture_lines_invoke;
  ot->modal = WM_gesture_lines_modal;
  ot->exec = cut_links_exec;
  ot->cancel = WM_gesture_lines_cancel;

  ot->poll = ED_operator_node_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  /* properties */
  PropertyRNA *prop;
  prop = RNA_def_collection_runtime(ot->srna, "path", RNA_OperatorMousePath, "Path", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  /* internal */
  RNA_def_int(ot->srna, "cursor", WM_CURSOR_KNIFE, 0, INT_MAX, "Cursor", "", 0, INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Mute Links Operator
 * \{ */

bool all_links_muted(const bNodeSocket &socket)
{
  for (const bNodeLink *link : socket.directly_linked_links()) {
    if (!(link->flag & NODE_LINK_MUTED)) {
      return false;
    }
  }
  return true;
}

static wmOperatorStatus mute_links_exec(bContext *C, wmOperator *op)
{
  Main &bmain = *CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  const ARegion &region = *CTX_wm_region(C);
  bNodeTree &ntree = *snode.edittree;

  Vector<float2> path;
  RNA_BEGIN (op->ptr, itemptr, "path") {
    float2 loc_region;
    RNA_float_get_array(&itemptr, "loc", loc_region);
    float2 loc_view;
    ui::view2d_region_to_view(&region.v2d, loc_region.x, loc_region.y, &loc_view.x, &loc_view.y);
    path.append(loc_view);
    if (path.size() >= 256) {
      break;
    }
  }
  RNA_END;

  if (path.is_empty()) {
    return OPERATOR_CANCELLED | OPERATOR_PASS_THROUGH;
  }

  ED_preview_kill_jobs(CTX_wm_manager(C), &bmain);

  ntree.ensure_topology_cache();

  Set<bNodeLink *> affected_links;
  for (bNodeLink &link : ntree.links) {
    if (node_link_is_hidden_or_dimmed(region.v2d, link)) {
      continue;
    }
    if (!link_path_intersection(link, path)) {
      continue;
    }
    affected_links.add(&link);
  }

  if (affected_links.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  bke::node_tree_runtime::AllowUsingOutdatedInfo allow_outdated_info{ntree};

  for (bNodeLink *link : affected_links) {
    bke::node_link_set_mute(ntree, *link, !(link->flag & NODE_LINK_MUTED));
    const bool muted = link->flag & NODE_LINK_MUTED;

    /* Propagate mute status downstream past reroute nodes. */
    if (link->tonode->is_reroute()) {
      Stack<bNodeLink *> links;
      links.push_multiple(link->tonode->output_socket(0).directly_linked_links());
      while (!links.is_empty()) {
        bNodeLink *link = links.pop();
        bke::node_link_set_mute(ntree, *link, muted);
        if (!link->tonode->is_reroute()) {
          continue;
        }
        links.push_multiple(link->tonode->output_socket(0).directly_linked_links());
      }
    }
    /* Propagate mute status upstream past reroutes, but only if all outputs are muted. */
    if (link->fromnode->is_reroute()) {
      if (!muted || all_links_muted(*link->fromsock)) {
        Stack<bNodeLink *> links;
        links.push_multiple(link->fromnode->input_socket(0).directly_linked_links());
        while (!links.is_empty()) {
          bNodeLink *link = links.pop();
          bke::node_link_set_mute(ntree, *link, muted);
          if (!link->fromnode->is_reroute()) {
            continue;
          }
          if (!muted || all_links_muted(*link->fromsock)) {
            links.push_multiple(link->fromnode->input_socket(0).directly_linked_links());
          }
        }
      }
    }
  }

  BKE_main_ensure_invariants(*CTX_data_main(C), ntree.id);
  return OPERATOR_FINISHED;
}

void NODE_OT_links_mute(wmOperatorType *ot)
{
  ot->name = "Mute Links";
  ot->idname = "NODE_OT_links_mute";
  ot->description = "Use the mouse to mute links";

  ot->invoke = WM_gesture_lines_invoke;
  ot->modal = WM_gesture_lines_modal;
  ot->exec = mute_links_exec;
  ot->cancel = WM_gesture_lines_cancel;

  ot->poll = ED_operator_node_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_DEPENDS_ON_CURSOR;

  /* properties */
  PropertyRNA *prop;
  prop = RNA_def_collection_runtime(ot->srna, "path", RNA_OperatorMousePath, "Path", "");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);

  /* internal */
  RNA_def_int(ot->srna, "cursor", WM_CURSOR_MUTE, 0, INT_MAX, "Cursor", "", 0, INT_MAX);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Detach Links Operator
 * \{ */

static wmOperatorStatus detach_links_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &ntree = *snode.edittree;

  ED_preview_kill_jobs(CTX_wm_manager(C), CTX_data_main(C));

  for (bNode *node : ntree.all_nodes()) {
    if (node->flag & SELECT) {
      bke::node_internal_relink(ntree, *node);
    }
  }

  BKE_main_ensure_invariants(*CTX_data_main(C), ntree.id);
  return OPERATOR_FINISHED;
}

void NODE_OT_links_detach(wmOperatorType *ot)
{
  ot->name = "Detach Links";
  ot->idname = "NODE_OT_links_detach";
  ot->description =
      "Remove all links to selected nodes, and try to connect neighbor nodes together";

  ot->exec = detach_links_exec;
  ot->poll = ED_operator_node_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Set Parent Operator
 * \{ */

static wmOperatorStatus node_parent_set_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &ntree = *snode.edittree;
  bNode *frame = bke::node_get_active(ntree);
  if (!frame || !frame->is_frame()) {
    return OPERATOR_CANCELLED;
  }

  for (bNode *node : ntree.all_nodes()) {
    if (node == frame) {
      continue;
    }
    if (node->flag & NODE_SELECT) {
      bke::node_detach_node(ntree, *node);
      bke::node_attach_node(ntree, *node, *frame);
    }
  }

  tree_draw_order_update(ntree);
  WM_event_add_notifier(C, NC_NODE | ND_DISPLAY, nullptr);

  return OPERATOR_FINISHED;
}

void NODE_OT_parent_set(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Make Parent";
  ot->description = "Attach selected nodes";
  ot->idname = "NODE_OT_parent_set";

  /* API callbacks. */
  ot->exec = node_parent_set_exec;
  ot->poll = ED_operator_node_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Join Nodes in Frame Operator
 * \{ */

struct NodeJoinState {
  bool done;
  bool descendent;
};

static void node_join_attach_recursive(bNodeTree &ntree,
                                       MutableSpan<NodeJoinState> join_states,
                                       bNode *node,
                                       bNode *frame,
                                       const VectorSet<bNode *> &selected_nodes)
{
  join_states[node->index()].done = true;

  if (node == frame) {
    join_states[node->index()].descendent = true;
  }
  else if (node->parent) {
    /* call recursively */
    if (!join_states[node->parent->index()].done) {
      node_join_attach_recursive(ntree, join_states, node->parent, frame, selected_nodes);
    }

    /* in any case: if the parent is a descendant, so is the child */
    if (join_states[node->parent->index()].descendent) {
      join_states[node->index()].descendent = true;
    }
    else if (selected_nodes.contains(node)) {
      /* if parent is not an descendant of the frame, reattach the node */
      bke::node_detach_node(ntree, *node);
      bke::node_attach_node(ntree, *node, *frame);
      join_states[node->index()].descendent = true;
    }
  }
  else if (selected_nodes.contains(node)) {
    bke::node_attach_node(ntree, *node, *frame);
    join_states[node->index()].descendent = true;
  }
}

static Vector<bNode *> get_sorted_node_parents(const bNode &node)
{
  Vector<bNode *> parents;
  for (bNode *parent = node.parent; parent; parent = parent->parent) {
    parents.append(parent);
  }
  /* Reverse so that the root frame is the first element (if there is any). */
  std::reverse(parents.begin(), parents.end());
  return parents;
}

bNode *find_common_parent_node(const Span<bNode *> nodes)
{
  if (nodes.is_empty()) {
    return nullptr;
  }
  /* The common parent node also has to be a parent of the first node. */
  Vector<bNode *> candidates = get_sorted_node_parents(*nodes[0]);
  for (const bNode *node : nodes.drop_front(1)) {
    const Vector<bNode *> parents = get_sorted_node_parents(*node);
    /* Possibly shrink set of candidates so that it only contains the parents common with the
     * current node. */
    candidates.resize(std::min(candidates.size(), parents.size()));
    for (const int i : candidates.index_range()) {
      if (candidates[i] != parents[i]) {
        candidates.resize(i);
        break;
      }
    }
    if (candidates.is_empty()) {
      break;
    }
  }
  if (candidates.is_empty()) {
    return nullptr;
  }
  return candidates.last();
}

static wmOperatorStatus node_join_in_frame_exec(bContext *C, wmOperator * /*op*/)
{
  Main &bmain = *CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &ntree = *snode.edittree;

  const VectorSet<bNode *> selected_nodes = get_selected_nodes(ntree);

  bNode *frame_node = add_static_node(*C, NODE_FRAME, snode.runtime->cursor);
  bke::node_set_active(ntree, *frame_node);
  frame_node->parent = find_common_parent_node(selected_nodes.as_span());

  ntree.ensure_topology_cache();

  Array<NodeJoinState> join_states(ntree.all_nodes().size(), NodeJoinState{false, false});

  for (bNode *node : ntree.all_nodes()) {
    if (!join_states[node->index()].done) {
      node_join_attach_recursive(ntree, join_states, node, frame_node, selected_nodes);
    }
  }

  tree_draw_order_update(ntree);
  BKE_main_ensure_invariants(bmain, snode.edittree->id);
  WM_event_add_notifier(C, NC_NODE | ND_DISPLAY, nullptr);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus node_join_in_frame_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  ARegion *region = CTX_wm_region(C);
  SpaceNode *snode = CTX_wm_space_node(C);

  /* Convert mouse coordinates to v2d space. */
  ui::view2d_region_to_view(&region->v2d,
                            event->mval[0],
                            event->mval[1],
                            &snode->runtime->cursor[0],
                            &snode->runtime->cursor[1]);

  snode->runtime->cursor[0] /= UI_SCALE_FAC;
  snode->runtime->cursor[1] /= UI_SCALE_FAC;

  return node_join_in_frame_exec(C, op);
}

void NODE_OT_join(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Join Nodes in Frame";
  ot->description = "Attach selected nodes to a new common frame";
  ot->idname = "NODE_OT_join";

  /* API callbacks. */
  ot->exec = node_join_in_frame_exec;
  ot->invoke = node_join_in_frame_invoke;
  ot->poll = ED_operator_node_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Join Nodes Operator
 * \{ */

static void join_group_inputs(bNodeTree &tree, VectorSet<bNode *> group_inputs, bNode *active_node)
{
  bNode *main_node = nullptr;
  if (group_inputs.contains(active_node)) {
    main_node = active_node;
  }
  else {
    main_node = group_inputs[0];
    /* Move main node to average of all group inputs. */
    float2 location{};
    for (const bNode *node : group_inputs) {
      location += node->location;
    }
    location /= float(group_inputs.size());
    copy_v2_v2(main_node->location, location);
  }
  tree.ensure_topology_cache();
  MultiValueMap<bNodeSocket *, bNodeLink *> old_link_map;
  for (bNode *node : group_inputs) {
    for (bNodeSocket *socket : node->output_sockets().drop_back(1)) {
      old_link_map.add_multiple(socket, socket->directly_linked_links());
    }
  }
  MultiValueMap<bNodeSocket *, bNodeSocket *> used_link_targets;
  for (bNodeSocket *socket : main_node->output_sockets()) {
    used_link_targets.add_multiple(socket, socket->directly_linked_sockets());
  }
  for (bNode *node : group_inputs) {
    if (node == main_node) {
      continue;
    }
    bool keep_node = false;

    /* Using runtime data directly because we know the parts that are used are still valid. */
    for (const int group_input_i : node->runtime->outputs.index_range().drop_back(1)) {
      bool keep_socket = false;
      bNodeSocket &new_socket = *main_node->runtime->outputs[group_input_i];
      bNodeSocket &old_socket = *node->runtime->outputs[group_input_i];
      for (bNodeLink *link : old_link_map.lookup(&old_socket)) {
        bNodeSocket &to_socket = *link->tosock;
        if (used_link_targets.lookup(&new_socket).contains(&to_socket)) {
          keep_node = true;
          keep_socket = true;
          continue;
        }
        used_link_targets.add(&new_socket, &to_socket);
        link->fromsock = &new_socket;
        link->fromnode = main_node;
        new_socket.flag &= ~SOCK_HIDDEN;
        BKE_ntree_update_tag_link_changed(&tree);
      }
      if (!keep_socket) {
        old_socket.flag |= SOCK_HIDDEN;
      }
    }
    if (!keep_node) {
      bke::node_free_node(&tree, *node);
    }
  }
}

static wmOperatorStatus node_join_nodes_exec(bContext *C, wmOperator *op)
{
  Main &bmain = *CTX_data_main(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &ntree = *snode.edittree;

  bNode *active_node = bke::node_get_active(ntree);
  VectorSet<bNode *> selected_nodes = get_selected_nodes(ntree);
  if (selected_nodes.size() <= 1) {
    return OPERATOR_CANCELLED;
  }

  if (std::all_of(selected_nodes.begin(), selected_nodes.end(), [](const bNode *node) {
        return node->is_group_input();
      }))
  {
    join_group_inputs(ntree, std::move(selected_nodes), active_node);
  }
  else {
    BKE_report(op->reports, RPT_ERROR, "Selected nodes can't be joined");
    return OPERATOR_CANCELLED;
  }

  BKE_main_ensure_invariants(bmain, snode.edittree->id);
  WM_event_add_notifier(C, NC_NODE | ND_DISPLAY, nullptr);

  return OPERATOR_FINISHED;
}

void NODE_OT_join_nodes(wmOperatorType *ot)
{
  ot->name = "Join Nodes";
  ot->description = "Merge selected group input nodes into one if possible";
  ot->idname = "NODE_OT_join_nodes";

  ot->exec = node_join_nodes_exec;
  ot->poll = ED_operator_node_editable;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Attach Operator
 * \{ */

static bNode *node_find_frame_to_attach(ARegion &region, bNodeTree &ntree, const int2 mouse_xy)
{
  /* convert mouse coordinates to v2d space */
  float2 cursor;
  ui::view2d_region_to_view(&region.v2d, mouse_xy.x, mouse_xy.y, &cursor.x, &cursor.y);

  for (bNode *frame : tree_draw_order_calc_nodes_reversed(ntree)) {
    /* skip selected, those are the nodes we want to attach */
    if (!frame->is_frame() || (frame->flag & NODE_SELECT)) {
      continue;
    }
    if (BLI_rctf_isect_pt_v(&frame->runtime->draw_bounds, cursor)) {
      return frame;
    }
  }

  return nullptr;
}

static bool can_attach_node_to_frame(const bNode &node, const bNode &frame)
{
  /* Disallow moving a parent into its child. */
  if (node.is_frame() && bke::node_is_parent_and_child(node, frame)) {
    return false;
  }
  if (node.parent == nullptr) {
    return true;
  }
  if (node.parent == &frame) {
    return false;
  }
  /* Attach nodes which share parent with the frame. */
  const bool share_parent = bke::node_is_parent_and_child(*node.parent, frame);
  if (!share_parent) {
    return false;
  }
  return true;
}

static wmOperatorStatus node_attach_invoke(bContext *C, wmOperator * /*op*/, const wmEvent *event)
{
  ARegion &region = *CTX_wm_region(C);
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &ntree = *snode.edittree;
  bNode *frame = node_find_frame_to_attach(region, ntree, event->mval);
  if (frame == nullptr) {
    /* Return "finished" so that auto offset operator macros can work. */
    return OPERATOR_FINISHED;
  }

  bool changed = false;

  for (bNode *node : tree_draw_order_calc_nodes_reversed(*snode.edittree)) {
    if (!(node->flag & NODE_SELECT)) {
      continue;
    }
    if (!can_attach_node_to_frame(*node, *frame)) {
      continue;
    }
    bke::node_detach_node(ntree, *node);
    bke::node_attach_node(ntree, *node, *frame);
    changed = true;
  }

  if (changed) {
    tree_draw_order_update(ntree);
    WM_event_add_notifier(C, NC_NODE | ND_DISPLAY, nullptr);
  }

  return OPERATOR_FINISHED;
}

void NODE_OT_attach(wmOperatorType *ot)
{
  ot->name = "Attach Nodes";
  ot->description = "Attach active node to a frame";
  ot->idname = "NODE_OT_attach";

  ot->invoke = node_attach_invoke;
  ot->poll = ED_operator_node_editable;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Detach Operator
 * \{ */

struct NodeDetachstate {
  bool done;
  bool descendent;
};

static void node_detach_recursive(bNodeTree &ntree,
                                  MutableSpan<NodeDetachstate> detach_states,
                                  bNode *node)
{
  detach_states[node->index()].done = true;

  if (node->parent) {
    /* Call recursively. */
    if (!detach_states[node->parent->index()].done) {
      node_detach_recursive(ntree, detach_states, node->parent);
    }

    /* In any case: if the parent is a descendant, so is the child. */
    if (detach_states[node->parent->index()].descendent) {
      detach_states[node->index()].descendent = true;
    }
    else if (node->flag & NODE_SELECT) {
      /* If parent is not a descendant of a selected node, detach. */
      bke::node_detach_node(ntree, *node);
      detach_states[node->index()].descendent = true;
    }
  }
  else if (node->flag & NODE_SELECT) {
    detach_states[node->index()].descendent = true;
  }
}

/* Detach the root nodes in the current selection. */
static wmOperatorStatus node_detach_exec(bContext *C, wmOperator * /*op*/)
{
  SpaceNode &snode = *CTX_wm_space_node(C);
  bNodeTree &ntree = *snode.edittree;

  Array<NodeDetachstate> detach_states(ntree.all_nodes().size(), NodeDetachstate{false, false});

  /* Detach nodes recursively. Relative order is preserved here. */
  for (bNode *node : ntree.all_nodes()) {
    if (!detach_states[node->index()].done) {
      node_detach_recursive(ntree, detach_states, node);
    }
  }

  tree_draw_order_update(ntree);
  WM_event_add_notifier(C, NC_NODE | ND_DISPLAY, nullptr);

  return OPERATOR_FINISHED;
}

void NODE_OT_detach(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Detach Nodes";
  ot->description = "Detach selected nodes from parents";
  ot->idname = "NODE_OT_detach";

  /* API callbacks. */
  ot->exec = node_detach_exec;
  ot->poll = ED_operator_node_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Automatic Node Insert on Dragging
 * \{ */

static bNode *get_selected_node_for_insertion(bNodeTree &node_tree)
{
  bNode *selected_node = nullptr;
  int selected_node_count = 0;
  for (bNode *node : node_tree.all_nodes()) {
    if (node->flag & SELECT) {
      selected_node = node;
      selected_node_count++;
    }
    if (selected_node_count > 1) {
      return nullptr;
    }
  }
  if (!selected_node) {
    return nullptr;
  }
  if (selected_node->input_sockets().is_empty() || selected_node->output_sockets().is_empty()) {
    return nullptr;
  }
  return selected_node;
}

static bool node_can_be_inserted_on_link(bNodeTree &tree, bNode &node, const bNodeLink &link)
{
  const bNodeSocket *main_input = get_main_socket(tree, node, SOCK_IN);
  const bNodeSocket *main_output = get_main_socket(tree, node, SOCK_IN);
  if (ELEM(nullptr, main_input, main_output)) {
    return false;
  }
  if (node.is_reroute()) {
    return true;
  }
  if (!tree.typeinfo->validate_link) {
    return true;
  }
  if (!tree.typeinfo->validate_link(eNodeSocketDatatype(link.fromsock->type),
                                    eNodeSocketDatatype(main_input->type)))
  {
    return false;
  }
  if (!tree.typeinfo->validate_link(eNodeSocketDatatype(main_output->type),
                                    eNodeSocketDatatype(link.tosock->type)))
  {
    return false;
  }
  return true;
}

void node_insert_on_link_flags_set(SpaceNode &snode,
                                   const ARegion &region,
                                   const bool attach_enabled,
                                   const bool is_new_node)
{
  bNodeTree &node_tree = *snode.edittree;
  node_tree.ensure_topology_cache();

  node_insert_on_link_flags_clear(node_tree);

  bNode *node_to_insert = get_selected_node_for_insertion(node_tree);
  if (!node_to_insert) {
    return;
  }
  Vector<bNodeSocket *> already_linked_sockets;
  for (bNodeSocket *socket : node_to_insert->input_sockets()) {
    already_linked_sockets.extend(socket->directly_linked_sockets());
  }
  for (bNodeSocket *socket : node_to_insert->output_sockets()) {
    already_linked_sockets.extend(socket->directly_linked_sockets());
  }
  if (!is_new_node && !already_linked_sockets.is_empty()) {
    return;
  }

  /* Find link to select/highlight. */
  bNodeLink *selink = nullptr;
  float dist_best = FLT_MAX;
  for (bNodeLink &link : node_tree.links) {
    if (node_link_is_hidden_or_dimmed(region.v2d, link)) {
      continue;
    }
    if (ELEM(node_to_insert, link.fromnode, link.tonode)) {
      /* Don't insert on a link that is connected to the node already. */
      continue;
    }
    if (is_new_node && !already_linked_sockets.is_empty()) {
      /* Only allow links coming from or going to the already linked socket after
       * link-drag-search. */
      bool is_linked_to_linked = false;
      for (const bNodeSocket *socket : already_linked_sockets) {
        if (ELEM(socket, link.fromsock, link.tosock)) {
          is_linked_to_linked = true;
          break;
        }
      }
      if (!is_linked_to_linked) {
        continue;
      }
    }

    std::array<float2, NODE_LINK_RESOL + 1> coords;
    node_link_bezier_points_evaluated(link, coords);
    float dist = FLT_MAX;

    /* Loop over link coords to find shortest dist to upper left node edge of a intersected line
     * segment. */
    for (int i = 0; i < NODE_LINK_RESOL; i++) {
      /* Check if the node rectangle intersects the line from this point to next one. */
      if (BLI_rctf_isect_segment(&node_to_insert->runtime->draw_bounds, coords[i], coords[i + 1]))
      {
        /* Store the shortest distance to the upper left edge of all intersections found so far. */
        const float node_xy[] = {node_to_insert->runtime->draw_bounds.xmin,
                                 node_to_insert->runtime->draw_bounds.ymax};

        /* To be precise coords should be clipped by `select->draw_bounds`, but not done since
         * there's no real noticeable difference. */
        dist = min_ff(dist_squared_to_line_segment_v2(node_xy, coords[i], coords[i + 1]), dist);
      }
    }

    /* We want the link with the shortest distance to node center. */
    if (dist < dist_best) {
      dist_best = dist;
      selink = &link;
    }
  }

  if (selink) {
    selink->flag |= NODE_LINK_INSERT_TARGET;
    if (!attach_enabled || !node_can_be_inserted_on_link(node_tree, *node_to_insert, *selink)) {
      selink->flag |= NODE_LINK_INSERT_TARGET_INVALID;
    }
  }
}

void node_insert_on_frame_flag_set(bContext &C, SpaceNode &snode, const int2 &cursor)
{
  snode.runtime->frame_identifier_to_highlight.reset();

  ARegion &region = *CTX_wm_region(&C);

  snode.edittree->ensure_topology_cache();
  const bNode *frame = node_find_frame_to_attach(region, *snode.edittree, cursor);
  if (!frame) {
    return;
  }
  for (const bNode *node : snode.edittree->all_nodes()) {
    if (!(node->flag & NODE_SELECT)) {
      continue;
    }
    if (!can_attach_node_to_frame(*node, *frame)) {
      continue;
    }
    /* We detected that a node can be attached to the frame, so highlight it. */
    snode.runtime->frame_identifier_to_highlight = frame->identifier;
    return;
  }
}

void node_insert_on_frame_flag_clear(SpaceNode &snode)
{
  snode.runtime->frame_identifier_to_highlight.reset();
}

void node_insert_on_link_flags_clear(bNodeTree &node_tree)
{
  for (bNodeLink &link : node_tree.links) {
    link.flag &= ~(NODE_LINK_INSERT_TARGET | NODE_LINK_INSERT_TARGET_INVALID);
  }
}

void node_insert_on_link_flags(Main &bmain, SpaceNode &snode, bool is_new_node)
{
  bNodeTree &node_tree = *snode.edittree;
  node_tree.ensure_topology_cache();
  bNode *node_to_insert = get_selected_node_for_insertion(node_tree);
  if (!node_to_insert) {
    return;
  }

  /* Find link to insert on. */
  bNodeTree &ntree = *snode.edittree;
  bNodeLink *old_link = nullptr;
  for (bNodeLink &link : ntree.links) {
    if (link.flag & NODE_LINK_INSERT_TARGET) {
      if (!(link.flag & NODE_LINK_INSERT_TARGET_INVALID)) {
        old_link = &link;
      }
      break;
    }
  }
  node_insert_on_link_flags_clear(node_tree);
  if (old_link == nullptr) {
    return;
  }

  bNodeSocket *best_input = nullptr;
  if (is_new_node) {
    for (bNodeSocket *socket : node_to_insert->input_sockets()) {
      if (!socket->directly_linked_sockets().is_empty()) {
        best_input = socket;
        break;
      }
    }
  }
  if (!best_input) {
    best_input = get_main_socket(ntree, *node_to_insert, SOCK_IN);
  }
  bNodeSocket *best_output = nullptr;
  if (is_new_node) {
    for (bNodeSocket *socket : node_to_insert->output_sockets()) {
      if (!socket->directly_linked_sockets().is_empty()) {
        best_output = socket;
        break;
      }
    }
  }
  if (!best_output) {
    best_output = get_main_socket(ntree, *node_to_insert, SOCK_OUT);
  }

  if (!node_to_insert->is_reroute()) {
    /* Ignore main sockets when the types don't match. */
    if (best_input != nullptr && ntree.typeinfo->validate_link != nullptr &&
        !ntree.typeinfo->validate_link(eNodeSocketDatatype(old_link->fromsock->type),
                                       eNodeSocketDatatype(best_input->type)))
    {
      best_input = nullptr;
    }
    if (best_output != nullptr && ntree.typeinfo->validate_link != nullptr &&
        !ntree.typeinfo->validate_link(eNodeSocketDatatype(best_output->type),
                                       eNodeSocketDatatype(old_link->tosock->type)))
    {
      best_output = nullptr;
    }
  }

  bNode *from_node = old_link->fromnode;
  bNodeSocket *from_socket = old_link->fromsock;
  bNode *to_node = old_link->tonode;

  const bool best_input_is_linked = best_input && best_input->is_directly_linked();

  if (best_output != nullptr) {
    /* Relink the "start" of the existing link to the newly inserted node. */
    old_link->fromnode = node_to_insert;
    old_link->fromsock = best_output;
    BKE_ntree_update_tag_link_changed(&ntree);
  }
  else {
    bke::node_remove_link(&ntree, *old_link);
  }

  if (best_input != nullptr) {
    /* Don't change an existing link. */
    if (!best_input_is_linked) {
      /* Add a new link that connects the node on the left to the newly inserted node. */
      bke::node_add_link(ntree, *from_node, *from_socket, *node_to_insert, *best_input);
    }
  }

  /* Set up insert offset data, it needs stuff from here. */
  if (U.uiflag & USER_NODE_AUTO_OFFSET) {
    BLI_assert(snode.runtime->iofsd == nullptr);
    NodeInsertOfsData *iofsd = MEM_new_zeroed<NodeInsertOfsData>(__func__);

    iofsd->insert = node_to_insert;
    iofsd->prev = from_node;
    iofsd->next = to_node;

    snode.runtime->iofsd = iofsd;
  }

  BKE_main_ensure_invariants(bmain, ntree.id);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Node Insert Offset Operator
 * \{ */

static int get_main_socket_priority(const bNodeSocket *socket)
{
  switch (eNodeSocketDatatype(socket->type)) {
    case SOCK_CUSTOM:
      return 0;
    case SOCK_MENU:
      return 1;
    case SOCK_BOOLEAN:
      return 2;
    case SOCK_INT:
      return 3;
    case SOCK_FLOAT:
      return 4;
    case SOCK_VECTOR:
      return 5;
    case SOCK_RGBA:
      return 6;
    case SOCK_INT_VECTOR:
    case SOCK_STRING:
    case SOCK_SHADER:
    case SOCK_OBJECT:
    case SOCK_IMAGE:
    case SOCK_ROTATION:
    case SOCK_MATRIX:
    case SOCK_GEOMETRY:
    case SOCK_COLLECTION:
    case SOCK_TEXTURE:
    case SOCK_MATERIAL:
    case SOCK_FONT:
    case SOCK_SCENE:
    case SOCK_TEXT_ID:
    case SOCK_MASK:
    case SOCK_SOUND:
    case SOCK_BUNDLE:
    case SOCK_CLOSURE:
      return 7;
  }
  return -1;
}

bNodeSocket *get_main_socket(bNodeTree &ntree, bNode &node, eNodeSocketInOut in_out)
{
  ListBaseT<bNodeSocket> *sockets = (in_out == SOCK_IN) ? &node.inputs : &node.outputs;

  /* Try to get the main socket based on the socket declaration. */
  bke::node_declaration_ensure(ntree, node);
  const nodes::NodeDeclaration *node_decl = node.declaration();
  if (node_decl != nullptr) {
    Span<nodes::SocketDeclaration *> socket_decls = (in_out == SOCK_IN) ? node_decl->inputs :
                                                                          node_decl->outputs;
    for (const nodes::SocketDeclaration *socket_decl : socket_decls) {
      if (!socket_decl->is_default_link_socket) {
        continue;
      }
      bNodeSocket *socket = static_cast<bNodeSocket *>(BLI_findlink(sockets, socket_decl->index));
      if (socket && socket->is_visible()) {
        return socket;
      }
    }
  }

  /* Find priority range. */
  int maxpriority = -1;
  for (bNodeSocket &sock : *sockets) {
    if (sock.flag & SOCK_UNAVAIL) {
      continue;
    }
    maxpriority = max_ii(get_main_socket_priority(&sock), maxpriority);
  }

  /* Try all priorities, starting from 'highest'. */
  for (int priority = maxpriority; priority >= 0; priority--) {
    for (bNodeSocket &sock : *sockets) {
      if (!!sock.is_visible() && priority == get_main_socket_priority(&sock)) {
        return &sock;
      }
    }
  }

  return nullptr;
}

static bool node_parents_offset_flag_enable_cb(bNode *parent, void * /*userdata*/)
{
  /* NODE_TEST is used to flag nodes that shouldn't be offset (again) */
  parent->flag |= NODE_TEST;

  return true;
}

static void node_offset_apply(bNode &node, const float offset_x)
{
  /* NODE_TEST is used to flag nodes that shouldn't be offset (again) */
  if ((node.flag & NODE_TEST) == 0) {
    node.runtime->anim_ofsx = (offset_x / UI_SCALE_FAC);
    node.flag |= NODE_TEST;
  }
}

#define NODE_INSOFS_ANIM_DURATION 0.25f

/**
 * Callback that applies NodeInsertOfsData.offset_x to a node or its parent,
 * considering the logic needed for offsetting nodes after link insert
 */
static bool node_link_insert_offset_chain_cb(bNode *fromnode,
                                             bNode *tonode,
                                             void *userdata,
                                             const bool reversed)
{
  NodeInsertOfsData *data = static_cast<NodeInsertOfsData *>(userdata);
  bNode *ofs_node = reversed ? fromnode : tonode;

  node_offset_apply(*ofs_node, data->offset_x);

  return true;
}

static bool node_link_insert_offset_ntree(NodeInsertOfsData *iofsd,
                                          ARegion *region,
                                          const int mouse_xy[2],
                                          const bool right_alignment)
{
  bNodeTree *ntree = iofsd->ntree;
  bNode &insert = *iofsd->insert;
  bNode *prev = iofsd->prev, *next = iofsd->next;
  bNode *init_parent = insert.parent; /* store old insert.parent for restoring later */

  const float min_margin = U.node_margin * UI_SCALE_FAC;
  const float width = NODE_WIDTH(insert);
  const bool needs_alignment = (next->runtime->draw_bounds.xmin -
                                prev->runtime->draw_bounds.xmax) < (width + (min_margin * 2.0f));

  float margin = width;

  /* NODE_TEST will be used later, so disable for all nodes */
  bke::node_tree_node_flag_set(*ntree, NODE_TEST, false);

  /* `insert.draw_bounds` isn't updated yet,
   * so `totr_insert` is used to get the correct world-space coords. */
  rctf totr_insert;
  node_to_updated_rect(insert, totr_insert);

  const float gap_left = totr_insert.xmin - prev->runtime->draw_bounds.xmax;
  const float gap_right = next->runtime->draw_bounds.xmin - totr_insert.xmax;
  if (gap_left >= min_margin && gap_right >= min_margin) {
    return false;
  }

  /* Frame attachment wasn't handled yet so we search the frame that the node will be attached to
   * later. */
  insert.parent = node_find_frame_to_attach(*region, *ntree, mouse_xy);

  /* This makes sure nodes are also correctly offset when inserting a node on top of a frame
   * without actually making it a part of the frame (because mouse isn't intersecting it)
   * - logic here is similar to node_find_frame_to_attach. */
  if (!insert.parent ||
      (prev->parent && (prev->parent == next->parent) && (prev->parent != insert.parent)))
  {
    rctf totr_frame;

    /* check nodes front to back */
    for (bNode *frame : tree_draw_order_calc_nodes_reversed(*ntree)) {
      /* skip selected, those are the nodes we want to attach */
      if (!frame->is_frame() || (frame->flag & NODE_SELECT)) {
        continue;
      }

      /* for some reason frame y coords aren't correct yet */
      node_to_updated_rect(*frame, totr_frame);

      if (BLI_rctf_isect_x(&totr_frame, totr_insert.xmin) &&
          BLI_rctf_isect_x(&totr_frame, totr_insert.xmax))
      {
        if (BLI_rctf_isect_y(&totr_frame, totr_insert.ymin) ||
            BLI_rctf_isect_y(&totr_frame, totr_insert.ymax))
        {
          /* frame isn't insert.parent actually, but this is needed to make offsetting
           * nodes work correctly for above checked cases (it is restored later) */
          insert.parent = frame;
          break;
        }
      }
    }
  }

  /* *** ensure offset at the left (or right for right_alignment case) of insert_node *** */

  float dist = right_alignment ? gap_left : gap_right;
  /* distance between insert_node and prev is smaller than min margin */
  if (dist < min_margin) {
    const float addval = (min_margin - dist) * (right_alignment ? 1.0f : -1.0f);

    node_offset_apply(insert, addval);

    totr_insert.xmin += addval;
    totr_insert.xmax += addval;
    margin += min_margin;
  }

  /* *** ensure offset at the right (or left for right_alignment case) of insert_node *** */

  dist = right_alignment ? next->runtime->draw_bounds.xmin - totr_insert.xmax :
                           totr_insert.xmin - prev->runtime->draw_bounds.xmax;
  /* distance between insert_node and next is smaller than min margin */
  if (dist < min_margin) {
    const float addval = (min_margin - dist) * (right_alignment ? 1.0f : -1.0f);
    if (needs_alignment) {
      bNode *offs_node = right_alignment ? next : prev;
      node_offset_apply(*offs_node, addval);
      margin = addval;
    }
    /* enough room is available, but we want to ensure the min margin at the right */
    else {
      /* offset inserted node so that min margin is kept at the right */
      node_offset_apply(insert, -addval);
    }
  }

  if (needs_alignment) {
    iofsd->offset_x = margin;

    /* flag all parents of insert as offset to prevent them from being offset */
    bke::node_parents_iterator(&insert, node_parents_offset_flag_enable_cb, nullptr);
    /* iterate over entire chain and apply offsets */
    bke::node_chain_iterator(ntree,
                             right_alignment ? next : prev,
                             node_link_insert_offset_chain_cb,
                             iofsd,
                             !right_alignment);
  }

  insert.parent = init_parent;
  return true;
}

/**
 * Modal handler for insert offset animation
 */
static wmOperatorStatus node_insert_offset_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  NodeInsertOfsData *iofsd = static_cast<NodeInsertOfsData *>(op->customdata);
  bool redraw = false;

  if (!snode || event->type != TIMER || iofsd == nullptr || iofsd->anim_timer != event->customdata)
  {
    return OPERATOR_PASS_THROUGH;
  }

  const float duration = float(iofsd->anim_timer->time_duration);

  /* handle animation - do this before possibly aborting due to duration, since
   * main thread might be so busy that node hasn't reached final position yet */
  for (bNode *node : snode->edittree->all_nodes()) {
    if (UNLIKELY(node->runtime->anim_ofsx)) {
      const float prev_duration = duration - float(iofsd->anim_timer->time_delta);
      /* Clamp duration to not overshoot. */
      const float clamped_duration = math::min(duration, NODE_INSOFS_ANIM_DURATION);
      if (prev_duration < clamped_duration) {
        const float offset_step = node->runtime->anim_ofsx *
                                  (BLI_easing_cubic_ease_in_out(
                                       clamped_duration, 0.0f, 1.0f, NODE_INSOFS_ANIM_DURATION) -
                                   BLI_easing_cubic_ease_in_out(
                                       prev_duration, 0.0f, 1.0f, NODE_INSOFS_ANIM_DURATION));
        node->location[0] += offset_step;
        redraw = true;
      }
    }
  }
  if (redraw) {
    ED_region_tag_redraw(CTX_wm_region(C));
  }

  /* end timer + free insert offset data */
  if (duration > NODE_INSOFS_ANIM_DURATION) {
    WM_event_timer_remove(CTX_wm_manager(C), nullptr, iofsd->anim_timer);

    for (bNode *node : snode->edittree->all_nodes()) {
      node->runtime->anim_ofsx = 0.0f;
    }

    MEM_delete(iofsd);

    return (OPERATOR_FINISHED | OPERATOR_PASS_THROUGH);
  }

  return OPERATOR_RUNNING_MODAL;
}

#undef NODE_INSOFS_ANIM_DURATION

static wmOperatorStatus node_insert_offset_invoke(bContext *C,
                                                  wmOperator *op,
                                                  const wmEvent *event)
{
  const SpaceNode *snode = CTX_wm_space_node(C);
  NodeInsertOfsData *iofsd = snode->runtime->iofsd;
  snode->runtime->iofsd = nullptr;
  op->customdata = iofsd;

  if (!iofsd || !iofsd->insert) {
    return OPERATOR_CANCELLED;
  }

  BLI_assert(U.uiflag & USER_NODE_AUTO_OFFSET);

  iofsd->ntree = snode->edittree;

  const bool offset_applied = node_link_insert_offset_ntree(
      iofsd, CTX_wm_region(C), event->mval, (snode->insert_ofs_dir == SNODE_INSERTOFS_DIR_RIGHT));
  if (!offset_applied) {
    MEM_delete(iofsd);
    op->customdata = nullptr;
    return OPERATOR_CANCELLED;
  }

  iofsd->anim_timer = WM_event_timer_add(CTX_wm_manager(C), CTX_wm_window(C), TIMER, 0.02);

  /* add temp handler */
  WM_event_add_modal_handler(C, op);

  return OPERATOR_RUNNING_MODAL;
}

void NODE_OT_insert_offset(wmOperatorType *ot)
{
  /* identifiers */
  ot->name = "Insert Offset";
  ot->description = "Automatically offset nodes on insertion";
  ot->idname = "NODE_OT_insert_offset";

  /* callbacks */
  ot->invoke = node_insert_offset_invoke;
  ot->modal = node_insert_offset_modal;
  ot->poll = ED_operator_node_editable;

  /* flags */
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
}

/** \} */

}  // namespace ed::space_node

/* Copy socket default value to simple struct for undo. */
struct SocketValueBackup {
  int socket_type;
  float float_value[4];
  int int_value;
  char bool_value;
  char string_value[256];
};

static SocketValueBackup *socket_value_to_backup(bNodeSocket *sock)
{
  if (!sock || !sock->default_value) {
    return nullptr;
  }

  SocketValueBackup *backup = MEM_new<SocketValueBackup>(__func__);
  backup->socket_type = sock->type;

  switch (sock->type) {
    case SOCK_FLOAT: {
      bNodeSocketValueFloat *val = (bNodeSocketValueFloat *)sock->default_value;
      backup->float_value[0] = val->value;
      break;
    }
    case SOCK_INT: {
      bNodeSocketValueInt *val = (bNodeSocketValueInt *)sock->default_value;
      backup->int_value = val->value;
      break;
    }
    case SOCK_BOOLEAN: {
      bNodeSocketValueBoolean *val = (bNodeSocketValueBoolean *)sock->default_value;
      backup->bool_value = val->value;
      break;
    }
    case SOCK_VECTOR: {
      bNodeSocketValueVector *val = (bNodeSocketValueVector *)sock->default_value;
      for (int i = 0; i < 4; i++) {
        backup->float_value[i] = val->value[i];
      }
      break;
    }
    case SOCK_RGBA: {
      bNodeSocketValueRGBA *val = (bNodeSocketValueRGBA *)sock->default_value;
      for (int i = 0; i < 4; i++) {
        backup->float_value[i] = val->value[i];
      }
      break;
    }
    case SOCK_STRING: {
      bNodeSocketValueString *val = (bNodeSocketValueString *)sock->default_value;
      BLI_strncpy(backup->string_value, val->value, sizeof(backup->string_value));
      break;
    }
    default:
      MEM_delete(backup);
      return nullptr;
  }

  return backup;
}

static void restore_socket_value(bNodeSocket *sock, SocketValueBackup *backup)
{
  if (!sock || !sock->default_value || !backup) {
    return;
  }

  switch (sock->type) {
    case SOCK_FLOAT: {
      bNodeSocketValueFloat *val = (bNodeSocketValueFloat *)sock->default_value;
      val->value = backup->float_value[0];
      break;
    }
    case SOCK_INT: {
      bNodeSocketValueInt *val = (bNodeSocketValueInt *)sock->default_value;
      val->value = backup->int_value;
      break;
    }
    case SOCK_BOOLEAN: {
      bNodeSocketValueBoolean *val = (bNodeSocketValueBoolean *)sock->default_value;
      val->value = backup->bool_value;
      break;
    }
    case SOCK_VECTOR: {
      bNodeSocketValueVector *val = (bNodeSocketValueVector *)sock->default_value;
      for (int i = 0; i < 4; i++) {
        val->value[i] = backup->float_value[i];
      }
      break;
    }
    case SOCK_RGBA: {
      bNodeSocketValueRGBA *val = (bNodeSocketValueRGBA *)sock->default_value;
      for (int i = 0; i < 4; i++) {
        val->value[i] = backup->float_value[i];
      }
      break;
    }
    case SOCK_STRING: {
      bNodeSocketValueString *val = (bNodeSocketValueString *)sock->default_value;
      BLI_strncpy(val->value, backup->string_value, sizeof(val->value));
      break;
    }
    default:
      break;
  }
}

/* -------------------------------------------------------------------- */
/** \name Property Send Operator
 * \brief Send a custom property to a node socket as a driver
 * \{ */

struct bNodePropertyDrag {
  std::string data_path;
  std::string property_name;
  std::string property_type;
  std::string drag_string;

  bNodeSocket *hovered_socket = nullptr;
  int2 cursor{0, 0};

  SpaceNode *snode = nullptr;
  ARegion *region = nullptr;
  bNode *source_node = nullptr;

  struct Backup {
    std::string rna_path;
    int array_index;
    FCurve *fcurve; /* Original fcurve copy (can be NULL). */
    bool is_ours;   /* True if it was changed in this session. */

    /* Original socket value backup. */
    bNodeSocket *socket = nullptr;
    SocketValueBackup *original_value = nullptr; /* Original value backup (for undo). */
  };
  blender::Vector<Backup> backups;

  Backup *find_backup(const char *rna_path, int array_index)
  {
    for (Backup &b : backups) {
      if (b.array_index == array_index && b.rna_path == rna_path) {
        return &b;
      }
    }
    return nullptr;
  }
};

static bool node_property_send_poll(bContext *C)
{
  bool result = false;
  /* Allow poll to pass if we are in a Node Editor... */
  if (CTX_wm_space_node(C)) {
    result = ED_operator_node_editable(C);
  }
  else {
    /* ...or if we are invoking this from another editor (like Properties)
     * and a Node Editor exists in the window. */
    if (CTX_wm_window(C)) {
      result = true;
    }
  }
  return result;
}

static void node_property_send_exit(bContext *C, wmOperator *op, bool restore)
{
  bNodePropertyDrag *drag = static_cast<bNodePropertyDrag *>(op->customdata);
  if (!drag) {
    return;
  }

  /* Cache snode and edittree pointers while drag is valid. */
  SpaceNode *snode = drag->snode;
  bNodeTree *ntree = (snode) ? snode->edittree : nullptr;
  bool changed = false;

  if (ntree && restore) {
    ID *tree_id = &ntree->id;
    AnimData *adt = BKE_animdata_from_id(tree_id);
    if (adt) {
      for (bNodePropertyDrag::Backup &b : drag->backups) {
        if (!b.is_ours) {
          continue;
        }

        /* Remove current temporary driver. */
        FCurve *current = BKE_fcurve_find(&adt->drivers, b.rna_path.c_str(), b.array_index);
        if (current) {
          BLI_remlink(&adt->drivers, current);
          BKE_fcurve_free(current);
          changed = true;
        }

        /* Restore original driver state from backup. */
        if (b.fcurve) {
          FCurve *restored = BKE_fcurve_copy(b.fcurve);
          if (restored) {
            BLI_addtail(&adt->drivers, restored);
            changed = true;
          }
        }

        /* Restore original socket value. */
        if (b.socket && b.original_value) {
          restore_socket_value(b.socket, b.original_value);
          changed = true;
        }
      }
    }
  }

  if (changed && ntree) {
    Main *bmain = CTX_data_main(C);
    DEG_relations_tag_update(bmain);
    DEG_id_tag_update(&ntree->id, ID_RECALC_ANIMATION);
    WM_event_add_notifier(C, NC_ANIMATION | ND_FCURVES_ORDER, nullptr);
    WM_event_add_notifier(C, NC_NODE | NA_EDITED, nullptr);
  }

  if (snode && snode->runtime) {
    snode->runtime->highlighted_socket_path.clear();
  }

  /* Always cleanup backup copies. */
  for (bNodePropertyDrag::Backup &b : drag->backups) {
    if (b.fcurve) {
      BKE_fcurve_free(b.fcurve);
      b.fcurve = nullptr;
    }
    if (b.original_value) {
      MEM_delete(b.original_value);
      b.original_value = nullptr;
    }
  }

  /* Final cleanup of custom data. */
  op->customdata = nullptr;
  MEM_delete(drag);

  ED_region_tag_redraw(CTX_wm_region(C));
}
static void node_property_send_cancel(bContext *C, wmOperator *op)
{
  node_property_send_exit(C, op, true);
}

static bool node_property_send_find_socket(const bContext &C,
                                           const wmEvent &event,
                                           bNodePropertyDrag &drag,
                                           bNodeSocket **r_socket)
{
  wmWindow *win = CTX_wm_window(&C);
  if (!win) return false;
  
  bScreen *screen = WM_window_get_active_screen(win);
  ScrArea *area = BKE_screen_find_area_xy(screen, SPACE_NODE, event.xy);
  if (!area) {
    return false;
  }
  
  ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event.xy);
  if (!region) {
    return false;
  }

  SpaceNode *snode = (SpaceNode *)area->spacedata.first;
  if (!snode || !snode->edittree) {
    return false;
  }

  int mval[2];
  mval[0] = event.xy[0] - region->winrct.xmin;
  mval[1] = event.xy[1] - region->winrct.ymin;

  float2 cursor;
  ui::view2d_region_to_view(&region->v2d, mval[0], mval[1], &cursor.x, &cursor.y);
  
  drag.region = region;
  drag.snode = snode;
  drag.cursor[0] = mval[0];
  drag.cursor[1] = mval[1];

  bNodeSocket *sock = ed::space_node::node_find_indicated_socket(*snode, *region, cursor, SOCK_IN);
  if (!sock) {
    drag.hovered_socket = nullptr;
    snode->runtime->highlighted_socket_path.clear();
    if (r_socket) {
      *r_socket = nullptr;
    }
    return false;
  }

  drag.hovered_socket = sock;
  snode->runtime->highlighted_socket_path = sock->identifier;
  if (r_socket) {
    *r_socket = sock;
  }
  return true;
}

static void node_property_send_highlight(bNodePropertyDrag &drag, const float2 &cursor)
{
  SpaceNode &snode = *drag.snode;
  ARegion &region = *drag.region;

  bNodeSocket *sock = ed::space_node::node_find_indicated_socket(snode, region, cursor, SOCK_IN);

  if (sock) {
    if (sock != drag.hovered_socket) {
      drag.hovered_socket = sock;
      snode.runtime->highlighted_socket_path = sock->identifier;
      ED_region_tag_redraw(&region);
    }
  }
}

static bool socket_type_compatible(const std::string &prop_type, const eNodeSocketDatatype socket_type)
{
  if (prop_type == "float") {
    return socket_type == SOCK_FLOAT || socket_type == SOCK_INT || socket_type == SOCK_VECTOR ||
           socket_type == SOCK_RGBA;
  }
  if (prop_type == "int") {
    return socket_type == SOCK_FLOAT || socket_type == SOCK_INT;
  }
  if (prop_type == "bool") {
    return socket_type == SOCK_BOOLEAN;
  }
  if (prop_type == "str") {
    return socket_type == SOCK_STRING;
  }
  if (prop_type == "list" || prop_type == "tuple") {
    return socket_type == SOCK_VECTOR || socket_type == SOCK_RGBA || socket_type == SOCK_INT_VECTOR;
  }

  return true;
}

static wmOperatorStatus node_property_send_invoke(bContext *C, wmOperator *op, const wmEvent *event)
{
  SpaceNode *snode = CTX_wm_space_node(C);
  ARegion *region = CTX_wm_region(C);

  char data_path[MAX_NAME];
  char property_name[MAX_IDPROP_NAME];

  RNA_string_get(op->ptr, "data_path", data_path);
  RNA_string_get(op->ptr, "property_name", property_name);

  if (!data_path[0] || !property_name[0]) {
    return OPERATOR_CANCELLED;
  }

  bNode *active_node = snode->edittree ? bke::node_get_active(*snode->edittree) : nullptr;
  if (!active_node) {
    BKE_report(op->reports, RPT_ERROR, "No active node found");
    return OPERATOR_CANCELLED;
  }

  bNodePropertyDrag *drag = MEM_new<bNodePropertyDrag>(__func__);
  drag->data_path = data_path;
  drag->property_name = property_name;
  drag->snode = snode;
  drag->region = region;
  drag->source_node = active_node;
  drag->cursor[0] = event->mval[0];
  drag->cursor[1] = event->mval[1];

  PointerRNA ptr = CTX_data_pointer_get(C, data_path);
  if (ptr.data) {
    PropertyRNA *prop = RNA_struct_find_property(&ptr, property_name);
    if (prop) {
      int prop_type = RNA_property_type(prop);
      if (prop_type == PROP_FLOAT) {
        drag->property_type = "float";
      }
      else if (prop_type == PROP_INT) {
        drag->property_type = "int";
      }
      else if (prop_type == PROP_BOOLEAN) {
        drag->property_type = "bool";
      }
      else if (prop_type == PROP_STRING) {
        drag->property_type = "str";
      }
    }
  }

  op->customdata = drag;

  WM_event_add_modal_handler(C, op);

  ED_workspace_status_text(C, RPT_("Select a node socket to create driver. Press ESC to cancel."));

  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus node_property_send_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  bNodePropertyDrag *drag = static_cast<bNodePropertyDrag *>(op->customdata);
  if (!drag) {
    return OPERATOR_CANCELLED;
  }

  SpaceNode *snode_ctx = CTX_wm_space_node(C);
  ARegion *region_ctx = CTX_wm_region(C);

  if (!snode_ctx || !region_ctx || region_ctx->regiontype != RGN_TYPE_WINDOW) {
    return OPERATOR_RUNNING_MODAL;
  }

  float2 cursor;
  ui::view2d_region_to_view(&region_ctx->v2d, event->mval[0], event->mval[1], &cursor.x, &cursor.y);
  
  if (event->type == MOUSEMOVE) {
    drag->region = region_ctx;
    node_property_send_highlight(*drag, cursor);
    drag->cursor[0] = event->mval[0];
    drag->cursor[1] = event->mval[1];
  }
  else if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    /* Search for socket in the correct region. */
    bNodeSocket *sock = ed::space_node::node_find_indicated_socket(
        *drag->snode, *region_ctx, cursor, SOCK_IN);
    
    if (!sock) {
      sock = drag->hovered_socket;
    }
    
    if (sock) {

      /* Validate type compatibility before creating driver. */
      if (!socket_type_compatible(drag->property_type, (eNodeSocketDatatype)sock->type)) {
        BKE_reportf(op->reports, RPT_ERROR, "Property type '%s' is not compatible with socket type", 
                    drag->property_type.c_str());
        MEM_delete(drag);
        return OPERATOR_CANCELLED;
      }
      bNode &node = sock->owner_node();

      /* Get the source property from the active node. */
      SpaceNode *snode = CTX_wm_space_node(C);
      bNode *active_node = snode ? bke::node_get_active(*snode->edittree) : nullptr;

      if (!active_node) {
        drag->snode->runtime->highlighted_socket_path.clear();
        ED_workspace_status_text(C, nullptr);
        MEM_delete(drag);
        op->customdata = nullptr;
        return OPERATOR_CANCELLED;
      }

      /* Get the IDProperty directly from the node. */
      IDProperty *idprop = active_node->prop ? IDP_GetPropertyFromGroup(active_node->prop, drag->property_name.c_str()) : nullptr;

      /* Use the node tree ID as the driver source (nodes don't have their own ID). */
      ID *tree_id = &snode->edittree->id;
      
      if (idprop && tree_id) {
        int sock_index = BLI_findindex(&node.inputs, sock);
        /* bool is_input = true; */
        
        if (sock_index == -1) {
          drag->snode->runtime->highlighted_socket_path.clear();
          ED_workspace_status_text(C, nullptr);
          BKE_report(op->reports, RPT_ERROR, "Cannot add driver to an output socket");
          MEM_delete(drag);
          op->customdata = nullptr;
          return OPERATOR_CANCELLED;
        }

        char socket_path[FILE_MAX];
        if (BLI_snprintf(socket_path, sizeof(socket_path), "nodes[\"%s\"].inputs[%d].default_value",
                         node.name, sock_index) >= FILE_MAX) {
          drag->snode->runtime->highlighted_socket_path.clear();
          ED_workspace_status_text(C, nullptr);
          BKE_report(op->reports, RPT_ERROR, "Socket path too long");
          MEM_delete(drag);
          op->customdata = nullptr;
          return OPERATOR_CANCELLED;
        }

        ReportList reports;
        BKE_reports_init(&reports, RPT_STORE);

        /* Resolve property to check if it's an array (for vector sockets) */
        PointerRNA id_ptr = RNA_id_pointer_create(tree_id);
        PointerRNA ptr;
        PropertyRNA *prop;
        int array_len = 1;
        if (RNA_path_resolve_property(&id_ptr, socket_path, &ptr, &prop)) {
          if (RNA_property_array_check(prop)) {
            array_len = RNA_property_array_length(&ptr, prop);
          }
        }

        int added_drivers_tot = 0;
        for (int array_idx = 0; array_idx < array_len; array_idx++) {
          int result = ANIM_add_driver(&reports, tree_id, socket_path, array_idx, 0, DRIVER_TYPE_PYTHON);

          if (result > 0) {
            AnimData *adt = BKE_animdata_from_id(tree_id);
            if (adt) {
              FCurve *fcurve = BKE_fcurve_find(&adt->drivers, socket_path, array_idx);
              if (fcurve && fcurve->driver) {
                ChannelDriver *driver = fcurve->driver;
                driver->type = DRIVER_TYPE_PYTHON;

                /* Clear existing variables if any were generated */
                if (driver->variables.first) {
                  DriverVar *dvar, *dvarn;
                  for (dvar = (DriverVar *)driver->variables.first; dvar; dvar = dvarn) {
                    dvarn = dvar->next;
                    driver_free_variable(&driver->variables, dvar);
                  }
                }

                DriverVar *var = driver_add_new_variable(driver);
                if (var) {
                  var->type = DVAR_TYPE_SINGLE_PROP;
                  BLI_strncpy(var->name, "var", sizeof(var->name));

                  DriverTarget *target = &var->targets[0];
                  if (target && drag->source_node) {
                    /* Use NodeTree ID type. */
                    target->idtype = (int)GS(tree_id->name);
                    target->id = tree_id;

                    char idprop_path[FILE_MAX];
                    if (BLI_snprintf(idprop_path, sizeof(idprop_path), "nodes[\"%s\"][\"%s\"]",
                                     drag->source_node->name, drag->property_name.c_str()) >= FILE_MAX) {
                      BKE_report(op->reports, RPT_ERROR, "IDProperty path too long");
                      MEM_delete(drag);
                      op->customdata = nullptr;
                      return OPERATOR_CANCELLED;
                    }
                    
                    target->rna_path = BLI_strdup(idprop_path);
                    var->num_targets = 1;

                    BLI_strncpy(driver->expression, "var", sizeof(driver->expression));
                  }
                }
                added_drivers_tot++;
              }
            }
          }
        }

        if (added_drivers_tot > 0) {
          /* Rebuild depsgraph relations — mandatory when adding new drivers. */
          DEG_relations_tag_update(CTX_data_main(C));
          /* Tag the node tree ID for full re-evaluation since drivers were added. */
          DEG_id_tag_update(tree_id, ID_RECALC_ALL);
          /* Force node editor to redraw socket driver highlights. */
          WM_event_add_notifier(C, NC_ANIMATION | ND_FCURVES_ORDER, nullptr);
          WM_event_add_notifier(C, NC_NODE | NA_EDITED, nullptr);
        }

        drag->snode->runtime->highlighted_socket_path.clear();
        ED_workspace_status_text(C, nullptr);
        MEM_delete(drag);
        op->customdata = nullptr;
        return OPERATOR_FINISHED;
      }
    }
  }
  else if (event->type == RIGHTMOUSE || event->type == EVT_ESCKEY) {
    node_property_send_cancel(C, op);
    ED_workspace_status_text(C, nullptr);
    return OPERATOR_CANCELLED;
  }

  return OPERATOR_RUNNING_MODAL;
}

void ed::space_node::NODE_OT_properties_send(wmOperatorType *ot)
{
  ot->name = "Send Property to Socket";
  ot->idname = "NODE_OT_properties_send";
  ot->description = "Send a custom property to a node socket as a driver";

  ot->exec = nullptr;
  ot->invoke = node_property_send_invoke;
  ot->modal = node_property_send_modal;
  ot->cancel = node_property_send_cancel;
  ot->poll = node_property_send_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  RNA_def_string(ot->srna, "data_path", nullptr, MAX_NAME, "Data Path", "Path to the source data");
  RNA_def_string(ot->srna, "property_name", nullptr, MAX_IDPROP_NAME, "Property Name", "Name of the property");
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Property Drop To Socket Operator (WM Drag & Drop)
 * \brief Drop operator for receiving a custom property drag and creating a driver on a node socket.
 * Registered as a drop box in the Node Editor.
 *
 * Payload string format: "ID_TYPE_SHORT:ID_NAME:PROP_NAME"
 * Examples:
 *   "OB:Cube:mySpeed"       - Object custom property
 *   "MA:Material.001:factor" - Material custom property
 *   "NT:MyNodeTree:value"   - NodeTree custom property
 * \{ */

/** Parse payload string into parts.
 *
 * Two formats are supported:
 *   "ID_TYPE:ID_NAME:PROP_NAME"              - property directly on an ID block
 *   "ID_TYPE:ID_NAME:NODE_NAME:PROP_NAME"    - property on a node inside a NodeTree
 *
 * r_node_name is set to empty string when the property is directly on the ID.
 */
static bool prop_drag_string_parse(const std::string &s,
                                   std::string &r_id_type,
                                   std::string &r_id_name,
                                   std::string &r_node_name,
                                   std::string &r_prop_name)
{
  size_t p1 = s.find(':');
  if (p1 == std::string::npos) {
    return false;
  }
  size_t p2 = s.find(':', p1 + 1);
  if (p2 == std::string::npos) {
    return false;
  }
  r_id_type = s.substr(0, p1);
  r_id_name = s.substr(p1 + 1, p2 - p1 - 1);

  /* Check for optional 4th field (node name). */
  size_t p3 = s.find(':', p2 + 1);
  if (p3 != std::string::npos) {
    r_node_name = s.substr(p2 + 1, p3 - p2 - 1);
    r_prop_name = s.substr(p3 + 1);
  }
  else {
    r_node_name.clear();
    r_prop_name = s.substr(p2 + 1);
  }

  return !r_id_type.empty() && !r_id_name.empty() && !r_prop_name.empty();
}

/** Resolve ID from type short name (e.g. "OB" -> ID_OB) and name. */
static ID *prop_drag_resolve_id(Main *bmain,
                                const std::string &id_type_str,
                                const std::string &id_name)
{
  /* BKE_idtype_idcode_from_name() can assert in debug builds when given invalid input.
   * Since the drag payload can be malformed (or come from older versions), guard it.
   */
  if (id_type_str.empty() || id_type_str.size() > 64) {
    return nullptr;
  }

  /* The drag payload encodes the ID type as a 2-letter code taken directly from id->name[0..1]
   * (e.g. "OB", "MA", "NT").  This is NOT the human-readable type name that
   * BKE_idtype_idcode_from_name() expects ("Object", "NodeTree", …).
   *
   * The correct way to turn a 2-letter code back into a short idcode is the GS() macro,
   * which simply packs the two chars into a short integer — exactly what id->name stores.
   * We then validate the result with BKE_idtype_idcode_is_valid() before using it, which
   * is safe in debug builds (no assert).
   */
  short idcode = 0;
  if (id_type_str.size() == 2 &&
      (id_type_str[0] >= 'A' && id_type_str[0] <= 'Z') &&
      (id_type_str[1] >= 'A' && id_type_str[1] <= 'Z'))
  {
    /* GS() packs two chars into a short — the same encoding id->name uses. */
    const short candidate = GS(id_type_str.c_str());
    if (BKE_idtype_idcode_is_valid(candidate)) {
      idcode = candidate;
    }
  }

  if (idcode == 0) {
    /* Unknown/invalid ID type code. */
  }

  ID *res = BKE_libblock_find_name(bmain, idcode, id_name.c_str());
  if (res) return res;

  /* BKE_libblock_find_name only searches top-level (non-embedded) ID blocks.
   * Embedded NodeTrees (e.g. a Shader NodeTree inside a Material) are stored as
   * ma->nodetree / ob->nodetree / etc. and carry ID_FLAG_EMBEDDED_DATA — they are
   * NOT in bmain->nodetrees.  Search for them by walking every ID in bmain and
   * checking whether it owns an embedded NodeTree with the requested name.
   */
  if (idcode == ID_NT) {
    MainListsArray lbarray = BKE_main_lists_get(*bmain);
    for (ListBase *lb : lbarray) {
      for (ID *owner = static_cast<ID *>(lb->first); owner;
           owner = static_cast<ID *>(owner->next))
      {
        /* node_tree_from_id() returns the embedded nodetree for any ID type that has one. */
        bNodeTree *ntree = bke::node_tree_from_id(owner);
        if (ntree && (ntree->id.name + 2 == id_name)) {
          return &ntree->id;
        }
      }
    }
  }

  return nullptr;
}

static wmOperatorStatus node_prop_drop_to_socket_exec(bContext *C, wmOperator *op)
{
  bNodePropertyDrag *drag_data = static_cast<bNodePropertyDrag *>(op->customdata);
  SpaceNode *snode = (drag_data) ? drag_data->snode : CTX_wm_space_node(C);

  if (!snode) {
    /* Fallback search for Node Editor in current window. */
    wmWindow *win = CTX_wm_window(C);
    bScreen *screen = WM_window_get_active_screen(win);
    for (ScrArea *area = (ScrArea *)screen->areabase.first; area; area = (ScrArea *)area->next) {
      if (area->spacetype == SPACE_NODE) {
        snode = (SpaceNode *)area->spacedata.first;
        if (snode && snode->edittree) {
           break;
        }
      }
    }
  }

  if (!snode || !snode->edittree) {
    BKE_report(op->reports, RPT_ERROR, "No Node Editor found");
    return OPERATOR_CANCELLED;
  }

  /* Read parameters filled by drop box copy function. */
  char drag_string_buf[512];
  char socket_node_name[MAX_NAME];
  char socket_identifier[MAX_NAME];

  RNA_string_get(op->ptr, "drag_string", drag_string_buf);
  RNA_string_get(op->ptr, "socket_node_name", socket_node_name);
  RNA_string_get(op->ptr, "socket_identifier", socket_identifier);

  if (!drag_string_buf[0] || !socket_node_name[0] || !socket_identifier[0]) {
    BKE_report(op->reports, RPT_ERROR, "NODE_OT_prop_drop_to_socket: missing parameters");
    return OPERATOR_CANCELLED;
  }

  std::string id_type_str, id_name_str, node_name_str, prop_name_str;
  if (!prop_drag_string_parse(drag_string_buf, id_type_str, id_name_str, node_name_str, prop_name_str)) {
    BKE_reportf(op->reports,
                RPT_ERROR,
                "NODE_OT_prop_drop_to_socket: invalid drag string '%s'",
                drag_string_buf);
    return OPERATOR_CANCELLED;
  }

  /* Resolve source ID block. */
  Main *bmain = CTX_data_main(C);
  ID *source_id = prop_drag_resolve_id(bmain, id_type_str, id_name_str);
  if (!source_id) {
    BKE_reportf(op->reports,
                RPT_ERROR,
                "NODE_OT_prop_drop_to_socket: cannot find ID '%s' of type '%s'",
                id_name_str.c_str(),
                id_type_str.c_str());
    return OPERATOR_CANCELLED;
  }

  /* Find target node and socket in the edit tree. */
  bNodeTree *ntree = snode->edittree;
  bNode *target_node = nullptr;
  bNodeSocket *target_socket = nullptr;

  for (bNode *node : ntree->all_nodes()) {
    if (STREQ(node->name, socket_node_name)) {
      target_node = node;
      break;
    }
  }
  if (!target_node) {
    BKE_reportf(op->reports, RPT_ERROR, "Cannot find node '%s'", socket_node_name);
    return OPERATOR_CANCELLED;
  }

  for (bNodeSocket *sock = (bNodeSocket *)target_node->inputs.first; sock; sock = sock->next) {
    if (STREQ(sock->identifier, socket_identifier)) {
      target_socket = sock;
      break;
    }
  }
  if (!target_socket) {
    BKE_reportf(op->reports,
                RPT_ERROR,
                "Cannot find input socket '%s' on node '%s'",
                socket_identifier,
                socket_node_name);
    return OPERATOR_CANCELLED;
  }

  /* Build RNA path for the socket default_value on the node tree. */
  int sock_index = BLI_findindex(&target_node->inputs, target_socket);
  char socket_rna_path[FILE_MAX];
  if (BLI_snprintf(socket_rna_path,
                   sizeof(socket_rna_path),
                   "nodes[\"%s\"].inputs[%d].default_value",
                   target_node->name,
                   sock_index) >= FILE_MAX) {
    BKE_report(op->reports, RPT_ERROR, "Socket RNA path too long");
    return OPERATOR_CANCELLED;
  }
  ID *tree_id = &ntree->id;

  /* Determine array length (vector sockets etc). */
  int array_len = 1;
  {
    PointerRNA id_ptr = RNA_id_pointer_create(tree_id);
    PointerRNA ptr;
    PropertyRNA *prop;
    if (RNA_path_resolve_property(&id_ptr, socket_rna_path, &ptr, &prop)) {
      if (RNA_property_array_check(prop)) {
        array_len = RNA_property_array_length(&ptr, prop);
      }
    }
  }

  /* Build source RNA path for driver variable.
   * - Property on a node:  nodes["NodeName"]["prop"]
   * - Property on an ID:   ["prop"]
   */
  char source_rna_path[FILE_MAX];
  if (!node_name_str.empty()) {
    if (BLI_snprintf(source_rna_path, sizeof(source_rna_path),
                     "nodes[\"%s\"][\"%s\"]",
                     node_name_str.c_str(), prop_name_str.c_str()) >= FILE_MAX) {
      BKE_report(op->reports, RPT_ERROR, "Source RNA path too long");
      return OPERATOR_CANCELLED;
    }
  }
  else {
    BLI_snprintf(source_rna_path, sizeof(source_rna_path), "[\"%s\"]", prop_name_str.c_str());
  }

  /* Create drivers for each component. */
  ReportList reports;
  BKE_reports_init(&reports, RPT_STORE);

  bool toggle = RNA_boolean_get(op->ptr, "toggle");
  bool do_restore = false;

  if (toggle && drag_data) {
    /* If the first component of this socket was already modified by us, toggle them all back. */
    bNodePropertyDrag::Backup *b0 = drag_data->find_backup(socket_rna_path, 0);
    if (b0 && b0->is_ours) {
      do_restore = true;
    }
  }

  int added_drivers_tot = 0;

  for (int array_idx = 0; array_idx < array_len; array_idx++) {
    AnimData *adt = BKE_animdata_from_id(tree_id);

    if (drag_data) {
      bNodePropertyDrag::Backup *b = drag_data->find_backup(socket_rna_path, array_idx);
      if (!b) {
        /* First time seeing this socket component: create backup. */
        bNodePropertyDrag::Backup new_b;
        new_b.rna_path = socket_rna_path;
        new_b.array_index = array_idx;
        new_b.fcurve = nullptr;
        new_b.is_ours = false;
        new_b.socket = nullptr;
        new_b.original_value = nullptr;

        /* Save socket reference and original value only for first component. */
        if (array_idx == 0 && target_socket) {
          new_b.socket = target_socket;
          new_b.original_value = socket_value_to_backup(target_socket);
        }

        if (adt) {
          FCurve *existing = BKE_fcurve_find(&adt->drivers, socket_rna_path, array_idx);
          if (existing) {
            new_b.fcurve = BKE_fcurve_copy(existing);
          }
        }
        drag_data->backups.append(new_b);
        b = &drag_data->backups.last();
      }

      if (do_restore) {
        /* Remove our driver. */
        if (adt) {
          FCurve *current = BKE_fcurve_find(&adt->drivers, socket_rna_path, array_idx);
          if (current) {
            BLI_remlink(&adt->drivers, current);
            BKE_fcurve_free(current);
          }
          /* Restore original if it existed. */
          if (b->fcurve) {
            FCurve *restored = BKE_fcurve_copy(b->fcurve);
            BLI_addtail(&adt->drivers, restored);
          }
        }

        /* Restore original socket value. */
        if (array_idx == 0 && b->socket && b->original_value) {
          restore_socket_value(b->socket, b->original_value);
        }

        b->is_ours = false;
        added_drivers_tot++; /* Count as success. */
        continue;
      }
      
      b->is_ours = true;
    }

    /* Remove existing driver if it exists (either original or from previous click). */
    if (adt) {
      FCurve *existing_fcurve = BKE_fcurve_find(&adt->drivers, socket_rna_path, array_idx);
      if (existing_fcurve) {
        BLI_remlink(&adt->drivers, existing_fcurve);
        BKE_fcurve_free(existing_fcurve);
      }
    }

    int result = ANIM_add_driver(&reports, tree_id, socket_rna_path, array_idx, 0, DRIVER_TYPE_PYTHON);
    if (result <= 0) {
      continue;
    }

    /* Important: Fetch AnimData *after* driver creation to ensure we pick it up 
     * even if it was just created for the very first time. */
    adt = BKE_animdata_from_id(tree_id);
    if (!adt) {
      continue;
    }

    FCurve *fcurve = BKE_fcurve_find(&adt->drivers, socket_rna_path, array_idx);
    if (!fcurve || !fcurve->driver) {
      continue;
    }

    ChannelDriver *driver = fcurve->driver;
    driver->type = DRIVER_TYPE_PYTHON;

    /* Clear auto-generated variables. */
    for (DriverVar *dvar = (DriverVar *)driver->variables.first, *next; dvar; dvar = next) {
      next = dvar->next;
      driver_free_variable(&driver->variables, dvar);
    }

    /* Add a SINGLE_PROP variable pointing to the source custom property. */
    DriverVar *var = driver_add_new_variable(driver);
    if (var) {
      var->type = DVAR_TYPE_SINGLE_PROP;
      BLI_strncpy(var->name, "var", sizeof(var->name));
      var->num_targets = 1;

      DriverTarget *target = &var->targets[0];
      target->idtype = GS(source_id->name);
      target->id = source_id;
      target->rna_path = BLI_strdup(source_rna_path);

      BLI_strncpy(driver->expression, "var", sizeof(driver->expression));
      added_drivers_tot++;
    }
  }

  BKE_reports_free(&reports);

  if (added_drivers_tot == 0) {
    BKE_report(op->reports, RPT_ERROR, "Failed to create driver(s)");
    return OPERATOR_CANCELLED;
  }

  /* Update. */
  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(tree_id, ID_RECALC_ALL);
  WM_event_add_notifier(C, NC_ANIMATION | ND_FCURVES_ORDER, nullptr);
  WM_event_add_notifier(C, NC_NODE | NA_EDITED, nullptr);

  return OPERATOR_FINISHED;
}

static wmOperatorStatus node_prop_drop_to_socket_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  char socket_identifier[MAX_NAME];
  RNA_string_get(op->ptr, "socket_identifier", socket_identifier);

  /* If socket is already specified, just execute. */
  if (socket_identifier[0] != '\0') {
    return op->type->exec(C, op);
  }

  char drag_string_buf[FILE_MAX];
  RNA_string_get(op->ptr, "drag_string", drag_string_buf);
  if (drag_string_buf[0] == '\0') {
    return OPERATOR_CANCELLED;
  }

  bNodePropertyDrag *drag = MEM_new<bNodePropertyDrag>(__func__);
  drag->drag_string = drag_string_buf;

  op->customdata = drag;
  WM_event_add_modal_handler(C, op);
  WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_EYEDROPPER);
  {
    WorkspaceStatus status(C);
    status.item(IFACE_("Assign Driver"), ICON_MOUSE_LMB);
    status.item(IFACE_("Multiple Select"), ICON_EVENT_SHIFT, ICON_MOUSE_LMB);
    status.item(IFACE_("Finish"), ICON_EVENT_RETURN);
    status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
  }
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus node_prop_drop_to_socket_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  bNodePropertyDrag *drag = static_cast<bNodePropertyDrag *>(op->customdata);
  if (!drag) {
    return OPERATOR_CANCELLED;
  }

  if (event->type == MOUSEMOVE) {
    bNodeSocket *sock = nullptr;
    bool found_socket = node_property_send_find_socket(*C, *event, *drag, &sock);

    /* Helper lambda to set the normal "assign driver" status. */
    auto set_normal_status = [&]() {
      WM_cursor_modal_set(CTX_wm_window(C), WM_CURSOR_EYEDROPPER);
      WorkspaceStatus status(C);
      status.item(IFACE_("Assign Driver"), ICON_MOUSE_LMB);
      status.item(IFACE_("Multiple"), ICON_EVENT_SHIFT, ICON_MOUSE_LMB);
      status.item(IFACE_("Finish"), ICON_EVENT_RETURN);
      status.item(IFACE_("Cancel"), ICON_EVENT_ESC);
    };

    /* Determine what's under the cursor and update cursor/status accordingly. */
    wmWindow *win = CTX_wm_window(C);
    bScreen *screen = WM_window_get_active_screen(win);
    ScrArea *hover_area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event->xy);

    if (found_socket && sock) {
      set_normal_status();
    }
    else if (hover_area && hover_area->spacetype != SPACE_NODE) {
      /* Outside Node Editor — check for animatable button. */
      ARegion *hover_region = nullptr;
      for (ARegion *ar = static_cast<ARegion *>(hover_area->regionbase.first); ar; ar = ar->next) {
        if (BLI_rcti_isect_pt_v(&ar->winrct, event->xy)) {
          hover_region = ar;
          break;
        }
      }
      if (hover_region) {
        ui::Button *hover_but = ui::but_find_mouse_over(hover_region, event);
        if (hover_but && hover_but->rnaprop) {
          bool anim = RNA_property_animateable(&hover_but->rnapoin, hover_but->rnaprop);
          PropertyType ptype = RNA_property_type(hover_but->rnaprop);
          bool valid_type = (ptype == PROP_FLOAT || ptype == PROP_INT || ptype == PROP_BOOLEAN);
          if (anim && valid_type) {
            set_normal_status();
          }
          else {
            /* Field doesn't support drivers — stop cursor + warning in status. */
            WM_cursor_modal_set(win, WM_CURSOR_STOP);
            WorkspaceStatus status(C);
            status.item(IFACE_("This field does not support drivers"), ICON_NONE);
          }
        }
        else {
          set_normal_status();
        }
      }
    }
    else {
      set_normal_status();
    }

    if (found_socket) {
      ED_region_tag_redraw(CTX_wm_region(C));
    }
    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == LEFTMOUSE && event->val == KM_PRESS) {
    /* First try: find a node socket in Node Editor. */
    bNodeSocket *sock = nullptr;
    if (node_property_send_find_socket(*C, *event, *drag, &sock) && sock) {

      SpaceNode *snode = drag->snode;
      if (!snode || !snode->edittree) {
        BKE_report(op->reports, RPT_ERROR, "No Node Editor found");
        return OPERATOR_CANCELLED;
      }

      bNode &node = sock->owner_node();
      char socket_node_name[MAX_NAME];
      char socket_identifier[MAX_NAME];
      BLI_strncpy(socket_node_name, node.name, sizeof(socket_node_name));
      BLI_strncpy(socket_identifier, sock->identifier, sizeof(socket_identifier));

      RNA_string_set(op->ptr, "socket_node_name", socket_node_name);
      RNA_string_set(op->ptr, "socket_identifier", socket_identifier);

      RNA_boolean_set(op->ptr, "toggle", (event->modifier & KM_SHIFT) != 0);
      const wmOperatorStatus exec_status = op->type->exec(C, op);
      (void)exec_status;

      if (!(event->modifier & KM_SHIFT)) {
        node_property_send_exit(C, op, false);
        WM_cursor_modal_restore(CTX_wm_window(C));
        ED_workspace_status_text(C, nullptr);
        return OPERATOR_FINISHED;
      }
      return OPERATOR_RUNNING_MODAL;
    }

    /* Second try: find an animatable button in any editor under the cursor
     * (Properties Editor, 3D Viewport N-Panel, Timeline, etc.)
     * Skip Node Editor — it's handled by the socket search above. */
    {
      wmWindow *win = CTX_wm_window(C);
      if (win) {
        bScreen *screen = WM_window_get_active_screen(win);
        ScrArea *area = BKE_screen_find_area_xy(screen, SPACE_TYPE_ANY, event->xy);

        if (area && area->spacetype != SPACE_NODE) {
          /* Search all regions under cursor. */
          ARegion *region = nullptr;
          for (ARegion *ar = static_cast<ARegion *>(area->regionbase.first); ar; ar = ar->next) {
            if (BLI_rcti_isect_pt_v(&ar->winrct, event->xy)) {
              region = ar;
              break;
            }
          }

          if (region) {
            ui::Button *but = ui::but_find_mouse_over(region, event);
            if (but && but->rnaprop) {
              bool animateable = RNA_property_animateable(&but->rnapoin, but->rnaprop);
              PropertyType prop_type = RNA_property_type(but->rnaprop);

              if (animateable &&
                  (prop_type == PROP_FLOAT || prop_type == PROP_INT || prop_type == PROP_BOOLEAN))
              {
                /* Build full RNA path from owner_id to the property.
                 * This handles nested structs like key_blocks["Basis"].value,
                 * modifier properties, constraint properties, etc. */
                std::optional<std::string> full_path = RNA_path_from_ID_to_property(
                    &but->rnapoin, but->rnaprop);
                if (!full_path) {
                  return OPERATOR_RUNNING_MODAL;
                }

                wmOperatorType *field_ot = WM_operatortype_find("OBJECT_OT_prop_drop_to_field",
                                                                false);
                if (field_ot) {
                  PointerRNA field_props = WM_operator_properties_create_ptr(field_ot);

                  char drag_str_buf[FILE_MAX];
                  RNA_string_get(op->ptr, "drag_string", drag_str_buf);
                  RNA_string_set(&field_props, "drag_string", drag_str_buf);

                  /* Use full RNA path, not just property identifier. */
                  RNA_string_set(&field_props, "target_property", full_path->c_str());

                  int array_idx = -1;
                  if (RNA_property_array_check(but->rnaprop)) {
                    array_idx = but->rnaindex;
                  }
                  RNA_int_set(&field_props, "target_array_index", array_idx);

                  ID *target_id = but->rnapoin.owner_id;
                  if (target_id) {
                    RNA_string_set(&field_props, "target_id_name", target_id->name + 2);
                    char id_type[3] = {target_id->name[0], target_id->name[1], '\0'};
                    RNA_string_set(&field_props, "target_id_type", id_type);
                  }

                  WM_operator_name_call_ptr(
                      C, field_ot, wm::OpCallContext::ExecDefault, &field_props, nullptr);
                  WM_operator_properties_free(&field_props);

                  if (!(event->modifier & KM_SHIFT)) {
                    node_property_send_exit(C, op, false);
                    WM_cursor_modal_restore(CTX_wm_window(C));
                    ED_workspace_status_text(C, nullptr);
                    return OPERATOR_FINISHED;
                  }
                  return OPERATOR_RUNNING_MODAL;
                }
              }
              else {
                /* Field found but doesn't support drivers — show warning report. */
                BKE_report(CTX_wm_reports(C),
                           RPT_WARNING,
                           "This field does not support drivers");
                WM_event_add_notifier(C, NC_SPACE | ND_SPACE_INFO_REPORT, nullptr);
                return OPERATOR_RUNNING_MODAL;
              }
            }
          }
        }
        else if (area && area->spacetype == SPACE_NODE) {
          /* Node Editor: also check for animatable buttons on nodes
           * (e.g. node value fields without sockets). */
          ARegion *region = BKE_area_find_region_xy(area, RGN_TYPE_WINDOW, event->xy);
          if (region) {
            ui::Button *but = ui::but_find_mouse_over(region, event);
            if (but && but->rnaprop) {
              bool animateable = RNA_property_animateable(&but->rnapoin, but->rnaprop);
              PropertyType prop_type = RNA_property_type(but->rnaprop);
              if (animateable &&
                  (prop_type == PROP_FLOAT || prop_type == PROP_INT || prop_type == PROP_BOOLEAN))
              {
                std::optional<std::string> full_path = RNA_path_from_ID_to_property(
                    &but->rnapoin, but->rnaprop);
                if (full_path) {
                  wmOperatorType *field_ot = WM_operatortype_find(
                      "OBJECT_OT_prop_drop_to_field", false);
                  if (field_ot) {
                    PointerRNA field_props = WM_operator_properties_create_ptr(field_ot);
                    char drag_str_buf[FILE_MAX];
                    RNA_string_get(op->ptr, "drag_string", drag_str_buf);
                    RNA_string_set(&field_props, "drag_string", drag_str_buf);
                    RNA_string_set(&field_props, "target_property", full_path->c_str());
                    int array_idx = -1;
                    if (RNA_property_array_check(but->rnaprop)) {
                      array_idx = but->rnaindex;
                    }
                    RNA_int_set(&field_props, "target_array_index", array_idx);
                    ID *target_id = but->rnapoin.owner_id;
                    if (target_id) {
                      RNA_string_set(&field_props, "target_id_name", target_id->name + 2);
                      char id_type[3] = {target_id->name[0], target_id->name[1], '\0'};
                      RNA_string_set(&field_props, "target_id_type", id_type);
                    }
                    WM_operator_name_call_ptr(
                        C, field_ot, wm::OpCallContext::ExecDefault, &field_props, nullptr);
                    WM_operator_properties_free(&field_props);
                    if (!(event->modifier & KM_SHIFT)) {
                      node_property_send_exit(C, op, false);
                      WM_cursor_modal_restore(CTX_wm_window(C));
                      ED_workspace_status_text(C, nullptr);
                      return OPERATOR_FINISHED;
                    }
                    return OPERATOR_RUNNING_MODAL;
                  }
                }
              }
              else {
                BKE_report(CTX_wm_reports(C),
                           RPT_WARNING,
                           "This field does not support drivers");
                WM_event_add_notifier(C, NC_SPACE | ND_SPACE_INFO_REPORT, nullptr);
                return OPERATOR_RUNNING_MODAL;
              }
            }
          }
        }
      }
    }

    return OPERATOR_RUNNING_MODAL;
  }

  if (event->type == EVT_ESCKEY || event->type == RIGHTMOUSE) {
    node_property_send_cancel(C, op);
    WM_cursor_modal_restore(CTX_wm_window(C));
    ED_workspace_status_text(C, nullptr);
    return OPERATOR_FINISHED;
  }
  
  if (event->type == EVT_RETKEY) {
    node_property_send_exit(C, op, false);
    WM_cursor_modal_restore(CTX_wm_window(C));
    ED_workspace_status_text(C, nullptr);
    return OPERATOR_FINISHED;
  }

  return OPERATOR_RUNNING_MODAL;
}

void ed::space_node::NODE_OT_prop_drop_to_socket(wmOperatorType *ot)
{
  ot->name = "Drop Property to Socket";
  ot->idname = "NODE_OT_prop_drop_to_socket";
  ot->description = "Select a node socket to create a driver from a property";

  ot->exec = node_prop_drop_to_socket_exec;
  ot->invoke = node_prop_drop_to_socket_invoke;
  ot->modal = node_prop_drop_to_socket_modal;
  ot->cancel = node_property_send_cancel;
  ot->poll = node_property_send_poll;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;

  RNA_def_boolean(ot->srna, "toggle", false, "Toggle", "Invert assignment if already exists");
  RNA_def_string(ot->srna, "drag_string", nullptr, 512,
                 "Drag String", "Encoded source property: 'ID_TYPE:ID_NAME:PROP_NAME'");
  RNA_def_string(ot->srna, "socket_node_name", nullptr, MAX_NAME,
                 "Node Name", "Name of the target node");
  RNA_def_string(ot->srna, "socket_identifier", nullptr, MAX_NAME,
                 "Socket Identifier", "Identifier of the target input socket");
}

/** \} */

}  // namespace blender
