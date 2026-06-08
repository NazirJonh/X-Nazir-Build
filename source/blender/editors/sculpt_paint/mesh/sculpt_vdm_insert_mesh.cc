/* SPDX-FileCopyrightText: 2025 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Vector Displacement Map (VDM) "insert mesh" tool.
 *
 * A VDM Draw stroke normally deforms the active mesh. In insert-mesh mode it instead behaves
 * like a stamp: the brush state at the first dab of the stroke is captured (see #VDMStampData)
 * and, once the stroke finishes, a brand new watertight volume is generated from the texture.
 *
 * The generated volume is a closed solid made of three parts:
 * - a displaced top surface sampled from the VDM texture,
 * - a flat bottom on the brush plane,
 * - a side wall (skirt) connecting their borders.
 *
 * This makes the result directly usable for boolean/join operations against the main mesh,
 * which is the whole point: a VDM brush becomes a lightweight, instantly placed and scaled
 * source of separate geometry the artist can refine before merging.
 */

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_brush.hh"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"

#include "BLI_array.hh"
#include "BLI_index_range.hh"
#include "BLI_math_matrix.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_object.hh"
#include "ED_screen.hh"
#include "ED_sculpt.hh"

#include "bmesh.hh"
#include "bmesh_tools.hh"

#include "MEM_guardedalloc.h"

#include "../paint_intern.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint {

