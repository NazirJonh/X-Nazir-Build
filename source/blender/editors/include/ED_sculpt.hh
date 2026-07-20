/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include <cstddef>

namespace blender {

struct Depsgraph;
struct Main;
struct Mesh;
struct Object;
struct RegionView3D;
struct ReportList;
struct Scene;
struct SculptLayer;
struct UndoType;
struct UndoStep;
struct bContext;
struct wmKeyConfig;
struct wmOperator;

namespace ed::sculpt_paint {

void object_sculpt_mode_enter(Main &bmain,
                              Depsgraph &depsgraph,
                              Scene &scene,
                              Object &ob,
                              bool force_dyntopo,
                              ReportList *reports);
void object_sculpt_mode_enter(bContext *C, Depsgraph &depsgraph, ReportList *reports);
void object_sculpt_mode_exit(Main &bmain, Depsgraph &depsgraph, Scene &scene, Object &ob);
void object_sculpt_mode_exit(bContext *C, Depsgraph &depsgraph);

/* `sculpt.cc` */

/**
 * Checks if the currently active shape key is able to be sculpted on.
 *
 * If the active shape key is either muted or locked, an error message will be reported, unless
 * \a reports is null.
 *
 * \return false if the shape key cannot be modified.
 */
bool shape_key_check(const Object &ob, ReportList *reports);

void operatortypes_sculpt();

void keymap_sculpt(wmKeyConfig *keyconf);

/* `sculpt_transform.cc` */

void update_modal_transform(bContext *C, Object &ob);
void cancel_modal_transform(bContext *C, Object &ob);
void init_transform(bContext *C, Object &ob, const float mval_fl[2], const char *undo_name);
void end_transform(bContext *C, Object &ob);

/* `sculpt_undo.cc` */

namespace undo {

void register_type(UndoType *ut);

/**
 * Pushes an undo step using the operator name. This is necessary for
 * redo panels to work; operators that do not support that may use
 * #geometry_begin_ex instead if so desired.
 */
void geometry_begin(const Scene &scene, Object &ob, const wmOperator *op);
void geometry_begin_ex(const Scene &scene, Object &ob, const char *name);
void geometry_end(Object &ob);

/**
 * Undo for changes happening on a base mesh for multires sculpting.
 * if there is no multi-res sculpt active regular undo is used.
 */
void push_multires_mesh_begin(bContext *C, const char *str);
void push_multires_mesh_end(bContext *C, const char *str);

size_t step_memory_size_get(UndoStep *step);

}  // namespace undo

namespace face_set {

int find_next_available_id(const Mesh &mesh);
int find_next_available_id(Object &object);
void initialize_none_to_id(Mesh *mesh, int new_id);
int active_update_and_get(bContext *C, Object &ob, const float mval_fl[2]);

}  // namespace face_set

/**
 * Fills the entire object's active color attribute layer with the fill color.
 *
 * \return #true if successful.
 */
bool object_active_color_init(Object &ob, const float fill_color[4]);

/**
 * Fully replace the sculpt mesh with a mesh outside of #Main. This implements various checks to
 * avoid pushing full geometry-type undo steps when possible, allowing for better performance.
 *
 * \warning To avoid false negatives when detecting mesh changes, it is critical that the caller
 * adds an owner to the attribute data arrays before modifying the original object's mesh. This
 * allows constant time checks for whether the mesh has changed.
 */
void store_mesh_from_eval(const wmOperator &op,
                          const Scene &scene,
                          const Depsgraph &depsgraph,
                          const RegionView3D *rv3d,
                          Object &object,
                          Mesh *new_mesh);

namespace layers {

/**
 * Fast viewport refresh after a sculpt-layer influence or visibility change that was already
 * applied directly to the mesh vertex positions (see #rna_SculptLayer_apply_mesh_delta).
 *
 * When \a mesh is being sculpted with a vertex-domain #bke::pbvh::Tree (and not through a
 * deform-modifier or shape-key session), this invalidates the PBVH nodes and requests a lightweight
 * redraw instead of a full geometry re-evaluation, keeping the influence slider interactive.
 *
 * \return true if the fast path handled the update; false if the caller should fall back to a full
 * #ID_RECALC_GEOMETRY tag (object not in sculpt mode, multires, shape-key / deform sessions, ...).
 */
bool flush_interactive_update(Main &bmain, Mesh &mesh);

/**
 * Grid-domain influence or visibility change from the RNA setter: request an honest geometry
 * re-evaluation (the CCG is rebuilt from `MDisps + sum(enabled layers)`, which already reflects
 * the changed value) and emit notifiers. MDisps are never written.
 *
 * Returns true when a live grid sculpt session was found; false when the caller should fall back
 * to a full #ID_RECALC_GEOMETRY tag itself (object not in sculpt mode, or no live grid PBVH).
 */
bool sync_multires_for_rna(Main &bmain, Scene *scene, Mesh &mesh, SculptLayer &changed_layer);

/**
 * Drop the cached runtime mesh base (#SculptSession::layers::mesh_base) and mark the session
 * state as needing a fresh re-capture. Multires keeps no runtime layer state. Safe to call when
 * no session exists (no-op).
 */
void invalidate_runtime(Object &object);

/**
 * Multires: flush pending base sculpt edits from the live CCG into the base MDisps for the first
 * sculpt-mode object using \a mesh with a grids PBVH. Must run BEFORE a grid layer's influence or
 * visibility value changes (e.g. from an RNA setter): a later flush would reshape the stale
 * composed CCG against the changed layer set and leak the difference into the base.
 * Returns true when a live grid session was found.
 */
bool flush_pending_multires_base_for_mesh(Main &bmain, Mesh &mesh);

/**
 * Reports an info message and returns false when \a mesh still carries sculpt layers that have
 * not been baked into the base geometry. A topology-destroying edit (Trim, Mask Slice, Remesh,
 * ...) cannot preserve their per-element data, so callers must cancel the operator when this
 * returns false.
 *
 * \a reports may be null to skip the message.
 */
bool destructive_edit_check(const Mesh &mesh, ReportList *reports);

/**
 * Re-derive which layer, if any, carries the REC exemption on \a object, returning true when the
 * answer changed.
 *
 * Exported for the RNA setter of the active sculpt layer. Every operator that moves the active layer
 * re-derives the exemption (usually through #commit_layers_change), but assigning
 * `Mesh.sculpt_layers.active` from `bpy` reaches none of them, and a stale exemption means an armed
 * REC records into a layer whose weight mask is not exempt.
 *
 * Idempotent and one uid lookup, so calling it when nothing moved costs nothing.
 */
bool rec_exemption_refresh(Object &object);

/**
 * Tag every node of \a object so the sculpt layer mask overlay refills.
 *
 * Exported alongside #rec_exemption_refresh and for the same reason: assigning
 * `Mesh.sculpt_layers.active` from `bpy` changes which node the overlay draws without going through
 * any of the operators that tag it.
 *
 * A no-op when the object has no PBVH, so callers need not check.
 */
void tag_layer_mask_overlay_dirty(Object &object);

/**
 * Refuse (reporting) an action that moves vertices outside the brush stroke path while a weight-mask
 * editing session is open on \a object. True when it was refused.
 *
 * \a reports may be null to skip the message, which is what the mirrored check in a transform's
 * after-update pass wants: it only needs to know whether the entry point bailed out.
 *
 * The reasoning is #mask_edit_blocks_brush's, applied to everything that reaches the surface without
 * going through a #PaintStroke: the mesh and cloth filters, the transform tools, the line-project
 * gesture, and the geometry-touching modes of face set editing.
 *
 * Two harms, and a caller only has to meet one of them. Anything that consults the mask reads the
 * *layer's* weights while the user's own mask is parked, so its result is shaped by a mask the user
 * cannot see and did not paint. And every caller here deforms outright rather than through a stroke,
 * with recording left disarmed by #mask_edit_enter, so the change lands in the base mesh while the
 * user believes they are painting a mask — already applied by the time anything could notice. Face
 * set fairing is the second kind only: it selects by boundary and face-set membership and never
 * touches the mask.
 *
 * Refused rather than closing the session, for the reason #mask_edit_blocks_brush is: ending it
 * silently would throw away an in-progress mask edit as a side effect of an unrelated action.
 *
 * Lives in this header rather than the module-internal `sculpt_intern.hh` because the transform
 * system calls it from `editors/transform/`.
 */
bool mask_edit_refuse_deform(const Object &object, ReportList *reports);

}  // namespace layers

}  // namespace ed::sculpt_paint

}  // namespace blender
