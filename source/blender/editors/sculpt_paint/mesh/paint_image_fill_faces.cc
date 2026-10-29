/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Visibility and facing helpers shared by the Texture Paint geometry fill, its box gesture and its
 * preview overlay. Kept apart from `paint_image_2d.cc`, which is already oversized.
 */

#include "MEM_guardedalloc.h"

#include "BLI_bitmap.h"
#include "BLI_map.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_rect.h"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_layer_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"

#include "BKE_context.hh"
#include "BKE_layer.hh"

#include "bmesh.hh"

#include "DRW_select_buffer.hh"

#include "ED_view3d.hh"

#include "../paint_intern.hh"

namespace blender {

ImagePaintGeometryFillVisibleFaces::~ImagePaintGeometryFillVisibleFaces()
{
  MEM_SAFE_DELETE(bitmap);
}

bool ImagePaintGeometryFillVisibleFaces::contains(const Object *ob, const int face_index) const
{
  if (bitmap == nullptr) {
    return true;
  }
  /* An object the shared context was not built for (should not happen for any object the
   * caller actually iterates) is treated as unfiltered rather than silently hidden. */
  const uint *offset = face_offset_by_object.lookup_ptr(ob);
  if (offset == nullptr || *offset == 0) {
    return true;
  }
  /* The select buffer stores `range_start + face_index`; the bitmap returned by
   * #DRW_select_buffer_bitmap_from_rect is indexed by `select_id - 1`. Edit Mode's own face
   * box selection does the same `offset - 1` before walking faces. Subtracting the missing 1
   * here keeps the test on the face it actually names instead of the next one. */
  const uint index = *offset - 1 + uint(face_index);
  return index < bitmap_num && BLI_BITMAP_TEST_BOOL(bitmap, index);
}

void paint_image_geometry_fill_visible_faces_build(
    const bContext *C,
    const Span<Object *> objects,
    ImagePaintGeometryFillVisibleFaces &r_visible)
{
  View3D *v3d = CTX_wm_view3d(C);
  ARegion *region = CTX_wm_region(C);
  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C);
  if (v3d == nullptr || region == nullptr || depsgraph == nullptr || XRAY_ENABLED(v3d) ||
      objects.is_empty())
  {
    return;
  }

  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  if (bmain == nullptr || view_layer == nullptr) {
    return;
  }
  BKE_view_layer_synced_ensure(*bmain, scene, view_layer);

  Vector<Base *> bases;
  bases.reserve(objects.size());
  for (Object *ob : objects) {
    if (Base *base = BKE_view_layer_base_find(view_layer, ob)) {
      bases.append(base);
    }
  }
  if (bases.is_empty()) {
    return;
  }

  DRW_select_buffer_context_create(depsgraph, bases, SCE_SELECT_FACE);
  rcti region_rect;
  BLI_rcti_init(&region_rect, 0, region->winx, 0, region->winy);
  r_visible.bitmap = DRW_select_buffer_bitmap_from_rect(
      depsgraph, region, v3d, &region_rect, &r_visible.bitmap_num);
  if (r_visible.bitmap == nullptr) {
    return;
  }

  r_visible.face_offset_by_object.reserve(objects.size());
  for (Object *ob : objects) {
    r_visible.face_offset_by_object.add(
        ob, DRW_select_buffer_context_offset_for_object_elem(depsgraph, ob, SCE_SELECT_FACE));
  }
}

float3x3 paint_image_geometry_fill_normal_matrix(const Object &ob)
{
  return math::transpose(math::invert(float3x3(ob.object_to_world())));
}

bool paint_image_geometry_fill_face_facing_view(const Object &ob,
                                                const float3x3 &normalmatrix,
                                                BMFace *efa,
                                                const float3 &view_pos_world,
                                                const float3 &view_dir_world,
                                                const bool is_ortho)
{
  float no[3];
  BM_face_calc_normal(efa, no);
  /* Transform normals with the normal matrix (transpose of inverse of upper-left 3×3), which
   * handles non-uniform scaling and flipped objects correctly. */
  const float3 no_world = normalmatrix * float3(no);
  /* A collapsed face transforms to a zero normal: it has no facing direction, so it never counts
   * as facing the viewer, instead of normalizing a zero vector. */
  if (math::length_squared(no_world) == 0.0f) {
    return false;
  }
  const float3 no_world_unit = math::normalize(no_world);
  if (is_ortho) {
    /* \a view_dir_world points from the scene toward the viewer (viewinv[2]). */
    return math::dot(view_dir_world, no_world_unit) > 0.0f;
  }
  float center[3];
  BM_face_calc_center_median(efa, center);
  const float3 center_world = math::transform_point(ob.object_to_world(), float3(center));
  return math::dot(view_pos_world - center_world, no_world_unit) > 0.0f;
}

void paint_image_geometry_fill_view_vectors_world(const RegionView3D &rv3d,
                                                  float3 &r_view_pos_world,
                                                  float3 &r_view_dir_world,
                                                  bool &r_is_ortho)
{
  r_is_ortho = rv3d.is_persp == 0;
  if (r_is_ortho) {
    /* The third row of #viewinv is view-space +Z, which points from the scene toward the viewer. */
    r_view_dir_world = float3(rv3d.viewinv[2][0], rv3d.viewinv[2][1], rv3d.viewinv[2][2]);
    r_view_pos_world = float3(0.0f);
  }
  else {
    r_view_pos_world = float3(rv3d.viewinv[3][0], rv3d.viewinv[3][1], rv3d.viewinv[3][2]);
    r_view_dir_world = float3(0.0f);
  }
}

}  // namespace blender
