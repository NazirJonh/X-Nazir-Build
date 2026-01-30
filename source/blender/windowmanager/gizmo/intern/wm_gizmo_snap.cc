/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * \name Gizmo Snapping Support
 * \brief Snap support for gizmo interactions (increment and grid snapping).
 */

#include "wm_gizmo_snap.hh"

#include "BKE_context.hh"
#include "WM_gizmo_types.hh"

#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_view3d_types.h"

#include "BLI_math_base.h"
#include "BLI_math_vector.h"

#include "ED_transform_snap_object_context.hh"
#include "ED_view3d.hh"

#include "DEG_depsgraph.hh"

namespace blender::wm::gizmo {

/* Snap distance threshold in pixels. */
#define GIZMO_SNAP_MIN_DISTANCE 30

/* -------------------------------------------------------------------- */
/** \name Snap Increment Functions
 * \{ */

bool gizmo_snap_increment_apply(const GizmoSnapParams *snap_params,
                                const bool use_precision,
                                float *value)
{
  printf("[GIZMO_SNAP_INC] Called with value=%.4f, use_precision=%d\n", *value, use_precision);

  if (!snap_params || !snap_params->use_snap) {
    printf("[GIZMO_SNAP_INC] Early return: snap_params=%p, use_snap=%d\n",
           snap_params, snap_params ? snap_params->use_snap : -1);
    return false;
  }

  if (!(snap_params->snap_mode & SCE_SNAP_TO_INCREMENT)) {
    printf("[GIZMO_SNAP_INC] Early return: snap_mode=0x%x, INCREMENT not set\n",
           snap_params->snap_mode);
    return false;
  }

  /* Get increment value. */
  float increment = snap_params->increment;
  if (increment == 0.0f) {
    /* Use default increment if not specified. */
    /* TODO: Get from scene settings based on transform_mode. */
    increment = 1.0f;
  }

  /* Apply precision modifier. */
  if (use_precision) {
    increment *= snap_params->increment_precision;
  }

  printf("[GIZMO_SNAP_INC] Using increment=%.4f\n", increment);

  /* Snap value to nearest increment. */
  if (increment != 0.0f) {
    float old_value = *value;
    *value = increment * roundf(*value / increment);
    printf("[GIZMO_SNAP_INC] Snapped: %.4f -> %.4f (increment=%.4f)\n",
           old_value, *value, increment);
    return true;
  }

  printf("[GIZMO_SNAP_INC] Early return: increment is zero\n");
  return false;
}

bool gizmo_snap_increment_apply_vec(const GizmoSnapParams *snap_params,
                                    const bool use_precision,
                                    float *value,
                                    const int num_components)
{
  if (!snap_params || !snap_params->use_snap) {
    return false;
  }

  if (!(snap_params->snap_mode & SCE_SNAP_TO_INCREMENT)) {
    return false;
  }

  /* Get increment value. */
  float increment = snap_params->increment;
  if (increment == 0.0f) {
    /* Use default increment if not specified. */
    /* TODO: Get from scene settings based on transform_mode. */
    increment = 1.0f;
  }

  /* Apply precision modifier. */
  if (use_precision) {
    increment *= snap_params->increment_precision;
  }

  /* Snap each component to nearest increment. */
  if (increment != 0.0f) {
    for (int i = 0; i < num_components; i++) {
      value[i] = increment * roundf(value[i] / increment);
    }
    return true;
  }

  return false;
}

float gizmo_snap_increment_get(const bContext *C, const eSnapTransformMode transform_mode)
{
  const Scene *scene = CTX_data_scene(C);
  if (!scene) {
    return 0.0f;
  }

  const ToolSettings *ts = scene->toolsettings;
  if (!ts) {
    return 0.0f;
  }

  /* Check if snap is enabled. */
  if (!(ts->snap_flag & SCE_SNAP)) {
    return 0.0f;
  }

  /* Check if increment snap mode is active. */
  if (!(ts->snap_mode & SCE_SNAP_TO_INCREMENT)) {
    return 0.0f;
  }

  /* Check if this transform mode is enabled for snapping. */
  if (!(ts->snap_transform_mode_flag & transform_mode)) {
    return 0.0f;
  }

  /* Return appropriate increment based on transform mode. */
  switch (transform_mode) {
    case SCE_SNAP_TRANSFORM_MODE_TRANSLATE: {
      /* Get grid spacing from scene. */
      ARegion *region = CTX_wm_region(C);
      if (region && region->regiontype == RGN_TYPE_WINDOW) {
        ScrArea *area = CTX_wm_area(C);
        if (area) {
          View3D *v3d = static_cast<View3D *>(area->spacedata.first);
          if (v3d) {
            return ED_view3d_grid_view_scale(scene, v3d, region, nullptr);
          }
        }
      }
      /* Fallback to default. */
      return 1.0f;
    }
    case SCE_SNAP_TRANSFORM_MODE_ROTATE:
      /* Return angle increment in radians. */
      return DEG2RADF(5.0f); /* Default 5 degrees */
    case SCE_SNAP_TRANSFORM_MODE_SCALE:
      return 0.1f; /* Default 0.1 for scale */
    default:
      return 0.0f;
  }
}

bool gizmo_snap_is_enabled(const bContext *C)
{
  const Scene *scene = CTX_data_scene(C);
  if (!scene) {
    return false;
  }

  const ToolSettings *ts = scene->toolsettings;
  if (!ts) {
    return false;
  }

  return (ts->snap_flag & SCE_SNAP) != 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Context Management
 * \{ */

GizmoSnapContext::GizmoSnapContext(const bContext *C) : C_(C)
{
  /* Snap context will be created on demand. */
}

GizmoSnapContext::~GizmoSnapContext()
{
  /* TODO: Destroy snap context when implemented.
   * Currently snap_context_ is nullptr, so nothing to destroy.
   */
  if (snap_context_) {
    /* ed::transform::snap_object_context_destroy(
     *     static_cast<ed::transform::SnapObjectContext *>(snap_context_)); */
  }
}

void GizmoSnapContext::ensure_snap_context()
{
  if (snap_context_) {
    return;
  }
  
  snap_context_ = ed::transform::snap_object_context_create();
}

void GizmoSnapContext::get_snap_params(const GizmoSnapParams *gizmo_snap_params,
                                       void *r_snap_params)
{
  (void)gizmo_snap_params;
  auto *snap_params = static_cast<ed::transform::SnapObjectParams *>(r_snap_params);
  
  snap_params->snap_target_select = SCE_SNAP_TARGET_ALL;
  snap_params->edit_mode_type = ed::transform::SNAP_GEOM_FINAL;
  snap_params->occlusion_test = ed::transform::SNAP_OCCLUSION_NEVER;
  snap_params->grid_size = 0.0f;  /* Automatic */
  snap_params->face_nearest_steps = 1;
  snap_params->use_backface_culling = false;
  snap_params->keep_on_same_target = false;
  snap_params->ignore_editmode_filtering = false;
}

eSnapMode GizmoSnapContext::snap_to_geometry(const GizmoSnapParams *snap_params,
                                             const float mval[2],
                                             float r_location[3],
                                             float r_normal[3])
{
  printf("[GIZMO_SNAP_GEOM] Called with mval=[%.0f, %.0f]\n", mval[0], mval[1]);

  if (!snap_params || !snap_params->use_snap) {
    printf("[GIZMO_SNAP_GEOM] Early return: snap_params=%p, use_snap=%d\n",
           snap_params, snap_params ? snap_params->use_snap : -1);
    last_snap_valid_ = false;
    return SCE_SNAP_TO_NONE;
  }

  /* Check if geometry snap modes are enabled. */
  const eSnapMode geom_snap_modes = eSnapMode(SCE_SNAP_TO_VERTEX | SCE_SNAP_TO_EDGE |
                                               SCE_SNAP_TO_FACE);
  if (!(snap_params->snap_mode & geom_snap_modes)) {
    printf("[GIZMO_SNAP_GEOM] Early return: snap_mode=0x%x, no geom modes set\n",
           snap_params->snap_mode);
    last_snap_valid_ = false;
    return SCE_SNAP_TO_NONE;
  }

  printf("[GIZMO_SNAP_GEOM] Geometry snap modes enabled\n");

  ensure_snap_context();

  Depsgraph *depsgraph = CTX_data_depsgraph_pointer(C_);
  ARegion *region = CTX_wm_region(C_);
  View3D *v3d = CTX_wm_view3d(C_);

  if (!depsgraph || !region || !v3d) {
    printf("[GIZMO_SNAP_GEOM] Early return: depsgraph=%p, region=%p, v3d=%p\n",
           depsgraph, region, v3d);
    last_snap_valid_ = false;
    return SCE_SNAP_TO_NONE;
  }
  
  ed::transform::SnapObjectParams snap_obj_params{};
  get_snap_params(snap_params, &snap_obj_params);
  
  float dist_px = GIZMO_SNAP_MIN_DISTANCE;
  
  eSnapMode snap_result = ed::transform::snap_object_project_view3d(
      static_cast<ed::transform::SnapObjectContext *>(snap_context_),
      depsgraph,
      region,
      v3d,
      eSnapMode(snap_params->snap_mode & geom_snap_modes),
      &snap_obj_params,
      nullptr,  /* init_co */
      mval,
      nullptr,  /* prev_co */
      &dist_px,
      r_location,
      r_normal);
  
  if (snap_result != SCE_SNAP_TO_NONE) {
    copy_v3_v3(last_snap_location_, r_location);
    copy_v3_v3(last_snap_normal_, r_normal);
    last_snap_valid_ = true;
    return snap_result;
  }
  
  last_snap_valid_ = false;
  return SCE_SNAP_TO_NONE;
}

bool GizmoSnapContext::project_snap_to_axis(const float snap_location[3],
                                            const float axis_origin[3],
                                            const float axis_direction[3],
                                            float *r_offset)
{
  /* Calculate vector from axis origin to snap location. */
  float to_snap[3];
  sub_v3_v3v3(to_snap, snap_location, axis_origin);
  
  /* Project onto axis direction (dot product). */
  *r_offset = dot_v3v3(to_snap, axis_direction);
  
  return true;
}

void GizmoSnapContext::get_last_snap_location(float r_location[3]) const
{
  copy_v3_v3(r_location, last_snap_location_);
}

void GizmoSnapContext::get_last_snap_normal(float r_normal[3]) const
{
  copy_v3_v3(r_normal, last_snap_normal_);
}

/** \} */

}  // namespace blender::wm::gizmo

/* -------------------------------------------------------------------- */
/** \name Public API Functions
 * \{ */

namespace blender {

bool WM_gizmo_snap_increment_apply(void *snap_params, bool use_precision, float *value)
{
  return wm::gizmo::gizmo_snap_increment_apply(
      static_cast<const wm::gizmo::GizmoSnapParams *>(snap_params), use_precision, value);
}

bool WM_gizmo_snap_is_enabled(const wmGizmo *gz)
{
  if (!gz || !gz->snap_params) {
    return false;
  }
  const wm::gizmo::GizmoSnapParams *snap_params =
      static_cast<const wm::gizmo::GizmoSnapParams *>(gz->snap_params);
  return snap_params->use_snap;
}

bool WM_gizmo_snap_use_snap_get(void *snap_params)
{
  if (!snap_params) {
    return false;
  }
  const wm::gizmo::GizmoSnapParams *params =
      static_cast<const wm::gizmo::GizmoSnapParams *>(snap_params);
  return params->use_snap;
}

uint32_t WM_gizmo_snap_mode_get(void *snap_params)
{
  if (!snap_params) {
    return 0;
  }
  const wm::gizmo::GizmoSnapParams *params =
      static_cast<const wm::gizmo::GizmoSnapParams *>(snap_params);
  return params->snap_mode;
}

void *WM_gizmo_snap_params_get(wmGizmo *gz)
{
  if (!gz) {
    return nullptr;
  }
  return gz->snap_params;
}

/* -------------------------------------------------------------------- */
/** \name Geometry Snapping (Vertex/Edge/Face)
 * \{ */

void *WM_gizmo_snap_context_create(const bContext *C)
{
  if (!C) {
    return nullptr;
  }
  auto *snap_context = new wm::gizmo::GizmoSnapContext(C);
  return static_cast<void *>(snap_context);
}

void WM_gizmo_snap_context_destroy(void *snap_context)
{
  if (!snap_context) {
    return;
  }
  auto *ctx = static_cast<wm::gizmo::GizmoSnapContext *>(snap_context);
  delete ctx;
}

int WM_gizmo_snap_to_geometry(void *snap_context,
                                void *snap_params,
                                const float mval[2],
                                float r_location[3],
                                float r_normal[3])
{
  if (!snap_context || !snap_params) {
    return SCE_SNAP_TO_NONE;
  }
  
  auto *ctx = static_cast<wm::gizmo::GizmoSnapContext *>(snap_context);
  auto *params = static_cast<const wm::gizmo::GizmoSnapParams *>(snap_params);
  
  float normal[3] = {0.0f, 0.0f, 0.0f};
  if (!r_normal) {
    r_normal = normal;
  }
  
  return ctx->snap_to_geometry(params, mval, r_location, r_normal);
}

bool WM_gizmo_snap_project_to_axis(const float snap_location[3],
                                    const float axis_origin[3],
                                    const float axis_direction[3],
                                    float *r_offset)
{
  if (!snap_location || !axis_origin || !axis_direction || !r_offset) {
    return false;
  }
  return wm::gizmo::GizmoSnapContext::project_snap_to_axis(
      snap_location, axis_origin, axis_direction, r_offset);
}

void WM_gizmo_snap_get_last_location(void *snap_context, float r_location[3])
{
  if (!snap_context || !r_location) {
    return;
  }
  auto *ctx = static_cast<wm::gizmo::GizmoSnapContext *>(snap_context);
  ctx->get_last_snap_location(r_location);
}

void WM_gizmo_snap_get_last_normal(void *snap_context, float r_normal[3])
{
  if (!snap_context || !r_normal) {
    return;
  }
  auto *ctx = static_cast<wm::gizmo::GizmoSnapContext *>(snap_context);
  ctx->get_last_snap_normal(r_normal);
}

bool WM_gizmo_snap_is_last_valid(void *snap_context)
{
  if (!snap_context) {
    return false;
  }
  auto *ctx = static_cast<wm::gizmo::GizmoSnapContext *>(snap_context);
  return ctx->is_last_snap_valid();
}

/** \} */

}  // namespace blender
