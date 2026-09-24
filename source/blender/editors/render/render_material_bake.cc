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

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>

#include "MEM_guardedalloc.h"

#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_context.hh"
#include "BKE_global.hh"
#include "BKE_idtype.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_runtime.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"
#include "BKE_idprop.hh"
#include "BKE_paint.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_debug.hh"
#include "BKE_paint_layers_generate.hh"
#include "BKE_paint_layers_composite.hh"
#include "BKE_paint_material_resolve.hh"
#include "BKE_scene.hh"

#include "NOD_defaults.hh"

#include "BLI_assert.h"
#include "BLI_hash.hh"
#include "BLI_index_range.hh"
#include "BLI_listbase.h"
#include "BLI_map.hh"
#include "BLI_math_vector.h"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_utildefines.h"
#include "BLI_uuid.h"
#include "BLI_vector.hh"

#include "DNA_camera_types.h"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_scene_types.h"

#include "IMB_colormanagement.hh"
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

uint64_t material_bake_source_node_tree_hash(const Material &ma)
{
  /* One formula with the generator and the layer bake: the BKE content hash. It must not read
   * `previews_refresh_state`, which a wrapper or localized-copy update bumps on every shared
   * nested group and which used to restart a running bake every editor pass. */
  return BKE_paint_layers_source_material_tree_hash(ma);
}

