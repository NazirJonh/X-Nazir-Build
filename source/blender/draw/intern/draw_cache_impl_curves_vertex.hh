/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Curves Vertex Paint API for render engines
 */

#pragma once

#include "GPU_batch.hh"

struct Object;

namespace blender::draw {

gpu::Batch *DRW_cache_curves_vertex_paint_points_get(Object *object);
gpu::Batch *DRW_cache_curves_vertex_paint_lines_get(Object *object);

}  // namespace blender::draw
