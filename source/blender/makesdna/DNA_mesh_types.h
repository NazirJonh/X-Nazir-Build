/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_attribute_types.h"
#include "DNA_customdata_types.h"
#include "DNA_defs.h"

#include "BLI_enum_flags.hh"

#include <optional>

#include "BLI_index_mask_fwd.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_memory_counter_fwd.hh"
#include "BLI_vector_set.hh"

namespace blender {

template<typename T> struct Bounds;
namespace offset_indices {
template<typename T> struct GroupedSpan;
template<typename T> class OffsetIndices;
}  // namespace offset_indices
using offset_indices::GroupedSpan;
using offset_indices::OffsetIndices;
template<typename T> class MutableSpan;
template<typename T> class Span;
namespace bke {
struct BVHTreeFromMesh;
struct MeshRuntime;
class AttributeAccessor;
class MutableAttributeAccessor;
enum class MeshNormalDomain : int8_t;
namespace sculpt_layers {
class SculptLayerGroupRuntime;
}
}  // namespace bke

struct AnimData;
struct bDeformGroup;
struct Ipo;
struct Key;
struct Material;
struct MCol;
struct MEdge;
struct MFace;

/** #Mesh.texspace_flag */
enum eMesh_TexSpaceFlag : char {
  ME_TEXSPACE_FLAG_AUTO = 1 << 0,
  ME_TEXSPACE_FLAG_AUTO_EVALUATED = 1 << 1,
};
ENUM_OPERATORS(eMesh_TexSpaceFlag)

/** #Mesh.editflag */
enum eMesh_EditFlag : char {
  ME_EDIT_MIRROR_VERTEX_GROUPS = 1 << 0,
  ME_EDIT_MIRROR_Y = 1 << 1, /* unused so far */
  ME_EDIT_MIRROR_Z = 1 << 2, /* unused so far */

