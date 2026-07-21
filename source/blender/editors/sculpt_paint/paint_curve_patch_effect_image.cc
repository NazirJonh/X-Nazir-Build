/* SPDX-FileCopyrightText: 2026 Blender Authors
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
 * unaccepted pixels carrying their unchanged originals -- exactly the invariant `do_paint_pixels()`
 * maintains by bailing out before the read when a whole chunk is rejected.
 */

#include <algorithm>
#include <optional>
#include <string>

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
#include "BLI_vector.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf_types.hh"

#include "ED_paint.hh"
#include "ED_undo.hh"

#include "paint_curve_patch_effect_common.hh"
#include "paint_intern.hh"

#include "mesh/mesh_brush_common.hh"
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
 * `ED_image_paint_tile_push()`'s own tiles already do; a per-pixel `Map` has no bound comparable to
 * `ColorEffect::orig_colors_`'s `verts_num`/`corners_num`. */
struct TileSnapshot {
  Array<float4> pixels;
  BitVector<> captured;
  /** One bit per tile ROW, set whenever any slot in that row is captured. A pure summary of
   * `captured`, maintained beside it, so `restore()` can skip an untouched row in O(1) instead of
   * testing its `tile_size` slots -- a tile enters the map on its FIRST painted pixel, so most rows
   * of most tiles stay empty for the whole session.
   *
   * A summary rather than `bits::any_bit_set()` over a row slice of `captured`: only row 0's slice
   * satisfies #is_bounded_span (`BLI_bit_span.hh:224-243` rejects an offset >= `BitsPerInt`), so
   * every later row would fall into `any_set_expr()`'s bit-at-a-time fallback
   * (`BLI_bit_span_ops.hh:100-110`) and test the same `tile_size` bits the loop already tests. */
  BitVector<> captured_rows;
  /** Dense id assigned in creation order, used to build the `CurvePatchApplyState::pass_weight_accum`
   * key. See #ImageColorEffect::apply_pass for why a dense id rather than the tile's coordinates. */
  int slot_id = 0;
};

class ImageColorEffect : public CurvePatchEffect {
 public:
  ImageColorEffect(PaintModeSettings &paint_mode_settings, std::string canvas_key);
  ~ImageColorEffect() override;

  void session_undo_begin() override;
  int64_t element_num(Object &ob) const override;
  void restore(Object &ob, const CurvePatchSession &patch) override;
  void begin_restamp(const Depsgraph &depsgraph, Object &ob, CurvePatchSession &patch) override;
  void apply_pass(const Depsgraph &depsgraph,
                  Object &ob,
                  const Brush &brush,
                  CurvePatchSession &patch) override;
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

 private:
  /** True when the resolved canvas (Single Image or Texture Slots, including which `Image *` it
   * names) still matches what the session started on. Mirrors `ColorEffect::attribute_matches` --
   * the modal passes events through, so the Paint Mode canvas panel is reachable mid-session. */
  bool canvas_matches(Object &ob) const;

  /** Re-open this effect's `ImageUndoStep` if the one `session_undo_begin()` opened is no longer
   * the transaction in flight. Must be called before every `do_push_undo_tile()`. */
  void ensure_undo_step_live();

  /** Raw pointer, not owned: `scene->toolsettings->paint_mode`, stable for the scene's lifetime. */
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

  /** Whether patch pixels are currently ON the canvas: set when `apply_pass()` writes a chunk,
   * cleared when `restore()` completes a full write-back. It is the destructor's "is this a commit
   * or a cancel?" signal -- the effect is destroyed on both and is told neither. See the destructor
   * for the four teardown paths and what this reads as on each. */
  bool patch_pixels_on_canvas_ = false;
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
   * would pass this check and receive our tiles. That is deliberately preferred to the alternative,
   * but it is NOT harmless, and the honest comparison is between two bad outcomes: reopening on a
   * name mismatch would FREE a live foreign image-paint transaction outright, while sharing one
   * mixes both strokes' "before" data into a single step AND, when this session ends, our
   * destructor's `ED_image_undo_push_end()` commits the foreign stroke's step out from under it --
   * so that stroke's next tile push hits the same broken lookup this guard exists to prevent. The
   * name is no way out: `undo_system.cc:595-596` copies the pusher's name only into an EMPTY one,
   * so an adopted step keeps ours and the name is unreliable in both directions.
   *
   * The reopen path has its own destructive case, for the same unavoidable reason: when `step_init`
   * holds a live FOREIGN NON-image transaction, `ED_image_undo_push_begin()` frees it
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
}