static bool vdm_insert_mesh_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  if (ob != nullptr && ob->mode == OB_MODE_SCULPT) {
    return ED_operator_object_active_editable_mesh(C);
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name VDM Sampling
 *
 * Both functions reproduce the math used while sculpting (`sculpt_apply_texture()` and
 * `calc_vertex_displacement()`) so the generated shape matches what the VDM Draw brush would
 * have carved into the active mesh. The brush state is read from the captured #VDMStampData
 * instead of the live #StrokeCache.
 * \{ */

/**
 * Object-space position of a point on the flat brush plane.
 *
 * \param uv: normalized brush-plane coordinates in [-1, 1]. The matrix already embeds the brush
 * location, orientation and radius, so the resulting point spans the full brush footprint.
 */
static float3 stamp_plane_point(const VDMStampData &stamp, const float2 &uv)
{
  float3 co(uv.x, uv.y, 0.0f);
  mul_m4_v3(stamp.brush_local_mat_inv.ptr(), co);
  return co;
}

/**
 * VDM displacement for a plane point, in object space.
 *
 * Mirrors `sculpt_apply_texture()` (the `MTEX_MAP_MODE_AREA` branch) for the texture lookup and
 * `calc_vertex_displacement()` for turning the sampled color into an object-space vector.
 */
static float3 stamp_displacement(const VDMStampData &stamp,
                                 const Brush &brush,
                                 SculptSession &ss,
                                 const float3 &plane_co)
{
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  if (!mtex->tex) {
    return float3(0.0f);
  }

  /* Sampling point: `sculpt_apply_texture()` starts from `(brush_point - plane_offset)`. */
  float3 point = plane_co - stamp.plane_offset;
  if (stamp.radial_symmetry_pass) {
    mul_m4_v3(stamp.symm_rot_mat_inv.ptr(), point);
  }
  float3 symm_point = symmetry_flip(point, stamp.mirror_symmetry_pass);
  mul_m4_v3(stamp.brush_local_mat.ptr(), symm_point);

  const float x = symm_point.x * mtex->size[0] + mtex->ofs[0];
  const float y = symm_point.y * mtex->size[1] + mtex->ofs[1];

  float value;
  float4 rgba;
  paint_get_tex_pixel(mtex, x, y, ss.tex_pool, 0, &value, rgba);
  add_v3_fl(rgba, brush.texture_sample_bias);

  /* Color -> object-space displacement, matching `calc_vertex_displacement()`. */
  float3 disp = float3(rgba);
  mul_v3_fl(disp, stamp.bstrength);
  if (stamp.bstrength < 0.0f) {
    disp.x *= -1.0f;
    disp.y *= -1.0f;
  }
  for (int i = 0; i < 3; i++) {
    disp[i] *= math::safe_divide(1.0f, pow2f(brush.mtex.size[i]));
  }
  mul_mat3_m4_v3(stamp.brush_local_mat_inv.ptr(), disp);
  if (stamp.radial_symmetry_pass) {
    mul_m4_v3(stamp.symm_rot_mat.ptr(), disp);
  }
  return symmetry_flip(disp, stamp.mirror_symmetry_pass);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Volume Construction
 * \{ */

/**
 * Estimate a stamp grid resolution that matches the polygon density of \a mesh at the brush
 * footprint.
 *
 * Scans all edges whose midpoints lie within \a radius of \a center, computes the average edge
 * length, and returns how many quads fit across the stamp diameter at that scale.
 * Clamped to [2, 256]. Returns 32 as a safe default when no edges are found in the footprint.
 */
static int resolution_from_mesh_density(const Mesh &mesh,
                                        const float3 &center,
                                        const float radius)
{
  const Span<float3> positions = mesh.vert_positions();
  const Span<int2> edges = mesh.edges();

  float total_length = 0.0f;
  int count = 0;
  const float radius_sq = radius * radius;

  for (const int2 &edge : edges) {
    const float3 mid = (positions[edge[0]] + positions[edge[1]]) * 0.5f;
    const float3 to_mid = mid - center;
    if (math::dot(to_mid, to_mid) <= radius_sq) {
      total_length += math::length(positions[edge[1]] - positions[edge[0]]);
      count++;
    }
  }

  if (count == 0 || total_length == 0.0f) {
    return 32;
  }

  const float avg_edge_len = total_length / float(count);
  const int resolution = int(2.0f * radius / avg_edge_len);
  return math::clamp(resolution, 2, 256);
}

/**
 * Append one watertight VDM stamp volume to \a bm.
 *
 * Only grid quads that carry meaningful VDM displacement are generated — flat border padding
 * common in VDM textures is silently skipped. The resulting mesh therefore conforms to the
 * actual shape encoded in the texture rather than always being a rectangular block.
 *
 * A quad is "active" when at least one of its four corner vertices has displacement whose
 * magnitude exceeds 1 % of the grid cell size. Watertightness is preserved by emitting skirt
 * faces on every edge that borders an inactive quad (or the grid boundary). When the VDM
 * contains a recess, the flat bottom cap is sunk below the deepest displaced vertex so the
 * volume fully encloses concave geometry without self-intersection.
 *
 * New faces are tagged with #BM_ELEM_TAG so the caller can orient their normals outward without
 * touching pre-existing geometry.
 */
static void add_stamp_to_bmesh(BMesh *bm,
                               const VDMStampData &stamp,
                               const Brush &brush,
                               SculptSession &ss,
                               const int resolution)
{
  BLI_assert(resolution >= 2);
  const int side = resolution;
  const int last = side - 1;
  const auto idx = [side](const int i, const int j) { return i * side + j; };
  const auto qidx = [last](const int i, const int j) { return i * last + j; };

  /* Brush normal in object space: transform local (0, 0, 1) through `brush_local_mat_inv`. */
  float3 brush_normal(0.0f, 0.0f, 1.0f);
  mul_mat3_m4_v3(stamp.brush_local_mat_inv.ptr(), brush_normal);
  brush_normal = math::normalize(brush_normal);

  /* === Pass 1: evaluate the raw VDM displacement for every grid point. === */
  const int grid_size = side * side;
  Array<float3> base_cos(grid_size);
  Array<float3> disp_vecs(grid_size);
  Array<float> active_lens(grid_size);

  for (const int i : IndexRange(side)) {
    const float u = -1.0f + 2.0f * (float(i) / float(last));
    for (const int j : IndexRange(side)) {
      const float v = -1.0f + 2.0f * (float(j) / float(last));
      const float3 base = stamp_plane_point(stamp, float2(u, v));
      base_cos[idx(i, j)] = base;
      disp_vecs[idx(i, j)] = stamp_displacement(stamp, brush, ss, base);
    }
  }

  /* Baseline = mean displacement around the grid border.
   *
   * VDM textures pad their unused area with a flat "neutral" value, but that value is texture
   * dependent (mid-grey, black, ...) and the Sample Bias shifts it further, so it cannot be
   * assumed to be 50 % grey. Measure it directly from the outermost ring of the brush footprint,
   * which by construction lies in that flat padding. Subtracting this measured baseline from every
   * displacement makes both the active-region detection and the generated surface independent of
   * the texture's neutral encoding and of the Sample Bias: padding collapses onto the brush plane
   * (so it is cropped away and never forms a raised platform) while real content keeps its
   * relative shape and stays on the front side of the stroke. */
  float3 baseline(0.0f);
  int border_num = 0;
  for (const int i : IndexRange(side)) {
    for (const int j : IndexRange(side)) {
      if (i == 0 || i == last || j == 0 || j == last) {
        baseline += disp_vecs[idx(i, j)];
        border_num++;
      }
    }
  }
  baseline /= float(border_num);

  /* Re-express every displacement relative to the baseline, then gather the active-region metric
   * and the deepest recess (used to sink the bottom cap below concave geometry). */
  float min_depth = 0.0f;
  for (const int n : IndexRange(grid_size)) {
    disp_vecs[n] -= baseline;
    active_lens[n] = math::length(disp_vecs[n]);
    min_depth = math::min(min_depth, math::dot(disp_vecs[n], brush_normal));
  }

  /* Sink the bottom cap if the VDM has a recess below the brush plane. */
  const float3 bottom_offset = (min_depth < 0.0f) ? (brush_normal * min_depth) : float3(0.0f);

  /* === Pass 2: determine which quads carry meaningful displacement. ===
   * Threshold = 1 % of the grid cell size — separates real content from 8-bit quantisation
   * noise and floating-point rounding in truly-flat border areas. */
  const float cell_size = (2.0f * stamp.radius) / float(last);
  const float threshold = cell_size * 0.01f;
  const int quad_count = last * last;
  Array<bool> quad_active(quad_count, false);

  for (const int i : IndexRange(last)) {
    for (const int j : IndexRange(last)) {
      quad_active[qidx(i, j)] = active_lens[idx(i, j)] > threshold ||
                                 active_lens[idx(i + 1, j)] > threshold ||
                                 active_lens[idx(i + 1, j + 1)] > threshold ||
                                 active_lens[idx(i, j + 1)] > threshold;
    }
  }

  /* === Pass 3: allocate BMVerts only for vertices that belong to at least one active quad. === */
  Array<bool> vert_needed(grid_size, false);
  for (const int i : IndexRange(last)) {
    for (const int j : IndexRange(last)) {
      if (!quad_active[qidx(i, j)]) {
        continue;
      }
      vert_needed[idx(i, j)] = true;
      vert_needed[idx(i + 1, j)] = true;
      vert_needed[idx(i + 1, j + 1)] = true;
      vert_needed[idx(i, j + 1)] = true;
    }
  }

  Array<BMVert *> top_verts(grid_size, nullptr);
  Array<BMVert *> bottom_verts(grid_size, nullptr);
  for (const int n : IndexRange(grid_size)) {
    if (!vert_needed[n]) {
      continue;
    }
    top_verts[n] = BM_vert_create(bm, base_cos[n] + disp_vecs[n], nullptr, BM_CREATE_NOP);
    bottom_verts[n] = BM_vert_create(
        bm, base_cos[n] + bottom_offset, nullptr, BM_CREATE_NOP);
  }

  const auto add_quad = [&](BMVert *v0, BMVert *v1, BMVert *v2, BMVert *v3) {
    BMFace *f = BM_face_create_quad_tri(bm, v0, v1, v2, v3, nullptr, BM_CREATE_NOP);
    if (f) {
      BM_elem_flag_enable(f, BM_ELEM_TAG);
    }
  };

  /* === Pass 4: build faces. ===
   * Top and bottom cap for every active quad. Skirt on every edge that borders an inactive quad
   * or the grid boundary — this guarantees a watertight volume regardless of the active region's
   * shape (convex, concave, non-rectangular). Winding is corrected later via recalc_face_normals. */
  for (const int i : IndexRange(last)) {
    for (const int j : IndexRange(last)) {
      if (!quad_active[qidx(i, j)]) {
        continue;
      }

      add_quad(top_verts[idx(i, j)],
               top_verts[idx(i + 1, j)],
               top_verts[idx(i + 1, j + 1)],
               top_verts[idx(i, j + 1)]);
      add_quad(bottom_verts[idx(i, j)],
               bottom_verts[idx(i, j + 1)],
               bottom_verts[idx(i + 1, j + 1)],
               bottom_verts[idx(i + 1, j)]);

      /* Skirt: emit a wall on each edge that faces outside the active region. */
      if (j == 0 || !quad_active[qidx(i, j - 1)]) {
        add_quad(top_verts[idx(i, j)],
                 top_verts[idx(i + 1, j)],
                 bottom_verts[idx(i + 1, j)],
                 bottom_verts[idx(i, j)]);
      }
      if (j == last - 1 || !quad_active[qidx(i, j + 1)]) {
        add_quad(top_verts[idx(i, j + 1)],
                 bottom_verts[idx(i, j + 1)],
                 bottom_verts[idx(i + 1, j + 1)],
                 top_verts[idx(i + 1, j + 1)]);
      }
      if (i == 0 || !quad_active[qidx(i - 1, j)]) {
        add_quad(top_verts[idx(i, j + 1)],
                 top_verts[idx(i, j)],
                 bottom_verts[idx(i, j)],
                 bottom_verts[idx(i, j + 1)]);
      }
      if (i == last - 1 || !quad_active[qidx(i + 1, j)]) {
        add_quad(top_verts[idx(i + 1, j)],
                 top_verts[idx(i + 1, j + 1)],
                 bottom_verts[idx(i + 1, j + 1)],
                 bottom_verts[idx(i + 1, j)]);
      }
    }
  }
}

/**
 * Build a tool-flags BMesh and append every captured stamp volume to it.
 *
 * \a existing_mesh, when non-null, is loaded first so the stamps are merged into a copy of the
 * active geometry (the "into active mesh" mode).
 */
static BMesh *build_stamps_bmesh(const Span<VDMStampData> stamps,
                                 const Brush &brush,
                                 SculptSession &ss,
                                 const int resolution,
                                 Mesh *existing_mesh)
{
  BMAllocTemplate allocsize = bm_mesh_allocsize_default;
  if (existing_mesh) {
    allocsize = BMALLOC_TEMPLATE_FROM_ME(existing_mesh);
  }
  BMeshCreateParams bm_create_params{};
  bm_create_params.use_toolflags = true;
  BMesh *bm = BM_mesh_create(&allocsize, &bm_create_params);

  if (existing_mesh) {
    BMeshFromMeshParams mesh_to_bm_params{};
    mesh_to_bm_params.calc_face_normal = true;
    mesh_to_bm_params.calc_vert_normal = true;
    BM_mesh_bm_from_me(bm, existing_mesh, &mesh_to_bm_params);
  }

  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  for (const VDMStampData &stamp : stamps) {
    add_stamp_to_bmesh(bm, stamp, brush, ss, resolution);
  }

  /* Orient only the freshly created stamp faces outward, leaving existing geometry untouched.
   * `recalc_face_normals` requires `f->no` to be populated before it runs (it asserts
   * `BM_face_is_normal_valid` on each input face). The faces were just created with
   * `BM_CREATE_NOP`, so their stored normals are still zero — compute them first. */
  BM_mesh_normals_update(bm);
  BMO_op_callf(bm, BMO_FLAG_DEFAULTS, "recalc_face_normals faces=%hf", BM_ELEM_TAG);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  BM_mesh_normals_update(bm);
  return bm;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Operator
 * \{ */

static wmOperatorStatus vdm_insert_mesh_exec(bContext *C, wmOperator *op)
{
  Main *bmain = CTX_data_main(C);
  Object *ob = CTX_data_active_object(C);
  View3D *v3d = CTX_wm_view3d(C);
  Scene *scene = CTX_data_scene(C);
  SculptSession &ss = *ob->runtime->sculpt_session;

  /* Dyntopo keeps geometry inside a BMesh; the regular mesh path below does not apply. */
  if (ss.bm) {
    ss.vdm_stamps.clear();
    return OPERATOR_CANCELLED;
  }
  if (ss.vdm_stamps.is_empty()) {
    return OPERATOR_CANCELLED;
  }

  const Brush &brush = *BKE_paint_brush_for_read(&scene->toolsettings->sculpt->paint);
  const bool into_active = (brush.flag2 & BRUSH_VDM_INSERT_INTO_ACTIVE) != 0;

  const Vector<VDMStampData> stamps = ss.vdm_stamps;
  ss.vdm_stamps.clear();

  Mesh &active_mesh = *id_cast<Mesh *>(ob->data);
  /* Derive resolution from the mesh density at the first stamp's footprint so the generated grid
   * matches the polygon size of the target mesh rather than using an arbitrary fixed value. */
  const int resolution = resolution_from_mesh_density(
      active_mesh, stamps[0].location, stamps[0].radius);
  BMeshToMeshParams bm_to_mesh_params{};
  bm_to_mesh_params.calc_object_remap = false;

  if (into_active) {
    /* Merge the stamps into a copy of the active mesh, then write it back. The geometry change
     * happens while a sculpt session is live, so wrap it in a geometry undo step and drop the
     * cached PBVH afterwards (see #SCULPT_OT_paint_mask_slice for the same pattern). */
    undo::geometry_begin(*scene, *ob, op);

    Mesh *work_mesh = id_cast<Mesh *>(BKE_id_copy(bmain, &active_mesh.id));
    BMesh *bm = build_stamps_bmesh(stamps, brush, ss, resolution, work_mesh);
    BKE_id_free(bmain, work_mesh);
    Mesh *result = BKE_mesh_from_bmesh_nomain(bm, &bm_to_mesh_params, &active_mesh);
    BM_mesh_free(bm);

    BKE_mesh_nomain_to_mesh(result, &active_mesh, ob);

    undo::geometry_end(*ob);
    BKE_sculptsession_free_pbvh(*ob);

    BKE_mesh_batch_cache_dirty_tag(&active_mesh, BKE_MESH_BATCH_DIRTY_ALL);
    DEG_id_tag_update(&ob->id, ID_RECALC_GEOMETRY);
    WM_event_add_notifier(C, NC_GEOM | ND_DATA, &active_mesh);
    return OPERATOR_FINISHED;
  }

  /* Separate object: build the stamps in their own mesh. */
  BMesh *bm = build_stamps_bmesh(stamps, brush, ss, resolution, nullptr);

  /* `add_type` only applies the active object's location and rotation, so bake its scale into the
   * geometry to keep the stamp aligned with where the stroke happened. */
  BMVert *v;
  BMIter iter;
  BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
    mul_v3_v3(v->co, ob->scale);
  }

  Mesh *new_mesh = BKE_mesh_from_bmesh_nomain(bm, &bm_to_mesh_params, &active_mesh);
  BM_mesh_free(bm);

  if (!new_mesh || new_mesh->verts_num == 0) {
    if (new_mesh) {
      BKE_id_free(nullptr, new_mesh);
    }
    return OPERATOR_CANCELLED;
  }

  ushort local_view_bits = 0;
  if (v3d && v3d->localvd) {
    local_view_bits = v3d->local_view_uid;
  }

  Object *new_ob = ed::object::add_type(
      C, OB_MESH, nullptr, ob->loc, ob->rot, false, local_view_bits);
  BKE_mesh_nomain_to_mesh(new_mesh, id_cast<Mesh *>(new_ob->data), new_ob);

  WM_event_add_notifier(C, NC_OBJECT | ND_MODIFIER, new_ob);
  BKE_mesh_batch_cache_dirty_tag(id_cast<Mesh *>(new_ob->data), BKE_MESH_BATCH_DIRTY_ALL);
  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&new_ob->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, new_ob->data);

  return OPERATOR_FINISHED;
}

void SCULPT_OT_vdm_insert_mesh(wmOperatorType *ot)
{
  ot->name = "VDM Insert Mesh";
  ot->description = "Create a watertight mesh from VDM brush stamps";
  ot->idname = "SCULPT_OT_vdm_insert_mesh";

  ot->poll = vdm_insert_mesh_poll;
  ot->exec = vdm_insert_mesh_exec;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_int(ot->srna,
              "resolution",
              32,
              2,
              512,
              "Resolution",
              "Grid resolution for the generated mesh",
              2,
              256);
}

/** \} */

}  // namespace blender::ed::sculpt_paint