  ME_EDIT_PAINT_FACE_SEL = 1 << 3,
  ME_EDIT_MIRROR_TOPO = 1 << 4,
  ME_EDIT_PAINT_VERT_SEL = 1 << 5,
};
ENUM_OPERATORS(eMesh_EditFlag)

/* Helper macro to see if vertex group X mirror is on. */
#define ME_USING_MIRROR_X_VERTEX_GROUPS(_me) \
  (((_me)->editflag & ME_EDIT_MIRROR_VERTEX_GROUPS) && ((_me)->symmetry & ME_SYMMETRY_X))

/* We can't have both flags enabled at once,
 * flags defined in DNA_scene_types.h */
#define ME_EDIT_PAINT_SEL_MODE(_me) \
  (((_me)->editflag & ME_EDIT_PAINT_FACE_SEL) ? SCE_SELECT_FACE : \
   ((_me)->editflag & ME_EDIT_PAINT_VERT_SEL) ? SCE_SELECT_VERTEX : \
                                                0)

/** #Mesh.flag */
enum eMesh_Flag : uint16_t {
  ME_FLAG_UNUSED_0 = 1 << 0,     /* cleared */
  ME_FLAG_UNUSED_1 = 1 << 1,     /* cleared */
  ME_FLAG_DEPRECATED_2 = 1 << 2, /* deprecated */
  /**
   * The UV selection is marked as synchronized.
   * See #BMesh::uv_select_sync_valid for details.
   */
  ME_FLAG_UV_SELECT_SYNC_VALID = 1 << 3,
  ME_FLAG_UNUSED_4 = 1 << 4,     /* cleared */
  ME_AUTOSMOOTH_LEGACY = 1 << 5, /* deprecated */
  ME_FLAG_UNUSED_6 = 1 << 6,     /* cleared */
  ME_FLAG_UNUSED_7 = 1 << 7,     /* cleared */
  ME_REMESH_REPROJECT_ATTRIBUTES = 1 << 8,
  ME_DS_EXPAND = 1 << 9,
  ME_SCULPT_DYNAMIC_TOPOLOGY = 1 << 10,
  /**
   * Used to tag that the mesh has no overlapping topology (see #Mesh::no_overlapping_topology()).
   * Theoretically this is runtime data that could always be recalculated, but since the intent is
   * to improve performance and it only takes one bit, it is stored in the mesh instead.
   */
  ME_NO_OVERLAPPING_TOPOLOGY = 1 << 11,
  ME_FLAG_UNUSED_8 = 1 << 12, /* deprecated */
  ME_REMESH_FIX_POLES = 1 << 13,
  ME_REMESH_REPROJECT_VOLUME = 1 << 14,
  ME_FLAG_UNUSED_9 = 1 << 15, /* deprecated */
};
ENUM_OPERATORS(eMesh_Flag)

#ifdef DNA_DEPRECATED_ALLOW
/** #Mesh.cd_flag */
enum eMesh_CDFlag : char {
  ME_CDFLAG_VERT_BWEIGHT = 1 << 0,
  ME_CDFLAG_EDGE_BWEIGHT = 1 << 1,
  ME_CDFLAG_EDGE_CREASE = 1 << 2,
  ME_CDFLAG_VERT_CREASE = 1 << 3,
};
ENUM_OPERATORS(eMesh_CDFlag)
#endif

/** #Mesh.remesh_mode */
enum eMesh_RemeshMode : char {
  REMESH_VOXEL = 0,
  REMESH_QUAD = 1,
};

/** #SubsurfModifierData.subdivType */
enum MeshSubdivType : int {
  ME_CC_SUBSURF = 0,
  ME_SIMPLE_SUBSURF = 1,
};

/** #Mesh.symmetry */
enum eMeshSymmetryType : char {
  ME_SYMMETRY_X = 1 << 0,
  ME_SYMMETRY_Y = 1 << 1,
  ME_SYMMETRY_Z = 1 << 2,
};
ENUM_OPERATORS(eMeshSymmetryType)

/** #SculptLayerTreeNode.flag of a #SculptLayer. */
enum eSculptLayerFlag : int {
  /** Layer contributes to the combined sculpted result. */
  SCULPT_LAYER_ENABLED = 1 << 0,
  /** Layer is protected from being recorded into or edited. */
  SCULPT_LAYER_LOCKED = 1 << 1,
  /**
   * Layer was enabled but is temporarily hidden by the "Solo Base" mode, which isolates the base
   * shape for direct sculpting (a brush over the composed surface would otherwise bake the layer
   * residual into the base). Ending the mode re-enables exactly the layers carrying this flag.
   */
  SCULPT_LAYER_SOLO_HIDDEN = 1 << 2,
  /**
   * Row selected in the tree view. Pure UI state for multi-item drag and drop reordering; existing
   * layer operators still act on the single active layer (#Mesh::sculpt_layers_active_uid), not on
   * this selection. Never versioned: a new bit in an existing #flag reads as unset from old files.
   */
  SCULPT_LAYER_SELECTED = 1 << 3,
  /**
   * Set when any ancestor group (following the #SculptLayerTreeNode::parent chain) is currently
   * disabled. Maintained by
   * #bke::sculpt_layers::resync_group_state and consulted by #bke::sculpt_layers::effective; the
   * layer's own #SCULPT_LAYER_ENABLED bit is left untouched, so re-enabling the ancestor restores
   * exactly what was visible before — the same convention as #SCULPT_LAYER_SOLO_HIDDEN. Never
   * versioned: a new bit in an existing #flag reads as unset from old files.
   */
  SCULPT_LAYER_GROUP_HIDDEN = 1 << 4,
  /**
   * REC is armed on this layer, so every composite ignores this layer's mask *and* the masks of the
   * folders above it. See #bke::sculpt_layers::rec_exempt_set for why the exemption has to exist.
   *
   * Session state living in a DNA field, which is unusual and deliberate. The composite reaches a
   * layer through an *evaluated* mesh with no #Object and no #SculptSession in scope (see
   * #bke::sculpt_layers::apply_vert_layers_eval), so the answer has to travel with the layer
   * itself; #flag is one of the members the original-to-evaluated copy carries verbatim, exactly as
   * #SculptLayerTreeNode::uid does. An #ID::session_uid cannot be used for this — the copy starts
   * at `sizeof(ID)` and never carries the header — and a process-wide slot cannot either, because
   * it is scoped to nothing and would exempt an unrelated mesh's layer.
   *
   * Never persisted: `group_blend_write_recursive` — the per-node walk
   * #bke::sculpt_layers::tree_blend_write drives, in `blenkernel/intern/sculpt_layers.cc` — strips
   * it from the sanitized copy it writes, and the matching reader (`group_blend_read_children`)
   * clears it again. A file that opened with this bit set would show a layer whose weight map has
   * silently vanished, with nothing in the UI to explain it and no operator that puts it back.
   *
   * Never versioned: a new bit in an existing #flag reads as unset from old files.
   */
  SCULPT_LAYER_REC_EXEMPT = 1 << 5,
  /**
   * The node's own weight mask is switched off: it stays on the node with every painted weight
   * intact, but the composite ignores it. What the user reaches for to compare "with mask" against
   * "without mask" without destroying the weights, which Remove Mask would.
   *
   * Only the node's *own* mask, deliberately unlike #SCULPT_LAYER_REC_EXEMPT, which additionally
   * drops the folder chain above the layer. That wider radius exists because REC stores its delta
   * raw and any surviving factor would distort it; nothing here needs it, and "own mask only"
   * composes — every folder carries this same bit, so switching a layer clear of every mask is a
   * matter of switching it and the folders above it.
   *
   * Persisted, deliberately unlike #SCULPT_LAYER_REC_EXEMPT, which is stripped on write: that bit
   * is invisible to the user, so a file opening with it set would show a mask that silently
   * vanished. This one is set by a visible button whose state the tree row always shows.
   *
   * Never versioned: a new bit in an existing #flag reads as unset from old files, and unset is
   * "the mask is in force" — the behavior every existing file was saved with.
   */
  SCULPT_LAYER_MASK_DISABLED = 1 << 6,
  /**
   * REC is armed on this layer. A mirror of #SculptSession::layers::rec_active kept on the mesh, and
   * it exists for exactly one reason: the session is destroyed on the way out of sculpt mode (see
   * #object_sculpt_mode_exit), so a flag living only there cannot survive a trip through object mode.
   * The entry path reads this bit back to restore REC; everything inside the mode keeps reading the
   * session, which stays the authority for as long as it exists.
   *
   * Written only by #bke::sculpt_layers::rec_armed_set, driven from
   * #ed::sculpt_paint::layers::rec_exemption_refresh alongside #SCULPT_LAYER_REC_EXEMPT. Both bits
   * follow the same "at most one layer carries it" invariant, and the refresh is called from every
   * path that can change either answer, so a bit dropped by an undo restore repairs itself.
   *
   * The one place the two part ways is the mode exit, where there is no session left to mirror:
   * #SCULPT_LAYER_REC_EXEMPT must be cleared there (a composite outside sculpt mode would otherwise
   * silently drop the layer's weight map), while this bit must be left standing — that is the whole
   * feature.
   *
   * Never persisted: stripped on write and cleared again on read, like #SCULPT_LAYER_REC_EXEMPT but
   * for a different reason. Not "the surface would be wrong" — nothing composes from this bit — but
   * "a file must not open with strokes silently recording into a layer the user did not arm in this
   * session". Recording state is scoped to a Blender run, deliberately.
   *
   * Never versioned: a new bit in an existing #flag reads as unset from old files.
   */
  SCULPT_LAYER_REC_ARMED = 1 << 7,
};
ENUM_OPERATORS(eSculptLayerFlag)

/** #SculptLayer.domain (matches the #short storage type). */
enum eSculptLayerDomain : int16_t {
  /** #SculptLayer.data holds one `float3` per mesh vertex. */
  SCULPT_LAYER_DOMAIN_VERT = 0,
  /** #SculptLayer.data holds one `float3` per multires grid point. */
  SCULPT_LAYER_DOMAIN_GRID = 1,
};

/** #SculptLayerTreeNode.type */
enum eSculptLayerTreeNodeType : int8_t {
  SCULPT_LAYER_TREE_NODE_TYPE_LAYER = 0,
  SCULPT_LAYER_TREE_NODE_TYPE_GROUP = 1,
};

/**
 * Folder color tag, drawn in place of the folder icon in the sculpt layer tree. Mirrors
 * #GroupColorTag, with one deliberate difference: zero means "no tag" here, because the field is
 * carved out of #SculptLayerTreeNode's existing padding and therefore reads back as zero from
 * every file written before it existed. Grease Pencil's -1 sentinel would paint all of them.
 */
enum eSculptLayerColorTag : int8_t {
  SCULPT_LAYER_COLOR_NONE = 0,
  SCULPT_LAYER_COLOR_01 = 1,
  SCULPT_LAYER_COLOR_02 = 2,
  SCULPT_LAYER_COLOR_03 = 3,
  SCULPT_LAYER_COLOR_04 = 4,
  SCULPT_LAYER_COLOR_05 = 5,
  SCULPT_LAYER_COLOR_06 = 6,
  SCULPT_LAYER_COLOR_07 = 7,
  SCULPT_LAYER_COLOR_08 = 8,
};

struct SculptLayerGroup;

/**
 * Optional per-element weight map attached to a sculpt layer tree node.
 *
 * Stored sparsely: elements are grouped into fixed-size blocks, and a block that holds a single
 * repeated value keeps only that value. A typical mask covers a small part of the mesh, so most
 * blocks stay uniform — which is what keeps this affordable next to the node's own offset data
 * (a mask costs roughly 1-2% of the layer it modulates).
 *
 * Values are `uint8` mapping 0..255 onto 0..1. The quantization is invisible for a weight, and the
 * byte width is what keeps the composite loop from becoming bandwidth-bound.
 */
typedef struct SculptLayerMask {
  /** Domain element count this mask describes. Used to detect a stale mask, as #SculptLayer::totelem does. */
  int totelem;
  /** Elements per block. Grids use `grid_area`; meshes use #SCULPT_LAYER_MASK_VERT_BLOCK. */
  int block_size;
  int blocks_num;
  int data_num;
  char _pad[8];
  /** `blocks_num` entries of #eSculptLayerMaskBlockKind. */
  int8_t *block_kind;
  /** `blocks_num` entries: the value of a uniform block. Meaningless for a dense block. */
  uint8_t *block_value;
  /** `blocks_num` entries: byte offset into #data, -1 for a uniform block. */
  int *block_offset;
  /** `data_num` bytes: the contents of every dense block, back to back. */
  uint8_t *data;
} SculptLayerMask;

typedef enum eSculptLayerMaskBlockKind {
  SCULPT_LAYER_MASK_BLOCK_UNIFORM = 0,
  SCULPT_LAYER_MASK_BLOCK_DENSE = 1,
} eSculptLayerMaskBlockKind;

#define SCULPT_LAYER_MASK_VERT_BLOCK 4096

/**
 * Fields shared by every row of the sculpt layer tree. Embedded as #SculptLayer::base and
 * #SculptLayerGroup::base, following #GreasePencilLayerTreeNode.
 *
 * #next and #prev must stay the first two members, and this struct must stay the first member of
 * both node types: the blend-file reader relinks a #ListBase by casting each element to #Link and
 * reading `next` at offset 0, and #ListBaseTIterator does the same, so both only ever see this
 * node's links no matter which concrete type the list is declared over.
 */
struct SculptLayerTreeNode {
  SculptLayerTreeNode *next = nullptr, *prev = nullptr;
  /**
   * The folder holding this node, and the inverse of #SculptLayerGroup::children: a node appears in
   * exactly the child list of the group named here. Null only for the root group, which is what
   * terminates every walk up the tree.
   */
  SculptLayerGroup *parent = nullptr;
  /** MAX_NAME. Unique across every node of the mesh tree (see #node_name_ensure_unique). */
  char name[64] = {};
  /** #eSculptLayerFlag for a layer, #eSculptLayerGroupFlag for a group. */
  int flag = 0;
  /** Unique across every node of the mesh. Stable across reordering. */
  int uid = 0;
  /** #eSculptLayerTreeNodeType. */
  int8_t type = SCULPT_LAYER_TREE_NODE_TYPE_LAYER;
  /**
   * #eSculptLayerColorTag. Taken out of the padding below, so the struct size is unchanged and
   * older files load without versioning. Held on the shared node rather than on
   * #SculptLayerGroup so a layer tag stays possible later; only folders expose it today.
   */
  int8_t color_tag = SCULPT_LAYER_COLOR_NONE;
  char _pad[6] = {};
  /**
   * Optional weight map. Null means there is no mask, which is *not* the same as a mask full of
   * ones: a node without a mask skips the masked code paths entirely and costs nothing.
   *
   * Lives on the shared base so folders and layers are served by one implementation.
   */
  struct SculptLayerMask *mask = nullptr;
};

/**
 * A single non-destructive sculpt layer.
 *
 * Stores a per-element displacement delta together with an #influence factor. The final sculpted
 * position of an element is:
 * \code{.unparsed}
 *   position = base + sum_over_enabled_layers(layer.data[i] * layer.influence)
 * \endcode
 * The delta domain (#domain) is per mesh vertex for regular meshes and per multires grid point
 * for multires:
 * - VERT domain: object-space deltas, one `float3` per mesh vertex; the base is the un-layered
 *   vertex position.
 * - GRID domain: *tangent-space* displacement in the #MDisps grid layout, one `float3` per grid
 *   point at subdivision level #level (always the multires top level). The base is #CD_MDISPS
 *   (which never contains layer contributions); layers are composed with the base displacement
 *   at subdivision-surface evaluation time (see `subdiv_displacement_multires.cc`).
 * Held by the #SculptLayerGroup::children list of the folder that contains it (the root group for a
 * top-level layer), persisted in blend files.
 */
struct SculptLayer {
  /** Must stay first: the list links live here, see #SculptLayerTreeNode. */
  SculptLayerTreeNode base;
  /** Influence factor (soft UI range `0..1` shown as 0..100%, hard range `-10..10`). */
  float influence = 1.0f;
  /** Number of elements in #data (mesh vertices, or multires grid points). */
  int totelem = 0;
  /** #eSculptLayerDomain. */
  short domain = 0;
  /**
   * Grid (multires) domain only: subdivision level at which #data is stored. Always equals the
   * mesh's multires top level (`totlvl`); Subdivide / Delete Higher resample the data to keep
   * this invariant. Mirrors #MDisps.level. Unused (0) for the vertex domain.
   */
  short level = 0;
  /**
   * Derived cache: the product of the #influence of every ancestor folder (the running
   * `Pi_ancestors(influence_folder)` of the position model). Not authored state — it is recomputed
   * from the tree by #bke::sculpt_layers::resync_group_state on every tree mutation and on blend
   * read, and #bke::sculpt_layers::effective multiplies it in so the hot path stays a single flat
   * multiply. Written to blend files like any member, but its stored value is ignored on read.
   */
  float group_influence_cached = 1.0f;
  /** `float3[totelem]` array of per-element displacement deltas. May be null (treated as zeros). */
  void *data = nullptr;
};

/** #SculptLayerTreeNode.flag of a #SculptLayerGroup. */
enum eSculptLayerGroupFlag : int {
  /**
   * The group's own enabled state. Descendant layers are not edited when this changes; instead
   * #bke::sculpt_layers::resync_group_state folds the whole ancestor chain into their
   * #SCULPT_LAYER_GROUP_HIDDEN bit, so a group can be re-enabled without having to remember what
   * each descendant's own state was.
   */
  SCULPT_LAYER_GROUP_ENABLED = 1 << 0,
  /** Row selected in the tree view. Pure UI state, same semantics as #SCULPT_LAYER_SELECTED. */
  SCULPT_LAYER_GROUP_SELECTED = 1 << 1,
  /**
   * Tree-view expanded (as opposed to collapsed) state. Persisted rather than kept as view-only
   * runtime state — matching the #GP_LAYER_TREE_NODE_EXPANDED precedent — so it survives a file
   * save and stays consistent across every window showing the tree. New groups start expanded.
   */
  SCULPT_LAYER_GROUP_EXPANDED = 1 << 2,
  /**
   * The folder counterpart of #SCULPT_LAYER_MASK_DISABLED, and deliberately the *same numeric
   * value*: it lets #bke::sculpt_layers::mask_enabled answer for a node of either kind without a
   * type test, exactly as #SculptLayerTreeNode::mask serves both with one implementation. A
   * #BLI_STATIC_ASSERT next to that function holds the two spellings together.
   *
   * Switches off this folder's own mask, so every layer below it stops being attenuated by it. The
   * masks of folders further up are unaffected and keep applying.
   */
  SCULPT_LAYER_GROUP_MASK_DISABLED = 1 << 6,
};
ENUM_OPERATORS(eSculptLayerGroupFlag)

/**
 * A folder in the sculpt layer tree, persisted in blend files. Every group is reachable from
 * #Mesh::sculpt_layer_root by following #children.
 *
 * Groups carry no displacement data of their own, so the model stays purely additive and
 * order-independent: `position = base + sum(layer.data[i] * effective(layer))`. A group affects that
 * sum only through per-layer *weights*, never through a term of its own — its #influence folds into
 * every descendant layer's #effective as one factor of the ancestor product, and its enabled bit
 * feeds the boolean visibility cascade (both maintained by #bke::sculpt_layers::resync_group_state).
 */
struct SculptLayerGroup {
  /**
   * Must stay first: the list links live here, see #SculptLayerTreeNode.
   *
   * NOTE: #SculptLayerTreeNode::flag defaults to 0 here, not to
   * `SCULPT_LAYER_GROUP_ENABLED | SCULPT_LAYER_GROUP_EXPANDED` as it did while it was a member
   * of this struct — a default member initializer cannot reach into an embedded base. Every
   * group is created through #bke::sculpt_layers::group_add, which sets both bits explicitly.
   * The root group is the one exception: it is never created through #group_add and keeps uid 0
   * (see #Mesh::sculpt_layer_root).
   */
  SculptLayerTreeNode base;
  /**
   * Influence multiplier this folder contributes to the cascade (soft UI range `0..1`, hard range
   * `-10..10`, default 1). It folds into every descendant layer's weight as one factor of
   * `Pi_ancestors(influence_folder)`; #bke::sculpt_layers::resync_group_state bakes that product onto
   * each layer's #SculptLayer::group_influence_cached. A folder dialled to 0 is not the same as a
   * disabled folder: the enabled bit stays a separate #SCULPT_LAYER_GROUP_HIDDEN cascade.
   */
  float influence = 1.0f;
  char _pad[4] = {};
  /**
   * The nodes this folder holds, of both kinds interleaved, owned by this group.
   *
   * This list *is* the sibling order, which is why interleaving needs no order field: a folder
   * dropped between two layers is simply linked between them. Declared over the shared node rather
   * than over either concrete type because it holds both; dispatch on #SculptLayerTreeNode::type
   * and reinterpret to the concrete struct (legal because #base is its first member).
   */
  ListBaseT<SculptLayerTreeNode> children = {nullptr, nullptr};
  /**
   * Runtime-only cache of this folder's flat layer span (see
   * #bke::sculpt_layers::SculptLayerGroupRuntime). Never persisted: it is written to the file as a
   * null and rebuilt on read, and every group-creating path allocates it through
   * #bke::sculpt_layers::group_runtime_ensure.
   *
   * A *pointer* rather than the runtime by value, following #GreasePencilLayerTreeGroup::runtime:
   * the runtime holds a #CacheMutex and a #Vector, neither of which is trivially destructible,
   * whereas a raw pointer is — and no destructor ever runs on a group (see the allocation notes in
   * `BKE_sculpt_layers.hh`, and the static asserts in `blenkernel/intern/sculpt_layers.cc` that
   * enforce this).
   */
  bke::sculpt_layers::SculptLayerGroupRuntime *runtime = nullptr;
};

struct Mesh {
#ifdef __cplusplus
  DNA_DEFINE_CXX_METHODS(Mesh)
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_ME;
#endif

