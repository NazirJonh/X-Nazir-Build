/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * Runtime state for one live Curve Patch. Owned by
 * `SculptSession::curve_patch_session`, sibling to `StrokeCache`/`filter::Cache`/`expand::Cache`.
 * Created right after the anchor stroke of a `BRUSH_STROKE_CURVE_PATCH` brush finishes; destroyed
 * on commit (Enter) or cancel (Esc). See `paint_curve_patch_session.cc` for the restore-then-
 * re-stamp recompute that uses it.
 *
 * The parameters and the derived geometry live in `BKE_curve_patch.hh` instead, so that both can
 * be driven without a brush, a sculpt session or an operator. What stays here is everything that
 * genuinely needs the editor layer: the DNA texture binding, the document the user edits, and the
 * bookkeeping against the live mesh.
 */

#include <array>
#include <memory>

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_bit_vector.hh"
#include "BLI_function_ref.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_texture_types.h"

#include "BKE_curve_patch.hh"
#include "BKE_curves.hh"

#include "ED_view3d.hh"

#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_effect.hh"
#include "paint_curve_patch_targets.hh"

namespace blender {
struct Brush;
struct Depsgraph;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct Object;
struct PaintModeSettings;
struct PaintVector;
struct ReportList;
struct Scene;
struct Sculpt;
struct SculptSession;
struct bContext;
}  // namespace blender

namespace blender::ed::sculpt_paint {

struct StrokeCache;

/** The next value of #CurvePatchSession::serial. Main thread only, like the sessions themselves. */
uint64_t curve_patch_session_serial_next();

/**
 * What every patch stamped so far has left on one element (a vertex, a color-domain element), kept
 * premultiplied so a later patch can be composited over it. See #curve_patch_blend_over.
 */
struct CurvePatchOverAccum {
  /** Composite of the patches stamped BEFORE the current one: payload in `xyz`, coverage in `w`. */
  float4 below = float4(0.0f);
  /** `weight * (payload, coverage)` summed over the passes of the current patch, and the weights'
   * sum, so the passes of one patch (symmetry) still average. */
  float4 current = float4(0.0f);
  float current_weight = 0.0f;
  /** #CurvePatchApplyState::patch_rank of the patch `current` belongs to; -1 = none yet. */
  int patch_rank = -1;
};

/** Per-application bookkeeping against the live mesh. */
struct CurvePatchApplyState {
  /** Number of elements the keys of the effect's snapshot index into, sampled once when the patch
   * starts: `Mesh::verts_num` for `Type::Mesh`, `SubdivCCG::positions.size()` for `Type::Grids`.
   *
   * The modal passes events through whenever the cursor leaves its region, so an unrelated
   * operator can retopologize the object while a patch is live. Every key in the effect's snapshot
   * would then name a different element or none at all, and both restoring and committing would
   * write against the wrong surface -- or past the end of the array. #element_num plus
   * #faces_num / #corners_num is the cheap detection for that. */
  int64_t element_num = 0;

  /**
   * Mesh topology counts sampled with #element_num. -1 means "not a mesh" (Grids / no snapshot).
   * `element_num` alone misses Triangulate, Poke, and remesh that keep `verts_num` and change
   * faces/corners -- ColorEffect already documents that the two counts move independently.
   */
  int64_t faces_num = -1;
  int64_t corners_num = -1;

  /** Set when the check above fails. The patch is then unusable: its snapshot describes a mesh
   * that no longer exists, so it must be abandoned WITHOUT restoring (which would write stale
   * positions) and without pushing anything to the undo stack. */
  bool invalidated = false;

  /** Per-restamp accumulator for blending symmetry passes that land on the same real vertex (a
   * patch straddling a mirror/radial symmetry plane can have both the direct and the mirrored
   * pass claim the same vertex). Keyed like the effect's snapshot; `x` is the running sum of each
   * claiming pass's falloff weight, `y` the running sum of `weight * height`, so every pass's
   * PHASE 2 can recompute the weighted-average target height so far instead of unconditionally
   * overwriting an earlier pass's contribution (see `ReliefEffect::apply_pass()`). Unlike
   * the effect's snapshot, this does NOT persist for the patch's whole life -- cleared at the
   * start of every `curve_patch_restore_and_restamp()`, since blending is only meaningful between
   * passes of the SAME restamp.
   *
   * Shared across every `CurvePatchItem` of this restamp: the passes of ONE patch average, while
   * a patch stamped later composites over the result of the earlier ones (see
   * #curve_patch_blend_over). */
  Map<int, CurvePatchOverAccum> pass_weight_accum;

  /** Position of the patch being stamped in the stamping order (0 = bottom of the stack, stamped
   * first). Set by the session before every patch's symmetry passes; those passes must run
   * back to back, because the accumulator above detects a new patch by this value changing. */
  int patch_rank = 0;

  /** PBVH node indices displaced by the PREVIOUS restamp (one bit per `bke::pbvh::Tree` node).
   * `curve_patch_restore_only()` tags exactly these nodes so `bke::pbvh::update_normals()` reverts
   * only their normals -- the footprint that just moved away, which a "current node mask only" tag
   * would miss. `curve_patch_restore_and_restamp()` clears it before each restamp; the relief
   * action ORs each symmetry pass's node mask back in, so it always describes what the last
   * restamp touched. Tracking this precisely is what keeps a restore/re-stamp O(patch footprint)
   * rather than O(whole mesh) on every interactive drag event -- see
   * `paint_curve_patch_session.cc`. */
  BitVector<> last_restamp_nodes;

