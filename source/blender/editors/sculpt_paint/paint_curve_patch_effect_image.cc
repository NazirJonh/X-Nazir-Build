/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Image-canvas target for a Curve Patch session: mixes the brush's primary color into the active
 * Sculpt Mode image canvas by the magnitude `CurvePatchSampler` reports.
 *
 * Reuses `curve_patch_cull_nodes()` verbatim -- texpaint `PixelNode`s are index-parallel to
 * ordinary PBVH mesh nodes -- and reuses `CurvePatchSampler::sample()` verbatim. Unlike
 * `ColorEffect`, pixel positions have no pre-existing flat array to hand the sampler (a pixel's 3D
 * position only exists by barycentric derivation from a `PackedPixelRow`), so a small
 * `CurvePatchSampler` is constructed per pixel-row chunk instead of once per restamp -- mirroring
 * the per-chunk shape `do_paint_pixels()` already uses for the same class of problem.
 *
 * The unit of work is a CHUNK (a contiguous run of at most 512 pixels within one `PackedPixelRow`)
 * rather than a single pixel, and that is a correctness requirement, not an optimization:
 * `read_image_pixels()`'s float overload converts the buffer to scene-linear IN PLACE and hands
 * back a span aliasing the image itself, so every pixel that was read must be written back or the
 * image is left holding half-converted data. PHASE 2 therefore rewrites each chunk whole, with the
 * unaccepted pixels carrying their unchanged originals -- exactly the invariant
 * `do_paint_pixels()` maintains by bailing out before the read when a whole chunk is rejected.
 */

#include <algorithm>
#include <cstdio> /* `printf`/`fflush` for `CURVE_PATCH_PROFILING`. */
#include <optional>
#include <string>
#include <utility>

#include "paint_curve_patch_effect.hh"

#include "DNA_image_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.hh"
#include "BKE_colortools.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_wrappers.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"
#include "BKE_paint_types.hh"
#include "BKE_undo_system.hh"

#include "DEG_depsgraph.hh"

#include "BLI_array.hh"
#include "BLI_assert.h"
#include "BLI_bit_vector.hh"
#include "BLI_bounds.hh"
#include "BLI_enumerable_thread_specific.hh"
#include "BLI_execution_mode.hh"
#include "BLI_hash.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_set.hh"
#include "BLI_task.h"
#include "BLI_task.hh"
#include "BLI_time.h" /* `BLI_time_now_seconds()` for `CURVE_PATCH_PROFILING`. */
#include "BLI_vector.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "ED_paint.hh"
#include "ED_undo.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "paint_curve_patch_effect_common.hh"
#include "paint_intern.hh"

#include "mesh/mesh_brush_common.hh"
#include "mesh/paint_material_blend.hh"
#include "mesh/paint_material_source.hh"
#include "mesh/sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

using paint::image::ImageData;
using paint::image::TileColorspaceProcessor;

namespace {

/** Side of one snapshot tile, in pixels. Deliberately the same 64 as the real image-undo system's
 * tile, so the snapshot's memory behaves like the undo tiles it sits alongside. */
constexpr int tile_size = ED_IMAGE_UNDO_TILE_SIZE;

/** Name this effect's own `ImageUndoStep` is opened under. Shared by the initial open and the
 * reopen in #ImageColorEffect::ensure_undo_step_live so the two cannot drift apart. */
constexpr const char *undo_step_name = "Curve Patch Image";


/** Identifies one `tile_size` x `tile_size` snapshot tile within one UDIM tile of one image.
 *
 * `image` is carried even though the sculpt image canvas resolves to exactly one `Image *` at a
 * time (see #curve_patch_effect_image_create): it costs one pointer per touched tile and makes the
 * key self-describing, so a stale entry can never be silently matched against a different canvas.
 * The real defense against a mid-session canvas swap is #ImageColorEffect::canvas_matches. */
struct TileKey {
  const Image *image;
  bke::image::TileNumber tile_number;
  int tile_x;
  int tile_y;

  uint64_t hash() const
  {
    return get_default_hash(image, tile_number, tile_x, tile_y);
  }

  friend bool operator==(const TileKey &a, const TileKey &b)
  {
    return a.image == b.image && a.tile_number == b.tile_number && a.tile_x == b.tile_x &&
           a.tile_y == b.tile_y;
  }
};

/** One snapshot tile's pre-patch contents.
 *
 * `pixels` is scene-linear, row-major within the tile and always exactly `tile_size * tile_size`
 * long; `captured` marks which of those slots hold a real original rather than uninitialized
 * space. The bitmap is what makes a genuine `{0, 0, 0, 0}` original distinguishable from a
 * never-touched slot -- with a zero sentinel such a pixel would be re-captured on a later pass,
 * after it had already been painted, and `restore()` would then put the PAINTED color back. It is
 * also what lets `restore()` write back exactly the painted pixels instead of stamping the whole
 * tile (a tile enters the map the moment ONE of its pixels is painted).
 *
 * Tile granularity rather than per-pixel keeps the map's overhead bounded the way
 * `ED_image_paint_tile_push()`'s own tiles already do; a per-pixel `Map` has no bound comparable
 * to `ColorEffect::orig_colors_`'s `verts_num`/`corners_num`. */
struct TileSnapshot {
  Array<float4> pixels;
  BitVector<> captured;
  /** One bit per tile ROW, set whenever any slot in that row is captured. A pure summary of
   * `captured`, maintained beside it, so `restore()` can skip an untouched row in O(1) instead of
   * testing its `tile_size` slots -- a tile enters the map on its FIRST painted pixel, so most
   * rows of most tiles stay empty for the whole session.
   *
   * A summary rather than `bits::any_bit_set()` over a row slice of `captured`: only row 0's slice
   * satisfies #is_bounded_span (`BLI_bit_span.hh:224-243` rejects an offset >= `BitsPerInt`), so
   * every later row would fall into `any_set_expr()`'s bit-at-a-time fallback
   * (`BLI_bit_span_ops.hh:100-110`) and test the same `tile_size` bits the loop already tests. */
  BitVector<> captured_rows;

  /** Cross-pass blend accumulator for this tile's slots: `x` the running sum of each claiming
   * pass's falloff weight, `y` the running sum of `weight * value` -- the same pair, with the same
   * meaning, that #curve_patch_blend_across_passes keeps in `CurvePatchApplyState`.
   *
   * Held here instead of in that shared `Map` because the image effect's key is one per PIXEL, so
   * the map grew to one entry per painted texel (millions) and cost a hash lookup each, in a
   * SERIAL loop. The snapshot tile this pixel already resolved to indexes it directly.
   *
   * A slot holds live data only where #accum_claimed is set, which in turn is only meaningful once
   * #accum_gen matches the effect's current restamp generation; see those fields.
   * Sized with #pixels, so a slot's accumulator is always addressable once the tile exists. */
  Array<float2> accum;
  /**
   * Which restamp this tile's #accum_claimed was last reset for.
   *
   * Blending is only meaningful between passes of the SAME restamp, so the accumulator has to
   * start empty every restamp -- what `pass_weight_accum.clear()` did for the map. Stamping the
   * TILE rather than each slot keeps that reset to one 512-byte #accum_claimed clear per tile per
   * restamp, instead of a 4-byte stamp beside every one of the `tile_size * tile_size` slots.
   *
   * Zero is never a live generation (the effect's counter pre-increments), so a freshly allocated
   * tile always compares stale and gets its bitmap cleared before first use.
   */
  uint32_t accum_gen = 0;
  /** Which #accum slots a pass of the CURRENT restamp has already claimed; meaningful only once
   * #accum_gen matches. A clear bit means the slot's #accum holds nothing this restamp owns, which
   * is the state a missing map entry represented. */
  BitVector<> accum_claimed;
};

class ImageColorEffect : public CurvePatchEffect {
 public:
  ImageColorEffect(PaintModeSettings &paint_mode_settings, std::string canvas_key);
  ~ImageColorEffect() override;

  void session_undo_begin() override;
  int64_t element_num(Object &ob) const override;
  void restore(Object &ob, const CurvePatchSession &patch) override;
  void sync_targets(Object &ob) override;
  void begin_restamp(const Depsgraph &depsgraph, Object &ob, CurvePatchSession &patch) override;
  void apply_pass(const Depsgraph &depsgraph,
                  Object &ob,
                  const Brush &brush,
                  CurvePatchSession &patch,
                  const CurvePatchItem &item) override;
  UpdateType update_type() const override
  {
    return UpdateType::Image;
  }
  void end_restamp(Object &ob, CurvePatchSession &patch) override;
  void commit(const Scene &scene,
              const Depsgraph &depsgraph,
              Object &ob,
              const CurvePatchSession &patch) override;
  int64_t snapshot_size() const override;
  /* This effect blends through `TileSnapshot::accum`, not the shared map the default counts, so
   * `displaced` would read zero without this. See #painted_slots_. */
  std::optional<int64_t> displaced_size() const override
  {
    return painted_slots_;
  }

 private:
  /** True when the resolved canvas (Single Image or Texture Slots, including which `Image *` it
   * names) still matches what the session started on. Mirrors `ColorEffect::attribute_matches` --
   * the modal passes events through, so the Paint Mode canvas panel is reachable mid-session. */
  bool canvas_matches(Object &ob) const;

  /** Re-open this effect's `ImageUndoStep` if the one `session_undo_begin()` opened is no longer
   * the transaction in flight. Must be called before every `do_push_undo_tile()`. */
  void ensure_undo_step_live();

  /** Raw pointer, not owned: `scene->toolsettings->paint_mode`, stable for the scene's lifetime.
   */
  PaintModeSettings *paint_mode_settings_;
  /** `BKE_paint_canvas_key_get()`'s result at session start. One string covers both Single Image
   * and Texture Slots, so no separate per-mode identity check is needed. */
  std::string canvas_key_;

  /** Lazily-grown snapshot of original (pre-patch) pixels. This is what `restore()` writes back;
   * it is deliberately NOT the same mechanism as the `ED_image_undo` step this effect opens, which
   * serves real Ctrl+Z and has a different lifetime. */
  Map<TileKey, TileSnapshot> orig_tiles_;

  /** Whether this effect has opened its own `ImageUndoStep`. Set by `session_undo_begin()`, which
   * the handoff calls once after aborting the spawning stroke's transaction; the destructor closes
   * the step only if this is set. A session torn down before the hook ever ran closes nothing. */
  bool undo_step_open_ = false;

  /** Whether the gated `do_push_undo_tile()` wave in `apply_pass()` has ever run over a non-empty
   * node mask, i.e. whether the open step can hold any "before" tile at all. False for a session
   * that only ever stamped the initial preview (which runs before `session_undo_begin()`), and for
   * one whose every restamp culled all nodes. See the destructor. */
  bool undo_tiles_pushed_ = false;

  /**
   * Nodes whose "before" tiles this effect has already pushed into the open `ImageUndoStep`,
   * keyed by the canvas `Image` they were pushed for (one node covers different tiles on each
   * material channel's image, so the node index alone is not a key).
   *
   * A second push of an already-captured tile is a no-op inside the undo system -- but not a free
   * one: #ED_image_paint_tile_push takes the undo system's single global `paint_tiles_lock` before
   * it can even look the tile up (`space_image/image_undo.cc:215-232`), and the push wave runs
   * from a `grain_size(1)` parallel loop, so every worker serializes on that one lock. An
   * interactive session re-stamps continuously over a tile set that only grows, so from the second
   * re-stamp on nearly the whole wave is that lock and nothing else.
   *
   * Skipping is only sound while the captured tiles stay valid, so two events clear this:
   * - #ensure_undo_step_live reopening the step, which leaves the earlier tiles behind in a step
   *   we no longer own;
   * - a `PixelNode` rebuild in #begin_restamp, which replaces `PixelNode::undo_regions` and so may
   *   move a node onto tiles that were never captured.
   */
  Set<std::pair<const Image *, int>> undo_pushed_nodes_;

  /** Restamp counter stamped into `TileSnapshot::accum_gen`, pre-incremented once per restamp by
   * #begin_restamp. Starts at 0 so that the first restamp stamps 1 and a freshly allocated tile's
   * default `accum_gen` of 0 can never be mistaken for the current restamp. */
  uint32_t restamp_generation_ = 0;

  /** Distinct texels this restamp has written, i.e. slots whose accumulator #begin_restamp's
   * generation bump invalidated and this restamp re-initialized. Reported as `displaced`/`pixels`,
   * the role `pass_weight_accum.size()` played before the accumulator moved into the snapshot.
   * See #displaced_size. */
  int64_t painted_slots_ = 0;

