/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Internal declarations shared by the split 3D shape backend translation units
 * (`sculpt_paint_shape_sample.cc`, `_attr.cc`, `_pixels.cc`, and the `sculpt_paint_shape.cc`
 * dispatcher). Not a public header: the public API stays in `sculpt_paint_shape.hh`.
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <memory>
#include <string>

#include "BLI_array.hh"
#include "BLI_bounds.hh"
#include "BLI_index_mask.hh"
#include "BLI_math_base.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_offset_indices.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_task.hh"
#include "BLI_vector.hh"
#include "BLI_virtual_array.hh"

#include "DNA_ID.h"
#include "DNA_mesh_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"
#include "DNA_windowmanager_types.h"

#include "BKE_attribute.h"
#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_paint_bvh_pixels.hh"
#include "BKE_paint_types.hh"
#include "BKE_report.hh"

#include "BLT_translation.hh"

#include "DEG_depsgraph.hh"

#include "ED_paint.hh"
#include "ED_screen.hh"
#include "ED_view3d.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "../paint_intern.hh"
#include "../shapes/paint_shape.hh"
#include "../shapes/paint_shape_render.hh"
#include "../shapes/paint_shape_space.hh"
#include "../shapes/paint_shape_target.hh"
#include "mesh_brush_common.hh"
#include "../selection/paint_face_selection_mask.hh"
#include "paint_image_shape_composite.hh"
#include "paint_material_blend.hh"
#include "paint_material_source.hh"
#include "sculpt_color.hh"
#include "sculpt_intern.hh"
#include "sculpt_paint_material.hh"
#include "sculpt_paint_shape.hh"
#include "sculpt_undo.hh"

namespace blender::ed::sculpt_paint::shape {

/* #ShapeViewBasis lives in `paint_shape_space.hh` (it is space math, not a 3D-backend type), and
 * is produced by #ShapeSpace::view_basis. */

/** The sampled shape coverage of one vertex plus its combined mask/occlusion factor (0 = not
 * painted). */
struct ShapeVertexSample {
  ShapeSample sample;
  float factor = 0.0f;
};

/** Everything the write loops need: the affected nodes, their per-vertex samples and the flat
 * vertex offsets of those samples. */
struct ShapeBakeData {
  Vector<int> candidates;
  Array<int> node_offset;
  Array<ShapeVertexSample> samples;
  int painted = 0;
};

/** Copy \a bounds's corner \a corner (0..7) into a vector. */
float3 bounds_corner(const Bounds<float3> &bounds, int corner);

/**
 * Gather the affected PBVH nodes and sample every candidate vertex once; thread-parallel over
 * nodes (PBVH nodes own disjoint vertices). Each vertex and its normal are mirrored into every
 * symmetry pass, back-face culled there, and the pass with the strongest coverage is kept.
 *
 * \return false (with a reason reported on \a op) when nothing can be painted.
 */
bool shape_bake_prepare(Object &ob,
                        const ShapeSpace &space,
                        Span<PaintShape> shapes,
                        const ShapeStyle &style,
                        const Depsgraph &depsgraph,
                        ShapeBakeData &r_bake,
                        ReportList *reports);

/* -------------------------------------------------------------------- */
/** \name 3D write targets behind #ShapeTargetBackend
 *
 * The 3D canvas backends are objects holding the write context; the dispatcher builds one by
 * #PaintCanvasSource and drives #begin + #commit. The Image canvas backend implements the live
 * preview (#preview / #cancel back up and restore the touched tiles); the attribute backends
 * still bake one-shot, so their #preview / #cancel remain unimplemented (TODO) -- the live
 * Vector session is only offered on the Image canvas until then.
 * \{ */

/** Everything the 3D backends need. `ARegion` / `rv3d` deliberately are not part of it: they are
 * consumed once to build #ShapeSpace. `bContext` is not stored either: the live session passes it
 * to #preview / #commit per call, so the backend never holds a context across events. */
struct Sculpt3DTargetContext {
  Object *ob = nullptr;
  Scene *scene = nullptr;
  ViewLayer *view_layer = nullptr;
  const ShapeSpace *space = nullptr;
  ToolSettings *toolsettings = nullptr;
  Paint *paint = nullptr;
  /** Decides the target channel set (Override Channels, per-part `use`); never the shared tool
   * settings block when a session owns its own copy. */
  const PaintShapeSettings *settings = nullptr;