  /** Union of EVERY restamp's node mask over the patch's whole life (one bit per
   * `bke::pbvh::Tree` node), as opposed to #last_restamp_nodes which describes only the latest
   * one. Sized alongside it but never cleared.
   *
   * This is the mask the commit-time undo step is built from, and the wider set is required, not a
   * safety margin: `ReliefEffect::smooth_relief()` writes to every key of the effect's snapshot,
   * which accumulates across the patch's whole life. A vertex touched by an early restamp and left
   * alone by the final one sits in that map holding a zero displacement, and smoothing averages it
   * with its displaced neighbors into a non-zero one -- so it moves at commit time even though its
   * node is absent from #last_restamp_nodes. Every such key was written by SOME restamp, whose
   * node mask this set contains, so the coverage holds by construction. */
  BitVector<> all_touched_nodes;
};

/**
 * One object a live Curve Patch session stamps, with everything describing ITS surface: the
 * effect instance (one per object -- the snapshots, undo masks and canvas keys of a single effect
 * describe a single mesh), the per-object bookkeeping against the live mesh, and the `StrokeCache`
 * the symmetry machinery and the effect read.
 *
 * A session always has at least one target, the PRIMARY (`is_primary`, `targets[0]`): the object
 * the document lives on, whose local space the control curves are authored in. A multi-object
 * session adds one secondary target per additional object the curve reaches; a secondary mirrors
 * the document's items into its own local space (see #mirror_items) instead of sharing them.
 */
struct CurvePatchObjectTarget {
  /** The stamped object. Null marks a target detached mid-session (its object was deleted or left
   * the mode); detached targets are skipped by every loop and removed lazily. */
  Object *ob = nullptr;

  /** Owned by the session while it lives: the primary's is the anchor stroke's cache taken over
   * at the handoff, a secondary's the stand-in #curve_patch_session_add_target builds for it (the
   * non-stroke entry points build the same kind of stand-in for the primary). Freed by the session
   * teardown on every path. */
  StrokeCache *cache = nullptr;

  /** What this target writes and how it takes it back. Chosen once at target creation from the
   * brush and this object's own configuration; never null on a registered target. */
  std::unique_ptr<CurvePatchEffect> effect;

  /** This target's texture binding, resolved against ITS local radius. Null for the primary, whose
   * binding is the document's (`CurvePatchSession::doc.texture`); a secondary gets its own because
   * the ribbon caps are lengths in the target's local units. */
  std::unique_ptr<CurvePatchTextureBinding> texture;

  /** Per-target bookkeeping against the live mesh (see #CurvePatchApplyState). The staleness
   * counters and #CurvePatchApplyState::invalidated are per-target: one target whose mesh changed
   * underneath it is skipped alone, and the others keep restoring / committing. */
  CurvePatchApplyState apply;

  /** The document's items carried into this target's own local space, SECONDARY targets only. The
   * primary reads `doc.patches` directly and never duplicates the document.
   *
   * Index-parallel to #CurvePatchDocument::patches, which is also the stable identity: a mirror
   * item at index N always describes the document item at index N. The curve, parameters and
   * identity fields are re-derived when the document revision or either object matrix changes. */
  Vector<CurvePatchItem> mirror_items;

  /** Inputs the current #mirror_items were derived from. A re-stamp that changes neither the
   * document nor either matrix reuses the mirror instead of copying every control curve again. */
  uint64_t mirror_document_revision = 0;
  float4x4 mirror_primary_to_world = float4x4::identity();
  float4x4 mirror_target_world_to_object = float4x4::identity();

  bool is_primary = false;

  /** Whether the curve currently reaches this object (the reach test runs on every restamp). Only
   * gates the geometry rebuild and the apply: the RESTORE runs for every valid target regardless,
   * so an object the curve was dragged away from loses its previous stamp. */
  bool in_reach = false;

  /** Frozen `brush.curve_patch.face_set` request copied from the session at target creation (see
   * #CurvePatchSession::use_face_set_on_commit). */
  bool use_face_set_on_commit = false;

  /** Whether the non-uniform-scale approximation was already reported for this target, so a live
   * drag does not flood the report list on every restamp. */
  bool reported_nonuniform_scale = false;

  /** Whether the last restamp actually applied this target. Out-of-reach and empty targets are
   * restored but not applied; they still need a redraw so the restored surface reaches the screen. */
  bool applied_this_restamp = false;

