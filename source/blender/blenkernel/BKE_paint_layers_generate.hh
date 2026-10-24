/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "BLI_function_ref.hh"
#include "BLI_span.hh"

#include "DNA_uuid_types.h"

namespace blender {

struct Main;
struct Material;
struct MaterialPaintLayer;
struct bNode;
struct bNodeTree;

/**
 * The generator that turns a layered material's DNA description (`Material::paint_layers`) into the
 * node tree that renders it.
 *
 * `BKE_paint_layers_tree_build` builds into an empty tree and only touches #Main when the caller
 * hands a database in the build context (to rebuild a kept group); otherwise it is a pure function
 * over the description. `BKE_paint_layers_regenerate` is the #Main-side wrapper that owns the
 * generated group and wires it into the material.
 *
 * The generated tree holds one node group per row, chained bottom to top for every channel the
 * description wires. The values a row owns (its opacity, the constant of a map-less channel) are
 * inputs of that row's group, so a row's nodes are a function of the description's *structure*
 * alone.
 */

/**
 * What `BKE_paint_layers_tree_build` needs that is not a pure function of the description: the shared
 * Normal Combine group, and one empty node group per layer, both of which have to be created in
 * #Main first.
 */
struct PaintLayersBuildContext {
  /** Shared group an instance of which combines two tangent-space normal maps, or null. */
  bNodeTree *normal_combine_group = nullptr;
  /**
   * Shared group an instance of which blends by a runtime mode, or null. Only rows and channels in
   * the runtime "scrubbing" tier use it; every other blend stays a static Mix node.
   */
  bNodeTree *blend_group = nullptr;
  /**
   * Shared group that turns a UV coordinate and a Gradient's line into its ramp position, or null
   * when the description has no gradient. The same group is instantiated in every Gradient row's
   * group.
   */
  bNodeTree *gradient_group = nullptr;
  /**
   * Hands out the empty node group that holds \a layer's own nodes, named and marked as that
   * layer's. Required: the build puts every layer in a group of its own, so a caller that cannot
   * create data-blocks cannot build.
   */
  FunctionRef<bNodeTree *(const MaterialPaintLayer &layer)> layer_tree_get;
  /**
   * Whether \a layer's group was kept unchanged by the caller: its nodes and interface already hold
   * the row, so the build only re-links its instance into the new root instead of rebuilding it.
   * Empty (the default) means every group is built from scratch.
   */
  FunctionRef<bool(const MaterialPaintLayer &layer)> layer_unchanged;
  /**
   * Forget that \a layer's group is current: drop its stored topology hash so the next regeneration
   * rebuilds it. Used when a kept group could not be re-linked and the rebuild failed, leaving an
   * empty group that must not be taken for unchanged on the next pass.
   */
  FunctionRef<void(const MaterialPaintLayer &layer)> layer_invalidate;
  /**
   * Hands out the group holding a copy of the source's shader graph for the Material row \a layer,
   * filled and current, or null when the source cannot be wrapped. Empty (the default) means no
   * row shows its source's graph and each reads the map rendered from it.
   */
  FunctionRef<bNodeTree *(const MaterialPaintLayer &layer)> source_group_get;
  /**
   * The main database the generated groups live in, when the build may tear a group down and
   * rebuild it (a kept group whose instance can no longer be re-linked). Null in a pure build that
   * only creates nodes.
   */
  Main *bmain = nullptr;
};

/**
 * Fill an empty \a tree with the stack \a ma's description stands for, bottom to top, one output
 * per channel the description wires. Pure: the caller owns \a tree and its lifetime, and \a ctx
 * only ever names data-blocks the caller already owns.
 *
 * Each row is built inside a node group of its own (see #PaintLayersBuildContext::layer_tree_get):
 * the tree gets one instance per row and the result chain, so the root stays readable. The root's
 * outputs are named `Result <channel>`.
 */
void BKE_paint_layers_tree_build(const Material &ma,
                             bNodeTree &tree,
                             const PaintLayersBuildContext &ctx = {});

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
  /** The depsgraph relations had to be rebuilt: a generated group appeared or disappeared. */
  bool relations_changed = false;
};

/**
 * Rebuild `ma.paint_layers_tree` from `ma.paint_layers` and wire it into the material.
 *
 * Creates the generated group when the material has none, overwrites the one it owns (matched by
 * #Material::paint_layers_owner_uid) when it has one, and leaves a foreign tree alone. The group is
 * instantiated in the material's embedded node tree (the instance node is found by its marker) and
 * its channel outputs are routed into the Principled BSDF -- Normal through a Normal Map, Height
 * through a Bump.
 *
 * Never called from draw or evaluation; the edit API only marks #MA_PAINT_LAYERS_REGEN and
 * #BKE_paint_layers_regenerate_tagged is the single scheduling point. Clears #MA_PAINT_LAYERS_REGEN
 * on success.
 *
 * \return false when \a ma is not a layered material or has no embedded node tree.
 */
bool BKE_paint_layers_regenerate(Main &bmain,
                                 Material &ma,
                                 PaintLayersRegenerateReport *r_report = nullptr);

