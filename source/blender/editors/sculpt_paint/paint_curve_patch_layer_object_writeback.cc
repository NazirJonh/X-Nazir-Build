/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Curve Patch object-source write-back: a shape edit in the tool lands in the Curves object the
 * patch reads from, so the object stays the single source of truth while it is set.
 */

#include "paint_curve_patch_layer.hh"

#include <algorithm>
#include <utility>

#include "BLI_generic_virtual_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_index_range.hh"
#include "BLI_map.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_offset_indices.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_ID.h"
#include "DNA_object_types.h"
#include "DNA_paint_vector_types.h"

#include "BKE_attribute.hh"
#include "BKE_curves.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"
#include "BKE_paint_layers_curve_patch_object.hh"
#include "BKE_paint_vector.hh"

#include "DEG_depsgraph.hh"

#include "paint_curve_patch_document.hh"
#include "paint_curve_patch_layer_object_source_intern.hh"
#include "paint_curve_patch_session.hh"

namespace blender::ed::sculpt_paint {

/* Map every point of \a curve, and its bezier handles when it has any, through \a m. The handle
 * attributes are created before they are read, so a bezier whose publish copy lacks them does not
 * collapse at the origin. */
void curve_patch_transform_points(bke::CurvesGeometry &curve, const float4x4 &m)
{
  for (float3 &position : curve.positions_for_write()) {
    position = math::transform_point(m, position);
  }
  if (curve.has_curve_with_type(CURVE_TYPE_BEZIER)) {
    for (float3 &handle : curve.handle_positions_left_for_write()) {
      handle = math::transform_point(m, handle);
    }
    for (float3 &handle : curve.handle_positions_right_for_write()) {
      handle = math::transform_point(m, handle);
    }
  }
  curve.tag_positions_changed();
}

/* Write \a src_curve's points and handles into the range \a dst_points of \a r_dst, mapped through
 * \a m. \a r_dst is the object's own geometry: its topology and every attribute outside this range
 * stay as they are. */
static void curve_patch_write_patch_points(bke::CurvesGeometry &r_dst,
                                           const bke::CurvesGeometry &src_curve,
                                           const IndexRange dst_points,
                                           const float4x4 &m)
{
  const Span<float3> src_positions = src_curve.positions();
  MutableSpan<float3> dst_positions = r_dst.positions_for_write();
  for (const int i : dst_points.index_range()) {
    dst_positions[dst_points[i]] = math::transform_point(m, src_positions[i]);
  }
  if (!src_curve.has_curve_with_type(CURVE_TYPE_BEZIER)) {
    return;
  }
  const Span<float3> src_left = *src_curve.handle_positions_left();
  const Span<float3> src_right = *src_curve.handle_positions_right();
  const VArray<int8_t> src_types_left = src_curve.handle_types_left();
  const VArray<int8_t> src_types_right = src_curve.handle_types_right();
  MutableSpan<float3> dst_left = r_dst.handle_positions_left_for_write();
  MutableSpan<float3> dst_right = r_dst.handle_positions_right_for_write();
  MutableSpan<int8_t> dst_types_left = r_dst.handle_types_left_for_write();
  MutableSpan<int8_t> dst_types_right = r_dst.handle_types_right_for_write();
  for (const int i : dst_points.index_range()) {
    const int dst_i = dst_points[i];
    dst_left[dst_i] = math::transform_point(m, src_left[i]);
    dst_right[dst_i] = math::transform_point(m, src_right[i]);
    dst_types_left[dst_i] = src_types_left[i];
    dst_types_right[dst_i] = src_types_right[i];
  }
}

/* Which patch writes which spline: a claim pairs a patch of the document with the spline of the
 * object its curve reads. A patch with no points claims nothing (there is no shape to write); its
 * spline, if any, is carried over. Two patches claiming one spline cannot come out of a session;
 * if one ever does, the last wins. A claim past the object's last spline appends a new one. */
struct CurvePatchWriteClaims {
  Vector<int> patch_of_spline;
  Vector<int> appended_patches;
  bool topology_changed = false;
  int writable_patches = 0;
};

/* False when a patch is a nurbs: its shape depends on the order and knots a rebuild does not
 * carry, so an object holding one is a read-only source. */
static bool curve_patch_write_claims_prepare(CurvePatchSession &session,
                                             const Span<std::pair<int, int>> claims,
                                             const int old_curves_num,
                                             const OffsetIndices<int> old_points_by_curve,
                                             CurvePatchWriteClaims &r_claims)
{
  r_claims.patch_of_spline = Vector<int>(old_curves_num, -1);
  for (const std::pair<int, int> &claim : claims) {
    const int patch_i = claim.first;
    const int spline_i = claim.second;
    const CurvePatchItem &item = session.doc.patches[patch_i];
    if (item.control_curve.points_num() == 0) {
      continue;
    }
    if (item.control_curve.curve_types()[0] == CURVE_TYPE_NURBS) {
      return false;
    }
    r_claims.writable_patches++;
    if (spline_i < 0 || spline_i >= old_curves_num) {
      r_claims.appended_patches.append(patch_i);
      continue;
    }
    r_claims.patch_of_spline[spline_i] = patch_i;
    r_claims.topology_changed |= item.control_curve.points_num() !=
                                 old_points_by_curve[spline_i].size();
  }
  return true;
}

/* The topology is the object's own: the standard curve data is written in place and nothing else
 * is touched, so the object's own attributes keep every value (selection, custom metadata,
 * per-point parameters it carries). */
static void curve_patch_write_in_place(CurvePatchSession &session,
                                       bke::CurvesGeometry &dst,
                                       const Span<int> patch_of_spline,
                                       const OffsetIndices<int> old_points_by_curve,
                                       const float4x4 &m)
{
  MutableSpan<int8_t> curve_types = dst.curve_types_for_write();
  MutableSpan<bool> cyclic = dst.cyclic_for_write();
  MutableSpan<int> resolution = dst.resolution_for_write();
  for (const int spline_i : dst.curves_range()) {
    const int patch_i = patch_of_spline[spline_i];
    if (patch_i < 0) {
      continue;
    }
    const bke::CurvesGeometry &src_curve = session.doc.patches[patch_i].control_curve;
    curve_types[spline_i] = src_curve.curve_types()[0];
    cyclic[spline_i] = src_curve.cyclic()[0];
    resolution[spline_i] = src_curve.resolution()[0];
    curve_patch_write_patch_points(dst, src_curve, old_points_by_curve[spline_i], m);
  }
  /* The radius stays the object's own: the session does not edit it (the per-point opacity lives
   * in the not-persisted `paintcurve_*` attributes), so writing the document's copy back would only
   * overwrite the user's values with an identical one. */
  dst.update_curve_types();
  /* Only the positions moved: the topology is the object's own, so the topological caches are
   * still valid and are not invalidated. */
  dst.tag_positions_changed();
}

/* Build one geometry from per-spline pieces, in the order they are given: a carried-over piece
 * keeps every attribute it holds, a rebuilt one carries the attributes its publish copy holds. */
static bke::CurvesGeometry curve_patch_join_pieces(const Span<bke::CurvesGeometry> sources)
{
  int points_total = 0;
  for (const bke::CurvesGeometry &src : sources) {
    points_total += src.points_num();
  }
  const int curves_total = sources.size();

  bke::CurvesGeometry joined;
  joined.resize(points_total, curves_total);
  MutableSpan<int> offsets = joined.offsets_for_write();
  MutableSpan<int8_t> curve_types = joined.curve_types_for_write();
  MutableSpan<bool> cyclic = joined.cyclic_for_write();
  MutableSpan<int> resolution = joined.resolution_for_write();
  const bool any_bezier = std::any_of(
      sources.begin(), sources.end(), [](const bke::CurvesGeometry &src) {
        return src.has_curve_with_type(CURVE_TYPE_BEZIER);
      });
  MutableSpan<float3> dst_handles_left = any_bezier ?
                                             joined.handle_positions_left_for_write() :
                                             MutableSpan<float3>();
  MutableSpan<float3> dst_handles_right = any_bezier ?
                                              joined.handle_positions_right_for_write() :
                                              MutableSpan<float3>();
  MutableSpan<int8_t> dst_types_left = any_bezier ? joined.handle_types_left_for_write() :
                                                    MutableSpan<int8_t>();
  MutableSpan<int8_t> dst_types_right = any_bezier ? joined.handle_types_right_for_write() :
                                                     MutableSpan<int8_t>();
  MutableSpan<float3> dst_positions = joined.positions_for_write();
  /* A source without a radius attribute answers the generic 0.01 curve default; the patch
   * convention is 1.0 = full brush size, so such a range gets the 1.0 fill instead. */
  MutableSpan<float> dst_radius = joined.radius_for_write();

  int point_ofs = 0;
  for (const int src_i : sources.index_range()) {
    const bke::CurvesGeometry &src_curve = sources[src_i];
    const int spline_points = src_curve.points_num();
    const IndexRange dst_points(point_ofs, spline_points);
    point_ofs += spline_points;
    offsets[src_i] = dst_points.start();
    curve_types[src_i] = src_curve.curve_types()[0];
    cyclic[src_i] = src_curve.cyclic()[0];
    resolution[src_i] = src_curve.resolution()[0];

    const Span<float3> src_positions = src_curve.positions();
    if (src_curve.attributes().contains("radius")) {
      const VArray<float> src_radius = src_curve.radius();
      for (const int i : IndexRange(spline_points)) {
        dst_positions[dst_points[i]] = src_positions[i];
        dst_radius[dst_points[i]] = src_radius[i];
      }
    }
    else {
      dst_radius.slice(dst_points).fill(1.0f);
      for (const int i : IndexRange(spline_points)) {
        dst_positions[dst_points[i]] = src_positions[i];
      }
    }
    if (any_bezier && src_curve.has_curve_with_type(CURVE_TYPE_BEZIER)) {
      const Span<float3> src_left = *src_curve.handle_positions_left();
      const Span<float3> src_right = *src_curve.handle_positions_right();
      const VArray<int8_t> src_types_left = src_curve.handle_types_left();
      const VArray<int8_t> src_types_right = src_curve.handle_types_right();
      for (const int i : IndexRange(spline_points)) {
        dst_handles_left[dst_points[i]] = src_left[i];
        dst_handles_right[dst_points[i]] = src_right[i];
        dst_types_left[dst_points[i]] = src_types_left[i];
        dst_types_right[dst_points[i]] = src_types_right[i];
      }
    }
  }
  offsets[curves_total] = points_total;

  /* The custom attributes are gathered by index: every source is one spline, so its points map one
   * to one onto its destination range, and a source without the attribute keeps the default. The
   * `paintcurve_*` patch parameters are excluded: they are the per-curve parameters the object mode
   * does not persist (see the TODO in #curve_patch_document_to_object's doc). */
  Map<StringRefNull, bke::AttributeMetaData> gathered;
  for (const bke::CurvesGeometry &src : sources) {
    src.attributes().foreach_attribute([&](const bke::AttributeIter &it) {
      if (ELEM(it.name,
               "position",
               "radius",
               "handle_left",
               "handle_right",
               "handle_type_left",
               "handle_type_right",
               "cyclic",
               "resolution",
               "curve_type") ||
          it.name.startswith("paintcurve_") || gathered.contains(it.name))
      {
        return;
      }
      gathered.add(it.name, bke::AttributeMetaData{it.domain, it.data_type});
    });
  }
  bke::MutableAttributeAccessor dst_attrs = joined.attributes_for_write();
  for (const auto &[name, meta] : gathered.items()) {
    if (!dst_attrs.add(name, meta.domain, meta.data_type, bke::AttributeInitDefaultValue())) {
      continue;
    }
    bke::GSpanAttributeWriter writer = dst_attrs.lookup_for_write_span(name);
    int piece_point_ofs = 0;
    for (const int src_i : sources.index_range()) {
      const bke::CurvesGeometry &src_curve = sources[src_i];
      const IndexRange dst_range = (meta.domain == bke::AttrDomain::Curve) ?
                                       IndexRange(src_i, 1) :
                                       IndexRange(piece_point_ofs, src_curve.points_num());
      piece_point_ofs += src_curve.points_num();
      const bke::GAttributeReader reader = src_curve.attributes().lookup(name);
      if (!reader) {
        /* The piece does not carry the attribute: its range keeps the default value. */
        continue;
      }
      /* A computed attribute is not stored as a span; #GVArraySpan materializes it, so this does
       * not read through `get_internal_span()`'s precondition. */
      writer.span.slice(dst_range).copy_from(GVArraySpan(reader.varray));
    }
    writer.finish();
  }
  return joined;
}

/* The topology changed: the geometry is rebuilt from per-spline pieces, in the object's spline
 * order, appended patches last. */
static void curve_patch_write_rebuild(CurvePatchSession &session,
                                      bke::CurvesGeometry &dst,
                                      const Span<int> patch_of_spline,
                                      const CurvePatchWriteClaims &claims,
                                      const float4x4 &m)
{
  const int old_curves_num = patch_of_spline.size();
  Vector<bke::CurvesGeometry> sources;
  sources.reserve(old_curves_num + int(!claims.appended_patches.is_empty()));
  for (const int spline_i : IndexRange(old_curves_num)) {
    const int patch_i = patch_of_spline[spline_i];
    if (patch_i >= 0) {
      bke::CurvesGeometry piece = session.doc.patches[patch_i].control_curve;
      curve_patch_transform_points(piece, m);
      sources.append(std::move(piece));
    }
    else {
      /* Carried over whole: nothing about it was edited, attributes included. */
      sources.append(bke::curves_copy_curve_selection(dst, IndexMask(IndexRange(spline_i, 1)), {}));
    }
  }
  for (const int patch_i : claims.appended_patches) {
    bke::CurvesGeometry piece = session.doc.patches[patch_i].control_curve;
    curve_patch_transform_points(piece, m);
    sources.append(std::move(piece));
  }

  dst = curve_patch_join_pieces(sources);
  dst.update_curve_types();
  dst.tag_positions_changed();
  dst.tag_topology_changed();
}

/** Write the document's patches back into the curve object's Curves data: in the object source
 * mode the object is the single source of truth, so a shape edit in the tool lands in the
 * object's own geometry -- the same two-way contract the PaintCurve has with its source object
 * (#paintcurve_sync_to_source_object). The curves are mapped from the painted object's local
 * space back into the curve object's, the inverse of the publish mapping
 * (#curve_patch_painted_to_curve_space). The frozen per-curve parameters have no object-side
 * storage and are not written back; a spline the publish hid for being too short is written back
 * with its own points, so nothing the object held is lost.
 *
 * The splines are addressed through #CurvePatchItem::source_curve_index, not the patch order: the
 * publish skips splines (the degenerate ones), so the two orders diverge. A patch with no live
 * source spline appends as a new one; a spline no patch claims is carried over untouched. With
 * the topology unchanged (no appended patch, no point count changed) the write goes straight
 * into the existing buffers and touches nothing else, so every attribute of the object keeps its
 * values. A topology change rebuilds the geometry from per-spline pieces: the carried-over
 * splines keep all their attributes, a rebuilt spline carries the attributes its publish copy
 * holds (a changed point count has no meaningful per-point gather; the pieces that did not move
 * keep the object's values). TODO: persist the per-curve patch parameters as optional
 * `paintcurve_*` attributes of the object's splines -- the session document already carries them,
 * only this write-back drops them.
 *
 * Nurbs patches are the one hard exception: their shape depends on the nurbs order and knots,
 * which a rebuild does not carry -- an object holding any nurbs patch is a read-only source, and
 * nothing is written back at all.
 *
 * \return true when the geometry was written. */
static bool curve_patch_document_to_object(CurvePatchSession &session,
                                           Object &curve_object,
                                           const Span<std::pair<int, int>> claims)
{
  if (session.owner == nullptr || curve_object.type != OB_CURVES || curve_object.data == nullptr) {
    return false;
  }
  /* Re-checked here, not only where the object was picked: it could have become a linked or an
   * override reference in between. */
  if (!ID_IS_EDITABLE(&curve_object.id) || !ID_IS_EDITABLE(static_cast<ID *>(curve_object.data))) {
    return false;
  }
  Curves *curves_id = id_cast<Curves *>(curve_object.data);
  bke::CurvesGeometry &dst = curves_id->geometry.wrap();
  const int old_curves_num = dst.curves_num();
  const OffsetIndices<int> old_points_by_curve = dst.points_by_curve();

  CurvePatchWriteClaims write_claims;
  if (!curve_patch_write_claims_prepare(
          session, claims, old_curves_num, old_points_by_curve, write_claims))
  {
    return false;
  }
  if (write_claims.writable_patches == 0) {
    /* Nothing to write; the object is left exactly as it stands. */
    return false;
  }

  const float4x4 curve_from_source = curve_patch_painted_to_curve_space(curve_object,
                                                                        *session.owner);
  if (write_claims.appended_patches.is_empty() && !write_claims.topology_changed) {
    curve_patch_write_in_place(
        session, dst, write_claims.patch_of_spline, old_points_by_curve, curve_from_source);
  }
  else {
    curve_patch_write_rebuild(
        session, dst, write_claims.patch_of_spline, write_claims, curve_from_source);
  }

  /* The object's curves are user data the material only reads: the geometry edit follows the
   * normal object-data path, exactly like the paint curve's sync back to its source object. The
   * undo step is the commit's own "Curve Patch Layer Edit" memfile push, which records the
   * object's data with it; the write-back itself pushes none, like the PaintCurve sync. */
  DEG_id_tag_update(&curves_id->id, ID_RECALC_GEOMETRY);
  return true;
}

/** Write the document's curves that name an external object back into the objects: per object,
 * the patches that read it with the spline each one reads. The patches' own items are not touched
 * (see #curve_patch_document_to_vector, which skips the geometry of an object-backed curve). */
void curve_patch_document_to_objects(CurvePatchSession &session, MaterialPaintLayer &row)
{
  MaterialPaintLayerCurvePatch &payload = *row.curve_patch;
  if (payload.vectors == nullptr || !BKE_paint_layers_curve_patch_reads_object(&payload)) {
    return;
  }
  const PaintVector &pv = *payload.vectors;
  struct ObjectClaims {
    Object *object;
    Vector<std::pair<int, int>> claims;
  };
  Vector<ObjectClaims> by_object;
  for (const int patch_i : session.doc.patches.index_range()) {
    const int item_i = session.doc.patches[patch_i].source_curve_index;
    if (item_i < 0 || item_i >= pv.items_num) {
      continue;
    }
    const PaintVectorItem &item = pv.items[item_i];
    if (item.type != PAINT_VECTOR_ITEM_CURVE_PATCH || item.curve_object == nullptr) {
      continue;
    }
    ObjectClaims *entry = nullptr;
    for (ObjectClaims &candidate : by_object) {
      if (candidate.object == item.curve_object) {
        entry = &candidate;
        break;
      }
    }
    if (entry == nullptr) {
      by_object.append({item.curve_object, {}});
      entry = &by_object.last();
    }
    entry->claims.append({patch_i, item.curve_object_spline});
  }

  bool any_written = false;
  for (ObjectClaims &entry : by_object) {
    any_written |= curve_patch_document_to_object(session, *entry.object, entry.claims);
  }
  /* The shape this write lands is the shape the next refresh reads: registering its hash makes the
   * refresh treat the echo of this write as the session's own edit (the session has already
   * stamped the new shape) instead of re-publishing and re-stamping a second time. */
  if (any_written && session.layer_material != nullptr) {
    BKE_paint_layers_curve_patch_object_expected_hash_set(
        *session.layer_material, row, BKE_paint_layers_curve_patch_row_hash(payload, nullptr));
  }
}

}  // namespace blender::ed::sculpt_paint