  /** Whether patch pixels are currently ON the canvas: set when `apply_pass()` writes a chunk,
   * cleared when `restore()` completes a full write-back. It is the destructor's "is this a commit
   * or a cancel?" signal -- the effect is destroyed on both and is told neither. See the
   * destructor for the four teardown paths and what this reads as on each. */
  bool patch_pixels_on_canvas_ = false;

#if CURVE_PATCH_PROFILING
  /* DEBUG-cpatch-image: per-restamp wall-clock accumulators (seconds), reset at the top of
   * `begin_restamp()` and reported as one line at the end of `end_restamp()`. Every restamp calls
   * `apply_pass()` once per symmetry pass x patch, so these SUM those calls; `prof_pass_count_`
   * records how many actually ran past the guards. */
  double prof_build_pixels_ = 0.0; /* `bke::pbvh::build_pixels()` in `begin_restamp()`. */
  double prof_cull_ = 0.0;         /* `curve_patch_effect_node_mask()`. */
  /* Deliberately two accumulators rather than one: the two waves scale with different things --
   * the fetch is a per-tile map hit, while the undo push serializes every node on the undo
   * system's single `paint_tiles_lock`. A combined column cannot tell those apart, which is
   * exactly the question a restamp that grows with tile count poses. */
  double prof_fetch_ = 0.0;        /* `fetch_image_buffers()` wave. */
  double prof_undo_push_ = 0.0;    /* `do_push_undo_tile()` wave. */
  double prof_phase1_ = 0.0;       /* Parallel sample + read-pixels (wall-clock elapsed). */
  double prof_phase2_ = 0.0;       /* Serial snapshot + mix + write-back. */
  /* Split of `prof_phase2_`: the chunk write-back alone, so the remainder is the per-pixel
   * snapshot/blend loop. The loop does a `pass_weight_accum` hash lookup per PIXEL, which is the
   * one cost in this phase that cannot be hoisted without restructuring; this column is what says
   * whether that is worth doing. */
  double prof_p2_write_ = 0.0;
  /* PHASE 1 split, summed across threads (CPU-time; sums exceed `prof_phase1_`). */
  /* The INTERNAL CONTROL of every PHASE 1 measurement: the barycentric position/normal/mask
   * interpolation runs once per candidate pixel in the same loop as the patch sampling, and no
   * ribbon/LUT optimization touches it. Its per-candidate cost must hold steady across runs; when
   * it does not, the two runs drew different enough curves that `patch`/`cand` cannot be compared
   * either. */
  double prof_p1_interp_ = 0.0;
  double prof_p1_sample_ = 0.0;     /* The `sampler.sample()` loop alone. */
  double prof_p1_read_ = 0.0;       /* `read_image_pixels()` on accepted chunks. */
  int64_t prof_candidates_ = 0;     /* Pixels sampled (vs the accepted `pixels` count). */
  int64_t prof_culled_ = 0;         /* Pixels in chunks skipped by the broad-phase cull. */
  int64_t prof_reached_lut_ = 0;    /* Passed cheap culls, reached the ribbon/frames LUT. */
  int64_t prof_reached_relief_ = 0; /* LUT returned >=1 branch. */
  int64_t prof_tex_evals_ = 0;      /* paint_get_tex_pixel calls. */
  int64_t prof_shaded_ = 0;         /* Accepted pixels that sampled a material channel source. */
  /* Which test in `branch_relief()` threw the work away. A counter rather than a timer on purpose
   * -- a clock call costs more than the step it would time. */
  CurvePatchSampler::BranchFunnel prof_branch_funnel_;
  int prof_pass_count_ = 0;
#endif
};

ImageColorEffect::ImageColorEffect(PaintModeSettings &paint_mode_settings, std::string canvas_key)
    : paint_mode_settings_(&paint_mode_settings), canvas_key_(std::move(canvas_key))
{
  /* Deliberately opens NO undo step. The stroke that spawns this session already opened an
   * `ImageUndoStep` of its own: `stroke_undo_begin()` (`mesh/sculpt.cc:5696-5712`) takes the
   * `ED_image_undo_push_begin()` branch for exactly this brush configuration -- a Paint brush for
   * which `SCULPT_use_image_paint_brush()` is true -- which is the same configuration that selects
   * this effect. That step is still OPEN when `curve_patch_begin_editing()` constructs this object
   * (`paint_curve_patch_session.cc:680`), so opening a second one here would free the first's
   * `step_init` (`undo_system.cc:491-494`). Worse, the handoff then calls
   * `BKE_undosys_step_push_init_abort()` to discard the spawning stroke's transaction -- at
   * `mesh/sculpt.cc:6140` for the anchor handoff and at `:6183` for the Roll bridge, which reaches
   * `curve_patch_begin_editing()` just as live a way -- which would free the step this constructor
   * had just created and leave `step_init` null. Every later `do_push_undo_tile()` would then trip
   * the `us_p == us_prev` assert in `ED_image_paint_tile_map_get()`
   * (`space_image/image_undo.cc:1053`) and, in a Release build, dereference null at `:1056`.
   *
   * The step is opened from `session_undo_begin()` instead, which the handoff calls after that
   * abort. */
}

void ImageColorEffect::session_undo_begin()
{
  /* Opening here rather than in the constructor or lazily in `apply_pass()` is what makes the step
   * survive: both of those run inside the handoff, BEFORE `BKE_undosys_step_push_init_abort()`
   * destroys whatever transaction is in flight. `curve_patch_begin_editing()` stamps an initial
   * preview synchronously (`paint_curve_patch_session.cc:697` -> `:554` -> `:201` ->
   * `apply_pass()`), so even "the first `apply_pass()`" still runs inside the handoff --
   * `curve_patch_start_from_anchor()` at `mesh/sculpt.cc:6130` for the anchor path, or
   * `roll_start_curve_patch_from_stroke()` at `:6172` for the Roll bridge -- and therefore still
   * before that handoff's abort, at `:6140` and `:6183` respectively. See the base class's
   * `session_undo_begin()` doc comment.
   *
   * Called exactly once per session, but guarded anyway so a second call cannot orphan the first
   * step. */
  if (undo_step_open_) {
    return;
  }
  ED_image_undo_push_begin(undo_step_name, PaintMode::Sculpt);
  undo_step_open_ = true;
  /* Defensive: the memo tracks captures in the step opened here, and the preview passes that ran
   * before this point pushed nothing (they are gated on `undo_step_open_`). See
   * #undo_pushed_nodes_. */
  undo_pushed_nodes_.clear();
}

void ImageColorEffect::ensure_undo_step_live()
{
  BLI_assert(undo_step_open_);

  /* The step `session_undo_begin()` opened stays open for the whole modal session, and the modal
   * deliberately passes events through (so that, for instance, editing a brush property in a panel
   * works mid-session). Any foreign undo push in that window takes our step away, by either of two
   * routes, and NEITHER clears `undo_step_open_`:
   * - `BKE_undosys_step_push_init_with_type()` frees the existing `step_init` and installs its own
   *   (`undo_system.cc:491-494`);
   * - a plain `BKE_undosys_step_push_with_type()` ADOPTS `step_init`, retypes it to the pusher's
   *   own type and commits it (`undo_system.cc:590-598`), leaving `step_init` null. (That route is
   *   also lossy upstream, independently of anything here: the adopted buffer was allocated at
   *   `sizeof(ImageUndoStep)` (`image_undo.cc:1028`) but is then encoded and, eventually, freed as
   *   the pusher's type, so our `PaintTileMap` and `UndoImageHandle`s leak.)
   *
   * Either way the next `do_push_undo_tile()` would reach `ED_image_paint_tile_map_get()`
   * (`space_image/image_undo.cc:1045-1059`, called per tile from `push_undo()` in
   * `mesh/sculpt_paint_image.cc`), where `BKE_undosys_stack_init_or_active_with_type()`
   * (`undo_system.cc:408-415`) no longer returns `step_init` and falls through to
   * `BKE_undosys_stack_active_with_type()`. Both of its outcomes are broken:
   * - no committed IMAGE step on the stack -> null `us`, which aborts on
   *   `BLI_assert(us_p == us_prev)` (`:1053`) whenever `step_init` was replaced rather than
   *   emptied, and dereferences null at `:1056` or, failing that, at `:1058`;
   * - an older committed IMAGE step -> the assert fires and our "before" tiles are pushed into a
   *   step that was already committed, corrupting an unrelated point in the undo history.
   *
   * WHAT THE CHECK BELOW PROVES: that a transaction is in flight AND that its type is
   * `BKE_UNDOSYS_TYPE_IMAGE`. That is exactly -- and only -- the condition
   * `BKE_undosys_stack_init_or_active_with_type()` tests, so when it holds the tile-map lookup
   * returns `step_init`'s own map with `us_p == us_prev` and cannot crash.
   *
   * WHAT IT DOES NOT PROVE: that the step is the very allocation we opened. `UndoStep` carries no
   * identity beyond its type and a 64-byte name (`BKE_undo_system.hh:76-79`), and a freed step's
   * address can be reused, so pointer identity is not sound either. A foreign in-flight IMAGE step
   * would pass this check and receive our tiles. That is deliberately preferred to the
   * alternative, but it is NOT harmless, and the honest comparison is between two bad outcomes:
   * reopening on a name mismatch would FREE a live foreign image-paint transaction outright, while
   * sharing one mixes both strokes' "before" data into a single step AND, when this session ends,
   * our destructor's `ED_image_undo_push_end()` commits the foreign stroke's step out from under
   * it -- so that stroke's next tile push hits the same broken lookup this guard exists to
   * prevent. The name is no way out: `undo_system.cc:595-596` copies the pusher's name only into
   * an EMPTY one, so an adopted step keeps ours and the name is unreliable in both directions.
   *
   * The reopen path has its own destructive case, for the same unavoidable reason: when
   * `step_init` holds a live FOREIGN NON-image transaction, `ED_image_undo_push_begin()` frees it
   * (`undo_system.cc:491-494`). There is no API to open a step without displacing whatever is
   * there; a crash is the worse outcome.
   *
   * Nor does the check prove our earlier tiles survived -- if the step was freed, every "before"
   * tile pushed before that point is gone with it and only the pixels touched from here on stay
   * undoable.
   *
   * COST OF REOPENING: the session's pixels end up split across more than one `ImageUndoStep`, so
   * reverting the whole session takes several Ctrl+Z presses instead of one. That is the accepted
   * trade against the crash above. */
  const UndoStack *ustack = ED_undo_stack_get();
  const UndoStep *step_init = ustack != nullptr ? ustack->step_init : nullptr;
  if (step_init != nullptr && step_init->type == BKE_UNDOSYS_TYPE_IMAGE) {
    return;
  }
  ED_image_undo_push_begin(undo_step_name, PaintMode::Sculpt);
  /* The tiles captured so far live in the step we just lost, so nothing is captured in the new one
   * and every node must be pushed again. See #undo_pushed_nodes_. */
  undo_pushed_nodes_.clear();
}

ImageColorEffect::~ImageColorEffect()
{
  /* `CurvePatchSession` (which owns this effect through a `unique_ptr`) is destroyed on four
   * paths, only some of which call `commit()`:
   * - `curve_patch_edit_finish()` (`paint_curve_patch_edit.cc:1614`) -- the PRIMARY one, serving
   *   both the modal's commit branch (`:1602`) and its Esc-cancel branch (`:1582`, which never
   *   calls `commit()`);
   * - `curve_patch_commit_on_session_end()` (`paint_curve_patch_session.cc:115`), after
   * `commit()`;
   * - `curve_patch_discard_on_session_end()` (`:169`), which never calls `commit()`;
   * - `curve_patch_begin_editing()`'s refusal branch (`:684`), which cannot reach this destructor
   *   at all: it is taken only when `curve_patch_effect_create()` returned null, so no
   *   `ImageColorEffect` was ever constructed there.
   * The destructor is therefore the one place that runs on every exit that had an effect, which is
   * why the step is closed here rather than in `commit()`.
   *
   * Conditional, unlike `stroke_undo_end()`'s unconditional call: that one pairs with an open it
   * knows happened, whereas this effect's open lives in `session_undo_begin()`, which is called by
   * the handoff strictly after this object is constructed. `undo_step_open_` is therefore the only
   * thing that knows whether a step exists, and a session torn down before the hook ran must close
   * nothing.
   *
   * `ED_image_undo_push_end()` is a bare `BKE_undosys_step_push()` on the global stack
   * (`space_image/image_undo.cc:1139-1145`), so calling it without a matching open would push
   * whatever unrelated `step_init` happened to be in flight, or manufacture a spurious step.
   *
   * Still exactly one close after `ensure_undo_step_live()` may have re-opened the step
   * mid-session. That helper neither sets nor clears `undo_step_open_`, and it re-opens ONLY when
   * our previous step is already gone -- freed by a foreign `push_init`, or adopted and committed
   * by a foreign push (see its own comment). So at most one step of ours is ever in flight, this
   * closes that last-opened one, and the earlier ones are already committed steps on the stack.
   *
   * CLOSED TWO WAYS, because a step is only worth committing when it would actually undo
   * something. `image_undosys_step_encode()` (`space_image/image_undo.cc:791-882`) pairs each
   * pushed tile's "before" data with the buffer's CURRENT content, so a step whose pixels have
   * since been put back encodes `pre == post`: pushing it costs the user a slot on the undo stack,
   * truncates the redo branch (`undo_system.cc:606-611`) and silently swallows their next Ctrl+Z,
   * while undoing nothing. `BKE_undosys_step_push_init_abort()` discards such a step instead.
   *
   * The two flags below answer that question without the destructor being told which teardown it
   * is on -- and it is told nothing. "`commit()` ran" would not answer it either: a patch whose
   * mesh was invalidated skips `commit()` while leaving its pixels on the canvas. The four
   * teardown paths read as:
   * - modal commit (`paint_curve_patch_edit.cc:1602` -> `:1614`): the final re-stamp restored and
   *   then repainted, so pixels are on the canvas and the wave pushed their "before" data ->
   *   PUSHED, exactly as before this guard existed;
   * - session-end commit (`paint_curve_patch_session.cc:108` -> `:115`): identical sequence ->
   * PUSHED;
   * - modal cancel (`paint_curve_patch_edit.cc:1582` -> `:1614`): `curve_patch_restore_only()` ran
   *   and put every captured pixel back, so the step would encode `pre == post` -> ABORTED;
   * - session-end discard (`paint_curve_patch_session.cc:156` -> `:169`): same restore -> ABORTED.
   * Neither invalidated-mesh variant reaches a completed write-back, so both keep the flag set and
   * PUSH -- which is right, because their pixels really are still on the canvas and must stay
   * undoable. `edit.cc:1599` skips the commit after `curve_patch_restore_and_restamp()` has
   * already bailed out on its own element-count guard (`paint_curve_patch_session.cc:206-215`)
   * BEFORE restoring anything, and `cache.cc:105`'s `curve_patch_restore_only()` reaches
   * `restore()` only for it to return on the identical guard.
   *
   * The abort is guarded by the same "an IMAGE transaction is in flight" test
   * `ensure_undo_step_live()` uses, so a foreign NON-image transaction that took our slot is never
   * destroyed. Be precise about what that does and does not buy: a foreign IMAGE-paint transaction
   * PASSES this test and would be aborted. That is accepted rather than solved, because the branch
   * this replaced would have called `ED_image_undo_push_end()` on that same foreign step and
   * committed it out from under its owner -- comparably destructive, and there is no test that can
   * tell their in-flight image step from ours (see `ensure_undo_step_live()`'s own comment for why
   * neither the pointer nor the name is sound). When the test fails there is nothing of ours left
   * to abort and nothing to do. The push branch is deliberately left unguarded, exactly as it was.
   *
   * One knowingly accepted loss wherever the abort branch is taken:
   * `fix_non_manifold_seam_bleeding()` writes pixels outside the patch that `restore()` does not
   * capture (see the NOTE in `restore()`), so the discarded step would have been able to undo that
   * residue and now cannot. This is not confined to cancellation -- a COMMIT whose final restamp
   * accepted no pixels (the patch was dragged clear of the mesh) also lands here, with earlier
   * restamps' residue still on the canvas. A dead step that eats the user's next Ctrl+Z is the
   * worse of the two. */
  if (!undo_step_open_) {
    return;
  }
  if (undo_tiles_pushed_ && patch_pixels_on_canvas_) {
    ED_image_undo_push_end();
    return;
  }
  UndoStack *ustack = ED_undo_stack_get();
  const UndoStep *step_init = ustack != nullptr ? ustack->step_init : nullptr;
  if (step_init != nullptr && step_init->type == BKE_UNDOSYS_TYPE_IMAGE) {
    BKE_undosys_step_push_init_abort(ustack);
  }
}

/**
 * The image canvas this effect paints into.
 *
 * The stroke cache carries one #ImagePaintTarget per painted material channel. Curve Patch writes
 * the primary one, which is also the target `sculpt_pbvh_update_pixels()` builds the PBVH pixel
 * encoding from -- so the pixel rows this effect walks address exactly this image. Painting every
 * enabled channel is Curve Patch stage 3.1.
 */
static paint::image::ImageData *curve_patch_primary_image_data(const SculptSession *ss)
{
  if (ss == nullptr || ss->cache == nullptr || ss->cache->image_paint_targets.is_empty()) {
    return nullptr;
  }
  return ss->cache->image_paint_targets[0].data.get();
}

/** Every canvas the current stroke paints, keyed by its #Image so a snapshot tile can be routed
 * back to the buffer it was captured from. One entry for Mode=`Image`, one per enabled
 * Principled channel for Mode=`Material`. */
static Map<const Image *, paint::image::ImageData *> curve_patch_image_data_by_image(
    const SculptSession *ss)
{
  Map<const Image *, paint::image::ImageData *> by_image;
  if (ss == nullptr || ss->cache == nullptr) {
    return by_image;
  }
  for (paint::image::ImagePaintTarget &target : ss->cache->image_paint_targets) {
    if (target.data && target.data->image != nullptr) {
      /* Two channels can name the same image; the first entry wins, which is the one every
       * snapshot tile of that image was captured through. */
      by_image.add(target.data->image, target.data.get());
    }
  }
  return by_image;
}

/**
 * Whether \a image_data's tile layout is the one the PBVH pixel encoding currently describes.
 *
 * The encoding (resolution, UDIM set, seam margin, UV map) is built ONCE per restamp, from the
 * primary target -- see #ImageColorEffect::begin_restamp. Every pixel row it holds addresses
 * texels of that layout, so a channel map of a different size cannot be painted through it: the
 * rows would land on the wrong texels. The ordinary stroke engine rebuilds the encoding per
 * layout group instead (`do_paint_pixels()` in `mesh/sculpt_paint_image.cc`); a patch cannot
 * follow it there yet, because its seam fix and dirty marking run once per RESTAMP across every
 * symmetry pass, and a mid-restamp rebuild would discard the dirty state the earlier passes
 * recorded.
 *
 * So such a channel is SKIPPED rather than painted wrongly. In practice the Principled maps of
 * one material are authored at the same resolution and every channel passes.
 */
static bool curve_patch_layout_matches(const bke::pbvh::Tree &pbvh,
                                       const paint::image::ImageData &image_data,
                                       const StringRef uv_map_name)
{
  if (pbvh.pixels_ == nullptr || image_data.image == nullptr ||
      image_data.image_user == nullptr)
  {
    return false;
  }
  return pbvh.pixels_->layout_key ==
         BKE_paint_pixels_layout_key_get(*image_data.image, *image_data.image_user, uv_map_name);
}

/**
 * The flat color a channel paints RIGHT NOW.
 *
 * #ImagePaintTarget::color_override is resolved ONCE, when the stroke cache builds its targets,
 * and that is all a dab ever needs: a stroke cannot outlive a slider drag. A Curve Patch session
 * can. It stays open across panel edits -- and its live-sync poll re-stamps for exactly those --
 * so reading the cached override would re-stamp with the value the session STARTED on. The
 * Roughness slider would move and the texture would not.
 *
 * Resolved through the same BKE helpers #init_image_paint_targets uses, so the two cannot drift
 * on what a channel's paint value means.
 */
static float3 curve_patch_channel_flat_color(const paint::image::ImagePaintTarget &target,
                                             const Brush &brush,
                                             const Paint &paint,
                                             const PaintModeSettings &settings,
                                             const bool invert)
{
  const bool live_channel = target.is_material_channel && brush.material_paint != nullptr;
  if (!live_channel) {
    return target.color_override ? float3(*target.color_override) :
                                   BKE_brush_color_get(&paint, &brush);
  }
  const BrushMaterialPaint &brush_paint = *brush.material_paint;
  if (target.is_normal_channel) {
    /* The same clamp / normalize / pack #BKE_paint_material_image_targets_get and
     * #init_image_paint_targets perform between them. Inversion is deliberately not handled, for
     * the same reason it is not there: a direction has no "erase toward the default" meaning the
     * scalar channels' rule would give it. */
    const float2 range = BKE_paint_material_channel_range(settings, target.channel);
    float3 direction;
    for (const int i : IndexRange(3)) {
      direction[i] = math::clamp(
          brush_paint.channels[target.channel].value[i], range.x, range.y);
    }
    /* An all-zero direction cannot be normalized; `normalize_v3` leaves it zero and the pack
     * turns that into neutral grey, so match it rather than emitting NaNs. */
    const float direction_len = math::length(direction);
    direction = direction_len > 1e-6f ? direction / direction_len : float3(0.0f);
    float packed[3];
    BKE_pbr_normal_pack(direction, false, packed);
    return float3(packed[0], packed[1], packed[2]);
  }
  if (target.is_color_channel) {
    return BKE_paint_material_channel_color_get(brush_paint, paint, brush, target.channel, invert);
  }
  /* Scalar channel: a scalar IS the gray with that value in every component -- the convention
   * both paint engines share (#material::scalar_as_color). Erasing pulls it to the channel
   * default instead of the slider, the same rule `do_paint_material_brush()` follows. */
  const float value = invert ? BKE_paint_material_channel_default_value(target.channel) :
                               BKE_paint_material_channel_value(
                                   brush_paint, settings, target.channel);
  return float3(value);
}

/** The #Paint the spawning stroke resolved; #StrokeCache::paint is the same pointer its own init
 * used. Both the canvas key stored at session start and every later comparison of it read the
 * brush and channel mask from here, so the two cannot be computed from different inputs. */
static const Paint *curve_patch_stroke_paint(const SculptSession *ss)
{
  return (ss != nullptr && ss->cache != nullptr) ? ss->cache->paint : nullptr;
}

/** #BKE_paint_canvas_key_get for the canvas the given session is painting. */
static std::string curve_patch_canvas_key(PaintModeSettings &paint_mode_settings,
                                          Object &ob,
                                          const SculptSession *ss)
{
  const Paint *paint = curve_patch_stroke_paint(ss);
  const Brush *brush = paint != nullptr ? BKE_paint_brush_for_read(paint) : nullptr;
  const int visible_material_channels = paint != nullptr ? paint->visible_material_channels : 0;
  return BKE_paint_canvas_key_get(&paint_mode_settings, &ob, brush, visible_material_channels);
}

bool ImageColorEffect::canvas_matches(Object &ob) const
{
  return curve_patch_canvas_key(*paint_mode_settings_, ob, ob.runtime->sculpt_session) ==
         canvas_key_;
}

/** Mirrors `ColorEffect::element_num()`'s Corner-domain case: a `PixelNode`'s pixel rows are built
 * from mesh triangles (`PixelData::vert_tris`), invalidated by exactly the topology edits
 * (Triangulate, Poke, ...) that already invalidate `corners_num`. This is a STABLE count compared
 * against `CurvePatchApplyState::element_num` every restamp -- it must NOT be the snapshot's size,
 * which grows across the session. */
int64_t ImageColorEffect::element_num(Object &ob) const
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return 0;
  }
  const Mesh &mesh = *id_cast<const Mesh *>(ob.data);
  return mesh.corners_num;
}

