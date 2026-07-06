/* SPDX-FileCopyrightText: 2007 by Nicholas Bishop. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 */

#include "BLI_array.hh"
#include "BLI_enum_flags.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct Depsgraph;
struct Main;
struct MDisps;
struct Mesh;
struct ModifierData;
struct MultiresModifierData;
struct Object;
struct ReportList;
struct Scene;
struct SubdivCCG;
struct View3D;
struct ViewLayer;
namespace bke::subdiv {
struct Settings;
struct ToMeshSettings;
}  // namespace bke::subdiv

enum MultiresModifiedFlags {
  /* indicates the grids have been sculpted on, so MDisps
   * have to be updated */
  MULTIRES_COORDS_MODIFIED = 1,
  /* indicates elements have been hidden or unhidden */
  MULTIRES_HIDDEN_MODIFIED = 2,
};

/**
 * Delete mesh mdisps and grid paint masks.
 */
void multires_customdata_delete(Mesh *mesh);

void multires_set_tot_level(Object *ob, MultiresModifierData *mmd, int lvl);

void multires_mark_as_modified(Depsgraph *depsgraph, Object *object, MultiresModifiedFlags flags);

void multires_flush_sculpt_updates(Object *object);
void multires_force_sculpt_rebuild(Object *object);
void multires_force_external_reload(Object *object);

/**
 * Reset the multi-res levels to match the number of mdisps.
 */
void multiresModifier_set_levels_from_disps(MultiresModifierData *mmd, Object *ob);

enum class MultiresLevelType : int8_t {
  Viewport = 0,
  Sculpt = 1,
  Render = 2,
};

/** Read one of `mmd`'s three subdivision level fields, selected by `level_type`. */
int multires_level_get(const MultiresModifierData *mmd, MultiresLevelType level_type);
/** Write one of `mmd`'s three subdivision level fields, clamped to `[0, mmd->totlvl]`. */
void multires_level_set(MultiresModifierData *mmd, MultiresLevelType level_type, int value);

/**
 * Apply `new_value` for `level_type` to every object in `candidates` other than `active_ob`
 * that has a Multires modifier.
 * When `only_matching` is true, an object is updated only if its current value equals
 * `old_value` (objects already in sync with the old reference follow along; diverged objects
 * are left untouched). When false, every candidate with a Multires modifier is force-set.
 * Pure data function: does not tag the depsgraph or post notifiers, callers do that for the
 * returned objects.
 */
Vector<Object *> multires_level_group_sync(Span<Object *> candidates,
                                           const Object *active_ob,
                                           MultiresLevelType level_type,
                                           int old_value,
                                           int new_value,
                                           bool only_matching);

/**
 * Mesh objects that are part of the same multi-object sculpt session as `active_ob` would be
 * (`OB_MODE_SCULPT` within `view_layer`) and have a Multires modifier.
 */
Vector<Object *> multires_group_objects(const Main *bmain,
                                        const Scene *scene,
                                        ViewLayer *view_layer,
                                        const View3D *v3d);

/**
 * Objects in `candidates` (other than `active_ob`) whose current `MultiresModifierData::totlvl`
 * equals `reference_totlvl`. Pure filter, no side effects.
 */
Vector<Object *> multires_totlvl_matching_group(Span<Object *> candidates,
                                                const Object *active_ob,
                                                int reference_totlvl);

enum class MultiresFlags : uint8_t {
  UseLocalMMD = 1,
  UseRenderParams = 2,
  AllocPaintMask = 4,
  IgnoreSimplify = 8,
};
ENUM_OPERATORS(MultiresFlags);

MultiresModifierData *find_multires_modifier_before(Scene *scene, ModifierData *lastmd);
/**
 * used for applying scale on mdisps layer and syncing subdivide levels when joining objects.
 * \param use_first: return first multi-res modifier if all multi-res'es are disabled.
 */
MultiresModifierData *get_multires_modifier(Scene *scene, Object *ob, bool use_first);
int multires_get_level(const Scene *scene,
                       const Object *ob,
                       const MultiresModifierData *mmd,
                       bool render,
                       bool ignore_simplify);

/**
 * Creates mesh with multi-res modifier applied on current object's deform mesh.
 */
Mesh *BKE_multires_create_mesh(Depsgraph *depsgraph, Object *object, MultiresModifierData *mmd);

/**
 * Get coordinates of a deformed base mesh which is an input to the given multi-res modifier.
 * \note The modifiers will be re-evaluated.
 */
Array<float3> BKE_multires_create_deformed_base_mesh_vert_coords(Depsgraph *depsgraph,
                                                                 Object *object,
                                                                 MultiresModifierData *mmd);

/**
 * \param direction: 1 for delete higher, 0 for lower (not implemented yet).
 */
void multiresModifier_del_levels(MultiresModifierData *mmd,
                                 Scene *scene,
                                 Object *object,
                                 int direction);

enum class ApplyBaseMode : int8_t {
  Base,
  ForSubdivision,
};

