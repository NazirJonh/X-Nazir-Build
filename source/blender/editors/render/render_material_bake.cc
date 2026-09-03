/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edrend
 *
 * Renders a source #Material's Principled BSDF inputs into flat buffers for PBR Paint.
 *
 * The mechanism is the one the shader node previews use (see `node_shader_preview.cc`): route the
 * wanted socket up to the material's root node tree, attach a #ShaderNodeOutputAOV, declare a
 * matching #ViewLayerAOV and render. That path is what already evaluates arbitrary sockets of
 * arbitrary node graphs today, so a material that previews also bakes.
 *
 * Two things differ from the node previews, both deliberate:
 * - The domain is a quad built here rather than the flat object from `preview.blend`, because a
 *   paint source needs UV [0, 1]^2 to land exactly on the rendered frame and the preview scene
 *   promises no such mapping.
 * - The scene therefore lives in a temporary #Main of its own instead of `G.pr_main`, which also
 *   keeps a bake from colliding with an icon or node preview over that shared database.
 *
 * The render itself knows nothing about PBR Paint: #bake_requests_attach and #bake_requests_render
 * take a list of #BakeSocketRequest -- a socket, a name, and how to deliver it -- and hand back one
 * buffer each. Everything that decides which socket carries which paint channel lives in the "PBR
 * Paint Channel Requests" section, and a second consumer of the bake would replace only that.
 */

#include "ED_material_bake.hh"

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "MEM_guardedalloc.h"

#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"
#include "BKE_paint.hh"
#include "BKE_scene.hh"

#include "BLI_assert.h"
#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_camera_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "RE_engine.h"
#include "RE_pipeline.h"

#include "WM_api.hh"
#include "WM_types.hh"

/**
 * Diagnostic tracing of the bake, one line per stage boundary, so a single run shows which stage
 * failed. Set to 1 for a debugging session, then back to 0.
 *
 * The consuming side has a switch of its own, #PBR_PAINT_BAKE_DEBUG in
 * `editors/sculpt_paint/mesh/paint_debug.hh`: that is a different module and this file cannot
 * reach its header, so the two halves are enabled independently.
 */
#define PBR_MATERIAL_BAKE_DEBUG 0
#if PBR_MATERIAL_BAKE_DEBUG
#  include <cstdio>
#  define PBR_BAKE_LOG(...) \
    do { \
      printf("[PBR-BAKE] " __VA_ARGS__); \
      fflush(stdout); \
    } while (0)
#else
#  define PBR_BAKE_LOG(...) ((void)0)
#endif