int64_t ImageColorEffect::snapshot_size() const
{
  return orig_tiles_.size();
}

/** Writes `values` into `image_buffer` starting at absolute image coordinate `start`, reusing
 * `write_image_pixels()` so the linear-to-buffer color management, the float/byte split and the
 * destination offset arithmetic all stay in ONE place.
 *
 * `values` must be in whatever space `processors` expects as its input: scene-linear for a real
 * processor, or already in the buffer's own space when the caller passes a default-constructed
 * (`is_noop`) one because it has applied the transform itself.
 *
 * `write_image_pixels()` reads only `PackedPixelRow::start_image_coordinate`, so a row synthesized
 * from `start` addresses the intended run; the remaining fields are unused by it. It also converts
 * `values` IN PLACE, which is why every caller hands it a scratch copy rather than the snapshot.
 */
void write_pixel_run(MutableSpan<float4> values,
                     ImBuf &image_buffer,
                     const TileColorspaceProcessor &processors,
                     const int2 start)
{
  bke::pbvh::pixels::PackedPixelRow row = {};
  row.start_image_coordinate = ushort2(uint16_t(start.x), uint16_t(start.y));
  row.num_pixels = uint16_t(values.size());

  const int64_t buffer_size = int64_t(image_buffer.x) * image_buffer.y;
  if (image_buffer.float_data()) {
    const MutableSpan<float4> dst(reinterpret_cast<float4 *>(image_buffer.float_data_for_write()),
                                  buffer_size);
    paint::image::write_image_pixels(
        values, dst, processors, row, IndexRange(0, values.size()), image_buffer.x);
  }
  else {
    const MutableSpan<uchar4> dst(reinterpret_cast<uchar4 *>(image_buffer.byte_data_for_write()),
                                  buffer_size);
    paint::image::write_image_pixels(
        values, dst, processors, row, IndexRange(0, values.size()), image_buffer.x);
  }
}

void ImageColorEffect::restore(Object &ob, const CurvePatchSession & /*patch*/)
{
  /* No `element_num` check of its own -- `curve_patch_restore_only()` performs it for every
   * caller. The canvas check below is NOT redundant with it: a swapped canvas changes no mesh
   * count.
   *
   * Unlike the other two effects this one reads nothing else off the session either: the pixels it
   * writes back are addressed by its own tile snapshot, not by any PBVH node mask. */
  if (!this->canvas_matches(ob)) {
    /* The active canvas was swapped mid-session -- the keys describe a different image now. */
    return;
  }
  if (orig_tiles_.is_empty()) {
    return;
  }
  SculptSession *ss = ob.runtime->sculpt_session;
  /* Keyed by image rather than taken from one canvas: with Mode=`Material` the snapshot holds
   * tiles of every painted channel, and each has to go back through its own buffer. */
  const Map<const Image *, ImageData *> image_data_by_image = curve_patch_image_data_by_image(
      ss);
  if (image_data_by_image.is_empty()) {
    return;
  }

  /* NOTE: This restores only pixels the patch itself wrote, i.e. the ones `apply_pass()` captured.
   * `end_restamp()` additionally runs `fix_non_manifold_seam_bleeding()`, which writes pixels
   * OUTSIDE the patch on non-manifold seams; those are neither captured here nor undone here, so a
   * long drag can leave faint residue along such a seam.
   *
   * The same root cause reaches real undo as well. The initial preview restamp runs its seam fix
   * BEFORE `session_undo_begin()` opens the undo step (see the push gate in `apply_pass()`), so
   * that first pass's out-of-patch writes are already in the buffer when the first
   * `do_push_undo_tile()` captures its "before" data. They are therefore baked into the step's
   * pre-patch state, and Ctrl+Z after a commit will not remove them.
   *
   * Fixing either would mean snapshotting from inside the seam fix (or reproducing its bleed set
   * here), which is a design decision beyond the scope this effect was written to -- recorded
   * rather than papered over. */

  /* Scratch for ONE tile row's captured pixels, gathered contiguously, plus the (start column,
   * length) of each contiguous captured run that fed it. Reused across rows and tiles.
   *
   * A row's pixels are gathered so the whole row costs ONE colorspace call instead of one per run.
   * On the common non-scene-linear canvas (every 8-bit sRGB texture) `processors.is_noop` is
   * false, and a patch that touches N short runs in a row was otherwise issuing N separate OCIO
   * calls of a few pixels each -- on the main thread, every restamp, for every tile ever touched.
   * The result is unchanged: the transform is per-pixel, which the rest of this pipeline already
   * relies on (both `read_image_pixels()` and `write_image_pixels()` apply it over runs of
   * whatever length their caller happens to have, `mesh/sculpt_paint_image.cc:205-211` /
   * `:247-250`), so batching moves only where the call boundary falls. */
  Vector<float4> row_values;
  Vector<int2> row_runs;

  /* Default-constructed, so `is_noop` is true (`mesh/sculpt_intern.hh:166-170`) and
   * `write_image_pixels()` performs only the destination arithmetic and the float/byte store. The
   * linear-to-buffer transform is applied once per row above it instead. */
  const TileColorspaceProcessor noop_processors = {};

  /* Cleared by any tile the loop skips, so `patch_pixels_on_canvas_` below stays set unless the
   * write-back was complete. See there for why a partial restore must not clear it. */
  bool restored_all = true;

  for (const auto item : orig_tiles_.items()) {
    const TileKey &key = item.key;
    const TileSnapshot &snapshot = item.value;
    ImageData *key_data = image_data_by_image.lookup_default(key.image, nullptr);
    if (key_data == nullptr || key_data->image_user == nullptr) {
      /* A tile of a canvas this stroke no longer paints. Cannot happen while
       * `canvas_matches()` holds, but a stale entry must never be written through a buffer
       * belonging to a different image. */
      restored_all = false;
      continue;
    }
    /* Shadows nothing: the rest of this loop body addresses the canvas of THIS tile. */
    ImageData &image_data = *key_data;

    /* Re-acquire rather than reading `ImageData::buffers`: that map is populated per-restamp
     * inside `apply_pass()`, and `restore()` runs FIRST in every restamp. Acquisition is reference
     * counted, so this returns the same `ImBuf` the paint path uses. */
    ImageUser tile_user = *image_data.image_user;
    tile_user.tile = key.tile_number;
    ImBuf *image_buffer = BKE_image_acquire_ibuf(image_data.image, &tile_user, nullptr);
    if (image_buffer == nullptr) {
      restored_all = false;
      continue;
    }

    /* Every tile in the snapshot was created by a write that had a valid processor, so this lookup
     * should always succeed. Skip rather than fall back to a bare conversion: a bare conversion
     * would silently write wrong colors on exactly the color-managed images this lookup exists
     * for. */
    const TileColorspaceProcessor *processors = image_data.processors.lookup_ptr(key.tile_number);
    if (processors == nullptr) {
      BLI_assert_unreachable();
      BKE_image_release_ibuf(image_data.image, image_buffer, nullptr);
      restored_all = false;
      continue;
    }

    /* Region actually written back, accumulated so the image can be marked for a PARTIAL update.
     * `end_restamp()` only marks the nodes THIS restamp painted, and a patch dragged away from a
     * region restores it without repainting it -- without this the screen would keep showing the
     * patch where it no longer is.
     *
     * The `+1` past the run's one-past-end column below is deliberate and is the in-tree
     * convention, not an off-by-one: `PixelNode::rebuild_undo_regions()` bounds a row with
     * `start.x + num_pixels + 1` / `start.y + 1` (`BKE_paint_bvh_pixels.hh`), and
     * `do_paint_pixels()` uses the same `start + int2(num_pixels + 1, 0)`
     * (`mesh/sculpt_paint_image.cc`). Both are one column over-inclusive on purpose; matching them
     * keeps every partial-update region in this pipeline computed the same way. */
    rcti restored_region;
    BLI_rcti_init_minmax(&restored_region);

    bool wrote_anything = false;
    for (const int row : IndexRange(tile_size)) {
      const int abs_y = key.tile_y * tile_size + row;
      if (abs_y >= image_buffer->y) {
        break;
      }
      if (!snapshot.captured_rows[row]) {
        /* Nothing in this row was ever painted. Checked before the slot scan rather than by it:
         * the map holds every tile a SINGLE pixel landed in, so across a long drag most rows of
         * most tiles are empty and this is the loop's dominant cost. */
        continue;
      }
      /* Write back exactly the captured slots and nothing else. A tile enters the snapshot as soon
       * as one of its pixels is painted, so restoring whole rows would stamp uninitialized data
       * over every pixel the patch never touched. */
      row_values.clear();
      row_runs.clear();
      int run_start = -1;
      for (int col = 0; col <= tile_size; col++) {
        const int abs_x = key.tile_x * tile_size + col;
        const bool captured = col < tile_size && abs_x < image_buffer->x &&
                              snapshot.captured[row * tile_size + col];
        if (captured) {
          if (run_start == -1) {
            run_start = col;
          }
          row_values.append(snapshot.pixels[row * tile_size + col]);
          continue;
        }
        if (run_start != -1) {
          row_runs.append(int2(run_start, col - run_start));
          run_start = -1;
        }
      }
      if (row_runs.is_empty()) {
        /* Reachable despite `captured_rows`: the slots may all sit past `image_buffer->x`. */
        continue;
      }

      /* The one colorspace call this row costs. `row_values` is scratch, so converting it in place
       * is safe -- the snapshot itself is never handed to a processor, or each restore would
       * compound the transform. */
      if (!processors->is_noop) {
        processors->linear_to_buffer_processor.apply(
            reinterpret_cast<float *>(row_values.data()), int(row_values.size()), 1, 4, false);
      }

      int64_t values_start = 0;
      for (const int2 &run : row_runs) {
        const int run_x = key.tile_x * tile_size + run.x;
        write_pixel_run(row_values.as_mutable_span().slice(values_start, run.y),
                        *image_buffer,
                        noop_processors,
                        int2(run_x, abs_y));
        BLI_rcti_do_minmax_v(&restored_region, int2(run_x, abs_y));
        BLI_rcti_do_minmax_v(&restored_region, int2(run_x + run.y + 1, abs_y + 1));
        values_start += run.y;
        wrote_anything = true;
      }
    }

    if (wrote_anything) {
      BKE_image_mark_dirty(image_data.image, image_buffer);
      if (const ImageTile *image_tile = BKE_image_get_tile(image_data.image, key.tile_number)) {
        BKE_image_partial_update_mark_region(
            image_data.image, image_tile, image_buffer, &restored_region);
      }
    }
    BKE_image_release_ibuf(image_data.image, image_buffer, nullptr);
  }

  /* Cleared only when EVERY captured pixel is back to its pre-patch value, so that nothing of the
   * patch is left on the canvas until the next `apply_pass()` puts it there.
   *
   * Both ways of leaving a pixel painted must keep the flag set, because the destructor reads it
   * to decide between committing the undo step and aborting it, and aborting one that still
   * describes live painted pixels destroys the user's ability to undo them:
   * - every early return above leaves the canvas entirely as it found it (that is what keeps the
   *   invalidated-mesh teardown, where `restore()` returns on the element-count guard with the
   *   patch still painted, committing its step);
   * - a `continue` inside the loop skips ONE tile while restoring the others, which the flag alone
   *   cannot express -- hence `restored_all`. The reachable case is an `ImBuf` that a foreign
   *   operator invalidated mid-session, which the pass-through modal permits. */
  if (restored_all) {
    patch_pixels_on_canvas_ = false;
  }
}

