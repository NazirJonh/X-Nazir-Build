/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "projection_target.hh"

#include <cmath>

#include "BLI_math_matrix.hh"
#include "BLI_math_vector.h"
#include "BLI_uuid.h"

#include "DNA_material_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "BKE_bvhutils.hh"
#include "BKE_context.hh"
#include "BKE_mesh.hh"
#include "BKE_object.hh"
#include "BKE_paint_layers.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"

#include "ED_view3d.hh"

#include "../uv_mapping/uv_mapping_target.hh"

namespace blender::ed::sculpt_paint::projection {

MaterialPaintLayer *PaintLayerRowProjectionTarget::row() const
{
  if (material_ == nullptr || BLI_uuid_is_nil(marker_)) {
    return nullptr;
  }
  MaterialPaintLayer *layer = BKE_paint_layers_find(*material_, marker_);
  if (layer == nullptr || !BKE_paint_layers_projection_applies(*material_, *layer)) {
    return nullptr;
  }
  return layer;
}

bool PaintLayerRowProjectionTarget::resolve(const bContext &C)
{
  material_ = nullptr;
  object_ = nullptr;
  marker_ = {};

  Object *ob = CTX_data_active_object(&C);
  /* The frame is edited in Sculpt Mode only: the handles must not stay behind in Object Mode or
   * show up in Texture Paint, and the Place, Move and Flip operators share this poll. */
  if (ob == nullptr || (ob->mode & OB_MODE_SCULPT) == 0) {
    return false;
  }
  Material *ma = BKE_paint_layers_active_material_get(ob);
  if (ma == nullptr) {
    return false;
  }
  /* The one shown-row rule the Mapping panel, its Python mirror and the mapping gizmo share. */
  MaterialPaintLayer *layer = uv_mapping::mapping_shown_row_get(C, *ma);
  if (layer == nullptr || !BKE_paint_layers_projection_applies(*ma, *layer)) {
    return false;
  }

  material_ = ma;
  object_ = ob;
  marker_ = layer->marker;
  return true;
}

bool PaintLayerRowProjectionTarget::read(ProjectionFrame &r_frame) const
{
  const MaterialPaintLayer *layer = row();
  if (layer == nullptr || layer->projection == nullptr) {
    return false;
  }
  copy_v3_v3(r_frame.center, layer->projection->center);
  BKE_paint_layers_projection_size_effective(*layer, r_frame.size);
  BKE_paint_layers_projection_normal_get(*layer, r_frame.normal);
  r_frame.roll = layer->projection->roll;
  return true;
}

bool PaintLayerRowProjectionTarget::write(const ProjectionFrame &frame)
{
  MaterialPaintLayer *layer = row();
  if (layer == nullptr) {
    return false;
  }
  ProjectionFrame written = frame;
  if (BKE_paint_layers_projection_size_from_controller_get(*layer) &&
      layer->projection != nullptr)
  {
    /* Read() reports one while the option is on; writing that one back would erase the extents
     * the user kept for turning the option off. Keep the stored size, which is ignored anyway. */
    copy_v3_v3(written.size, layer->projection->size);
  }
  return BKE_paint_layers_projection_set_values(
      *material_, layer, written.center, written.size, written.normal, written.roll);
}

bool PaintLayerRowProjectionTarget::write_roll(const float roll)
{
  if (!std::isfinite(roll)) {
    return false;
  }
  ProjectionFrame frame;
  if (!read(frame)) {
    return false;
  }
  frame.roll = roll;
  return write(frame);
}

Object *PaintLayerRowProjectionTarget::space_object() const
{
  const MaterialPaintLayer *layer = row();
  if (layer != nullptr && layer->projection_object != nullptr) {
    return layer->projection_object;
  }
  return object_;
}

float3 normal_local_to_world(const float4x4 &o2w, const float3 &local_no)
{
  return math::normalize(math::transpose(math::invert(float3x3(o2w))) * local_no);
}

float3 normal_world_to_local(const float4x4 &o2w, const float3 &world_no)
{
  return math::normalize(math::transpose(float3x3(o2w)) * world_no);
}

float4x4 projection_space_to_world(const bContext &C, const Object &space_ob)
{
  if (Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C)) {
    if (const Object *ob_eval = DEG_get_evaluated<Object>(depsgraph, &space_ob)) {
      /* The evaluated matrix is what the bake and the composite read: a conversion against the
       * original would drift as soon as a parent or a constraint moves the object. */
      return ob_eval->object_to_world();
    }
  }
  return space_ob.object_to_world();
}

