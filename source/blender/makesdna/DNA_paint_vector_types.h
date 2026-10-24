/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup DNA
 *
 * Paint Vector: the persisted, non-destructive vector document of the Paint Shapes + Curve Patch
 * tools. One ID holds a list of items (Shape / Curve Patch), each with its geometry,
 * the space it lives in, its origin and an embedded snapshot of the shape settings. The runtime
 * counterpart is `VectorDocument` in `editors/sculpt_paint/paint_vector_document.hh`.
 *
 * The item arrays are flat owned arrays (`points` / `splines`), not listbases, so the whole
 * document is one `blend_write` / `blend_read` pass; see `intern/paint_vector.cc`.
 *
 * \note No bake images live here: a bake writes into the target channel maps resolved by
 * the shared target resolver, not into maps owned by this ID.
 */

#pragma once

#include "DNA_ID.h"
#include "DNA_curves_types.h"
#include "DNA_scene_types.h"

namespace blender {

/** #PaintVectorItem::type -- matches runtime `VectorItemType`. */
enum PaintVectorItemType {
  PAINT_VECTOR_ITEM_SHAPE = 0,
  PAINT_VECTOR_ITEM_CURVE_PATCH = 1,
};

/** #PaintVectorSpace::type -- matches runtime `ShapeSpaceType`. */
enum PaintVectorSpaceType {
  PAINT_VECTOR_SPACE_CANVAS_UV = 0,
  PAINT_VECTOR_SPACE_VIEW_PROJECTOR = 1,
  PAINT_VECTOR_SPACE_SURFACE_ANCHORED = 2,
};

/** #PaintVectorItem::flag. */
enum PaintVectorItemFlag {
  /** #PaintVectorItem::origin is user-set and must survive point edits. */
  PAINT_VECTOR_ITEM_ORIGIN_CUSTOM = 1 << 0,
  /** A Curve Patch item still at its Add-time default position (a segment on the object origin,
   * usually inside the mesh); the tool entry lays it on the surface under the view and clears the
   * flag. */
  PAINT_VECTOR_ITEM_CURVE_SEED = 1 << 1,
};

/** #PaintVectorPoint::flag. */
enum PaintVectorPointFlag {
  /** The segment leading into this point is a straight line (polyline corner). */
  PAINT_VECTOR_POINT_CORNER = 1 << 0,
  /** Evaluate the handles from the neighbors instead of the stored ones. */
  PAINT_VECTOR_POINT_AUTO_HANDLES = 1 << 1,
  /** #PaintVectorPoint::surface_uv is set (SurfaceAnchored). */
  PAINT_VECTOR_POINT_HAS_SURFACE_UV = 1 << 2,
};

/** #PaintVectorSpline::flag. */
enum PaintVectorSplineFlag {
  PAINT_VECTOR_SPLINE_CYCLIC = 1 << 0,
  PAINT_VECTOR_SPLINE_BEZIER = 1 << 1,
};

/** #PaintVector::flag. */
enum PaintVectorFlag {
  /**
   * The target maps were baked at least once. Until Stack Layers, re-editing an already baked
   * PaintVector only writes the document (no re-bake); the flag is the explicit "baked" marker
   * (deliberately not derived from the revision counters).
   */
  PAINT_VECTOR_BAKED = 1 << 0,
};

/** One control point of a #PaintVectorSpline. */
struct PaintVectorPoint {
  float co[2] = {0.0f, 0.0f};
  float handle_l[2] = {0.0f, 0.0f};
  float handle_r[2] = {0.0f, 0.0f};
  /** Surface anchor; valid only when #PAINT_VECTOR_POINT_HAS_SURFACE_UV is set. */
  float surface_uv[2] = {0.0f, 0.0f};
  /** Stroke width multiplier at this point, interpolated along the spline. */
  float width_factor = 1.0f;
  /** #PaintVectorPointFlag. */
  short flag = PAINT_VECTOR_POINT_AUTO_HANDLES;
  short _pad = 0;
};

/**
 * One spline of a #PaintVectorItem. Points live in the owning item's flat `points` array; the
 * spline is a view `[point_offset, point_offset + point_num)` into it. This is what keeps the
 * whole document a single owned array rather than nested allocations.
 */
struct PaintVectorSpline {
  int point_offset = 0;
  int point_num = 0;
  /** #PaintVectorSplineFlag. */
  short flag = 0;
  short _pad = 0;
};

/** The space a #PaintVectorItem lives in; POD, serialized whole. CanvasUV uses the tile fields,
 * ViewProjector the matrices. SurfaceAnchored adds per-point anchors on the item's points. */
struct PaintVectorSpace {
  int type = PAINT_VECTOR_SPACE_CANVAS_UV;
  /** CanvasUV: reference UDIM tile and its pixel resolution. */
  int ref_tile = 1001;
  int ref_tile_size[2] = {1024, 1024};
  /** ViewProjector: frozen view matrices and region size at draw time. */
  float persmat[4][4] = {};
  float viewinv[4][4] = {};
  float clip_start = 0.0f;
  float clip_end = 0.0f;
  int win_size[2] = {0, 0};
  short is_persp = 0;
  short _pad = 0;

