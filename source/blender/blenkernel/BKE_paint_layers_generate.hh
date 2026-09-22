/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstdint>

#include "BLI_function_ref.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"
#include "DNA_uuid_types.h"

namespace blender {

struct Main;
struct Material;
struct MaterialPaintLayer;
struct PaintLayersRegenCache;
struct PaintModeSettings;
struct bNode;
struct bNodeTree;
enum class PaintLayerMaterialMode : int8_t;

/**
 * The generator that turns a layered material's DNA description (`Material::paint_layers`) into the
 * node tree that renders it.
 *
 * `paint_layers_tree_build` is a pure function over an empty tree: it never touches #Main or the
 * context, so the same build can later run at evaluation time. `BKE_paint_layers_regenerate` is the
 * #Main-side wrapper that owns the generated group and wires it into the material.
 *
 * Topology and values are separated (design B-3): the animatable values (`opacity`, `enabled`, a
 * Fill layer's constant) are inputs of each layer group's own interface, and their current values
 * live on that group's instance node in its parent tree (the root generated tree for a top-level
 * layer, a folder's tree for a child). `BKE_paint_layers_values_sync` walks the instances and
 * copies the values from the description; the material's evaluation calls it, so animation never
 * rebuilds topology.
 */

/**
 * What `paint_layers_tree_build` needs that is not a pure function of the description: the shared
 * Normal Combine group, and one empty node group per layer, both of which have to be created in
 * #Main first.
 */
struct PaintLayersBuildContext {
  /** Shared group an instance of which combines two tangent-space normal maps, or null. */
  bNodeTree *normal_combine_group = nullptr;
  /**
   * Hands out the empty node group that holds \a layer's own nodes, named and marked as that
   * layer's. Required: the build puts every layer in a group of its own, so a caller that cannot
   * create data-blocks cannot build.
   */
  FunctionRef<bNodeTree *(const MaterialPaintLayer &layer)> layer_tree_get;
  /**
   * Optional: whether the tree #layer_tree_get just handed out for \a layer was reused untouched,
   * i.e. its stored topology hash matches the description. When true, the build reuses the group's
   * existing nodes and interface, restores only the parent-side links, and never runs the row
   * builder for that layer. A caller that always makes fresh trees (a pure build) leaves this
   * empty, so every layer is built.
   */
  FunctionRef<bool(const MaterialPaintLayer &layer)> layer_tree_unchanged;
  /**
   * Optional: hands out the wrapper group of a Material row's source, or null when the source
   * cannot be wrapped. The caller creates them, since the factory needs #Main and a main-thread
   * call; a pure build leaves this empty and a Material row in
   * #PaintLayerMaterialMode::SourceGroup falls back to its baked maps.
   */
  FunctionRef<bNodeTree *(const Material &source)> source_group_get;
  /**
   * Optional: what the regeneration that runs this build has already learned (a source's resolve, a
   * row's mode, the Pass Through scales). A caller that is not a regeneration leaves it null and
   * every question is answered afresh.
   */
  const PaintLayersRegenCache *regen_cache = nullptr;
};

/**
 * A hash of everything about \a layer that decides the *nodes* of its group: kind and source,
 * per-channel participation and blend, which maps stand in (by `session_uid`), the effects and
 * mask items in order, and a folder's children. Values that only feed group inputs -- opacity, a
 * Fill constant, the fill colour -- are deliberately excluded, so editing them never rebuilds
 * topology. \a wired_channels is the set the material wires at all, so a channel appearing or
 * disappearing invalidates every group.
 */
uint64_t paint_layers_layer_topology_hash(const Material &ma,
                                          const MaterialPaintLayer &layer,
                                          Span<int> wired_channels,
                                          const PaintLayersRegenCache *cache = nullptr);

/**
 * Fill an empty \a tree with the stack \a ma's description stands for, bottom to top, one output
 * per channel the description wires. Pure: the caller owns \a tree and its lifetime, and \a ctx
 * only ever names data-blocks the caller already owns.
 *
 * Each row is built inside a node group of its own (see #PaintLayersBuildContext::layer_tree_get):
 * the tree gets one instance per row and the result chain, so the root stays readable. The root's
 * interface -- the value inputs and the `Result <channel>` outputs -- is unchanged. Pure: the
 * caller owns \a tree and its lifetime, and \a ctx only ever names data-blocks the caller owns.
 *
 * A Custom (material) layer has no generated representation yet (phase 4) and is skipped.
 */
void paint_layers_tree_build(const Material &ma,
                             bNodeTree &tree,
                             const PaintLayersBuildContext &ctx = {});

/** Why a source material could not be wrapped into a live channel group. */
enum class PaintLayersSourceGroupRefusal : int8_t {
  None = 0,
  /** The source material has no node tree at all. */
  NoNodeTree,
  /** No Principled BSDF feeds the source's active Material Output. */
  NoPrincipled,
  /**
   * The Principled lives inside a nested group. Routing its sockets out to the wrapper's root is
   * not supported in this version, so the row must stay on its baked maps.
   */
  PrincipledInGroup,
  /** The owner and the source are the same material. */
  SelfReference,
  /** The wrapper could not be assembled (an internal build failure). */
  BuildFailed,
  /** The row was live but the material's sampler budget forced it onto its baked maps. */
  TooManyTextures,
};

/** Non-fatal notes the generator produced while regenerating a material. */
struct PaintLayersRegenerateReport {
  /** A group input the generator owns had been detached and was linked again. */
  bool restored_inputs = false;
  /** A foreign (hand-built) node group was found where the generated tree belongs; it was left
   * alone and a fresh tree was created instead. */
  bool replaced_foreign_tree = false;
  /** The material has no Principled to route the stack into (and the Surface input is taken by a
   * different shader), so only the group was built. */
  bool no_principled = false;
  /** A Principled BSDF was created to route the stack into a free Surface input. */
  bool created_principled = false;
  /** An owned input already carried a foreign link in Unlocked mode, so it was left alone. */
  bool skipped_foreign_inputs = false;
  /** The first source wrapper that could not be built, so a UI can say why a Material row is
   * showing its baked maps instead of its live source. None when every wrapper was built. */
  PaintLayersSourceGroupRefusal source_group_refusal = PaintLayersSourceGroupRefusal::None;
  /** The depsgraph relations had to be rebuilt: a generated group or a Material row's source
   * material appeared or disappeared since the last regeneration. */
  bool relations_changed = false;
  /**
   * The sampler count is still above the runtime budget after every fallback, so EEVEE will likely
   * refuse the material. The graph is left as it is -- dropping rows would change the picture
   * silently -- and the caller is expected to warn.
   */
  bool sampler_budget_exceeded = false;
  /**
   * The sampler count the description estimates for the built graph, before any fallback. Tests and
   * the calibration line compare it with #BKE_paint_layers_sampler_count of the finished material.
   */
  int sampler_estimate = 0;