  ID id;
  /** Animation data (must be immediately after id for utilities to use it). */
  struct AnimData *adt = nullptr;

  struct Key *key = nullptr;

  /**
   * An array of materials, with length #totcol. These can be overridden by material slots
   * on #Object. Indices in the "material_index" attribute control which material is used for every
   * face.
   */
  struct Material **mat = nullptr;

  /** The number of vertices in the mesh, and the size of #vert_data. */
  int verts_num = 0;
  /** The number of edges in the mesh, and the size of #edge_data. */
  int edges_num = 0;
  /** The number of faces in the mesh, and the size of #face_data. */
  int faces_num = 0;
  /** The number of face corners in the mesh, and the size of #corner_data. */
  int corners_num = 0;

  /**
   * Array owned by mesh. See #Mesh::faces() and #OffsetIndices.
   *
   * This array is shared based on the bke::MeshRuntime::face_offsets_sharing_info.
   * Avoid accessing directly when possible.
   */
  int *face_offset_indices = nullptr;

  /**
   * Vertex, edge, face, and corner generic attributes.
   */
  struct AttributeStorage attribute_storage;

  /** Store for non-generic layer data on each domain. */
  CustomData vert_data;
  CustomData edge_data;
  CustomData face_data;
  CustomData corner_data;

  /**
   * List of vertex group (#bDeformGroup) names and flags only. Actual weights are stored in dvert.
   * \note This pointer is for convenient access to the #CD_MDEFORMVERT layer in #vert_data.
   */
  ListBaseT<bDeformGroup> vertex_group_names = {nullptr, nullptr};
  /** The active index in the #vertex_group_names list. */
  int vertex_group_active_index = 0;
  /**
   * #SculptLayerTreeNode::uid of the active sculpt layer, or 0 when there is none (uids start at 1).
   *
   * Identifies the active layer rather than pointing at a position, because a position is only
   * meaningful for as long as the list does not change shape underneath it.
   *
   * This field occupies what used to be explicit padding: `vertex_group_active_index` (4 bytes)
   * must be followed by 4 more bytes so that #sculpt_layer_root (an 8-byte pointer) starts on an
   * 8-byte boundary, as the DNA system requires.
   *
   * NOTE: 0 means "no active layer", it does NOT resolve to the root group the way it does for
   * #bke::sculpt_layers::node_find_by_uid. See #bke::sculpt_layers::active_get, which is the only
   * correct way to read this field.
   */
  int sculpt_layers_active_uid = 0;

