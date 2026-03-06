/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * \brief Curves Weight Paint API for render engines
 */

#pragma once

#include "GPU_batch.hh"

#include "DNA_object_types.h"

namespace blender::draw {

/**
 * Gets the curves weight batch cache for points rendering.
 */
gpu::Batch *DRW_cache_curves_weight_points_get(Object *object);

/**
 * Gets the curves weight batch cache for lines rendering.
 */
gpu::Batch *DRW_cache_curves_weight_lines_get(Object *object);

}  // namespace blender::draw