  /** Per Material row: its marker, the mode it was built in and, for SourceGroup, why the
   * wrapper was refused. Diagnostic: the mode is otherwise invisible from outside. */
  struct MaterialRowModeReport {
    bUUID marker;
    char name[64];
    PaintLayerMaterialMode mode;
    PaintLayersSourceGroupRefusal refusal;
    bool wrapper_built;
    /** How many nested groups the Principled was reached through. */
    int group_depth;
    /** The source material's name, or empty when the row reads none. */
    char source_name[64];
    /** The source material's `session_uid`, or zero when the row reads none. */
    uint32_t source_uid;
    /** The row is the active one, or an ancestor of it, so its source stays live. */
    bool deferred;
  };
  Vector<MaterialRowModeReport> material_rows;
};

/**
 * Rebuild `ma.paint_layers_tree` from `ma.paint_layers` and wire it into the material.
 *
 * Creates the generated group when the material has none, overwrites the one it owns (matched by
 * #Material::paint_layers_owner_uid) when it has one, and leaves a foreign tree alone. The group is
 * instantiated in the material's embedded node tree (the instance node is found by its marker) and
 * its channel outputs are routed into the Principled BSDF -- Normal through a Normal Map, Height
 * through a Bump. Current values are synced from the description last.
 *
 * Never called from draw or evaluation (rule K-1); the edit API and the single scheduling point are
 * the callers. Clears #MA_PAINT_LAYERS_REGEN on success.
 *
 * \return false when \a ma is not a layered material or has no embedded node tree.
 */
bool BKE_paint_layers_regenerate(Main &bmain,
                                 Material &ma,
                                 PaintLayersRegenerateReport *r_report = nullptr);

/**
 * Copy the animatable values of \a ma's description into the instance node's input sockets, in \a
 * ma's own (possibly evaluated) embedded tree. Does nothing when the material has no generated
 * tree. Topology is untouched, so this is cheap enough for the material evaluation.
 */
void BKE_paint_layers_values_sync(Material &ma);

/**
 * Regenerate every layered material of \a bmain that needs it -- #MA_PAINT_LAYERS_REGEN is set or
 * the generated tree is missing -- and nothing else.
 *
 * This is the single scheduling point of rule K-1: it runs on the main thread, once per event loop
 * iteration, *before* the depsgraph update, never from draw, depsgraph flush or evaluation. A
 * mutator only marks and tags; the rebuild happens here, so a composite edit that issues several
 * tags still generates once from its final state, and a new ID is never created while the
 * depsgraph relations of the current pass are already built.
 */
void BKE_paint_layers_regenerate_tagged(Main &bmain,
                                        const PaintModeSettings *paint_mode = nullptr);

/**
 * Take ownership of the generated tree for a copied material.
 *
 * A regular copy gets its own deep copy of the tree (a copy that shared it would change the
 * original's stack as soon as either is edited), a fresh owner uid, and its instance node
 * re-pointed at the copy. An evaluated (COW) copy only carries the pointer; the depsgraph relinks
 * it. Called from `material_copy_data` while the destination is still being assembled, so it must
 * not assume the copy's node tree is final.
 */
void BKE_paint_layers_generate_copy_data(Main *bmain,
                                         Material &ma_dst,
                                         const Material &ma_src,
                                         const int copy_flag);

/**
 * The node group that exposes \a source's Principled channels as COLOR:<CHANNEL> outputs.
 *
 * A live Material row instances this instead of its baked maps, so any node graph the source is
 * built from is shown as it is. The group is a copy of the source's tree with its Material Output
 * replaced by a Group Output; it is cached per (owner, source) and rebuilt only when the source's
 * tree hash moves.
 *
 * \return null when the source cannot be expressed per channel -- see
 *         #PaintLayersSourceGroupRefusal -- and the row must stay on its baked maps.
 *
 * \param r_changed: set when the wrapper was created or rebuilt.
 * \param r_values_synced: set when an existing wrapper had the source's values copied into it.
 */
bNodeTree *BKE_paint_layers_source_group_ensure(Main &bmain,
                                                Material &owner,
                                                Material &source,
                                                PaintLayersSourceGroupRefusal &r_refusal,
                                                bool *r_changed = nullptr,
                                                bool *r_values_synced = nullptr);

/**
 * The state hash of \a ma's node tree, or zero when it has none. The recursive form includes every
 * group the tree reaches, so an edit inside a nested group moves this too. Shared with the bake
 * hash and the source-group cache, so the formula lives in one place.
 */
uint64_t BKE_paint_layers_source_material_tree_hash(const Material &ma);

/**
 * The topology-only hash of \a ma's node tree, or zero when it has none: node types and names,
 * links, group IDs and interfaces, but no values. Used by the source-group wrapper to tell a
 * rebuild (topology) from a value sync; the bake keeps the value-sensitive
 * #BKE_paint_layers_source_material_tree_hash.
 */
uint64_t BKE_paint_layers_source_material_topology_hash(const Material &ma);

/**
 * A content hash of one node's DNA storage, used to tell whether a wrapper copy already holds the
 * source's storage. #CurveMapping is hashed by its points rather than its pointers.
 */
uint64_t BKE_paint_layers_source_node_storage_hash(const bNode &node);

/**
 * Forget that \a marker was left out of the generated graph, returning whether it was recorded.
 * Called when a row is enabled: a recorded row is absent, so bringing it back is the one rebuild
 * that its re-enable needs.
 */
bool BKE_paint_layers_row_removed_clear(Material &ma, const bUUID &marker);

/** Force the next regenerate to rebuild the root even if its topology hash still matches. */
void BKE_paint_layers_root_hash_invalidate(Material &ma);

/* -------------------------------------------------------------------- */
/** \name Sampler budget
 *
 * EEVEE draws a whole material with one shader whose sampler count is bounded by
 * `GPU_max_textures()`. Blenkernel never calls the GPU, so the budget is a runtime value the
 * editor sets once the GPU is initialized; zero means "do not check" (background mode, tests).
 * \{ */

/**
 * Sampler slots EEVEE reserves for its own textures before it hands the rest to material textures.
 *
 * #eevee::SlotAllocator counts the *occupied* bits of the engine's create-infos, so the budget must
 * subtract a count, not the highest slot index. The generator can run under any material pipeline,
 * so the number is the most any surface pipeline can reserve. The EEVEE slot table
 * (`eevee_defines.hh`: `RBUFS_UTILITY_TEX_SLOT`=2 .. `GBUF_HEADER_TEX_SLOT`=19) declares 18 texture
 * slots; slots 0 and 1 belong to `draw_gpencil` and are never reserved by a material. The worst-case
 * deferred material fills all of them, so 18 is both the maximum a pipeline can occupy and the safe
 * (slightly conservative) value for a lighter forward pass. The `over=` field of the
 * `paint layers samplers:` line is the calibration signal when a driver reports a smaller value.
 */
constexpr int PAINT_LAYERS_EEVEE_RESERVED_SAMPLERS = 18;

/**
 * Set the runtime sampler budget. `budget` is the number of samplers material textures may use
 * (`GPU_max_textures() - PAINT_LAYERS_EEVEE_RESERVED_SAMPLERS`); zero disables the check. `max` is
 * `GPU_max_textures()`, carried only so the regeneration line can print it.
 */
void BKE_paint_layers_sampler_budget_set(int budget, int max_textures);

/** The current budget; zero means the check is off. */
int BKE_paint_layers_sampler_budget_get();

/** The `GPU_max_textures()` value reported with the budget, or zero when never set. */
int BKE_paint_layers_sampler_max_get();

/**
 * The number of unique GPU samplers EEVEE would allocate for \a ma's node graph, following only
 * nodes reachable from a Material Output. Mirrors `gpu_node_graph.cc`: image textures dedup by
 * `(Image, sampler state)`, tiled images add a mapping sampler, every colorband node shares one
 * texture, Nishita sky shares one, and IES contributes none.
 */
int BKE_paint_layers_sampler_count(const Material &ma);

/** \} */

}  // namespace blender