void ImageColorEffect::sync_targets(Object &ob)
{
  SculptSession *ss = ob.runtime->sculpt_session;
  if (ss == nullptr || ss->cache == nullptr || ss->cache->paint == nullptr) {
    return;
  }
  std::string live_key = curve_patch_canvas_key(*paint_mode_settings_, ob, ss);
  if (live_key == canvas_key_) {
    return;
  }
  /* The destination set changed. Only a PBR channel toggle can do that without the user touching
   * the canvas panel, and that is exactly the interaction this follows: the targets were resolved
   * once by `stroke_cache_init()` (`init_image_paint_targets`), and a session that outlives panel
   * edits has to re-resolve them or a channel switched on mid-session never gets a canvas while a
   * channel switched off keeps receiving paint.
   *
   * Rebuilding through the very same helper the stroke path uses is what keeps the two from
   * drifting on which channels are targets at all. `restore()` has already run (see
   * #CurvePatchEffect::sync_targets), so the images being dropped here are pristine.
   *
   * An empty result is adopted like any other: with every channel switched off the patch paints
   * nothing, and `apply_pass()` / `restore()` already treat an empty target set that way. */
  StrokeCache &cache = *ss->cache;
  const Paint &paint = *cache.paint;
  cache.image_paint_targets = paint::image::init_image_paint_targets(
      ob, *paint_mode_settings_, BKE_paint_brush_for_read(&paint), paint.visible_material_channels);
  canvas_key_ = std::move(live_key);

  /* The pixel encoding is built from `targets[0]` and is keyed by that image's layout, so a
   * rebuild that changed which canvas comes first must be followed by a matching rebuild of the
   * encoding. `begin_restamp()` does that unconditionally on the same restamp, right after this,
   * so nothing more is needed here -- recorded because it is the non-obvious half of why
   * re-targeting between `restore()` and the re-stamp is safe. */
}

void ImageColorEffect::begin_restamp(const Depsgraph &depsgraph,
                                     Object &ob,
                                     CurvePatchSession & /*patch*/)
{
  /* Invalidate every tile's cross-pass accumulator for the restamp that starts here, the reset
   * `pass_weight_accum.clear()` performs for the other effects. Bumped BEFORE the early returns
   * below so a restamp that bails cannot leave the next one blending against its leftovers.
   * See `TileSnapshot::accum_gen`. */
  restamp_generation_++;
  painted_slots_ = 0;

  /* Curve Patch bypasses `do_brush_action()` entirely for its anchor dab, so the ordinary per-dab
   * call site that builds `PixelNode` pixel data (`sculpt_pbvh_update_pixels()`) never runs for a
   * Curve Patch stroke. `bke::pbvh::build_pixels()` is cheap to call every restamp once the tables
   * exist -- it only rebuilds nodes flagged `rebuild`, the same lazy behavior an ordinary stroke's
   * own per-dab call already relies on. */
#if CURVE_PATCH_PROFILING
  /* Reset every per-restamp accumulator here, at the head of the single hook the orchestrator
   * calls once before the passes, so a restamp that later bails still starts each column clean. */
  prof_build_pixels_ = 0.0;
  prof_cull_ = 0.0;
  prof_fetch_ = 0.0;
  prof_undo_push_ = 0.0;
  prof_phase1_ = 0.0;
  prof_phase2_ = 0.0;
  prof_p2_write_ = 0.0;
  prof_p1_interp_ = 0.0;
  prof_p1_sample_ = 0.0;
  prof_p1_read_ = 0.0;
  prof_candidates_ = 0;
  prof_culled_ = 0;
  prof_reached_lut_ = 0;
  prof_reached_relief_ = 0;
  prof_tex_evals_ = 0;
  prof_shaded_ = 0;
  prof_branch_funnel_ = {};
  prof_pass_count_ = 0;
#endif
  SculptSession *ss = ob.runtime->sculpt_session;
  ImageData *image_data_ptr = curve_patch_primary_image_data(ss);
  if (image_data_ptr == nullptr) {
    return;
  }
  ImageData &image_data = *image_data_ptr;
  if (image_data.image == nullptr || image_data.image_user == nullptr) {
    return;
  }
#if CURVE_PATCH_PROFILING
  const double prof_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch-image */
#endif
  const StringRef uv_map_name =
      BKE_paint_canvas_uvmap_name_get(paint_mode_settings_, &ob).value_or("");
  /* Read BEFORE the build, which is what clears the flag. A rebuilt node gets fresh
   * `PixelNode::undo_regions` and so may land on tiles the open step never captured, which would
   * make #undo_pushed_nodes_ claim coverage that does not exist. Dropping the whole memo rather
   * than the rebuilt entries keeps this to one flag scan; the next restamp simply re-pushes, and
   * rebuilds are rare once a session is under way. */
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh != nullptr && pbvh->type() == bke::pbvh::Type::Mesh && pbvh->pixels_ != nullptr) {
    const Span<bke::pbvh::pixels::PixelNode> pixel_nodes =
        bke::pbvh::pixels::data_get(*pbvh).nodes;
    const bool any_rebuild = std::any_of(
        pixel_nodes.begin(), pixel_nodes.end(), [](const bke::pbvh::pixels::PixelNode &node) {
          return node.flags.rebuild;
        });
    if (any_rebuild) {
      undo_pushed_nodes_.clear();
    }
  }
  bke::pbvh::build_pixels(
      depsgraph, ob, *image_data.image, *image_data.image_user, uv_map_name);
#if CURVE_PATCH_PROFILING
  prof_build_pixels_ += BLI_time_now_seconds() - prof_t0; /* DEBUG-cpatch-image */
#endif
}

/** One accepted pixel inside a chunk. `local` indexes `ChunkWrite::values`, i.e. it is relative to
 * the chunk's own `range`, not to the pixel row. */
struct AcceptedPixel {
  int local;
  float4 tex_color;
  /** Whether #tex_color is a real sample -- #CurvePatchSample::tex_valid. */
  bool tex_valid;
  float value;
  float weight;
  /** What the channel's own PBR source supplies at this texel: RGB for a color channel, the value
   * replicated into all three for a scalar one, and the 0..1 PACKED tangent-space direction for
   * Normal. Equals the channel's slider color/value when the channel has no usable source, so
   * PHASE 2 can use it unconditionally. */
  float3 source_color;
  /** The Alpha channel sampled at this texel, or 1 when Alpha masking is off or does not apply
   * to this channel (Alpha never masks its own write -- #channel_uses_alpha_mask). */
  float alpha_factor;
};

/** One chunk of a `PackedPixelRow` that PHASE 1 accepted at least one pixel in.
 *
 * `values` starts out holding the chunk's scene-linear ORIGINALS and is turned into the chunk's
 * final contents by PHASE 2, which then writes it back whole. Carrying the unaccepted pixels along
 * unchanged is what keeps the float path -- whose read converted the image buffer in place -- from
 * leaving half-converted pixels behind. */
struct ChunkWrite {
  bke::pbvh::pixels::UDIMTilePixels *tile_data;
  bke::image::TileNumber tile_number;
  ImBuf *image_buffer;
  const TileColorspaceProcessor *processors;
  bke::pbvh::pixels::PackedPixelRow pixel_row;
  IndexRange range;
  Vector<float4> values;
  Vector<AcceptedPixel> accepted;
  int node_index;
};