  /* SurfaceAnchored: tangent-plane frame + surface restoration cache. */
  float anchor_co[3] = {0.0f, 0.0f, 0.0f};
  int anchor_tri = -1;
  /** True when the anchor UV / triangle are valid (a UV map existed at creation). */
  char anchor_has_surface_uv = 0;
  char _pad_anchor[7] = {};
  float anchor_normal[3] = {0.0f, 0.0f, 1.0f};
  float anchor_tangent[3] = {1.0f, 0.0f, 0.0f};
  /** Object-space UV of the anchor (restoration via #geometry::ReverseUVSampler). */
  float anchor_surface_uv[2] = {0.0f, 0.0f};
  /** Region pixels per world unit at the anchor, so pixel-authored style keeps its size. */
  float anchor_px_per_unit = 1.0f;
  /** Rejection depth along the anchor normal, world units (0 = unlimited). */
  float anchor_max_depth = 0.0f;
  /** UV map the anchor was sampled through; empty = the object's active UV map. */
  char surface_uv_map[64] = "";
};

/**
 * DNA mirror of `bke::CurvePatchParams` (`BKE_curve_patch.hh`), which cannot live in DNA itself.
 * Field order and names match the source struct one to one, so the trivial converter in
 * `BKE_paint_vector.hh` stays a field-by-field copy. Enums are widened to `int`; bools to `int`.
 */
struct PaintVectorCurveParams {
  float radius = 0.0f;
  float radius_per_size = 0.0f;
  int swap_axis = 0;

  int length_mode = 0;
  int length_repeat = 1;

  int end_falloff_mode = 0;
  int end_falloff_percent = 0;
  int start_point_shape = 0;
  int end_point_shape = 0;

  int stamp_mode = 0;
  int stamp_layout = 0;
  int stamp_projection = 0;
  int stamp_use_random_angle = 0;
  float stamp_size_random = 0.0f;
  float stamp_strength_random = 0.0f;
  int stamp_seed = 0;
  float spacing_frac = 0.0f;
  float jitter_amount = 0.0f;
  float base_angle = 0.0f;
  float random_angle = 0.0f;

  float plane_normal[3] = {0.0f, 0.0f, 1.0f};

  int final_quality = 0;
  char _pad[4] = {};
};

/** #PaintVectorSurfaceBinding::state. */
enum ePaintVectorBindState {
  /** Control points are plain object-space positions; nothing is tracked. */
  PAINT_VECTOR_BIND_NONE = 0,
  /** Every control point is anchored to the surface (see #PaintVectorSurfaceBinding). */
  PAINT_VECTOR_BIND_BOUND = 1,
  /** The surface topology no longer matches: the points stay where they were stored and the user
   * must accept or re-bind them explicitly. */
  PAINT_VECTOR_BIND_BROKEN = 2,
};

/**
 * Surface anchoring of a Curve Patch item, so the curve follows a deformed or animated mesh.
 *
 * The per-point anchors are attributes of #PaintVectorItem::curve_patch_geometry (serialized with
 * it): `paintcurve_bind_tri` (int, corner-tri index of the bind mesh, -1 = unbound),
 * `paintcurve_bind_bary` (float3 barycentric weights) and `paintcurve_bind_offset` (float, distance
 * along the interpolated normal). This struct only holds what validates them: the topology
 * signature of the mesh they were computed on. A mismatch means the indices are meaningless, so
 * the curve is never re-evaluated through them (state #PAINT_VECTOR_BIND_BROKEN).
 */
struct PaintVectorSurfaceBinding {
  /** Hash of the face/corner topology of the bind mesh (positions excluded). */
  uint32_t topology_hash = 0;
  int verts_num = 0;
  int corner_tris_num = 0;
  /** #ePaintVectorBindState. */
  int state = PAINT_VECTOR_BIND_NONE;
};

/**
 * One element of a #PaintVector: the geometry, its space and the style snapshot.
 *
 * \note Ownership: the struct is shallow-copied *only* inside `paint_vector_copy_data` /
 * #BKE_paint_vector_item_add, where the owning pointers (`points`, `splines`, the style's
 * `CurveMapping` / `ColorBand`) are duplicated explicitly. Everywhere else the pointers are
 * shared, never byte-copied without fixing the owned data. `DNA_DEFINE_CXX_METHODS` is
 * intentionally not used (the explicit copy paths rely on assignment).
 *
 * \note The embedded #PaintVectorItem::curve_patch_geometry is the one non-flat owned member: it
 * is bitwise-copied like the rest (the DNA `CurvesGeometry` stays trivial), but the copy sites
 * must re-establish it through a placement-new `bke::CurvesGeometry` whenever it holds curves,
 * and the free path destroys it. Same contract as #PaintCurve::geometry.
 */
struct PaintVectorItem {
  /* Owned flat arrays. Kept together so the struct has no implicit padding between the pointers
   * and their counts. */
  PaintVectorPoint *points = nullptr;
  PaintVectorSpline *splines = nullptr;
  int points_num = 0;
  int splines_num = 0;

