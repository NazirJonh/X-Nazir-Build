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
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct Depsgraph;
struct MDisps;
struct Mesh;
struct ModifierData;
struct MultiresModifierData;
struct Object;
struct ReportList;
struct Scene;
struct SculptLayer;
struct SubdivCCG;
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

enum class MultiresReshapeFromCCGMode : int8_t {
  /** The CCG positions are the evaluated composed surface (`base + enabled sculpt layers`). */
  Composed,
  /** The CCG positions are a base-edit source; enabled sculpt layers must not be written back. */
  Base,
};

bool multiresModifier_reshapeFromCCG(int tot_level,
                                     Mesh *coarse_mesh,
                                     SubdivCCG *subdiv_ccg,
                                     MultiresReshapeFromCCGMode mode);

/**
 * Sculpt layer recording: convert the sculpted CCG surface to tangent displacement at the top
 * level and accumulate the difference against the pre-stroke composed surface
 * (`MDisps + sum(enabled layers)`) into \a layer for the \a touched_grids. #CD_MDISPS itself is
 * left unchanged (it only ever stores the base surface, layers are composed at evaluation time).
 *
 * \param touched_grids: indices of the grids modified by the stroke.
 * \param r_undo_delta: per-element deltas that were added to the layer, laid out as
 * `touched_grids.size() * grid_area` values in the order of \a touched_grids.
 */
bool multiresModifier_reshapeFromCCG_into_sculpt_layer(int tot_level,
                                                       Mesh *coarse_mesh,
                                                       SubdivCCG *subdiv_ccg,
                                                       Span<int> touched_grids,
                                                       SculptLayer &layer,
                                                       Vector<float3> &r_undo_delta);

/**
 * Add `layer.data * factor` to the mesh's #CD_MDISPS displacement (both are stored in the same
 * tangent-space grid layout). Used to bake a layer's contribution into the base and to undo such
 * a bake (negative factor). No-op for layers whose data does not match the MDisps layout.
 */
void BKE_multires_sculpt_layer_apply_to_mdisps(Mesh &mesh, const SculptLayer &layer, float factor);

/**
 * True when the mesh has any grid-domain sculpt layer with displacement data (regardless of
 * visibility). Used to block destructive multires operations that would invalidate the layers.
 */
bool BKE_multires_mesh_has_grid_sculpt_layers(const Mesh &mesh);

/**
 * An enabled grid-domain sculpt layer selected for composition with (or subtraction from) the base
 * multires displacement: a pointer into its tangent coefficients (MDisps layout at the top level)
 * and its influence.
 */
struct MultiresGridSculptLayer {
  const float3 *data;
  float influence;
};

/**
 * Collect the enabled grid-domain sculpt layers whose data matches the MDisps layout at
 * \a grid_area (the `grid_size^2` of the multires top level). Both the displacement evaluator
 * (which composes them onto the base) and the base flush (which subtracts them back) use this
 * single collector so they always operate on an identical layer set; any asymmetry would leak the
 * difference into the base #CD_MDISPS.
 */
Vector<MultiresGridSculptLayer> BKE_multires_grid_sculpt_layers_collect(const Mesh &mesh,
                                                                        int grid_area);

/**
 * Compute the object-space contribution of \a layer (at influence 1.0) for every element of the
 * given CCG at its current level: the layer's tangent coefficients subsampled to the CCG level
 * and transformed by the base-mesh limit-surface tangent matrices. Used by the interactive
 * influence drag to update positions incrementally (`positions += contribution * delta`).
 *
 * \param r_contrib: must hold `grids_num * ccg_grid_size^2` elements.
 */
bool BKE_multires_sculpt_layer_object_contribution(Mesh &base_mesh,
                                                   SubdivCCG &subdiv_ccg,
                                                   const SculptLayer &layer,
                                                   MutableSpan<float3> r_contrib);

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

/**
 * Tangent matrix construction as it was before the well-conditioned rewrite. Only used by
 * versioning code to decode displacement grids stored by older files.
 */
BLI_INLINE void BKE_multires_construct_tangent_matrix_for_versioning(float3x3 &tangent_matrix,
                                                                     const float3 &dPdu,
                                                                     const float3 &dPdv,
                                                                     int corner);

/* Versioning. */

/**
 * Convert displacement which is stored for simply-subdivided mesh to a Catmull-Clark
 * subdivided mesh.
 */
void multires_do_versions_simple_to_catmull_clark(Object *object, MultiresModifierData *mmd);

/**
 * Re-encode displacement grids from the legacy tangent space (normalized axes) into the
 * well-conditioned tangent space produced by #BKE_multires_construct_tangent_matrix.
 */
void multires_do_versions_tangent_space_conversion(Object *object, MultiresModifierData *mmd);

}  // namespace blender

#include "intern/multires_inline.hh"  // IWYU pragma: export