  /** The single predicate for "this target can still be restored, applied or committed". */
  bool is_usable() const
  {
    return this->ob != nullptr && this->effect != nullptr && this->cache != nullptr &&
           !this->apply.invalidated;
  }
};

/**
 * The per-target view the effects operate on: what used to be read off #CurvePatchSession (its
 * apply state, its active item, its texture binding) plus the object the effect is writing. Built
 * per call from a #CurvePatchObjectTarget by #CurvePatchSession::target_state; nothing here owns
 * anything.
 */
struct CurvePatchTargetState {
  /** The object this target stamps. */
  Object &ob;
  /** The target's own #StrokeCache -- the same one published on `ob.runtime->sculpt_session->cache`
   * while the session lives. */
  StrokeCache &cache;
  /** The effect instance that writes this target. */
  CurvePatchEffect &effect;
  /** The target's own bookkeeping (see #CurvePatchApplyState). */
  CurvePatchApplyState &apply;
  /** The document items this target stamps: the document's own `patches` for the primary, the
   * target's mirror for a secondary. Index-parallel to the document. */
  MutableSpan<CurvePatchItem> items;
  /** The shared document (active patch index). Never written through here. */
  const CurvePatchDocument &doc;
  /** This target's texture binding, in its own local units (see
   * #CurvePatchObjectTarget::texture). */
  const CurvePatchTextureBinding &texture;
  /** Frozen face-set request of this target (see #CurvePatchObjectTarget::use_face_set_on_commit).
   */
  bool use_face_set_on_commit = false;
  /**
   * Whether this target's effect owns the session-level `ImageUndoStep` (see
   * #CurvePatchSession::owns_image_undo_step): true for the primary target, false for secondary
   * Image targets, which push their undo tiles into the step the owner opened without opening or
   * closing it themselves. Only the Image effect reads it.
   */
  bool image_undo_step_owner = true;

  /** The patch the modal editor acts on, of THIS target's items. Asserts the document is
   * well-formed, like #CurvePatchDocument::active_item. */
  CurvePatchItem &active_item()
  {
    BLI_assert(this->doc.has_active_item());
    BLI_assert(this->items.index_range().contains(this->doc.active_patch));
    return this->items[this->doc.active_patch];
  }
  const CurvePatchItem &active_item() const
  {
    BLI_assert(this->doc.has_active_item());
    BLI_assert(this->items.index_range().contains(this->doc.active_patch));
    return this->items[this->doc.active_patch];
  }
};

/**
 * The shared commit-time sculpt undo step of one session (`curve_patch_finish_commit()`'s local
 * state). ONE step for every target of the session: the first target with content opens it with
 * `undo::push_begin_ex()`, every later one extends it with `undo::push_begin_add_object()` (the
 * only way to add an object to the same step -- a second `push_begin_ex()` would free the first's
 * `step_init`), and one `undo::push_end_all_ex()` parks it for the committing operator's
 * `OPTYPE_UNDO`, which is what makes the whole multi-object commit one Ctrl+Z. If no target has
 * content, the step is never opened and `push_end_all_ex()` is never called.
 */
struct CurvePatchCommitStep {
  /** Whether the shared step is currently open in the undo stack's `step_init`. */
  bool open = false;
  /** Whether ANY target's commit wants a face-set write, which forces the shared step into the
   * stack instead of leaving it parked (a face-set step's own `push_begin_ex()` would otherwise
   * free the parked step). Computed across all targets before the step is closed. */
  bool face_set_follows = false;
};

/** A live Curve Patch editing session. Published on `SculptSession::curve_patch_session` -- of the
 * owner object AND of every secondary target, so the effect action, the overlay and the teardown
 * callbacks can find the one shared session from any participant. */
struct CurvePatchSession {
  /** What the user edits: the control curves, the texture binding, the session-local undo stack.
   * Target-independent, so the shared editor can act on it without knowing this session owns it.
   *
   * Stays in the PRIMARY object's local space (the persisted `PaintVector` convention); the
   * curves the secondary targets stamp are derived on every restamp -- see
   * #CurvePatchObjectTarget::mirror_items. */
  CurvePatchDocument doc;

  /** One state per stamped object; the primary -- the object the document lives on and the owner
   * below -- is `[0]`. The set is NOT fixed: a Shift+drag of a control point onto another Sculpt
   * Mode object appends that object through #curve_patch_session_add_target. The primary stays at
   * `[0]`; an added target is appended at the end and owns its own cache. Never hold a reference
   * or pointer into this vector across an append. */
  Vector<CurvePatchObjectTarget> targets;

  /** The object this session belongs to: its document, its view context, its layer row. Teardown
   * (`curve_patch_session_free`, the mode-exit commit, the modal editor) goes through the owner;
   * the same session pointer published on a secondary only ever detaches that one target. */
  Object *owner = nullptr;

  /** True once the session-level `ImageUndoStep` was opened through the PRIMARY target's effect
   * (#CurvePatchEffect::session_undo_begin): exactly one owner per session, even with several
   * Image targets, because the global image undo stack holds a single in-flight step. Secondary
   * Image effects push their tiles into that step without opening or closing it (their
   * #CurvePatchTargetState::image_undo_step_owner is false). */
  bool owns_image_undo_step = false;

  /** The effect kind every target of this session writes. One kind per SESSION on purpose: the
   * commit orchestrator builds one shared undo step, and mixing undo systems (an Image target
   * beside a Relief one) would leave the secondary's pixels with no owner for their step. A
   * candidate object that would resolve to a different kind is filtered out at target creation
   * (skip + warn). */
  CurvePatchEffectType effect_type = CurvePatchEffectType::Relief;

  /** The primary target's effect. Null only on a half-built session (see #curve_patch_session_
   * publish's refusal contract); every published session has one. */
  CurvePatchEffect &effect()
  {
    BLI_assert(!this->targets.is_empty() && this->targets[0].effect != nullptr);
    return *this->targets[0].effect;
  }
  const CurvePatchEffect &effect() const
  {
    BLI_assert(!this->targets.is_empty() && this->targets[0].effect != nullptr);
    return *this->targets[0].effect;
  }