/**
 * Regenerate every layered material of \a bmain that needs it -- #MA_PAINT_LAYERS_REGEN is set or
 * the generated tree is missing -- and nothing else.
 *
 * This is the single scheduling point: it runs on the main thread, before the depsgraph update,
 * never from draw, depsgraph flush or evaluation. A mutator only marks and tags; the rebuild
 * happens here, so a composite edit that issues several tags still generates once from its final
 * state, and a new ID is never created while the depsgraph relations of the current pass are
 * already built.
 */
void BKE_paint_layers_regenerate_tagged(Main &bmain);

/* -------------------------------------------------------------------- */
/** \name Viewport paint pass preview
 *
 * The 3D Viewport shows a layered material through its Principled shading. To paint while seeing
 * one channel alone, a Mix Shader is inserted between that Principled and the Material Output and
 * an Emission fed by the channel's generated result takes over while a pass is selected.
 * \{ */

/**
 * The passes the 3D Viewport offers, in menu order: Combined, every shading channel the generated
 * tree can expose (Height included), and the active layer's Mask. This is a viewport list of its
 * own: the Image Editor's display list keeps ignoring Height, and its Mask entry is a layer-map
 * role rather than the active layer's mask this list ends with.
 *
 * #Material::paint_layers_view_pass stores an index into this list, so it is append-only: changing
 * the position of an entry silently changes what files saved earlier show. A new pass goes at the
 * end, or the order changes together with a versioning step that remaps the stored index.
 */
Span<int> BKE_paint_layers_view_passes();

/** The pass \a ma is shown through in the 3D Viewport: a value from #BKE_paint_layers_view_passes,
 * #PAINT_LAYER_PASS_COMBINED for the shaded material. */
int BKE_paint_layers_view_pass_get(const Material &ma);

/** Select the 3D Viewport pass of \a ma. Returns false when \a pass is not one the preview can
 * show. */
bool BKE_paint_layers_view_pass_set(Material &ma, int pass);

/** Whether the viewer can show \a pass of \a ma: any channel of its paint channel set (one without a
 * layer stack shows its neutral value), or the Mask of an active row that has a mask. Combined is
 * always available. */
bool BKE_paint_layers_view_pass_available(const Material &ma, int pass);

/**
 * Make the material's viewport preview nodes match its stored view pass: install the Mix Shader and
 * Emission when a pass is selected, wire the channel's generated result into it, and move the mix
 * factor so the viewport shows the pass instead of the Principled shading. Idempotent, and safe to
 * call after every regeneration. \a bmain may be null when the caller cannot update the tree.
 */
bool BKE_paint_layers_view_pass_sync(Main *bmain, Material &ma);

/** Whether \a node is the Mix Shader #BKE_paint_layers_view_pass_sync installs. */
bool BKE_paint_layers_is_view_pass_mix(const bNode &node);

/** \} */

/**
 * Copy the description's animatable values (each row's opacity, a Fill row's constant) into the
 * layer groups' input sockets of \a ma's generated tree, without touching any topology.
 *
 * A value edit only calls this; the socket set is fixed by the structure, so a slider move never
 * rebuilds a group. \return false when a value input the description owns has no socket in the
 * tree (the tree was hand-edited), so the caller can fall back to a full regeneration.
 */
bool BKE_paint_layers_values_sync(Material &ma);

/**
 * Tag the generated trees of \a ma (the root and every row group) so their evaluated copies pick up
 * what #BKE_paint_layers_values_sync wrote. Only for the original material: the sync itself also
 * runs during evaluation, where tagging would retrigger the evaluation.
 */
void BKE_paint_layers_values_tag_trees(Material &ma);

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
                                         int copy_flag);

/** Re-assign generated tree and subgroup ownership to ma's new owner UID. */
void BKE_paint_layers_owner_uid_changed(Main &bmain, Material &ma, const bUUID &old_uid);

/**
 * Detach the derived data of every layered material, ahead of a memfile undo or redo decode.
 *
 * The generated trees are rewritten after an operator's undo push (regeneration on the next
 * update). The undo system keeps a data-block whose stored chunks equal the state it is leaving, so
 * such a data-block would survive the decode still pointing at maps and groups the target state does
 * not have, and those are freed under it. The decode walks those pointers itself (user-count
 * recompute) before any later hook runs, and the undo tags do not say which data-blocks are
 * affected, so every layered material is handled. The pointers are derived data: dropping them
 * makes the regeneration after the undo put them back.
 */
void BKE_paint_layers_undo_prepare(Main &bmain);

/**
 * Null the ID pointers of every layered material's generated trees that name a data-block no longer
 * in \a bmain, and make the material regenerate.
 *
 * Call right after a memfile undo or redo decode. #BKE_paint_layers_undo_prepare only covers the
 * data-blocks the undo tags say changed after the last push; a tree the decode kept as it was can
 * still name a map or group the target state does not have, and regeneration then decrements the
 * users of freed memory. This one looks at the result of the decode instead of at the tags, and
 * leaves every pointer that is still valid alone.
 */
void BKE_paint_layers_undo_validate(Main &bmain);

/**
 * Drop the bake change subscriptions on images that no longer exist in \a bmain. Called at the end
 * of #BKE_paint_layers_undo_validate: a decode that freed a source map leaves a subscription naming
 * it, and the row is marked changed so it is re-baked.
 */
void BKE_paint_layers_bake_subscriptions_validate(Main &bmain);

}  // namespace blender
