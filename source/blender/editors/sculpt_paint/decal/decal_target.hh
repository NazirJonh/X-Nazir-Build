/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * The editor-facing decal target: a decal is the group of projected Fill rows one image maps
 * through, named `"<decal> · <channel>"` so the group survives save/load and reordering without a
 * side table. The rows share one frame (the Planar space the Decal Master camera projects through),
 * which the operators here place from the 3D viewport the way a camera at the view would.
 */

#pragma once

#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "DNA_uuid_types.h"

namespace blender {
struct bContext;
struct Main;
struct Material;
struct MaterialPaintLayer;
struct Object;
struct ReportList;
struct wmOperatorType;
}  // namespace blender

namespace blender::ed::sculpt_paint::projection {
struct ProjectionFrame;
}  // namespace blender::ed::sculpt_paint::projection

namespace blender::ed::sculpt_paint::decal {

/** The joiner between a decal's name and a channel in a row name: `"<decal> · <channel>"`. */
constexpr const char *DECAL_NAME_SEPARATOR = " · ";

/**
 * The decal name in the row name \a row_name: the text in front of the last separator (channel
 * names never contain it, a decal name may). False when there is no separator or nothing in front
 * of it. The one parser of the naming scheme, paired with #decal_row_name_make.
 */
bool decal_name_of_row_name(const char *row_name, char r_decal[64]);

/**
 * The decal name \a row belongs to (see #decal_name_of_row_name). False for a row that is not a
 * decal row.
 */
bool decal_name_of_row(const MaterialPaintLayer &row, char r_decal[64]);

/**
 * Build the row name of \a decal's \a channel_name. The one place the name is formed: an empty
 * \a decal reads "Decal", and a \a decal too long for the 64 byte name is cut at a UTF-8 character
 * boundary so the separator and the channel always fit. Read the decal name back with
 * #decal_name_of_row_name, since it may have been cut.
 */
void decal_row_name_make(const char *decal, const char *channel_name, char r_name[64]);

/**
 * Whether \a ob is in a mode the decal operators work in: Sculpt Mode, where the paint toolset and
 * the Stack Layers outliner live, or Texture Paint. The one check every decal poll shares. The
 * decal operators only use the mode-free helpers of projection_target (the raycast, the world
 * conversions), never #PaintLayerRowProjectionTarget::resolve, which admits Sculpt Mode alone (the
 * Place/Move/Flip gestures and the gizmo), so they work in both modes.
 */
bool decal_mode_supported(const Object &ob);

/** The rows of the decal \a decal_name, in stack order (bottom of the stack first). */
void decal_rows_find(Material &ma, const char *decal_name, Vector<MaterialPaintLayer *> &r_rows);

/**
 * The controller (\a projection_object) all \a rows share, or null when none of them has one.
 * False when the rows disagree -- some have a controller and some do not, or they name different
 * objects -- a state no group operator can act on.
 */
bool decal_group_controller_get(Span<MaterialPaintLayer *> rows, Object **r_controller);

/**
 * The decal the Layer tab shows: the one of the active row, or the only decal in the material.
 * False without a texture-paint context, an editable layered material, or when more than one decal
 * exists and none is active.
 */
bool decal_target_find(const bContext &C,
                       Material **r_material,
                       Object **r_object,
                       char r_name[64],
                       Vector<MaterialPaintLayer *> *r_rows);

/**
 * The frame a camera at the current view would project through, in \a ob's object space: centre at
 * the surface hit under \a mval (the bounding-box centre when the ray misses or \a mval is
 * negative), the view's orientation as normal+roll, extents covering the object in frame space.
 */
bool decal_frame_from_view(const bContext &C,
                           Object &ob,
                           const float mval[2],
                           projection::ProjectionFrame &r_frame);

/**
 * The frame that covers \a ob without a view: the bounding box centre, the default orientation,
 * extents covering the box.
 */
void decal_frame_from_bounds(Object &ob, projection::ProjectionFrame &r_frame);

/**
 * Write \a frame to every row of a decal (value-only), with the decal's shared settings: the frame
 * reads along its local Z and follows the given clip.
 */
void decal_frame_apply(Material &ma,
                       const Span<MaterialPaintLayer *> rows,
                       const projection::ProjectionFrame &frame,
                       bool clip);

/**
 * Convert \a frame from the space of \a from_to_world to the space of \a world_to_to, through the
 * world: the centre is carried as a point, the orientation is composed with the relative rotation
 * (#BKE_paint_layers_projection_frame_matrix / _frame_from_matrix keep it orthonormal), each extent
 * is scaled by the largest relative axis scale. One warning when the relative transform is
 * non-uniform -- the extents are then approximated along the frame's own axes; a degenerate
 * relative transform converts nothing and returns false.
 *
 * \param from_name, to_name: the labels of the two spaces, for the warning's report.
 */
bool decal_frame_convert(const projection::ProjectionFrame &frame,
                         const float4x4 &from_to_world,
                         const float4x4 &world_to_to,
                         projection::ProjectionFrame &r_frame_out,
                         ReportList *reports,
                         const char *from_name,
                         const char *to_name);

/**
 * Anchor \a rows (one decal group of \a ma) to the world: create a controller Empty in the active
 * collection, place it at the group's frame as it sits in \a ob's space, and assign it to every
 * row. Each row's own frame is converted into that space: a group with one shared frame reads
 * centre/normal+roll neutral and the same size, so the projection holds its world place
 * (M_empty = M_ob · T(c) · R); a row moved on its own keeps its offset. The axis and clip settings
 * of the rows are left alone, and the active object and mode are not touched.
 *
 * The whole group is validated before the Empty is created; a failure after that rolls the rows
 * back and removes the Empty. False when a row does not project, the rows disagree about an
 * existing controller, the scene refuses the object or a row refuses the assignment.
 * \a r_controller (may be null) receives the new (or existing) Empty.
 */
bool decal_controller_ensure(bContext &C,
                             Material &ma,
                             Object &ob,
                             const Span<MaterialPaintLayer *> rows,
                             ReportList *reports,
                             Object **r_controller);

/**
 * Release \a rows from their shared controller: each frame is converted from the controller's
 * space into \a ob's own and written back (values before the reset, they read the controller's
 * space while it is still set), then the controller is dropped. With \a delete_empty the Empty is
 * removed when nothing else uses it (no user beyond its collection membership, no parented
 * object); otherwise it is left in place and said so.
 */
bool decal_controller_detach(const bContext &C,
                             Material &ma,
                             Object &ob,
                             const Span<MaterialPaintLayer *> rows,
                             ReportList *reports,
                             bool delete_empty);

/**
 * The objects a propagation can reach: the selected editable objects other than \a source, in the
 * context's order. No material filtering -- the operator reports each skip reason with the
 * object's name.
 */
void decal_propagation_candidates(const bContext &C,
                                  const Object &source,
                                  Vector<Object *> &r_candidates);

/** How #decal_rows_copy places the copied frame. */
enum class DecalCopyMode {
  /** The copies keep the source's controller object, so one frame edit moves them all. */
  LinkController,
  /** The copies project in each target object's own space; the frame is converted. */
  ConvertFrame,
};

/**
 * Copy the decal group \a rows (of \a src_ma) into \a dst_ma, in source order, each copy above the
 * one before it. Per row: the row's channels have to be in the destination's channel set and, in
 * ConvertFrame, the row has to project; every refusal is a warning naming the row, the copy goes on. The
 * copies share the source's channel images. No notifier and no undo step -- the operator owns
 * those. False when nothing was copied. \a r_copies (may be null) receives the rows created.
 */
bool decal_rows_copy(Main &bmain,
                     const bContext &C,
                     Material &src_ma,
                     Object &src_ob,
                     Material &dst_ma,
                     Object &dst_ob,
                     const Span<MaterialPaintLayer *> rows,
                     DecalCopyMode mode,
                     ReportList *reports,
                     Vector<MaterialPaintLayer *> *r_copies);

void PAINT_OT_decal_add(wmOperatorType *ot);
/** D7: the same create path for an already-loaded image, the operator a drop target calls. */
void PAINT_OT_decal_add_image(wmOperatorType *ot);
void PAINT_OT_decal_project_from_view(wmOperatorType *ot);
void PAINT_OT_decal_remove(wmOperatorType *ot);
void PAINT_OT_decal_visibility_set(wmOperatorType *ot);
void PAINT_OT_decal_move(wmOperatorType *ot);
void PAINT_OT_decal_blend_mode_set(wmOperatorType *ot);
void PAINT_OT_decal_projection_mode_set(wmOperatorType *ot);
/** D3: the group's coverage mask preset (each row gets its own mask). */
void PAINT_OT_decal_mask_add(wmOperatorType *ot);
void PAINT_OT_decal_mask_remove(wmOperatorType *ot);
/** D6: place the frame from the live selection-cage transform (Phase B hand-over). */
void PAINT_OT_decal_from_selection(wmOperatorType *ot);
/** Attach the decal group's frame to a new controller Empty (or release it back to the object). */
void PAINT_OT_decal_world_anchor_set(wmOperatorType *ot);
/** Copy the decal group into the layered material of every other selected object. */
void PAINT_OT_decal_propagate_to_selected(wmOperatorType *ot);

/** The operator-type registration of the decal module, so #paint_ops.cc registers the module with
 * one call instead of naming its operators. */
void operatortypes_register();

}  // namespace blender::ed::sculpt_paint::decal
