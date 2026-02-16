/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * \name Gizmo Snapping Support
 * \brief Snap support for gizmo interactions (increment and grid snapping).
 */

#pragma once

#include "DNA_scene_types.h"

namespace blender {

struct bContext;
struct wmGizmo;

namespace wm::gizmo {

/* -------------------------------------------------------------------- */
/** \name Snap Parameters
 * \{ */

/**
 * Snap configuration for gizmo interaction.
 * Stores parameters for increment and grid snapping.
 */
struct GizmoSnapParams {
  /** Enable snapping for this gizmo. */
  bool use_snap = false;

  /** Snap mode (increment, grid, etc.). */
  eSnapMode snap_mode = SCE_SNAP_TO_NONE;

  /** Transform mode for snap filtering (translate/rotate/scale). */
  eSnapTransformMode transform_mode = SCE_SNAP_TRANSFORM_MODE_TRANSLATE;

  /** Constraint axis for 1D snapping (for linear gizmo).
   * When use_constraint is true, snapping is constrained along this axis. */
  float constraint_axis[3] = {0.0f, 0.0f, 0.0f};

  /** Use constraint axis for 1D snapping. */
  bool use_constraint = false;

  /** Increment value for snap (0 = use scene default). */
  float increment = 0.0f;

  /** Precision multiplier for snap (0 = use scene default). */
  float increment_precision = 0.1f;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Functions
 * \{ */

/**
 * Apply increment snapping to a value.
 * 
 * \param snap_params: Snap parameters from gizmo.
 * \param use_precision: Apply precision modifier (Shift key).
 * \param value: Value to snap (in/out parameter).
 * \return: True if snapping was applied.
 */
bool gizmo_snap_increment_apply(const GizmoSnapParams *snap_params,
                                bool use_precision,
                                float *value);

/**
 * Apply increment snapping to a 3D vector.
 * 
 * \param snap_params: Snap parameters from gizmo.
 * \param use_precision: Apply precision modifier (Shift key).
 * \param value: Vector to snap (in/out parameter).
 * \param num_components: Number of components to snap (1-3).
 * \return: True if snapping was applied.
 */
bool gizmo_snap_increment_apply_vec(const GizmoSnapParams *snap_params,
                                    bool use_precision,
                                    float *value,
                                    int num_components);

/**
 * Get the increment value for snapping from scene settings.
 * 
 * \param C: Context for accessing scene settings.
 * \param transform_mode: Transform mode (translate/rotate/scale).
 * \return: Increment value, or 0.0f if snap is not active.
 */
float gizmo_snap_increment_get(const bContext *C, eSnapTransformMode transform_mode);

/**
 * Check if snap is enabled in scene settings.
 * 
 * \param C: Context for accessing scene settings.
 * \return: True if snap is enabled.
 */
bool gizmo_snap_is_enabled(const bContext *C);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Snap Context Management
 * \{ */

/**
 * Gizmo snap context manager.
 * Manages lifecycle of SnapObjectContext for gizmo snapping operations.
 */
class GizmoSnapContext {
 private:
  /** Snap object context (created on demand). */
  void *snap_context_ = nullptr;
  
  /** Context for accessing scene data. */
  const bContext *C_ = nullptr;
  
  /** Last snapped location in world space. */
  float last_snap_location_[3] = {0.0f, 0.0f, 0.0f};
  
  /** Last snapped normal in world space. */
  float last_snap_normal_[3] = {0.0f, 0.0f, 1.0f};
  
  /** Was last snap successful? */
  bool last_snap_valid_ = false;

 public:
  GizmoSnapContext(const bContext *C);
  ~GizmoSnapContext();
  
  /**
   * Perform snapping for a gizmo.
   * 
   * \param snap_params: Snap parameters from gizmo.
   * \param mval: Current mouse position in region coordinates.
   * \param r_location: Snapped world-space location (output).
   * \param r_normal: Snapped world-space normal (output, optional).
   * \return: Snap mode that was used, or SCE_SNAP_TO_NONE if no snap.
   */
  eSnapMode snap_to_geometry(const GizmoSnapParams *snap_params,
                             const float mval[2],
                             float r_location[3],
                             float *r_normal);
  
  /**
   * Project a snapped point onto a constraint axis.
   * Used for Linear Gizmo to constrain snap along axis.
   * 
   * \param snap_location: Snapped location in world space.
   * \param axis_origin: Origin point of constraint axis.
   * \param axis_direction: Direction of constraint axis (normalized).
   * \param r_offset: Offset along axis (output).
   * \return: True if projection was successful.
   */
  static bool project_snap_to_axis(const float snap_location[3],
                                   const float axis_origin[3],
                                   const float axis_direction[3],
                                   float *r_offset);
  
  /** Get last snapped location. */
  void get_last_snap_location(float r_location[3]) const;
  
  /** Get last snapped normal. */
  void get_last_snap_normal(float r_normal[3]) const;
  
  /** Was last snap valid? */
  bool is_last_snap_valid() const { return last_snap_valid_; }

 private:
  /** Ensure snap context is created. */
  void ensure_snap_context();
  
  /** Get snap parameters for SnapObjectContext. */
  void get_snap_params(const GizmoSnapParams *gizmo_snap_params,
                      void *r_snap_params);
};

/** \} */

}  // namespace wm::gizmo
}  // namespace blender
