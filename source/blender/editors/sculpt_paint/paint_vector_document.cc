/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Implementation of the runtime Vector document, its session undo and the runtime <-> persisted
 * Vector document converters; see #paint_vector_document.hh.
 */

#include "paint_vector_document.hh"

#include <algorithm>
#include <utility>

#include "MEM_guardedalloc.h"

#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_string.h"
#include "BLI_vector.hh"

#include "DNA_paint_vector_types.h"

#include "BKE_paint_vector.hh"

namespace blender::ed::sculpt_paint::shape {

/* -------------------------------------------------------------------- */
/** \name Document and session undo
 * \{ */

VectorItemStyle vector_item_style_make(const PaintShapeSettings &settings)
{
  PaintShapeSettings *copy = MEM_new<PaintShapeSettings>(__func__);
  BKE_paint_vector_style_copy(*copy, settings);
  return VectorItemStyle(copy, [](PaintShapeSettings *style) {
    BKE_paint_vector_style_free(*style);
    MEM_delete(style);
  });
}

/** Restore a step's items into the live document, keeping the live active index (clamped). */
static void restore_step_items(VectorDocument &doc, const VectorDocumentStep &step)
{
  doc.items = step.items;
  doc.active = std::clamp(doc.active, 0, std::max(int(doc.items.size()) - 1, 0));
}

void vector_document_set_items(VectorDocument &doc, Vector<VectorItem> items)
{
  doc.items = std::move(items);
  doc.active = 0;
  doc.undo_steps.clear();
  doc.undo_step_current = -1;
  doc.undo_anchor_trimmed = false;
  /* Entry 0 is the start state, so the invariant holds before any edit. */
  vector_document_undo_push(doc);
}

void vector_document_undo_push(VectorDocument &doc)
{
  /* Anything above the cursor is a redo branch the new edit invalidates. */
  doc.undo_steps.resize(doc.undo_step_current + 1);
  doc.undo_steps.append(VectorDocumentStep{doc.items});

  if (doc.undo_steps.size() > VECTOR_DOCUMENT_UNDO_STEPS_MAX) {
    doc.undo_steps.remove(0);
    /* Entry 0 is no longer the start state; "back at 0" must not be read as "nothing to undo". */
    doc.undo_anchor_trimmed = true;
  }
  doc.undo_step_current = int(doc.undo_steps.size()) - 1;
}

bool vector_document_undo_back(VectorDocument &doc)
{
  if (doc.undo_step_current <= 0) {
    /* Curve Patch semantics: once the oldest step was trimmed, index 0 is no longer the start
     * state, so "already at 0" must NOT be read as "nothing left to undo / cancel the patch". */
    return doc.undo_anchor_trimmed;
  }
  doc.undo_step_current--;
  restore_step_items(doc, doc.undo_steps[doc.undo_step_current]);
  return true;
}

bool vector_document_undo_forward(VectorDocument &doc)
{
  if (doc.undo_step_current + 1 >= int(doc.undo_steps.size())) {
    return false;
  }
  doc.undo_step_current++;
  restore_step_items(doc, doc.undo_steps[doc.undo_step_current]);
  return true;
}

void vector_document_undo_restore(VectorDocument &doc)
{
  if (doc.undo_step_current < 0 || doc.undo_step_current >= doc.undo_steps.size()) {
    return;
  }
  restore_step_items(doc, doc.undo_steps[doc.undo_step_current]);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Space
 * \{ */

static void space_to_dna(const ShapeSpaceDesc &src, PaintVectorSpace &dst)
{
  dst.type = int(src.type);
  dst.ref_tile = src.ref_tile;
  copy_v2_v2_int(dst.ref_tile_size, src.ref_tile_size);
  copy_v2_v2_int(dst.win_size, src.win_size);
  dst.clip_start = src.clip_start;
  dst.clip_end = src.clip_end;
  dst.is_persp = src.is_persp ? 1 : 0;
  copy_m4_m4(dst.persmat, src.persmat.ptr());
  copy_m4_m4(dst.viewinv, src.viewinv.ptr());
  copy_v3_v3(dst.anchor_co, src.anchor.co);
  dst.anchor_tri = src.anchor.tri;
  dst.anchor_has_surface_uv = src.anchor.has_surface_uv ? 1 : 0;
  copy_v3_v3(dst.anchor_normal, src.anchor.normal);
  copy_v3_v3(dst.anchor_tangent, src.anchor.tangent);
  copy_v2_v2(dst.anchor_surface_uv, src.anchor.surface_uv);
  dst.anchor_px_per_unit = src.anchor.px_per_unit;
  dst.anchor_max_depth = src.anchor.max_depth;
  BLI_strncpy(dst.surface_uv_map, src.anchor.surface_uv_map, sizeof(dst.surface_uv_map));
}

static void space_from_dna(const PaintVectorSpace &src, ShapeSpaceDesc &dst)
{
  dst.type = ShapeSpaceType(src.type);
  dst.ref_tile = src.ref_tile;
  copy_v2_v2_int(dst.ref_tile_size, src.ref_tile_size);
  copy_v2_v2_int(dst.win_size, src.win_size);
  dst.clip_start = src.clip_start;
  dst.clip_end = src.clip_end;
  dst.is_persp = src.is_persp != 0;
  copy_m4_m4(dst.persmat.ptr(), src.persmat);
  copy_m4_m4(dst.viewinv.ptr(), src.viewinv);
  copy_v3_v3(dst.anchor.co, src.anchor_co);
  dst.anchor.tri = src.anchor_tri;
  dst.anchor.has_surface_uv = src.anchor_has_surface_uv != 0;
  copy_v3_v3(dst.anchor.normal, src.anchor_normal);
  copy_v3_v3(dst.anchor.tangent, src.anchor_tangent);
  copy_v2_v2(dst.anchor.surface_uv, src.anchor_surface_uv);
  dst.anchor.px_per_unit = src.anchor_px_per_unit;
  dst.anchor.max_depth = src.anchor_max_depth;
  BLI_strncpy(dst.anchor.surface_uv_map, src.surface_uv_map, sizeof(dst.anchor.surface_uv_map));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Write (runtime -> DNA)
 * \{ */

void paint_vector_from_document(PaintVector &r_pv, const VectorDocument &doc)
{
  BKE_paint_vector_items_clear(r_pv);

  for (const VectorItem &item : doc.items) {
    PaintVectorItem &pvi = BKE_paint_vector_item_add(r_pv);
    const PaintShape &shape = item.shape;

    pvi.type = int(item.type);
    pvi.shape_type = int(shape.type);
    space_to_dna(item.space, pvi.space);

    copy_v2_v2(pvi.origin, shape.origin);
    if (shape.origin_is_custom) {
      pvi.flag |= PAINT_VECTOR_ITEM_ORIGIN_CUSTOM;
    }

    copy_v2_v2(pvi.center, shape.center);
    copy_v2_v2(pvi.half_size, shape.half_size);
    pvi.rotation = shape.rotation;
    copy_v4_v4(pvi.corner_radius, shape.corner_radius);
    pvi.polygon_sides = shape.polygon_sides;
    int total_points = 0;
    for (const ShapeSpline &spline : shape.splines) {
      total_points += spline.points.size();
    }
    MutableSpan<PaintVectorSpline> splines =
        BKE_paint_vector_item_splines_alloc(pvi, shape.splines.size());
    MutableSpan<PaintVectorPoint> points =
        BKE_paint_vector_item_points_alloc(pvi, total_points);

    int point_offset = 0;
    for (const int spline_i : shape.splines.index_range()) {
      const ShapeSpline &spline = shape.splines[spline_i];
      PaintVectorSpline &out_spline = splines[spline_i];
      out_spline.point_offset = point_offset;
      out_spline.point_num = spline.points.size();
      if (spline.cyclic) {
        out_spline.flag |= PAINT_VECTOR_SPLINE_CYCLIC;
      }
      if (spline.is_bezier) {
        out_spline.flag |= PAINT_VECTOR_SPLINE_BEZIER;
      }
      for (const ShapePoint &point : spline.points) {
        PaintVectorPoint &out_point = points[point_offset++];
        copy_v2_v2(out_point.co, point.co);
        copy_v2_v2(out_point.handle_l, point.handle_left);
        copy_v2_v2(out_point.handle_r, point.handle_right);
        out_point.width_factor = point.width_factor;
        /* The points array is value-initialized, so only set the bits; nothing to clear. */
        if (point.corner) {
          out_point.flag |= PAINT_VECTOR_POINT_CORNER;
        }
        if (point.auto_handles) {
          out_point.flag |= PAINT_VECTOR_POINT_AUTO_HANDLES;
        }
      }
    }

    /* The style belongs to the item itself, so it follows the item through add / delete / reorder.
     * An item that never got one (e.g. a freshly built document) gets the default style. */
    if (item.style) {
      BKE_paint_vector_item_style_copy(pvi, *item.style);
    }
    else {
      BKE_paint_vector_item_style_init(pvi);
    }
  }

  r_pv.active_item = std::min<int>(std::max(doc.active, 0), std::max(r_pv.items_num - 1, 0));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Read (DNA -> runtime)
 * \{ */

void paint_vector_to_document(const PaintVector &pv, VectorDocument &r_doc)
{
  Vector<VectorItem> items;
  items.reserve(pv.items_num);

  for (int item_i = 0; item_i < pv.items_num; item_i++) {
    const PaintVectorItem &pvi = pv.items[item_i];
    VectorItem item;
    item.type = VectorItemType(pvi.type);
    space_from_dna(pvi.space, item.space);

    PaintShape &shape = item.shape;
    shape.type = ePaintShapeType(pvi.shape_type);
    copy_v2_v2(shape.origin, pvi.origin);
    shape.origin_is_custom = (pvi.flag & PAINT_VECTOR_ITEM_ORIGIN_CUSTOM) != 0;
    copy_v2_v2(shape.center, pvi.center);
    copy_v2_v2(shape.half_size, pvi.half_size);
    shape.rotation = pvi.rotation;
    copy_v4_v4(shape.corner_radius, pvi.corner_radius);
    shape.polygon_sides = pvi.polygon_sides;
    shape.splines.reserve(pvi.splines_num);
    for (int spline_i = 0; spline_i < pvi.splines_num; spline_i++) {
      const PaintVectorSpline &src = pvi.splines[spline_i];
      ShapeSpline spline;
      spline.cyclic = (src.flag & PAINT_VECTOR_SPLINE_CYCLIC) != 0;
      spline.is_bezier = (src.flag & PAINT_VECTOR_SPLINE_BEZIER) != 0;
      const int point_end = std::min(src.point_offset + src.point_num, pvi.points_num);
      for (int point_i = src.point_offset; point_i < point_end; point_i++) {
        const PaintVectorPoint &src_point = pvi.points[point_i];
        ShapePoint point;
        copy_v2_v2(point.co, src_point.co);
        copy_v2_v2(point.handle_left, src_point.handle_l);
        copy_v2_v2(point.handle_right, src_point.handle_r);
        point.width_factor = src_point.width_factor;
        point.corner = (src_point.flag & PAINT_VECTOR_POINT_CORNER) != 0;
        point.auto_handles = (src_point.flag & PAINT_VECTOR_POINT_AUTO_HANDLES) != 0;
        spline.points.append(point);
      }
      shape.splines.append(std::move(spline));
    }

    item.style = vector_item_style_make(pvi.style);

    items.append(std::move(item));
  }

  vector_document_set_items(r_doc, std::move(items));
  r_doc.active = std::min<int>(std::max(pv.active_item, 0),
                               std::max(int(r_doc.items.size()) - 1, 0));
}

/** \} */

}  // namespace blender::ed::sculpt_paint::shape