  /** The primary target. Asserts the invariant `targets[0].is_primary` every publish maintains. */
  CurvePatchObjectTarget &primary_target()
  {
    BLI_assert(!this->targets.is_empty() && this->targets[0].is_primary);
    return this->targets[0];
  }
  const CurvePatchObjectTarget &primary_target() const
  {
    BLI_assert(!this->targets.is_empty() && this->targets[0].is_primary);
    return this->targets[0];
  }

  /** Whether the primary target carries its published effect -- the usual "live enough to stamp"
   * gate for callers that resolve the session from a published pointer and must tolerate a
   * half-built one (the handoff before the effect exists, a session mid-teardown). */
  bool has_primary_effect() const
  {
    return !this->targets.is_empty() && this->targets[0].effect != nullptr;
  }

  /** This session's target for \a ob, or null. The owner's target is found by identity; a
   * secondary by its registered object. Null when \a ob is not stamped by this session. */
  CurvePatchObjectTarget *target_for(Object &ob)
  {
    for (CurvePatchObjectTarget &target : this->targets) {
      if (target.ob == &ob) {
        return &target;
      }
    }
    return nullptr;
  }
  const CurvePatchObjectTarget *target_for(const Object &ob) const
  {
    for (const CurvePatchObjectTarget &target : this->targets) {
      if (target.ob == &ob) {
        return &target;
      }
    }
    return nullptr;
  }

  /** Visit every target that passes #CurvePatchObjectTarget::is_usable. */
  void for_each_usable_target(const FunctionRef<void(CurvePatchObjectTarget &)> fn)
  {
    for (CurvePatchObjectTarget &target : this->targets) {
      if (target.is_usable()) {
        fn(target);
      }
    }
  }
  void for_each_usable_target(const FunctionRef<void(const CurvePatchObjectTarget &)> fn) const
  {
    for (const CurvePatchObjectTarget &target : this->targets) {
      if (target.is_usable()) {
        fn(target);
      }
    }
  }

  /** The document items \a target stamps: the document's own patches for the primary, the target's
   * mirror for a secondary. Index-parallel to #CurvePatchDocument::patches either way. */
  MutableSpan<CurvePatchItem> items_for(CurvePatchObjectTarget &target)
  {
    if (target.is_primary) {
      return this->doc.patches;
    }
    return target.mirror_items;
  }
  Span<CurvePatchItem> items_for(const CurvePatchObjectTarget &target) const
  {
    if (target.is_primary) {
      return this->doc.patches;
    }
    return target.mirror_items;
  }

  /** Build the per-target view the effect methods take. */
  CurvePatchTargetState target_state(CurvePatchObjectTarget &target)
  {
    BLI_assert(target.ob != nullptr && target.cache != nullptr && target.effect != nullptr);
    const CurvePatchTextureBinding &texture = target.texture ? *target.texture :
                                                               this->doc.texture;
    return CurvePatchTargetState{*target.ob,
                                 *target.cache,
                                 *target.effect,
                                 target.apply,
                                 this->items_for(target),
                                 this->doc,
                                 texture,
                                 target.use_face_set_on_commit,
                                 target.is_primary};
  }

  /** Whether at least one target can still be restored, applied or committed. A broken primary
   * does not cancel the whole session while a secondary remains usable. */
  bool has_valid_target() const
  {
    for (const CurvePatchObjectTarget &target : this->targets) {
      if (target.is_usable()) {
        return true;
      }
    }
    return false;
  }

  /** Owned copy of the anchor stroke's `ViewContext`, set once in `curve_patch_start_from_anchor`.
   * `StrokeCache::vc` is a non-owning pointer that normally points into the interactive stroke's
   * own (stack-lifetime) `ViewContext`; since Curve Patch takes over `SculptSession::cache` after
   * that stroke's operator has already finished and torn its `ViewContext` down, `StrokeCache::vc`
   * is repointed at this member instead so every `curve_patch_restore_and_restamp()` re-stamp
   * (which reads it via `calc_local_from_screen()`/`cache->vc`) dereferences valid memory for the
   * whole lifetime of the patch. Shared by every target's cache. */
  ViewContext view_context = {};

  /** Suppresses repeating the window-cap warning: `curve_patch_restore_and_restamp()` runs on
   * EVERY mouse event, so an unguarded message would flood the info line. */
  bool reported_frame_cap = false;

  /** Set when a curve look value (per-point opacity or opacity falloff) was edited through RNA
   * (the Active Tool panel). The modal consumes it on its next event to defer its undo step to the
   * end of the slider gesture, the same way `curve_patch_layer_active_item_follow()` reports a
   * per-curve look change. */
  bool rna_look_changed = false;

  /** Frozen `brush.curve_patch.face_set` from the anchor stroke (see #curve_patch_begin_editing).
   * `ReliefEffect::face_set_masks()` reads the LIVE brush so the toggle can be flipped mid-edit,
   * but falls back to this when no live brush is available at commit time. An active Face Sets
   * From Texture mode implies the request as well (see the live OR there): its per-dab write is
   * suppressed for the whole anchor phase, so the commit is its only chance. Without the fallback
   * a brush switch mid-edit or a null `StrokeCache::paint` would silently drop the Face Set. */
  bool use_face_set_on_commit = false;

