/* SPDX-FileCopyrightText: 2008 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editors
 */

#pragma once

#include <cstddef>

#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct Depsgraph;
struct Main;
struct Mesh;
struct Object;
struct RegionView3D;
struct ReportList;
struct Scene;
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
void operatormacros_sculpt();

void keymap_sculpt(wmKeyConfig *keyconf);

/* `sculpt_transform.cc` */

void update_modal_transform(bContext *C, Object &ob);
void cancel_modal_transform(bContext *C, Object &ob);
void init_transform(bContext *C, Object &ob, const float mval_fl[2], const char *undo_name);
/** Like #init_transform, but adds \a ob to an already-open multi-object undo step (see
 * #undo::push_begin_add_object) instead of opening a new one. */
void init_transform_add_object(bContext *C, Object &ob, const float mval_fl[2]);
void end_transform(bContext *C, Object &ob);
/** Multi-object counterpart of #end_transform: closes the shared undo step opened across \a
 * objects by #init_transform + #init_transform_add_object. */
void end_transform(bContext *C, Span<Object *> objects);

/**
 * Objects the Transform tool's interactive gizmo (Move/Rotate/Scale pivot) should affect: the
 * active object alone, or every object currently in Sculpt Mode, depending on
 * `Sculpt::transform_all_objects`. Independent of `Sculpt::multi_object_edit_scope` (which only
 * governs brush strokes and other tools).
 */
Vector<Object *> transform_target_objects(bContext *C);

/**
 * Converts \a local_rot (an object's local #SculptSession::pivot_rot) into a world-space
 * quaternion \a r_world_rot, using ONLY \a ob's orientation -- never its own scale. See
 * #sync_local_pivot_from_world's doc comment for why rotation needs this, unlike position.
 */
void local_pivot_rot_to_world(const Object &ob, const float local_rot[4], float r_world_rot[4]);
/**
 * Refreshes \a ob's LOCAL #SculptSession::pivot_pos/#pivot_rot from its (already-set, shared
 * across every object in the session) #SculptSession::transform_pivot_pos_world/
 * #transform_pivot_rot_world. Called once per object every modal step of a Transform session
 * (see #update_modal_transform) so the per-object vertex-displacement math always sees an
 * up-to-date local pivot.
 *
 * Position uses \a ob's full (possibly anisotropic) world-to-object matrix -- exact for a point.
 * Rotation uses \a ob's orientation ONLY, with scale stripped out: conjugating a rotation by a
 * non-uniformly-scaled matrix does not generally produce a valid rotation (it shears), so
 * position and rotation need two different conversions here.
 */
void sync_local_pivot_from_world(Object &ob);

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

int find_next_available_id(const Mesh &object);
int find_next_available_id(Object &object);

/**
 * The next available Face Set id that is free across every mesh in \a meshes — i.e.
 * `max(find_next_available_id(mesh) for mesh in meshes)`. Used to assign one shared id across
 * all objects in a multi-object sculpt-mode gesture/operator.
 */
int find_shared_next_available_id(Span<const Mesh *> meshes);
int find_shared_next_available_id(Span<Object *> objects);

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

}  // namespace ed::sculpt_paint

}  // namespace blender