  /**
   * The evaluated graph of #scene / #view_layer, looked up on every call: a live session outlives
   * many events and must not keep a graph pointer that a view layer removal or a window close can
   * free. The context's graph wins when there is one; a context-less refresh (RNA update,
   * listener) finds the scene's existing graph. Null when neither exists (the caller bails out).
   */
  const Depsgraph *depsgraph_get(const bContext *C) const;
};

class SculptMaterialPaintBackend : public ShapeTargetBackend {
 public:
  explicit SculptMaterialPaintBackend(Sculpt3DTargetContext ctx);
  ~SculptMaterialPaintBackend() override;
  bool begin(bContext &C, ReportList *reports) override;
  bool preview(bContext *C, ReportList *reports, Span<PaintShape> shapes, const ShapeStyle &style) override;
  bool commit(bContext *C,
              ReportList *reports,
              Span<PaintShape> shapes,
              const ShapeStyle &style,
              const char *undo_name) override;
  void cancel() override;
  void preview_force_update(bContext *C) override;
  bool supports_preview() const override
  {
    return true;
  }
  void space_replace(const ShapeSpace &space) override
  {
    ctx_.space = &space;
  }

 private:
  Sculpt3DTargetContext ctx_;
  /** Snapshot of the touched vertex values the live preview restores; defined in the `.cc`. */
  struct Data;
  std::unique_ptr<Data> data_;
  void tag_preview(bContext *C, bool force);
};

class SculptColorAttributeBackend : public ShapeTargetBackend {
 public:
  explicit SculptColorAttributeBackend(Sculpt3DTargetContext ctx);
  ~SculptColorAttributeBackend() override;
  bool begin(bContext &C, ReportList *reports) override;
  bool preview(bContext *C, ReportList *reports, Span<PaintShape> shapes, const ShapeStyle &style) override;
  bool commit(bContext *C,
              ReportList *reports,
              Span<PaintShape> shapes,
              const ShapeStyle &style,
              const char *undo_name) override;
  void cancel() override;
  void preview_force_update(bContext *C) override;
  bool supports_preview() const override
  {
    return true;
  }
  void space_replace(const ShapeSpace &space) override
  {
    ctx_.space = &space;
  }

 private:
  Sculpt3DTargetContext ctx_;
  struct Data;
  std::unique_ptr<Data> data_;
  void tag_preview(bContext *C, bool force);
};

class SculptImageBackend : public ShapeTargetBackend {
 public:
  explicit SculptImageBackend(Sculpt3DTargetContext ctx);
  ~SculptImageBackend() override;
  bool begin(bContext &C, ReportList *reports) override;
  bool preview(bContext *C, ReportList *reports, Span<PaintShape> shapes, const ShapeStyle &style) override;
  bool commit(bContext *C,
              ReportList *reports,
              Span<PaintShape> shapes,
              const ShapeStyle &style,
              const char *undo_name) override;
  void cancel() override;
  void preview_force_update(bContext *C) override;
  bool supports_preview() const override
  {
    return true;
  }
  void space_replace(const ShapeSpace &space) override
  {
    ctx_.space = &space;
  }
  void target_images(Vector<const Image *> &r_images) const override;
  /** False once a target Image resolved at #begin was freed (an undo / material edit); the session
   * must not write through it. */
  bool targets_alive() const override;

 private:
  Sculpt3DTargetContext ctx_;
  /**
   * Targets resolved once at #begin and reused by every preview / commit, plus the full-tile
   * backups #cancel restores. Defined in the `.cc` because it holds image types.
   */
  struct Data;
  std::unique_ptr<Data> data_;

  /** Restore every backup tile; free the copies when \a free_buffers (cancel / commit). */
  void restore_backups(bool free_buffers);
  /** Fully restore the tiles a live refresh restored only by region and could not re-derive the
   * seam pixels of. */
  void restore_pending_tiles();
  /** Emit the throttled image/object shading tags of the preview; \a force skips the throttle. */
  void tag_preview(bContext *C, bool force);
};

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