namespace blender::ed::material_bake {

/* -------------------------------------------------------------------- */
/** \name Baked Result
 * \{ */

MaterialSourceBake::MaterialSourceBake(const MaterialSourceResolve &resolve,
                                       std::array<ImBuf *, PAINT_MATERIAL_CHANNEL_NUM> images)
    : resolve_(resolve), images_(images)
{
}

MaterialSourceBake::~MaterialSourceBake()
{
  for (ImBuf *ibuf : images_) {
    if (ibuf != nullptr) {
      IMB_freeImBuf(ibuf);
    }
  }
}

ChannelResolution MaterialSourceBake::resolution(const eMaterialPaintChannel channel) const
{
  return resolve_.channels[channel];
}

ChannelUnavailableReason MaterialSourceBake::unavailable_reason(
    const eMaterialPaintChannel channel) const
{
  return resolve_.reasons[channel];
}

const ImBuf *MaterialSourceBake::channel_image(const eMaterialPaintChannel channel) const
{
  return images_[channel];
}

float4 MaterialSourceBake::channel_constant(const eMaterialPaintChannel channel) const
{
  return resolve_.constants[channel];
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bake Cache
 *
 * Keyed by the material's #ID.session_uid plus a hash of the refresh state of every node tree the
 * material reaches. A freed material's address can be handed straight back to a new one, so a
 * cache keyed on pointers would serve the old bake for the new material; session UIDs are never
 * recycled within a session.
 *
 * The hash has to cover nested group trees, not just #Material.nodetree: a value edited inside a
 * group definition only bumps that group's own refresh state, and keying on the root tree alone
 * would then keep serving a bake of the material as it was before the edit.
 * \{ */

struct BakeCacheKey {
  uint32_t material_session_uid = 0;
  uint64_t node_tree_state_hash = 0;
  int resolution = 0;

  uint64_t hash() const
  {
    return get_default_hash(
        this->material_session_uid, this->node_tree_state_hash, this->resolution);
  }

  friend bool operator==(const BakeCacheKey &a, const BakeCacheKey &b)
  {
    return a.material_session_uid == b.material_session_uid &&
           a.node_tree_state_hash == b.node_tree_state_hash && a.resolution == b.resolution;
  }
};

/** Combine \a tree's refresh state and that of every group tree it reaches, once each. */
static void node_tree_state_hash_recursive(const bNodeTree &tree,
                                           Set<const bNodeTree *> &visited_trees,
                                           uint64_t &r_hash)
{
  if (!visited_trees.add(&tree)) {
    return;
  }
  r_hash = get_default_hash(r_hash,
                            tree.runtime->previews_refresh_state,
                            tree.runtime->output_topology_hash);
  for (const bNode *node : tree.all_nodes()) {
    if (!node->is_group() || node->id == nullptr) {
      continue;
    }
    if (const bNodeTree *group_tree = id_cast<const bNodeTree *>(node->id)) {
      node_tree_state_hash_recursive(*group_tree, visited_trees, r_hash);
    }
  }
}

static BakeCacheKey bake_cache_key(const Material &ma, const int resolution)
{
  BakeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.resolution = resolution;
  if (ma.nodetree != nullptr) {
    ma.nodetree->ensure_topology_cache();
    Set<const bNodeTree *> visited_trees;
    node_tree_state_hash_recursive(*ma.nodetree, visited_trees, key.node_tree_state_hash);
  }
  return key;
}

/** One cached bake, plus what it takes to know when it has gone out of date. */
struct BakeCacheEntry {
  std::shared_ptr<const MaterialSourceBake> bake;
  /**
   * The #Image data-blocks the bake read, by session UID.
   *
   * Their pixels are not part of #BakeCacheKey and cannot be: an image carries no content version
   * to hash, and painting one would have to be noticed anyway. So the dependency is recorded here
   * and #material_source_bake_tag_image_changed marks the entry stale instead.
   */
  Vector<uint32_t> image_session_uids;
  /** The bake no longer matches its inputs, but is still served until a fresh one lands. */
  bool stale = false;
  /** Total bytes of the buffers, for the budget below. */
  int64_t byte_size = 0;
  /** Bumped on every hit, so the budget can evict what has gone unused the longest. */
  uint64_t last_used_serial = 0;
};

/**
 * Guards every container below. The worker thread writes results while the paint path and the UI
 * read them, so none of them may be touched without it.
 */
static std::mutex g_bake_cache_mutex;
static Map<BakeCacheKey, BakeCacheEntry> g_bake_cache;
/** Keys whose job has been started and has not stored a result yet. */
static Set<BakeCacheKey> g_bake_pending;
static uint64_t g_bake_use_serial = 0;

/**
 * How much the cache may hold before the least recently used entries are dropped.
 *
 * A bound is needed rather than nice to have: nothing tells this module that a material was
 * deleted, and session UIDs are never reused, so an entry for a material the user removed is
 * unreachable and would otherwise sit there for the rest of the session. At the larger bake sizes
 * a single material's buffers run to tens of megabytes, so a count-based limit would not describe
 * the cost. A stroke holding an evicted bake keeps it alive through its own `shared_ptr`.
 */
static constexpr int64_t BAKE_CACHE_BYTE_BUDGET = 512 * 1024 * 1024;

/** Drop least recently used entries until the cache is inside #BAKE_CACHE_BYTE_BUDGET. */
static void bake_cache_trim_to_budget()
{
  int64_t total = 0;
  for (const auto item : g_bake_cache.items()) {
    total += item.value.byte_size;
  }
  while (total > BAKE_CACHE_BYTE_BUDGET && !g_bake_cache.is_empty()) {
    const BakeCacheKey *oldest_key = nullptr;
    uint64_t oldest_serial = UINT64_MAX;
    int64_t oldest_size = 0;
    for (const auto item : g_bake_cache.items()) {
      if (item.value.last_used_serial < oldest_serial) {
        oldest_serial = item.value.last_used_serial;
        oldest_key = &item.key;
        oldest_size = item.value.byte_size;
      }
    }
    if (oldest_key == nullptr) {
      break;
    }
    const BakeCacheKey key_copy = *oldest_key;
    g_bake_cache.remove(key_copy);
    total -= oldest_size;
  }
}

std::shared_ptr<const MaterialSourceBake> material_source_bake_get(const Material &ma,
                                                                   const int resolution)
{
  const BakeCacheKey key = bake_cache_key(ma, resolution);
  std::lock_guard lock(g_bake_cache_mutex);
  if (BakeCacheEntry *cached = g_bake_cache.lookup_ptr(key)) {
    cached->last_used_serial = ++g_bake_use_serial;
    return cached->bake;
  }
  /* The exact lookup misses for a whole class of ordinary actions -- changing Bake Size, editing
   * the material, selecting a different one -- for as long as the new bake is rendering, which is
   * seconds. Returning nothing there is not a soft failure: #ChannelSourceSet decides at stroke
   * start, once, that every baked channel is unusable, so the whole stroke silently paints nothing
   * for those channels. That is what "switching the source material does nothing" looks like.
   *
   * So serve any bake this material has, whatever its resolution or node-tree state. It is by
   * construction a bake of this same material, just an older revision of it; the exact one takes
   * over as soon as it lands, and the entries it supersedes are dropped then. Slightly stale
   * pixels for a moment beat a stroke that does nothing. */
  for (auto item : g_bake_cache.items()) {
    if (item.key.material_session_uid == key.material_session_uid) {
      PBR_BAKE_LOG("get: res=%d not cached yet, serving res=%d\n", resolution, item.key.resolution);
      item.value.last_used_serial = ++g_bake_use_serial;
      return item.value.bake;
    }
  }
  return nullptr;
}

void material_source_bake_tag_image_changed(const Image &image)
{
  const uint32_t session_uid = image.id.session_uid;
  std::lock_guard lock(g_bake_cache_mutex);
  for (auto item : g_bake_cache.items()) {
    if (item.value.image_session_uids.contains(session_uid)) {
      /* Marked rather than dropped. Removing it would leave #material_source_bake_get with nothing
       * to serve, and a stroke started before the replacement lands would paint nothing at all for
       * every baked channel. Stale pixels for a second or two are the lesser wrong. */
      item.value.stale = true;
    }
  }
}

void material_source_bake_invalidate(const Material *ma)
{
  std::lock_guard lock(g_bake_cache_mutex);
  if (ma == nullptr) {
    g_bake_cache.clear();
    g_bake_pending.clear();
    return;
  }
  const uint32_t session_uid = ma->id.session_uid;
  g_bake_cache.remove_if([&](auto item) { return item.key.material_session_uid == session_uid; });
  g_bake_pending.remove_if([&](const BakeCacheKey &key) {
    return key.material_session_uid == session_uid;
  });
}

bool material_source_bake_cache_contains(const Material &ma, const int resolution)
{
  const BakeCacheKey key = bake_cache_key(ma, resolution);
  std::lock_guard lock(g_bake_cache_mutex);
  return g_bake_cache.contains(key);
}

bool material_source_preview_get(const BrushMaterialPaint &brush_paint,
                                 const PaintModeSettings &mode_settings,
                                 const int visible_material_channels,
                                 MaterialSourcePreview &r_preview,
                                 std::shared_ptr<const MaterialSourceBake> &r_bake)
{
  /* Reset field by field: #MTex deletes its copy assignment (#DNA_DEFINE_CXX_METHODS), so the
   * struct as a whole cannot be assigned from a temporary. */
  r_preview.ibuf = nullptr;
  r_preview.constant = float4(0.0f);
  r_preview.usable = false;
  r_preview.mtex = dna::shallow_copy(MTex());

  if (brush_paint.source_material == nullptr) {
    return false;
  }
  r_bake = material_source_bake_get(*brush_paint.source_material, brush_paint.source_bake_size);
  if (r_bake == nullptr) {
    /* The bake job is still running; the caller picks the result up on a later redraw. */
    return false;
  }

  for (const eMaterialPaintChannel channel : BKE_paint_material_channel_preview_order()) {
    if (!BKE_paint_material_channel_is_enabled(
            brush_paint, mode_settings, visible_material_channels, channel))
    {
      continue;
    }
    switch (r_bake->resolution(channel)) {
      case ChannelResolution::Baked: {
        const ImBuf *ibuf = r_bake->channel_image(channel);
        /* Samplers stride a float buffer by four, the shape every baked buffer is normalized to;
         * anything else would be read out of bounds. */
        if (ibuf == nullptr || ibuf->float_data() == nullptr || ibuf->channels != 4) {
          continue;
        }
        r_preview.ibuf = ibuf;
        break;
      }
      case ChannelResolution::Constant: {
        r_preview.constant = r_bake->channel_constant(channel);
        break;
      }
      default:
        continue;
    }
    r_preview.usable = true;
    BKE_paint_material_channel_effective_mtex(
        brush_paint, brush_paint.channels[channel], r_preview.mtex);
    /* The pixels come from the bake, so the channel's own #Tex must not be sampled on top. */
    r_preview.mtex.tex = nullptr;
    return true;
  }

  r_bake.reset();
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Socket Routing
 *
 * Ported from #connect_nested_node_to_node in `node_shader_preview.cc`, which does the same job
 * for node previews. The node editor knows the path to the edited tree from its #bNodeTreePath
 * stack; a bake is told the path by the resolver that found the socket in the first place.
 * \{ */

/**
 * Link \a source_socket, which may live in a group nested inside \a root_tree, to \a dest_socket
 * on \a dest_node in \a root_tree, adding a group output named \a route_name at each level.
 *
 * An AOV only counts when it sits in the material's own tree, so the signal has to be routed out
 * rather than tapped where it is.
 *
 * \param group_path: the group instance nodes from \a root_tree down to the socket's own tree,
 *                    outermost first, as #BKE_paint_material_source_socket reported them. It has
 *                    to come from there and cannot be searched for here: a group definition can be
 *                    instanced more than once, every instance shares the tree the socket lives in,
 *                    and only the resolver knows which one it actually descended through. Routing
 *                    out through any other instance bakes that instance's inputs instead, which
 *                    silently produces a buffer for a material the user is not looking at.
 *
 * \return false when the path does not lead where it claims.
 */
static bool route_socket_to_root(Main &bmain,
                                 bNodeTree &root_tree,
                                 const bNodeSocket &source_socket,
                                 const Span<const bNode *> group_path,
                                 bNode &dest_node,
                                 bNodeSocket &dest_socket,
                                 const StringRefNull route_name)
{
  bNode *current_node = const_cast<bNode *>(&source_socket.owner_node());
  bNodeSocket *current_socket = const_cast<bNodeSocket *>(&source_socket);

  /* Walk outwards: the innermost group is the last entry of the path. */
  for (int group_index = group_path.size() - 1; group_index >= 0; group_index--) {
    bNode &group_node = *const_cast<bNode *>(group_path[group_index]);
    bNodeTree *group_tree = id_cast<bNodeTree *>(group_node.id);
    if (group_tree == nullptr) {
      return false;
    }
    group_tree->ensure_topology_cache();
    /* Checked rather than assumed. A path that does not describe the actual nesting would be
     * linked up regardless, producing links that cross trees; #node_add_link only asserts on
     * that, so a release build would go on to bake from a silently malformed graph. */
    if (!group_tree->all_nodes().contains(current_node)) {
      PBR_BAKE_LOG("route: '%s' path level %d does not contain the current node\n",
                   route_name.c_str(),
                   group_index);
      return false;
    }
    bNode *group_output = group_tree->group_output_node();
    if (group_output == nullptr) {
      group_output = bke::node_add_static_node(nullptr, *group_tree, NODE_GROUP_OUTPUT);
      group_output->flag |= NODE_DO_OUTPUT;
    }

    group_tree->tree_interface.add_socket(
        route_name, "", current_socket->idname, NODE_INTERFACE_SOCKET_OUTPUT, nullptr);
    BKE_ntree_update_after_single_tree_change(bmain, *group_tree);

    bNodeSocket *group_output_socket = bke::node_find_enabled_input_socket(*group_output,
                                                                          route_name);
    if (group_output_socket == nullptr) {
      return false;
    }
    bke::node_add_link(
        *group_tree, *current_node, *current_socket, *group_output, *group_output_socket);
    BKE_ntree_update_after_single_tree_change(bmain, *group_tree);

    /* The instance node in the parent tree only grows the matching output once it is told its
     * group's interface moved. Nothing else tags it: the localized trees are not in \a bmain, so
     * the update system cannot discover the relation on its own. */
    bNodeTree &parent_tree = group_index == 0 ?
                                 root_tree :
                                 *id_cast<bNodeTree *>(group_path[group_index - 1]->id);
    BKE_ntree_update_tag_node_property(&parent_tree, &group_node);
    BKE_ntree_update_after_single_tree_change(bmain, parent_tree);

    current_node = &group_node;
    current_socket = bke::node_find_enabled_output_socket(group_node, route_name);
    if (current_socket == nullptr) {
      return false;
    }
  }

  root_tree.ensure_topology_cache();
  if (!root_tree.all_nodes().contains(current_node)) {
    PBR_BAKE_LOG("route: '%s' did not reach the root tree; path is shorter than the nesting\n",
                 route_name.c_str());
    return false;
  }
  bke::node_add_link(root_tree, *current_node, *current_socket, dest_node, dest_socket);
  BKE_ntree_update_after_single_tree_change(bmain, root_tree);
  return true;
}

/**
 * Add a node to \a tree encoding a signed vector into the [0, 1] range a normal map stores. The
 * caller routes the source into its `Vector` input and reads its `Vector` output.
 *
 * The encoding lives in the graph rather than in the buffer read-back so that every baked buffer
 * leaves the render in the space its consumer samples it in, and no later layer has to know which
 * request it is looking at.
 */
static bNode *vector_encode_node_add(bNodeTree &tree)
{
  bNode *node = bke::node_add_static_node(nullptr, tree, SH_NODE_VECTOR_MATH);
  if (node == nullptr) {
    return nullptr;
  }
  node->custom1 = NODE_VECTOR_MATH_MULTIPLY_ADD;
  bNodeSocket *multiplier = bke::node_find_socket(*node, SOCK_IN, "Vector_001"_ustr);
  bNodeSocket *addend = bke::node_find_socket(*node, SOCK_IN, "Vector_002"_ustr);
  if (multiplier == nullptr || addend == nullptr) {
    return nullptr;
  }
  for (bNodeSocket *socket : {multiplier, addend}) {
    copy_v3_fl(static_cast<bNodeSocketValueVector *>(socket->default_value)->value, 0.5f);
  }
  return node;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bake Domain
 * \{ */

/** A unit quad whose UV covers [0, 1]^2, framed exactly by the ortho camera below. */
static Mesh *bake_quad_mesh(Main *bmain)
{
  /* The geometry arrays and the face offset storage are allocated from the element counts given at
   * creation time; assigning the counts to an already created empty mesh leaves them unallocated. */
  Mesh *mesh_src = BKE_mesh_new_nomain(4, 0, 1, 4);
  MutableSpan<float3> positions = mesh_src->vert_positions_for_write();
  positions[0] = float3(-1.0f, -1.0f, 0.0f);
  positions[1] = float3(1.0f, -1.0f, 0.0f);
  positions[2] = float3(1.0f, 1.0f, 0.0f);
  positions[3] = float3(-1.0f, 1.0f, 0.0f);
  MutableSpan<int> face_offsets = mesh_src->face_offsets_for_write();
  face_offsets[0] = 0;
  face_offsets[1] = 4;
  MutableSpan<int> corner_verts = mesh_src->corner_verts_for_write();
  corner_verts[0] = 0;
  corner_verts[1] = 1;
  corner_verts[2] = 2;
  corner_verts[3] = 3;

  bke::MutableAttributeAccessor attributes = mesh_src->attributes_for_write();
  bke::SpanAttributeWriter<float2> uv = attributes.lookup_or_add_for_write_only_span<float2>(
      "UVMap", bke::AttrDomain::Corner);
  uv.span[0] = float2(0.0f, 0.0f);
  uv.span[1] = float2(1.0f, 0.0f);
  uv.span[2] = float2(1.0f, 1.0f);
  uv.span[3] = float2(0.0f, 1.0f);
  uv.finish();
  mesh_src->uv_maps_active_set("UVMap");
  mesh_src->uv_maps_default_set("UVMap");

  bke::mesh_calc_edges(*mesh_src, false, false);

  /* Move the geometry into a Main data-block so the temporary object can own it, freeing
   * `mesh_src`. */
  Mesh *mesh = BKE_mesh_add(bmain, "PBR Paint Bake Plane");
  BKE_mesh_nomain_to_mesh(mesh_src, mesh, nullptr);
  return mesh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bake Render
 *
 * Everything in this section is free of PBR Paint: it renders whatever sockets it is handed, of
 * whatever node graph, and never asks what they mean. The section below it is the adapter that
 * turns a #MaterialSourceResolve into these requests, and is the only part a second consumer --
 * the material compositing path, say -- would have to write for itself.
 * \{ */

/** One socket to bake, and the AOV it is delivered under. */
struct BakeSocketRequest {
  /** The socket to read. May live in a group nested anywhere inside the material's own tree. */
  const bNodeSocket *source = nullptr;
  /**
   * The group instance nodes leading to #source, outermost first; empty when it is in the root
   * tree. Must come from the same walk that found #source -- see #route_socket_to_root.
   */
  Vector<const bNode *> group_path;
  /** Unique within one bake: names the AOV, and the group output routed for it at every level. */
  char name[64] = "";
  /** Delivered as an #AOV_TYPE_COLOR pass rather than an #AOV_TYPE_VALUE one. */
  bool is_color = false;
  /** Encode the signed vector the socket produces into the [0, 1] range a normal map stores. */
  bool encode_vector = false;
};

/**
 * Attach an AOV output for every request, routing each source socket up to \a tree.
 *
 * \return false when any of them could not be routed. Reported rather than partially baked: a
 *         stroke that silently paints only some of the requested sockets is harder to diagnose
 *         than one that reports the material as unusable.
 */
static bool bake_requests_attach(Main &bmain,
                                 bNodeTree &tree,
                                 const Span<BakeSocketRequest> requests)
{
  for (const BakeSocketRequest &request : requests) {
    bNode *aov = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_AOV);
    if (aov == nullptr || aov->storage == nullptr) {
      return false;
    }
    STRNCPY(static_cast<NodeShaderOutputAOV *>(aov->storage)->name, request.name);
    bNodeSocket *aov_input = bke::node_find_socket(
        *aov, SOCK_IN, request.is_color ? "Color"_ustr : "Value"_ustr);
    if (aov_input == nullptr) {
      return false;
    }
    /* A signed vector is encoded before it reaches the AOV, so the buffer leaves the render in
     * the same space every other one does and the read-back stays request-agnostic. */
    bNode *route_dest_node = aov;
    bNodeSocket *route_dest_socket = aov_input;
    if (request.encode_vector) {
      bNode *encode = vector_encode_node_add(tree);
      bNodeSocket *encode_input = encode != nullptr ?
                                      bke::node_find_socket(*encode, SOCK_IN, "Vector"_ustr) :
                                      nullptr;
      bNodeSocket *encode_output = encode != nullptr ?
                                       bke::node_find_socket(*encode, SOCK_OUT, "Vector"_ustr) :
                                       nullptr;
      if (encode_input == nullptr || encode_output == nullptr) {
        PBR_BAKE_LOG("prepare: AOV='%s' vector encode node failed\n", request.name);
        return false;
      }
      bke::node_add_link(tree, *encode, *encode_output, *aov, *aov_input);
      BKE_ntree_update_after_single_tree_change(bmain, tree);
      route_dest_node = encode;
      route_dest_socket = encode_input;
    }
    if (!route_socket_to_root(bmain,
                              tree,
                              *request.source,
                              request.group_path,
                              *route_dest_node,
                              *route_dest_socket,
                              request.name))
    {
      PBR_BAKE_LOG("prepare: AOV='%s' routing to root tree failed\n", request.name);
      return false;
    }
    PBR_BAKE_LOG("prepare: AOV='%s' is_color=%d encode_vector=%d routed ok\n",
                 request.name,
                 int(request.is_color),
                 int(request.encode_vector));
  }
  return true;
}

/**
 * Render \a bake_material's AOVs into \a r_images, one entry per request in the same order.
 *
 * \a bake_material must already be owned by \a bmain and carry the AOV nodes #bake_requests_attach
 * added. Must run on a thread that may take the draw lock, i.e. not the one already inside a draw
 * or paint callback.
 *
 * Every buffer leaves with four float channels whatever its AOV delivered, so a consumer can read
 * them all the same way. On failure nothing is written and \a r_images is left all null.
 */
static bool bake_requests_render(Main &bmain,
                                 Material &bake_material,
                                 const int resolution,
                                 const Span<BakeSocketRequest> requests,
                                 MutableSpan<ImBuf *> r_images)
{
  BLI_assert(r_images.size() == requests.size());
  Scene *scene = BKE_scene_add(&bmain, "PBR Paint Bake Scene");
  ViewLayer *view_layer = static_cast<ViewLayer *>(scene->view_layers.first);
  STRNCPY(scene->r.engine, RE_engine_id_BLENDER_EEVEE_NEXT);
  scene->r.xsch = resolution;
  scene->r.ysch = resolution;
  scene->r.size = 100;
  scene->r.cfra = 1;
  /* Only the AOVs are read back; the combined pass is what the render always produces. */
  view_layer->passflag = SCE_PASS_COMBINED;
  /* Render as a preview: the full pipeline updates the frame for the whole Main, which needs a
   * window manager (#BKE_image_editors_update_frame) and runs Python callbacks, neither of which
   * exists for this temporary Main. The preview path evaluates only this scene's depsgraph, which
   * is what the shader preview render relies on for its own separate Main as well. */
  scene->r.scemode |= R_BUTS_PREVIEW;

  Mesh *mesh = bake_quad_mesh(&bmain);
  Object *object = BKE_object_add_for_data(
      &bmain, scene, view_layer, OB_MESH, "PBR Paint Bake Plane", &mesh->id, true);
  BKE_object_material_slot_add(&bmain, object);
  BKE_object_material_assign(&bmain, object, &bake_material, 1, BKE_MAT_ASSIGN_OBJECT);
  BKE_collection_object_add(&bmain, scene->master_collection, object);

  Object *camera = BKE_object_add(&bmain, scene, view_layer, OB_CAMERA, "PBR Paint Bake Camera");
  Camera *camera_data = id_cast<Camera *>(camera->data);
  camera_data->type = CAM_ORTHO;
  /* The quad spans [-1, 1], so an ortho width of 2 frames it exactly: UV 0..1 maps onto the whole
   * render, which is the mapping the sampling path assumes. */
  camera_data->ortho_scale = 2.0f;
  camera_data->clip_start = 0.01f;
  camera_data->clip_end = 10.0f;
  camera->loc[2] = 1.0f;
  scene->camera = camera;

  for (const BakeSocketRequest &request : requests) {
    ViewLayerAOV *aov = BKE_view_layer_add_aov(view_layer);
    STRNCPY(aov->name, request.name);
    aov->type = request.is_color ? AOV_TYPE_COLOR : AOV_TYPE_VALUE;
  }
  BKE_view_layer_synced_ensure(bmain, scene, view_layer);

  Render *render = RE_NewSceneRender(scene);
  /* The engine binds its own GPU context (#DRW_render_context_enable); enabling one here would
   * take the draw lock a second time on this thread. Like the regular render and preview jobs,
   * leave context handling to the engine. */
  RE_PreviewRender(render, &bmain, scene);

  bool success = true;
  RenderResult *render_result = RE_AcquireResultRead(render);
  RenderLayer *render_layer = render_result != nullptr ?
                                  static_cast<RenderLayer *>(render_result->layers.first) :
                                  nullptr;
  if (render_layer == nullptr) {
    PBR_BAKE_LOG("render: no render layer (result=%p)\n", (void *)render_result);
    success = false;
  }
  else {
#if PBR_MATERIAL_BAKE_DEBUG
    PBR_BAKE_LOG("render: layer='%s' passes present:\n", render_layer->name);
    for (const RenderPass &pass : render_layer->passes) {
      PBR_BAKE_LOG("  pass='%s' view='%s' channels=%d ibuf=%p %dx%d\n",
                   pass.name,
                   pass.view,
                   pass.channels,
                   (void *)pass.ibuf,
                   pass.ibuf != nullptr ? pass.ibuf->x : 0,
                   pass.ibuf != nullptr ? pass.ibuf->y : 0);
    }
#endif
    for (const int request_index : requests.index_range()) {
      const BakeSocketRequest &request = requests[request_index];
      const RenderPass *pass = RE_pass_find_by_name(render_layer, request.name, "");
      if (pass == nullptr || pass->ibuf == nullptr || pass->ibuf->float_data() == nullptr ||
          !ELEM(pass->channels, 1, 4) || pass->ibuf->x != resolution ||
          pass->ibuf->y != resolution)
      {
        PBR_BAKE_LOG(
            "render: AOV='%s' unusable (pass=%p ibuf=%p float=%p channels=%d size=%dx%d want=%d)\n",
            request.name,
            (void *)pass,
            pass != nullptr ? (void *)pass->ibuf : nullptr,
            (pass != nullptr && pass->ibuf != nullptr) ? (void *)pass->ibuf->float_data() : nullptr,
            pass != nullptr ? pass->channels : -1,
            (pass != nullptr && pass->ibuf != nullptr) ? pass->ibuf->x : 0,
            (pass != nullptr && pass->ibuf != nullptr) ? pass->ibuf->y : 0,
            resolution);
        success = false;
        break;
      }
      ImBuf *ibuf = IMB_allocImBuf(resolution, resolution, ImBufFlags::Zero);
      if (ibuf == nullptr) {
        success = false;
        break;
      }
      /* Always four channels, whatever the AOV delivered. The direct samplers that read these
       * buffers stride a float buffer by four (#interpolate_bilinear_wrap_fl loads a whole
       * `float4` per texel), which is the same shape #channel_source_image_direct_ok demands of
       * every other source kind. Handing them a one-channel Value AOV reads three floats past
       * each texel, and past the allocation entirely on the last one. */
      ibuf->channels = 4;
      if (!IMB_alloc_float_pixels(ibuf, 4, false)) {
        IMB_freeImBuf(ibuf);
        success = false;
        break;
      }
      float *dst = ibuf->float_data_for_write();
      const float *src = pass->ibuf->float_data();
      const int64_t texel_num = int64_t(resolution) * resolution;
      if (pass->channels == 4) {
        std::memcpy(dst, src, size_t(texel_num) * 4 * sizeof(float));
      }
      else {
        /* A scalar channel is broadcast to RGB so the same sample reads correctly whether the
         * caller wants an intensity or a color, and alpha is opaque. */
        for (const int64_t texel : IndexRange(texel_num)) {
          const float value = src[texel];
          dst[texel * 4 + 0] = value;
          dst[texel * 4 + 1] = value;
          dst[texel * 4 + 2] = value;
          dst[texel * 4 + 3] = 1.0f;
        }
      }
      r_images[request_index] = ibuf;
    }
  }
  if (render_result != nullptr) {
    RE_ReleaseResult(render);
  }
  RE_FreeRender(render);

  if (!success) {
    for (ImBuf *&ibuf : r_images) {
      if (ibuf != nullptr) {
        IMB_freeImBuf(ibuf);
        ibuf = nullptr;
      }
    }
  }
  return success;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PBR Paint Channel Requests
 *
 * The adapter between PBR Paint and the neutral bake above: it decides which sockets of the
 * material carry which paint channel, and everything paint-specific about the bake lives here.
 * A second consumer would write its own function of this shape and reuse the rest unchanged.
 * \{ */

/**
 * The socket carrying \a channel's map data on \a principled, after transparent nodes are skipped.
 *
 * Normal is the exception the generic rule cannot cover. A Normal Map node is read at its own
 * Color input, which is the encoded map itself: its output is already transformed out of tangent
 * space and cannot be turned back into a map. Any other source is taken as the vector it produces
 * and has to be encoded, which \a r_encode_vector reports; that is correct only because the bake
 * quad's tangent basis is the identity, so its world-space normal already is the tangent-space
 * one.
 *
 * \param r_group_path: the group instances the source was reached through, outermost first, which
 *                      #route_socket_to_root needs to route out through the right one.
 */
static const bNodeSocket *channel_bake_source_socket(const bNode &principled,
                                                     const MaterialPaintChannelInfo &info,
                                                     bool &r_encode_vector,
                                                     Vector<const bNode *> &r_group_path)
{
  r_encode_vector = false;
  r_group_path.clear();
  const bNodeSocket *input_socket = bke::node_find_socket(
      principled, SOCK_IN, UString::from_ptr_noinline(info.socket_name));
  if (input_socket == nullptr) {
    return nullptr;
  }
  const bNodeSocket *source = BKE_paint_material_source_socket(*input_socket, &r_group_path);
  if (info.channel != PAINT_MATERIAL_CHANNEL_NORMAL) {
    return source;
  }
  if (source == nullptr) {
    return nullptr;
  }
  if (source->owner_node().type_legacy != SH_NODE_NORMAL_MAP) {
    r_encode_vector = true;
    return source;
  }
  const bNodeSocket *color = bke::node_find_socket(source->owner_node(), SOCK_IN, "Color"_ustr);
  if (color == nullptr) {
    return nullptr;
  }
  /* The second walk starts at the Normal Map node, which already sits at the end of the path found
   * so far, so the path it reports is relative to that tree and the two have to be concatenated.
   * Appending to a separate vector first: the resolver clears the one it is given. */
  Vector<const bNode *> color_group_path;
  const bNodeSocket *color_source = BKE_paint_material_source_socket(*color, &color_group_path);
  if (color_source == nullptr) {
    return nullptr;
  }
  r_group_path.extend(color_group_path);
  return color_source;
}

/**
 * Describe every channel \a resolve marks as #ChannelResolution::Baked as a socket to bake.
 *
 * Reads \a bake_material but never edits it: attaching the AOVs is #bake_requests_attach's job, so
 * a caller that only wants to know whether the material can be baked at all can stop here.
 *
 * \param r_request_channels: the #eMaterialPaintChannel each request in \a r_requests stands for,
 *                            which is what maps the rendered buffers back onto channels.
 * \return false when any of them has no resolvable source, which is reported rather than partially
 *         baked: a stroke that silently paints only some of the channels the material describes is
 *         harder to diagnose than one that reports the material as unusable.
 */
static bool bake_channel_requests_build(const Material &bake_material,
                                        const MaterialSourceResolve &resolve,
                                        Vector<BakeSocketRequest> &r_requests,
                                        Vector<int> &r_request_channels)
{
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  /* The Surface input is resolved like any other socket, so the Principled itself may sit inside a
   * node group. Every path found from one of its inputs is then relative to that group's tree, and
   * routing such a socket out to the root tree needs this leading stretch in front of it. */
  Vector<const bNode *> principled_group_path;
  const bNode *principled = BKE_paint_material_principled_find(
      bake_material, reason, &principled_group_path);
  if (principled == nullptr) {
    PBR_BAKE_LOG("prepare: no Principled, reason=%d\n", int(reason));
    return false;
  }

  for (const int channel : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    if (resolve.channels[channel] != ChannelResolution::Baked) {
      continue;
    }
    const MaterialPaintChannelInfo &info = BKE_paint_material_channels()[channel];
    if (info.socket_name == nullptr) {
      PBR_BAKE_LOG("prepare: channel=%d has no socket_name\n", channel);
      return false;
    }
    BakeSocketRequest request;
    Vector<const bNode *> channel_group_path;
    request.source = channel_bake_source_socket(
        *principled, info, request.encode_vector, channel_group_path);
    if (request.source == nullptr) {
      PBR_BAKE_LOG("prepare: channel=%d socket='%s' has no resolvable source\n",
                   channel,
                   info.socket_name);
      return false;
    }
    /* Root tree -> Principled's tree -> the source's tree. Both stretches are needed: routing only
     * the second one leaves the walk inside the Principled's group with nothing left to unwind. */
    request.group_path = principled_group_path;
    request.group_path.extend(channel_group_path);
    /* Normal is packed RGB, so it travels as a color AOV even though it is not a color. */
    request.is_color = info.is_color || info.channel == PAINT_MATERIAL_CHANNEL_NORMAL;
    SNPRINTF(request.name, "__PBR_PAINT_BAKE_%d", channel);
    PBR_BAKE_LOG("prepare: channel=%d socket='%s' source node='%s' sock='%s' tree='%s'\n",
                 channel,
                 info.socket_name,
                 request.source->owner_node().name,
                 request.source->name,
                 request.source->owner_tree().id.name + 2);

    r_requests.append(request);
    r_request_channels.append(channel);
  }
  return !r_requests.is_empty();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Bake Job
 * \{ */

struct MaterialBakeJob {
  /**
   * Localized copy of the source material, made on the main thread before the job starts: the
   * worker must not read a material the user may be editing meanwhile. Owned by the job until it
   * is handed to the temporary #Main.
   */
  Material *material_copy = nullptr;
  BakeCacheKey key;
  int resolution = 0;
};

/** The #Image data-blocks \a tree and every group it reaches sample, by session UID, once each. */
static void image_dependencies_collect(const bNodeTree &tree,
                                       Set<const bNodeTree *> &visited_trees,
                                       Vector<uint32_t> &r_image_session_uids)
{
  if (!visited_trees.add(&tree)) {
    return;
  }
  for (const bNode *node : tree.all_nodes()) {
    if (node->id == nullptr) {
      continue;
    }
    if (node->is_group()) {
      if (const bNodeTree *group_tree = id_cast<const bNodeTree *>(node->id)) {
        image_dependencies_collect(*group_tree, visited_trees, r_image_session_uids);
      }
      continue;
    }
    if (GS(node->id->name) == ID_IM) {
      r_image_session_uids.append_non_duplicates(node->id->session_uid);
    }
  }
}

static void material_bake_startjob(void *customdata, wmJobWorkerStatus *worker_status)
{
  MaterialBakeJob &job = *static_cast<MaterialBakeJob *>(customdata);
  if (job.material_copy == nullptr) {
    return;
  }

  Main *bake_main = BKE_main_new();
  /* Hand the localized copy to the temporary database properly rather than just linking it in:
   * this clears #ID_TAG_NO_MAIN, restores the user counts the localize dropped and registers the
   * ID, all of which the depsgraph build behind the render relies on. */
  BKE_libblock_management_main_add(bake_main, job.material_copy);
  Material &bake_material = *job.material_copy;
  job.material_copy = nullptr;

  if (bake_material.nodetree != nullptr) {
    /* A localized copy has no topology cache yet, and the resolver reads runtime link data. */
    BKE_ntree_update_after_single_tree_change(*bake_main, *bake_material.nodetree);
  }

  /* Collected before the AOV nodes go in, so the routing this bake adds cannot show up as a
   * dependency of its own. The localized copy shares the original's #Image pointers, so these are
   * the same data-blocks #material_source_bake_tag_image_changed will report. */
  Vector<uint32_t> image_session_uids;
  if (bake_material.nodetree != nullptr) {
    Set<const bNodeTree *> visited_trees;
    image_dependencies_collect(*bake_material.nodetree, visited_trees, image_session_uids);
  }

  MaterialSourceResolve resolve = BKE_paint_material_source_resolve(&bake_material);
  bool any_baked = false;
  for (const ChannelResolution channel_resolution : resolve.channels) {
    any_baked |= channel_resolution == ChannelResolution::Baked;
  }
#if PBR_MATERIAL_BAKE_DEBUG
  PBR_BAKE_LOG("job: start res=%d any_baked=%d uid=%u hash=%llu\n",
               job.resolution,
               int(any_baked),
               job.key.material_session_uid,
               (unsigned long long)job.key.node_tree_state_hash);
  for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
    PBR_BAKE_LOG("job: resolve channel=%d resolution=%d reason=%d\n",
                 i,
                 int(resolve.channels[i]),
                 int(resolve.reasons[i]));
  }
#endif

  std::array<ImBuf *, PAINT_MATERIAL_CHANNEL_NUM> images{};
  if (any_baked) {
    Vector<BakeSocketRequest> requests;
    Vector<int> request_channels;
    Vector<ImBuf *> request_images;
    bool baked = bake_material.nodetree != nullptr &&
                 bake_channel_requests_build(bake_material, resolve, requests, request_channels);
    /* The render itself cannot be interrupted once it is under way, so the stop flag is honored at
     * the boundary before it. That covers the case it is actually raised in: the job system sets
     * stop when a newer bake supersedes this one, which happens while the user is still dragging
     * Bake Size or picking a material, and the superseded render is then pure waste. */
    if (baked && worker_status != nullptr && worker_status->stop) {
      PBR_BAKE_LOG("job: superseded before render, dropping\n");
      BKE_main_free(bake_main);
      return;
    }
    if (baked) {
      request_images.resize(requests.size(), nullptr);
      baked = bake_requests_attach(*bake_main, *bake_material.nodetree, requests) &&
              bake_requests_render(
                  *bake_main, bake_material, job.resolution, requests, request_images);
    }
    if (baked) {
      for (const int request_index : requests.index_range()) {
        images[request_channels[request_index]] = request_images[request_index];
      }
    }
    PBR_BAKE_LOG("job: baked=%d channels=%d\n", int(baked), int(requests.size()));
    if (!baked) {
      /* Constant channels stay usable: a failure to render the graph says nothing about inputs
       * that were never linked in the first place. */
      for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
        if (resolve.channels[i] == ChannelResolution::Baked) {
          resolve.channels[i] = ChannelResolution::Unavailable;
          resolve.reasons[i] = ChannelUnavailableReason::GpuCompileFailed;
        }
      }
    }
  }

  BakeCacheEntry entry;
  entry.bake = std::make_shared<const MaterialSourceBake>(resolve, images);
  entry.image_session_uids = std::move(image_session_uids);
  for (const ImBuf *ibuf : images) {
    if (ibuf != nullptr) {
      entry.byte_size += int64_t(sizeof(float)) * 4 * ibuf->x * ibuf->y;
    }
  }

  {
    std::lock_guard lock(g_bake_cache_mutex);
    /* Replaces an entry that has gone stale, and otherwise keeps the one already there: a stroke
     * that started earlier keeps sampling the buffers it was handed either way, so replacing a
     * still-current entry would only churn memory. */
    BakeCacheEntry &stored = g_bake_cache.lookup_or_add_default(job.key);
    if (stored.bake == nullptr || stored.stale) {
      stored = std::move(entry);
      stored.last_used_serial = ++g_bake_use_serial;
    }
    g_bake_pending.remove(job.key);
    /* Every edit to the node trees and every change of Bake Size produces a distinct key, so
     * without this the cache would grow by one full set of buffers -- megabytes per channel at
     * the larger sizes -- for every such change over the life of the session. Only the entry just
     * stored can still be looked up for this material, so the older ones are unreachable. Any
     * stroke still sampling one holds it alive through its own `shared_ptr`. */
    g_bake_cache.remove_if([&](auto item) {
      return item.key.material_session_uid == job.key.material_session_uid &&
             !(item.key == job.key);
    });
    bake_cache_trim_to_budget();
  }

  /* Frees the bake material, its embedded tree, the scene and the quad. */
  BKE_main_free(bake_main);
}

static void material_bake_free(void *customdata)
{
  MaterialBakeJob *job = static_cast<MaterialBakeJob *>(customdata);
  if (job->material_copy != nullptr) {
    /* The job never ran, so the copy never reached a Main of its own. */
    BKE_id_free(nullptr, &job->material_copy->id);
  }
  {
    std::lock_guard lock(g_bake_cache_mutex);
    g_bake_pending.remove(job->key);
  }
  MEM_delete(job);
}

/**
 * Shared body of both #material_source_bake_ensure overloads. \a win may be null; the job system
 * only uses it to route progress reporting.
 */
static void material_source_bake_ensure_impl(wmWindowManager &wm,
                                             wmWindow *win,
                                             Material &ma,
                                             const int resolution)
{
  if (resolution <= 0 || ma.nodetree == nullptr) {
    return;
  }
  const BakeCacheKey key = bake_cache_key(ma, resolution);
  {
    std::lock_guard lock(g_bake_cache_mutex);
    const BakeCacheEntry *cached = g_bake_cache.lookup_ptr(key);
    /* A stale entry still answers lookups, but its inputs have moved on, so it has to be rebaked
     * rather than treated as a hit. */
    if ((cached != nullptr && !cached->stale) || !g_bake_pending.add(key)) {
      return;
    }
  }
  PBR_BAKE_LOG("ensure: starting job for '%s' uid=%u hash=%llu res=%d\n",
               ma.id.name + 2,
               key.material_session_uid,
               (unsigned long long)key.node_tree_state_hash,
               resolution);

  wmJob *wm_job = WM_jobs_get(&wm,
                              win,
                              &ma,
                              "Baking material source...",
                              WM_JOB_EXCL_RENDER,
                              WM_JOB_TYPE_MATERIAL_SOURCE_BAKE);
  MaterialBakeJob *job = MEM_new<MaterialBakeJob>(__func__);
  job->key = key;
  job->resolution = resolution;
  /* Copied here, on the main thread, so the worker never reads a material being edited. Localizing
   * also copies every nested group tree, which is what makes the AOV routing safe: it rewires the
   * copies and never the node groups the user owns. */
  job->material_copy = id_cast<Material *>(BKE_id_copy_ex(
      nullptr,
      &ma.id,
      nullptr,
      LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_NO_ANIMDATA));

  WM_jobs_customdata_set(wm_job, job, material_bake_free);
  WM_jobs_timer(wm_job, 0.2, NC_MATERIAL, NC_MATERIAL);
  WM_jobs_callbacks(wm_job, material_bake_startjob, nullptr, nullptr, nullptr);
  WM_jobs_start(&wm, wm_job);
}

void material_source_bake_ensure(const bContext &C, Material &ma, const int resolution)
{
  wmWindowManager *wm = CTX_wm_manager(&C);
  if (wm == nullptr) {
    return;
  }
  material_source_bake_ensure_impl(*wm, CTX_wm_window(&C), ma, resolution);
}

void material_source_bake_ensure(Main &bmain, Material &ma, const int resolution)
{
  /* Reached from an RNA update, which has no #bContext. Any window will do: the job is keyed on
   * the material, not on where the change came from. */
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain.wm.first);
  if (wm == nullptr) {
    return;
  }
  material_source_bake_ensure_impl(
      *wm, static_cast<wmWindow *>(wm->windows.first), ma, resolution);
}

/** \} */

}  // namespace blender::ed::material_bake