/**
 * Use when un-subdivide has *partial* success,
 * report issues to the user as warnings since an "error" implies
 * failure which is handled separately.
 */
struct MultiresUnsubdivideInfo {
  /** When over zero report that some data was not preserved. */
  int unsupported_grid_count = 0;
};

void multiresModifier_base_apply(Depsgraph *depsgraph,
                                 Object *object,
                                 MultiresModifierData *mmd,
                                 ApplyBaseMode mode);
int multiresModifier_rebuild_subdiv(Depsgraph *depsgraph,
                                    Object *object,
                                    MultiresModifierData *mmd,
                                    int rebuild_limit,
                                    bool switch_view_to_lower_level,
                                    MultiresUnsubdivideInfo &info);

void multiresModifier_unsubdivide_report_if_needed(const MultiresUnsubdivideInfo &info,
                                                   ReportList *reports);

/**
 * If `ob_src` and `ob_dst` both have multi-res modifiers,
 * synchronize them such that `ob_dst` has the same total number of levels as `ob_src`.
 */
void multiresModifier_sync_levels_ex(Object *ob_dst,
                                     const MultiresModifierData *mmd_src,
                                     MultiresModifierData *mmd_dst);

void multires_stitch_grids(Object *);

void multiresModifier_scale_disp(Depsgraph *depsgraph, Scene *scene, Object *ob);
void multiresModifier_prepare_join(Depsgraph *depsgraph, Scene *scene, Object *ob, Object *to_ob);

int multires_mdisp_corners(const MDisps *s);

/**
 * Update multi-res data after topology changing.
 */
void multires_topology_changed(Mesh *mesh);

/**
 * Makes sure data from an external file is fully read.
 *
 * Since the multi-res data files only contain displacement vectors without knowledge about
 * subdivision level some extra work is needed. Namely make is to all displacement grids have
 * proper level and number of displacement vectors set.
 */
void multires_ensure_external_read(Mesh *mesh, int top_level);
void multiresModifier_ensure_external_read(Mesh *mesh, const MultiresModifierData *mmd);

/**** interpolation stuff ****/
/* Adapted from `sculptmode.c` */

void old_mdisps_bilinear(float out[3], float (*disps)[3], int st, float u, float v);

/* Reshaping, define in multires_reshape.cc */
/**
 * Returns truth on success, false otherwise.
 *
 * This function might fail in cases like source and destination not having
 * matched amount of vertices.
 */
bool multiresModifier_reshapeFromObject(Depsgraph *depsgraph,
                                        MultiresModifierData *mmd,
                                        Object *dst,
                                        Object *src);
bool multiresModifier_reshapeFromDeformModifier(Depsgraph *depsgraph,
                                                Object *ob,
                                                MultiresModifierData *mmd,
                                                ModifierData *deform_md);
bool multiresModifier_reshapeFromCCG(int tot_level, Mesh *coarse_mesh, SubdivCCG *subdiv_ccg);

/* Subdivide multi-res displacement once. */

enum class MultiresSubdivideModeType : int8_t {
  CatmullClark,
  Simple,
  Linear,
};

void multiresModifier_subdivide(Object *object,
                                MultiresModifierData *mmd,
                                MultiresSubdivideModeType mode);
void multires_subdivide_create_tangent_displacement_linear_grids(Object *object,
                                                                 MultiresModifierData *mmd);

/**
 * Subdivide displacement to the given level.
 * If level is lower than the current top level nothing happens.
 */
void multiresModifier_subdivide_to_level(Object *object,
                                         MultiresModifierData *mmd,
                                         int top_level,
                                         MultiresSubdivideModeType mode);

/* Subdivision integration, defined in multires_subdiv.cc */

void BKE_multires_subdiv_settings_init(bke::subdiv::Settings *settings,
                                       const MultiresModifierData *mmd);

/* TODO(sergey): Replace this set of boolean flags with bitmask. */
void BKE_multires_subdiv_mesh_settings_init(bke::subdiv::ToMeshSettings *mesh_settings,
                                            const Scene *scene,
                                            const Object *object,
                                            const MultiresModifierData *mmd,
                                            bool use_render_params,
                                            bool ignore_simplify,
                                            bool ignore_control_edges);

/* General helpers. */

/**
 * For a given partial derivatives of a PTEX face get tangent matrix for displacement.
 *
 * Corner needs to be known to properly "rotate" partial derivatives when the
 * matrix is being constructed for quad. For non-quad the corner is to be set to 0.
 */
BLI_INLINE void BKE_multires_construct_tangent_matrix(float3x3 &tangent_matrix,
                                                      const float3 &dPdu,
                                                      const float3 &dPdv,
                                                      int corner);

/* Versioning. */

/**
 * Convert displacement which is stored for simply-subdivided mesh to a Catmull-Clark
 * subdivided mesh.
 */
void multires_do_versions_simple_to_catmull_clark(Object *object, MultiresModifierData *mmd);

}  // namespace blender

#include "intern/multires_inline.hh"  // IWYU pragma: export