void ImageColorEffect::apply_pass(const Depsgraph &depsgraph,
                                  Object &ob,
                                  const Brush &brush,
                                  CurvePatchSession &patch,
                                  const CurvePatchItem &item)
{
  /* Guarded the same way `restore()`, `begin_restamp()` and `end_restamp()` are: nothing about the
   * effect interface promises a live session and stroke cache on every call. */
  SculptSession *ss_ptr = ob.runtime->sculpt_session;
  if (ss_ptr == nullptr || ss_ptr->cache == nullptr) {
    return;
  }
  SculptSession &ss = *ss_ptr;
  StrokeCache &cache = *ss.cache;
  /* Snapshotted per PASS, not per re-stamp: `do_symmetrical_brush_actions()` rewrites every field
   * of it between the passes it drives. */
  const CurvePatchStrokeContext ctx = curve_patch_stroke_context_from_cache(cache);
  if (cache.image_paint_targets.is_empty()) {
    return;
  }
  if (!this->canvas_matches(ob)) {
    return;
  }
  Mesh &mesh = *id_cast<Mesh *>(ob.data);
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(ob);
  if (pbvh.type() != bke::pbvh::Type::Mesh || pbvh.pixels_ == nullptr) {
    return;
  }
  bke::pbvh::pixels::PixelData &pixel_data = bke::pbvh::pixels::data_get(pbvh);

  const Span<float3> positions = mesh.vert_positions();
  const Span<float3> normals = mesh.vert_normals();
  const MeshAttributeData attribute_data(mesh);
  const Span<float> mask = attribute_data.mask;

  /* Only a brush-mapped Normal source projects into the view; a session whose view context is
   * gone falls back to #StrokeCache::view_right, which #build_normal_write_basis handles. */
  const ARegion *view_region = cache.vc != nullptr ? cache.vc->region : nullptr;

  curve_patch_effect_ensure_falloff_curve(brush);

  /* PBR Paint gives every channel an optional SOURCE texture of its own -- that is what the
   * Base Color image in the channel panel is -- and an Alpha channel that masks the others.
   * The ordinary stroke resolves both through this sampler (`do_paint_pixels()` in
   * `mesh/sculpt_paint_image.cc`); without it a patch could only ever paint the flat slider
   * color, which is exactly what made an assigned Base Color texture look ignored.
   *
   * Built once per pass rather than per canvas: it is a property of the brush, and its
   * per-channel sources are all resolved up front. */
  std::optional<material::ChannelSourceSampler> channel_sources;
  bool alpha_masking_active = false;
  if (brush.material_paint != nullptr) {
    channel_sources.emplace(ss,
                            brush,
                            *brush.material_paint,
                            *paint_mode_settings_,
                            cache.paint->visible_material_channels);
    if (channel_sources->is_active()) {
      /* Area Plane mapping depends on the dab's own location and motion, so the matrices are
       * rebuilt per pass -- the patch's equivalent of "per dab". */
      channel_sources->update_area_local_mats(ob);
      /* Inverting erases toward the channel default, where masking the erase by the Alpha the
       * stroke is also painting would be circular. Same condition the stroke engine uses. */
      alpha_masking_active = !cache.toggle_settings.invert &&
                             BKE_paint_material_channel_masks_stroke(
                                 *brush.material_paint,
                                 *paint_mode_settings_,
                                 cache.paint->visible_material_channels);
    }
    else {
      channel_sources.reset();
    }
  }

#if CURVE_PATCH_PROFILING
  /* Counted here, past every guard above, so the pass total matches the timed work. */
  prof_pass_count_++;
  const double prof_cull_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch-image */
#endif
  const float max_radius = curve_patch_max_radius(item.geometry);
  /* Broad-phase cull tube for the per-chunk reject in PHASE 1 below: the same tube (same margin)
   * the node/grid culls use, so chunk rejection stays result-identical with them. */
  const float cull_tube_radius = curve_patch_cull_tube_radius(item.geometry, max_radius);
  IndexMaskMemory culled_memory;
  const IndexMask node_mask = curve_patch_effect_node_mask(
      depsgraph, ob, brush, item, ctx, pbvh, max_radius, culled_memory);
#if CURVE_PATCH_PROFILING
  prof_cull_ += BLI_time_now_seconds() - prof_cull_t0; /* DEBUG-cpatch-image */
#endif

  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  MutableSpan<bke::pbvh::pixels::PixelNode> pixel_nodes = pixel_data.nodes;

  /* Fetch buffers and push the REAL undo tiles for every node this pass will touch -- the two
   * calls `SCULPT_do_paint_brush_image()` makes before its own pixel loop, reused as-is.
   * `do_push_undo_tile()` is a no-op for a tile already captured by this undo step.
   *
   * The push is conditional because the INITIAL PREVIEW RESTAMP's passes all run before
   * `session_undo_begin()` does. That restamp is driven synchronously from
   * `curve_patch_begin_editing()` (`paint_curve_patch_session.cc:697`), and it reaches
   * `apply_pass()` once per symmetry pass, not once in total: `do_symmetrical_brush_actions()`
   * (`mesh/sculpt.cc:3844-3861`) invokes the action once per valid mirror pass and again for each
   * radial pass, so with symmetry enabled several of them precede the hook. Every one of them sees
   * the spawning stroke's own about-to-be-aborted transaction as the one in flight, and pushing
   * into it would only fill a step `BKE_undosys_step_push_init_abort()` is about to free. Only the
   * push is skipped -- the painting below still runs, so the preview appears as usual.
   *
   * Skipping it loses no undo data, including in the no-drag case (anchor, then commit with no
   * intervening edit). Every path that can commit re-stamps at final quality FIRST --
   * `curve_patch_edit_finish()` (`paint_curve_patch_edit.cc:1591`) and
   * `curve_patch_commit_on_session_end()` (`paint_curve_patch_session.cc:97`) -- and `restore()`
   * runs first within each re-stamp, putting the pristine content back. So by the time this loop
   * runs with a step open, the buffer holds the true pre-patch originals and they are what gets
   * captured. The cancel paths need no push at all: they only `restore()`.
   *
   * ONE EXCEPTION, where the preview's pixels really are lost: if the Paint Mode canvas is swapped
   * mid-session, `canvas_matches()` fails and BOTH `restore()` and `apply_pass()` early-return, so
   * the preview's pixels stay on the old canvas -- neither restorable nor undoable. That is the
   * same exposure the canvas-swap guards accept everywhere else in this effect; the alternative is
   * writing patch data into an image the user has since pointed the canvas away from. */

  /* Everything above is canvas-independent -- the cull and the node mask come from the patch
   * geometry alone -- so it is computed once and the pixel work below repeats per canvas. One
   * iteration for Mode=`Image`, one per enabled Principled channel for Mode=`Material`. */
  const StringRef uv_map_name =
      BKE_paint_canvas_uvmap_name_get(paint_mode_settings_, &ob).value_or("");
  for (paint::image::ImagePaintTarget &target : cache.image_paint_targets) {
  ImageData &image_data = *target.data;
  if (image_data.image == nullptr) {
    continue;
  }
  if (!curve_patch_layout_matches(pbvh, image_data, uv_map_name)) {
    continue;
  }
  /* Whether the RIBBON's own texture may supply this canvas's paint color. Normal packs a
   * direction and a scalar channel carries a single value, so neither can take an RGB from it.
   * The channel's own SOURCE is a separate mechanism and does reach Normal -- see
   * `target_paints_normal_source`. Resolved per canvas because it is a property of the channel. */
  const bool target_paints_color = !target.is_normal_channel &&
                                   (target.is_color_channel || !target.is_material_channel);
  const eMaterialPaintChannel target_channel = target.channel;
  const bool target_has_source = target.is_material_channel && channel_sources.has_value() &&
                                 channel_sources->has_usable_source(target_channel);
  /* A Normal source is neither a color nor a scalar: sampling it as either would write a flat
   * grey over the directions the map encodes. It takes its own path, which needs the destination
   * tangent basis per UV primitive -- the same `tri_tangent` / `bitangent_sign` pair
   * `do_paint_pixels()` builds (`mesh/sculpt_paint_image.cc`). */
  const bool target_paints_normal_source = target.is_normal_channel && target_has_source;
  const bool target_masked_by_alpha = target.is_material_channel &&
                                      material::channel_uses_alpha_mask(alpha_masking_active,
                                                                        target_channel);
  /* Decode a Base Color source once per CHUNK rather than once per pixel.
   *
   * The per-pixel decode goes through `IMB_colormanagement_colorspace_to_scene_linear_v3`, which
   * re-acquires the colorspace's CPU processor on every call -- and that acquisition takes a mutex
   * shared by every colorspace (`cpu_processor_cache.hh`), so with one worker per core the painted
   * pixels spend most of their time queueing for a lock that then just hands back an
   * already-built pointer. It measured at ~9 of the ~13 seconds of CPU this phase was spending.
   * The buffer entry point takes that lock once for a whole span instead, which is why
   * `do_paint_pixels()` and the 2D paint path already sample raw and call #decode_linear_batch
   * afterwards; this is the same move for the Curve Patch path, and it is why the sampling below
   * asks for `decode_linear = !batch_decode_color`. */
  const bool batch_decode_color = target_has_source && target_paints_color &&
                                  !target_paints_normal_source &&
                                  channel_sources->needs_linear_conversion(target_channel);

  /* The channel's flat color, used where no source supplies one. Read from the LIVE brush, not
   * from the target's cached override -- see #curve_patch_channel_flat_color. */
  const float3 target_flat_color = curve_patch_channel_flat_color(
      target, brush, *cache.paint, *paint_mode_settings_, cache.toggle_settings.invert);
#if CURVE_PATCH_PROFILING
  /* Taken per canvas, not once before the loop: the two waves below run once per target, so a
   * timestamp hoisted out of the loop would charge every later canvas with the PHASE 1/2 time of
   * the canvases before it. */
  const double prof_fetch_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch-image */
#endif
  node_mask.foreach_index([&](const int i) {
    paint::image::fetch_image_buffers(image_data, nodes[i], pixel_nodes[i]);
  });
#if CURVE_PATCH_PROFILING
  const double prof_undo_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch-image */
  prof_fetch_ += prof_undo_t0 - prof_fetch_t0;        /* DEBUG-cpatch-image */
#endif
  if (undo_step_open_ && !node_mask.is_empty()) {
    /* Re-validate before every push: the step opened at session start may have been taken by a
     * foreign undo push since the last restamp. See #ImageColorEffect::ensure_undo_step_live. */
    this->ensure_undo_step_live();
    /* Only nodes this effect has not already captured for THIS canvas: re-pushing a captured tile
     * changes nothing but still serializes on the undo system's global lock. See
     * #undo_pushed_nodes_. The mask is built before the wave so the `Set` is only read here and
     * only written below, never concurrently with either. */
    const Image *undo_image = image_data.image;
    IndexMaskMemory pending_memory;
    const IndexMask pending_mask = IndexMask::from_predicate(
        node_mask, pending_memory, [&](const int i) {
          return !undo_pushed_nodes_.contains({undo_image, i});
        });
    pending_mask.foreach_index(
        [&](const int i) {
          paint::image::do_push_undo_tile(image_data, nodes[i], pixel_nodes[i]);
        },
        exec_mode::grain_size(1));
    pending_mask.foreach_index([&](const int i) { undo_pushed_nodes_.add({undo_image, i}); });
    /* Recorded rather than inferred later: the destructor cannot ask the undo system how many
     * tiles a step holds, and this is the only call that can put one there. Deliberately
     * conservative -- a node whose `PixelNode::undo_regions` are empty for these tiles contributes
     * nothing
     * (`mesh/sculpt_paint_image.cc:509-530`), so this can read true for a step that stayed empty.
     * That errs toward committing a harmless empty step; the opposite error would abort a step
     * holding real "before" data. */
    undo_tiles_pushed_ = true;
  }
#if CURVE_PATCH_PROFILING
  prof_undo_push_ += BLI_time_now_seconds() - prof_undo_t0; /* DEBUG-cpatch-image */
#endif

  /* NOTE: This buffers a `float4` copy of every ACCEPTED chunk across the whole node mask before
   * PHASE 2 begins, so peak extra memory scales with the painted area of this pass rather than
   * with a fixed per-element snapshot the way `ColorEffect`'s `Vector<ColorWrite>` does. It is
   * bounded (only chunks with at least one accepted pixel are kept) and freed at the end of the
   * pass, but it is a different memory-scaling class -- recorded so the difference is not mistaken
   * for an oversight. */
  struct LocalData {
    Vector<ChunkWrite> chunks;
#if CURVE_PATCH_PROFILING
    /* DEBUG-cpatch-image: per-thread CPU-time split of PHASE 1. `interp` is the barycentric
     * position/normal/mask interpolation, `sample` the `sampler.sample()` loop it feeds -- split
     * because `interp` is the one per-candidate cost no ribbon optimization touches, which makes
     * it the control that says whether two runs are comparable at all. `read` is
     * `read_image_pixels()`, run only for accepted chunks. `candidates` is
     * how many pixels were sampled at all -- compared against the reported `pixels` (accepted) it
     * shows how much of PHASE 1 is spent on pixels the patch then rejects. Summed across threads
     * after the parallel region, so the sums exceed the wall-clock `phase1`; their RATIO is the
     * signal. */
    double interp_time = 0.0;
    double sample_time = 0.0;
    double read_time = 0.0;
    int64_t candidates = 0;
    /* Pixels in chunks the broad-phase cull skipped entirely (never sampled). `candidates +
     * culled` equals the pre-cull candidate total. */
    int64_t culled = 0;
    /* sample() funnel, accumulated from the chunk's sampler after the sample loop. */
    int64_t reached_lut = 0;
    int64_t reached_relief = 0;
    int64_t tex_evals = 0;
    /* Accepted pixels that reached the channel-source sampling. That work runs per ACCEPTED pixel
     * inside the same timed span as the per-CANDIDATE patch sampling, so `patch` only means the
     * same thing across two runs at the same `shaded`/`cand` ratio. */
    int64_t shaded = 0;
    CurvePatchSampler::BranchFunnel branch_funnel;
#endif
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;

  /* Resolved before the parallel region below, not inside it: the samplers are built per chunk,
   * and letting several worker threads race to lazily create the session's pool would leak all but
   * one of them. */
  ImagePool &tex_pool = ss.tex_pool_ensure();

#if CURVE_PATCH_PROFILING
  const double prof_phase1_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch-image */
#endif
  /* PHASE 1 (parallel): derive each candidate pixel's 3D position and normal, sample the patch
   * there, and -- only for chunks that accepted at least one pixel -- read the chunk's current
   * contents into thread-local storage.
   *
   * No snapshot, accumulator or tag state is written here; those belong to PHASE 2 alone. The one
   * shared thing this phase DOES mutate is the image buffer itself, and only on the float path:
   * `read_image_pixels()`'s float overload converts its slice to scene-linear IN PLACE
   * (`mesh/sculpt_paint_image.cc:205-211`), so reading a chunk rewrites that region of the live
   * `ImBuf`. That is acceptable because each thread only ever reads chunks it owns and PHASE 2
   * writes every read chunk back whole, restoring consistency for accepted and unaccepted pixels
   * alike -- it is also why a chunk that accepted nothing must bail out BEFORE the read. */
  node_mask.foreach_index(
      [&](const int i) {
        bke::pbvh::pixels::PixelNode &pixel_node = pixel_nodes[i];
        for (bke::pbvh::pixels::UDIMTilePixels &tile_data : pixel_node.tiles) {
          ImBuf *image_buffer = image_data.buffers.lookup_default(tile_data.tile_number, nullptr);
          if (image_buffer == nullptr) {
            continue;
          }
          const TileColorspaceProcessor *processors = image_data.processors.lookup_ptr(
              tile_data.tile_number);
          if (processors == nullptr) {
            continue;
          }

          const int64_t buffer_size = int64_t(image_buffer->x) * image_buffer->y;
          MutableSpan<float4> float_buffer;
          Span<uchar4> byte_buffer;
          if (image_buffer->float_data()) {
            float_buffer = MutableSpan(
                reinterpret_cast<float4 *>(image_buffer->float_data_for_write()), buffer_size);
          }
          else {
            byte_buffer = Span(reinterpret_cast<const uchar4 *>(image_buffer->byte_data()),
                               buffer_size);
          }

          /* One 512-pixel chunk of one pixel row: broad-phase reject, then sample + read into
           * thread-local storage. Extracted into a lambda so the loop below can parallelize ACROSS
           * rows without reindenting or altering this body; its `return`s still just skip the
           * chunk. A pixel row is usually shorter than a 512-pixel within-row split, so the
           * previous inner `parallel_for(row_size, 512)` ran as a single task and, with `nodes ==
           * 1`, PHASE 1 executed essentially serially. */
          auto process_chunk = [&](const bke::pbvh::pixels::PackedPixelRow &pixel_row,
                                   const IndexRange range,
                                   const int thread_id,
                                   LocalData &local) {
            /* Broad-phase reject: skip this whole chunk when its 3D bounding sphere lies entirely
             * outside the patch's falloff tube, before any per-pixel position/normal/mask work or
             * `sample()` call. Within one `PackedPixelRow` (one UV primitive) a pixel's 3D
             * position is affine in the pixel index, so the chunk's pixels lie on a straight
             * segment and its bbox is exactly the bbox of its two end pixels -- no need to
             * interpolate all of them. Same predicate (same `cull_tube_radius` margin, same
             * canonicalization) as `curve_patch_cull_nodes`/`curve_patch_cull_grids`, so this is
             * result-identical: any pixel `sample()` would accept sits within `max_radius` of the
             * polyline. */
            const int cull_tri_index =
                pixel_node.uv_primitives.tri_indices[pixel_row.uv_primitive_index];
            const float2 cull_delta_bary =
                pixel_node.uv_primitives.delta_barycentric_coords[pixel_row.uv_primitive_index];
            const float2 cull_bary_first = pixel_row.start_barycentric_coord +
                                           cull_delta_bary * float(range.start());
            const float2 cull_bary_last = pixel_row.start_barycentric_coord +
                                          cull_delta_bary *
                                              float(range.start() + range.size() - 1);
            const float3 cull_p_first = paint::image::calc_pixel_position(
                positions, pixel_data.vert_tris, cull_tri_index, cull_bary_first);
            const float3 cull_p_last = paint::image::calc_pixel_position(
                positions, pixel_data.vert_tris, cull_tri_index, cull_bary_last);
            const float3 cull_bbox_min = math::min(cull_p_first, cull_p_last);
            const float3 cull_bbox_max = math::max(cull_p_first, cull_p_last);
            const float3 cull_center = (cull_bbox_min + cull_bbox_max) * 0.5f;
            const float cull_chunk_radius = math::distance(cull_center, cull_bbox_max);
            const float3 cull_canonical = curve_patch_canonicalize(ctx, cull_center);
            const float cull_reach = cull_tube_radius + cull_chunk_radius;
            if (item.geometry.spline.distance_sq_to(cull_canonical) > cull_reach * cull_reach) {
#if CURVE_PATCH_PROFILING
              local.culled += range.size();
#endif
              return;
            }
#if CURVE_PATCH_PROFILING
            local.candidates += range.size();
            const double prof_sample_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch-image */
#endif

            /* Row-chunk-local position/normal arrays: the same interpolation
             * `calc_pixel_row_positions()` performs, called a second time with vertex normals in
             * place of vertex positions -- `calc_pixel_position()` is a generic barycentric
             * interpolator that does not know it is "position". */
            Array<float3> chunk_positions(range.size());
            paint::image::calc_pixel_row_positions(
                positions,
                pixel_data.vert_tris,
                pixel_node.uv_primitives.tri_indices,
                pixel_node.uv_primitives.delta_barycentric_coords,
                pixel_row,
                range,
                chunk_positions);
            Array<float3> chunk_normals(range.size());
            paint::image::calc_pixel_row_positions(
                normals,
                pixel_data.vert_tris,
                pixel_node.uv_primitives.tri_indices,
                pixel_node.uv_primitives.delta_barycentric_coords,
                pixel_row,
                range,
                chunk_normals);

            /* The sculpt mask has no `calc_pixel_row_positions()` counterpart (it is scalar), so
             * interpolate it with the same weight convention `calc_pixel_position()` uses. */
            Array<float> chunk_mask;
            if (!mask.is_empty()) {
              chunk_mask.reinitialize(range.size());
              const int3 &verts =
                  pixel_data.vert_tris[pixel_node.uv_primitives
                                           .tri_indices[pixel_row.uv_primitive_index]];
              const float m0 = mask[verts[0]];
              const float m1 = mask[verts[1]];
              const float m2 = mask[verts[2]];
              const float2 first_bary = pixel_row.start_barycentric_coord;
              const float2 delta_bary =
                  pixel_node.uv_primitives.delta_barycentric_coords[pixel_row.uv_primitive_index];
              for (const int li : IndexRange(range.size())) {
                const float2 bary = first_bary + delta_bary * float(range.start() + li);
                chunk_mask[li] = m0 * bary.x + m1 * bary.y + m2 * (1.0f - bary.x - bary.y);
              }
            }

            /* Cheap to construct per chunk: only references plus a handful of precomputed
             * floats. Unlike `ColorEffect`, which shares one sampler over the whole mesh's
             * pre-existing `vert_positions()` span, pixels have no pre-existing flat array --
             * the chunk-local arrays above ARE the source, so the sampler is chunk-local too.
             *
             * `indices_are_mesh_verts` is false precisely because of that: the index handed to
             * `sample()` is a slot in these chunk-local arrays, not a mesh vertex, so the
             * sampler must not resolve it against the per-mesh-vertex normal snapshot. */
            const CurvePatchSourceGeometry source{
                chunk_positions, chunk_normals, nullptr, /*indices_are_mesh_verts*/ false};
            const CurvePatchSampler sampler(
                item, patch.doc.texture, ctx, brush, source, chunk_mask, tex_pool);

            /* Destination tangent basis for a Normal source, plus the screen basis a texel with
             * no patch frame falls back to. Constant across a UV primitive, and a chunk never
             * spans more than one, so it is built once here -- the same per-row hoist
             * `do_paint_pixels()` makes for the identical reason. */
            float3 n_m(0.0f, 0.0f, 1.0f);
            float3 t_screen(1.0f, 0.0f, 0.0f);
            float3 b_screen(0.0f, 1.0f, 0.0f);
            float3 t_m(1.0f, 0.0f, 0.0f);
            float3 b_m(0.0f, 1.0f, 0.0f);
            if (target_paints_normal_source) {
              const int64_t tri_position_start = int64_t(pixel_row.uv_primitive_index) * 3;
              material::build_normal_write_basis(
                  pixel_node.uv_primitives.tangents[pixel_row.uv_primitive_index],
                  pixel_node.uv_primitives.bitangent_signs[pixel_row.uv_primitive_index],
                  pixel_node.uv_primitives.triangle_positions.as_span().slice(tri_position_start,
                                                                             3),
                  cache.view_right,
                  view_region,
                  cache.projection_mat,
                  t_screen,
                  b_screen,
                  n_m,
                  t_m,
                  b_m);
            }

            /* NARROW-PHASE reject, in spans of #cull_span_size pixels.
             *
             * The chunk-level test above rejects on the bounding sphere of all 512 pixels at once,
             * which on a 4K canvas covers so much of the surface that it almost never lies wholly
             * outside the tube -- in practice it passes nearly every chunk through to the ribbon
             * LUT, and the LUT then rejects the great majority one pixel at a time, which is the
             * expensive way to learn the same thing.
             *
             * Splitting the same predicate over shorter spans shrinks that sphere by the split
             * factor and rejects whole spans before any `sample()` call. It is result-identical
             * for the same reason the chunk test is: `chunk_positions` is affine in the pixel
             * index within one `PackedPixelRow`, so a span's pixels lie on a straight segment and
             * its bounding box is exactly the box of its two end pixels -- a span whose sphere
             * misses the tube contains no pixel `sample()` would have accepted.
             *
             * The 512-pixel chunking itself is untouched: `read_image_pixels()` requires it, and
             * PHASE 2 still writes back whole chunks. */
#if CURVE_PATCH_PROFILING
            /* Chunk granularity on purpose: one clock pair per 512 pixels is noise-free, while a
             * pair around each pixel's sampling would cost more than the sampling itself. */
            const double prof_patch_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch-image */
            local.interp_time += prof_patch_t0 - prof_sample_t0;
#endif
            constexpr int64_t cull_span_size = 32;
            Vector<AcceptedPixel> accepted;
            int64_t span_end = 0;
            bool span_live = false;
            for (const int li : IndexRange(range.size())) {
              if (li >= span_end) {
                const int64_t span_start = li;
                span_end = std::min<int64_t>(li + cull_span_size, range.size());
                const float3 span_p_first = chunk_positions[span_start];
                const float3 span_p_last = chunk_positions[span_end - 1];
                const float3 span_center = (math::min(span_p_first, span_p_last) +
                                            math::max(span_p_first, span_p_last)) *
                                           0.5f;
                const float span_radius = math::distance(
                    span_center, math::max(span_p_first, span_p_last));
                const float span_reach = cull_tube_radius + span_radius;
                span_live = item.geometry.spline.distance_sq_to(curve_patch_canonicalize(
                                ctx, span_center)) <= span_reach * span_reach;
#if CURVE_PATCH_PROFILING
                if (!span_live) {
                  /* Move the span from `cand` to `culled`: the chunk test above already counted
                   * every pixel of this chunk as a candidate. */
                  local.culled += span_end - span_start;
                  local.candidates -= span_end - span_start;
                }
#endif
              }
              if (!span_live) {
                continue;
              }
              const std::optional<CurvePatchSample> sample = sampler.sample(li, thread_id);
              if (!sample) {
                continue;
              }
              float3 source_color = target_flat_color;
              float alpha_factor = 1.0f;
#if CURVE_PATCH_PROFILING
              if (target_has_source || target_masked_by_alpha) {
                local.shaded++;
              }
#endif
              if (target_has_source || target_masked_by_alpha) {
                /* WHERE a channel source is sampled decides whether it turns with the curve.
                 *
                 * The patch reports its own frame (`u` across the ribbon, `v` along it) whenever
                 * it has one, and that is the frame the ribbon's own zone textures have always
                 * used. Sampling a channel source there is what makes Base Color and the Alpha
                 * map follow the curve instead of standing still in view/area space while the
                 * curve turns under them.
                 *
                 * Without a patch frame -- a sample that reached neither the ribbon nor the
                 * stamp branch -- fall back to the ordinary brush mapping, which is what the
                 * stroke engine uses and is still better than nothing. */
                const bool use_patch_frame = sample->patch_uv_valid;
                std::optional<material::TexelSampleContext> tex_ctx;
                if (!use_patch_frame) {
                  tex_ctx = material::sculpt_texel_sample_context(ss, chunk_positions[li]);
                }
                if (target_paints_normal_source) {
                  /* The decal frame a normal map is authored in. In the patch frame that is the
                   * ribbon's own axes flattened onto the surface, which is what makes the map
                   * TURN with the curve instead of staying pinned to the screen while the color
                   * texture under it rotates; without a patch frame it is the screen basis, the
                   * same one the stroke engine uses. */
                  float3 t_decal = t_screen;
                  float3 b_decal = b_screen;
                  if (use_patch_frame) {
                    const float3 flat_u = sample->patch_axis_u -
                                          n_m * math::dot(sample->patch_axis_u, n_m);
                    const float3 flat_v = sample->patch_axis_v -
                                          n_m * math::dot(sample->patch_axis_v, n_m);
                    const float len_u = math::length(flat_u);
                    const float len_v = math::length(flat_v);
                    /* A frame seen edge-on by the surface projects to nothing; the screen basis
                     * already loaded above then stands in, as it does for a frameless texel. */
                    if (len_u > 1e-6f && len_v > 1e-6f) {
                      t_decal = flat_u / len_u;
                      b_decal = flat_v / len_v;
                    }
                  }
                  source_color = use_patch_frame ?
                                     channel_sources->tangent_normal_packed_at_uv(target_channel,
                                                                                 sample->patch_uv,
                                                                                 thread_id,
                                                                                 t_decal,
                                                                                 b_decal,
                                                                                 n_m,
                                                                                 t_m,
                                                                                 b_m) :
                                     channel_sources->tangent_normal_packed(target_channel,
                                                                            *tex_ctx,
                                                                            thread_id,
                                                                            t_decal,
                                                                            b_decal,
                                                                            n_m,
                                                                            t_m,
                                                                            b_m);
                }
                else if (target_has_source) {
                  if (target_paints_color) {
                    source_color = use_patch_frame ?
                                       channel_sources->color_at_uv(target_channel,
                                                                    sample->patch_uv,
                                                                    thread_id,
                                                                    !batch_decode_color) :
                                       channel_sources->color(
                                           target_channel, *tex_ctx, thread_id,
                                           !batch_decode_color);
                  }
                  else {
                    source_color = float3(
                        use_patch_frame ? channel_sources->scalar_at_uv(
                                              target_channel, sample->patch_uv, thread_id) :
                                          channel_sources->scalar(
                                              target_channel, *tex_ctx, thread_id));
                  }
                }
                if (target_masked_by_alpha) {
                  const float alpha_sample =
                      use_patch_frame ? channel_sources->scalar_at_uv(
                                            PAINT_MATERIAL_CHANNEL_ALPHA,
                                            sample->patch_uv,
                                            thread_id) :
                                        channel_sources->scalar(
                                            PAINT_MATERIAL_CHANNEL_ALPHA, *tex_ctx, thread_id);
                  alpha_factor = math::clamp(alpha_sample, 0.0f, 1.0f);
                }
              }
              accepted.append({li,
                               sample->tex_color,
                               sample->tex_valid,
                               sample->value,
                               sample->weight,
                               source_color,
                               alpha_factor});
            }
            if (batch_decode_color && !accepted.is_empty()) {
              /* Gathered into a contiguous span because the decode entry point takes one. Every
               * accepted pixel of this chunk sampled the color source -- the branch that fills
               * `source_color` from it is selected by chunk-constant flags -- so the whole array
               * decodes, with no per-pixel bookkeeping of which entries are raw. */
              Vector<float3> raw_colors(accepted.size());
              for (const int ai : accepted.index_range()) {
                raw_colors[ai] = accepted[ai].source_color;
              }
              material::ChannelSourceSampler::decode_linear_batch(
                  raw_colors, channel_sources->colorspace(target_channel));
              for (const int ai : accepted.index_range()) {
                accepted[ai].source_color = raw_colors[ai];
              }
            }
#if CURVE_PATCH_PROFILING
            local.reached_lut += sampler.dbg_reached_lut();
            local.reached_relief += sampler.dbg_reached_relief();
            local.tex_evals += sampler.dbg_tex_evals();
            local.branch_funnel.add(sampler.dbg_branch_funnel());
#endif
            if (accepted.is_empty()) {
          /* Bail out BEFORE the read. The float overload of `read_image_pixels()` converts
           * the image buffer in place, so a chunk that is read must also be written back. */
#if CURVE_PATCH_PROFILING
              local.sample_time += BLI_time_now_seconds() -
                                   prof_patch_t0; /* DEBUG-cpatch-image */
#endif
              return;
            }
#if CURVE_PATCH_PROFILING
            local.sample_time += BLI_time_now_seconds() - prof_patch_t0; /* DEBUG-cpatch-image */
            const double prof_read_t0 = BLI_time_now_seconds();          /* DEBUG-cpatch-image */
#endif

            Vector<float4> byte_storage;
            MutableSpan<float4> scene_linear;
            if (!float_buffer.is_empty()) {
              scene_linear = paint::image::read_image_pixels(
                  float_buffer, *processors, pixel_row, range, image_buffer->x);
            }
            else {
              scene_linear = paint::image::read_image_pixels(
                  byte_buffer, *processors, pixel_row, range, image_buffer->x, byte_storage);
            }
#if CURVE_PATCH_PROFILING
            local.read_time += BLI_time_now_seconds() - prof_read_t0; /* DEBUG-cpatch-image */
#endif

            ChunkWrite chunk;
            chunk.tile_data = &tile_data;
            chunk.tile_number = tile_data.tile_number;
            chunk.image_buffer = image_buffer;
            chunk.processors = processors;
            chunk.pixel_row = pixel_row;
            chunk.range = range;
            /* Copied, not aliased: on the float path `scene_linear` points straight into the
             * image buffer, which PHASE 2 overwrites. */
            chunk.values.extend(scene_linear);
            chunk.accepted = std::move(accepted);
            chunk.node_index = i;
            local.chunks.append(std::move(chunk));
          };

          /* Parallelize across ROWS, not within one row. Rows map to disjoint pixels, so this is
           * race-free for the same reason the reference `do_paint_pixels()`
           * (`mesh/sculpt_paint_image.cc`) parallelizes over its rows with the same in-place read.
           * The 512-pixel chunking `read_image_pixels()` requires is kept, iterated serially
           * within each row (a row spans at most the image width). `all_tls.local()` and the
           * tex-pool `thread_id` are fetched once per row-range task -- one thread runs the whole
           * task. */
          threading::parallel_for(
              tile_data.pixel_rows.index_range(), 8, [&](const IndexRange rows) {
                const int thread_id = BLI_task_parallel_thread_id(nullptr);
                LocalData &local = all_tls.local();
                for (const int row_i : rows) {
                  const bke::pbvh::pixels::PackedPixelRow &pixel_row = tile_data.pixel_rows[row_i];
                  const int row_size = pixel_row.num_pixels;
                  for (int64_t chunk_start = 0; chunk_start < row_size; chunk_start += 512) {
                    const IndexRange range(
                        chunk_start, std::min<int64_t>(512, int64_t(row_size) - chunk_start));
                    process_chunk(pixel_row, range, thread_id, local);
                  }
                }
              });
        }
      },
      exec_mode::grain_size(1));
#if CURVE_PATCH_PROFILING
  prof_phase1_ += BLI_time_now_seconds() - prof_phase1_t0; /* DEBUG-cpatch-image */
  /* Sum the per-thread PHASE 1 split now that the parallel region has joined. */
  for (const LocalData &local : all_tls) {
    prof_p1_interp_ += local.interp_time;
    prof_p1_sample_ += local.sample_time;
    prof_p1_read_ += local.read_time;
    prof_candidates_ += local.candidates;
    prof_culled_ += local.culled;
    prof_reached_lut_ += local.reached_lut;
    prof_reached_relief_ += local.reached_relief;
    prof_tex_evals_ += local.tex_evals;
    prof_shaded_ += local.shaded;
    prof_branch_funnel_.add(local.branch_funnel);
  }
  const double prof_phase2_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch-image */
#endif

  /* PHASE 2 (serial): the sole writer of the snapshot, of its cross-pass accumulator and of
   * the image buffers. Follows `ColorEffect::apply_pass()`'s PHASE 2 step for step -- lazy
   * per-slot snapshot capture, mix FROM the snapshot rather than from the live value, cross-pass
   * blend, clamp, texture-alpha attenuation.
   *
   * Two things differ, both forced by the target rather than chosen. The snapshot is keyed by
   * `tile_size` x `tile_size` tile plus an in-tile offset instead of by a domain element index,
   * because pixels have no bounded element domain to key on. And the write is per CHUNK, not per
   * element: the mixed value is stored back into `chunk.values` and the whole chunk is handed to
   * `write_image_pixels()` once, which is what undoes PHASE 1's in-place scene-linear conversion
   * for the pixels this pass did not accept. */
  /* Not named `paint`: that would shadow the `paint` namespace this function also calls into
   * (`paint::image::write_image_pixels()` below). */
  const Paint &paint_settings = *cache.paint;
  /* A material channel carries its own color -- a channel RGB, or grayscale `(v, v, v)` for a
   * scalar channel -- resolved by #init_image_paint_targets. Only the plain image canvas paints
   * with the brush color. Mirrors #BrushPainter.use_material_channel_color on the 2D path. */
  /* Same live resolve as `target_flat_color` above -- a cached override would make a slider
   * edit re-stamp with the old value. */
  const float3 brush_color = curve_patch_channel_flat_color(
      target, brush, paint_settings, *paint_mode_settings_, cache.toggle_settings.invert);

  /* A material channel decides how its value composites -- Normal must go through
   * #IMB_BLEND_NORMAL_MIX rather than a plain lerp of the encoded RGB, and a scalar channel may
   * be configured for Add, Multiply and so on. Resolved exactly as `do_paint_pixels()` resolves
   * it (`mesh/sculpt_paint_image.cc`) so the two engines cannot drift on what a channel means.
   *
   * Deliberately NOT `brush.blend` for the plain image canvas: a Curve Patch stamps a shape
   * rather than accumulating dabs, and Mix is the behavior that contract has always had.
   * Only the per-channel modes are honored here. */
  const IMB_BlendMode blend_mode = (target.is_material_channel &&
                                    brush.material_paint != nullptr) ?
                                       IMB_BlendMode(BKE_paint_material_channel_blend_mode(
                                           *brush.material_paint,
                                           target.channel,
                                           cache.toggle_settings.invert)) :
                                       IMB_BLEND_MIX;

  /* The Strength slider, applied as a separate factor exactly as the ordinary paint pipeline does
   * -- `brush_strength()` leaves it out of `bstrength` for a Paint brush. See
   * #curve_patch_color_mix_factor. */
  const float strength = BKE_brush_alpha_get(&paint_settings, &brush);

  BitVector<> touched(pbvh.nodes_num(), false);
  Set<ImBuf *> dirty_buffers;

  for (LocalData &local : all_tls) {
    for (ChunkWrite &chunk : local.chunks) {
      touched[chunk.node_index].set();

      const int row_start_x = int(chunk.pixel_row.start_image_coordinate.x) +
                              int(chunk.range.start());
      const int abs_y = int(chunk.pixel_row.start_image_coordinate.y);

      /* A chunk lives inside ONE `PackedPixelRow`, so its `abs_y` -- and with it the snapshot
       * tile's row and the in-tile row base -- is constant for every pixel below. */
      const int tile_y = abs_y / tile_size;
      const int in_tile_row_base = (abs_y % tile_size) * tile_size;

      /* The snapshot lookup is per `tile_size`-wide tile, but the loop below runs per PIXEL, so
       * looking it up each time hashes the same key up to `tile_size` times in a row. `accepted`
       * is filled in ascending `pixel.local` order and `abs_y` is fixed, so `tile_x` only ever
       * steps forward -- caching the last one collapses the hash lookups to one per tile the
       * chunk actually spans (at most 512 / `tile_size` + 1 of them).
       *
       * Holding the reference across iterations is safe only because it is re-taken the moment
       * the key changes: #Map stores values inline, so an insertion can rehash and move them, and
       * the only insertion here is the `lookup_or_add_cb` guarded by that very check. The cache is
       * scoped to one chunk, so it cannot outlive the run of equal keys that justifies it. */
      int cached_tile_x = -1;
      TileSnapshot *cached_snapshot = nullptr;

      for (const AcceptedPixel &pixel : chunk.accepted) {
        const int abs_x = row_start_x + pixel.local;
        const int tile_x = abs_x / tile_size;
        const int in_tile_offset = in_tile_row_base + (abs_x % tile_size);

        if (tile_x != cached_tile_x) {
          const TileKey key{image_data.image, chunk.tile_number, tile_x, tile_y};
          cached_snapshot = &orig_tiles_.lookup_or_add_cb(key, [&]() {
            TileSnapshot new_snapshot;
            new_snapshot.pixels.reinitialize(tile_size * tile_size);
            new_snapshot.captured.resize(tile_size * tile_size, false);
            new_snapshot.captured_rows.resize(tile_size, false);
            /* `accum` is left uninitialized on purpose: a slot is only ever read once its
             * `accum_claimed` bit is set, and that bit is only set together with a zero write. */
            new_snapshot.accum.reinitialize(tile_size * tile_size);
            new_snapshot.accum_claimed.resize(tile_size * tile_size, false);
            return new_snapshot;
          });
          cached_tile_x = tile_x;
          /* Bring this tile's accumulator into the current restamp. Done here rather than per
           * pixel because a run of pixels sharing `tile_x` shares the tile, and a tile revisited
           * by a later chunk or symmetry pass re-enters this branch and finds its stamp already
           * current. */
          if (cached_snapshot->accum_gen != restamp_generation_) {
            cached_snapshot->accum_gen = restamp_generation_;
            cached_snapshot->accum_claimed.fill(false);
          }
        }
        TileSnapshot &snapshot = *cached_snapshot;
        /* Capture first, then mix FROM the snapshot -- the same order and the same reason as
         * `ColorEffect::apply_pass()`, which looks `orig_colors_` up before falling back to a live
         * read. `chunk.values[pixel.local]` is only the buffer's CURRENT content, which an earlier
         * symmetry pass of this same restamp may already have painted; mixing from it would apply
         * the patch twice wherever two passes overlap (a mirror or radial seam), and the
         * cross-pass blend below cannot undo that because it assumes a constant base.
         * The snapshot slot is the pre-patch value by construction: on first touch it is exactly
         * what was just captured, and on every later pass it is still the original. */
        if (!snapshot.captured[in_tile_offset]) {
          snapshot.captured[in_tile_offset].set();
          /* Kept in step with `captured` at its single write site, so the summary cannot drift. */
          snapshot.captured_rows[in_tile_offset / tile_size].set();
          snapshot.pixels[in_tile_offset] = chunk.values[pixel.local];
        }
        const float4 orig = snapshot.pixels[in_tile_offset];

        /* Cross-pass blend, the arithmetic #curve_patch_blend_across_passes performs, but against
         * this tile's own dense accumulator instead of the shared `Map`.
         *
         * The map needed a key unique per painted pixel across the whole canvas AND across UDIM
         * tiles, packed into 32 bits; the snapshot tile plus the in-tile offset ARE that identity,
         * and the tile is already resolved here, so the pixel indexes its slot directly and the
         * per-pixel hash lookup disappears. A clear `accum_claimed` bit means no pass of THIS
         * restamp has claimed the slot yet, which is exactly the state a cleared map represented.
         *
         * TODO(I10): same Color/Image last-RGB + averaged-factor split as ColorEffect. Image is
         * not grouped with Relief for this policy; see #curve_patch_blend_across_passes. */
        float2 &accum = snapshot.accum[in_tile_offset];
        if (!snapshot.accum_claimed[in_tile_offset]) {
          snapshot.accum_claimed[in_tile_offset].set();
          accum = float2(0.0f, 0.0f);
          /* One per texel this restamp writes -- the count the shared map reported by its size. */
          painted_slots_++;
        }
        accum.x += pixel.weight;
        accum.y += pixel.weight * pixel.value;
        const float blended = accum.y / accum.x;
        /* The Alpha channel scales every other channel's write, exactly as it does for a dab. */
        const float factor = curve_patch_color_mix_factor(
                                 blended, pixel.tex_color, pixel.tex_valid, strength) *
                             pixel.alpha_factor;

        /* The destination alpha is carried through untouched in BOTH branches: the color a patch
         * paints is a `float3` (`BKE_brush_color_get()`, or a channel color), so writing
         * anything else would invent data neither the brush nor the channel specified. A
         * channel configured for an alpha-targeting mode therefore affects only its RGB here. */
        /* Precedence, matching the stroke engine: the CHANNEL's own source wins (that is the
         * image assigned to Base Color in the channel panel); failing that the ribbon's own
         * texture supplies the color; failing that the flat channel/brush color. A ribbon
         * texture still contributes its intensity and alpha through `factor` either way. */
        const float3 paint_rgb = target_has_source ?
                                     pixel.source_color :
                                     (target_paints_color ?
                                          curve_patch_paint_color(
                                              brush_color, pixel.tex_color, pixel.tex_valid) :
                                          brush_color);
        if (blend_mode == IMB_BLEND_NORMAL_MIX) {
          /* A packed tangent normal is an ENCODED direction, not a linear color, so it must NOT
           * be pre-multiplied by coverage the way every other mode's source is: scaling it toward
           * (0, 0, 0) unpacks to a direction tilted hard toward (-1, -1, -1), which showed up as
           * the whole ribbon bulging wherever the falloff was partial -- the Alpha mask cropped
           * the decal correctly and this rode along underneath it. Coverage belongs in `t` alone,
           * exactly as `prepare_sampled_paint_range()` builds it for the stroke engine.
           *
           * #BKE_pbr_normal_blend_mix is called directly rather than through
           * #composite_coverage, whose #IMB_blend_color_float entry for this mode hard-codes
           * `is_float = true`. The destination's STORAGE decides whether its normals are signed
           * (float map) or packed into 0..1 (byte map -- every 8-bit normal map), and getting it
           * wrong reinterprets every texel already in the map. `do_paint_pixels()` passes the
           * real flag; so does this. */
          const float t = std::clamp(factor, 0.0f, 1.0f);
          const float target_n[3] = {
              paint_rgb.x * 2.0f - 1.0f, paint_rgb.y * 2.0f - 1.0f, paint_rgb.z * 2.0f - 1.0f};
          const float3 orig_rgb(orig);
          float blended_normal[3];
          BKE_pbr_normal_blend_mix(orig_rgb,
                                   target_n,
                                   t,
                                   chunk.image_buffer->float_data() != nullptr,
                                   blended_normal);
          chunk.values[pixel.local] = float4(
              blended_normal[0], blended_normal[1], blended_normal[2], orig.w);
        }
        else if (blend_mode == IMB_BLEND_MIX) {
          /* `composite_coverage()` would compute the same RGB for Mix, but it is a lerp either
           * way; keep the direct form so the common canvas pays nothing for the general path. */
          chunk.values[pixel.local] = float4(math::interpolate(float3(orig), paint_rgb, factor),
                                             orig.w);
        }
        else {
          /* `orig` is the frozen pre-patch value and `factor` this restamp's total coverage of
           * the texel, which is exactly the (pre-stroke value, accumulated coverage) pair
           * #composite_coverage expects -- the patch snapshot plays the role the raster
           * engine's #MaterialStrokeAccum plays for a stroke. Pre-multiplied, as that contract
           * requires. */
          const float4 mix(paint_rgb * factor, factor);
          float4 composited = material::composite_coverage(orig, mix, blend_mode);
          composited.w = orig.w;
          chunk.values[pixel.local] = composited;
        }
      }

      /* Write the chunk back WHOLE. The unaccepted pixels still hold the originals read in PHASE
       * 1, so this both applies the patch and undoes the in-place scene-linear conversion the
       * float read performed. */
#if CURVE_PATCH_PROFILING
      const double prof_p2_write_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch-image */
#endif
      const int64_t buffer_size = int64_t(chunk.image_buffer->x) * chunk.image_buffer->y;
      if (chunk.image_buffer->float_data()) {
        const MutableSpan<float4> dst(
            reinterpret_cast<float4 *>(chunk.image_buffer->float_data_for_write()), buffer_size);
        paint::image::write_image_pixels(chunk.values,
                                         dst,
                                         *chunk.processors,
                                         chunk.pixel_row,
                                         chunk.range,
                                         chunk.image_buffer->x);
      }
      else {
        const MutableSpan<uchar4> dst(
            reinterpret_cast<uchar4 *>(chunk.image_buffer->byte_data_for_write()), buffer_size);
        paint::image::write_image_pixels(chunk.values,
                                         dst,
                                         *chunk.processors,
                                         chunk.pixel_row,
                                         chunk.range,
                                         chunk.image_buffer->x);
      }

#if CURVE_PATCH_PROFILING
      prof_p2_write_ += BLI_time_now_seconds() - prof_p2_write_t0; /* DEBUG-cpatch-image */
#endif

      /* Same row-granular dirty region `do_paint_pixels()` records. `end_restamp()`'s seam fix
       * reads these flags through `collect_dirty_tiles()`, and `mark_image_dirty()` clears them.
       */
      const int2 dirty_start(int(chunk.pixel_row.start_image_coordinate.x),
                             int(chunk.pixel_row.start_image_coordinate.y));
      const int2 dirty_end = dirty_start + int2(chunk.pixel_row.num_pixels + 1, 0);
      chunk.tile_data->mark_dirty(Bounds<int2>(dirty_start, dirty_end));
      pixel_nodes[chunk.node_index].flags.dirty = true;
      dirty_buffers.add(chunk.image_buffer);
    }
  }

  /* Hoisted out of the per-pixel loop: `BKE_image_mark_dirty()` only sets flags on the image, so
   * once per touched buffer is both sufficient and far cheaper. */
  for (ImBuf *image_buffer : dirty_buffers) {
    BKE_image_mark_dirty(image_data.image, image_buffer);
  }

  /* Non-empty exactly when a chunk was written back above, i.e. when this pass put patch pixels on
   * the canvas. The destructor reads this to tell a commit from a cancel; `restore()` clears it.
   */
  if (!dirty_buffers.is_empty()) {
    patch_pixels_on_canvas_ = true;
  }

  /* `touched` is built per-write rather than from the whole `node_mask` query, for the same reason
   * `ColorEffect` scopes its own tag to the nodes it actually wrote. */
  IndexMaskMemory tag_memory;

  curve_patch_record_touched_nodes(patch.apply, IndexMask::from_bits(touched, tag_memory));
#if CURVE_PATCH_PROFILING
  prof_phase2_ += BLI_time_now_seconds() - prof_phase2_t0; /* DEBUG-cpatch-image */
#endif
  }
}

void ImageColorEffect::end_restamp(Object &ob, CurvePatchSession &patch)
{
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->pixels_ == nullptr) {
    return;
  }
  SculptSession *ss = ob.runtime->sculpt_session;
  if (ss == nullptr || ss->cache == nullptr || ss->cache->image_paint_targets.is_empty()) {
    return;
  }
  bke::pbvh::pixels::PixelData &pixel_data = bke::pbvh::pixels::data_get(*pbvh);

  IndexMaskMemory memory;
  const IndexMask touched_mask = IndexMask::from_bits(patch.apply.last_restamp_nodes, memory);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  MutableSpan<bke::pbvh::pixels::PixelNode> pixel_nodes = pixel_data.nodes;

  /* The same two closing steps an ordinary image-paint dab runs after writing pixels, in the same
   * order: the seam fix reads the per-tile dirty flags that `mark_image_dirty()` then clears. */
#if CURVE_PATCH_PROFILING
  const double prof_seam_t0 = BLI_time_now_seconds(); /* DEBUG-cpatch-image */
#endif
  /* The dirty-tile scan is a property of the shared pixel nodes, so it is collected once and
   * replayed per canvas. */
  const Vector<bke::image::TileNumber> dirty_tiles = paint::image::collect_dirty_tiles(
      pixel_nodes, touched_mask);
  /* Exactly the canvases `apply_pass()` could have written -- the same predicate, so a channel
   * skipped there is not seam-fixed or marked dirty here either. */
  const StringRef uv_map_name =
      BKE_paint_canvas_uvmap_name_get(paint_mode_settings_, &ob).value_or("");
  for (paint::image::ImagePaintTarget &target : ss->cache->image_paint_targets) {
    ImageData &image_data = *target.data;
    if (image_data.image == nullptr ||
        !curve_patch_layout_matches(*pbvh, image_data, uv_map_name))
    {
      continue;
    }
    if (!dirty_tiles.is_empty()) {
      paint::image::fix_non_manifold_seam_bleeding(*pbvh, image_data.buffers, dirty_tiles);
    }
  }
  /* #mark_image_dirty CONSUMES the dirty state: it clears #PixelNode::flags.dirty and every
   * tile's dirty region. The canvases share these nodes, so a later canvas would find nothing
   * left to mark. Snapshot the node's state and restore it between canvases -- the same thing
   * `do_paint_pixels()` does for its own grouped destinations
   * (`mesh/sculpt_paint_image.cc`, MarkDirty). */
  touched_mask.foreach_index([&](const int i) {
    bke::pbvh::pixels::PixelNode &pixel_node = pixel_nodes[i];
    const bool saved_node_dirty = pixel_node.flags.dirty;
    Vector<std::pair<bool, rcti>> saved_tile_dirty;
    saved_tile_dirty.reserve(pixel_node.tiles.size());
    for (const bke::pbvh::pixels::UDIMTilePixels &tile : pixel_node.tiles) {
      saved_tile_dirty.append({bool(tile.flags.dirty), tile.dirty_region});
    }
    bool first = true;
    for (paint::image::ImagePaintTarget &target : ss->cache->image_paint_targets) {
      ImageData &image_data = *target.data;
      if (image_data.image == nullptr ||
          !curve_patch_layout_matches(*pbvh, image_data, uv_map_name))
      {
        continue;
      }
      if (!first) {
        pixel_node.flags.dirty = saved_node_dirty;
        for (const int tile_i : pixel_node.tiles.index_range()) {
          pixel_node.tiles[tile_i].flags.dirty = saved_tile_dirty[tile_i].first;
          pixel_node.tiles[tile_i].dirty_region = saved_tile_dirty[tile_i].second;
        }
      }
      first = false;
      bke::pbvh::pixels::mark_image_dirty(
          nodes[i], pixel_node, *image_data.image, image_data.buffers);
    }
  });

  /* Force pending partial updates onto the GPU texture NOW, while the event-thread GL context is
   * still available. `mark_image_dirty()` only records changeset tiles; the upload normally waits
   * for the next `BKE_image_get_gpu_*` during draw. Curve Patch restamps (restore + re-apply) can
   * leave Workbench/EEVEE holding a cached `gpu::Texture *` whose contents are still the previous
   * frame -- so the mesh canvas keeps showing a stale preview until some unrelated full refresh.
   * Calling get here applies the changeset in place to that same texture object.
   *
   * Also notify Image Editors the way legacy texture paint does on every dab (`NA_PAINTING`), so
   * an open canvas preview redraws without waiting for a full `NA_EDITED` area refresh. */
  for (paint::image::ImagePaintTarget &target : ss->cache->image_paint_targets) {
    ImageData &image_data = *target.data;
    if (image_data.image == nullptr ||
        !curve_patch_layout_matches(*pbvh, image_data, uv_map_name))
    {
      continue;
    }
    if (image_data.image_user != nullptr) {
      BKE_image_get_gpu_material_texture(image_data.image, image_data.image_user, true);
    }
    WM_main_add_notifier(NC_IMAGE | NA_PAINTING, image_data.image);
    /* Refreshing the texture object is not enough on its own: the shading consumers of these
     * pixels keep their cached GPU material state until a SHADING-relevant tag lands on the ID,
     * so the 3D Viewport goes on showing the previous frame -- until an unrelated full refresh
     * (moving the view) rebuilds it. A re-stamp driven by the live-brush timer has no such
     * refresh behind it, which is what made a panel edit look like it did nothing.
     *
     * The 2D path already learned this (`paint_image_2d.cc`, `redraw_single()`, #150957); the 3D
     * canvas needs it for the same reason, and the multi-channel case makes it worse: only the
     * image a viewport happens to display gets refreshed as a side effect of its own redraw, so a
     * channel nobody has open would stay stale indefinitely. Explicit flags, not `0`. */
    DEG_id_tag_update(&image_data.image->id, ID_RECALC_SHADING | ID_RECALC_PARAMETERS);
  }

  /* Once for the object, not per canvas. The images ARE wired into the Principled BSDF, but the
   * draw code does not connect an image tag to the object's GPU material, so the object needs its
   * own shading tag -- the same conclusion, and the same comment, as the 2D path's Material
   * canvas branch. `ID_RECALC_SHADING` does NOT rebuild the PBVH the way a geometry tag would,
   * which is what `flush_update_step()` is careful to avoid on its image branch. */
  if (!ss->cache->image_paint_targets.is_empty()) {
    DEG_id_tag_update(&ob.id, ID_RECALC_SHADING);
  }
#if CURVE_PATCH_PROFILING
  const double prof_seam = BLI_time_now_seconds() - prof_seam_t0;
  /* DEBUG-cpatch-image: one line per restamp, summing every symmetry pass x patch. `pixels` is the
   * total painted-pixel count this restamp (#painted_slots_, reset per restamp), `tiles` the
   * snapshot's live tile count. Times are wall-clock ms; the parallel phases (build/cull/phase1)
   * report elapsed span, not summed core time. Paired with the session-level `DEBUG-cpatch` line,
   * whose `apply` column contains all of these plus the `do_symmetrical_brush_actions()` overhead.
   */
  const double to_ms = 1000.0;
  /* `p1_sample`/`p1_read` are summed across threads (CPU-time), so they add up to more than the
   * wall-clock `phase1`; read their RATIO, not their sum. `cand` is pixels sampled, `pixels` is
   * pixels the patch accepted -- a large `cand`/`pixels` gap means PHASE 1 burns most of its time
   * sampling pixels it then rejects (a culling-tightness signal). */
  printf(
      "[DEBUG-cpatch-image] passes=%d | build_px=%.2f cull=%.2f fetch=%.2f undo=%.2f phase1=%.2f "
      "(interp=%.2f sample=%.2f read=%.2f cpu) phase2=%.2f (write=%.2f) seam=%.2f (ms) | "
      "culled=%lld "
      "cand=%lld lut=%lld "
      "relief=%lld texev=%lld pixels=%lld tiles=%lld\n",
      prof_pass_count_,
      prof_build_pixels_ * to_ms,
      prof_cull_ * to_ms,
      prof_fetch_ * to_ms,
      prof_undo_push_ * to_ms,
      prof_phase1_ * to_ms,
      prof_p1_interp_ * to_ms,
      prof_p1_sample_ * to_ms,
      prof_p1_read_ * to_ms,
      prof_phase2_ * to_ms,
      prof_p2_write_ * to_ms,
      prof_seam * to_ms,
      (long long)prof_culled_,
      (long long)prof_candidates_,
      (long long)prof_reached_lut_,
      (long long)prof_reached_relief_,
      (long long)prof_tex_evals_,
      (long long)painted_slots_,
      (long long)orig_tiles_.size());

  /* DEBUG-cpatch-funnel: where PHASE 1's work goes to die, all normalized per CANDIDATE pixel so
   * two restamps of two DIFFERENT curves stay comparable -- absolute counts are not, since the
   * user draws a new curve every run. `interp_ns` is the control: it must hold steady, or the two
   * runs are not comparable at all and `patch_ns` means nothing. `shaded` is the second half of
   * that caveat: the channel sampling folded into `patch_ns` runs per ACCEPTED pixel, so two runs
   * only compare at a matching `shaded` ratio.
   *
   * The `rej_*` columns split the branches the LUT returned by the test that discarded them, which
   * is what names the check worth making cheaper or hoisting ahead of the spline evaluations. */
  const double per_cand = prof_candidates_ > 0 ? 1.0 / double(prof_candidates_) : 0.0;
  const double to_ns = 1.0e9;
  const CurvePatchSampler::BranchFunnel &bf = prof_branch_funnel_;
  const double per_branch = bf.branch_calls > 0 ? 1.0 / double(bf.branch_calls) : 0.0;
  printf(
      "[DEBUG-cpatch-funnel] per-cand: interp_ns=%.1f patch_ns=%.1f shaded=%.3f || per-branch: "
      "calls=%lld "
      "radius=%.3f nd=%.3f falloff=%.3f endpt=%.3f srange=%.3f endfade=%.3f late=%.3f "
      "accepted=%.3f\n",
      prof_p1_interp_ * to_ns * per_cand,
      prof_p1_sample_ * to_ns * per_cand,
      double(prof_shaded_) * per_cand,
      (long long)bf.branch_calls,
      double(bf.rej_radius) * per_branch,
      double(bf.rej_normal_dist) * per_branch,
      double(bf.rej_falloff) * per_branch,
      double(bf.rej_endpoint) * per_branch,
      double(bf.rej_s_range) * per_branch,
      double(bf.rej_end_falloff) * per_branch,
      double(bf.rej_late) * per_branch,
      double(bf.branch_calls - bf.rej_radius - bf.rej_normal_dist - bf.rej_falloff -
             bf.rej_endpoint - bf.rej_s_range - bf.rej_end_falloff - bf.rej_late) *
          per_branch);
  fflush(stdout);
#endif
}