  PaintVectorSpace space = {};

  /** Pivot of the whole-item Move/Rotate/Scale gestures, shape-space. */
  float origin[2] = {0.0f, 0.0f};
  /** #PaintVectorItemFlag. */
  short flag = 0;
  /* Explicit padding (keeps the parametric block 4-byte aligned). */
  short _pad = 0;

  /* Parametric data of Rect/Ellipse/Polygon, used when `splines_num == 0`. */
  float center[2] = {0.0f, 0.0f};
  float half_size[2] = {0.0f, 0.0f};
  float rotation = 0.0f;
  float corner_radius[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  int polygon_sides = 6;

  /** Embedded style snapshot: the item is self-contained across reload / undo. Owns its
   * profiles (`stroke_profile` / `fill_profile` / `stroke_ramp`) and references the curve source
   * IDs through `foreach_id`. */
  PaintShapeSettings style = {};

  /** #PaintVectorItemType. */
  int type = PAINT_VECTOR_ITEM_SHAPE;
  /** #ePaintShapeType of the geometry (Rect/Ellipse/Polygon/...), independent of #type above. */
  int shape_type = PAINT_SHAPE_RECT;

  /** Curve Patch item payload; valid when #type == #PAINT_VECTOR_ITEM_CURVE_PATCH.
   * Embedded like #PaintCurve::geometry; attribute contract: required position/handles+types/
   * radius, optional per-point paintcurve_surface_normal/stamp_strength/stamp_angle/
   * stamp_texture(-1=Auto)/stamp_seed (defaults applied on read). */
  CurvesGeometry curve_patch_geometry;
  /** Frozen per-curve parameters, a mirror of `bke::CurvePatchParams`. */
  PaintVectorCurveParams curve_patch_params = {};
  /** Name shown in the Active Tool curve list. */
  char curve_name[64] = "";
  /* Pads the struct to a multiple of the pointer size, as every DNA struct must. */
  char _pad_curve_patch[4] = {};
  /** Surface anchoring of the curve's control points. */
  PaintVectorSurfaceBinding surface_binding = {};
};

/**
 * PaintVector: a persisted Vector document, referenced by a Material through the ID-property
 * group `pbr_paint_vectors`. One PaintVector is the future unit of one Stack Layers row.
 */
struct PaintVector {
#ifdef __cplusplus
  /** See #ID_Type comment for why this is here. */
  static constexpr ID_Type id_type = ID_PV;
#endif

  ID id;
  PaintVectorItem *items = nullptr;
  int items_num = 0;
  /** Active item index; the session validates it. */
  int active_item = 0;
  /** Bumped whenever the document changes; compared with #baked_revision for "needs rebake".
   * No bake images live here; the baked result is in the target channel maps. */
  int bake_revision = 0;
  /** #bake_revision the target maps were last baked from. */
  int baked_revision = 0;
  short flag = 0;
  /* Pad to a multiple of the struct's 8-byte alignment (ID id + pointer). */
  char _pad[6] = {};
};

}  // namespace blender