static BakeCacheKey bake_cache_key(const Material &ma, const int resolution)
{
  BakeCacheKey key;
  key.material_session_uid = ma.id.session_uid;
  key.resolution = resolution;
  key.node_tree_state_hash = material_bake_source_node_tree_hash(ma);
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
/**
 * #Image session UIDs whose #material_bake_to_images job is in flight, and those marked stale by a
 * change with no hash to compare against. Both are session-local and guarded by the mutex above.
 */
/**
 * Counted rather than a set: restarting a job frees the replaced job's data, which releases the
 * very UIDs its replacement has just claimed.
 */
static Map<uint32_t, int> g_image_bake_pending;
static Set<uint32_t> g_image_bake_stale;
/**
 * Per source material session UID, the node-tree hash of the last automatic re-bake started, so
 * the repeated editor updates of a single change do not restart the render for the same state.
 */
static Map<uint32_t, uint64_t> g_image_rebake_started_hash;
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

/** The #Image data-blocks \a tree and every group it reaches sample, by session UID, once each. */
static void image_dependencies_collect(const bNodeTree &tree,
                                       Set<const bNodeTree *> &visited_trees,
                                       Vector<uint32_t> &r_image_session_uids);

/** Whether \a ma's node trees reach an #Image with \a image_session_uid. */
static bool material_samples_image(const Material &ma, const uint32_t image_session_uid)
{
  if (ma.nodetree == nullptr) {
    return false;
  }
  Set<const bNodeTree *> visited_trees;
  Vector<uint32_t> image_session_uids;
  image_dependencies_collect(*ma.nodetree, visited_trees, image_session_uids);
  return image_session_uids.contains(image_session_uid);
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

  /* Maps produced by #material_bake_to_images carry their own link and are not in the cache above.
   * A changed sampled image invalidates every map linked to a material that samples it; image
   * pixels have no content hash, so mark rather than compare. */
  Main *bmain = G_MAIN;
  if (bmain == nullptr) {
    return;
  }
  for (Image &other : bmain->images) {
    ImageMaterialSource source;
    if (!BKE_image_material_source_get(other, source)) {
      continue;
    }
    if (material_samples_image(*source.material, session_uid)) {
      g_image_bake_stale.add(other.id.session_uid);
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

/* Membership helpers for the two runtime sets. Separate from #material_bake_to_images so the
 * completion callback can clear them without reaching into the cache section's internals. */

static void material_bake_images_pending_add(const uint32_t image_session_uid)
{
  BKE_paint_layers_bake_image_pending_add(image_session_uid);
  std::lock_guard lock(g_bake_cache_mutex);
  g_image_bake_pending.lookup_or_add(image_session_uid, 0)++;
}

static void material_bake_images_pending_remove(const uint32_t image_session_uid)
{
  BKE_paint_layers_bake_image_pending_remove(image_session_uid);
  std::lock_guard lock(g_bake_cache_mutex);
  int *count = g_image_bake_pending.lookup_ptr(image_session_uid);
  if (count != nullptr && --*count <= 0) {
    g_image_bake_pending.remove(image_session_uid);
  }
}

static void material_bake_images_stale_remove(const uint32_t image_session_uid)
{
  std::lock_guard lock(g_bake_cache_mutex);
  g_image_bake_stale.remove(image_session_uid);
}

bool material_bake_source_is_baking(const Image &image)
{
  std::lock_guard lock(g_bake_cache_mutex);
  return g_image_bake_pending.contains(image.id.session_uid);
}

bool material_bake_source_is_stale(const Image &image)
{
  ImageMaterialSource source;
  if (!BKE_image_material_source_get(image, source)) {
    return false;
  }
  {
    std::lock_guard lock(g_bake_cache_mutex);
    if (g_image_bake_stale.contains(image.id.session_uid)) {
      return true;
    }
  }
  return material_bake_source_node_tree_hash(*source.material) != source.node_tree_hash;
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
   * The row's coverage, when the request bakes a layer row: an output feeding the row's Factor
   * input, delivered under #alpha_name and composed into the buffer's alpha. Null when the whole
   * channel is baked, whose alpha is opaque by construction. Lives in the same tree as #source,
   * so #group_path serves both.
   */
  const bNodeSocket *alpha_source = nullptr;
  /**
   * The group instance nodes leading to #source, outermost first; empty when it is in the root
   * tree. Must come from the same walk that found #source -- see #route_socket_to_root.
   */
  Vector<const bNode *> group_path;
  /** Unique within one bake: names the AOV, and the group output routed for it at every level. */
  char name[64] = "";
  /** The AOV #alpha_source is delivered under, when it is set. */
  char alpha_name[80] = "";
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

    if (request.alpha_source != nullptr) {
      /* A layer-row bake renders the row's coverage too, under its own Value AOV: the read-back
       * composes it into the buffer's alpha instead of the opaque default a whole-channel bake
       * stands for. */
      bNode *alpha_aov = bke::node_add_static_node(nullptr, tree, SH_NODE_OUTPUT_AOV);
      if (alpha_aov == nullptr || alpha_aov->storage == nullptr) {
        return false;
      }
      STRNCPY(static_cast<NodeShaderOutputAOV *>(alpha_aov->storage)->name, request.alpha_name);
      bNodeSocket *alpha_input = bke::node_find_socket(*alpha_aov, SOCK_IN, "Value"_ustr);
      if (alpha_input == nullptr) {
        return false;
      }
      if (!route_socket_to_root(bmain,
                                tree,
                                *request.alpha_source,
                                request.group_path,
                                *alpha_aov,
                                *alpha_input,
                                request.alpha_name))
      {
        PBR_BAKE_LOG("prepare: AOV='%s' routing to root tree failed\n", request.alpha_name);
        return false;
      }
      PBR_BAKE_LOG("prepare: AOV='%s' routed ok\n", request.alpha_name);
    }
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
/**
 * Where an images bake job stands on its progress bar: preparing the copy and routing the AOVs is
 * quick, the EEVEE render is nearly all of it, and the write-back happens after the worker ends.
 */
constexpr float BAKE_PROGRESS_ATTACHED = 0.1f;
constexpr float BAKE_PROGRESS_RENDERED = 0.95f;

/** Maps the render's own progress into the part of a job's progress bar the render takes up. */
static void bake_render_progress_cb(void *handle, const float progress)
{
  wmJobWorkerStatus *worker_status = static_cast<wmJobWorkerStatus *>(handle);
  worker_status->progress = BAKE_PROGRESS_ATTACHED +
                            (BAKE_PROGRESS_RENDERED - BAKE_PROGRESS_ATTACHED) * progress;
  worker_status->do_update = true;
}

static bool bake_render_test_break_cb(void *handle)
{
  return static_cast<wmJobWorkerStatus *>(handle)->stop;
}

/**
 * \param worker_status: when given, the render reports into its progress and stops early on its
 * stop flag; a render stopped that way is reported as failed.
 */
static bool bake_requests_render(Main &bmain,
                                 Material &bake_material,
                                 const int resolution,
                                 const Span<BakeSocketRequest> requests,
                                 MutableSpan<ImBuf *> r_images,
                                 wmJobWorkerStatus *worker_status = nullptr)
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

  PL_DEBUG_PRINTF(
      "material bake color: render engine='%s' res=%d view_transform='%s' look='%s' "
      "display='%s' exposure=%.2f gamma=%.2f\n",
      scene->r.engine,
      resolution,
      scene->view_settings.view_transform,
      scene->view_settings.look,
      scene->display_settings.display_device,
      scene->view_settings.exposure,
      scene->view_settings.gamma);

  for (const BakeSocketRequest &request : requests) {
    PL_DEBUG_PRINTF("material bake color: aov name='%s' type=%s alpha='%s'\n",
                    request.name,
                    request.is_color ? "Color" : "Value",
                    request.alpha_source != nullptr ? request.alpha_name : "-");
    ViewLayerAOV *aov = BKE_view_layer_add_aov(view_layer);
    STRNCPY(aov->name, request.name);
    aov->type = request.is_color ? AOV_TYPE_COLOR : AOV_TYPE_VALUE;
    if (request.alpha_source != nullptr) {
      ViewLayerAOV *alpha_aov = BKE_view_layer_add_aov(view_layer);
      STRNCPY(alpha_aov->name, request.alpha_name);
      alpha_aov->type = AOV_TYPE_VALUE;
    }
  }
  BKE_view_layer_synced_ensure(bmain, scene, view_layer);

  Render *render = RE_NewSceneRender(scene);
  if (worker_status != nullptr) {
    RE_progress_cb(render, worker_status, bake_render_progress_cb);
    RE_test_break_cb(render, worker_status, bake_render_test_break_cb);
  }
  /* The engine binds its own GPU context (#DRW_render_context_enable); enabling one here would
   * take the draw lock a second time on this thread. Like the regular render and preview jobs,
   * leave context handling to the engine. */
  RE_PreviewRender(render, &bmain, scene);

  bool success = worker_status == nullptr || !worker_status->stop;
  RenderResult *render_result = RE_AcquireResultRead(render);
  RenderLayer *render_layer = render_result != nullptr ?
                                  static_cast<RenderLayer *>(render_result->layers.first) :
                                  nullptr;
  if (render_layer == nullptr) {
    PBR_BAKE_LOG("render: no render layer (result=%p)\n", (void *)render_result);
    success = false;
  }
  else if (success) {
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
        /* An AOV is a Color socket, three channels wide; the fourth the render layer hands back
         * is whatever the film's own coverage happened to be, not something this AOV defined --
         * a layer built from it must read as fully covered, the way the scalar branch below
         * already forces for a Value AOV, or its Mix factor (driven by this alpha as coverage,
         * see #layer_factor_coverage_link) reads the bake as unpainted and never shows it. */
        for (const int64_t texel : IndexRange(texel_num)) {
          dst[texel * 4 + 3] = 1.0f;
        }
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
      if (request.alpha_source != nullptr) {
        const RenderPass *alpha_pass = RE_pass_find_by_name(render_layer, request.alpha_name, "");
        if (alpha_pass == nullptr || alpha_pass->ibuf == nullptr ||
            alpha_pass->ibuf->float_data() == nullptr || !ELEM(alpha_pass->channels, 1, 4) ||
            alpha_pass->ibuf->x != resolution || alpha_pass->ibuf->y != resolution)
        {
          PBR_BAKE_LOG("render: alpha AOV='%s' unusable (pass=%p ibuf=%p channels=%d)\n",
                       request.alpha_name,
                       (void *)alpha_pass,
                       alpha_pass != nullptr ? (void *)alpha_pass->ibuf : nullptr,
                       alpha_pass != nullptr ? alpha_pass->channels : -1);
          IMB_freeImBuf(ibuf);
          success = false;
          break;
        }
        const float *alpha_src = alpha_pass->ibuf->float_data();
        /* The row's coverage replaces the opaque alpha the compose above wrote. A Value AOV is
         * delivered either as one channel or broadcast to RGB, so channel 0 is the value either
         * way. */
        const int64_t alpha_stride = alpha_pass->channels;
        for (const int64_t texel : IndexRange(texel_num)) {
          dst[texel * 4 + 3] = alpha_src[texel * alpha_stride];
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
                                        const bool include_image_resolution,
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
    /* An Image-resolution channel is a plain texture the caller could read directly, so the paint
     * cache skips it. Baking into an #Image data-block still has to render it: the map has to hold
     * the channel as the material actually evaluates it, sampling nodes and all. */
    const ChannelResolution channel_resolution = resolve.channels[channel];
    const bool wanted = channel_resolution == ChannelResolution::Baked ||
                        (include_image_resolution &&
                         channel_resolution == ChannelResolution::Image);
    if (!wanted) {
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
                 bake_channel_requests_build(bake_material,
                                             resolve,
                                             /*include_image_resolution=*/false,
                                             requests,
                                             request_channels);
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
                                             const int resolution,
                                             [[maybe_unused]] const char *reason)
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
  PL_DEBUG_PRINTF("paint layers bake: start kind=source material='%s' row='-' reason=%s\n",
                  ma.id.name + 2,
                  reason);

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

void material_source_bake_ensure(const bContext &C,
                                 Material &ma,
                                 const int resolution,
                                 const char *reason)
{
  wmWindowManager *wm = CTX_wm_manager(&C);
  if (wm == nullptr) {
    return;
  }
  material_source_bake_ensure_impl(*wm, CTX_wm_window(&C), ma, resolution, reason);
}

void material_source_bake_ensure(Main &bmain,
                                 Material &ma,
                                 const int resolution,
                                 const char *reason)
{
  /* Reached from an RNA update, which has no #bContext. Any window will do: the job is keyed on
   * the material, not on where the change came from. */
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain.wm.first);
  if (wm == nullptr) {
    return;
  }
  material_source_bake_ensure_impl(
      *wm, static_cast<wmWindow *>(wm->windows.first), ma, resolution, reason);
}

/** \} */


/* -------------------------------------------------------------------- */
/** \name Material to Images
 *
 * The same render as above, delivered into #Image data-blocks the user owns instead of into the
 * session cache. Everything that touches an #Image runs on the main thread: the worker only
 * renders, and the buffers it produces are written back from the completion callback.
 * \{ */

struct MaterialBakeImagesJob {
  /** Localized copy of the source material, made on the main thread. Owned until the job runs. */
  Material *material_copy = nullptr;
  /** Parallel arrays: the target for #channels[i] is the image with #target_session_uids[i]. */
  Vector<uint32_t> target_session_uids;
  Vector<eMaterialPaintChannel> channels;
  /** Rendered buffers, one per entry of #channels, filled by the worker. Owned. */
  Vector<ImBuf *> rendered;
  int size = 0;
  /** Node-tree hash the targets carry once this bake lands. */
  uint64_t baked_hash = 0;
  uint32_t material_session_uid = 0;
};

static int bake_size_clamp(const int size)
{
  return std::clamp(size, 16, 16384);
}

/** Whether \a channel holds color rather than data, which decides its colorspace. */
static bool bake_channel_is_color(const eMaterialPaintChannel channel)
{
  return ELEM(channel, PAINT_MATERIAL_CHANNEL_BASE_COLOR, PAINT_MATERIAL_CHANNEL_EMISSION);
}

/**
 * Bring a reused map to the colorspace its channel needs: scene linear for color, data for the
 * rest. Maps baked before the scene-linear fix carry sRGB in their float buffer, so their pixels
 * are converted to scene linear before the tag changes; otherwise only the tag is corrected.
 * Main thread only.
 */
void bake_target_image_normalize_colorspace(Image &image,
                                            const eMaterialPaintChannel channel)
{
  const char *wanted = IMB_colormanagement_role_colorspace_name_get(
      bake_channel_is_color(channel) ? COLOR_ROLE_SCENE_LINEAR : COLOR_ROLE_DATA);
  if (STREQ(image.colorspace_settings.name, wanted)) {
    return;
  }
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf != nullptr && ibuf->float_data() != nullptr) {
    const ColorSpace *from = ibuf->float_buffer.colorspace;
    if (from != nullptr && !IMB_colormanagement_space_is_data(from) &&
        !IMB_colormanagement_space_is_scene_linear(from))
    {
      IMB_colormanagement_colorspace_to_scene_linear(
          ibuf->float_data_for_write(), ibuf->x, ibuf->y, 4, from, false);
      ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
      BKE_image_mark_dirty(&image, ibuf);
    }
    /* Keep the cached buffer's own colorspace in step with the tag; the pair disagreeing is the
     * exact state the fix has to clear, and a stale pointer would keep the buffer on the old
     * space even though `colorspace_settings` now says otherwise. */
    IMB_colormanagement_assign_float_colorspace(ibuf, wanted);
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
  STRNCPY(image.colorspace_settings.name, wanted);
  BKE_image_partial_update_mark_full_update(&image);
  BKE_image_free_gputextures(&image);
}

/** The target #Image linked to (\a material, \a channel), or null when there is none. */
static Image *bake_target_image_find(Main &bmain,
                                     const Material &material,
                                     const eMaterialPaintChannel channel)
{
  for (Image &image : bmain.images) {
    ImageMaterialSource source;
    if (!BKE_image_material_source_get(image, source)) {
      continue;
    }
    if (source.material == &material && source.channel == int(channel)) {
      return &image;
    }
  }
  return nullptr;
}

/** Create one target image and link it back to \a material. Main thread only. */
Image *bake_target_image_create(Main &bmain,
                                Material &material,
                                const eMaterialPaintChannel channel,
                                const int size,
                                const char *layer_id,
                                const uint64_t current_hash)
{
  const MaterialPaintChannelInfo &info = BKE_paint_material_channels()[channel];
  char name[MAX_ID_NAME - 2];
  SNPRINTF(name, "%s %s Bake", material.id.name + 2, info.ui_name);

  const bool is_color = bake_channel_is_color(channel);
  const float color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  Image *image = BKE_image_add_generated(&bmain,
                                         size,
                                         size,
                                         name,
                                         32,
                                         /*floatbuf=*/true,
                                         IMA_GENTYPE_BLANK,
                                         color,
                                         /*stereo3d=*/false,
                                         /*is_data=*/!is_color,
                                         /*tiled=*/false);
  if (image == nullptr) {
    return nullptr;
  }
  /* The map is a float buffer holding scene-linear pixels, so its colorspace has to say so: the
   * shader path uploads a float buffer raw and never decodes a display space from it (see the
   * FloatBufferCache assertion that a float buffer be scene linear or data). Declaring a color map
   * sRGB instead made the write below encode every pixel, which the shader then read as if it were
   * already linear, and the baked color came out lighter than the live material. Data stays data. */
  STRNCPY(image->colorspace_settings.name,
          IMB_colormanagement_role_colorspace_name_get(is_color ? COLOR_ROLE_SCENE_LINEAR :
                                                                  COLOR_ROLE_DATA));

  BLI_uuid_parse_string(&image->paint_layer_id, layer_id);
  image->paint_layer_channel = int(channel);

#if PAINT_LAYERS_DEBUG_LOG
  {
    /* Diagnostic: the colorspace the image declares versus the one the float buffer actually
     * carries. A mismatch is what makes the write below encode pixels the shader path reads raw. */
    void *diag_lock = nullptr;
    ImBuf *diag_ibuf = BKE_image_acquire_ibuf(image, nullptr, &diag_lock);
    printf("material bake color: create image='%s' channel=%d is_color=%d size=%d "
           "settings_cs='%s' float_cs='%s' is_data=%d gpu_linear_premul=%d\n",
           image->id.name + 2,
           int(channel),
           int(is_color),
           size,
           image->colorspace_settings.name,
           diag_ibuf != nullptr ? IMB_colormanagement_get_float_colorspace(diag_ibuf) : "<no ibuf>",
           int(IMB_colormanagement_space_name_is_data(image->colorspace_settings.name)),
           int((image->flag & IMA_GPU_LINEAR_PREMUL) != 0));
    BKE_image_release_ibuf(image, diag_ibuf, diag_lock);
  }
#endif

  ImageMaterialSource link;
  link.material = &material;
  link.channel = int(channel);
  /* Not "successfully baked" yet: the completion callback rewrites this with the same value once
   * pixels land. A bake that fails leaves the map stale rather than claiming to match. */
  link.node_tree_hash = current_hash;
  link.bake_size = size;
  BKE_image_material_source_set(*image, link);
  return image;
}

/** Fill \a image with a single color. Main thread only. */
static void bake_target_image_fill_constant(Image &image, const float4 &value)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf != nullptr && ibuf->float_data() != nullptr) {
    float *dst = ibuf->float_data_for_write();
    const int64_t texel_num = int64_t(ibuf->x) * ibuf->y;
    for (const int64_t texel : IndexRange(texel_num)) {
      dst[texel * 4 + 0] = value.x;
      dst[texel * 4 + 1] = value.y;
      dst[texel * 4 + 2] = value.z;
      dst[texel * 4 + 3] = value.w;
    }
    /* The target is scene linear (color) or data, and the constant is already in that space, so
     * there is nothing to convert. The old `scene_linear_to_colorspace` call encoded color into
     * the sRGB the map used to declare, which the shader read raw as linear. */
#if PAINT_LAYERS_DEBUG_LOG
    const float mean_before[3] = {dst[0], dst[1], dst[2]};
    printf("material bake color: fill_constant image='%s' settings_cs='%s' float_cs='%s' "
           "is_data=%d gpu_linear_premul=%d mean_before=(%.4f,%.4f,%.4f) "
           "mean_after=(%.4f,%.4f,%.4f)\n",
           image.id.name + 2,
           image.colorspace_settings.name,
           IMB_colormanagement_get_float_colorspace(ibuf),
           int(IMB_colormanagement_space_name_is_data(image.colorspace_settings.name)),
           int((image.flag & IMA_GPU_LINEAR_PREMUL) != 0),
           mean_before[0],
           mean_before[1],
           mean_before[2],
           dst[0],
           dst[1],
           dst[2]);
#endif
    ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
    BKE_image_mark_dirty(&image, ibuf);
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
  BKE_image_partial_update_mark_full_update(&image);
  BKE_image_free_gputextures(&image);
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, &image);
}

/** Write one rendered buffer into its target image. Main thread only. */
void bake_target_image_write_back(Image &image, const ImBuf &rendered)
{
  void *lock = nullptr;
  ImBuf *ibuf = BKE_image_acquire_ibuf(&image, nullptr, &lock);
  if (ibuf != nullptr && ibuf->float_data() != nullptr) {
    /* The render is square and matches the requested size, but the target may have been resized
     * by the user since it was created. */
    const ImBuf *source = &rendered;
    ImBuf *scaled = nullptr;
    if (rendered.x != ibuf->x || rendered.y != ibuf->y) {
      scaled = IMB_scale_into_new(&rendered, ibuf->x, ibuf->y, IMBScaleFilter::Box, false);
      source = scaled;
    }
    if (source != nullptr && source->float_data() != nullptr) {
      float *dst = ibuf->float_data_for_write();
      const int64_t texel_num = int64_t(ibuf->x) * ibuf->y;
      memcpy(dst, source->float_data(), size_t(texel_num) * 4 * sizeof(float));
      /* The render delivers scene-linear pixels and the target is scene linear (color) or data,
       * so the pixels go in unchanged. The old `scene_linear_to_colorspace` encoded color maps
       * into the sRGB they used to declare; a float texture is uploaded raw, so the shader read
       * those encoded values as linear and every baked color came out too light. */
#if PAINT_LAYERS_DEBUG_LOG
      double mean_render[3] = {0.0, 0.0, 0.0};
      double mean_stored[3] = {0.0, 0.0, 0.0};
      for (const int64_t texel : IndexRange(texel_num)) {
        for (const int component : IndexRange(3)) {
          mean_render[component] += source->float_data()[texel * 4 + component];
          mean_stored[component] += dst[texel * 4 + component];
        }
      }
      for (const int component : IndexRange(3)) {
        mean_render[component] /= double(texel_num);
        mean_stored[component] /= double(texel_num);
      }
      printf("material bake color: write image='%s' settings_cs='%s' float_cs='%s' is_data=%d "
             "gpu_linear_premul=%d mean_render=(%.4f,%.4f,%.4f) mean_stored=(%.4f,%.4f,%.4f)\n",
             image.id.name + 2,
             image.colorspace_settings.name,
             IMB_colormanagement_get_float_colorspace(ibuf),
             int(IMB_colormanagement_space_name_is_data(image.colorspace_settings.name)),
             int((image.flag & IMA_GPU_LINEAR_PREMUL) != 0),
             mean_render[0],
             mean_render[1],
             mean_render[2],
             mean_stored[0],
             mean_stored[1],
             mean_stored[2]);
#endif
      ibuf->userflags |= IB_DISPLAY_BUFFER_INVALID;
      BKE_image_mark_dirty(&image, ibuf);
    }
    if (scaled != nullptr) {
      IMB_freeImBuf(scaled);
    }
  }
  BKE_image_release_ibuf(&image, ibuf, lock);
  BKE_image_partial_update_mark_full_update(&image);
  BKE_image_free_gputextures(&image);
  WM_main_add_notifier(NC_IMAGE | NA_EDITED, &image);
  WM_main_add_notifier(NC_IMAGE | ND_DISPLAY, &image);
}

static void material_bake_images_startjob(void *customdata, wmJobWorkerStatus *worker_status)
{
  MaterialBakeImagesJob &job = *static_cast<MaterialBakeImagesJob *>(customdata);
  if (job.material_copy == nullptr) {
    return;
  }

  Main *bake_main = BKE_main_new();
  BKE_libblock_management_main_add(bake_main, job.material_copy);
  Material &bake_material = *job.material_copy;
  job.material_copy = nullptr;

  if (bake_material.nodetree == nullptr) {
    BKE_main_free(bake_main);
    return;
  }
  BKE_ntree_update_after_single_tree_change(*bake_main, *bake_material.nodetree);

  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(&bake_material);
  Vector<BakeSocketRequest> requests;
  Vector<int> request_channels;
  bool baked = bake_channel_requests_build(bake_material,
                                           resolve,
                                           /*include_image_resolution=*/true,
                                           requests,
                                           request_channels);
  /* The builder answers for every renderable channel of the material; this job asked for a subset,
   * and rendering the rest would only cost time. */
  for (int64_t request_index = requests.size() - 1; request_index >= 0; request_index--) {
    if (!job.channels.contains(eMaterialPaintChannel(request_channels[request_index]))) {
      requests.remove(request_index);
      request_channels.remove(request_index);
    }
  }
  if (requests.is_empty()) {
    baked = false;
  }
  if (baked && worker_status != nullptr && worker_status->stop) {
    BKE_main_free(bake_main);
    return;
  }

  Vector<ImBuf *> request_images;
  if (baked) {
    request_images.resize(requests.size(), nullptr);
    const bool attached = bake_requests_attach(*bake_main, *bake_material.nodetree, requests);
    if (worker_status != nullptr) {
      worker_status->progress = BAKE_PROGRESS_ATTACHED;
      worker_status->do_update = true;
    }
    /* Stoppable here, unlike the brush source bake: this job shows a progress bar with a cancel
     * button, and the status bar's cancel raises the same flag a superseding bake does. */
    const bool rendered = attached && bake_requests_render(*bake_main,
                                                           bake_material,
                                                           job.size,
                                                           requests,
                                                           request_images,
                                                           worker_status);
    if (worker_status != nullptr) {
      worker_status->progress = BAKE_PROGRESS_RENDERED;
      worker_status->do_update = true;
    }
    baked = rendered;
  }
  if (baked) {
    for (const int request_index : requests.index_range()) {
      const int64_t channel_index = job.channels.first_index_of_try(
          eMaterialPaintChannel(request_channels[request_index]));
      if (channel_index < 0) {
        IMB_freeImBuf(request_images[request_index]);
        continue;
      }
      job.rendered[channel_index] = request_images[request_index];
    }
  }
  else {
    for (ImBuf *ibuf : request_images) {
      if (ibuf != nullptr) {
        IMB_freeImBuf(ibuf);
      }
    }
  }

  BKE_main_free(bake_main);
}

static const MaterialPaintLayer *bake_image_owner_find(Main &bmain,
                                                       const Image &image,
                                                       const Material **r_layered);

/**
 * Settle the bake hash of every Paint Layers row every one of whose bake targets among \a job_targets
 * landed pixels this round (\a landed, same indices), so #BKE_paint_layers_bake_is_valid trusts them.
 * #BKE_paint_layers_material_bake_apply (the hand-over that wires a row onto its target images ahead
 * of the render) deliberately leaves the row unfinalised so a cancelled or failed job cannot leave it
 * stamped valid over blank/stale maps; a row is only settled here once every target of its this job
 * touched is confirmed written -- a partial landing (one channel failed, another did not) leaves it
 * unfinalised too, same as a full cancellation. Images that belong to no row are ignored, so this is
 * safe to call with a mix of paint-layer and ordinary bake targets.
 */
static void material_bake_rows_finalize(Main &bmain,
                                        const Span<Image *> job_targets,
                                        const Span<bool> landed)
{
  BLI_assert(job_targets.size() == landed.size());
  Map<const MaterialPaintLayer *, const Material *> rows;
  Set<const MaterialPaintLayer *> incomplete;
  for (const int64_t i : job_targets.index_range()) {
    Image *image = job_targets[i];
    if (image == nullptr) {
      continue;
    }
    const Material *layered = nullptr;
    const MaterialPaintLayer *layer = bake_image_owner_find(bmain, *image, &layered);
    if (layer == nullptr || layered == nullptr) {
      continue;
    }
    if (!landed[i]) {
      incomplete.add(layer);
      continue;
    }
    rows.add(layer, layered);
  }
  for (const auto item : rows.items()) {
    if (incomplete.contains(item.key)) {
      continue;
    }
    const MaterialPaintLayer *layer = item.key;
    const Material *layered = item.value;
    BKE_paint_layers_bake_finalize(*const_cast<Material *>(layered),
                                   *const_cast<MaterialPaintLayer *>(layer));
    PL_DEBUG_PRINTF("paint layers bake: row finalized material='%s' row='%s'\n",
                    layered->id.name + 2,
                    layer->name);
  }
}

/**
 * Publish the rendered buffers into their targets. Runs on the main thread once the worker is
 * done, which is what makes it safe to touch real #Image data-blocks at all.
 */
static void material_bake_images_endjob(void *customdata)
{
  MaterialBakeImagesJob &job = *static_cast<MaterialBakeImagesJob *>(customdata);
  Main *bmain = G_MAIN;
  if (bmain == nullptr) {
    return;
  }
  Material *source_material = nullptr;
  for (Material &material : bmain->materials) {
    if (material.id.session_uid == job.material_session_uid) {
      source_material = &material;
      break;
    }
  }
  /* The render is of the material as it was copied. An edit since then has already started the
   * bake of the newer state; an undo since then restores both the material and the maps' link to
   * the older one, and nothing would ever start a bake to put the undone pixels right. Either way,
   * these pixels describe a state the material is no longer in. */
  if (source_material == nullptr ||
      material_bake_source_node_tree_hash(*source_material) != job.baked_hash)
  {
    return;
  }
  Vector<Image *> job_targets;
  Vector<bool> landed_flags;
  for (const int i : job.channels.index_range()) {
    /* The worker renders a channel once and stores it at the channel's first entry; later targets
     * of the same channel -- layers baked from one material -- share that buffer. The buffers stay
     * owned by the job and are freed with it. */
    const ImBuf *rendered = job.rendered[job.channels.first_index_of(job.channels[i])];
    const uint32_t session_uid = job.target_session_uids[i];
    Image *target = nullptr;
    for (Image &image : bmain->images) {
      if (image.id.session_uid == session_uid) {
        target = &image;
        break;
      }
    }
    ImageMaterialSource source;
    const bool target_valid = target != nullptr &&
                              BKE_image_material_source_get(*target, source) &&
                              source.channel == int(job.channels[i]) &&
                              source.material != nullptr &&
                              source.material->id.session_uid == job.material_session_uid;
    if (rendered == nullptr || !target_valid) {
      /* A channel that failed to render (including a cancelled job, whose #job.rendered stays
       * empty since #bake_requests_render answers for the whole batch at once) keeps its old
       * pixels and its old hash, so it stays stale and the next re-bake picks it up again. Its
       * owning row, if any, is recorded as incomplete below and so stays unfinalised too. */
      if (target != nullptr) {
        job_targets.append(target);
        landed_flags.append(false);
      }
      continue;
    }
    PL_DEBUG_PRINTF("material bake color: endjob channel=%d target='%s' uid=%u\n",
                    int(job.channels[i]),
                    target->id.name + 2,
                    session_uid);
    bake_target_image_write_back(*target, *rendered);
    /* #bake_target_image_write_back only notifies NC_IMAGE, which the Image Editor listens for --
     * the 3D viewport's shading redraw does not, so a layer added with a still-rendering map (the
     * common case: the graph is wired and the job started before this endjob ever runs) keeps
     * showing whatever the placeholder evaluated to until something unrelated sends NC_MATERIAL.
     * The map just landed in a real material's stack (#source.material, from the same link this
     * validated above), so that material is what needs telling. */
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &source.material->id);

    ImageMaterialSource link = source;
    link.node_tree_hash = job.baked_hash;
    link.bake_size = job.size;
    BKE_image_material_source_set(*target, link);
    material_bake_images_stale_remove(session_uid);
    /* The pending claim is released by #material_bake_images_free, which always follows. */
    job_targets.append(target);
    landed_flags.append(true);
  }
  /* Only the channels that actually landed pixels this round may settle their row's bake hash; a
   * row with a channel that failed (cancelled/failed job, or a target reassigned meanwhile) stays
   * unfinalised and #BKE_paint_layers_bake_is_valid keeps reporting it invalid, so the planner
   * re-queues it by the normal due/stale rules instead of looping. */
  material_bake_rows_finalize(*bmain, job_targets, landed_flags);
}

/** Tag the layered material owning any of the maps \a session_uids for one regeneration. */
static void material_bake_rows_landed_tag(Main &bmain, const Span<uint32_t> session_uids)
{
  Set<const Material *> owners;
  for (Image &image : bmain.images) {
    if (!session_uids.contains(image.id.session_uid)) {
      continue;
    }
    const Material *owner = nullptr;
    if (bake_image_owner_find(bmain, image, &owner) != nullptr && owner != nullptr) {
      owners.add(owner);
    }
  }
  for (const Material *owner : owners) {
    BKE_paint_layers_tag_edited(*const_cast<Material *>(owner));
    PL_DEBUG_PRINTF("paint layers bake: maps landed material='%s'\n", owner->id.name + 2);
  }
}

static void material_bake_images_free(void *customdata)
{
  MaterialBakeImagesJob *job = static_cast<MaterialBakeImagesJob *>(customdata);
  if (job->material_copy != nullptr) {
    /* The job never ran, so the copy never reached a Main of its own. */
    BKE_id_free(nullptr, &job->material_copy->id);
  }
  for (ImBuf *ibuf : job->rendered) {
    if (ibuf != nullptr) {
      IMB_freeImBuf(ibuf);
    }
  }
  for (const uint32_t session_uid : job->target_session_uids) {
    material_bake_images_pending_remove(session_uid);
  }
  /* The maps have landed (or the job was dropped): a Material row that held itself live because
   * they were in flight becomes ready now, and only a regeneration turns that into its Baked mode. */
  if (Main *bmain = G_MAIN) {
    material_bake_rows_landed_tag(*bmain, job->target_session_uids);
  }
  MEM_delete(job);
}

MaterialBakeToImagesResult material_bake_to_images(Main &bmain,
                                                   wmWindowManager *wm,
                                                   wmWindow *win,
                                                   const MaterialBakeToImagesParams &params)
{
  MaterialBakeToImagesResult result;
  if (params.material == nullptr || params.targets.is_empty()) {
    return result;
  }
  BLI_assert(params.blocking || wm != nullptr);

  const int size = bake_size_clamp(params.size);
  const MaterialSourceResolve resolve = BKE_paint_material_source_resolve(params.material);
  ChannelUnavailableReason reason = ChannelUnavailableReason::None;
  if (BKE_paint_material_principled_find(*params.material, reason) == nullptr) {
    return result;
  }

  /* Deduplicated per channel for new maps; an explicit target is its own entry, so two layers
   * baked
   * from one material can both be re-filled by a single render. */
  Vector<const BakeTargetSpec *> to_create;
  for (const BakeTargetSpec &target : params.targets) {
    if (resolve.channels[target.channel] == ChannelResolution::Unavailable) {
      result.skipped_unavailable.append_non_duplicates(target.channel);
      continue;
    }
    const bool duplicate = std::any_of(
        to_create.begin(), to_create.end(), [&](const BakeTargetSpec *other) {
          return other->channel == target.channel && other->existing == target.existing;
        });
    if (!duplicate) {
      to_create.append(&target);
    }
  }
  if (to_create.is_empty()) {
    return result;
  }

  /* #BLI_uuid_generate_random is not thread safe, and this is the main thread. */
  if (params.layer_id[0] != 0) {
    STRNCPY(result.layer_id, params.layer_id);
  }
  else {
    BLI_uuid_format(result.layer_id, BLI_uuid_generate_random());
  }
  const uint64_t current_hash = material_bake_source_node_tree_hash(*params.material);

  Vector<eMaterialPaintChannel> render_channels;
  Vector<uint32_t> render_target_uids;
  for (const BakeTargetSpec *target : to_create) {
    const eMaterialPaintChannel channel = target->channel;
    Image *image = target->existing != nullptr ? target->existing :
                   params.reuse_existing ?
                       bake_target_image_find(bmain, *params.material, channel) :
                       bake_target_image_create(
                           bmain, *params.material, channel, size, result.layer_id, current_hash);
    if (image == nullptr) {
      /* Only reachable when re-baking a layer whose target for this channel is gone. */
      continue;
    }
    /* A reused map may still carry an older colorspace (a pre-fix sRGB color map); a freshly
     * created one is already right, so this is a no-op for it. */
    bake_target_image_normalize_colorspace(*image, channel);
    result.created.append(image);
    result.created_channels.append(channel);

    if (resolve.channels[channel] == ChannelResolution::Constant) {
      /* Nothing to render: the channel is a plain value on the Principled input. */
      bake_target_image_fill_constant(*image, resolve.constants[channel]);
      ImageMaterialSource link;
      link.material = params.material;
      link.channel = int(channel);
      link.node_tree_hash = current_hash;
      link.bake_size = size;
      BKE_image_material_source_set(*image, link);
      material_bake_images_stale_remove(image->id.session_uid);
      continue;
    }
    render_channels.append(channel);
    render_target_uids.append(image->id.session_uid);
  }

  result.ok = !result.created.is_empty();
  if (render_channels.is_empty()) {
    if (result.ok && params.before_render && !params.before_render(result)) {
      result.ok = false;
    }
    if (result.ok) {
      /* Every created image was filled synchronously above (all-Constant channels): there is no
       * job and thus no #material_bake_images_endjob to settle the row's bake hash, so this is the
       * only landing point for this case. All of them landed, so every one is marked true. */
      Vector<bool> all_landed(result.created.size(), true);
      material_bake_rows_finalize(bmain, result.created, all_landed);
    }
    return result;
  }

  MaterialBakeImagesJob *job = MEM_new<MaterialBakeImagesJob>(__func__);
  /* Copied here, on the main thread, so the worker never reads a material being edited. */
  job->material_copy = id_cast<Material *>(
      BKE_id_copy_ex(nullptr,
                     &params.material->id,
                     nullptr,
                     LIB_ID_CREATE_LOCAL | LIB_ID_COPY_LOCALIZE | LIB_ID_COPY_NO_ANIMDATA));
  job->target_session_uids = std::move(render_target_uids);
  job->channels = std::move(render_channels);
  job->rendered.resize(job->channels.size(), nullptr);
  job->size = size;
  job->baked_hash = current_hash;
  job->material_session_uid = params.material->id.session_uid;

  for (const uint32_t session_uid : job->target_session_uids) {
    material_bake_images_pending_add(session_uid);
  }

  /* After the copy, so a hand-over that edits the source material itself cannot leak into the
   * bake; before the job, so no worker thread is touching node trees while it runs. */
  if (params.before_render && !params.before_render(result)) {
    material_bake_images_free(job);
    result.ok = false;
    return result;
  }

  if (params.blocking) {
    material_bake_images_startjob(job, nullptr);
    material_bake_images_endjob(job);
    material_bake_images_free(job);
    return result;
  }

  /* Keyed on the material, so a second bake of the same material restarts this slot rather than
   * racing it: v1's overlap unit is the material, and every target belongs to exactly one. Its own
   * job type keeps it out of the brush source bake's slot for the same material, which would
   * replace this job's callbacks -- its write-back included; #WM_JOB_EXCL_RENDER queues the two
   * renders instead. */
  wmJob *wm_job = WM_jobs_get(wm,
                              win,
                              params.material,
                              "Baking material to images...",
                              WM_JOB_EXCL_RENDER | WM_JOB_PROGRESS,
                              WM_JOB_TYPE_MATERIAL_IMAGES_BAKE);
  WM_jobs_customdata_set(wm_job, job, material_bake_images_free);
  WM_jobs_timer(wm_job, 0.2, NC_IMAGE, NC_IMAGE);
  WM_jobs_callbacks(
      wm_job, material_bake_images_startjob, nullptr, nullptr, material_bake_images_endjob);
  WM_jobs_start(wm, wm_job);
  return result;
}

/**
 * Collect the maps of \a ma to re-bake: those \a wanted selects, plus every map of \a ma whose
 * bake
 * is still in flight. A material has one images job slot, and starting a bake replaces whatever
 * that slot was rendering -- a map only that job knew about would otherwise never get its pixels.
 */

/**
 * The row that owns \a image as one of its bake maps, or null. \a r_layered receives the layered
 * material the row belongs to. Used to keep the automatic re-bake away from orphan maps: an image
 * still linked to the source material but no longer referenced by any row is a leftover from an
 * earlier bake, and re-rendering it would only keep the leak alive.
 */
static const MaterialPaintLayer *bake_image_owner_find(Main &bmain,
                                                       const Image &image,
                                                       const Material **r_layered)
{
  for (const Material &layered : bmain.materials) {
    if (!paint_layers_is_layered(layered)) {
      continue;
    }
    Vector<const MaterialPaintLayer *> layers;
    BKE_paint_layers_flatten(layered, layers);
    for (const MaterialPaintLayer *layer : layers) {
      if (layer->bake == nullptr) {
        continue;
      }
      bool owns = layer->bake->coverage == &image;
      for (const int i : IndexRange(PAINT_MATERIAL_CHANNEL_NUM)) {
        owns |= layer->bake->images[i] == &image;
      }
      if (owns) {
        if (r_layered != nullptr) {
          *r_layered = &layered;
        }
        return layer;
      }
    }
  }
  return nullptr;
}

/**
 * Detach and free every map of \a ma that no row shows and nothing references. Main thread only,
 * and only from the automatic path: the explicit re-bake keeps whatever the user asked for.
 */
static void bake_orphan_images_free(Main &bmain, const Material &ma)
{
  Vector<Image *> orphans;
  for (Image &image : bmain.images) {
    ImageMaterialSource source;
    if (!BKE_image_material_source_get(image, source) || source.material != &ma ||
        !ID_IS_EDITABLE(&image.id))
    {
      continue;
    }
    /* `id.us`, not #ID_REAL_USERS: a fake user must keep the map alive. A map being rendered right
     * now is not an orphan either -- its job still owns it. */
    if (image.id.us > 0 || material_bake_source_is_baking(image) ||
        bake_image_owner_find(bmain, image, nullptr) != nullptr)
    {
      continue;
    }
    orphans.append(&image);
  }
  for (Image *image : orphans) {
    PL_DEBUG_PRINTF("paint layers bake: orphan map freed image='%s'\n", image->id.name + 2);
    BKE_image_material_source_clear(*image);
    BKE_id_free(&bmain, &image->id);
  }
}

static void rebake_targets_collect(
    Main &bmain,
    const Material &ma,
    const FunctionRef<bool(const Image &, const ImageMaterialSource &)> wanted,
    const bool skip_deferred,
    const bool owned_only,
    Vector<BakeTargetSpec> &r_targets,
    int &r_size,
    bool &r_all_pending)
{
  r_all_pending = true;
  for (Image &image : bmain.images) {
    ImageMaterialSource source;
    if (!BKE_image_material_source_get(image, source) || source.material != &ma ||
        !ID_IS_EDITABLE(&image.id))
    {
      continue;
    }
    /* The map of the row the user is working inside stays live: re-baking it here would fight the
     * edit. It catches up on a later update, once that row is left. Tested before the pending read,
     * so a deferred map cannot keep the whole material's bake marked in flight. */
    if (skip_deferred && BKE_paint_layers_bake_image_is_deferred(bmain, image)) {
      continue;
    }
    /* The automatic path only maintains maps a row still shows; an orphan has no consumer, so
     * rendering it is pure waste and pins the memory it occupies. */
    if (owned_only && bake_image_owner_find(bmain, image, nullptr) == nullptr) {
      continue;
    }
    bool pending;
    {
      std::lock_guard lock(g_bake_cache_mutex);
      pending = g_image_bake_pending.contains(image.id.session_uid);
    }
    if (!pending && !wanted(image, source)) {
      continue;
    }
    r_all_pending &= pending;
    r_targets.append({eMaterialPaintChannel(source.channel), &image});
    r_size = std::max(r_size, source.bake_size);
  }
}

/** Start the images bake of \a targets and remember the node-tree state it was started for. */
static void rebake_start(Main &bmain,
                         Material &ma,
                         const Span<BakeTargetSpec> targets,
                         const int size,
                         const uint64_t current_hash)
{
  wmWindowManager *wm = static_cast<wmWindowManager *>(bmain.wm.first);
  if (wm == nullptr || targets.is_empty()) {
    return;
  }
  {
    std::lock_guard lock(g_bake_cache_mutex);
    g_image_rebake_started_hash.add_overwrite(ma.id.session_uid, current_hash);
  }
  MaterialBakeToImagesParams params;
  params.material = &ma;
  params.targets = targets;
  params.size = size;
  params.blocking = false;
  material_bake_to_images(bmain, wm, static_cast<wmWindow *>(wm->windows.first), params);
}

void material_bake_layered_rows_ensure(Main &bmain, Material &ma)
{
  if (bmain.wm.first == nullptr || !paint_layers_is_layered(ma)) {
    return;
  }
  /* A row whose maps are being rendered right now must not be restarted: the running job lands
   * its pixels and, if the source changed meanwhile, the next update starts the newer bake. This
   * is the per-row half of the guard; #BKE_paint_layers_bake_is_valid already covers the case
   * where the row's stamped hash matches. */
  const auto row_bake_in_flight = [](const MaterialPaintLayer &row) {
    if (row.bake == nullptr) {
      return false;
    }
    for (Image *image : row.bake->images) {
      if (image != nullptr && material_bake_source_is_baking(*image)) {
        return true;
      }
    }
    return row.bake->coverage != nullptr && material_bake_source_is_baking(*row.bake->coverage);
  };
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  for (const MaterialPaintLayer *layer_const : layers) {
    /* The row the user is editing inside is left live: re-baking it on every source edit would
     * fight the edit. The bake catches up on a later update, once another row becomes active. */
    if (layer_const->source != MA_PAINT_LAYER_SOURCE_MATERIAL || layer_const->material == nullptr ||
        layer_const->bake == nullptr || layer_const->bake->mode == MA_PAINT_LAYER_BAKE_NEVER ||
        BKE_paint_layers_bake_is_valid(ma, *layer_const) ||
        BKE_paint_layers_bake_row_is_deferred(ma, *layer_const) ||
        row_bake_in_flight(*layer_const))
    {
      continue;
    }
    MaterialPaintLayer *row = const_cast<MaterialPaintLayer *>(layer_const);
    const int size = row->bake->size > 0 ? row->bake->size : 1024;

    /* Reuse the row's own maps so a re-bake does not mint a new `.00N` set every time. Only a
     * changed bake size forces a fresh image; the channel's type (color vs data) is fixed by the
     * channel, so it cannot change under an existing map. */
    Vector<BakeTargetSpec> targets;
    for (const eMaterialPaintChannel channel : BKE_paint_material_bakeable_channels()) {
      Image *existing = (channel == PAINT_MATERIAL_CHANNEL_ALPHA) ? row->bake->coverage :
                                                                    row->bake->images[channel];
      if (existing != nullptr) {
        int existing_w = 0;
        int existing_h = 0;
        BKE_image_get_size(existing, nullptr, &existing_w, &existing_h);
        if (existing_w != size || existing_h != size) {
          existing = nullptr;
        }
      }
      targets.append({channel, existing});
    }

    MaterialBakeToImagesParams params;
    params.material = row->material;
    params.targets = targets;
    params.size = size;
    params.blocking = false;
    /* Hand-over on the main thread, before the job runs: the fresh maps become the row's bake maps
     * and the hash is stamped, so the generator substitutes as soon as the worker fills them. */
    /* Named: #FunctionRef does not own the callable, so a temporary lambda would dangle. */
    const auto hand_over = [&](const MaterialBakeToImagesResult &result) -> bool {
      Vector<int> channels;
      Vector<Image *> images;
      for (const int i : result.created.index_range()) {
        channels.append(int(result.created_channels[i]));
        images.append(result.created[i]);
      }
      BKE_paint_layers_material_bake_apply(
          bmain, ma, *row, size, channels.as_span(), images.as_span());
      return true;
    };
    params.before_render = hand_over;
    wmWindowManager *wm = static_cast<wmWindowManager *>(bmain.wm.first);
    wmWindow *win = (wm != nullptr) ? static_cast<wmWindow *>(wm->windows.first) : nullptr;
    PL_DEBUG_PRINTF("paint layers bake: start kind=images material='%s' row='%s' reason=due\n",
                    ma.id.name + 2,
                    row->name);
    const uint64_t source_hash = material_bake_source_node_tree_hash(*row->material);
    const MaterialBakeToImagesResult due_result = material_bake_to_images(bmain, wm, win, params);
    if (due_result.ok) {
      /* The maps just handed over are the source's; record the state this job renders so the
       * catch-up stale pass of the same source does not start a second, duplicate job that would
       * replace this one on the material's single bake slot. */
      std::lock_guard lock(g_bake_cache_mutex);
      g_image_rebake_started_hash.add_overwrite(row->material->id.session_uid, source_hash);
    }
  }
  /* Custom rows have no CPU expression at all and are rendered by the EEVEE/AOV core directly. */
  material_bake_custom_rows_ensure(bmain, ma);
}

void material_bake_images_rebake_stale(Main &bmain, Material &ma)
{
  /* While some Material row reads \a ma live, the user is editing it through that row: every
   * automatic re-bake of its maps waits until the active marker leaves the row, where the
   * MATERIAL_BAKE_DUE catch-up calls this again. The explicit
   * #material_bake_images_rebake (a button) is not gated by this. */
  if (BKE_paint_layers_source_material_is_live(bmain, ma)) {
    return;
  }
  if (bmain.wm.first == nullptr || ma.nodetree == nullptr) {
    return;
  }

  std::optional<uint64_t> current_hash;
  auto is_stale = [&](const Image &image, const ImageMaterialSource &source) {
    /* Hashed only once a map of this material turns up: every material edit reaches here, and
     * almost none of them were ever baked. */
    if (!current_hash) {
      current_hash = material_bake_source_node_tree_hash(ma);
    }
    std::lock_guard lock(g_bake_cache_mutex);
    return source.node_tree_hash != *current_hash ||
           g_image_bake_stale.contains(image.id.session_uid);
  };
  Vector<BakeTargetSpec> targets;
  int size = 0;
  bool all_pending = true;
  /* Replaced or superseded maps that no row shows any more are leftovers; drop the ones nothing
   * references so the leak of `.00N` maps cannot grow. A map still referenced by the old layer
   * tree has a real user and is left for the pass after that tree is regenerated. */
  bake_orphan_images_free(bmain, ma);
  /* A stale-map pass is the automatic path that must leave the edited row alone, and maintain only
   * maps a row still shows. */
  rebake_targets_collect(bmain, ma, is_stale, true, /*owned_only=*/true, targets, size, all_pending);
  if (targets.is_empty()) {
    return;
  }
  if (!current_hash) {
    current_hash = material_bake_source_node_tree_hash(ma);
  }
  {
    std::lock_guard lock(g_bake_cache_mutex);
    /* One change reaches the editors once per view layer; only a new node-tree state restarts. */
    if (all_pending &&
        g_image_rebake_started_hash.lookup_default(ma.id.session_uid, 0) == *current_hash)
    {
      return;
    }
  }
#if PAINT_LAYERS_DEBUG_LOG
  /* Why this image and not another: name the row that owns it, or say that no row does. */
  for (const BakeTargetSpec &target : targets) {
    Image &image = *target.existing;
    const Material *owner_layered = nullptr;
    const MaterialPaintLayer *owner_row = bake_image_owner_find(bmain, image, &owner_layered);
    if (owner_row != nullptr) {
      printf("paint layers bake:   target image='%s' channel=%d owner=row '%s' of '%s' "
             "deferred=%d\n",
             image.id.name + 2,
             int(target.channel),
             owner_row->name,
             owner_layered->id.name + 2,
             BKE_paint_layers_bake_row_is_deferred(*owner_layered, *owner_row) ? 1 : 0);
    }
    else {
      printf("paint layers bake:   target image='%s' channel=%d owner=none (users=%d)\n",
             image.id.name + 2,
             int(target.channel),
             int(ID_REAL_USERS(&image.id)));
    }
  }
#endif
  PL_DEBUG_PRINTF("paint layers bake: start kind=images material='%s' row='-' reason=stale\n",
                  ma.id.name + 2);
  rebake_start(bmain, ma, targets, size, *current_hash);
}

void material_bake_images_rebake(Main &bmain,
                                 Material &ma,
                                 const Span<Image *> images,
                                 const int size)
{
  if (ma.nodetree == nullptr) {
    return;
  }
  auto is_requested = [&](const Image &image, const ImageMaterialSource & /*source*/) {
    return images.contains(const_cast<Image *>(&image));
  };
  Vector<BakeTargetSpec> targets;
  int max_size = 0;
  bool all_pending = true;
  /* An explicit re-bake (a resize, a freshly bound map): the user asked for it, so a deferred row
   * is baked like any other. */
  rebake_targets_collect(bmain, ma, is_requested, false, /*owned_only=*/false, targets, max_size, all_pending);
  PL_DEBUG_PRINTF("paint layers bake: start kind=images material='%s' row='-' reason=explicit\n",
                  ma.id.name + 2);
  rebake_start(
      bmain, ma, targets, size > 0 ? size : max_size, material_bake_source_node_tree_hash(ma));
}

/* -------------------------------------------------------------------- */
/** \name Custom Layer Bake
 *
 * A Custom row is a node group the CPU cannot evaluate, so both the generator and the CPU
 * composite substitute its baked maps. The group is instantiated in a throw-away host material and
 * rendered by the same EEVEE/AOV core the source-material bake uses: every `COLOR:<CHANNEL>` output
 * becomes one request, its `COVERAGE` output rides the alpha via the request's `alpha_source`, and
 * every `BELOW:<CHANNEL>` input is fed from a texture holding the live stack under the row. The
 * host, the below images and a localized copy of the group all live in a temporary #Main the worker
 * owns, so nothing the user is editing is read off the main thread.
 * \{ */

namespace {

struct CustomBakeJob {
  Main *bake_main = nullptr;
  Material *host = nullptr;
  uint32_t owner_session_uid = 0;
  bUUID marker = {};
  uint32_t target_hash[2] = {0, 0};
  int size = 0;
  Vector<BakeSocketRequest> requests;
  /** Parallel to #requests: the channel of each, or -1 for a coverage-only request. */
  Vector<int> channels;
  Vector<ImBuf *> rendered;
};

const char *custom_socket_role(const bNodeTreeInterfaceSocket &socket)
{
  if (socket.properties == nullptr) {
    return nullptr;
  }
  const IDProperty *prop = IDP_GetPropertyTypeFromGroup(
      socket.properties, "pbr_custom_role", IDP_STRING);
  return prop != nullptr ? IDP_string_get(prop) : nullptr;
}

bNodeSocket *custom_group_socket(bNode &group_node, const char *identifier, const bool input)
{
  if (identifier == nullptr) {
    return nullptr;
  }
  for (bNodeSocket &socket : input ? group_node.inputs : group_node.outputs) {
    if (socket.identifier != nullptr && STREQ(socket.identifier, identifier)) {
      return &socket;
    }
  }
  return nullptr;
}

uint64_t custom_bake_key(const uint32_t session_uid, const bUUID &marker)
{
  uint64_t h = session_uid;
  h = h * 1000003ull ^ marker.time_low;
  h = h * 1000003ull ^ marker.time_mid;
  h = h * 1000003ull ^ marker.time_hi_and_version;
  h = h * 1000003ull ^ marker.clock_seq_hi_and_reserved;
  h = h * 1000003ull ^ marker.clock_seq_low;
  for (const uint8_t byte : marker.node) {
    h = h * 1000003ull ^ byte;
  }
  return h;
}

Set<uint64_t> &custom_bake_active()
{
  static Set<uint64_t> active;
  return active;
}

CustomBakeJob *custom_bake_prepare(Main &bmain,
                                   const Material &ma,
                                   const MaterialPaintLayer &layer,
                                   const int size)
{
  if (layer.custom_group == nullptr || size <= 0) {
    return nullptr;
  }
  CustomBakeJob *job = MEM_new<CustomBakeJob>(__func__);
  job->bake_main = BKE_main_new();
  job->owner_session_uid = ma.id.session_uid;
  job->marker = layer.marker;
  job->size = size;

  job->host = BKE_material_add(job->bake_main, "PBR Custom Bake");
  if (job->host == nullptr) {
    return job;
  }
  nodes::node_tree_shader_default(nullptr, job->bake_main, &job->host->id);
  bNodeTree *tree = job->host->nodetree;
  if (tree == nullptr) {
    return job;
  }

  bNodeTree *group_copy = id_cast<bNodeTree *>(
      BKE_id_copy_ex(nullptr, &layer.custom_group->id, nullptr, LIB_ID_COPY_LOCALIZE));
  if (group_copy == nullptr) {
    return job;
  }
  BKE_libblock_management_main_add(job->bake_main, group_copy);

  bNode *group_node = bke::node_add_node(nullptr, *tree, "ShaderNodeGroup"_ustr);
  if (group_node == nullptr) {
    return job;
  }
  group_node->id = &group_copy->id;
  id_us_plus(&group_copy->id);
  BKE_ntree_update_tag_node_property(tree, group_node);
  BKE_ntree_update_after_single_tree_change(*job->bake_main, *tree);
  tree->ensure_topology_cache();

  Map<int, Image *> below_images;
  auto below_for_channel = [&](const int channel) -> Image * {
    if (Image **found = below_images.lookup_ptr(channel)) {
      return *found;
    }
    Image *image = BKE_paint_layers_below_image(*job->bake_main, ma, layer, channel, size);
    if (image != nullptr) {
      below_images.add(channel, image);
    }
    return image;
  };

  int first_color_channel = -1;
  for (const bNodeTreeInterfaceSocket *iface : group_copy->interface_outputs()) {
    if (BKE_paint_layers_custom_role_kind(custom_socket_role(*iface)) ==
        PaintLayerCustomRole::Color)
    {
      first_color_channel = BKE_paint_layers_custom_role_channel(custom_socket_role(*iface));
      if (first_color_channel >= 0) {
        break;
      }
    }
  }

  for (const bNodeTreeInterfaceSocket *iface : group_copy->interface_inputs()) {
    const PaintLayerCustomRole role = BKE_paint_layers_custom_role_kind(
        custom_socket_role(*iface));
    bNodeSocket *dest = custom_group_socket(*group_node, iface->identifier, true);
    if (dest == nullptr) {
      continue;
    }
    if (role == PaintLayerCustomRole::Below) {
      const int channel = BKE_paint_layers_custom_role_channel(custom_socket_role(*iface));
      Image *below = (channel >= 0) ? below_for_channel(channel) : nullptr;
      if (below == nullptr) {
        continue;
      }
      bNode *texture = bke::node_add_static_node(nullptr, *tree, SH_NODE_TEX_IMAGE);
      if (texture == nullptr) {
        continue;
      }
      texture->id = &below->id;
      id_us_plus(&below->id);
      bke::node_add_link(*tree,
                         *texture,
                         *bke::node_find_socket(*texture, SOCK_OUT, "Color"_ustr),
                         *group_node,
                         *dest);
    }
    else if (role == PaintLayerCustomRole::CoverageBelow) {
      if (first_color_channel < 0) {
        continue;
      }
      Image *below = below_for_channel(first_color_channel);
      if (below == nullptr) {
        continue;
      }
      bNode *texture = bke::node_add_static_node(nullptr, *tree, SH_NODE_TEX_IMAGE);
      if (texture == nullptr) {
        continue;
      }
      texture->id = &below->id;
      id_us_plus(&below->id);
      bke::node_add_link(*tree,
                         *texture,
                         *bke::node_find_socket(*texture, SOCK_OUT, "Alpha"_ustr),
                         *group_node,
                         *dest);
    }
    else if (role == PaintLayerCustomRole::None) {
      /* A user parameter: the bake uses the value the description stores for it. */
      const IDProperty *prop = (layer.properties != nullptr) ?
                                   IDP_GetPropertyTypeFromGroup(
                                       layer.properties, iface->identifier, IDP_DOUBLE) :
                                   nullptr;
      if (prop != nullptr && dest->type == SOCK_FLOAT && dest->default_value != nullptr) {
        static_cast<bNodeSocketValueFloat *>(dest->default_value)->value = float(
            IDP_coerce_to_double_or_zero(prop));
      }
    }
  }

  const bNodeSocket *coverage_out = nullptr;
  for (const bNodeTreeInterfaceSocket *iface : group_copy->interface_outputs()) {
    if (BKE_paint_layers_custom_role_kind(custom_socket_role(*iface)) ==
        PaintLayerCustomRole::Coverage)
    {
      coverage_out = custom_group_socket(*group_node, iface->identifier, false);
      break;
    }
  }

  for (const bNodeTreeInterfaceSocket *iface : group_copy->interface_outputs()) {
    if (BKE_paint_layers_custom_role_kind(custom_socket_role(*iface)) !=
        PaintLayerCustomRole::Color)
    {
      continue;
    }
    const int channel = BKE_paint_layers_custom_role_channel(custom_socket_role(*iface));
    bNodeSocket *source = custom_group_socket(*group_node, iface->identifier, false);
    if (channel < 0 || source == nullptr) {
      continue;
    }
    BakeSocketRequest request;
    request.source = source;
    request.is_color = BKE_paint_material_channel_info(eMaterialPaintChannel(channel)).is_color;
    SNPRINTF(request.name, "__PBR_CUSTOM_%d", channel);
    if (coverage_out != nullptr) {
      request.alpha_source = coverage_out;
      SNPRINTF(request.alpha_name, "%s__ALPHA", request.name);
    }
    job->requests.append(request);
    job->channels.append(channel);
  }
  if (job->requests.is_empty()) {
    return job;
  }
  if (!bake_requests_attach(*job->bake_main, *tree, job->requests)) {
    job->requests.clear();
  }
  BKE_paint_layers_bake_hash(layer, job->target_hash);
  return job;
}

void custom_bake_job_free(CustomBakeJob *job)
{
  if (job == nullptr) {
    return;
  }
  for (ImBuf *ibuf : job->rendered) {
    if (ibuf != nullptr) {
      IMB_freeImBuf(ibuf);
    }
  }
  if (job->bake_main != nullptr) {
    BKE_main_free(job->bake_main);
    job->bake_main = nullptr;
  }
  MEM_delete(job);
}

void custom_bake_render(CustomBakeJob &job)
{
  if (job.bake_main == nullptr || job.host == nullptr || job.requests.is_empty()) {
    return;
  }
  job.rendered.clear();
  job.rendered.resize(job.requests.size(), nullptr);
  bake_requests_render(*job.bake_main, *job.host, job.size, job.requests, job.rendered);
}

/** Publish the finished render onto the row. Main thread only. */
bool custom_bake_apply(Main &bmain, CustomBakeJob &job)
{
  Material *ma = nullptr;
  for (Material &candidate : bmain.materials) {
    if (candidate.id.session_uid == job.owner_session_uid) {
      ma = &candidate;
      break;
    }
  }
  if (ma == nullptr) {
    return false;
  }
  MaterialPaintLayer *row = BKE_paint_layers_find(*ma, job.marker);
  if (row == nullptr || row->source != MA_PAINT_LAYER_SOURCE_NODE_GROUP || row->bake == nullptr) {
    return false;
  }
  uint32_t hash[2];
  BKE_paint_layers_bake_hash(*row, hash);
  if (hash[0] != job.target_hash[0] || hash[1] != job.target_hash[1]) {
    /* The description moved on while the render ran; the newer state starts its own bake. */
    return false;
  }
  Vector<int> channels;
  Vector<const ImBuf *> colors;
  const ImBuf *coverage = nullptr;
  for (const int64_t i : job.requests.index_range()) {
    const ImBuf *buffer = job.rendered[i];
    if (buffer == nullptr) {
      continue;
    }
    if (job.requests[i].alpha_source != nullptr && coverage == nullptr) {
      coverage = buffer;
    }
    if (job.channels[i] >= 0) {
      channels.append(job.channels[i]);
      colors.append(buffer);
    }
  }
  if (channels.is_empty()) {
    return false;
  }
  BKE_paint_layers_custom_bake_apply(bmain, *ma, *row, job.size, channels, colors, coverage);
  return true;
}

void custom_bake_wm_start(void *customdata, wmJobWorkerStatus * /*status*/)
{
  custom_bake_render(*static_cast<CustomBakeJob *>(customdata));
}

void custom_bake_wm_end(void *customdata)
{
  CustomBakeJob *job = static_cast<CustomBakeJob *>(customdata);
  Main *bmain = G_MAIN;
  if (bmain == nullptr) {
    return;
  }
  if (custom_bake_apply(*bmain, *job)) {
    WM_main_add_notifier(NC_MATERIAL | ND_SHADING, nullptr);
  }
}

void custom_bake_wm_free(void *customdata)
{
  CustomBakeJob *job = static_cast<CustomBakeJob *>(customdata);
  custom_bake_active().remove(custom_bake_key(job->owner_session_uid, job->marker));
  custom_bake_job_free(job);
}

}  // namespace

void material_bake_custom_rows_ensure(Main &bmain, Material &ma)
{
  if (!paint_layers_is_layered(ma)) {
    return;
  }
  Vector<const MaterialPaintLayer *> layers;
  BKE_paint_layers_flatten(ma, layers);
  for (const MaterialPaintLayer *layer_const : layers) {
    if (layer_const->source != MA_PAINT_LAYER_SOURCE_NODE_GROUP ||
        layer_const->custom_group == nullptr || layer_const->bake == nullptr ||
        layer_const->bake->mode == MA_PAINT_LAYER_BAKE_NEVER ||
        BKE_paint_layers_bake_is_valid(ma, *layer_const))
    {
      continue;
    }
    /* The row the user is editing inside stays live; it is baked only once the active marker
     * leaves its subtree, like the Material rows above. */
    if (BKE_paint_layers_bake_row_is_deferred(ma, *layer_const)) {
      continue;
    }
    MaterialPaintLayer *row = const_cast<MaterialPaintLayer *>(layer_const);
    const uint64_t key = custom_bake_key(ma.id.session_uid, row->marker);
    if (custom_bake_active().contains(key)) {
      continue;
    }
    const int size = row->bake->size > 0 ? row->bake->size : 1024;
    PL_DEBUG_PRINTF("paint layers bake: start kind=custom material='%s' row='%s' reason=due\n",
                    ma.id.name + 2,
                    row->name);
    CustomBakeJob *job = custom_bake_prepare(bmain, ma, *row, size);
    if (job == nullptr) {
      continue;
    }
    if (job->requests.is_empty()) {
      custom_bake_job_free(job);
      continue;
    }
    wmWindowManager *wm = static_cast<wmWindowManager *>(bmain.wm.first);
    const bool heavy = size >= PAINT_LAYERS_HEAVY_BAKE_SIZE && wm != nullptr;
    custom_bake_active().add(key);
    if (heavy) {
      wmWindow *win = static_cast<wmWindow *>(wm->windows.first);
      wmJob *wm_job = WM_jobs_get(wm,
                                  win,
                                  job->host,
                                  "Baking custom paint layer...",
                                  WM_JOB_EXCL_RENDER | WM_JOB_PROGRESS,
                                  WM_JOB_TYPE_MATERIAL_IMAGES_BAKE);
      WM_jobs_customdata_set(wm_job, job, custom_bake_wm_free);
      WM_jobs_timer(wm_job, 0.2, NC_MATERIAL, NC_MATERIAL);
      WM_jobs_callbacks(wm_job, custom_bake_wm_start, nullptr, nullptr, custom_bake_wm_end);
      WM_jobs_start(wm, wm_job);
    }
    else {
      custom_bake_render(*job);
      if (custom_bake_apply(bmain, *job)) {
        WM_main_add_notifier(NC_MATERIAL | ND_SHADING, &ma.id);
      }
      custom_bake_active().remove(key);
      custom_bake_job_free(job);
    }
  }
}

/** \} */

}  // namespace blender::ed::material_bake