void ImageColorEffect::commit(const Scene & /*scene*/,
                              const Depsgraph & /*depsgraph*/,
                              Object & /*ob*/,
                              const CurvePatchSession & /*patch*/)
{
  /* Nothing to do. Every write already landed live in the `ImBuf` during `apply_pass()`, and the
   * real `ImageUndoStep` was opened by `session_undo_begin()` and had its per-tile "before" data
   * pushed lazily during the session via `do_push_undo_tile()`. The destructor closes the step --
   * see its own comment for why that, not `commit()`, is the right place.
   *
   * The redraw this used to issue moved to `curve_patch_finish_commit()`, which is where the
   * `bContext` `flush_update_done()` needs legitimately lives -- see the note there for why an
   * Image-typed flush is issued on top of the shared Position-typed one. */
}

}  // namespace

std::unique_ptr<CurvePatchEffect> curve_patch_effect_image_create(
    Object &ob, PaintModeSettings &paint_mode_settings)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    /* Texpaint pixel data is mesh-triangle-based; no counterpart for Multires/Dyntopo. */
    return nullptr;
  }
  SculptSession *ss = ob.runtime->sculpt_session;
  if (curve_patch_primary_image_data(ss) == nullptr) {
    /* Defensive: `SCULPT_use_image_paint_brush()` already gated this call in the factory, and
     * `stroke_cache_init()` populates the paint targets under the identical condition, so this
     * should never trigger in practice. */
    return nullptr;
  }
  return std::make_unique<ImageColorEffect>(
      paint_mode_settings, curve_patch_canvas_key(paint_mode_settings, ob, ss));
}

}  // namespace blender::ed::sculpt_paint
