/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Shared internals of the Curve Patch object-source module: the document converters both the
 * object-source file and the session bridge call, the point-transform the conversion and the
 * write-back share, and the UV helper the host search uses.
 */

#pragma once

#include "BLI_math_matrix_types.hh"

namespace blender {

struct Depsgraph;
struct MaterialPaintLayer;
struct MaterialPaintLayerCurvePatch;
struct Object;

namespace bke {
class CurvesGeometry;
}  // namespace bke

}  // namespace blender

namespace blender::ed::sculpt_paint {

struct CurvePatchDocument;
struct CurvePatchSession;

/** Publish the session document of \a payload: the container's items, with the control curve of
 * every item that names an external object replaced by the object's spline. */
void curve_patch_publish_document(MaterialPaintLayerCurvePatch &payload,
                                  Object &painted_object,
                                  const Depsgraph *depsgraph,
                                  CurvePatchDocument &r_doc);

/** Write the document's curves that name an external object back into the objects. */
void curve_patch_document_to_objects(CurvePatchSession &session, MaterialPaintLayer &row);

/** Whether \a ob has a UV map the object-space curves can be projected through. */
bool curve_patch_layer_object_has_uv(const Object &ob);

/** Map every point of \a curve, and its bezier handles when it has any, through \a m. */
void curve_patch_transform_points(bke::CurvesGeometry &curve, const float4x4 &m);

}  // namespace blender::ed::sculpt_paint