  /**
   * Root of the non-destructive sculpt layer tree (#SculptLayer / #SculptLayerGroup), owned by the
   * mesh and *always allocated* — see #bke::sculpt_layers::root_group_ensure, which every path that
   * produces a mesh (ID creation and the blend-file reader) goes through.
   *
   * The root is a real #SculptLayerGroup rather than a bare list so that a folder and "the top
   * level" are the same type: every walk, insert and reparent then has one case instead of a root
   * special case. It is never drawn as a row and keeps uid 0. Follows the #GreasePencil
   * ::root_group_ptr precedent.
   */
  SculptLayerGroup *sculpt_layer_root = nullptr;

  /**
   * The index of the active attribute in the UI. The attribute list is a combination of the
   * generic type attributes from vertex, edge, face, and corner custom data.
   *
   * Set to -1 when none is active.
   */
  int attributes_active_index = 0;
  /**
   * Explicit padding so that the following #mselect pointer stays 8-byte aligned. Needed because
   * #attributes_active_index lost the neighbouring int it used to pair up with (the deprecated
   * `sculpt_layers_active_index`); makesdna demands explicit padding and never inserts any.
   */
  char _pad2[4] = {};

  /**
   * This array represents the selection order when the user manually picks elements in edit-mode,
   * some tools take advantage of this information. All elements in this array are expected to be
   * selected, see #BKE_mesh_mselect_validate which ensures this. For procedurally created meshes,
   * this is generally empty (selections are stored as boolean attributes in the corresponding
   * custom data).
   */
  struct MSelect *mselect = nullptr;

