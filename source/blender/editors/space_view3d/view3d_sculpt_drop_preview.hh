/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spview3d
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "BLI_math_matrix_types.hh"
#include "BLI_vector.hh"

namespace blender {

struct bContext;
struct Depsgraph;
struct Main;
struct Collection;
struct Mesh;
struct Object;
struct TempLibraryContext;
struct V3DSnapCursorState;
struct wmDrag;
struct wmPaintCursor;
struct wmWindow;
struct wmWindowManager;

namespace gpu {
class Batch;
}

namespace ed::view3d::sculpt_drop_preview {

struct PreviewPlacement {
  float3 object_scale = float3(1.0f);
  float3 bottom_offset = float3(0.0f); /* object-local */
  bool use_bottom_offset = false;
};

struct PreviewItemCPU {
  Mesh *mesh_nomain = nullptr; /* owned until moved/freed */
  float4x4 local_to_drop = float4x4::identity();
};

struct PreviewJobResult {
  Vector<PreviewItemCPU> items;
  PreviewPlacement placement; /* single-object; ignored for collection */
  bool is_collection = false;
};

struct PreviewJobState {
  std::atomic<bool> accepting{true};
  std::atomic<bool> ready{false};
  std::atomic<bool> load_error{false};
  uint64_t generation = 0;

  std::string blend_filepath;
  short idcode = 0;
  std::string idname;
  bool is_collection = false;

  /* Captured on main thread before start. */
  Main *real_main = nullptr; /* non-owning; main-thread lifetime covering load per spike */
  uint winid = 0;
  /** Opaque WM job owner cookie (the originating #wmDrag); never dereferenced, only compared by
   * address. Distinguishes concurrent preview jobs started by different drags (e.g. one per
   * window) instead of sharing a single process-wide owner. */
  const void *job_owner = nullptr;

  /** Filled in initjob, freed in endjob. Worker reads during startjob only. */
  TempLibraryContext *temp_ctx = nullptr;

  PreviewJobResult result; /* single-owner; see spec */

  ~PreviewJobState();  /* frees temp_ctx + leftover result when last ref dies */
  void clear_result(); /* frees meshes in result.items */
};

struct PreviewItemGPU {
  Mesh *mesh_nomain = nullptr;
  float4x4 local_to_drop = float4x4::identity();
  gpu::Batch *batch_tris = nullptr;
  gpu::Batch *batch_edges = nullptr; /* optional */
};

struct SculptDropPreviewRuntime {
  PreviewPlacement placement;
  Vector<PreviewItemGPU> items;
  std::shared_ptr<PreviewJobState> active_job;
  uint64_t generation = 0;
  /** Window to re-find View3D region for redraw after async pull (§6.4). */
  uint redraw_winid = 0;
  bool is_collection = false;
  bool preview_ready = false;
  bool preview_failed = false;
};

/**
 * Per-drop interactive state for sculpt-mode asset drop boxes.
 * Stored in #wmDropBox::draw_data.
 */
struct SculptDropData {
  V3DSnapCursorState *snap_state; /* owned */
  float base_box_dims[3];         /* bbox half-extents captured at on_enter */

  /** Draws the mesh preview from within the region's own paint-cursor pass so it gets the
   * region's normal viewport/scissor clipping (unlike #wmDropBox::draw_in_view, which runs after
   * the window has already been composited and is not region-clipped). Owned; released on
   * #sculpt_drop_data_free. */
  wmPaintCursor *preview_cursor = nullptr;

  SculptDropPreviewRuntime preview;

  /* Scale (S key) */
  float scale_factor;
  bool is_scaling;
  int scale_start_mval_x;
  float scale_start_factor;

  /* Rotation around surface normal (R key) */
  float rotation_angle;
  bool is_rotating;
  int rotate_start_mval_x;
  float rotate_start_angle;

  /** Placement options, toggled during hover (J/F/L keys) and read by #copy right before exec.
   * Deciding these before the drop runs (rather than via the operator's redo panel afterward)
   * avoids re-running exec() with properties like `session_uid`/`collection_uid` that reference
   * an object/collection the first run may already have deleted or duplicated. */
  bool join_to_active = true;
  bool replace_face_sets = false;
  bool linked = true;

  /** Mask (M key): when on (default), the active mesh's pre-existing geometry is masked so only
   * the dropped-in geometry stays sculptable. When off, the active mesh's existing
   * `.sculpt_mask` is left untouched entirely. */
  bool apply_mask = true;
};

void preview_runtime_clear(SculptDropPreviewRuntime &rt);

bool preview_try_pull(SculptDropPreviewRuntime &rt);

void preview_build_batches(SculptDropPreviewRuntime &rt);

bool preview_build_local_object(SculptDropPreviewRuntime &rt, Depsgraph &depsgraph, Object &ob);

bool preview_build_local_collection(SculptDropPreviewRuntime &rt,
                                    Depsgraph &depsgraph,
                                    Collection &collection);

/**
 * Paint-cursor draw callback (registered per-drop via #WM_paint_cursor_activate with the owning
 * #SculptDropData as `customdata`). Runs inside the region's normal draw pass, so it is naturally
 * clipped to the region like the rest of the snap cursor overlay — see the note on
 * #SculptDropData::preview_cursor.
 */
void preview_draw_paint_cursor(bContext *C, const int2 &xy, const float2 &tilt, void *customdata);

void free_preview_job_customdata(void *p); /* delete shared_ptr* */

/** Starts WM job with WM_JOB_PROGRESS and name "Loading Sculpt Asset Preview...". */
void preview_job_start(wmWindowManager &wm,
                       wmWindow *win,
                       const std::shared_ptr<PreviewJobState> &state);
void preview_job_start(bContext &C, const std::shared_ptr<PreviewJobState> &state);

void preview_job_request_stop(wmWindowManager &wm, const void *owner);

void fill_placement_from_object(const Object &ob, PreviewPlacement &r_place);

/** Order: snap orient+loc → user rotation → rescale(effective_scale) → bottom offset. */
void build_single_object_matrix(const float rotation_angle,
                                const float scale_factor,
                                const PreviewPlacement &place,
                                const float plane_omat[3][3],
                                const float snap_loc[3],
                                float r_mat[4][4]);

/** Snap orient+loc → user rotation → uniform scale_factor only. No Object*, no bottom offset. */
void build_collection_snap_matrix(const float rotation_angle,
                                  const float scale_factor,
                                  const float plane_omat[3][3],
                                  const float snap_loc[3],
                                  float r_mat[4][4]);

}  // namespace ed::view3d::sculpt_drop_preview

}  // namespace blender
