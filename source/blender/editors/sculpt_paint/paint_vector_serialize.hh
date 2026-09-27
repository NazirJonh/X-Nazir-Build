/* SPDX-FileCopyrightText: 2026 Nazir Galimov
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Converters between the runtime #VectorDocument and the persisted `PaintVector` ID.
 *
 * The runtime document holds no style; the persistence ID embeds a `PaintShapeSettings` snapshot
 * per item. To keep a saved vector reproducible, the write direction takes the style to
 * snapshot as an argument and the read direction leaves the runtime document geometry-only.
 */

#pragma once

#include "DNA_scene_types.h"

#include "paint_vector_document.hh"

namespace blender {
struct PaintVector;
}

namespace blender::ed::sculpt_paint::shape {

/** Write \a doc into \a r_pv, snapshotting \a style into every item. Clears \a r_pv first. */
void paint_vector_from_document(PaintVector &r_pv,
                                const VectorDocument &doc,
                                const PaintShapeSettings &style);

/** Read the geometry of \a pv into \a r_doc (its undo history is reset). */
void paint_vector_to_document(const PaintVector &pv, VectorDocument &r_doc);

}  // namespace blender::ed::sculpt_paint::shape