float4x4 projection_space_from_world(const bContext &C, const Object &space_ob)
{
  /* #math::invert returns a zero matrix on failure; the relative transform then fails its own
   * invertibility check and the conversion refuses before anything is written. */
  return math::invert(projection_space_to_world(C, space_ob));
}

bool projection_frame_raycast(const bContext &C,
                              const ARegion *region,
                              const View3D *v3d,
                              const Object &ob,
                              const float mval[2],
                              float r_world_loc[3],
                              float r_world_no[3])
{
  Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);
  float ray_start[3], ray_dir[3];
  if (!ED_view3d_win_to_ray_clipped(depsgraph, region, v3d, mval, ray_start, ray_dir, true)) {
    return false;
  }
  const Object *ob_eval = DEG_get_evaluated(depsgraph, &ob);
  if (ob_eval == nullptr || ob_eval->type != OB_MESH) {
    return false;
  }
  const Mesh *me_eval = BKE_object_get_evaluated_mesh(ob_eval);
  if (me_eval == nullptr) {
    return false;
  }
  /* Transform ray to object space. */
  const float4x4 &o2w = ob_eval->object_to_world();
  const float4x4 w2o = math::invert(o2w);
  float3 local_start = math::transform_point(w2o, float3(ray_start));
  float3 local_dir = math::normalize(math::transform_direction(w2o, float3(ray_dir)));

  bke::BVHTreeFromMesh tree_data = me_eval->bvh_corner_tris_no_hidden();
  if (tree_data.tree == nullptr) {
    return false;
  }
  BVHTreeRayHit hit{};
  hit.index = -1;
  hit.dist = BVH_RAYCAST_DIST_MAX;
  const float start_f[3] = {local_start.x, local_start.y, local_start.z};
  const float dir_f[3] = {local_dir.x, local_dir.y, local_dir.z};
  BLI_bvhtree_ray_cast(
      tree_data.tree, start_f, dir_f, 0.0f, &hit, tree_data.raycast_callback, &tree_data);
  if (hit.index < 0) {
    return false;
  }
  /* hit.co and hit.no are in object space; transform to world. */
  const float3 world_loc = math::transform_point(o2w, float3(hit.co));
  const float3 world_no = normal_local_to_world(o2w, float3(hit.no));
  copy_v3_v3(r_world_loc, world_loc);
  copy_v3_v3(r_world_no, world_no);
  return true;
}

void projection_frame_point_from_world(const bContext &C,
                                       const Object *space_ob,
                                       const float world_loc[3],
                                       const float world_no[3],
                                       float r_center[3],
                                       float r_normal[3])
{
  if (space_ob != nullptr) {
    Depsgraph *depsgraph = CTX_data_ensure_evaluated_depsgraph(&C);
    const Object *sp_eval = DEG_get_evaluated(depsgraph, space_ob);
    if (sp_eval != nullptr) {
      const float4x4 w2o = sp_eval->world_to_object();
      const float3 lp = math::transform_point(w2o, float3(world_loc));
      copy_v3_v3(r_center, lp);
      /* Normal: world→frame uses transpose(M3(o2w)). */
      const float3 ln = normal_world_to_local(sp_eval->object_to_world(), float3(world_no));
      copy_v3_v3(r_normal, ln);
      return;
    }
  }
  copy_v3_v3(r_center, world_loc);
  copy_v3_v3(r_normal, world_no);
}

}  // namespace blender::ed::sculpt_paint::projection