ImageColorEffect::~ImageColorEffect()
{
  /* `CurvePatchSession` (which owns this effect through a `unique_ptr`) is destroyed on four paths,
   * only some of which call `commit()`:
   * - `curve_patch_edit_finish()` (`paint_curve_patch_edit.cc:1614`) -- the PRIMARY one, serving
   *   both the modal's commit branch (`:1602`) and its Esc-cancel branch (`:1582`, which never
   *   calls `commit()`);
   * - `curve_patch_commit_on_session_end()` (`paint_curve_patch_session.cc:115`), after `commit()`;
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
   * Still exactly one close after `ensure_undo_step_live()` may have re-opened the step mid-session.
   * That helper neither sets nor clears `undo_step_open_`, and it re-opens ONLY when our previous
   * step is already gone -- freed by a foreign `push_init`, or adopted and committed by a foreign
   * push (see its own comment). So at most one step of ours is ever in flight, this closes that
   * last-opened one, and the earlier ones are already committed steps on the stack.
   *
   * CLOSED TWO WAYS, because a step is only worth committing when it would actually undo something.
   * `image_undosys_step_encode()` (`space_image/image_undo.cc:791-882`) pairs each pushed tile's
   * "before" data with the buffer's CURRENT content, so a step whose pixels have since been put
   * back encodes `pre == post`: pushing it costs the user a slot on the undo stack, truncates the
   * redo branch (`undo_system.cc:606-611`) and silently swallows their next Ctrl+Z, while undoing
   * nothing. `BKE_undosys_step_push_init_abort()` discards such a step instead.
   *
   * The two flags below answer that question without the destructor being told which teardown it is
   * on -- and it is told nothing. "`commit()` ran" would not answer it either: a patch whose mesh
   * was invalidated skips `commit()` while leaving its pixels on the canvas. The four teardown paths
   * read as:
   * - modal commit (`paint_curve_patch_edit.cc:1602` -> `:1614`): the final re-stamp restored and
   *   then repainted, so pixels are on the canvas and the wave pushed their "before" data ->
   *   PUSHED, exactly as before this guard existed;
   * - session-end commit (`paint_curve_patch_session.cc:108` -> `:115`): identical sequence -> PUSHED;
   * - modal cancel (`paint_curve_patch_edit.cc:1582` -> `:1614`): `curve_patch_restore_only()` ran
   *   and put every captured pixel back, so the step would encode `pre == post` -> ABORTED;
   * - session-end discard (`paint_curve_patch_session.cc:156` -> `:169`): same restore -> ABORTED.
   * Neither invalidated-mesh variant reaches a completed write-back, so both keep the flag set and
   * PUSH -- which is right, because their pixels really are still on the canvas and must stay
   * undoable. `edit.cc:1599` skips the commit after `curve_patch_restore_and_restamp()` has already
   * bailed out on its own element-count guard (`paint_curve_patch_session.cc:206-215`) BEFORE
   * restoring anything, and `cache.cc:105`'s `curve_patch_restore_only()` reaches `restore()` only
   * for it to return on the identical guard.
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
   * One knowingly accepted loss wherever the abort branch is taken: `fix_non_manifold_seam_bleeding()`
   * writes pixels outside the patch that `restore()` does not capture (see the NOTE in `restore()`),
   * so the discarded step would have been able to undo that residue and now cannot. This is not
   * confined to cancellation -- a COMMIT whose final restamp accepted no pixels (the patch was
   * dragged clear of the mesh) also lands here, with earlier restamps' residue still on the canvas.
   * A dead step that eats the user's next Ctrl+Z is the worse of the two. */
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

bool ImageColorEffect::canvas_matches(Object &ob) const
{
  return BKE_paint_canvas_key_get(paint_mode_settings_, &ob) == canvas_key_;
}

/** Mirrors `ColorEffect::element_num()`'s Corner-domain case: a `PixelNode`'s pixel rows are built
 * from mesh triangles (`PixelData::vert_tris`), invalidated by exactly the topology edits
 * (Triangulate, Poke, ...) that already invalidate `corners_num`. This is a STABLE count compared
 * against `CurvePatchApplyState::element_num` every restamp -- it must NOT be the snapshot's size, which
 * grows across the session. */
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
 * `values` IN PLACE, which is why every caller hands it a scratch copy rather than the snapshot. */
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
  /* No `element_num` check of its own -- `curve_patch_restore_only()` performs it for every caller.
   * The canvas check below is NOT redundant with it: a swapped canvas changes no mesh count.
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
  if (ss == nullptr || ss->cache == nullptr || !ss->cache->image_data) {
    return;
  }
  ImageData &image_data = *ss->cache->image_data;
  if (image_data.image == nullptr || image_data.image_user == nullptr) {
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
   * On the common non-scene-linear canvas (every 8-bit sRGB texture) `processors.is_noop` is false,
   * and a patch that touches N short runs in a row was otherwise issuing N separate OCIO calls of a
   * few pixels each -- on the main thread, every restamp, for every tile ever touched. The result is
   * unchanged: the transform is per-pixel, which the rest of this pipeline already relies on (both
   * `read_image_pixels()` and `write_image_pixels()` apply it over runs of whatever length their
   * caller happens to have, `mesh/sculpt_paint_image.cc:205-211` / `:247-250`), so batching moves
   * only where the call boundary falls. */
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
    if (key.image != image_data.image) {
      /* Cannot happen while `canvas_matches()` holds, but a stale entry must never be written
       * through a buffer belonging to a different image. */
      restored_all = false;
      continue;
    }

    /* Re-acquire rather than reading `ImageData::buffers`: that map is populated per-restamp inside
     * `apply_pass()`, and `restore()` runs FIRST in every restamp. Acquisition is reference
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
   * Both ways of leaving a pixel painted must keep the flag set, because the destructor reads it to
   * decide between committing the undo step and aborting it, and aborting one that still describes
   * live painted pixels destroys the user's ability to undo them:
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

void ImageColorEffect::begin_restamp(const Depsgraph &depsgraph,
                                     Object &ob,
                                     CurvePatchSession & /*patch*/)
{
  /* Curve Patch bypasses `do_brush_action()` entirely for its anchor dab, so the ordinary per-dab
   * call site that builds `PixelNode` pixel data (`sculpt_pbvh_update_pixels()`) never runs for a
   * Curve Patch stroke. `bke::pbvh::build_pixels()` is cheap to call every restamp once the tables
   * exist -- it only rebuilds nodes flagged `rebuild`, the same lazy behavior an ordinary stroke's
   * own per-dab call already relies on. */
  SculptSession *ss = ob.runtime->sculpt_session;
  if (ss == nullptr || ss->cache == nullptr || !ss->cache->image_data) {
    return;
  }
  ImageData &image_data = *ss->cache->image_data;
  if (image_data.image == nullptr || image_data.image_user == nullptr) {
    return;
  }
  bke::pbvh::build_pixels(depsgraph, ob, *image_data.image, *image_data.image_user);
}