  /** The length of the #mselect array. */
  int totselect = 0;

  /**
   * In most cases the last selected element (see #mselect) represents the active element.
   * For faces we make an exception and store the active face separately so it can be active
   * even when no faces are selected. This is done to prevent flickering in the material properties
   * and UV Editor which base the content they display on the current material which is controlled
   * by the active face.
   *
   * \note This is mainly stored for use in edit-mode.
   */
  int act_face = 0;

  /**
   * An optional mesh owned elsewhere (by #Main) that can be used to override
   * the texture space #loc and #size.
   * \note Vertex indices should be aligned for this to work usefully.
   */
  struct Mesh *texcomesh = nullptr;

  /** Texture space location and size, used for procedural coordinates when rendering. */
  float texspace_location[3] = {};
  float texspace_size[3] = {1.0f, 1.0f, 1.0f};
  char texspace_flag = ME_TEXSPACE_FLAG_AUTO;

  /** Various flags used when editing the mesh. */
  eMesh_EditFlag editflag = ME_EDIT_MIRROR_VERTEX_GROUPS;
  /** Mostly more flags used when editing or displaying the mesh. */
  eMesh_Flag flag = ME_REMESH_REPROJECT_VOLUME | ME_REMESH_REPROJECT_ATTRIBUTES;