  /** The stack row this session was entered from, when it edits a
   * #MA_PAINT_LAYER_SOURCE_CURVE_PATCH row (see #curve_patch_start_from_layer and
   * `paint_curve_patch_layer.hh`). Null for an anchor-stroke or headless session. The row is
   * addressed by its marker, never by a raw pointer: the Outliner can delete it while the session
   * runs, and a dangling pointer on the commit path would crash. */
  Material *layer_material = nullptr;
  bUUID layer_row_marker = {};

  /**
   * The previous-brush restore of a layer session: the single path that puts the user's own brush
   * back on every exit (commit, cancel, refused invoke, object death). Owned here rather than by
   * the modal's operator data, so it survives the confirm dialog's teardown / relaunch round-trip.
   * Null for a session that did not switch the brush.
   */
  std::unique_ptr<class PrevBrushRestore> layer_brush_restore;

  /** Identity of this session, unique for the run. A modal editor compares it to tell its own
   * session from a later one the same object published after it ended (a pointer would not do:
   * the allocator hands the freed address to the next session). */
  uint64_t serial = curve_patch_session_serial_next();

  /** Forwards to #CurvePatchDocument::has_active_item. */
  bool has_active_item() const
  {
    return this->doc.has_active_item();
  }

  /** Forwards to #CurvePatchDocument::active_item. */
  CurvePatchItem &active_item()
  {
    return this->doc.active_item();
  }
  const CurvePatchItem &active_item() const
  {
    return this->doc.active_item();
  }
};

/**
 * Build a Curve Patch control curve from explicit positions and radii.
 *
 * Exists as one function because the sequence it performs is unforgiving. `paintcurve_geometry_
 * init_bezier()` creates the handle TYPES but not the handle POSITION attributes, and
 * `calculate_bezier_auto_handles()` silently EARLY-OUTS when those attributes are absent -- so
 * omitting any step produces no error, just a bezier collapsed to the origin and a silently empty
 * patch. Three callers would otherwise each have to know it.
 *
 * `radii` must match `positions` in size, and carries this codebase's paint-curve convention where
 * 1.0 means "full brush size" -- NOT `bke::CurvesGeometry::radius()`'s hair-oriented 0.01 default.
 *
 * Lives in the editor layer, not in blenkernel, only because `paintcurve_geometry_init_bezier()`
 * does (`paint_curve_intern.hh`); nothing about the curve itself needs an editor.
 */
bke::CurvesGeometry curve_patch_control_curve_from_points(Span<float3> positions,
                                                          Span<float> radii,
                                                          bool cyclic);

/**
 * Resolve one build's worth of core parameters from a brush.
 *
 * The five values the brush cannot supply are passed in instead: `radius`, `radius_per_size`,
 * `plane_normal` and `stamp_seed` are frozen when the patch starts, and `swap_axis` is owned by
 * the session as well -- the modal's S hotkey and its undo stack both write it without touching
 * the brush, so reading it back from the brush unconditionally would undo them on the next poll.
 *
 * `final_quality` is left false; a caller taking the commit-time build sets it itself.
 */
bke::CurvePatchParams curve_patch_params_from_brush(const Brush &brush,
                                                    float radius,
                                                    float radius_per_size,
                                                    const float3 &plane_normal,
                                                    uint32_t stamp_seed,
                                                    bool swap_axis);

/**
 * Live poll overlay: brush-driven fields from `brush`, frozen per-patch fields from `frozen`.
 *
 * This is the live/frozen contract in one place. A new brush field must be classified here:
 *
 * - **Frozen per patch** (`radius` / `radius_per_size`, `plane_normal`, `stamp_seed`,
 *   `final_quality`): copied from `frozen`. `radius` is rebuilt from `radius_per_size *
 * brush_size` so a Size slider still scales a started patch without unfreezing the captured ratio.
 * - **Session-owned** (`swap_axis`): taken from the brush only when `apply_brush_swap_axis` is
 *   true (the brush itself changed). Otherwise kept from `frozen`, so the Y hotkey and session
 *   undo are not overwritten on the next poll.
 * - **Live in params** (length, falloff, stamps mode, spacing, jitter, angles): come from
 *   #curve_patch_params_from_brush. `operator==` on the result triggers a re-stamp.
 * - **Live, not in params** (strength, Add/Subtract, symmetry, textures, color, falloff curve):
 *   stay on the modal's #CurvePatchLiveInputs. Do not add them here.
 *
 * Writes the same overlay onto every patch; only the frozen fields differ per item.
 */
bke::CurvePatchParams curve_patch_params_live_overlay(const Brush &brush,
                                                      const bke::CurvePatchParams &frozen,
                                                      int brush_size,
                                                      bool apply_brush_swap_axis,
                                                      bool radius_is_per_curve = false);

/**
 * The half of starting a session that owes nothing to a stroke: reset the per-session bookkeeping,
 * build the pristine surface snapshot, create the effect of `effect_type` and publish the session
 * on `ob`.
 *
 * `session->doc.patches` must already hold at least one fully built item -- curve and `params`
 * both (the surface snapshot ignores the parameters, but every later consumer does not). Nothing
 * is stamped here and no viewport is touched, which is what lets the headless apply path share it
 * with the two interactive entry points.
 *
 * Frees NOTHING on failure -- returning false leaves `session` unpublished and still owned by the
 * caller, whose cleanup differs: the interactive entry points also own the anchor stroke's
 * `SculptSession::cache` and must free that too.
 */
bool curve_patch_session_publish(Object &ob,
                                 CurvePatchSession &session,
                                 CurvePatchEffectType effect_type,
                                 PaintModeSettings &paint_mode_settings);

/**
 * Reset a binding to "no multi-texture": no variants, no caps. The relief then reads `brush.mtex`
 * directly, which is exactly the SINGLE-mode behavior.
 *
 * Separate from the reset #curve_patch_texture_binding_from_brush already performs because a
 * re-stamp can find no active brush at all, and `MTex`'s deleted copy assignment (see
 * #DNA_DEFINE_CXX_METHODS) rules out the obvious `binding = {}`.
 */
void curve_patch_texture_binding_clear(CurvePatchTextureBinding &r_binding);

/**
 * Resolve which textures one build samples. `radius` is the patch's frozen base radius, used to
 * convert the UI's brush-diameter cap lengths into world units.
 *
 * `r_binding` is fully overwritten, including the SINGLE-mode case, where it comes back empty and
 * the relief reads `brush.mtex` directly.
 */
void curve_patch_texture_binding_from_brush(const Brush &brush,
                                            float radius,
                                            CurvePatchTextureBinding &r_binding);

/**
 * Restore every vertex in the effect's snapshot to its snapshotted position, then re-apply the
 * frozen brush along the current control curve, growing the effect's snapshot on demand for any
 * newly-touched vertex not yet snapshotted. Call after every control-curve mutation (move/add/
 * remove point, radius change, axis toggle) and once more right before commit.
 *
 * Takes its dependencies explicitly rather than through a `bContext` so that a caller with no
 * window manager can drive it -- which is the whole point of the headless apply path. `reports`
 * may be null; the two conditions it carries (a mesh changed underneath the session, a curve too
 * complex for a full surface wrap) are then simply not surfaced.
 */
void curve_patch_restore_and_restamp(const Scene &scene,
                                     const Depsgraph &depsgraph,
                                     Sculpt &sd,
                                     PaintModeSettings &paint_mode_settings,
                                     CurvePatchSession &session,
                                     ReportList *reports);

/** #curve_patch_restore_and_restamp with everything resolved from the context. The form every
 * interactive caller wants; the explicit one above exists for callers that have no context. The
 * session's own primary target names the object whose space the document lives in -- no explicit
 * object is passed, since a modal event can arrive with a secondary as the active object and the
 * document must resolve through the owner either way. */
void curve_patch_restore_and_restamp(bContext &C, CurvePatchSession &session);

/**
 * Record the CURRENT state as a new step on the session's own undo stack (see
 * `CurvePatchDocument::undo_steps` for why this cannot go through Blender's own undo systems).
 * Called after an action completes -- once per action, not once per event, so a drag is a single
 * step. Defined in `paint_curve_patch_edit_undo.cc`; declared here (rather than kept file-static)
 * because `paint_curve_patch_session.cc`'s #ED_curve_patch_session_undo_push also needs it, for
 * the Transform system's G/R/S handle drags. */
void curve_patch_undo_push(CurvePatchSession &patch);

/**
 * Restores every VALID target's snapshot and does *not* re-stamp. Used for Esc-cancel and for the
 * session-end discard/commit paths. A target whose mesh changed underneath it (its own
 * `CurvePatchApplyState::invalidated` guard) is skipped -- writing its snapshot back would put
 * stale values into an unrelated mesh -- while the other targets still restore.
 */
void curve_patch_restore_only(CurvePatchSession &session);

/** Whether \a target's mesh still has the topology its snapshot was keyed against. */
bool curve_patch_apply_target_changed(Object &ob, const CurvePatchObjectTarget &target);

/** Free one target's stroke cache and clear the SculptSession pointer that published it. */
void curve_patch_stroke_cache_free(SculptSession *ss, StrokeCache *cache);

/** Tag the layer controls UI so their polling re-evaluates the session lock. */
void curve_patch_session_tag_layer_ui(Object &ob);

/** Mark a stamped mesh for an evaluated-mesh republish. Non-mesh objects are ignored. */
void curve_patch_tag_target_mesh_dirty(Object &ob);

/**
 * Commit a live patch -- EVERY target of it -- as an Enter-commit would, because the `SculptSession`
 * holding the session is about to be destroyed. Returns true when the patch was actually written;
 * no-op returning false when no patch is live or when \a ob is not the session's owner (a
 * secondary object's session pointer must never tear the shared session down). Frees the session
 * and every target's `StrokeCache`.
 *
 * Call from a mode-exit path that still has a `bContext` and has not torn anything down yet -- the
 * final re-stamp needs an evaluated depsgraph, and the undo step this pushes is left parked for
 * the calling operator's `OPTYPE_UNDO` exactly as `curve_patch_finish_commit()` requires.
 *
 * The modal that owns the patch (`SCULPT_OT_curve_patch_edit`) is deliberately left running: it
 * sees the freed session on its next event and releases its own operator state through the
 * liveness guard at the top of `curve_patch_edit_modal()`.
 */
bool curve_patch_commit_on_session_end(bContext &C, Object &ob);

/**
 * Discard a live patch -- EVERY target of it -- as an Esc-cancel would, because the `SculptSession`
 * holding the session is about to be destroyed. No-op when no patch is live or when \a ob is not
 * the session's owner (see #curve_patch_commit_on_session_end); the owner's own teardown callback
 * detaches a single secondary instead.
 *
 * The last-resort counterpart to #curve_patch_commit_on_session_end, for the teardown paths that
 * have no `bContext` to commit through. Deliberately still called on the paths that DO commit
 * first, where it is a no-op -- it is what guarantees the session can never outlive the sculpt
 * session.
 *
 * `SculptSession::curve_patch_session` is owned by `SCULPT_OT_curve_patch_edit` and freed when
 * that modal commits or cancels, but leaving sculpt mode -- and `BKE_object_free` -- destroy the
 * sculpt session without consulting the modal. #SculptSession's destructor cannot `MEM_delete` it:
 * `CurvePatchSession` is an editor type that blenkernel only forward-declares. Publish therefore
 * registers this function on `SculptSession::free_curve_patch_session` -- on the owner (full
 * discard) and on every secondary target (detach of that one target, see
 * #curve_patch_target_detach_on_session_end) -- and #BKE_sculptsession_free invokes it whenever
 * the pointer is still live.
 *
 * Cancel rather than commit is deliberate. Committing needs a `bContext` to re-stamp and push an
 * undo step, and the session is also freed from paths that have none -- object deletion
 * (#BKE_object_free) among them -- so committing here would make the outcome depend on HOW the
 * session ended. The modal itself is left to notice the freed session on its next event and tear
 * its own operator state down (see the liveness guard in `curve_patch_edit_modal()`).
 */
void curve_patch_discard_on_session_end(Object &ob);

/**
 * Free every target's `StrokeCache` and the Curve Patch session itself, and clear the published
 * pointers plus the teardown callbacks (#SculptSession::free_curve_patch_session) of every object
 * the session was published on. The shared tail of every path that ends a live patch -- the
 * modal's own commit/cancel, #curve_patch_commit_on_session_end, #curve_patch_discard_on_session_
 * end -- once that path's own restore/re-stamp/commit has already run. Requires a published
 * session on \a ob, which must be the session's OWNER; callers already checked that to reach this
 * point.
 */
void curve_patch_session_free(Object &ob);

/**
 * Keep the curves of a live effect session before the mode exit discards its pixels. Needs the
 * material alive, so it is called from the mode exit and not from the discard.
 */
void curve_patch_store_on_mode_exit(Object &ob);

/** Whether any usable target has data for the shared sculpt undo step. Image canvases keep their
 * own undo system and relief/color/mask targets with an empty snapshot answer false. */
bool curve_patch_session_has_commit_content(CurvePatchSession &session);

/**
 * Finish a committed Curve Patch edit: open ONE shared sculpt undo step for the whole session
 * (#CurvePatchCommitStep), store every valid target's data into it
 * (#CurvePatchEffect::commit_push_nodes), park it for the calling operator's `OPTYPE_UNDO` -- and,
 * if the brush's `curve_patch.face_set` flag is set and any relief actually raised anything,
 * assign ONE shared face set id to the raised faces of every target in a second, shared step.
 *
 * Call ONCE, from the commit branch of `SCULPT_OT_curve_patch_edit`, INSTEAD of a bare
 * `undo::push_end()`, and after the final-quality re-stamp so the threshold is measured against
 * the smoothed profile the user will actually keep. Never called on cancel.
 *
 * One step per SESSION, not per target: `undo::push_begin_ex()` frees the previous `step_init`, so
 * a per-target loop of opens would leave only the last target undoable. The first target with
 * content opens the step (`undo::push_begin_ex`), the rest extend it
 * (`undo::push_begin_add_object`), and a single `undo::push_end_all_ex(false, false)` parks it --
 * so the whole multi-object edit costs exactly one Ctrl+Z.
 *
 * Closing the shared step belongs here rather than in the caller because HOW it must be closed
 * depends on whether a face-set step follows: an undo step carries exactly one `undo::Type`, so
 * the face set needs a second step, and opening one would destroy the position step unless that
 * step was force-pushed first (see the implementation). When no face set follows, the position
 * step must be left parked instead, so that the operator's own `OPTYPE_UNDO` push is the one that
 * files it and the edit costs exactly one Ctrl+Z. That is only knowable after the raised faces
 * have been computed for every target, which is why the implementation computes them before
 * closing the step.
 *
 * "Raised" is measured as displacement from the effect's snapshot, thresholded at a fraction of
 * that target's own maximum displacement -- relative rather than absolute, because displacement is
 * in scene units and would otherwise behave differently on differently scaled objects.
 */
void curve_patch_finish_commit(const Scene &scene,
                               const Depsgraph &depsgraph,
                               CurvePatchSession &session);

/** #curve_patch_finish_commit plus the finished-stroke redraw an interactive commit needs, for
 * every target the session stamped. The headless form above deliberately issues none: a caller
 * with no window manager has nothing to repaint, and tags the ID itself instead. */
void curve_patch_finish_commit(bContext &C, CurvePatchSession &session);

/**
 * Plane the patch is projected onto, fitted to the control curve itself.
 *
 * `fallback` is returned (normalized) when the curve spans no plane -- a straight segment, which
 * every anchor stroke starts as. Pass whatever the caller measured against the real surface (the
 * stroke's `StrokeCache::sculpt_normal`, or a roll's frozen projection normal); a constant would
 * orient those patches arbitrarily.
 */
float3 curve_patch_plane_normal_from_curve(const bke::CurvesGeometry &curve,
                                           const float3 &fallback);

/**
 * Publish a Curve Patch session right after a `BRUSH_STROKE_CURVE_PATCH` anchor stroke finishes.
 * Takes over ownership of the just-finished stroke's `SculptSession::cache` of \a ob; the caller
 * must not tear it down itself when this is invoked. The anchor dab is restored (undone) before
 * anything is stamped, so a refusal leaves the mesh pristine.
 *
 * The session starts single-object: secondary targets join later through the modal's Shift+drag
 * gesture (#curve_patch_session_add_target), never from the anchor stroke.
 *
 * Returns true when a session was published on the object. The caller is responsible for launching
 * `SCULPT_OT_curve_patch_edit` -- see #curve_patch_begin_editing in `paint_curve_patch_session.cc`
 * for why starting a modal from here would invert the layering.
 *
 * It does NOT take over the undo transaction `stroke_undo_begin()` opened for that stroke. This
 * function restores the mesh to its pre-stroke state before anything else touches it, so by the
 * time it returns that transaction describes no net change, and the caller discards it
 * (`SculptPaintStroke::done()`, `mesh/sculpt.cc`). The editor records its own single undo step
 * when the patch is committed. Leaving a transaction open for the modal's whole lifetime is what
 * let any unrelated undo push in the application adopt or free it. The modal editor lives in
 * `paint_curve_patch_edit.cc`.
 *
 * When the brush already owns an OBJECT-SPACE paint curve (`use_3d_space`) with at least two points
 * on a spline (as left behind by Stroke Method: Curve), that curve is adopted as the control curve
 * instead of seeding a fresh two-point segment from the anchor drag. A screen-space paint curve is
 * never adopted: its points are region pixels, not object-space coordinates, so seeding from the
 * anchor is the only way to get a patch at the click.
 */
bool curve_patch_start_from_anchor(const Depsgraph &depsgraph,
                                   Object &ob,
                                   Sculpt &sd,
                                   const Brush &brush,
                                   const ViewContext &vc);

/**
 * Publish a Curve Patch session from a persisted Stack Layers row: adopt the curve items of the
 * row's #PaintVector as the patches (no `Brush::paint_curve` adoption, no anchor stroke), freeze
 * each patch's parameters as they are stored in DNA, and stamp the initial state the way the
 * headless apply path does (a stand-in `StrokeCache` built with #curve_patch_apply_cache_init).
 *
 * The effect is always #CurvePatchEffectType::Image: a Stack Layers row's content is its material
 * channel maps, whatever canvas the scene happens to show. The caller must create the row's channel
 * map and set the Material canvas before this (see `curve_patch_layer_activate`).
 *
 * Unlike the anchor entry this does not take over a stroke's undo transaction or cache: there is
 * no stroke. The caller activates the row's brush snapshot BEFORE calling this (so the effect
 * resolution and the live inputs read the snapshot), passes a ViewContext on a 3D Viewport (the
 * Outliner/panel entry has none of its own), and launches `SCULPT_OT_curve_patch_edit` after it,
 * as with #curve_patch_start_from_anchor. Returns the published session, or null when the object
 * or the container cannot carry one (the same refusals the anchor entry reports).
 *
 * \a prepare_canvas runs once the session is published and before the effect snapshots the canvas,
 * with the material channels this session is going to write, so a caller can reset exactly the
 * maps the patches are about to re-render. A session that refuses to start never reaches it.
 */
CurvePatchSession *curve_patch_start_from_layer(bContext &C,
                                                Object &ob,
                                                Sculpt &sd,
                                                PaintVector &vectors,
                                                const ViewContext &vc,
                                                FunctionRef<void(Span<int>)> prepare_canvas);

/**
 * Publish a Curve Patch session from a finished #BRUSH_STROKE_ROLL stroke. Builds the control
 * curve from the stroke's resampled contour (`control_positions`, object space, with a per-point
 * `control_radii`), after undoing the live roll relief back to a pristine baseline. From there the
 * handoff is identical to #curve_patch_start_from_anchor, including the returned "launch the modal
 * yourself" contract: it takes over ownership of `SculptSession::cache` but NOT the stroke's undo
 * transaction, which the caller discards once the modal has started; and on any early bail
 * (Dynamic Topology, degenerate input) it frees `ss.cache` itself and returns false.
 * `control_positions.size()` must equal `control_radii.size()`. `plane_normal` is the roll's
 * frozen projection normal, used as the patch's projection plane.
 */
bool roll_start_curve_patch_from_stroke(const Depsgraph &depsgraph,
                                        Object &ob,
                                        Sculpt &sd,
                                        const Brush &brush,
                                        const ViewContext &vc,
                                        Span<float3> control_positions,
                                        Span<float> control_radii,
                                        const float3 &plane_normal);

}  // namespace blender::ed::sculpt_paint