/** One accepted pixel inside a chunk. `local` indexes `ChunkWrite::values`, i.e. it is relative to
 * the chunk's own `range`, not to the pixel row. */
struct AcceptedPixel {
  int local;
  float4 tex_color;
  float value;
  float weight;
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
                                  CurvePatchSession &patch)
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
  if (!cache.image_data) {
    return;
  }
  if (!this->canvas_matches(ob)) {
    return;
  }
  ImageData &image_data = *cache.image_data;
  if (image_data.image == nullptr) {
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

  curve_patch_effect_ensure_falloff_curve(brush);

  const float max_radius = curve_patch_max_radius(patch.geometry);
  IndexMaskMemory culled_memory;
  const IndexMask node_mask = curve_patch_effect_node_mask(
      depsgraph, ob, brush, patch, ctx, pbvh, max_radius, culled_memory);

  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  MutableSpan<bke::pbvh::pixels::PixelNode> pixel_nodes = pixel_data.nodes;

  /* Fetch buffers and push the REAL undo tiles for every node this pass will touch -- the two calls
   * `SCULPT_do_paint_brush_image()` makes before its own pixel loop, reused as-is.
   * `do_push_undo_tile()` is a no-op for a tile already captured by this undo step.
   *
   * The push is conditional because the INITIAL PREVIEW RESTAMP's passes all run before
   * `session_undo_begin()` does. That restamp is driven synchronously from
   * `curve_patch_begin_editing()` (`paint_curve_patch_session.cc:697`), and it reaches `apply_pass()`
   * once per symmetry pass, not once in total: `do_symmetrical_brush_actions()`
   * (`mesh/sculpt.cc:3844-3861`) invokes the action once per valid mirror pass and again for each
   * radial pass, so with symmetry enabled several of them precede the hook. Every one of them sees
   * the spawning stroke's own about-to-be-aborted transaction as the one in flight, and pushing
   * into it would only fill a step `BKE_undosys_step_push_init_abort()` is about to free. Only the
   * push is skipped -- the painting below still runs, so the preview appears as usual.
   *
   * Skipping it loses no undo data, including in the no-drag case (anchor, then commit with no
   * intervening edit). Every path that can commit re-stamps at final quality FIRST --
   * `curve_patch_edit_finish()` (`paint_curve_patch_edit.cc:1591`) and
   * `curve_patch_commit_on_session_end()` (`paint_curve_patch_session.cc:97`) -- and `restore()` runs
   * first within each re-stamp, putting the pristine content back. So by the time this loop runs
   * with a step open, the buffer holds the true pre-patch originals and they are what gets
   * captured. The cancel paths need no push at all: they only `restore()`.
   *
   * ONE EXCEPTION, where the preview's pixels really are lost: if the Paint Mode canvas is swapped
   * mid-session, `canvas_matches()` fails and BOTH `restore()` and `apply_pass()` early-return, so
   * the preview's pixels stay on the old canvas -- neither restorable nor undoable. That is the
   * same exposure the canvas-swap guards accept everywhere else in this effect; the alternative is
   * writing patch data into an image the user has since pointed the canvas away from. */
  node_mask.foreach_index(
      [&](const int i) { paint::image::fetch_image_buffers(image_data, nodes[i], pixel_nodes[i]); });
  if (undo_step_open_ && !node_mask.is_empty()) {
    /* Re-validate before every push: the step opened at session start may have been taken by a
     * foreign undo push since the last restamp. See #ImageColorEffect::ensure_undo_step_live. */
    this->ensure_undo_step_live();
    node_mask.foreach_index(
        [&](const int i) { paint::image::do_push_undo_tile(image_data, nodes[i], pixel_nodes[i]); },
        exec_mode::grain_size(1));
    /* Recorded rather than inferred later: the destructor cannot ask the undo system how many tiles
     * a step holds, and this is the only call that can put one there. Deliberately conservative --
     * a node whose `PixelNode::undo_regions` are empty for these tiles contributes nothing
     * (`mesh/sculpt_paint_image.cc:509-530`), so this can read true for a step that stayed empty.
     * That errs toward committing a harmless empty step; the opposite error would abort a step
     * holding real "before" data. */
    undo_tiles_pushed_ = true;
  }

  /* NOTE: This buffers a `float4` copy of every ACCEPTED chunk across the whole node mask before
   * PHASE 2 begins, so peak extra memory scales with the painted area of this pass rather than with
   * a fixed per-element snapshot the way `ColorEffect`'s `Vector<ColorWrite>` does. It is bounded
   * (only chunks with at least one accepted pixel are kept) and freed at the end of the pass, but it
   * is a different memory-scaling class -- recorded so the difference is not mistaken for an
   * oversight. */
  struct LocalData {
    Vector<ChunkWrite> chunks;
  };
  threading::EnumerableThreadSpecific<LocalData> all_tls;

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

          for (const bke::pbvh::pixels::PackedPixelRow &pixel_row : tile_data.pixel_rows) {
            const int row_size = pixel_row.num_pixels;
            threading::parallel_for(IndexRange(row_size), 512, [&](const IndexRange range) {
              const int thread_id = BLI_task_parallel_thread_id(nullptr);
              LocalData &local = all_tls.local();

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
                const int3 &verts = pixel_data.vert_tris[pixel_node.uv_primitives.tri_indices
                                                             [pixel_row.uv_primitive_index]];
                const float m0 = mask[verts[0]];
                const float m1 = mask[verts[1]];
                const float m2 = mask[verts[2]];
                const float2 first_bary = pixel_row.start_barycentric_coord;
                const float2 delta_bary =
                    pixel_node.uv_primitives
                        .delta_barycentric_coords[pixel_row.uv_primitive_index];
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
                  patch, ctx, brush, source, chunk_mask, ss.tex_pool);

              Vector<AcceptedPixel> accepted;
              for (const int li : IndexRange(range.size())) {
                const std::optional<CurvePatchSample> sample = sampler.sample(li, thread_id);
                if (!sample) {
                  continue;
                }
                accepted.append({li, sample->tex_color, sample->value, sample->weight});
              }
              if (accepted.is_empty()) {
                /* Bail out BEFORE the read. The float overload of `read_image_pixels()` converts
                 * the image buffer in place, so a chunk that is read must also be written back. */
                return;
              }

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
            });
          }
        }
      },
      exec_mode::grain_size(1));

  /* PHASE 2 (serial): the sole writer of the snapshot, of `patch.apply.pass_weight_accum` and of the
   * image buffers. Follows `ColorEffect::apply_pass()`'s PHASE 2 step for step -- lazy per-slot
   * snapshot capture, mix FROM the snapshot rather than from the live value, cross-pass blend,
   * clamp, texture-alpha attenuation.
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
  const float3 brush_color = BKE_brush_color_get(&paint_settings, &brush);
  /* The RGB the patch paints is ALWAYS the brush's primary color; a brush texture contributes only
   * its intensity (already folded into `CurvePatchSample::value` by the sampler) and its alpha,
   * which attenuates the mix below. `CurvePatchSample::tex_color`'s RGB is deliberately left unread
   * until the color path grows real RGBA-texture support. */
  const bool has_texture = brush.mtex.tex != nullptr;

  BitVector<> touched(pbvh.nodes_num(), false);
  Set<ImBuf *> dirty_buffers;

  for (LocalData &local : all_tls) {
    for (ChunkWrite &chunk : local.chunks) {
      touched[chunk.node_index].set();

      const int row_start_x = int(chunk.pixel_row.start_image_coordinate.x) +
                              int(chunk.range.start());
      const int abs_y = int(chunk.pixel_row.start_image_coordinate.y);

      for (const AcceptedPixel &pixel : chunk.accepted) {
        const int abs_x = row_start_x + pixel.local;
        const TileKey key{
            image_data.image, chunk.tile_number, abs_x / tile_size, abs_y / tile_size};
        const int in_tile_offset = (abs_y % tile_size) * tile_size + (abs_x % tile_size);

        const int next_slot_id = int(orig_tiles_.size());
        TileSnapshot &snapshot = orig_tiles_.lookup_or_add_cb(key, [&]() {
          TileSnapshot new_snapshot;
          new_snapshot.pixels.reinitialize(tile_size * tile_size);
          new_snapshot.captured.resize(tile_size * tile_size, false);
          new_snapshot.captured_rows.resize(tile_size, false);
          new_snapshot.slot_id = next_slot_id;
          return new_snapshot;
        });
        /* Capture first, then mix FROM the snapshot -- the same order and the same reason as
         * `ColorEffect::apply_pass()`, which looks `orig_colors_` up before falling back to a live
         * read. `chunk.values[pixel.local]` is only the buffer's CURRENT content, which an earlier
         * symmetry pass of this same restamp may already have painted; mixing from it would apply
         * the patch twice wherever two passes overlap (a mirror or radial seam), and the
         * `pass_weight_accum` blend below cannot undo that because it assumes a constant base.
         * The snapshot slot is the pre-patch value by construction: on first touch it is exactly
         * what was just captured, and on every later pass it is still the original. */
        if (!snapshot.captured[in_tile_offset]) {
          snapshot.captured[in_tile_offset].set();
          /* Kept in step with `captured` at its single write site, so the summary cannot drift. */
          snapshot.captured_rows[in_tile_offset / tile_size].set();
          snapshot.pixels[in_tile_offset] = chunk.values[pixel.local];
        }
        const float4 orig = snapshot.pixels[in_tile_offset];

        /* `pass_weight_accum` is `Map<int, float2>`, cleared once per restamp by the orchestrator
         * before the first symmetry pass. The key must be unique per painted pixel across the whole
         * canvas AND across UDIM tiles, and it has only 32 bits to do it in. Packing the tile's
         * image-space coordinates cannot work: `tile_x << 22` already overflows a signed `int` at
         * the maximum coordinate a `ushort2` pixel address permits, and coordinates alone would
         * collide between two UDIM tiles, which each start at (0, 0). A dense per-tile id assigned
         * in first-touch order sidesteps both: 20 bits of id (over a million snapshot tiles, far
         * more than the snapshot's memory could hold) plus the 12 bits `tile_size * tile_size`
         * needs exactly. */
        BLI_assert(snapshot.slot_id < (1 << 20));
        const int accum_key = int((uint32_t(snapshot.slot_id) << 12) | uint32_t(in_tile_offset));
        const float blended = curve_patch_blend_across_passes(
            patch.apply, accum_key, pixel.weight, pixel.value);
        const float factor = curve_patch_color_mix_factor(blended, pixel.tex_color, has_texture);
        /* The destination alpha is carried through untouched: `BKE_brush_color_get()` returns a
         * `float3`, so writing anything else would invent data the brush never specified. */
        chunk.values[pixel.local] = float4(math::interpolate(float3(orig), brush_color, factor),
                                           orig.w);
      }

      /* Write the chunk back WHOLE. The unaccepted pixels still hold the originals read in PHASE 1,
       * so this both applies the patch and undoes the in-place scene-linear conversion the float
       * read performed. */
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

      /* Same row-granular dirty region `do_paint_pixels()` records. `end_restamp()`'s seam fix
       * reads these flags through `collect_dirty_tiles()`, and `mark_image_dirty()` clears them. */
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
   * the canvas. The destructor reads this to tell a commit from a cancel; `restore()` clears it. */
  if (!dirty_buffers.is_empty()) {
    patch_pixels_on_canvas_ = true;
  }

  /* `touched` is built per-write rather than from the whole `node_mask` query, for the same reason
   * `ColorEffect` scopes its own tag to the nodes it actually wrote. */
  IndexMaskMemory tag_memory;
  curve_patch_record_touched_nodes(patch.apply, IndexMask::from_bits(touched, tag_memory));
}

void ImageColorEffect::end_restamp(Object &ob, CurvePatchSession &patch)
{
  bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->pixels_ == nullptr) {
    return;
  }
  SculptSession *ss = ob.runtime->sculpt_session;
  if (ss == nullptr || ss->cache == nullptr || !ss->cache->image_data) {
    return;
  }
  ImageData &image_data = *ss->cache->image_data;
  if (image_data.image == nullptr) {
    return;
  }
  bke::pbvh::pixels::PixelData &pixel_data = bke::pbvh::pixels::data_get(*pbvh);

  IndexMaskMemory memory;
  const IndexMask touched_mask = IndexMask::from_bits(patch.apply.last_restamp_nodes, memory);
  MutableSpan<bke::pbvh::MeshNode> nodes = pbvh->nodes<bke::pbvh::MeshNode>();
  MutableSpan<bke::pbvh::pixels::PixelNode> pixel_nodes = pixel_data.nodes;

  /* The same two closing steps an ordinary image-paint dab runs after writing pixels, in the same
   * order: the seam fix reads the per-tile dirty flags that `mark_image_dirty()` then clears. */
  paint::image::fix_non_manifold_seam_bleeding(ob, image_data, nodes, pixel_nodes, touched_mask);
  touched_mask.foreach_index([&](const int i) {
    bke::pbvh::pixels::mark_image_dirty(
        nodes[i], pixel_nodes[i], *image_data.image, image_data.buffers);
  });
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
    Object &ob,
    PaintModeSettings &paint_mode_settings)
{
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(ob);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    /* Texpaint pixel data is mesh-triangle-based; no counterpart for Multires/Dyntopo. */
    return nullptr;
  }
  SculptSession *ss = ob.runtime->sculpt_session;
  if (ss == nullptr || ss->cache == nullptr || !ss->cache->image_data) {
    /* Defensive: `SCULPT_use_image_paint_brush()` already gated this call in the factory, and
     * `stroke_cache_init()` populates `cache->image_data` under the identical condition, so this
     * should never trigger in practice. */
    return nullptr;
  }
  return std::make_unique<ImageColorEffect>(paint_mode_settings,
                                            BKE_paint_canvas_key_get(&paint_mode_settings, &ob));
}

}  // namespace blender::ed::sculpt_paint