  DNA_DEPRECATED float smoothresh_legacy = 0;

  /** Per-mesh settings for voxel remesh. */
  float remesh_voxel_size = 0.1f;
  float remesh_voxel_adaptivity = 0.0f;

  int face_sets_color_seed = 0;
  /* Stores the initial Face Set to be rendered white. This way the overlay can be enabled by
   * default and Face Sets can be used without affecting the color of the mesh. */
  int face_sets_color_default = 1;

  /** The color attribute currently selected in the list and edited by a user. */
  char *active_color_attribute = nullptr;
  /** The color attribute used by default (i.e. for rendering) if no name is given explicitly. */
  char *default_color_attribute = nullptr;

  /**
   * The UV map currently selected in the list and edited by a user.
   * \note While the edit BMesh (`edit_mesh.bm`) is non null, that is the source of truth instead.
   * Typical access should be through #Mesh::active_uv_map_name() rather than direct.
   */
  char *active_uv_map_attribute = nullptr;
  /**
   * The UV map used by default (i.e. for rendering) if no name is given explicitly.
   * \note While the edit BMesh (`edit_mesh.bm`) is non null, that is the source of truth instead.
   * Typical access should be through #Mesh::default_uv_map_name() rather than direct.
   */
  char *default_uv_map_attribute = nullptr;
  /** UV map selection used for texture paint masking. */
  char *stencil_uv_map_attribute = nullptr;
  /** UV map selection used for texture paint clone brush. */
  char *clone_uv_map_attribute = nullptr;

  /**
   * User-defined symmetry flag that causes editing operations to maintain
   * symmetrical geometry. Supported by operations such as transform and weight-painting.
   */
  eMeshSymmetryType symmetry = {};

  /** Choice between different remesh methods in the UI. */
  eMesh_RemeshMode remesh_mode = REMESH_VOXEL;

  /** The length of the #mat array. */
  short totcol = 0;

  /**
   * Deprecated flag for choosing whether to store specific custom data that was built into #Mesh
   * structs in edit mode. Replaced by separating that data to separate layers. Kept for forward
   * and backwards compatibility.
   */
  DNA_DEPRECATED char cd_flag = 0;
  DNA_DEPRECATED char subdiv = 0;
  DNA_DEPRECATED char subdivr = 0;
  DNA_DEPRECATED char subsurftype = 0;

  /** Deprecated pointer to mesh polygons, kept for forward compatibility. */
  DNA_DEPRECATED struct MPoly *mpoly = nullptr;
  /** Deprecated pointer to face corners, kept for forward compatibility. */
  DNA_DEPRECATED struct MLoop *mloop = nullptr;

  /** Deprecated array of mesh vertices, kept for reading old files, now stored in #CustomData. */
  DNA_DEPRECATED struct MVert *mvert = nullptr;
  /** Deprecated array of mesh edges, kept for reading old files, now stored in #CustomData. */
  DNA_DEPRECATED struct MEdge *medge = nullptr;
  /** Deprecated "Vertex group" data. Kept for reading old files, now stored in #CustomData. */
  DNA_DEPRECATED struct MDeformVert *dvert = nullptr;
  /** Deprecated runtime data for tessellation face UVs and texture, kept for reading old files. */
  DNA_DEPRECATED struct MTFace *mtface = nullptr;
  /** Deprecated, use mtface. */
  DNA_DEPRECATED struct TFace *tface = nullptr;
  /** Deprecated array of colors for the tessellated faces, kept for reading old files. */
  DNA_DEPRECATED struct MCol *mcol = nullptr;
  /** Deprecated face storage (quads & triangles only). Kept for reading old files. */
  DNA_DEPRECATED struct MFace *mface = nullptr;

  /**
   * Deprecated storage of old faces (only triangles or quads).
   *
   * \note This would be marked deprecated, however the particles still use this at run-time
   * for placing particles on the mesh (something which should be eventually upgraded).
   */
  CustomData fdata_legacy;
  /* Deprecated size of #fdata. */
  int totface_legacy = 0;

  char _pad1 = {};
  int8_t radial_symmetry[3] = {1, 1, 1};

  /**
   * Data that isn't saved in files, including caches of derived data, temporary data to improve
   * the editing experience, etc. The struct is created when reading files and can be accessed
   * without null checks, with the exception of some temporary meshes which should allocate and
   * free the data if they are passed to functions that expect run-time data.
   */
  bke::MeshRuntime *runtime = nullptr;
#ifdef __cplusplus
  /**
   * Array of vertex positions. Edges and face corners are defined by indices into this array.
   */
  Span<float3> vert_positions() const;
  /** Write access to vertex data. */
  MutableSpan<float3> vert_positions_for_write();
  /**
   * Array of edges, containing vertex indices, stored in the ".edge_verts" attribute. For simple
   * triangle or quad meshes, edges could be calculated from the face and #corner_edge arrays.
   * However, edges need to be stored explicitly for edge domain attributes and to support loose
   * edges that aren't connected to faces.
   */
  Span<int2> edges() const;
  /** Write access to edge data. */
  MutableSpan<int2> edges_for_write();
  /**
   * Face topology information (using the same internal data as #face_offsets()). Each face is a
   * contiguous chunk of face corners represented as an #IndexRange. Each face can be used to slice
   * the #corner_verts or #corner_edges arrays to find the vertices or edges that each face uses.
   */
  OffsetIndices<int> faces() const;
  /**
   * Return an array containing the first corner of each face. and the size of the face encoded as
   * the next offset. The total number of corners is the final value, and the first value is always
   * zero. May be empty if there are no faces.
   */
  Span<int> face_offsets() const;
  /** Write access to #face_offsets data. */
  MutableSpan<int> face_offsets_for_write();

  /**
   * Array of vertices for every face corner, stored in the ".corner_vert" integer attribute.
   * For example, the vertices in a face can be retrieved with the #slice method:
   * \code{.cc}
   * const Span<int> face_verts = corner_verts.slice(face);
   * \endcode
   * This span can often be passed as an argument in lieu of a face and the entire corner verts
   * array.
   */
  Span<int> corner_verts() const;
  /** Write access to the #corner_verts data. */
  MutableSpan<int> corner_verts_for_write();

  /**
   * Array of edges following every face corner traveling around each face, stored in the
   * ".corner_edge" attribute. The array sliced the same way as the #corner_verts data. The edge
   * previous to a corner must be accessed with the index of the previous face corner.
   */
  Span<int> corner_edges() const;
  /** Write access to the #corner_edges data. */
  MutableSpan<int> corner_edges_for_write();

  bke::AttributeAccessor attributes() const;
  bke::MutableAttributeAccessor attributes_for_write();

  /**
   * The names of all UV map attributes, in the order of the internal storage.
   * This is useful when UV maps are referenced by index.
   *
   * \warning Adding or removing attributes will invalidate the referenced memory.
   */
  VectorSet<StringRefNull> uv_map_names() const;

  /** The name of the active UV map attribute, if any. */
  StringRefNull active_uv_map_name() const;
  /** The name of the default UV map (e.g. for rendering) attribute, if any. */
  StringRefNull default_uv_map_name() const;
  /** The active UV map name, falling back to the default if no active map is set. */
  StringRefNull active_or_default_uv_map_name() const;

  void uv_maps_active_set(StringRef name);
  void uv_maps_default_set(StringRef name);

  /**
   * Vertex group data, encoded as an array of indices and weights for every vertex.
   * \warning: May be empty.
   */
  Span<MDeformVert> deform_verts() const;
  /** Write access to vertex group data. */
  MutableSpan<MDeformVert> deform_verts_for_write();

  /**
   * Cached triangulation of mesh faces, depending on the face topology and the vertex positions.
   */
  Span<int3> corner_tris() const;

  /**
   * A map containing the face index that each cached triangle from #Mesh::corner_tris() came from.
   */
  Span<int> corner_tri_faces() const;

  /**
   * Calculate the largest and smallest position values of vertices.
   */
  std::optional<Bounds<float3>> bounds_min_max() const;

  /** Set cached mesh bounds to a known-correct value to avoid their lazy calculation later on. */
  void bounds_set_eager(const Bounds<float3> &bounds);

  /** Get the largest material index used by the mesh or `nullopt` if it has no faces. */
  std::optional<int> material_index_max() const;

  /** Get all the material indices actually used by the mesh. */
  const VectorSet<int> &material_indices_used() const;

  /**
   * Cached map containing the index of the face using each face corner.
   */
  Span<int> corner_to_face_map() const;
  /**
   * Offsets per vertex used to slice arrays containing data for connected faces or face corners.
   */
  OffsetIndices<int> vert_to_face_map_offsets() const;
  /**
   * Cached map from each vertex to the corners using it.
   */
  GroupedSpan<int> vert_to_corner_map() const;
  /**
   * Cached map from each vertex to the faces using it.
   */
  GroupedSpan<int> vert_to_face_map() const;

  /**
   * Cached information about loose edges, calculated lazily when necessary.
   */
  const IndexMask &loose_edges() const;
  /**
   * Cached information about vertices that aren't used by any edges.
   */
  const IndexMask &loose_verts() const;
  /**
   * Cached information about vertices that aren't used by faces (but may be used by loose edges).
   */
  const IndexMask &verts_no_face() const;
  /**
   * True if the mesh has no faces or edges "inside" of other faces. Those edges or faces would
   * reuse a subset of the vertices of a face. Knowing the mesh is "clean" or "good" can mean
   * algorithms can skip checking for duplicate edges and faces when they create new edges and
   * faces inside of faces.
   *
   * \note This is just a hint, so there still might be no overlapping geometry if it is false.
   */
  bool no_overlapping_topology() const;

  /**
   * Explicitly set the cached number of loose edges to zero. This can improve performance
   * later on, because finding loose edges lazily can be skipped entirely.
   *
   * \note To allow setting this status on meshes without changing them, this does not tag the
   * cache dirty. If the mesh was changed first, the relevant dirty tags should be called first.
   */
  void tag_loose_edges_none() const;
  /**
   * Set the number of vertices not connected to edges to zero. Similar to #tag_loose_edges_none().
   * There may still be vertices only used by loose edges though.
   *
   * \note If both #tag_loose_edges_none() and #tag_loose_verts_none() are called,
   * all vertices are used by faces, so #verts_no_faces() will be tagged empty as well.
   */
  void tag_loose_verts_none() const;
  /** Set the #no_overlapping_topology() hint when the mesh is "clean." */
  void tag_overlapping_none();

  /**
   * Returns the least complex attribute domain needed to store normals encoding all relevant mesh
   * data. When all edges or faces are sharp, face normals are enough. When all are smooth, vertex
   * normals are enough. With a combination of sharp and smooth, normals may be "split",
   * requiring face corner storage.
   *
   * When possible, it's preferred to use face normals over vertex normals and vertex normals over
   * face corner normals, since there is a 2-4x performance cost increase for each more complex
   * domain.
   */
  bke::MeshNormalDomain normals_domain() const;
  /**
   * Normal direction of faces, defined by positions and the winding direction of face corners.
   */
  Span<float3> face_normals() const;
  Span<float3> face_normals_true() const;
  /**
   * Normal direction of vertices, defined as the weighted average of face normals
   * surrounding each vertex and the normalized position for loose vertices.
   */
  Span<float3> vert_normals() const;
  Span<float3> vert_normals_true() const;
  /**
   * Normal direction at each face corner. Defined by a combination of face normals, vertex
   * normals, the `sharp_edge` and `sharp_face` attributes, and potentially by custom normals.
   *
   * \note Because of the large memory requirements of storing normals per face corner, prefer
   * using #face_normals() or #vert_normals() when possible (see #normals_domain()). For this
   * reason, the "true" face corner normals aren't cached, since they're just the same as the
   * corresponding face normals.
   */
  Span<float3> corner_normals() const;

  bke::BVHTreeFromMesh bvh_verts() const;
  bke::BVHTreeFromMesh bvh_edges() const;
  bke::BVHTreeFromMesh bvh_legacy_faces() const;
  bke::BVHTreeFromMesh bvh_corner_tris() const;
  bke::BVHTreeFromMesh bvh_corner_tris_no_hidden() const;
  bke::BVHTreeFromMesh bvh_loose_verts() const;
  bke::BVHTreeFromMesh bvh_loose_edges() const;
  bke::BVHTreeFromMesh bvh_loose_no_hidden_verts() const;
  bke::BVHTreeFromMesh bvh_loose_no_hidden_edges() const;

  void count_memory(MemoryCounter &memory) const;

  /** Call after changing vertex positions to tag lazily calculated caches for recomputation. */
  void tag_positions_changed();
  /** Call after moving every mesh vertex by the same translation. */
  void tag_positions_changed_uniformly();
  /** Like #tag_positions_changed but doesn't tag normals; they must be updated separately. */
  void tag_positions_changed_no_normals();
  /** Call when changing "sharp_face" or "sharp_edge" data. */
  void tag_sharpness_changed();
  /** Call when changing "custom_normal" data. */
  void tag_custom_normals_changed();
  /** Call when face vertex order has changed but positions and faces haven't changed. */
  void tag_face_winding_changed();
  /** Call when new edges and vertices have been created but vertices and faces haven't changed. */
  void tag_edges_split();
  /** Call for topology updates not described by other update tags. */
  void tag_topology_changed();
  /** Call when changing the ".hide_vert", ".hide_edge", or ".hide_poly" attributes. */
  void tag_visibility_changed();
  /** Call when changing the "material_index" attribute. */
  void tag_material_index_changed();
#endif
};

/* deprecated by MTFace, only here for file reading */

#ifdef DNA_DEPRECATED_ALLOW
struct TFace {
  DNA_DEFINE_CXX_METHODS(TFace)

  /** The faces image for the active UVLayer. */
  void *tpage = nullptr;
  float uv[4][2] = {};
  unsigned int col[4] = {};
  char flag = 0, transp = 0;
  short mode = 0, tile = 0, unwrap = 0;
};
#endif

#define MESH_MAX_VERTS 2000000000L

}  // namespace blender
